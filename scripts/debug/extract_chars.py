#!/usr/bin/env python3
"""
extract_chars.py

Splits each letter/digit 16x16 tile into its 4 individual 8x8 character glyphs
(TL, TR, BL, BR quadrants) and produces:
  - build/chars/            : individual PNGs per character (e.g. 'A.png', '1.png')
  - build/chars_sheet.png   : combined sprite sheet of all extracted characters
  - build/chars_ascii.txt   : ASCII art of each individual 8x8 character
"""

import sys
from pathlib import Path
from math import ceil

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    print("ERROR: numpy and Pillow required.  pip install numpy Pillow")
    sys.exit(1)

TILESET_BIN = Path("build/decompressed_clock_graphics.bin")
PALETTE_BIN = Path("build/mario_day_palette.bin")
CHARS_DIR   = Path("build/chars")
OUT_SHEET   = Path("build/chars_sheet.png")
OUT_ASCII   = Path("build/chars_ascii.txt")

TILE_W  = 16
TILE_H  = 16
TILE_B  = TILE_W * TILE_H  # 256 bytes

# Quadrant definitions: (name, x-offset, y-offset)
QUADRANTS = [
    ("TL", 0, 0),
    ("TR", 8, 0),
    ("BL", 0, 8),
    ("BR", 8, 8),
]

# Tile index -> [(char, quadrant), ...]
CHAR_MAP = {
    96:  [("A","TL"), ("B","TR"), ("1","BL"), ("C","BR")],
    97:  [("C","TL"), ("D","TR"), ("2","BL"), ("3","BR")],
    98:  [("E","TL"), ("F","TR"), ("4","BL"), ("5","BR")],
    99:  [("G","TL"), ("H","TR"), ("6","BL"), ("7","BR")],
    100: [("I","TL"), ("J","TR"), ("8","BL"), ("9","BR")],
    101: [("K","TL"), ("L","TR"), (".","BL")            ],  # BR is blank
    102: [("M","TL"), ("N","TR"), ("!","BL"), ("?","BR")],
    103: [("O","TL"), ("P","TR"), ("0","BL"), ("X","BR")],
    104: [("Q","TL"), ("R","TR"), ("-","BL"), (":","BR")],
    105: [("S","TL"), ("T","TR")                        ],  # BL/BR blank
    106: [("U","TL"), ("V","TR")                        ],
    107: [("W","TL"), ("X","TR")                        ],
    108: [("Y","TL"), ("Z","TR")                        ],
}

DENSITY = " .:;+*#@"

def load_palette():
    raw = PALETTE_BIN.read_bytes()
    p = np.frombuffer(raw, dtype=np.uint8).reshape((-1, 4))
    p = np.fliplr(p[:, :3])          # BGR -> RGB, matches tileset.py
    flat = p.flatten().tolist()
    flat += [0] * (768 - len(flat))
    return bytes(flat)

def get_tile(tileset: bytes, tile_idx: int) -> np.ndarray:
    off = tile_idx * TILE_B
    return np.frombuffer(tileset[off:off+TILE_B], dtype=np.uint8).reshape(TILE_H, TILE_W)

def render_quad(tile_arr: np.ndarray, ox: int, oy: int, pil_pal: bytes, scale: int = 1) -> Image.Image:
    quad = tile_arr[oy:oy+8, ox:ox+8].copy()
    im = Image.fromarray(quad, "P")
    im.putpalette(pil_pal)
    im = im.convert("RGB")
    if scale > 1:
        im = im.resize((8 * scale, 8 * scale), Image.NEAREST)
    return im

def quad_to_ascii(tile_arr: np.ndarray, ox: int, oy: int, colours: list) -> list:
    lines = []
    for y in range(8):
        row = ""
        for x in range(8):
            idx = int(tile_arr[oy+y, ox+x])
            if idx == 0:
                row += " "
            else:
                r, g, b = colours[idx % len(colours)]
                lum = 0.299*r + 0.587*g + 0.114*b
                row += DENSITY[int(lum / 255 * (len(DENSITY)-1))]
        lines.append(row)
    return lines

def palette_colours(pil_pal: bytes) -> list:
    return [(pil_pal[i*3], pil_pal[i*3+1], pil_pal[i*3+2]) for i in range(256)]

def char_to_safe_filename(c: str) -> str:
    names = {
        '.': 'dot', '-': 'dash', '!': 'excl', '?': 'quest',
        ':': 'colon', '0': 'd0', '1': 'd1', '2': 'd2', '3': 'd3',
        '4': 'd4', '5': 'd5', '6': 'd6', '7': 'd7', '8': 'd8', '9': 'd9',
    }
    return names.get(c, c)

def main():
    for p in [TILESET_BIN, PALETTE_BIN]:
        if not p.exists():
            print(f"ERROR: {p} not found")
            sys.exit(1)

    tileset = TILESET_BIN.read_bytes()
    pil_pal = load_palette()
    colours = palette_colours(pil_pal)

    CHARS_DIR.mkdir(parents=True, exist_ok=True)

    # Build ordered list of (char, tile_idx, ox, oy)
    quad_map = {q: (ox, oy) for q, ox, oy in QUADRANTS}
    all_chars = []   # [(char, tile_idx, ox, oy)]
    for tile_idx in sorted(CHAR_MAP.keys()):
        for char, quad in CHAR_MAP[tile_idx]:
            ox, oy = quad_map[quad]
            all_chars.append((char, tile_idx, ox, oy))

    print(f"Extracting {len(all_chars)} characters...")

    # ------------------------------------------------------------------
    # 1. Individual character PNGs (8x8 scaled up 8x = 64x64)
    # ------------------------------------------------------------------
    SCALE = 8
    for char, tile_idx, ox, oy in all_chars:
        tile_arr = get_tile(tileset, tile_idx)
        img = render_quad(tile_arr, ox, oy, pil_pal, scale=SCALE)
        fname = CHARS_DIR / f"{char_to_safe_filename(char)}.png"
        img.save(fname)

    print(f"Saved {len(all_chars)} individual PNGs -> {CHARS_DIR}/")

    # ------------------------------------------------------------------
    # 2. Combined character sheet
    # ------------------------------------------------------------------
    SHEET_COLS = 10
    SHEET_ROWS = ceil(len(all_chars) / SHEET_COLS)
    CSCALE = 6
    CHAR_W = 8 * CSCALE
    CHAR_H = 8 * CSCALE
    LABEL_H = 12
    PAD = 2

    sheet_w = SHEET_COLS * (CHAR_W + PAD) + PAD
    sheet_h = SHEET_ROWS * (CHAR_H + LABEL_H + PAD) + PAD
    sheet = Image.new("RGB", (sheet_w, sheet_h), (15, 15, 30))
    draw = ImageDraw.Draw(sheet)

    for i, (char, tile_idx, ox, oy) in enumerate(all_chars):
        col = i % SHEET_COLS
        row = i // SHEET_COLS
        x = PAD + col * (CHAR_W + PAD)
        y = PAD + row * (CHAR_H + LABEL_H + PAD)

        tile_arr = get_tile(tileset, tile_idx)
        img = render_quad(tile_arr, ox, oy, pil_pal, scale=CSCALE)
        sheet.paste(img, (x, y))

        label = repr(char) if char not in (' ',) else "'spc'"
        draw.text((x + 1, y + CHAR_H + 1), label, fill=(255, 220, 50))

    sheet.save(OUT_SHEET)
    print(f"Saved character sheet -> {OUT_SHEET}  ({sheet_w}x{sheet_h}px)")

    # ------------------------------------------------------------------
    # 3. ASCII art dump of every individual character
    # ------------------------------------------------------------------
    ascii_lines = []
    for char, tile_idx, ox, oy in all_chars:
        ascii_lines.append(f"=== '{char}'  (tile {tile_idx}, ox={ox}, oy={oy}) ===")
        ascii_lines.extend(f"  {l}" for l in
                           quad_to_ascii(get_tile(tileset, tile_idx), ox, oy, colours))
        ascii_lines.append("")

    OUT_ASCII.write_text("\n".join(ascii_lines))
    print(f"Saved ASCII dump       -> {OUT_ASCII}  ({len(ascii_lines)} lines)")
    print("\nDone!")

if __name__ == "__main__":
    main()
