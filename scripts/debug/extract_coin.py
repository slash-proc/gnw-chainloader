#!/usr/bin/env python3
"""
extract_coin.py

Extracts tiles 112-115 (spinning coin animation frames) from the Mario OFW
clock tileset and saves them as:
  - build/coin/coin_frame_0.png .. coin_frame_3.png  (individual frames, 16x16 scaled)
  - build/coin/coin_strip.png                         (horizontal strip of all 4 frames)
"""

import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

TILESET_BIN = Path("build/decompressed_clock_graphics.bin")
PALETTE_BIN = Path("build/mario_day_palette.bin")
OUT_DIR     = Path("build/coin")

TILE_W = 16
TILE_H = 16
TILE_B = TILE_W * TILE_H

COIN_TILES = [112, 113, 114, 115]

def load_palette():
    raw = PALETTE_BIN.read_bytes()
    p = np.frombuffer(raw, dtype=np.uint8).reshape((-1, 4))
    p = np.fliplr(p[:, :3])  # BGR -> RGB
    flat = p.flatten().tolist()
    flat += [0] * (768 - len(flat))
    return bytes(flat)

def get_tile(tileset: bytes, tile_idx: int) -> np.ndarray:
    off = tile_idx * TILE_B
    return np.frombuffer(tileset[off:off+TILE_B], dtype=np.uint8).reshape(TILE_H, TILE_W)

def render_tile(tile_arr: np.ndarray, pil_pal: bytes, scale: int = 1) -> Image.Image:
    im = Image.fromarray(tile_arr, "P")
    im.putpalette(pil_pal)
    im = im.convert("RGB")
    if scale > 1:
        im = im.resize((TILE_W * scale, TILE_H * scale), Image.NEAREST)
    return im

def main():
    for p in [TILESET_BIN, PALETTE_BIN]:
        if not p.exists():
            print(f"ERROR: {p} not found")
            sys.exit(1)

    tileset = TILESET_BIN.read_bytes()
    pil_pal = load_palette()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    SCALE = 8  # 16x16 -> 128x128 per frame

    frames = []
    for i, tile_idx in enumerate(COIN_TILES):
        arr = get_tile(tileset, tile_idx)
        img = render_tile(arr, pil_pal, scale=SCALE)
        out = OUT_DIR / f"coin_frame_{i}.png"
        img.save(out)
        frames.append(img)
        print(f"Frame {i} (tile {tile_idx}) -> {out}")

    # Horizontal strip with labels
    PAD    = 4
    LABEL  = 14
    strip_w = len(frames) * (TILE_W * SCALE + PAD) + PAD
    strip_h = TILE_H * SCALE + LABEL + PAD * 2
    strip = Image.new("RGB", (strip_w, strip_h), (15, 15, 30))
    draw  = ImageDraw.Draw(strip)

    for i, (img, tile_idx) in enumerate(zip(frames, COIN_TILES)):
        x = PAD + i * (TILE_W * SCALE + PAD)
        y = PAD
        strip.paste(img, (x, y))
        draw.text((x + 2, y + TILE_H * SCALE + 2),
                  f"frame {i}  tile {tile_idx}", fill=(255, 220, 50))

    strip_path = OUT_DIR / "coin_strip.png"
    strip.save(strip_path)
    print(f"Saved strip -> {strip_path}  ({strip_w}x{strip_h}px)")
    print("\nDone! Note these as COIN_TILES = [112, 113, 114, 115] for the menu cursor.")

if __name__ == "__main__":
    main()
