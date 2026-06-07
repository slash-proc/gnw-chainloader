#!/usr/bin/env python3
"""Find free 4-byte-aligned words (0x00000000 or 0xFFFFFFFF padding) in a binary
within an address range -- used to park a new literal constant for an in-place
Thumb patch (the literal must sit within ldr.w's +/-4095 PC range of the patch
site). Reports each candidate with its load address (base + offset).

Usage: python3 scripts/debug/find_free_word.py FILE [--base 0x08000000]
                                                     [--lo 0x38C5] [--hi 0x585B]
"""
import argparse
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x08000000)
    ap.add_argument("--lo", type=lambda x: int(x, 0), default=0x38C5)
    ap.add_argument("--hi", type=lambda x: int(x, 0), default=0x585B)
    args = ap.parse_args()

    data = Path(args.file).read_bytes()
    lo = (args.lo + 3) & ~3   # round up to word alignment
    hi = min(args.hi, len(data))

    print(f"Scanning {args.file} for free aligned words in [0x{lo:X}, 0x{hi:X}):")
    found = 0
    off = lo
    while off + 4 <= hi:
        w = int.from_bytes(data[off:off + 4], "little")
        if w in (0x00000000, 0xFFFFFFFF):
            # require both neighbours also free-ish to avoid splitting real data
            print(f"  off 0x{off:06X}  load-addr 0x{args.base + off:08X}  = 0x{w:08X}")
            found += 1
        off += 4
    if not found:
        print("  (no all-zero / all-ones aligned words in range)")


if __name__ == "__main__":
    main()
