"""Image / ASCII rendering helpers for tile grids and sprites.

Centralises the PNG tile-grid assembly (plain and labelled), the ASCII tile-art
dump, and metasprite composition that were copy-pasted across the render/dump/
inspect scripts. Built on Pillow.
"""
from __future__ import annotations

from PIL import Image, ImageDraw

from . import nesgfx


def image_from_pixels(pixels: list[list[tuple]]) -> Image.Image:
    """Build an RGB image from a 2D ``[y][x] -> (r, g, b)`` list."""
    h = len(pixels)
    w = len(pixels[0]) if h else 0
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = pixels[y][x]
    return img


def blit_scaled(px, tile_rgb: list[list[tuple]], x0: int, y0: int, scale: int) -> None:
    """Draw a tile (2D RGB grid) at ``(x0, y0)`` enlarged by integer *scale*."""
    for ty, row in enumerate(tile_rgb):
        for tx, rgb in enumerate(row):
            for dy in range(scale):
                for dx in range(scale):
                    px[x0 + tx * scale + dx, y0 + ty * scale + dy] = rgb


def plain_grid(tiles_rgb: list[list[list[tuple]]], cols: int, tile_w: int, tile_h: int,
               scale: int = 1, bg=(0, 0, 0)) -> Image.Image:
    """Lay out *tiles_rgb* (each a 2D RGB grid) in a borderless ``cols``-wide grid.

    Reproduces ``dump_zelda_tiles`` (scale=1) and ``dump_zelda_tiles_8x8`` (scale>1).
    """
    rows = (len(tiles_rgb) + cols - 1) // cols
    img = Image.new("RGB", (cols * tile_w * scale, rows * tile_h * scale), bg)
    px = img.load()
    for idx, tile in enumerate(tiles_rgb):
        tx, ty = idx % cols, idx // cols
        blit_scaled(px, tile, tx * tile_w * scale, ty * tile_h * scale, scale)
    return img


def labeled_grid(tiles_rgb: list[list[list[tuple]]], cols: int, tile_px: int, scale: int = 2,
                 pad: int = 2, border=(80, 80, 80), bg=(40, 40, 40), label=True,
                 text=(255, 255, 255), start_index: int = 0) -> Image.Image:
    """Grid with a per-cell border and tile-index label.

    Reproduces ``render_zelda1_labeled``: ``cell = tile_px*scale + pad`` with the tile
    inset by ``pad // 2`` and a border rectangle around each cell.
    """
    rows = (len(tiles_rgb) + cols - 1) // cols
    cell = tile_px * scale + pad
    inset = pad // 2
    img = Image.new("RGB", (cols * cell, rows * cell), bg)
    draw = ImageDraw.Draw(img)
    px = img.load()
    for idx, tile in enumerate(tiles_rgb):
        tx, ty = idx % cols, idx // cols
        cx, cy = tx * cell + inset, ty * cell + inset
        blit_scaled(px, tile, cx, cy, scale)
        draw.rectangle([cx - inset, cy - inset, cx + tile_px * scale, cy + tile_px * scale],
                       outline=border)
        if label:
            draw.text((cx + 1, cy + 1), str(start_index + idx), fill=text)
    return img


def tiles_to_ascii(data: bytes, offset: int, start: int, end: int, *, tile_px: int = 16,
                   layout: str = "linear", bytes_per_tile: int | None = None,
                   overflow: str = "hex") -> str:
    """ASCII-art dump of tiles ``start..end`` (inclusive).

    A non-zero byte renders as ``f"{b:01X}"`` (so a value >=16 becomes two hex chars).
    ``overflow="hex"`` (default) matches ``inspect_walls``/``inspect_dots``/``inspect_boxes``;
    ``overflow="mark"`` collapses values >=16 to ``"##"`` to match ``print_tiles``.
    """
    if bytes_per_tile is None:
        bytes_per_tile = tile_px * tile_px
    out = []
    for tile_idx in range(start, end + 1):
        base = offset + tile_idx * bytes_per_tile
        if base >= len(data):
            break
        tile = data[base:base + bytes_per_tile]
        non_zero = sum(1 for b in tile if b != 0)
        out.append(f"=== TILE {tile_idx} (Non-zero pixels: {non_zero}/{bytes_per_tile}) ===")
        grid = nesgfx.read_gnw_tile(data, offset, tile_idx, tile_px, layout)
        for row in grid:
            row_str = ""
            for b in row:
                if b == 0:
                    row_str += "  "
                elif overflow == "mark" and b >= 16:
                    row_str += "##"
                else:
                    row_str += f"{b:01X}"
            out.append(row_str)
        out.append("")
    return "\n".join(out)


def metasprite_image(chr_rom: bytes, start_tile: int, palette, scale: int = 1,
                     layout: str = nesgfx.DEFAULT_METASPRITE_LAYOUT) -> Image.Image:
    """Render an assembled 16x32 metasprite as an image (optionally scaled, nearest)."""
    img = image_from_pixels(nesgfx.assemble_metasprite(chr_rom, start_tile, palette, layout))
    if scale != 1:
        img = img.resize((16 * scale, 32 * scale), Image.Resampling.NEAREST)
    return img
