import json
import struct
import zlib
import argparse
import sys
import re
from pathlib import Path

# Import from common
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import resolve, flashio, imaging

try:
    from PIL import Image
except ImportError:
    Image = None

# --- Constants for the Binary Format ---
FLAG_ZELDA_QUAD    = 0x01
FLAG_ZELDA_VERT    = 0x02
FLAG_NES_META      = 0x04
FLAG_EXTENDED_TILE = 0x08
FLAG_RAW_PIXELS    = 0x10

BLANK_TILE = 0xFFFF

def get_used_assets(src_dir):
    """Scan source code for ASSET_ symbols to determine which assets are actually used."""
    used = set()
    pattern = re.compile(r"ASSET_[A-Z0-9_]+")
    for path in src_dir.rglob("*.[ch]"):
        if "assets_gen" in path.name:
            continue
        with open(path, "r", errors="ignore") as f:
            for match in pattern.findall(f.read()):
                used.add(match)
    return used

def reconstruct_grid(tiles):
    if not tiles:
        return 0, 0, []
    if isinstance(tiles, int):
        return 1, 1, [tiles]
    
    positions = []
    for t in tiles:
        if isinstance(t, str):
            t_id = int(t.split('.')[0])
            sub_idx = int(t.split('.')[1]) if '.' in t else 0
        else:
            t_id, sub_idx = t, 0
        tx, ty = t_id % 16, t_id // 16
        positions.append({"id": t_id, "sub": sub_idx, "x": tx, "y": ty})

    min_x = min(p["x"] for p in positions)
    max_x = max(p["x"] for p in positions)
    min_y = min(p["y"] for p in positions)
    max_y = max(p["y"] for p in positions)
    width, height = (max_x - min_x) + 1, (max_y - min_y) + 1
    width, height = min(width, 8), min(height, 8)
    
    grid = [BLANK_TILE] * (width * height)
    for p in positions:
        gx, gy = p["x"] - min_x, p["y"] - min_y
        if 0 <= gx < width and gy < height:
            grid[gy * width + gx] = (p["id"] & 0x0FFF) | (p["sub"] << 12)
    return width, height, grid

def cook_mario(data, used_symbols):
    entries = []
    # Sprites
    for section in ["multi_tile_characters_and_objects", "single_tile_characters_and_items"]:
        for name, tiles in data.get(section, {}).items():
            sym_name = f"mario_{name}"
            if f"ASSET_{sym_name.upper()}" not in used_symbols:
                continue
                
            w, h, grid = reconstruct_grid(tiles)
            entry = {"name": sym_name, "w": w, "h": h, "flags": 0, "tiles": grid}
            entries.append(entry)
    return entries

def cook_zelda(data, used_symbols):
    entries = []
    def flatten(d, prefix="zelda_"):
        for k, v in d.items():
            full_name = f"{prefix}{k}" if prefix else k
            c_name = full_name.replace("dungeon_environment_", "").replace("overworld_environment_", "").replace("vfx_and_ui_indicators_", "")
            sym_name = f"ASSET_{c_name.upper()}"
            if isinstance(v, dict):
                if "tile" in v:
                    if sym_name not in used_symbols:
                        continue
                    parts = v["tile"].split(".")
                    t_id, sub_idx = int(parts[0]), int(parts[1]) if len(parts) > 1 else 0
                    flags = FLAG_ZELDA_QUAD
                    if v.get("vertical"): flags |= FLAG_ZELDA_VERT
                    
                    entry = {"name": c_name, "w": 1, "h": 1, "flags": flags, "tiles": [(t_id & 0x0FFF) | (sub_idx << 12)]}
                    entries.append(entry)
                else: flatten(v, f"{full_name}_")
            elif isinstance(v, list):
                if sym_name not in used_symbols:
                    continue
                w, h, grid = reconstruct_grid(v)
                entries.append({"name": c_name, "w": w, "h": h, "flags": FLAG_ZELDA_QUAD, "tiles": grid})
            elif isinstance(v, int):
                if sym_name not in used_symbols:
                    continue
                entries.append({"name": c_name, "w": 1, "h": 1, "flags": FLAG_ZELDA_QUAD, "tiles": [v]})
    
    # Don't flatten clock_and_hud_text (deprecated)
    d_no_font = {k:v for k,v in data.items() if k != "clock_and_hud_text"}
    flatten(d_no_font)
    return entries

def cook_nes_mario(used_symbols):
    BASE_CHR = 0x408010
    BIG_FRAMES = ["stand", "walk1", "walk2", "walk3", "skid", "jump"]
    MARIO_TILES = [[0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07], [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F],
                   [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17], [0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F],
                   [0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27], [0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F]]
    
    entries = []
    for i, name in enumerate(BIG_FRAMES):
        full_name = f"nes_mario_big_{name}"
        if f"ASSET_{full_name.upper()}" in used_symbols:
            entries.append({"name": full_name, "w": 2, "h": 4, "flags": FLAG_NES_META | FLAG_EXTENDED_TILE, "tiles": MARIO_TILES[i], "chr_offset": BASE_CHR})
    return entries

def cook_nes_zelda(used_symbols):
    BASE_CHR = 0xA8000
    LINK_TILES = [[0, 2, 1, 3, 4, 6, 5, 7], [8, 10, 9, 11, 12, 14, 13, 15], [40, 42, 41, 43, 44, 46, 45, 47]]
    
    entries = []
    for i in range(3):
        full_name = f"nes_link_walk_{i+1}"
        if f"ASSET_{full_name.upper()}" in used_symbols:
            entries.append({"name": full_name, "w": 2, "h": 4, "flags": FLAG_NES_META | FLAG_EXTENDED_TILE, "tiles": LINK_TILES[i], "chr_offset": BASE_CHR})
    return entries

def save_verification_png(entry, output_dir):
    # Verification is only for baked raw pixels (deprecated for now)
    if not Image or "pixels" not in entry:
        return
    
    w, h = entry["w"], entry["h"]
    pixels = entry["pixels"] # 8bpp
    img = Image.frombytes("L", (w * 16, h * 16), pixels)
    
    output_dir.mkdir(parents=True, exist_ok=True)
    img.save(output_dir / f"{entry['name']}.png")

def write_assets_c(all_entries, output_c, output_h, mario_out_dir=None, zelda_out_dir=None):
    with open(output_c, "w") as fc, open(output_h, "w") as fh:
        fh.write("#ifndef ASSETS_GEN_H\n#define ASSETS_GEN_H\n\n#include <stdint.h>\n\n")
        fc.write('#include <stdint.h>\n\n')
        
        all_entries.sort(key=lambda x: x["name"])
        symbol_names = []
        for e in all_entries:
            sym = "ASSET_" + e["name"].upper().replace(".", "_").replace("-", "_")
            symbol_names.append(sym)
            
            # dims: upper 4 bits width, lower 4 bits height.
            packed_dims = ((min(e["w"], 15)) << 4) | (min(e["h"], 15))
            flags = e.get("flags", 0)
            
            # Build data array
            data = [packed_dims, flags]
            
            is_extended = any(t > 255 for t in e["tiles"])
            if is_extended: flags |= FLAG_EXTENDED_TILE
            data[1] = flags # Update flags in data
            
            data.append(len(e["tiles"]))
            if flags & FLAG_NES_META:
                data.extend(list(struct.pack("<I", e.get("chr_offset", 0))))
            for t in e["tiles"]:
                if flags & FLAG_EXTENDED_TILE: data.extend(list(struct.pack("<H", t)))
                else: data.append(t & 0xFF)
            
            fh.write(f"extern const uint8_t {sym}[];\n")
            fc.write(f"const uint8_t {sym}[] = {{ " + ", ".join(f"0x{b:02x}" for b in data) + " };\n")
            
        fc.write("\nconst uint8_t * const asset_list[] = {\n    " + ",\n    ".join(symbol_names) + "\n};\n")
        fc.write(f"const uint16_t asset_list_count = sizeof(asset_list) / sizeof(asset_list[0]);\n")
        
        fh.write(f"\nextern const uint8_t * const asset_list[];\n")
        fh.write(f"extern const uint16_t asset_list_count;\n")
        fh.write("\n#endif // ASSETS_GEN_H\n")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, default="src/chainloader")
    parser.add_argument("--mario", type=Path, default="src/chainloader/mario_tiles.json")
    parser.add_argument("--zelda", type=Path, default="src/chainloader/zelda_tiles_v3.json")
    parser.add_argument("--out-c", type=Path, default="src/chainloader/assets_gen.c")
    parser.add_argument("--out-h", type=Path, default="src/chainloader/assets_gen.h")
    args = parser.parse_args()
    
    used_symbols = get_used_assets(args.src)
    print(f"Found {len(used_symbols)} asset symbols in source code.")

    all_entries = []
    if args.mario.exists():
        with open(args.mario) as f: all_entries.extend(cook_mario(json.load(f), used_symbols))
    if args.zelda.exists():
        with open(args.zelda) as f: all_entries.extend(cook_zelda(json.load(f), used_symbols))
    all_entries.extend(cook_nes_mario(used_symbols))
    all_entries.extend(cook_nes_zelda(used_symbols))
    
    write_assets_c(all_entries, args.out_c, args.out_h)
    print(f"Generated {len(all_entries)} used assets in {args.out_c}")

if __name__ == "__main__":
    main()
