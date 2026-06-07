
import argparse
import struct
import sys

def parse_lfs(file_path):
    try:
        with open(file_path, 'rb') as f:
            data = f.read()
    except IOError as e:
        print(f"Error opening file: {e}")
        return

    magic = b'littlefs'
    pos = data.find(magic)
    
    if pos == -1:
        print("Error: 'littlefs' signature not found in file.")
        return

    print(f"Found 'littlefs' magic string at offset: {hex(pos)}")

    # Superblock struct (lfs_superblock_t) is 24 bytes (6 * uint32_t)
    # The structure follows the magic string immediately (based on lfs.c)
    superblock_bytes = data[pos+8 : pos+8+24]
    
    if len(superblock_bytes) < 24:
        print("Error: Could not read 24 bytes of superblock metadata.")
        return

    # Unpack LittleFS superblock: version, block_size, block_count, name_max, file_max, attr_max
    # All are uint32_t (lfs_size_t).
    s = struct.unpack('<IIIIII', superblock_bytes)
    
    print("LittleFS Superblock Metadata:")
    print(f"  Version:     {hex(s[0])}")
    print(f"  Block Size:  {s[1]} bytes")
    print(f"  Block Count: {s[2]} blocks")
    print(f"  Name Max:    {s[3]} bytes")
    print(f"  File Max:    {s[4]} bytes")
    print(f"  Attr Max:    {s[5]} bytes")
    
    total_size = s[1] * s[2]
    print(f"Calculated Filesystem Size: {total_size} bytes ({total_size / 1024 / 1024:.2f} MB)")

def main():
    parser = argparse.ArgumentParser(description="Parse LittleFS superblock from a filesystem binary.")
    parser.add_argument("file_path", help="Path to the LittleFS binary file.")
    args = parser.parse_args()
    
    parse_lfs(args.file_path)

if __name__ == "__main__":
    main()
