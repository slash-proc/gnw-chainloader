#!/usr/bin/env python3
"""
Host-side binary inspection utility for Game & Watch (STM32H7B0) binaries.
Provides metadata decoding, rwdata table parsing, OTFDEC extraction,
literal reference checking, padding analysis, and binary diffing.
"""

import argparse
import struct
import sys
from pathlib import Path


def unpack_metadata(value):
    external_flash_size = (value & 0xFFFFFF) << 12
    flags = (value >> 24) & 0xFF
    magic = flags & 0x0F
    is_mario = bool(flags & (1 << 4))
    is_zelda = bool(flags & (1 << 5))
    return magic, external_flash_size, is_mario, is_zelda


def do_metadata(file_path):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    data = path.read_bytes()
    print(f"Inspecting metadata in '{file_path}' ({len(data)} bytes):")

    if len(data) < 0x1BC:
        print("  Error: File is too small to contain HeaderMetadata at 0x1B8.")
        sys.exit(1)

    val_32 = struct.unpack("<I", data[0x1B8:0x1BC])[0]
    print(f"  Raw 32-bit value at 0x1B8: 0x{val_32:08X}")

    try:
        magic, flash_size, is_mario, is_zelda = unpack_metadata(val_32)
        print(f"  Decoded Metadata:")
        print(f"    Magic:               {magic} (Expected: 4)")
        print(f"    External Flash Size: {flash_size} bytes ({flash_size / (1024*1024):.1f} MB)")
        print(f"    Is Mario:            {is_mario}")
        print(f"    Is Zelda:            {is_zelda}")
        if magic != 4:
            print("    WARNING: Magic value does not match expected G&W patch magic (4).")
    except Exception as e:
        print(f"  Error decoding metadata: {e}")


def do_rwdata(file_path, table_offset, table_len):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    data = path.read_bytes()
    table_data = data[table_offset : table_offset + table_len]
    
    if len(table_data) < table_len:
        print(f"Error: Binary size ({len(data)}) is too small for table at {hex(table_offset)} (len {table_len}).")
        sys.exit(1)

    print(f"Parsing rwdata table in '{file_path}' at offset {hex(table_offset)} (len {table_len} bytes):")
    
    # Each block is 16 bytes:
    # 0: fn_offset (4 bytes signed)
    # 4: data_offset relative to block start + 4 (4 bytes signed)
    # 8: data_len (compressed_len << 1 | 1? or similar) (4 bytes)
    # 12: data_dst (4 bytes)
    num_blocks = (table_len - 4) // 16
    for idx in range(num_blocks):
        offset = idx * 16
        block = table_data[offset : offset + 16]
        
        fn_offset = int.from_bytes(block[0:4], "little", signed=True)
        data_offset_rel = int.from_bytes(block[4:8], "little", signed=True)
        raw_len = int.from_bytes(block[8:12], "little")
        data_len = raw_len >> 1
        flag = bool(raw_len & 1)
        data_dst = int.from_bytes(block[12:16], "little")

        # Absolute offsets within binary file
        fn_abs = table_offset + offset + fn_offset
        data_abs = table_offset + offset + 4 + data_offset_rel

        print(f"  Block #{idx}:")
        print(f"    Raw bytes:           {block.hex(' ')}")
        print(f"    Decompress Function: offset={fn_offset} -> file offset {hex(fn_abs)}")
        print(f"    Source Data Address: offset={data_offset_rel} -> file offset {hex(data_abs)}")
        print(f"    Compressed Length:   {data_len} bytes (raw_len={hex(raw_len)}, flag={flag})")
        print(f"    Destination RAM:     0x{data_dst:08X}")
        print(f"    File range:          {hex(data_abs)} to {hex(data_abs + data_len)}")

    terminator = int.from_bytes(table_data[-4:], "little")
    print(f"  Table Terminator: 0x{terminator:08X}")


def do_otfdec(file_path, nonce_offset, key_offset):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    data = path.read_bytes()
    if len(data) < max(nonce_offset + 12, key_offset + 16):
        print("Error: Binary is too small to contain OTFDEC parameters at specified offsets.")
        sys.exit(1)

    nonce = data[nonce_offset : nonce_offset + 8].hex()
    end_val = struct.unpack("<I", data[nonce_offset + 8 : nonce_offset + 12])[0]
    word_extra = struct.unpack("<I", data[nonce_offset + 12 : nonce_offset + 16])[0]
    key = data[key_offset : key_offset + 16].hex()

    print(f"OTFDEC Cryptographic parameters in '{file_path}':")
    print(f"  Nonce (offset {hex(nonce_offset)}):      {nonce}")
    print(f"  End Address (offset {hex(nonce_offset+8)}): 0x{end_val:08x}")
    print(f"  Extra Word (offset {hex(nonce_offset+12)}):  0x{word_extra:08x}")
    print(f"  Key (offset {hex(key_offset)}):        {key}")


def do_literal_refs(file_path, target_addr):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    data = path.read_bytes()
    print(f"Searching for Thumb PC-relative loads targeting 0x{target_addr:X} in '{file_path}':")
    
    found = False
    # Check every 2 bytes (Thumb instruction alignment)
    for pc in range(0, len(data) - 4, 2):
        # 16-bit LDR PC-relative: 0100 1 rrr iiii iiii (0x4800 to 0x4FFF)
        val16 = struct.unpack("<H", data[pc:pc+2])[0]
        if (val16 & 0xF800) == 0x4800:
            reg = (val16 >> 8) & 7
            imm = (val16 & 0xFF) * 4
            target = (pc & ~3) + 4 + imm
            if target == target_addr:
                print(f"  16-bit LDR at {hex(pc)}: ldr r{reg}, [pc, #{hex(imm)}] -> targets {hex(target)}")
                found = True

        # 32-bit LDR PC-relative: 1111 1000 1101 1111 tttt iiii iiii iiii (0xf8df)
        hw1, hw2 = struct.unpack("<HH", data[pc:pc+4])
        if hw1 == 0xF8DF:
            reg = (hw2 >> 12) & 0xF
            imm = hw2 & 0xFFF
            target = (pc & ~3) + 4 + imm
            if target == target_addr:
                print(f"  32-bit LDR.W at {hex(pc)}: ldr.w r{reg}, [pc, #{hex(imm)}] -> targets {hex(target)}")
                found = True

    if not found:
        print("  No matching PC-relative loads found.")


def do_padding(file_path, start, end):
    path = Path(file_path)
    if not path.exists():
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    data = path.read_bytes()
    print(f"Scanning for zero-padding (4 aligned zero bytes) in '{file_path}' between {hex(start)} and {hex(end)}:")
    
    found = False
    for offset in range(start & ~3, min(end, len(data) - 3), 4):
        val = data[offset : offset + 4]
        if val == b"\x00\x00\x00\x00":
            print(f"  Found 4-byte padding at offset {hex(offset)}")
            found = True

    if not found:
        print("  No zero-padding block found in range.")


def do_diff(file1, file2, start, end):
    path1, path2 = Path(file1), Path(file2)
    if not path1.exists() or not path2.exists():
        print(f"Error: Ensure both files exist: '{file1}', '{file2}'")
        sys.exit(1)

    data1 = path1.read_bytes()
    data2 = path2.read_bytes()

    limit = min(len(data1), len(data2))
    if end is None:
        end = limit
    else:
        end = min(end, limit)

    print(f"Diffing '{file1}' and '{file2}' in range {hex(start)} to {hex(end)}:")

    diffs = []
    for idx in range(start, end):
        if data1[idx] != data2[idx]:
            diffs.append((idx, data1[idx], data2[idx]))

    print(f"  Total differences found in range: {len(diffs)} bytes")
    if diffs:
        print("  First 30 byte differences:")
        for idx, val1, val2 in diffs[:30]:
            print(f"    offset {hex(idx)}: file1=0x{val1:02X} | file2=0x{val2:02X}")
        if len(diffs) > 30:
            print("    ...")


def main():
    parser = argparse.ArgumentParser(
        description="Host-side binary inspection utility for Game & Watch (STM32H7B0) binaries."
    )
    subparsers = parser.add_subparsers(dest="command", required=True, help="Subcommand to execute")

    # Metadata
    meta_parser = subparsers.add_parser("metadata", help="Read/Decode HeaderMetadata at 0x1B8")
    meta_parser.add_argument("file", help="Binary file to inspect")

    # rwdata
    rwdata_parser = subparsers.add_parser("rwdata", help="Parse relocation rwdata decompression table")
    rwdata_parser.add_argument("file", help="Binary file to inspect")
    rwdata_parser.add_argument("game", choices=["mario", "zelda"], help="Game layout to parse")
    rwdata_parser.add_argument("--offset", type=lambda x: int(x, 0), help="Override table offset")
    rwdata_parser.add_argument("--len", type=lambda x: int(x, 0), help="Override table length")

    # otfdec
    otfdec_parser = subparsers.add_parser("otfdec", help="Extract OTFDEC crypto parameters (Zelda default)")
    otfdec_parser.add_argument("file", help="Binary file to inspect")
    otfdec_parser.add_argument("--nonce-offset", type=lambda x: int(x, 0), default=0x16590, help="Nonce offset (default 0x16590)")
    otfdec_parser.add_argument("--key-offset", type=lambda x: int(x, 0), default=0x165A0, help="Key offset (default 0x165A0)")

    # literal-refs
    refs_parser = subparsers.add_parser("literal-refs", help="Search for PC-relative LDRs targeting a specific address")
    refs_parser.add_argument("file", help="Binary file to inspect")
    refs_parser.add_argument("target", type=lambda x: int(x, 0), help="Target address to search for (hex)")

    # padding
    pad_parser = subparsers.add_parser("padding", help="Scan for zero padding blocks")
    pad_parser.add_argument("file", help="Binary file to inspect")
    pad_parser.add_argument("start", type=lambda x: int(x, 0), help="Hex start offset")
    pad_parser.add_argument("end", type=lambda x: int(x, 0), help="Hex end offset")

    # diff
    diff_parser = subparsers.add_parser("diff", help="Diff two binaries")
    diff_parser.add_argument("file1", help="First binary file")
    diff_parser.add_argument("file2", help="Second binary file")
    diff_parser.add_argument("--start", type=lambda x: int(x, 0), default=0, help="Start offset (default 0)")
    diff_parser.add_argument("--end", type=lambda x: int(x, 0), help="End offset (default EOF)")

    args = parser.parse_args()

    if args.command == "metadata":
        do_metadata(args.file)
    elif args.command == "rwdata":
        # Default offsets if not overridden
        offsets = {"mario": (0x180A4, 36), "zelda": (0x1B390, 20)}
        default_offset, default_len = offsets[args.game]
        offset = args.offset if args.offset is not None else default_offset
        length = args.len if args.len is not None else default_len
        do_rwdata(args.file, offset, length)
    elif args.command == "otfdec":
        do_otfdec(args.file, args.nonce_offset, args.key_offset)
    elif args.command == "literal-refs":
        do_literal_refs(args.file, args.target)
    elif args.command == "padding":
        do_padding(args.file, args.start, args.end)
    elif args.command == "diff":
        do_diff(args.file1, args.file2, args.start, args.end)


if __name__ == "__main__":
    main()
