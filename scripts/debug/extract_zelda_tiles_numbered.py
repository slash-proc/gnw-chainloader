#!/usr/bin/env python3
import sys
from pathlib import Path

# Insert the parent directory to allow importing common scripts
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from common import flashio, nesgfx, palette as palmod, imaging

def main():
    print("Loading Zelda patched external flash...")
    try:
        data = flashio.load_patched_external("zelda")
    except Exception as e:
        print(f"Error loading external flash: {e}")
        return

    print("Loading Zelda G&W palette...")
    palette = palmod.load_gnw_palette(data, "zelda")

    print("Extracting 256 Zelda tiles (quadrant layout)...")
    offset = flashio.info("zelda")["gnw_tileset_offset"]
    
    tiles_rgb = []
    for idx in range(256):
        tile_grid = nesgfx.read_gnw_tile(data, offset, idx, tile_px=16, layout="quadrant")
        tile_rgb_grid = palmod.apply(tile_grid, palette, missing=(0, 0, 0))
        tiles_rgb.append(tile_rgb_grid)

    print("Rendering labeled grid...")
    # Render with 16 columns, scaled to 4x, pad=12 pixels for readable index numbers
    img = imaging.labeled_grid(
        tiles_rgb,
        cols=16,
        tile_px=16,
        scale=4,
        pad=12,
        label=True,
        text=(255, 255, 0),       # Bright yellow text
        border=(120, 120, 120),    # Grey tile borders
        bg=(15, 30, 20)            # Dark green/grey background
    )

    out_path = Path("build/zelda_tileset_numbered.png")
    img.save(out_path)
    print(f"Success! Zelda numbered tileset saved to {out_path.resolve()}")

if __name__ == "__main__":
    main()
