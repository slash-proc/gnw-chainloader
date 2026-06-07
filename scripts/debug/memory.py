#!/usr/bin/env python3
"""
Memory operations and search utility for the Game & Watch (STM32H7B0).
Supports reading, writing, dumping, comparing, and searching target memory.
"""

import argparse
import struct
import sys
import re
from pathlib import Path
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

def parse_address(x):
    """Support basic hex math like 0x0811AE70+0x301A5"""
    try:
        # Replace common math operators with space-separated ones for simpler eval
        expr = x.replace("+", " + ").replace("-", " - ")
        # Tokenize and convert hex to int
        tokens = expr.split()
        final_expr = ""
        for t in tokens:
            if t in ["+", "-"]:
                final_expr += t
            else:
                final_expr += str(int(t, 0))
        return eval(final_expr)
    except Exception:
        raise argparse.ArgumentTypeError(f"Invalid address expression: {x}")

def do_read(backend, address, size, count):
    if size == "bytes":
        data = backend.read_memory(address, count)
        print(f"0x{address:08X}: {data.hex(' ')}")
    else:
        word_size = {8: 1, 16: 2, 32: 4}[size]
        total_len = word_size * count
        data = backend.read_memory(address, total_len)
        
        fmt = {8: "B", 16: "H", 32: "I"}[size]
        unpacked = struct.unpack(f"<{count}{fmt}", data)
        
        for i, val in enumerate(unpacked):
            addr = address + i * word_size
            hex_len = word_size * 2
            print(f"0x{addr:08X}: 0x{val:0{hex_len}X} ({val})")


def do_write(backend, address, size, value_str):
    if size == "bytes":
        try:
            data = bytes.fromhex(value_str.replace(" ", "").replace("0x", ""))
        except ValueError:
            print("Error: Invalid hex bytes string.")
            return
        backend.write_memory(address, data)
        print(f"Wrote {len(data)} bytes to 0x{address:08X}.")
    else:
        val = int(value_str, 0)
        word_size = {8: 1, 16: 2, 32: 4}[size]
        fmt = {8: "B", 16: "H", 32: "I"}[size]
        packed = struct.pack(f"<{fmt}", val)
        backend.write_memory(address, packed)
        hex_len = word_size * 2
        print(f"Wrote 0x{val:0{hex_len}X} ({size}-bit) to 0x{address:08X}.")


def do_dump(backend, address, length, file_path):
    print(f"Dumping {length} bytes from 0x{address:08X} to {file_path}...")
    try:
        # Read in blocks of 64KB to avoid OpenOCD timeout issues
        block_size = 65536
        data = bytearray()
        remaining = length
        curr_addr = address
        
        while remaining > 0:
            chunk_size = min(remaining, block_size)
            print(f"  Reading {chunk_size} bytes at 0x{curr_addr:08X}...")
            data.extend(backend.read_memory(curr_addr, chunk_size))
            curr_addr += chunk_size
            remaining -= chunk_size
            
        Path(file_path).write_bytes(data)
        print("Dump complete.")
    except Exception as e:
        print(f"Error dumping memory: {e}")


def do_compare(backend, address, file_path, length):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: Local file '{file_path}' not found.")
        return
        
    file_data = path.read_bytes()
    if length is not None:
        file_data = file_data[:length]
    else:
        length = len(file_data)
        
    print(f"Comparing 0x{address:08X} on target to '{file_path}' ({length} bytes)...")
    
    # Read device memory
    try:
        # Read in blocks of 64KB
        block_size = 65536
        dev_data = bytearray()
        remaining = length
        curr_addr = address
        
        while remaining > 0:
            chunk_size = min(remaining, block_size)
            dev_data.extend(backend.read_memory(curr_addr, chunk_size))
            curr_addr += chunk_size
            remaining -= chunk_size
    except Exception as e:
        print(f"Error reading target memory: {e}")
        return
        
    # Perform byte comparison
    diffs = []
    for i in range(length):
        if dev_data[i] != file_data[i]:
            diffs.append((i, dev_data[i], file_data[i]))
            
    if not diffs:
        print(">>> SUCCESS: Device memory matches local file perfectly!")
    else:
        print(f">>> FAILED: Found {len(diffs)} byte differences!")
        print("First 20 differences:")
        for idx, dev_val, file_val in diffs[:20]:
            offset_addr = address + idx
            print(f"  0x{offset_addr:08X} (offset {hex(idx)}): Target=0x{dev_val:02X} File=0x{file_val:02X}")


def do_search(backend, start_addr, end_addr, pattern_str, search_ospi):
    # Align addresses to 4 bytes
    start = start_addr & ~3
    end = end_addr & ~3
    length = end - start
    
    if length <= 0:
        print("Error: Invalid search range.")
        return
        
    print(f"Searching memory range 0x{start:08X} to 0x{end:08X} ({length} bytes)...")
    
    # Read memory in blocks
    block_size = 65536
    curr_addr = start
    matches = []
    
    # Parse search pattern
    if search_ospi:
        print("Searching for external flash pointers (0x90000000 - 0x94000000)...")
    else:
        try:
            # check if it's a number/hex value or byte pattern
            if pattern_str.startswith("0x") or pattern_str.isdigit():
                pattern_val = int(pattern_str, 0)
                pattern_bytes = struct.pack("<I", pattern_val)
            else:
                pattern_bytes = bytes.fromhex(pattern_str.replace(" ", ""))
        except ValueError:
            print("Error: Invalid search pattern hex string.")
            return
        print(f"Searching for byte pattern: {pattern_bytes.hex(' ')}")
        
    try:
        while curr_addr < end:
            chunk_size = min(end - curr_addr, block_size)
            chunk_data = backend.read_memory(curr_addr, chunk_size)
            
            if search_ospi:
                # Scan for any 32-bit aligned words in the range [0x90000000, 0x94000000]
                for offset in range(0, chunk_size - 3, 4):
                    val = struct.unpack("<I", chunk_data[offset:offset+4])[0]
                    if 0x90000000 <= val < 0x94000000:
                        matches.append((curr_addr + offset, val))
            else:
                # Standard byte search
                offset = 0
                while True:
                    idx = chunk_data.find(pattern_bytes, offset)
                    if idx == -1:
                        break
                    matches.append((curr_addr + idx, pattern_bytes))
                    offset = idx + 1
                    
            curr_addr += chunk_size
    except Exception as e:
        print(f"Error during search: {e}")
        return
        
    if not matches:
        print("No matches found.")
    else:
        print(f"Found {len(matches)} matches:")
        for addr, val in matches[:50]:  # Limit output to 50
            if search_ospi:
                print(f"  0x{addr:08X}: 0x{val:08X} (OSPI Pointer)")
            else:
                print(f"  0x{addr:08X}: match")
        if len(matches) > 50:
            print("... and more matches (capped at 50).")


def main():
    parser = argparse.ArgumentParser(
        description="Memory operations and search utility for the Game & Watch (STM32H7B0)."
    )
    parser.add_argument("--no-halt", action="store_true", help="Do not halt the target CPU before operations")
    subparsers = parser.add_subparsers(dest="command", required=True, help="Subcommand to execute")

    # Read
    read_parser = subparsers.add_parser("read", help="Read memory at address")
    read_parser.add_argument("address", type=parse_address, help="Hex address (supports math like 0x100+0x10)")
    read_parser.add_argument("size", choices=[8, 16, 32, "bytes"], type=lambda x: int(x) if x.isdigit() else x, help="Access size in bits, or raw bytes")
    read_parser.add_argument("count", type=int, nargs="?", default=1, help="Number of elements to read")

    # Write
    write_parser = subparsers.add_parser("write", help="Write memory at address")
    write_parser.add_argument("address", type=parse_address, help="Hex address (supports math)")
    write_parser.add_argument("size", choices=[8, 16, 32, "bytes"], type=lambda x: int(x) if x.isdigit() else x, help="Access size in bits, or raw bytes")
    write_parser.add_argument("value", help="Hex value to write (or hex bytes string for raw bytes size)")

    # Dump
    dump_parser = subparsers.add_parser("dump", help="Dump memory region to file")
    dump_parser.add_argument("address", type=parse_address, help="Start address")
    dump_parser.add_argument("length", type=lambda x: int(x, 0), help="Number of bytes to dump (decimal or 0x hex)")
    dump_parser.add_argument("file", help="Local output file path")

    # Compare
    comp_parser = subparsers.add_parser("compare", help="Compare local file to device memory")
    comp_parser.add_argument("address", type=parse_address, help="Start address on target")
    comp_parser.add_argument("file", help="Local file to compare against")
    comp_parser.add_argument("--length", type=lambda x: int(x, 0), help="Max bytes to compare (decimal or 0x hex)")

    # Search
    search_parser = subparsers.add_parser("search", help="Search for byte patterns or pointers")
    search_parser.add_argument("start", type=parse_address, help="Start address")
    search_parser.add_argument("end", type=parse_address, help="End address")
    search_parser.add_argument("pattern", nargs="?", help="Hex pattern to search for (e.g., 'AABB' or '0x12345678')")
    search_parser.add_argument("--ospi", action="store_true", help="Search specifically for external flash pointers")

    args = parser.parse_args()

    backend = OpenOCDBackend()
    backend.open()
    try:
        if not args.no_halt:
            backend.halt()
            
        if args.command == "read":
            do_read(backend, args.address, args.size, args.count)
        elif args.command == "write":
            do_write(backend, args.address, args.size, args.value)
        elif args.command == "dump":
            do_dump(backend, args.address, args.length, args.file)
        elif args.command == "compare":
            do_compare(backend, args.address, args.file, args.length)
        elif args.command == "search":
            do_search(backend, args.start, args.end, args.pattern, args.ospi)
            
        if not args.no_halt:
            backend.resume()
    finally:
        backend.close()


if __name__ == "__main__":
    main()
