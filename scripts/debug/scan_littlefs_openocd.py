#!/usr/bin/env python3
"""
LittleFS scan utility for Game & Watch using OpenOCD.
Replicates the device's geometric scan logic to identify LittleFS partitions.
"""

import argparse
import struct
import sys
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

EXTFLASH_BASE = 0x90000000

def parse_address(x):
    """Support MB or hex address"""
    if x.lower().endswith('mb'):
        return int(x[:-2]) * 1024 * 1024
    if x.lower().endswith('mib'):
        return int(x[:-3]) * 1024 * 1024
    return int(x, 0)

def print_progress_bar(iteration, total, prefix='', suffix='', length=50, fill='█'):
    if total <= 0: return
    percent = ("{0:.1f}").format(100 * (iteration / float(total)))
    filled_length = int(length * iteration // total)
    bar = fill * filled_length + '-' * (length - filled_length)
    print(f'\r{prefix} |{bar}| {percent}% {suffix}', end='\r')
    if iteration >= total:
        print()

def check_window(backend, base_addr, end_limit, verbose=False):
    """Check 16 blocks (64KB) from base_addr"""
    found = False
    for block_idx in range(16):
        block_addr = base_addr + (block_idx * 4096)
        if block_addr + 32 > end_limit:
            break
            
        try:
            data = backend.read_memory(block_addr, 32)
            if verbose:
                print(f"  Probing 0x{block_addr:08X}: {data[:16].hex(' ')}")
                
            if data[8:16] == b'littlefs':
                version, lfs_block_size, block_count = struct.unpack_from('<III', data, 20)
                major = version >> 16
                minor = version & 0xFFFF
                
                if major == 2 and lfs_block_size >= 128 and lfs_block_size <= 8192 and block_count > 0:
                    total_size = lfs_block_size * block_count
                    
                    # Try Forward Layout
                    if block_addr + total_size <= end_limit:
                        print(f"\nFOUND Forward LittleFS v2 at 0x{block_addr:08X}")
                        print(f"  Version:     {major}.{minor}")
                        print(f"  Block Size:  {lfs_block_size} bytes")
                        print(f"  Block Count: {block_count}")
                        print(f"  Total Size:  {total_size / 1024 / 1024:.2f} MB")
                        found = True
                        break
                    
                    # Try Inverted Layout (common for G&W/gnwmanager)
                    # For block 0, end is block_addr + 4096. For block 1, end is block_addr + 8192.
                    is_at_end = (block_addr >= end_limit - (2 * lfs_block_size))
                    if is_at_end:
                        phys_start = end_limit - total_size
                        if phys_start >= EXTFLASH_BASE:
                            print(f"\nFOUND Inverted LittleFS v2 (starts at 0x{phys_start:08X}, ends at 0x{end_limit:08X})")
                            print(f"  Version:     {major}.{minor}")
                            print(f"  Block Size:  {lfs_block_size} bytes")
                            print(f"  Block Count: {block_count}")
                            print(f"  Total Size:  {total_size / 1024 / 1024:.2f} MB")
                            found = True
                            break
                    
                    print(f"\nIGNORED phantom LittleFS at 0x{block_addr:08X}: size {total_size/1024/1024:.1f}MB exceeds flash boundary.")
        except Exception as e:
            if verbose: print(f"  Error reading 0x{block_addr:08X}: {e}")
            break
    return found

def scan_littlefs(backend, start_offset, end_offset, stride, flash_size_mb):
    ext_end = EXTFLASH_BASE + (flash_size_mb * 1024 * 1024)
    start_addr = EXTFLASH_BASE + start_offset
    end_addr = min(EXTFLASH_BASE + end_offset, ext_end)
    total_bytes = end_addr - start_addr
    
    if total_bytes <= 0:
        print("Error: Invalid range.")
        return

    print(f"Scanning range: 0x{start_addr:08X} to 0x{end_addr:08X} ({total_bytes / 1024 / 1024:.2f} MB)")
    print(f"Stride: {stride / 1024 / 1024:.2f} MB")
    print("-" * 60)

    found_any = False
    current_addr = start_addr
    while current_addr < end_addr:
        progress = current_addr - start_addr
        print_progress_bar(progress, total_bytes, prefix='Progress:', suffix=f'Addr: 0x{current_addr:08X}', length=40)
        
        if check_window(backend, current_addr, ext_end, verbose=(stride <= 1048576)):
            found_any = True
            
        current_addr += stride

    # Final check: Ensure we hit the very end of the flash (common for LittleFS)
    last_window = ext_end - 65536
    if last_window >= start_addr and last_window < end_addr:
        print(f"\nPerforming final check at end of flash: 0x{last_window:08X}")
        if check_window(backend, last_window, ext_end, verbose=True):
            found_any = True

    print_progress_bar(total_bytes, total_bytes, prefix='Progress:', suffix='Complete', length=40)
    if not found_any:
        print("\nNo LittleFS partitions identified in this scan.")

def main():
    parser = argparse.ArgumentParser(description="Scan Game & Watch external flash for LittleFS superblocks via OpenOCD.")
    parser.add_argument("--start", type=parse_address, default="0", help="Start offset from base")
    parser.add_argument("--end", type=parse_address, default="64mb", help="End offset from base")
    parser.add_argument("--stride", type=parse_address, default="1mb", help="Scan stride")
    parser.add_argument("--flash-size", type=int, default=64, help="Total external flash size in MB")
    parser.add_argument("--halt", action="store_true", help="Halt the CPU before scanning")

    args = parser.parse_args()
    backend = OpenOCDBackend()
    backend.open()
    try:
        if args.halt: backend.halt()
        scan_littlefs(backend, args.start, args.end, args.stride, args.flash_size)
        if args.halt: backend.resume()
    finally:
        backend.close()

if __name__ == "__main__":
    main()
