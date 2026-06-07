#!/usr/bin/env python3
"""Search a Mario 8bpp tileset for tiles matching a heuristic.

Consolidates the brick/letter finders:

    find_brick                       -> `--strategy color`   (count of a target colour index)
    find_brick_by_layout,
    find_brick_pattern               -> `--strategy solid`   (little/no background + brown/black)
    find_letters, find_letters_decompressed -> `--strategy letters`  (sparse tiles -> ASCII dump)

Examples
--------
    python3 scripts/debug/findtiles.py --strategy color
    python3 scripts/debug/findtiles.py --strategy solid
    python3 scripts/debug/findtiles.py --strategy letters \\
        --input backup/decrypt_flash_patched.bin --offset 0x98B84       
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio, resolve


def color(args):
    data = flashio.load_raw(args.input)
    region = data[args.offset:] if args.offset else data
    n = len(region) // 256
    print(f"Tiles containing color {args.color} ({args.label}):")
    for t in range(n):
        tile = region[t * 256:t * 256 + 256]
        cnt = sum(1 for b in tile if b == args.color)
        if cnt > args.threshold:
            print(f"  Tile {t:3d}: {cnt} pixels of color {args.color}")


def solid(args):
    data = flashio.load_raw(args.input)
    region = data[args.offset:] if args.offset else data
    n = len(region) // 256
    print("Brick candidates:")
    for t in range(n):
        tile = region[t * 256:t * 256 + 256]
        c0 = sum(1 for b in tile if b == 0)
        if c0 <= args.max_bg and (6 in set(tile) or 3 in set(tile)):
            c6 = sum(1 for b in tile if b == 6)
            c3 = sum(1 for b in tile if b == 3)
            print(f"  Tile {t:3d}: non-zero={256 - c0}, color 6={c6}, color 3={c3}")


def letters(args):
    data = flashio.load_raw(args.input)
    tiles_data = data[args.offset:args.offset + 0x10000]
    n = len(tiles_data) // 256
    print("Non-zero pixel count distribution:")
    counts: dict[int, int] = {}
    out = []
    for t in range(n):
        tile = tiles_data[t * 256:t * 256 + 256]
        nz = sum(1 for b in tile if b != 0)
        counts[nz] = counts.get(nz, 0) + 1
        if args.min <= nz <= args.max:
            out.append(f"=== TILE {t} (Non-zero: {nz}/256) ===")
            for ty in range(16):
                row = ""
                for tx in range(16):
                    b = tile[ty * 16 + tx]
                    row += "  " if b == 0 else (f"{b:01X}" if b < 16 else "##")
                out.append(row)
            out.append("")
    for k in sorted(counts):
        print(f"  {k} non-zero pixels: {counts[k]} tiles")
    out_path = resolve(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(out))
    print(f"Saved candidate letters to {out_path}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--strategy", choices=["color", "solid", "letters"], required=True)
    p.add_argument("--input", default="build/decompressed_clock_graphics.bin")
    p.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    # color
    p.add_argument("--color", type=int, default=6)
    p.add_argument("--threshold", type=int, default=20)
    p.add_argument("--label", default="brown")
    # solid
    p.add_argument("--max-bg", type=int, default=5, dest="max_bg")
    # letters
    p.add_argument("--min", type=int, default=5)
    p.add_argument("--max", type=int, default=150)
    p.add_argument("-o", "--output", default="build/candidate_letters.txt")
    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    {"color": color, "solid": solid, "letters": letters}[args.strategy](args)
