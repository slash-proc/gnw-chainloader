#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

def main():
    backend = OpenOCDBackend()
    backend.open()
    try:
        print("=== CPU Registers ===")
        for reg in ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "xpsr"]:
            try:
                val = backend.read_register(reg)
                print(f"  {reg:<4} = 0x{val:08X}")
            except Exception as e:
                print(f"  {reg:<4} = Error: {e}")
    finally:
        backend.close()

if __name__ == "__main__":
    main()
