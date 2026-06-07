#!/usr/bin/env python3
"""Compare two Link's Awakening external-flash region images and localize the
differences.

Both inputs are REGION-RELATIVE: offset 0 == external-flash 0xD2000 (the start
of the LA ROM block). Typically `device` is a dump pulled off the running OFW
(`memory.py dump 0x900D2000 0x122C00 ...`) and `build` is the clean slice from
the patched image (`scan_la_rom.py ... --extract`).

Reports each contiguous run of differing bytes, which of the four embedded
language ROMs it falls in, and whether the device side looks ERASED (0xFF),
ZEROED, or just stale/overwritten real data -- which tells corruption (a write
landed on the ROM) apart from a missing/partial flash write.
"""
import argparse
from pathlib import Path

REGION_BASE = 0xD2000
# ROM starts relative to the region base, from the logo scan of the clean build
# (absolute 0xD2000 / 0x148400 / 0x18D000 / 0x1D2800 minus 0xD2000).
ROM_STARTS = [0x00000, 0x76400, 0xBB000, 0x100800]


def rom_index(off: int) -> int:
    idx = -1
    for i, s in enumerate(ROM_STARTS):
        if off >= s:
            idx = i
    return idx


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", help="region-relative device dump")
    ap.add_argument("build", help="region-relative known-good slice")
    ap.add_argument("--sectors", action="store_true",
                    help="also print a 4K-sector diff histogram")
    args = ap.parse_args()

    d = Path(args.device).read_bytes()
    b = Path(args.build).read_bytes()
    n = min(len(d), len(b))
    if len(d) != len(b):
        print(f"(note: lengths differ -- device 0x{len(d):X}, build 0x{len(b):X}; "
              f"comparing first 0x{n:X})")

    runs = []
    i = 0
    while i < n:
        if d[i] != b[i]:
            j = i
            while j < n and d[j] != b[j]:
                j += 1
            runs.append((i, j - i))
            i = j
        else:
            i += 1

    total = sum(r[1] for r in runs)
    print(f"device = {args.device}")
    print(f"build  = {args.build}")
    print(f"length = 0x{n:X} ({n:,} bytes)")
    print(f"differing bytes: {total:,} in {len(runs)} contiguous run(s)\n")

    for start, length in runs:
        dev = d[start:start + length]
        ff = sum(1 for x in dev if x == 0xFF)
        zero = sum(1 for x in dev if x == 0x00)
        ri = rom_index(start)
        rel = start - ROM_STARTS[ri] if ri >= 0 else start
        kind = "ERASED(0xFF)" if ff == length else ("ZEROED" if zero == length else "real-data")
        print(f"  region+0x{start:06X} (ext 0x{REGION_BASE + start:06X}) "
              f"len 0x{length:X}  ROM#{ri} +0x{rel:X}  "
              f"dev={kind} ff={ff * 100 // length}% 00={zero * 100 // length}%")

    # Per-ROM totals.
    print("\nper-ROM differing bytes:")
    for i, s in enumerate(ROM_STARTS):
        e = ROM_STARTS[i + 1] if i + 1 < len(ROM_STARTS) else n
        cnt = sum(1 for off in range(s, min(e, n)) if d[off] != b[off])
        print(f"  ROM#{i}  region 0x{s:06X}-0x{e:06X}  diffs={cnt:,}")

    if args.sectors:
        print("\n4K sectors containing diffs:")
        sec = 0x1000
        for base in range(0, n, sec):
            cnt = sum(1 for off in range(base, min(base + sec, n)) if d[off] != b[off])
            if cnt:
                print(f"  region+0x{base:06X} (ext 0x{REGION_BASE + base:06X}): {cnt} diffs")


if __name__ == "__main__":
    main()
