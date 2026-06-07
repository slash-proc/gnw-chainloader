#!/usr/bin/env python3
"""Validate ROMs / flash images and run feasibility checks.

Consolidates the check_*/verify_*/find-sprite validation scripts:

    check_nes_header                                   -> `header`
    check_zelda2_rom, check_decrypted_stock_zelda2,
    check_raw_backup_zelda2, check_host_zelda2         -> `magic`
    check_mario_pattern, check_patterns                -> `tile-stats`
    check_zlib                                         -> (use `assets.py decompress --algo zlib`)
    verify_zelda_link, find_link_in_tileset            -> `find-sprite`
    verify_palette_compressed_memory                   -> `palette-memory`
    verify_framebuffer_colors                          -> `framebuffer-color`
    find_nonempty_tiles                                -> `nonempty` (needs a ROM)

Examples
--------
    python3 scripts/debug/romcheck.py palette-memory                 # rwdata LZMA palette check
    python3 scripts/debug/romcheck.py magic --console zelda --source patched
    python3 scripts/debug/romcheck.py find-sprite --cells 122,133,139,141,214
"""
from __future__ import annotations

import argparse
import lzma
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio, nesgfx, resolve


# --------------------------------------------------------------------------- #
def cmd_header(args):
    """Parse an iNES header (== check_nes_header.py)."""
    data = flashio.load_raw(args.rom) if args.rom else flashio.load_decrypted(args.console)
    print(f"File size: {len(data)} bytes")
    hdr = nesgfx.parse_ines(data)
    print(f"Header: {data[:16].hex()}")
    if not hdr.valid:
        print("Not a valid iNES file (magic mismatch).")
        return
    print(f"PRG ROM size: {hdr.prg_size} bytes")
    print(f"CHR ROM size: {hdr.chr_size} bytes")
    print(f"Mapper ID: {hdr.mapper}")


def cmd_magic(args):
    """Locate the NES iNES magic in a flash image (== check_zelda2_rom / check_*_backup)."""
    if args.source == "patched":
        data = flashio.load_patched_external(args.console)
    elif args.source == "decrypted":
        data = flashio.load_decrypted(args.console)
    else:  # raw
        data = flashio.load_raw(flashio.info(args.console)["external"])
    rom_off = flashio.info(args.console)["rom_offset"] or 0x70000
    header = data[rom_off:rom_off + 16]
    print(f"Loaded {len(data)} bytes ({args.source})")
    print(f"Header at 0x{rom_off:X}: {header.hex()} (ASCII: {header[:4]})")
    if header.startswith(b"NES\x1a"):
        print(f"iNES magic found at 0x{rom_off:X}! CHR-ROM at iNES offset 0x20010")
    else:
        found = data.find(b"NES\x1a")
        print(f"No iNES magic at 0x{rom_off:X}. First NES\\x1a anywhere: "
              f"{hex(found) if found >= 0 else 'none'}")


def cmd_tile_stats(args):
    """Per-tile byte statistics: sum / xor / unique colours (== check_mario_pattern/check_patterns)."""
    data = flashio.load_raw(args.input)
    offset = args.offset
    for t in _parse_ints(args.tiles):
        tile = data[offset + t * 256:offset + t * 256 + 256]
        xor = 0
        for b in tile:
            xor ^= b
        print(f"Tile {t}: first16={[int(b) for b in tile[:16]]}")
        print(f"  sum={sum(tile)}  xor={xor}  unique={sorted(set(tile))}")


def cmd_find_sprite(args):
    """ASCII-classify Zelda cells by colour group (== verify_zelda_link.py)."""
    ext = flashio.load_decrypted("zelda")
    offset = flashio.info("zelda")["gnw_tileset_offset"]
    green = {36, 43, 51, 52, 53, 56, 81}
    skin = {6, 18, 26, 41, 48, 64, 65, 66, 67, 78, 79}
    for cell in _parse_ints(args.cells):
        print(f"\n--- Cell {cell} ASCII Art ---")
        grid = nesgfx.read_gnw_tile(ext, offset, cell, 16, "quadrant")
        for row in grid:
            line = ""
            for p in row:
                line += " " if p == 0 else ("G" if p in green else ("S" if p in skin else "."))
            print(line)


# --- verify_palette_compressed_memory.py (faithful port) ------------------------------ #
_RWDATA_OFFSET = 0x180A4
_MAX_TABLE_ELEMENTS = 5
_PALETTE_OFFSET_IN_CM = 0xCD54
_PALETTE_BYTES = 320
_EXT_PALETTE_OFFSET = 0xBEC68
_DICT_SIZE = 16 * 1024


def _u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def _lzma_inflate_alone(blob, need_len):
    header = bytes([0x5D]) + struct.pack("<I", _DICT_SIZE) + struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
    out = b""
    try:
        out = dec.decompress(header + blob, max_length=need_len + 4096)
    except lzma.LZMAError as e:
        print(f"  (LZMA note: {e}; got {len(out)} bytes before stop)")
    return out


def _walk_rwdata(fw):
    e = _RWDATA_OFFSET
    for _n in range(_MAX_TABLE_ELEMENTS):
        inflate_ptr = _u32(fw, e + 0)
        rel_addr = _u32(fw, e + 4)
        data_len = _u32(fw, e + 8)
        dst = _u32(fw, e + 12)
        if inflate_ptr in (0x77777777, 0x00000000) or dst == 0x77777777:
            break
        yield e, inflate_ptr, (e + 4 + rel_addr) & 0xFFFFFFFF, data_len, dst
        e += 16


def cmd_palette_memory(args):
    """Verify the day palette can be sourced from patched internal flash (== verify_palette_compressed_memory.py)."""
    fw = flashio.load_raw("build/patched_internal_mario.bin")
    print(f"Loaded patched_internal_mario.bin ({len(fw)} bytes)")
    print(f"Walking rwdata table at 0x{_RWDATA_OFFSET:05X}:")
    need = _PALETTE_OFFSET_IN_CM + _PALETTE_BYTES
    bg = bytes([0xFF, 0x73, 0x5F, 0x00])
    candidate = None
    for n, (e, inflate_ptr, data_addr, data_len, dst) in enumerate(_walk_rwdata(fw)):
        dec = _lzma_inflate_alone(fw[data_addr:data_addr + data_len], need)
        has_pal = len(dec) >= need and bg in dec[_PALETTE_OFFSET_IN_CM:_PALETTE_OFFSET_IN_CM + _PALETTE_BYTES]
        tag = "  <-- palette@0xCD54" if has_pal else ""
        print(f"  [{n}] @0x{e:05X}: inflate=0x{inflate_ptr:08X} data@0x{data_addr:05X} "
              f"len=0x{data_len:X} dst=0x{dst:08X} -> inflated 0x{len(dec):X}{tag}")
        if has_pal and candidate is None:
            candidate = dec[_PALETTE_OFFSET_IN_CM:_PALETTE_OFFSET_IN_CM + _PALETTE_BYTES]
    if candidate is None:
        print("ERROR: no entry inflated to contain the palette signature at 0xCD54.")
        sys.exit(2)
    print(f"candidate palette @0xCD54 first 16 bytes: {candidate[:16].hex()}")
    try:
        truth = flashio.load_decrypted("mario")[_EXT_PALETTE_OFFSET:_EXT_PALETTE_OFFSET + _PALETTE_BYTES]
        print(f"ground-truth palette  @0xBEC68 first 16 bytes: {truth[:16].hex()}")
        print(f"\n==> candidate == ground truth: {candidate == truth}")
        sys.exit(0 if candidate == truth else 4)
    except Exception as ex:  # noqa: BLE001
        print(f"(could not load ground truth via MarioGnW: {ex})")


def cmd_framebuffer_color(args):
    """Check a captured framebuffer PNG's dominant background colour (== verify_framebuffer_colors.py)."""
    from collections import Counter

    from PIL import Image
    img = Image.open(resolve(args.input)).convert("RGB")
    colors = Counter(img.getdata())
    (r, g, b), n = colors.most_common(1)[0]
    print(f"Dominant color: #{r:02X}{g:02X}{b:02X} ({n} px of {img.width * img.height})")
    print(f"Blue background: {'PASS' if b > 200 and b > r and b > g else 'FAIL'}")


def _parse_ints(spec: str) -> list[int]:
    spec = spec.strip()
    if ":" in spec:
        a, b = spec.split(":", 1)
        return list(range(int(a, 0), int(b, 0) + 1))
    return [int(x, 0) for x in spec.split(",") if x.strip()]


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("header", help="parse an iNES header")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--rom", help="NES ROM file (else decrypted flash)")
    sp.set_defaults(func=cmd_header)

    sp = sub.add_parser("magic", help="locate the NES iNES magic in a flash image")
    sp.add_argument("--console", choices=list(flashio.CONSOLES), default="zelda")
    sp.add_argument("--source", choices=["patched", "decrypted", "raw"], default="patched")
    sp.set_defaults(func=cmd_magic)

    sp = sub.add_parser("tile-stats", help="per-tile sum/xor/unique-colour stats")
    sp.add_argument("--input", required=True)
    sp.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    sp.add_argument("--tiles", required=True, help="comma list or A:B range")
    sp.set_defaults(func=cmd_tile_stats)

    sp = sub.add_parser("find-sprite", help="ASCII-classify Zelda cells by colour group")
    sp.add_argument("--cells", default="122,133,139,141,214")
    sp.set_defaults(func=cmd_find_sprite)

    sp = sub.add_parser("palette-memory", help="verify day palette is recoverable from internal flash")
    sp.set_defaults(func=cmd_palette_memory)

    sp = sub.add_parser("framebuffer-color", help="check a captured framebuffer PNG's background")
    sp.add_argument("--input", default="build/framebuffer_after.png")
    sp.set_defaults(func=cmd_framebuffer_color)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
