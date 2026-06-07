#!/usr/bin/env python3
"""Report whether each file in a FAT16 image is CONTIGUOUS (one unbroken cluster run).

Contiguity is the property f_expand guarantees and that XIP requires: a module can only
execute in place from external flash if its bytes are a single uninterrupted run. f_expand
itself is a runtime install-time call; this tool verifies the *result* (or checks a factory
image built by pyfatfs, which does not call f_expand and so only lands files contiguously by
luck on a fresh volume).

Walks the directory tree, and for every file follows its FAT cluster chain from the directory
entry's start cluster, flagging the first gap. Works on the host image (build/fat.bin) or a raw
dump of the partition pulled over SWD.

  python3 scripts/debug/fat_contig.py [image]     # default build/fat.bin
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]


def u16(b, o): return b[o] | (b[o + 1] << 8)
def u32(b, o): return u16(b, o) | (u16(b, o + 2) << 16)


def main():
    img = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build" / "fat.bin"
    d = img.read_bytes()
    if u16(d, 510) != 0xAA55:
        print(f"{img}: not a FAT boot sector (no 0x55AA signature)", file=sys.stderr)
        return 1

    bps   = u16(d, 0x0B)                       # bytes per sector
    spc   = d[0x0D]                            # sectors per cluster
    rsvd  = u16(d, 0x0E)                       # reserved sectors
    nfat  = d[0x10]                            # number of FATs
    rootn = u16(d, 0x11)                       # root dir entries
    fatsz = u16(d, 0x16)                       # sectors per FAT (FAT16)
    fat_start = rsvd * bps
    root_start = (rsvd + nfat * fatsz) * bps
    root_sectors = (rootn * 32 + bps - 1) // bps
    data_start = (rsvd + nfat * fatsz + root_sectors) * bps

    # FAT16: 2 bytes per entry.
    fat = d[fat_start:fat_start + fatsz * bps]
    nextcl = lambda cl: u16(fat, cl * 2)
    clus_off = lambda cl: data_start + (cl - 2) * spc * bps

    def chain(start):
        out, cl = [], start
        while 2 <= cl < 0xFFF8 and len(out) < 200000:
            out.append(cl)
            cl = nextcl(cl)
        return out

    files = []  # (path, size, clusters)

    def walk(entries, path):
        for i in range(0, len(entries), 32):
            e = entries[i:i + 32]
            if not e or e[0] == 0x00:
                break
            if e[0] == 0xE5 or (e[0x0B] & 0x0F) == 0x0F or (e[0x0B] & 0x08):
                continue   # deleted / LFN / volume label
            name = e[0:8].decode("latin1").rstrip()
            ext = e[8:11].decode("latin1").rstrip()
            nm = name + ("." + ext if ext else "")
            if nm in (".", ".."):
                continue
            start = u16(e, 0x1A)
            size = u32(e, 0x1C)
            child = path + "/" + nm
            if e[0x0B] & 0x10:   # directory
                sub = b"".join(d[clus_off(c):clus_off(c) + spc * bps] for c in chain(start))
                walk(sub, child)
            else:
                files.append((child, size, chain(start)))

    walk(d[root_start:root_start + root_sectors * bps], "")

    print(f"{img.relative_to(REPO) if img.is_relative_to(REPO) else img}  "
          f"(FAT16, {bps} B/sec, {spc} sec/cluster = {spc*bps} B/cluster)")
    bad = 0
    for path, size, cl in files:
        if not cl:
            print(f"  {path:24} {size:>8} B  (empty)")
            continue
        gaps = [(cl[i], cl[i + 1]) for i in range(len(cl) - 1) if cl[i + 1] != cl[i] + 1]
        contig = not gaps
        bad += 0 if contig else 1
        first = clus_off(cl[0])
        tag = "CONTIGUOUS" if contig else f"FRAGMENTED ({len(gaps)} gap(s))"
        print(f"  {path:24} {size:>8} B  clusters {cl[0]}..{cl[-1]} ({len(cl)})  "
              f"@ 0x{first:06X}  {tag}")
        if gaps:
            for a, b in gaps[:4]:
                print(f"        gap: cluster {a} -> {b} (expected {a+1})")
    print(f"-> {len(files)} file(s), {bad} fragmented" +
          ("" if bad else "  [all XIP-able]"))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
