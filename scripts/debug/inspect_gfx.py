#!/usr/bin/env python3
"""Inspect tile/palette/colour data in flash images (stdout / ASCII diagnostics).

Consolidates the ``inspect_*`` family plus the colour-analysis one-offs:

    inspect_walls, inspect_dots, inspect_boxes, inspect_all_walls, inspect_some_tiles,
    inspect_tiles_80_95, print_tiles, dump_decompressed_ascii, inspect_zelda_palette,
    inspect_patched_external, inspect_zelda_sprites, inspect_font_pixels,
    analyse_color_frequency, check_unique_colors, find_tiles_by_colors, map_custom_palette

Examples
--------
    # Quadrant-mapped ASCII art of Zelda G&W tiles 10-15 (== inspect_dots.py)
    python3 scripts/debug/inspect.py tiles --console zelda --tiles 10:15

    # Linear ASCII art of decompressed Mario tiles 90-150 to a file (== print_tiles.py)
    python3 scripts/debug/inspect.py tiles --input build/decompressed_clock_graphics.bin \\
        --offset 0 --layout linear --tiles 90:150 -o build/selected_tiles_90_150.txt

    # Palette dump (== inspect_zelda_palette.py)
    python3 scripts/debug/inspect.py palette --console zelda --count 16

    # Colour-index frequency over the Mario tileset region (== analyse_color_frequency.py)
    python3 scripts/debug/inspect.py colors --input backup/decrypt_flash_patched.bin \\
        --offset 0x98B84 --length 0x10000 --mode freq
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio, imaging, palette as palmod


def _parse_tiles(spec: str) -> list[int]:
    """Parse ``A:B`` (inclusive range) or a comma list ``16,31,32`` into tile indices."""
    spec = spec.strip()
    if ":" in spec:
        a, b = spec.split(":", 1)
        return list(range(int(a, 0), int(b, 0) + 1))
    return [int(x, 0) for x in spec.split(",") if x.strip()]


def _input_for(args, default_key="patched_external") -> bytes:
    if args.input:
        return flashio.load_raw(args.input)
    return flashio.load_raw(flashio.info(args.console)[default_key])


def cmd_tiles(args):
    data = _input_for(args)
    offset = args.offset if args.offset is not None else flashio.info(args.console)["gnw_tileset_offset"]
    tiles = _parse_tiles(args.tiles)
    # tiles_to_ascii works on a contiguous start..end range; honour an explicit list too.
    chunks = []
    for t in tiles:
        chunks.append(imaging.tiles_to_ascii(data, offset, t, t, tile_px=args.tile_px,
                                              layout=args.layout, overflow=args.overflow))
    text = "\n".join(chunks)
    if args.output:
        Path(flashio.resolve(args.output)).write_text(text)
        print(f"Saved tile ASCII to {flashio.resolve(args.output)}")
    else:
        print(text)


def cmd_palette(args):
    data = _input_for(args)
    offset = args.offset if args.offset is not None else flashio.info(args.console)["gnw_palette_offset"]
    print(f"Loaded {len(data)} bytes")
    print(f"Bytes at offset 0x{offset:X}:")
    print(data[offset:offset + 128].hex())
    for i in range(args.count):
        c = data[offset + i * 4: offset + (i + 1) * 4]
        if len(c) < 4:
            break
        print(f"  Color {i:2d}: R={c[2]:3d}, G={c[1]:3d}, B={c[0]:3d}, A={c[3]:3d} (hex: {c.hex()})")


def cmd_hexdump(args):
    data = _input_for(args)
    offset = args.offset
    print(f"Loaded {len(data)} bytes")
    print(f"Bytes at offset 0x{offset:X} (len {args.length}):")
    print(data[offset:offset + args.length].hex())
    if args.blocks:
        for block in range(args.blocks):
            b_off = offset + block * args.block_size
            nz = sum(1 for b in data[b_off:b_off + args.block_size] if b != 0)
            print(f"  Block {block:2d} (0x{b_off:X}): non-zero = {nz}")


def cmd_colors(args):
    data = _input_for(args, default_key="patched_external")
    offset = args.offset or 0
    if args.mode == "freq":
        length = args.length if args.length is not None else (len(data) - offset)
        region = data[offset:offset + length]
        counter = palmod.count_colors(region)
        print("Color index frequencies across all tiles:")
        for val, count in counter.most_common():
            pct = count / len(region) * 100 if region else 0.0
            print(f"  Color Index {val:02d} (0x{val:02X}): {count} pixels ({pct:.2f}%)")
    elif args.mode == "unique":
        tiles = _parse_tiles(args.tiles) if args.tiles else []
        uniq = set()
        for t in tiles:
            base = offset + t * 256
            uniq.update(data[base:base + 256])
        print(f"Total target tiles: {len(tiles)}")
        print(f"Number of unique colors used: {len(uniq)}")
        print(f"Colors: {sorted(uniq)}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
        sp.add_argument("--input", help="explicit input .bin (overrides --console default)")

    sp = sub.add_parser("tiles", help="ASCII-art dump of a tile range/list")
    common(sp)
    sp.add_argument("--offset", type=lambda s: int(s, 0), default=None,
                    help="byte offset of tile 0 (default: console G&W tileset)")
    sp.add_argument("--tiles", required=True, help="A:B inclusive range or comma list")
    sp.add_argument("--layout", choices=["linear", "quadrant"], default="quadrant")
    sp.add_argument("--tile-px", type=int, default=16, dest="tile_px")
    sp.add_argument("--overflow", choices=["hex", "mark"], default="hex",
                    help="'hex' = full byte hex (inspect_*); 'mark' = '##' for >=16 (print_tiles)")
    sp.add_argument("-o", "--output", help="write to file instead of stdout")
    sp.set_defaults(func=cmd_tiles)

    sp = sub.add_parser("palette", help="dump BGRA palette colours as hex + RGB")
    common(sp)
    sp.add_argument("--offset", type=lambda s: int(s, 0), default=None)
    sp.add_argument("--count", type=int, default=16)
    sp.set_defaults(func=cmd_palette)

    sp = sub.add_parser("hexdump", help="raw hex of a region + optional per-block non-zero stats")
    common(sp)
    sp.add_argument("--offset", type=lambda s: int(s, 0), required=True)
    sp.add_argument("--length", type=lambda s: int(s, 0), default=256)
    sp.add_argument("--blocks", type=int, default=0)
    sp.add_argument("--block-size", type=lambda s: int(s, 0), default=256, dest="block_size")
    sp.set_defaults(func=cmd_hexdump)

    sp = sub.add_parser("colors", help="colour-index frequency / unique-colour analysis")
    common(sp)
    sp.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    sp.add_argument("--length", type=lambda s: int(s, 0), default=None)
    sp.add_argument("--mode", choices=["freq", "unique"], default="freq")
    sp.add_argument("--tiles", help="tile list/range for --mode unique")
    sp.set_defaults(func=cmd_colors)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
