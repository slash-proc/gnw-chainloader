"""Palette loading, conversion, and colour analysis.

Centralises the BGRA->RGB conversion (the G&W menu palettes) and the NES master
palette read (RGB triples) that were duplicated across the asset scripts.
"""
from __future__ import annotations

from collections import Counter

from . import flashio


def bgra_to_rgb(data: bytes, offset: int, count: int) -> list[tuple]:
    """Read *count* BGRA quads at *offset* and return ``(r, g, b)`` tuples.

    Stops early if the data runs out (matches the originals' bounds check).
    """
    out = []
    for i in range(count):
        c = offset + i * 4
        if c + 4 > len(data):
            break
        b, g, r, _a = data[c:c + 4]
        out.append((r, g, b))
    return out


def load_gnw_palette(data: bytes, console: str, count: int = 256) -> list[tuple]:
    """G&W menu palette (BGRA) from a patched-external image, via the offset registry."""
    return bgra_to_rgb(data, flashio.info(console)["gnw_palette_offset"], count)


def load_nes_master_palette(data: bytes, offset: int | None = None, count: int = 64) -> list[tuple]:
    """NES master palette: *count* RGB triples (3 bytes each) at *offset*."""
    if offset is None:
        offset = flashio.CONSOLES["zelda"]["nes_master_palette_offset"]
    out = []
    for i in range(count):
        c = offset + i * 3
        r, g, b = data[c:c + 3]
        out.append((r, g, b))
    return out


def link_sprite_palette(nes_palette: list[tuple]) -> list[tuple]:
    """The 4-colour Zelda II Link sprite palette selected from the master palette."""
    return [nes_palette[i] for i in flashio.LINK_COLOR_INDICES]


def apply(grid: list[list[int]], palette: list[tuple], missing=(0, 0, 0)) -> list[list[tuple]]:
    """Map a 2D grid of colour indices to RGB via *palette*."""
    return [[palette[i] if i < len(palette) else missing for i in row] for row in grid]


def count_colors(data: bytes, offset: int = 0, length: int | None = None) -> Counter:
    """Histogram of byte (colour-index) frequency over ``data[offset:offset+length]``."""
    end = len(data) if length is None else offset + length
    return Counter(data[offset:end])
