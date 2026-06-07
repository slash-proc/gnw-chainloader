import struct
import os
import sys

def scan_frogfs_geometry(file_path, custom_magic=None):
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' does not exist.")
        return

    with open(file_path, 'rb') as f:
        data = f.read()

    print(f"Analyzing Image: {file_path} ({len(data)} bytes)")
    print("-" * 60)

    # Common magic signature variations for custom/niche filesystems
    magics = [b'FROG', b'frogfs', b'FRG!']
    if custom_magic:
        magics.insert(0, custom_magic.encode('utf-8') if isinstance(custom_magic, str) else custom_magic)

    found = False

    for magic in magics:
        idx = 0
        while True:
            idx = data.find(magic, idx)
            if idx == -1:
                break
            
            found = True
            print(f"\nMatch found for magic '{magic.decode(errors='ignore')}' at offset 0x{idx:X}")
            
            # Print a hex dump of the surrounding 48 bytes for manual structural analysis
            start_dump = max(0, idx - 16)
            end_dump = min(len(data), idx + 32)
            context = data[start_dump:end_dump]
            
            print("  Surrounding Hex Context:")
            hex_str = context.hex(' ')
            # Highlight the magic bytes in the console output if possible, or just print cleanly
            print(f"    {hex_str}")
            
            # Speculative parsing: assuming standard 32-bit LE integers follow the magic string
            # Adjust the offset shift based on the length of the magic string
            val_offset = idx + len(magic)
            if val_offset + 12 <= len(data):
                param1, param2, param3 = struct.unpack_from('<III', data, val_offset)
                print("  Speculative little-endian 32-bit fields immediately following magic:")
                print(f"    Field 1 (hex/dec): 0x{param1:08X} / {param1}")
                print(f"    Field 2 (hex/dec): 0x{param2:08X} / {param2}")
                print(f"    Field 3 (hex/dec): 0x{param3:08X} / {param3}")
                
                # Check if a simple multiplication yields a size matching or within the image boundaries
                if param1 > 0 and param2 > 0:
                    speculative_size = param1 * param2
                    if speculative_size <= len(data) * 2:  # Allow some sanity margin
                        print(f"    Possible Geometry Match: Field 1 * Field 2 = {speculative_size} bytes")

            idx += 1

    if not found:
        print("No recognized FrogFS magic signatures identified.")
        print("First 64 bytes of the file for inspection:")
        print(f"  {data[:64].hex(' ')}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scan_frogfs.py <path_to_image> [optional_custom_magic]")
    else:
        magic_arg = sys.argv[2] if len(sys.argv) > 2 else None
        scan_frogfs_geometry(sys.argv[1], magic_arg)
