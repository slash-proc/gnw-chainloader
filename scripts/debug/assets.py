#!/usr/bin/env python3
"""Asset pipeline: decrypt / decompress flash and emit build + committed artifacts.

Folds the artifact-producing scripts into one tool (per the consolidation decision):

    decrypt_stock_flash                              -> `stock`
    extract_palette                                  -> `palette`
    scan_for_compressed_graphics, scan_lzma,
    scan_lzma_internal, decompress_and_dump,
    analyse_bins                                     -> `decompress`
    generate_mario_assets                            -> `mario-header`
    extract_zelda_assets                             -> `zelda`
    extract_zelda_palettes                           -> `zelda-palettes`
    download_font                                    -> `font`

Examples
--------
    python3 scripts/debug/assets.py stock --console mario       # OTFDEC stock decrypt + sha1 check
    python3 scripts/debug/assets.py palette --console mario     # mario_day_palette.bin + tileset bin
    python3 scripts/debug/assets.py mario-header                # -> src/chainloader/mario_assets.h
    python3 scripts/debug/assets.py zelda                       # -> build/extracted_zelda/*.png
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import compress, flashio, imaging, nesgfx, palette as palmod, resolve


def cmd_stock(args):
    """OTFDEC stock-flash decrypt (== decrypt_stock_flash.py)."""
    data, sha1_ok = flashio.decrypt_stock(args.console)
    if sha1_ok is not None:
        print(f"[{args.console}] encrypted-input sha1 {'OK' if sha1_ok else 'MISMATCH!'}")
    out = resolve(f"build_{args.console}") / "decrypt_flash_stock.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"-> {out} ({len(data)} bytes)")


def cmd_palette(args):
    """Decrypt Mario external and slice the day palette + clock tileset (== extract_palette.py)."""
    ext = flashio.load_decrypted(args.console)
    reg = flashio.info(args.console)
    pal = ext[reg["gnw_palette_offset"]:reg["gnw_palette_offset"] + 320]
    out_pal = resolve("build/mario_day_palette.bin")
    out_pal.parent.mkdir(parents=True, exist_ok=True)
    out_pal.write_bytes(pal)
    print(f"Saved palette -> {out_pal} ({len(pal)} bytes)")

    tiles = ext[reg["gnw_tileset_offset"]:reg["gnw_tileset_offset"] + 65536]
    out_tiles = resolve("build/decompressed_clock_graphics.bin")
    out_tiles.write_bytes(tiles)
    print(f"Saved decrypted tileset -> {out_tiles} ({len(tiles)} bytes)")


def cmd_decompress(args):
    """Find/inflate the compressed clock-graphics block (== scan_*/decompress_and_dump)."""
    out = resolve(args.output or "build/decompressed_clock_graphics.bin")
    out.parent.mkdir(parents=True, exist_ok=True)

    if args.algo == "lzma-internal":
        data = flashio.load_raw(args.input or "build/patched_internal_mario.bin")
        ptr = struct.unpack("<I", data[0x7350:0x7354])[0]
        if (ptr & 0xFF000000) != 0x08000000:
            raise SystemExit(f"pointer at 0x7350 ({ptr:#x}) is not internal flash")
        dec = compress.lzma_inflate(data[ptr - 0x08000000:])
        out.write_bytes(dec)
        print(f"lzma-internal: ptr {ptr:#x} -> {len(dec)} bytes -> {out}")
        return

    data = flashio.load_raw(args.input or "backup/decrypt_flash_patched.bin")
    if args.algo == "zlib":
        offset, kind, dec = compress.find_zlib_block(data, args.size)
        if dec is None:
            print("no zlib/deflate block found")
            return
        out.write_bytes(dec)
        print(f"{kind} block at {offset:#x} -> {len(dec)} bytes -> {out}")
    else:  # lzma
        offset, dec = compress.find_lzma_block(data, args.size)
        if dec is None:
            print("no LZMA block found")
            return
        out.write_bytes(dec)
        print(f"lzma block at {offset:#x} -> {len(dec)} bytes -> {out}")


def cmd_mario_header(args):
    """Generate src/chainloader/mario_assets.h (== generate_mario_assets.py)."""
    import numpy as np

    data = flashio.load_raw(args.tiles or "build/decompressed_clock_graphics.bin")
    pal_bytes = flashio.load_raw(args.palette or "build/mario_day_palette.bin")

    p = np.frombuffer(pal_bytes, dtype=np.uint8).reshape((-1, 4))
    palette_rgb565 = []
    for b, g, r, a in p:
        palette_rgb565.append(((int(r) & 0xF8) << 8) | ((int(g) & 0xFC) << 3) | (int(b) >> 3))

    def get_tile(i):
        off = i * 256
        return np.frombuffer(data[off:off + 256], dtype=np.uint8).reshape((16, 16))

    brick = get_tile(90)
    cloud = np.zeros((32, 48), dtype=np.uint8)
    for j, t in enumerate([9, 10, 11]):
        cloud[0:16, j * 16:j * 16 + 16] = get_tile(t)
    for j, t in enumerate([25, 26, 27]):
        cloud[16:32, j * 16:j * 16 + 16] = get_tile(t)
    coin_frames = [get_tile(i) for i in [112, 113, 114, 115]]
    yoshi_frames = []
    for tiles in [[132, 133, 148, 149], [134, 135, 150, 151], [136, 137, 152, 153]]:
        y = np.zeros((32, 32), dtype=np.uint8)
        y[0:16, 0:16], y[0:16, 16:32] = get_tile(tiles[0]), get_tile(tiles[1])
        y[16:32, 0:16], y[16:32, 16:32] = get_tile(tiles[2]), get_tile(tiles[3])
        yoshi_frames.append(y)

    font_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-!?: "
    char_mapping = {
        'A': (96, 'TL'), 'B': (96, 'TR'), 'C': (97, 'TL'), 'D': (97, 'TR'),
        'E': (98, 'TL'), 'F': (98, 'TR'), 'G': (99, 'TL'), 'H': (99, 'TR'),
        'I': (100, 'TL'), 'J': (100, 'TR'), 'K': (101, 'TL'), 'L': (101, 'TR'),
        'M': (102, 'TL'), 'N': (102, 'TR'), 'O': (103, 'TL'), 'P': (103, 'TR'),
        'Q': (104, 'TL'), 'R': (104, 'TR'), 'S': (105, 'TL'), 'T': (105, 'TR'),
        'U': (106, 'TL'), 'V': (106, 'TR'), 'W': (107, 'TL'), 'X': (107, 'TR'),
        'Y': (108, 'TL'), 'Z': (108, 'TR'),
        '0': (103, 'BL'), '1': (96, 'BL'), '2': (97, 'BL'), '3': (97, 'BR'),
        '4': (98, 'BL'), '5': (98, 'BR'), '6': (99, 'BL'), '7': (99, 'BR'),
        '8': (100, 'BL'), '9': (100, 'BR'),
        '.': (101, 'BL'), '-': (104, 'BL'), '!': (102, 'BL'), '?': (102, 'BR'),
        ':': (104, 'BR'), ' ': (101, 'BR'),
    }
    quad_offsets = {'TL': (0, 0), 'TR': (8, 0), 'BL': (0, 8), 'BR': (8, 8)}
    font_1bpp = []
    for c in font_chars:
        ti, quad = char_mapping[c]
        ox, oy = quad_offsets[quad]
        q = get_tile(ti)[oy:oy + 8, ox:ox + 8]
        font_1bpp.append([sum((1 << col) for col in range(8) if q[r, col] != 0) for r in range(8)])

    L = ["#ifndef MARIO_ASSETS_H", "#define MARIO_ASSETS_H", "", "#include <stdint.h>", ""]
    L += ["/* 80 colors day palette in RGB565 format */",
          "static const uint16_t mario_palette[80] __attribute__((unused)) = {"]
    for i in range(0, 80, 8):
        L.append("    " + ", ".join(f"0x{v:04X}" for v in palette_rgb565[i:i + 8]) + ",")
    L += ["};", "", "/* Mario font character lookup map */",
          f'#define MARIO_FONT_CHARS "{font_chars}"', "",
          "/* 8x8 Mario font in 1bpp (8 bytes per character) */",
          f"static const uint8_t mario_font[{len(font_chars)}][8] __attribute__((unused)) = {{"]
    for i, c in enumerate(font_chars):
        L.append("    { " + ", ".join(f"0x{b:02X}" for b in font_1bpp[i]) + f" }}, // '{c}'")
    L += ["};", "", "/* Brick tile (16x16) */",
          "static const uint8_t sprite_brick[256] __attribute__((unused)) = {"]
    for r in range(16):
        L.append("    " + ", ".join(f"{v}" for v in brick[r, :]) + ",")
    L += ["};", "", "/* Large cloud sprite (48x32) */",
          "static const uint8_t sprite_cloud[1536] __attribute__((unused)) = {"]
    for r in range(32):
        L.append("    " + ", ".join(f"{v}" for v in cloud[r, :]) + ",")
    L += ["};", "", "/* Spinning coin animation (4 frames, 16x16 each) */",
          "static const uint8_t sprite_coin[4][256] __attribute__((unused)) = {"]
    for f in range(4):
        L += [f"    // Frame {f}", "    {"]
        for r in range(16):
            L.append("        " + ", ".join(f"{v}" for v in coin_frames[f][r, :]) + ",")
        L.append("    },")
    L += ["};", "", "/* Green walking Yoshi (3 frames, 32x32 each) */",
          "static const uint8_t sprite_yoshi[3][1024] __attribute__((unused)) = {"]
    for f in range(3):
        L += [f"    // Frame {f}", "    {"]
        for r in range(32):
            L.append("        " + ", ".join(f"{v}" for v in yoshi_frames[f][r, :]) + ",")
        L.append("    },")
    L += ["};", "", "#endif // MARIO_ASSETS_H"]

    out = resolve("src/chainloader/mario_assets.h")
    out.write_text("\n".join(L))
    print(f"Generated {out} successfully!")


def extract_zelda(args):
    """Render the Zelda asset PNGs from the committed layout JSON.

    Authoritative extractor: the same nested-dict logic that used to run inline in
    scripts/build/patch_firmware.py, now living in common.zelda. Reads the patched
    external flash (build/patched_external_zelda.bin) — run ``make patch_zelda`` first.
    """
    from common import zelda as zeldamod

    ext_data = flashio.load_patched_external("zelda")
    metadata = json.loads(resolve(args.json or "src/chainloader/zelda_tiles_v3.json").read_text())
    reg = flashio.info("zelda")
    out_dir = resolve(args.output or "build/extracted_zelda")
    zeldamod.extract_assets(
        ext_data, metadata, out_dir,
        gnw_palette_offset=reg["gnw_palette_offset"],
        tiles_offset=reg["gnw_tileset_offset"],
        rom_offset=reg["rom_offset"],
        nes_palette_offset=reg["nes_master_palette_offset"],
    )
    print(f"Extracted Zelda assets to {out_dir}")


def cmd_font(args):
    """Download a clean fallback 8x8 font and emit build/fallback_font.h (== download_font.py)."""
    import re
    import urllib.request

    url = "https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h"
    print(f"Downloading from {url}...")
    with urllib.request.urlopen(url) as resp:
        content = resp.read().decode("utf-8")
    m = re.search(r'char font8x8_basic\[128\]\[8\] = \{(.*?)\};', content, re.DOTALL)
    if not m:
        raise SystemExit("could not find font8x8_basic array")
    blocks = re.findall(r'\{(.*?)\}(.*?)(?=\n|\r|$)', m.group(1))
    font_map = {}
    for ascii_val, (block, comment) in enumerate(blocks):
        cm = re.search(r'U\+[0-9A-Fa-f]+ (.*)', comment)
        desc = cm.group(1).strip() if cm else str(ascii_val)
        font_map[ascii_val] = ([int(v.strip(), 16) for v in block.split(',') if v.strip()], desc)
    chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-!?: "
    out_lines = ["/* Clean public domain fallback font (8x8 1bpp) */",
                 "static const uint8_t fallback_font[42][8] = {"]
    for c in chars:
        if ord(c) in font_map:
            hv, desc = font_map[ord(c)]
            out_lines.append("    { " + ", ".join(f"0x{v:02X}" for v in hv) + f" }}, // '{c}' ({desc})")
    out_lines.append("};")
    out = resolve("build/fallback_font.h")
    out.write_text("\n".join(out_lines))
    print(f"Saved to {out}")


def cmd_zelda_palettes(args):
    """Render the 12 firmware clock palettes over the tileset (== extract_zelda_palettes.py).

    Needs build/zelda/decrypt_flash_patched.bin + internal_flash_backup_zelda.bin and the
    patch repo's bytes_to_tilemap (the originals' inputs are not in the default tree).
    """
    from common.tileset import bytes_to_tilemap

    ext = flashio.load_raw(args.external or "build/zelda/decrypt_flash_patched.bin")
    intl = flashio.load_raw(args.internal or "internal_flash_backup_zelda.bin")
    out_dir = resolve(args.output or "build/zelda/palettes")
    out_dir.mkdir(parents=True, exist_ok=True)
    ptr_table, count, pal_bytes, ext_base = 0x1B688, 12, 0x150, 0x90000000
    tileset = ext[0x20000:0x20000 + 0x10000]
    table = intl[ptr_table:ptr_table + 4 * count]
    for i in range(count):
        ptr = struct.unpack_from("<I", table, 4 * i)[0]
        base = (ptr & ~0x3) - ext_base
        pal = bytearray(ext[base:base + pal_bytes])
        pal[0:4] = b"\x00\x00\x00\x00"
        img = bytes_to_tilemap(tileset, palette=bytes(pal), block_size=16, bpp=8,
                               width=256, layout="zelda").convert("RGB")
        img.save(out_dir / f"tileset_pal{i:02d}_{base:X}.png")
        print(f"palette {i:2d}: ext 0x{base:X}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("stock", help="OTFDEC stock-flash decrypt")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="mario")
    sp.set_defaults(func=cmd_stock)

    sp = sub.add_parser("palette", help="decrypt external and slice palette + clock tileset")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="mario")
    sp.set_defaults(func=cmd_palette)

    sp = sub.add_parser("decompress", help="find/inflate the compressed clock-graphics block")
    sp.add_argument("--algo", choices=["zlib", "lzma", "lzma-internal"], default="zlib")
    sp.add_argument("--input", help="input .bin (defaults per algo)")
    sp.add_argument("--size", type=lambda s: int(s, 0), default=65536)
    sp.add_argument("-o", "--output")
    sp.set_defaults(func=cmd_decompress)

    sp = sub.add_parser("mario-header", help="emit src/chainloader/mario_assets.h")
    sp.add_argument("--tiles", help="decompressed clock graphics .bin")
    sp.add_argument("--palette", help="mario_day_palette.bin")
    sp.set_defaults(func=cmd_mario_header)

    sp = sub.add_parser("zelda", help="render Zelda assets from the layout JSON")
    sp.add_argument("--json", help="layout JSON (default src/chainloader/zelda_tiles_v3.json)")
    sp.add_argument("-o", "--output", help="output dir (default build/extracted_zelda)")
    sp.set_defaults(func=extract_zelda)

    sp = sub.add_parser("font", help="download a clean fallback 8x8 font header")
    sp.set_defaults(func=cmd_font)

    sp = sub.add_parser("zelda-palettes", help="render the 12 firmware clock palettes")
    sp.add_argument("--external")
    sp.add_argument("--internal")
    sp.add_argument("-o", "--output")
    sp.set_defaults(func=cmd_zelda_palettes)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
