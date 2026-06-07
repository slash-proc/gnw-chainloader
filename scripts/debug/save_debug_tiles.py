#!/usr/bin/env python3
from pathlib import Path
import sys

# We want to use PIL to save individual tiles as PNGs to see which one is the brick
try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("PIL and numpy are required")
    sys.exit(1)

def main():
    bin_path = Path("build/decompressed_clock_graphics.bin")
    pal_path = Path("build/mario_day_palette.bin")
    
    if not bin_path.exists() or not pal_path.exists():
        print("Missing graphics or palette file")
        return
        
    data = bin_path.read_bytes()
    pal_bytes = pal_path.read_bytes()
    
    # Load palette
    p = np.frombuffer(pal_bytes, dtype=np.uint8).reshape((-1, 4))
    p = p[:, :3]
    p = np.fliplr(p) # BGR -> RGB
    
    # Create debug directory
    out_dir = Path("build/debug_tiles")
    out_dir.mkdir(exist_ok=True)
    
    # Let's save a list of potential brick tile candidates
    # In SMB1, standard brick is tile 142? Or is it tile 116? Let's check a range of tiles:
    # 0-50, 110-150.
    candidates = list(range(0, 40)) + list(range(110, 150))
    
    for tile_idx in candidates:
        tile_offset = tile_idx * 256
        if tile_offset + 256 > len(data):
            break
        tile_data = data[tile_offset : tile_offset + 256]
        
        # Check if empty
        if all(b == 0 for b in tile_data):
            continue
            
        # Reshape to 16x16
        arr = np.frombuffer(tile_data, dtype=np.uint8).reshape((16, 16))
        
        img = Image.fromarray(arr, "P")
        img.putpalette(p.flatten().tolist() + [0]*512)
        
        # Scale for visibility
        img_scaled = img.resize((32, 32), Image.NEAREST)
        img_scaled.save(out_dir / f"tile_{tile_idx:03d}.png")
        
    print(f"Saved candidate tiles to {out_dir}")

if __name__ == "__main__":
    main()
