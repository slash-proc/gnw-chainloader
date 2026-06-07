#!/usr/bin/env python3

import os
import sys
from littlefs import LittleFS

def extract_littlefs_image(image_path, output_dir="build/littlefs"):
    BLOCK_SIZE = 4096
    BLOCK_COUNT = 1024  # 4MB flash layout dimensions
    
    if not os.path.exists(image_path):
        print(f"Error: Image '{image_path}' not found.")
        return

    print(f"Reading and reversing blocks from {image_path}...")
    with open(image_path, "rb") as f:
        raw_data = f.read()

    # Reverse block sequence to counter flash block inversion layout
    blocks = [raw_data[i:i+BLOCK_SIZE] for i in range(0, len(raw_data), BLOCK_SIZE)]
    corrected_data = b"".join(blocks[::-1])

    # Mount virtual block instance
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)
    fs.context.buffer = bytearray(corrected_data)

    try:
        fs.mount()
        print("LittleFS volume successfully mounted.")
    except Exception as e:
        print(f"Mount failed: {e}")
        return

    print(f"\nExtracting assets to: {output_dir}/")
    print("-" * 60)

    def traverse_and_extract(path="/"):
        try:
            for entry in fs.scandir(path):
                if entry.name in (".", ".."):
                    continue
                
                # Format internal filesystem path
                virtual_path = os.path.join(path, entry.name).replace("\\", "/")
                
                # Construct host destination local file path
                # lstrip removes leading slashes so os.path.join works safely relative to target
                local_dest_path = os.path.join(output_dir, virtual_path.lstrip("/"))
                
                if entry.type == 1:  # Regular File
                    print(f"  Extracting: {virtual_path} ({entry.size} bytes)")
                    
                    # Ensure host operating system directory exists for target file
                    os.makedirs(os.path.dirname(local_dest_path), exist_ok=True)
                    
                    # Read sequence out of the LittleFS handle
                    with fs.open(virtual_path, "rb") as virtual_file:
                        file_data = virtual_file.read()
                        
                    # Write target block payload to host system disk
                    with open(local_dest_path, "wb") as local_file:
                        local_file.write(file_data)
                        
                elif entry.type == 2:  # Directory
                    # Dynamically generate directories on local disk ahead of internal files
                    os.makedirs(local_dest_path, exist_ok=True)
                    traverse_and_extract(virtual_path)
                    
        except Exception as e:
            print(f"Extraction exception at path '{path}': {e}")

    traverse_and_extract()
    print("-" * 60)
    print("Extraction run complete.")

if __name__ == "__main__":
    # You can supply an alternative image as an execution arg if needed
    target_image = sys.argv[1] if len(sys.argv) > 1 else "retro-go-sd/build/littlefs.bin"
    extract_littlefs_image(target_image)
