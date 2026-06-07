#!/usr/bin/env python3
import sys
import zlib
from pathlib import Path

# Import the vendored tileset module from scripts/common.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    from common.tileset import bytes_to_tilemap
    print("Successfully imported bytes_to_tilemap from common.tileset!")
except ImportError as e:
    print(f"Error importing from common.tileset: {e}")
    # Fallback definition if import fails
    def bytes_to_tilemap(data, palette=None, **kwargs):
        print("Using fallback dummy tileset converter...")
        return None

def main():
    bin_path = Path("backup/decrypt_flash_patched.bin")
    if not bin_path.exists():
        print(f"Error: {bin_path} not found.")
        return
    
    data = bin_path.read_bytes()
    print(f"Loaded {len(data)} bytes from {bin_path}")
    
    # Extract palette
    # Day palette: 0xBEC68, 320 bytes (80 colors * 4 bytes BGRA)
    pal_offset = 0xBEC68
    palette = data[pal_offset : pal_offset + 320]
    
    # Scan for zlib block decompressing to 65536 bytes
    decompressed_tiles = None
    found_offset = None
    
    print("Scanning for compressed clock graphics...")
    for offset in range(len(data)):
        if data[offset] == 0x78:
            try:
                dec = zlib.decompress(data[offset:])
                if len(dec) == 65536:
                    print(f"Found zlib compressed block at offset {hex(offset)} (size {len(dec)} bytes)")
                    decompressed_tiles = dec
                    found_offset = offset
                    break
            except Exception:
                pass
                
        try:
            dec = zlib.decompress(data[offset:offset+30000], -zlib.MAX_WBITS)
            if len(dec) == 65536:
                print(f"Found raw deflate compressed block at offset {hex(offset)} (size {len(dec)} bytes)")
                decompressed_tiles = dec
                found_offset = offset
                break
        except Exception:
            pass

    if decompressed_tiles is None:
        print("Could not find compressed clock graphics block. Using raw fallback at 0x98B84...")
        decompressed_tiles = data[0x98B84 : 0x98B84 + 0x10000]
        found_offset = 0x98B84
    
    # Save the decompressed bin
    build_dir = Path("build")
    build_dir.mkdir(exist_ok=True)
    Path("build/decompressed_clock_graphics.bin").write_bytes(decompressed_tiles)
    
    # Render and save tileset
    try:
        img = bytes_to_tilemap(decompressed_tiles, palette=palette)
        if img:
            img.save(build_dir / "tileset.png")
            print(f"Successfully saved rendered tileset to {build_dir / 'tileset.png'}")
            
            # Also save index image (grayscale)
            img_index = bytes_to_tilemap(decompressed_tiles)
            img_index.save(build_dir / "tileset_index.png")
            print(f"Successfully saved tileset index image to {build_dir / 'tileset_index.png'}")
    except Exception as e:
        print(f"Error rendering tileset: {e}")

if __name__ == "__main__":
    main()
