#!/usr/bin/env python3
"""Extract visual assets (PNGs) from Game & Watch flash / NES ROMs.

Consolidates the visual-extraction scripts:

    extract_zelda2_chr, extract_zelda2_chr_rom   -> `chr`
    extract_correct_link                          -> `sprites`
    dump_zelda_tiles, dump_zelda_tiles_8x8,
    dump_range_zelda, inspect_candidates          -> `tiles`

All output goes under ``build/`` (the originals also copied into an external
``~/.gemini/.../brain/`` workspace — that violates the repo's "extracted assets go in
build/" rule, so it is intentionally dropped here).

Examples
--------
    # 4 CHR pages from decrypted flash (== extract_zelda2_chr.py)
    python3 scripts/debug/extract.py chr --console zelda

    # 4 CHR pages from a NES ROM file (== extract_zelda2_chr_rom.py)
    python3 scripts/debug/extract.py chr --console zelda --rom "build/Zelda II ... .nes"

    # Link walking frames, left + right (== extract_correct_link.py)
    python3 scripts/debug/extract.py sprites --console zelda

    # Full 16x16 G&W tileset sheet (== dump_zelda_tiles.py)
    python3 scripts/debug/extract.py tiles --console zelda --grid --tile-px 16 \\
        --layout linear --tiles 0:255 -o build/zelda_tileset.png

    # 8x8 interpretation, 4x scale (== dump_zelda_tiles_8x8.py)
    python3 scripts/debug/extract.py tiles --console zelda --grid --tile-px 8 \\
        --cols 16 --scale 4 --tiles 0:1023 -o build/zelda_tileset_8x8.png

    # Individual quadrant-mapped tiles 115-145, 4x (== dump_range_zelda.py)
    python3 scripts/debug/extract.py tiles --console zelda --tiles 115:145 \\
        --layout quadrant --scale 4 -o build/candidates_range
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio, imaging, nesgfx, palette as palmod
from common import resolve
from common.zelda import chr_and_sprite_palette


def cmd_chr(args):
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    out_dir = resolve("build")
    out_dir.mkdir(parents=True, exist_ok=True)
    for page in range(4):
        base = page * 2048
        tiles = [palmod.apply(nesgfx.decode_chr_tile(chr_rom, base + i), sprite_palette)
                 for i in range(2048)]
        img = imaging.plain_grid(tiles, cols=32, tile_w=8, tile_h=8, scale=1)
        path = out_dir / f"zelda2_chr_pg{page}.png"
        img.save(path)
        print(f"Saved CHR-ROM page {page} to {path} ({img.width}x{img.height})")


def cmd_sprites(args):
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    out_dir = resolve(args.output or "build/extracted_link")
    out_dir.mkdir(parents=True, exist_ok=True)
    page_tile_offset = args.page * 2048
    for idx, start_t in enumerate(args.frames):
        sprite = imaging.metasprite_image(chr_rom, page_tile_offset + start_t, sprite_palette,
                                          scale=1, layout=args.layout)
        left = sprite.resize((16 * args.scale, 32 * args.scale), Image.Resampling.NEAREST)
        left.save(out_dir / f"link_walk_left_{idx}.png")
        right = sprite.transpose(Image.Transpose.FLIP_LEFT_RIGHT).resize(
            (16 * args.scale, 32 * args.scale), Image.Resampling.NEAREST)
        right.save(out_dir / f"link_walk_right_{idx}.png")
        print(f"Extracted walking frame {idx} (left/right)")


def cmd_tiles(args):
    data = flashio.load_patched_external(args.console) if not args.input \
        else flashio.load_raw(args.input)
    offset = args.offset if args.offset is not None else flashio.info(args.console)["gnw_tileset_offset"]
    palette = palmod.load_gnw_palette(data, args.console) if not args.input \
        else palmod.bgra_to_rgb(data, flashio.info(args.console)["gnw_palette_offset"], 256)
    tiles = _parse_tiles(args.tiles)

    def tile_rgb(idx):
        return palmod.apply(nesgfx.read_gnw_tile(data, offset, idx, args.tile_px, args.layout),
                            palette, missing=(0, 0, 0))

    if args.grid:
        cols = args.cols
        grid = imaging.plain_grid([tile_rgb(t) for t in tiles], cols=cols,
                                  tile_w=args.tile_px, tile_h=args.tile_px, scale=args.scale)
        out = resolve(args.output or f"build/{args.console}_tileset.png")
        out.parent.mkdir(parents=True, exist_ok=True)
        grid.save(out)
        print(f"Saved tileset sheet to {out} ({grid.width}x{grid.height})")
    else:
        out_dir = resolve(args.output or "build/candidates")
        out_dir.mkdir(parents=True, exist_ok=True)
        for t in tiles:
            img = imaging.image_from_pixels(tile_rgb(t))
            if args.scale != 1:
                img = img.resize((args.tile_px * args.scale, args.tile_px * args.scale),
                                 Image.Resampling.NEAREST)
            path = out_dir / f"tile_{t:03d}.png"
            img.save(path)
            print(f"Saved tile {t} to {path}")


def _parse_tiles(spec: str) -> list[int]:
    spec = spec.strip()
    if ":" in spec:
        a, b = spec.split(":", 1)
        return list(range(int(a, 0), int(b, 0) + 1))
    return [int(x, 0) for x in spec.split(",") if x.strip()]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("chr", help="render 4 NES CHR pages as PNGs")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--rom", help="NES ROM file (default: reconstruct CHR from decrypted flash)")
    sp.set_defaults(func=cmd_chr)

    sp = sub.add_parser("sprites", help="extract 16x32 metasprite walking frames (left/right)")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--rom", help="NES ROM file (default: reconstruct CHR from decrypted flash)")
    sp.add_argument("--page", type=int, default=3)
    sp.add_argument("--frames", type=int, nargs="+", default=[0, 8, 40],
                    help="page-relative start tiles for each frame")
    sp.add_argument("--layout", default=nesgfx.DEFAULT_METASPRITE_LAYOUT,
                    choices=list(nesgfx.METASPRITE_LAYOUTS))
    sp.add_argument("--scale", type=int, default=8)
    sp.add_argument("-o", "--output", help="output directory (default build/extracted_link)")
    sp.set_defaults(func=cmd_sprites)

    sp = sub.add_parser("tiles", help="extract G&W 8bpp tiles as a grid sheet or individual PNGs")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--input", help="explicit input .bin (default build/patched_external_<console>.bin)")
    sp.add_argument("--offset", type=lambda s: int(s, 0), default=None)
    sp.add_argument("--tiles", required=True, help="A:B inclusive range or comma list")
    sp.add_argument("--tile-px", type=int, default=16, choices=[8, 16], dest="tile_px")
    sp.add_argument("--layout", choices=["linear", "quadrant"], default="linear")
    sp.add_argument("--grid", action="store_true", help="one sheet instead of per-tile PNGs")
    sp.add_argument("--cols", type=int, default=16)
    sp.add_argument("--scale", type=int, default=1)
    sp.add_argument("-o", "--output", help="output file (grid) or directory (individual)")
    sp.set_defaults(func=cmd_tiles)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
