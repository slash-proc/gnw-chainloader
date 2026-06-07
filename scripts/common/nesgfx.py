"""NES / Game & Watch tile-graphics decoding.

Centralises the tile-decode logic that was copy-pasted across the extract/render/
inspect scripts:

- :func:`parse_ines` — iNES header parsing (from ``check_nes_header.py``).
- :func:`decode_chr_tile` — the NES 2bpp 8x8 decode loop (repeated ~8 times).
- :func:`read_gnw_tile` — the G&W 8bpp tileset reader, with the ``linear`` and
  ``quadrant`` layouts that different scripts used on the *same* data region.
- :data:`METASPRITE_LAYOUTS` — the four 16x32 metasprite mappings from ``try_layouts.py``.
"""
from __future__ import annotations

from collections import namedtuple

INesHeader = namedtuple("INesHeader", "valid prg_size chr_size mapper chr_offset")


def parse_ines(data: bytes) -> INesHeader:
    """Parse a 16-byte iNES header. ``chr_offset`` is relative to the ROM start."""
    if len(data) < 16 or not data[:16].startswith(b"NES\x1a"):
        return INesHeader(False, 0, 0, 0, 0)
    prg_pages, chr_pages = data[4], data[5]
    mapper = (data[7] & 0xF0) | (data[6] >> 4)
    prg_size = prg_pages * 16 * 1024
    chr_size = chr_pages * 8 * 1024
    return INesHeader(True, prg_size, chr_size, mapper, 16 + prg_size)


def decode_chr_tile(chr_rom: bytes, tile_idx: int) -> list[list[int]]:
    """Decode one 8x8 NES 2bpp tile (16 bytes) into an 8x8 grid of colour indices 0-3."""
    base = tile_idx * 16
    tile = chr_rom[base:base + 16]
    rows = []
    for py in range(8):
        low = tile[py] if py < len(tile) else 0
        high = tile[py + 8] if py + 8 < len(tile) else 0
        row = []
        for px in range(8):
            bit0 = (low >> (7 - px)) & 1
            bit1 = (high >> (7 - px)) & 1
            row.append((bit1 << 1) | bit0)
        rows.append(row)
    return rows


def read_gnw_tile(data: bytes, offset: int, tile_idx: int, tile_px: int = 16,
                  layout: str = "linear") -> list[list[int]]:
    """Read one Game & Watch 8bpp tile into a ``tile_px``-square grid of colour indices.

    ``layout="linear"``   — bytes stored row-major (``dump_zelda_tiles``, ``print_tiles``).
    ``layout="quadrant"`` — 16x16 split into four 8x8 quadrants, ``k = qx*2 + qy``
                            (``inspect_walls`` / ``inspect_dots`` / ``inspect_boxes``).
    """
    bytes_per_tile = tile_px * tile_px
    base = offset + tile_idx * bytes_per_tile
    tile = data[base:base + bytes_per_tile]
    grid = []
    for ty in range(tile_px):
        row = []
        for tx in range(tile_px):
            if layout == "quadrant" and tile_px == 16:
                qx, qy = tx // 8, ty // 8
                k = qx * 2 + qy
                idx = k * 64 + (ty % 8) * 8 + (tx % 8)
            else:  # linear (row-major)
                idx = ty * tile_px + tx
            row.append(tile[idx] if idx < len(tile) else 0)
        grid.append(row)
    return grid


# Four candidate 16x32 metasprite mappings (tile slot 0..7 -> top-left px in the sprite).
# Ported verbatim from try_layouts.py. "pairs_tl_tr_bl_br" is the confirmed Link layout.
METASPRITE_LAYOUTS = {
    "col_major": [(0, 0), (0, 8), (0, 16), (0, 24), (8, 0), (8, 8), (8, 16), (8, 24)],
    "row_major": [(0, 0), (8, 0), (0, 8), (8, 8), (0, 16), (8, 16), (0, 24), (8, 24)],
    "pairs_tl_tr_bl_br": [(0, 0), (0, 8), (8, 0), (8, 8), (0, 16), (0, 24), (8, 16), (8, 24)],
    "pairs_tl_bl_tr_br": [(0, 0), (0, 8), (0, 16), (0, 24), (8, 0), (8, 8), (8, 16), (8, 24)],
}
DEFAULT_METASPRITE_LAYOUT = "pairs_tl_tr_bl_br"


def assemble_metasprite(chr_rom: bytes, start_tile: int, palette,
                        layout: str = DEFAULT_METASPRITE_LAYOUT) -> list[list[tuple]]:
    """Assemble eight 8x8 CHR tiles into a 16x32 grid of RGB pixels using *layout*."""
    mapping = METASPRITE_LAYOUTS[layout]
    pixels = [[(0, 0, 0) for _ in range(16)] for _ in range(32)]
    for slot in range(8):
        tile = decode_chr_tile(chr_rom, start_tile + slot)
        ox, oy = mapping[slot]
        for py in range(8):
            for px in range(8):
                pixels[oy + py][ox + px] = palette[tile[py][px]]
    return pixels
