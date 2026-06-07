#!/usr/bin/env python3

import os
from littlefs import LittleFS

def walk_and_read_littlefs(image_path):
    BLOCK_SIZE = 4096
    BLOCK_COUNT = 1024  # Total size 4MB

    if not os.path.exists(image_path):
        print(f"Error: {image_path} not found.")
        return

    with open(image_path, "rb") as f:
        raw_data = f.read()

    # 1. Reverse the physical block sequence to fix the layout inversion
    blocks = [raw_data[i:i+BLOCK_SIZE] for i in range(0, len(raw_data), BLOCK_SIZE)]
    corrected_data = b"".join(blocks[::-1])

    # 2. Instantiate and fill virtual block device memory
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)
    fs.context.buffer = bytearray(corrected_data)

    try:
        fs.mount()
    except Exception as e:
        print(f"Mount failed: {e}")
        return

    print(f"{'File Path':<40} {'Size (Bytes)':<12}")
    print("-" * 55)

    # 3. Recursive directory directory structure walker using .scandir()
    def traverse(path="/"):
        try:
            # .scandir() replaces the non-existent .ilistdir()
            for entry in fs.scandir(path):
                if entry.name in (".", ".."):
                    continue

                # Standardize forward slashes for internal tracking
                full_path = os.path.join(path, entry.name).replace("\\", "/")

                if entry.type == 1:  # Type 1: LFS_TYPE_REG (Regular File)
                    print(f"{full_path:<40} {entry.size:<12}")

                    # Direct binary file reading out of the mount context
                    with fs.open(full_path, "rb") as file_obj:
                        content = file_obj.read()
                        #print(f"   Raw Snippet: {content[:32]}")

                elif entry.type == 2:  # Type 2: LFS_TYPE_DIR (Directory)
                    traverse(full_path)
        except Exception as e:
            print(f"Error scanning path {path}: {e}")

    traverse()

if __name__ == "__main__":
    walk_and_read_littlefs("retro-go-sd/build/littlefs.bin")
