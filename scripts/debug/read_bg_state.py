#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

def main():
    backend = OpenOCDBackend()
    backend.open()
    try:
        addresses = {
            "g_bg_tick": 0x2400ee6c,
            "g_bg_r9": 0x2400ee70,
            "g_mod_r9": 0x2400ee74,
        }
        print("=== Background State in RAM ===")
        for name, addr in addresses.items():
            val = struct.unpack("<I", backend.read_memory(addr, 4))[0]
            print(f"  {name:<10} @ 0x{addr:08X} = 0x{val:08X}")
    finally:
        backend.close()

if __name__ == "__main__":
    main()
