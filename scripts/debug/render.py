#!/usr/bin/env python3
"""Render labelled tile sheets, NES CHR pages, and sprite-layout experiments.

Consolidates the ``render_*`` scripts plus ``try_layouts``:

    render_zelda1_labeled     -> `tileset`      (G&W 8bpp, builtin labelled grid)
    render_numbered_tileset   -> `numbered`     (G&W 8bpp via common.tileset.bytes_to_tilemap)
    render_page1/3_labeled    -> `page`         (NES CHR page, labelled)
    render_link_combinations  -> `grid`         (NES CHR page, plain grid)
    render_all_poses          -> `poses`        (NES metasprite contact sheet)
    try_layouts               -> `layouts`      (4 metasprite layout candidates side by side)

The NES-CHR commands (`page`/`grid`/`poses`/`layouts`) need a ROM: pass ``--rom PATH`` or
rely on the (currently empty) plaintext-flash path. `tileset` and `numbered` read the
patched external image and work out of the box.

Examples
--------
    python3 scripts/debug/render.py tileset --console zelda          # == render_zelda1_labeled
    python3 scripts/debug/render.py numbered --console zelda          # == render_numbered_tileset
    python3 scripts/debug/render.py page --page 3 --rom ROM.nes       # == render_page3_labeled
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio, imaging, nesgfx, palette as palmod, resolve
from common.zelda import chr_and_sprite_palette


def cmd_tileset(args):
    """Builtin labelled G&W tileset (== render_zelda1_labeled.py)."""
    data = flashio.load_patched_external(args.console)
    offset = flashio.info(args.console)["gnw_tileset_offset"]
    palette = palmod.load_gnw_palette(data, args.console)
    tiles = [palmod.apply(nesgfx.read_gnw_tile(data, offset, i, 16, "linear"), palette,
                          missing=(0, 0, 0)) for i in range(args.count)]
    img = imaging.labeled_grid(tiles, cols=args.cols, tile_px=16, scale=args.scale, pad=2,
                               border=(80, 80, 80), bg=(40, 40, 40), label=True)
    out = resolve(args.output or f"build/{args.console}1_tileset_labeled.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"Saved labeled tileset to {out}")


# --- common.tileset.bytes_to_tilemap engine (render_numbered_tileset.py) ------------- #
_NUMBERED_ROMS = {
    # console: (ext_bin, tileset_addr, palette_bytes, default_palette_base, layout, force_idx0_black)
    "zelda": ("build/patched_external_zelda.bin", 0x20000, 0x150, 0x2E81F4, "zelda", True),
    "mario": ("build/patched_external_mario.bin", 0x98B84, 0x140, 0xBEC68, "standard", False),
}
_WIDTH, _BLOCK, _TILESET_SIZE = 256, 16, 0x10000


def _load_font(px):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(p, px)
        except OSError:
            continue
    return ImageFont.load_default()


def cmd_numbered(args):
    """Numbered tileset via the firmware's own tilemap renderer (== render_numbered_tileset.py)."""
    from common.tileset import bytes_to_tilemap

    ext_bin, taddr, pbytes, defpal, layout, force_black = _NUMBERED_ROMS[args.console]
    base = args.palette_base if args.palette_base is not None else defpal
    scale = args.scale

    ext = flashio.load_raw(ext_bin)
    tdata = ext[taddr:taddr + _TILESET_SIZE]
    pal = bytearray(ext[base:base + pbytes])
    if force_black:
        pal[0:4] = b"\x00\x00\x00\x00"

    img = bytes_to_tilemap(tdata, palette=bytes(pal), block_size=_BLOCK, bpp=8,
                           width=_WIDTH, layout=layout).convert("RGB")
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    draw = ImageDraw.Draw(img)
    cell, cols = _BLOCK * scale, _WIDTH // _BLOCK
    rows = img.height // cell
    font = _load_font(max(7, cell // 7))
    for sy in range(rows):
        for sx in range(cols):
            idx = sy * cols + sx
            x0, y0 = sx * cell, sy * cell
            draw.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], outline=(60, 60, 60))
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                draw.text((x0 + 2 + dx, y0 + 1 + dy), str(idx), fill=(0, 0, 0), font=font)
            draw.text((x0 + 2, y0 + 1), str(idx), fill=(255, 255, 0), font=font)
    out = resolve("build/palettes") / f"tileset_numbered_{base:X}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"saved {out}  ({cols}x{rows} sprites, scale {scale}x, layout={layout})")


def cmd_page(args):
    """Labelled NES CHR page (== render_page1/3_labeled.py)."""
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    base = args.page * 2048
    tiles = [palmod.apply(nesgfx.decode_chr_tile(chr_rom, base + i), sprite_palette)
             for i in range(2048)]
    img = imaging.labeled_grid(tiles, cols=32, tile_px=8, scale=args.scale, pad=2,
                               border=(80, 80, 80), bg=(40, 40, 40), label=True)
    out = resolve(args.output or f"build/zelda2_page{args.page}_labeled.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"Saved labeled Page {args.page} sheet to {out}")


def cmd_grid(args):
    """Plain NES CHR page grid (== render_link_combinations.py)."""
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    base = args.page * 2048
    tiles = [palmod.apply(nesgfx.decode_chr_tile(chr_rom, base + i), sprite_palette)
             for i in range(args.count)]
    img = imaging.plain_grid(tiles, cols=args.cols, tile_w=8, tile_h=8, scale=args.scale)
    out = resolve(args.output or f"build/zelda2_page{args.page}_first{args.count}.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"Saved {args.count} tiles of Page {args.page} to {out}")


def cmd_poses(args):
    """NES metasprite contact sheet (== render_all_poses.py)."""
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    base = args.page * 2048
    scale = args.scale
    sheet_w = sheet_h = 8
    canvas = Image.new("RGB", (sheet_w * 18 * scale, sheet_h * 34 * scale), (60, 60, 60))
    for sy in range(sheet_h):
        for sx in range(sheet_w):
            start_t = (sy * sheet_w + sx) * 8
            sprite = imaging.metasprite_image(chr_rom, base + start_t, sprite_palette,
                                              scale=scale, layout=args.layout)
            canvas.paste(sprite, (sx * 18 * scale + scale, sy * 34 * scale + scale))
    out = resolve(args.output or "build/all_poses/page3_poses_grid.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)
    print(f"Saved all Page {args.page} poses to {out}")


def cmd_layouts(args):
    """Four metasprite layout candidates side by side per start tile (== try_layouts.py)."""
    chr_rom, sprite_palette = chr_and_sprite_palette(args.console, args.rom)
    base = args.page * 2048
    scale = args.scale
    out_dir = resolve(args.output or "build/layouts_test")
    out_dir.mkdir(parents=True, exist_ok=True)
    names = list(nesgfx.METASPRITE_LAYOUTS)
    for start_t in args.starts:
        canvas = Image.new("RGB", (4 * 18 * scale, 34 * scale), (80, 80, 80))
        for l_idx, name in enumerate(names):
            sprite = imaging.metasprite_image(chr_rom, base + start_t, sprite_palette,
                                              scale=scale, layout=name)
            canvas.paste(sprite, (l_idx * 18 * scale + scale, scale))
        path = out_dir / f"start_t{start_t:03d}.png"
        canvas.save(path)
        print(f"Saved layout test to {path}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("tileset", help="labelled G&W 8bpp tileset (builtin renderer)")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--count", type=int, default=256)
    sp.add_argument("--cols", type=int, default=16)
    sp.add_argument("--scale", type=int, default=2)
    sp.add_argument("-o", "--output")
    sp.set_defaults(func=cmd_tileset)

    sp = sub.add_parser("numbered", help="numbered G&W tileset via common.tileset.bytes_to_tilemap")
    sp.add_argument("--console", choices=list(_NUMBERED_ROMS), default="zelda")
    sp.add_argument("--palette-base", type=lambda s: int(s, 0), default=None, dest="palette_base")
    sp.add_argument("--scale", type=int, default=6)
    sp.set_defaults(func=cmd_numbered)

    def nes_common(sp):
        sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
        sp.add_argument("--rom", help="NES ROM file (else reconstruct from decrypted flash)")
        sp.add_argument("--page", type=int, default=3)
        sp.add_argument("--scale", type=int, default=4)
        sp.add_argument("-o", "--output")

    sp = sub.add_parser("page", help="labelled NES CHR page (32x64 tiles)")
    nes_common(sp)
    sp.set_defaults(func=cmd_page)

    sp = sub.add_parser("grid", help="plain NES CHR page grid")
    nes_common(sp)
    sp.add_argument("--count", type=int, default=128)
    sp.add_argument("--cols", type=int, default=16)
    sp.set_defaults(func=cmd_grid)

    sp = sub.add_parser("poses", help="NES metasprite 8x8 contact sheet")
    nes_common(sp)
    sp.add_argument("--layout", default=nesgfx.DEFAULT_METASPRITE_LAYOUT,
                    choices=list(nesgfx.METASPRITE_LAYOUTS))
    sp.set_defaults(func=cmd_poses)

    sp = sub.add_parser("layouts", help="compare the 4 metasprite layout candidates")
    nes_common(sp)
    sp.add_argument("--starts", type=int, nargs="+",
                    default=[0, 8, 16, 24, 32, 40, 48, 56, 64])
    sp.set_defaults(func=cmd_layouts)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
