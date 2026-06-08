#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

def main():
    backend = OpenOCDBackend()
    backend.open()
    try:
        addr = 0x905582B0
        size = 32
        print(f"Reading {size} bytes from 0x{addr:08X}...")
        data = backend.read_memory(addr, size)
        print("Raw bytes:")
        print(data.hex())
        
        # Disassemble using capstone if available
        try:
            from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
            md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
            print("Disassembly:")
            for op in md.disasm(data, addr):
                print(f"  0x{op.address:08X}:  {op.mnemonic:<8s} {op.op_str}")
        except ImportError:
            print("Capstone not available for disassembly.")
    finally:
        backend.close()

if __name__ == "__main__":
    main()
