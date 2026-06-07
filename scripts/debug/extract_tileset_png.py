#!/usr/bin/env python3
"""
extract_tileset_png.py

Extracts the Mario OFW clock tileset from build/decompressed_clock_graphics.bin,
reads the actual BGRA palette from build/patched_external_mario.bin (already decrypted),
and produces:
  - build/tileset_full.png     : entire tileset as a sprite sheet (16 tiles wide)
  - build/tileset_letters.png  : only the known letter/digit tiles (96-108), annotated
  - build/tileset_ascii.txt    : ASCII art of every non-blank tile (using palette brightness)
  - build/letters_ascii.txt    : ASCII art of letter/digit tiles with char labels

Tile format: 8bpp, 16x16 pixels, palette-indexed (1 byte per pixel, row-major).
Palette format: BGRA, 4 bytes per colour, 80 colours, at offset 0xBEC68 in patched_external_mario.bin.
Palette handling mirrors scripts/common/tileset.py (vendored from game-and-watch-patch) exactly:
  p = p[:, :3]; p = np.fliplr(p)  # drop A, BGR->RGB; use PIL putpalette()
"""

import sys
from pathlib import Path
from math import ceil

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    print("ERROR: numpy and Pillow are required. Install with: pip install numpy Pillow")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
TILESET_BIN       = Path("build/decompressed_clock_graphics.bin")
PALETTE_BIN       = Path("build/mario_day_palette.bin")  # extracted from decrypted stock backup
OUT_FULL          = Path("build/tileset_full.png")
OUT_LETTERS       = Path("build/tileset_letters.png")
OUT_ASCII         = Path("build/tileset_ascii.txt")
OUT_LETTERS_ASCII = Path("build/letters_ascii.txt")

# ---------------------------------------------------------------------------
# Palette: 320 raw BGRA bytes extracted from the decrypted stock backup
# by extract_palette.py (which calls device.crypt() before reading 0xBEC68).
# ---------------------------------------------------------------------------
PALETTE_BYTES   = 320   # 80 colours * 4 bytes each
PALETTE_COLOURS = 80

# ---------------------------------------------------------------------------
# Tile geometry
# ---------------------------------------------------------------------------
TILE_W     = 16
TILE_H     = 16
TILE_BYTES = TILE_W * TILE_H   # 256 bytes per tile
BLOCK_SIZE = 16

# ---------------------------------------------------------------------------
# Known character mapping (tile_index -> [(char, quadrant), ...])
# Quadrants: TL=top-left 8x8, TR=top-right 8x8, BL=bottom-left 8x8, BR=bottom-right 8x8
# ---------------------------------------------------------------------------
CHAR_MAP = {
    96:  [('A','TL'), ('B','TR'), ('1','BL'), ('C','BR')],
    97:  [('C','TL'), ('D','TR'), ('2','BL'), ('3','BR')],
    98:  [('E','TL'), ('F','TR'), ('4','BL'), ('5','BR')],
    99:  [('G','TL'), ('H','TR'), ('6','BL'), ('7','BR')],
    100: [('I','TL'), ('J','TR'), ('8','BL'), ('9','BR')],
    101: [('K','TL'), ('L','TR'), ('.','BL'), (' ','BR')],
    102: [('M','TL'), ('N','TR'), ('!','BL'), ('?','BR')],
    103: [('O','TL'), ('P','TR'), ('0','BL'), ('X','BR')],
    104: [('Q','TL'), ('R','TR'), ('-','BL'), (':','BR')],
    105: [('S','TL'), ('T','TR'), (' ','BL'), (' ','BR')],
    106: [('U','TL'), ('V','TR'), (' ','BL'), (' ','BR')],
    107: [('W','TL'), ('X','TR'), (' ','BL'), (' ','BR')],
    108: [('Y','TL'), ('Z','TR'), (' ','BL'), (' ','BR')],
}

# ASCII density ramp (index 0=transparent/dark -> bright)
DENSITY = " .:;+*#@"

# ---------------------------------------------------------------------------
# Core palette helpers — identical logic to patches/tileset.py
# ---------------------------------------------------------------------------
def load_palette_raw() -> bytes:
    """Return 320 raw BGRA bytes from the pre-extracted palette file."""
    if not PALETTE_BIN.exists():
        print(f"ERROR: {PALETTE_BIN} not found. Run extract_palette.py first.")
        import sys; sys.exit(1)
    return PALETTE_BIN.read_bytes()

def build_pil_palette(palette_raw: bytes) -> bytes:
    """
    Build a flat 768-byte RGB palette for PIL putpalette(), padded to 256 colours.
    Mirrors tileset.py lines 114-116:
        p = np.frombuffer(palette, ...).reshape((-1, 4))
        p = p[:, :3]       # drop A
        p = np.fliplr(p)   # BGR -> RGB
    """
    p = np.frombuffer(palette_raw, dtype=np.uint8).reshape((-1, 4))
    p = p[:, :3]
    p = np.fliplr(p)   # BGR -> RGB
    flat = p.flatten().tolist()
    flat += [0] * (768 - len(flat))   # pad to 256 colours
    return bytes(flat)

def palette_to_rgb_list(palette_raw: bytes) -> list:
    """Return a list of (R,G,B) tuples for ASCII luminance mapping."""
    pil = build_pil_palette(palette_raw)
    return [(pil[i*3], pil[i*3+1], pil[i*3+2]) for i in range(256)]

# ---------------------------------------------------------------------------
# Tile layout helpers — mirrors bytes_to_tilemap() "standard" layout
# ---------------------------------------------------------------------------
def tileset_to_canvas(tileset_data: bytes, width: int = 256) -> np.ndarray:
    """Assemble tile bytes into a palette-indexed 2D numpy canvas."""
    num_tiles = len(tileset_data) // TILE_BYTES
    sprites_per_row = width // BLOCK_SIZE
    h = int(ceil(num_tiles / sprites_per_row)) * BLOCK_SIZE
    canvas = np.zeros((h, width), dtype=np.uint8)
    for i in range(num_tiles):
        sprite = tileset_data[i * TILE_BYTES : (i + 1) * TILE_BYTES]
        if len(sprite) < TILE_BYTES:
            break
        x = (i % sprites_per_row) * BLOCK_SIZE
        y = (i // sprites_per_row) * BLOCK_SIZE
        canvas[y:y+BLOCK_SIZE, x:x+BLOCK_SIZE] = np.frombuffer(sprite, dtype=np.uint8).reshape(BLOCK_SIZE, BLOCK_SIZE)
    return canvas

def render_tileset(tileset_data: bytes, pil_pal: bytes) -> Image.Image:
    """Render full tileset as RGB image, identical to bytes_to_tilemap()."""
    canvas = tileset_to_canvas(tileset_data)
    im = Image.fromarray(canvas, "P")
    im.putpalette(pil_pal)
    return im.convert("RGB")

def get_tile_array(tileset_data: bytes, tile_idx: int) -> np.ndarray:
    off = tile_idx * TILE_BYTES
    return np.frombuffer(tileset_data[off:off+TILE_BYTES], dtype=np.uint8).reshape(TILE_H, TILE_W)

def render_tile(tileset_data: bytes, tile_idx: int, pil_pal: bytes, scale: int = 1) -> Image.Image:
    arr = get_tile_array(tileset_data, tile_idx)
    im = Image.fromarray(arr, "P")
    im.putpalette(pil_pal)
    im = im.convert("RGB")
    if scale > 1:
        im = im.resize((TILE_W * scale, TILE_H * scale), Image.NEAREST)
    return im

# ---------------------------------------------------------------------------
# ASCII helpers
# ---------------------------------------------------------------------------
def _idx_to_ascii(idx: int, colours: list) -> str:
    if idx == 0:
        return " "
    r, g, b = colours[idx % len(colours)]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    return DENSITY[int(lum / 255 * (len(DENSITY) - 1))]

def tile_to_ascii_lines(tileset_data: bytes, tile_idx: int, colours: list) -> list:
    arr = get_tile_array(tileset_data, tile_idx)
    return ["".join(_idx_to_ascii(int(arr[y, x]), colours) for x in range(TILE_W)) for y in range(TILE_H)]

def quadrant_to_ascii(tileset_data: bytes, tile_idx: int, ox: int, oy: int, colours: list) -> list:
    arr = get_tile_array(tileset_data, tile_idx)
    return ["".join(_idx_to_ascii(int(arr[oy+y, ox+x]), colours) for x in range(8)) for y in range(8)]

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    if not TILESET_BIN.exists():
        print(f"ERROR: {TILESET_BIN} not found. Run scan_lzma_internal.py first.")
        sys.exit(1)

    tileset_data = TILESET_BIN.read_bytes()
    num_tiles = len(tileset_data) // TILE_BYTES
    print(f"Loaded {len(tileset_data)} bytes -> {num_tiles} tiles ({TILE_W}x{TILE_H})")

    palette_raw = load_palette_raw()
    colours = palette_to_rgb_list(palette_raw)
    pil_pal = build_pil_palette(palette_raw)
    print(f"Palette: {PALETTE_COLOURS} colours from {PALETTE_BIN}")
    print(f"  colour[0]  = #{colours[0][0]:02X}{colours[0][1]:02X}{colours[0][2]:02X}  (sky/background)")
    print(f"  colour[1]  = #{colours[1][0]:02X}{colours[1][1]:02X}{colours[1][2]:02X}")
    print(f"  colour[9]  = #{colours[9][0]:02X}{colours[9][1]:02X}{colours[9][2]:02X}")

    # ------------------------------------------------------------------
    # 1. Full sprite sheet (same rendering as game-and-watch-patch tileset.png)
    # ------------------------------------------------------------------
    SCALE = 3
    # Render using the reference tileset layout (16 tiles wide = 256px)
    full_rgb = render_tileset(tileset_data, pil_pal)
    full_img = full_rgb.resize((full_rgb.width * SCALE, full_rgb.height * SCALE), Image.NEAREST)

    draw = ImageDraw.Draw(full_img)
    sprites_per_row = 256 // BLOCK_SIZE  # 16
    for tile_idx in range(num_tiles):
        col = tile_idx % sprites_per_row
        row = tile_idx // sprites_per_row
        draw.text((col * BLOCK_SIZE * SCALE + 1, row * BLOCK_SIZE * SCALE + 1),
                  str(tile_idx), fill=(255, 255, 0))

    full_img.save(OUT_FULL)
    print(f"Saved full sprite sheet -> {OUT_FULL}  ({full_img.width}x{full_img.height}px)")

    # ------------------------------------------------------------------
    # 2. Letter tiles annotated sheet
    # ------------------------------------------------------------------
    LSCALE = 6
    letter_tile_ids = sorted(CHAR_MAP.keys())
    L_COLS = 4
    L_ROWS = (len(letter_tile_ids) + L_COLS - 1) // L_COLS
    CELL_W = TILE_W * LSCALE + 4
    CELL_H = TILE_H * LSCALE + 18

    lsheet = Image.new("RGB", (L_COLS * CELL_W + 4, L_ROWS * CELL_H + 4), (10, 10, 10))
    ldraw  = ImageDraw.Draw(lsheet)

    for i, tile_idx in enumerate(letter_tile_ids):
        col = i % L_COLS
        row = i // L_COLS
        x = 2 + col * CELL_W
        y = 2 + row * CELL_H
        tile_img = render_tile(tileset_data, tile_idx, pil_pal, scale=LSCALE)
        lsheet.paste(tile_img, (x, y))
        chars = CHAR_MAP[tile_idx]
        label = " ".join(f"{q}={repr(c)}" for c, q in chars)
        ldraw.text((x, y + TILE_H * LSCALE + 2), f"[{tile_idx}] {label}", fill=(220, 220, 80))

    lsheet.save(OUT_LETTERS)
    print(f"Saved letters sheet    -> {OUT_LETTERS}  ({lsheet.width}x{lsheet.height}px)")

    # ------------------------------------------------------------------
    # 3. Full ASCII dump of every non-blank tile
    # ------------------------------------------------------------------
    ascii_lines = []
    for tile_idx in range(num_tiles):
        arr = get_tile_array(tileset_data, tile_idx)
        non_zero = int(np.count_nonzero(arr))
        if non_zero == 0:
            continue
        char_label = ""
        if tile_idx in CHAR_MAP:
            char_label = "  <- " + ", ".join(f"{q}={repr(c)}" for c, q in CHAR_MAP[tile_idx])
        ascii_lines.append(f"=== TILE {tile_idx} (non-zero: {non_zero}/256){char_label} ===")
        ascii_lines.extend(tile_to_ascii_lines(tileset_data, tile_idx, colours))
        ascii_lines.append("")

    OUT_ASCII.write_text("\n".join(ascii_lines))
    print(f"Saved full ASCII dump  -> {OUT_ASCII}  ({len(ascii_lines)} lines)")

    # ------------------------------------------------------------------
    # 4. Letters-only ASCII with per-quadrant breakdown
    # ------------------------------------------------------------------
    QUAD_OFFSETS = {'TL': (0,0), 'TR': (8,0), 'BL': (0,8), 'BR': (8,8)}
    quads_order  = ['TL', 'TR', 'BL', 'BR']
    letter_lines = []

    for tile_idx in sorted(CHAR_MAP.keys()):
        chars     = CHAR_MAP[tile_idx]
        char_dict = {q: c for c, q in chars}

        letter_lines.append("=" * 62)
        letter_lines.append(
            f"TILE {tile_idx}  |  " +
            "  ".join(f"{q}={repr(char_dict.get(q,' '))}" for q in quads_order)
        )
        letter_lines.append("=" * 62)

        for y, row in enumerate(tile_to_ascii_lines(tileset_data, tile_idx, colours)):
            letter_lines.append(f"{y:2d} {row}{'  <- mid' if y == 7 else ''}")
        letter_lines.append("")

        for q in quads_order:
            c = char_dict.get(q, '?')
            ox, oy = QUAD_OFFSETS[q]
            letter_lines.append(f"  -- {q} -> {repr(c)} --")
            letter_lines.extend(f"    {l}" for l in
                                 quadrant_to_ascii(tileset_data, tile_idx, ox, oy, colours))
            letter_lines.append("")

    OUT_LETTERS_ASCII.write_text("\n".join(letter_lines))
    print(f"Saved letters ASCII    -> {OUT_LETTERS_ASCII}  ({len(letter_lines)} lines)")
    print("\nDone!")

if __name__ == "__main__":
    main()
