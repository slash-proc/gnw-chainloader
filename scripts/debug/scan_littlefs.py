import struct
import os
import sys

def parse_littlefs_geometry(file_path):
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' does not exist.")
        return

    with open(file_path, 'rb') as f:
        data = f.read()

    print(f"Analyzing Image: {file_path} ({len(data)} bytes)")
    print("-" * 60)

    idx = 0
    found = False
    
    while True:
        idx = data.find(b'littlefs', idx)
        if idx == -1:
            break
            
        # --- LittleFS v2 Parsing ---
        # The fields follow at fixed offsets relative to the magic string:
        # Magic + 12: Version (32-bit LE)
        # Magic + 16: Block Size (32-bit LE)
        # Magic + 20: Block Count (32-bit LE)
        if idx + 24 <= len(data):
            version, block_size, block_count = struct.unpack_from('<III', data, idx + 12)
            major = version >> 16
            minor = version & 0xFFFF
            
            # Validation check to ensure it's a legitimate superblock and not random text
            if major == 2 and block_size in [128, 256, 512, 1024, 2048, 4096, 8192] and block_count > 0:
                total_size = block_size * block_count
                print(f"Found Valid LittleFS v2 Superblock Signature!")
                print(f"  Magic String File Offset: 0x{idx:X}")
                print(f"  Filesystem Version:       {major}.{minor}")
                print(f"  Configured Block Size:    {block_size} bytes")
                print(f"  Configured Block Count:   {block_count}")
                print(f"  Computed Total Length:    {total_size} bytes ({total_size / 1024 / 1024:.2f} MiB)")
                found = True

        # --- LittleFS v1 Parsing Fallback ---
        # In v1, fields precede the magic string:
        # Magic - 12: Block Size (32-bit LE)
        # Magic - 8:  Block Count (32-bit LE)
        # Magic - 4:  Version (32-bit LE)
        if idx >= 12:
            block_size, block_count, version = struct.unpack_from('<III', data, idx - 12)
            major = version >> 16
            minor = version & 0xFFFF
            
            if major == 1 and block_size in [128, 256, 512, 1024, 2048, 4096, 8192] and block_count > 0:
                total_size = block_size * block_count
                print(f"Found Valid LittleFS v1 Superblock Signature!")
                print(f"  Magic String File Offset: 0x{idx:X}")
                print(f"  Filesystem Version:       {major}.{minor}")
                print(f"  Configured Block Size:    {block_size} bytes")
                print(f"  Configured Block Count:   {block_count}")
                print(f"  Computed Total Length:    {total_size} bytes ({total_size / 1024 / 1024:.2f} MiB)")
                found = True

        idx += 1

    if not found:
        print("Error: No valid LittleFS superblock geometry could be identified.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scan_littlefs.py <path_to_littlefs_bin>")
    else:
        parse_littlefs_geometry(sys.argv[1])
