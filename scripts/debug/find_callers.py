#!/usr/bin/env python3
"""Find Thumb BL/B.W call sites that target a given address in an OFW backup.

Decodes the 32-bit Thumb BL/B.W (and BLX) encoding across the whole image and
reports every instruction whose computed branch target equals --target. Used to
find what calls the OFW's standby routine (so we can trace the boot-path gate
that powers the device off on a warm reset).

Usage: python3 scripts/debug/find_callers.py mario 0x08003198
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import resolve

FLASH_BASE = 0x08000000
BACKUPS = {"mario": "backup/internal_flash_backup_mario.bin",
           "zelda": "backup/internal_flash_backup_zelda.bin"}


def bl_target(addr, hw1, hw2):
    """Decode a Thumb-2 BL/B.W at `addr` (hw1=first halfword, hw2=second). Returns
    the target address, or None if not a BL/BLX/B.W (J-form) encoding."""
    if (hw1 & 0xF800) != 0xF000:
        return None
    if (hw2 & 0xD000) not in (0xD000, 0xC000):  # BL=0xD, BLX=0xC, B.W=0x9/0xB
        # also accept B.W (0x8/0x9 in top nibble of hw2): handle below
        if (hw2 & 0x8000) == 0:
            return None
    s = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3FF
    j1 = (hw2 >> 13) & 1
    j2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7FF
    i1 = 1 - (j1 ^ s)
    i2 = 1 - (j2 ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if s:
        imm -= (1 << 25)
    return (addr + 4 + imm) & 0xFFFFFFFF


def main() -> int:
    target_name = sys.argv[1]
    want = int(sys.argv[2], 0) & ~1
    data = resolve(BACKUPS[target_name]).read_bytes()

    print(f"# {target_name}: BL/B.W call sites targeting 0x{want:08X}\n")
    n = 0
    for i in range(0, len(data) - 3, 2):
        hw1 = data[i] | (data[i + 1] << 8)
        hw2 = data[i + 2] | (data[i + 3] << 8)
        t = bl_target(FLASH_BASE + i, hw1, hw2)
        if t is not None and (t & ~1) == want:
            kind = "BL " if (hw2 & 0x1000) else "B.W"
            print(f"  0x{FLASH_BASE + i:08X}: {kind} -> 0x{want:08X}")
            n += 1
    print(f"\n# {n} call site(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
