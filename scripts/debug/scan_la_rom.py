#!/usr/bin/env python3
"""Scan the Zelda OFW external-flash image for the embedded Link's Awakening
Game Boy ROMs and report each one's integrity.

The Zelda OFW bundles the Game Boy game "The Legend of Zelda: Link's Awakening"
as one or more Game Boy ROM images concatenated in external flash at
0xD2000-0x1F4C00 (see gnwmanager/.../gnw_patch/zelda.py docstring). When a
language white-screens, the question is whether THAT language's ROM bytes are
intact. This tool locates every Game Boy ROM by its Nintendo-logo header magic,
then validates the header checksum and looks for erased/0xFF gaps, so we can map
language -> ROM offset from ground truth and spot partial corruption.

Usage:
    python3 scripts/debug/scan_la_rom.py [image.bin] [--start HEX] [--end HEX]

Defaults to build/patched_external_zelda.bin over the LA region 0xD2000-0x1F4C00.
Pass a device dump (e.g. from `gnwmanager flash read`) to compare against the
build, or point --start/--end elsewhere to scan a different region.
"""
import argparse
import sys
from pathlib import Path

# The 48-byte Nintendo logo every real Game Boy ROM carries at header offset
# 0x104. Finding it locates a ROM start at (match_offset - 0x104).
GB_LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
])
LOGO_HDR_OFF = 0x104

# Cartridge-size code at header 0x148 -> ROM bytes.
ROM_SIZE_TABLE = {i: 32 * 1024 << i for i in range(9)}  # 0->32K, 1->64K, ... 8->8M


def header_checksum(rom: bytes, base: int) -> int:
    """Game Boy header checksum: x = 0; for 0x134..0x14C: x = x - b - 1."""
    x = 0
    for i in range(base + 0x134, base + 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    return x


def ascii_title(rom: bytes, base: int) -> str:
    raw = rom[base + 0x134 : base + 0x144]
    return "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in raw).rstrip(". ")


def ff_runs(data: bytes, start: int, end: int, threshold: int = 0x1000):
    """Yield (offset, length) for runs of >=threshold 0xFF bytes (erased flash)."""
    i = start
    n = end
    while i < n:
        if data[i] == 0xFF:
            j = i
            while j < n and data[j] == 0xFF:
                j += 1
            if j - i >= threshold:
                yield (i, j - i)
            i = j
        else:
            i += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", default="build/patched_external_zelda.bin")
    ap.add_argument("--start", default="0xD2000")
    ap.add_argument("--end", default="0x1F4C00")
    ap.add_argument("--selectors", action="store_true",
                    help="Also dump the 4 LA language menu-selector words "
                         "(EN 0x315B54, FR 0x315B58, DE 0x315B5C, JP 0x315B60)")
    ap.add_argument("--dump", nargs=2, metavar=("HEX_OFF", "LEN"),
                    help="Hexdump LEN bytes at HEX_OFF and exit")
    ap.add_argument("--extract", metavar="OUTFILE",
                    help="Write the [--start,--end) slice to OUTFILE and exit "
                         "(for byte-comparing against a device dump)")
    args = ap.parse_args()

    start = int(args.start, 0)
    end = int(args.end, 0)
    path = Path(args.image)
    if not path.exists():
        print(f"Error: {path} not found", file=sys.stderr)
        sys.exit(1)

    data = path.read_bytes()

    if args.extract:
        Path(args.extract).write_bytes(data[start:end])
        print(f"Wrote 0x{start:X}-0x{end:X} ({end - start} bytes) -> {args.extract}")
        return

    if args.dump:
        off = int(args.dump[0], 0)
        length = int(args.dump[1], 0)
        for row in range(off, off + length, 16):
            chunk = data[row : row + 16]
            hexs = " ".join(f"{b:02X}" for b in chunk)
            asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
            print(f"  0x{row:06X}  {hexs:<47s}  {asc}")
        return

    if args.selectors:
        labels = {0x315B54: "EN", 0x315B58: "FR", 0x315B5C: "DE", 0x315B60: "JP"}
        print("LA language menu-selector words (little-endian):")
        for sel_off, lang in labels.items():
            word = int.from_bytes(data[sel_off : sel_off + 4], "little")
            print(f"  {lang}: [0x{sel_off:06X}] = 0x{word:08X}")
        print()

    if end > len(data):
        print(f"Warning: --end 0x{end:X} past EOF (len 0x{len(data):X}); clamping", file=sys.stderr)
        end = len(data)

    region = data[start:end]
    print(f"Image:  {path}  ({len(data):,} bytes)")
    print(f"Region: 0x{start:X}-0x{end:X}  ({len(region):,} bytes)")
    print()

    # Locate every Game Boy ROM by its logo header.
    roms = []
    search_from = 0
    while True:
        m = region.find(GB_LOGO, search_from)
        if m < 0:
            break
        rom_base = m - LOGO_HDR_OFF
        if rom_base >= 0:
            roms.append(start + rom_base)
        search_from = m + 1

    if not roms:
        print("!! No Game Boy logo headers found in region -- LA ROM area is")
        print("   missing or corrupt (no valid GB cartridge headers).")
    else:
        print(f"Found {len(roms)} Game Boy ROM header(s):\n")
        for idx, base in enumerate(roms):
            b = base  # absolute offset into `data`
            title = ascii_title(data, b)
            cart_type = data[b + 0x147]
            size_code = data[b + 0x148]
            rom_size = ROM_SIZE_TABLE.get(size_code)
            stored = data[b + 0x14D]
            calc = header_checksum(data, b)
            ok = "OK" if stored == calc else f"BAD (stored 0x{stored:02X} != calc 0x{calc:02X})"
            nextrom = roms[idx + 1] if idx + 1 < len(roms) else end
            gap = nextrom - b
            print(f"  ROM #{idx}: off 0x{b:06X}  title={title!r:20s} "
                  f"cart_type=0x{cart_type:02X} size_code=0x{size_code:02X} "
                  f"({rom_size//1024 if rom_size else '?'}K)  hdr_csum={ok}")
            print(f"           span to next: 0x{gap:X} bytes "
                  f"({'declared ' + str(rom_size//1024) + 'K' if rom_size else 'unknown size'})")

    # Erased/corrupt gaps across the whole region.
    print("\nErased (0xFF) runs >= 4K in region:")
    any_ff = False
    for off, length in ff_runs(data, start, end):
        any_ff = True
        print(f"  0x{off:06X}  len 0x{length:X} ({length//1024}K)")
    if not any_ff:
        print("  (none -- no large erased gaps)")


if __name__ == "__main__":
    main()
