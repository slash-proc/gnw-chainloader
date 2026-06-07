#!/usr/bin/env python3
"""Scan a stock OFW backup for power-off / standby / power-hold mechanisms.

We know the OFW powers the device fully off on a warm (software-reset) entry.
This locates the candidate mechanisms so we can see WHAT it does to turn off:
  - WFI / WFE instructions (standby/stop entry usually ends in WFI)
  - references to PWR base (0x58024800) — CPUCR/standby config
  - references to the GPIO ODR/BSRR of the main power-hold pin
  - SCB SCR (0xE000ED10) writes (SLEEPDEEP set => deep sleep/standby)

Usage: python3 scripts/debug/scan_ofw_power.py mario
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

# 32-bit constants worth finding in the literal pools.
INTERESTING = {
    0x58024800: "PWR_BASE (CR1)",
    0x58024810: "PWR_CPUCR (standby PDDS/CSSF)",
    0x58024820: "PWR_WKUPCR",
    0x58024824: "PWR_WKUPFR",
    0x58024828: "PWR_WKUPEPR",
    0x58024818: "PWR_SRDCR",
    0xE000ED10: "SCB_SCR (SLEEPDEEP)",
    0x580244D0: "RCC_RSR",
    0x58024530: "RCC_RSR (alias)",
    0x58020000: "GPIOA_BASE",
    0x58020400: "GPIOB_BASE",
    0x58020800: "GPIOC_BASE",
    0x58020C00: "GPIOD_BASE",
    0x58021000: "GPIOE_BASE",
}


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "mario"
    data = resolve(BACKUPS[target]).read_bytes()

    print(f"# {target}: scanning {len(data)} bytes\n")

    # 1) WFI (0xBF30) / WFE (0xBF20) thumb opcodes.
    print("== WFI/WFE instructions (standby/stop entry) ==")
    for i in range(0, len(data) - 1, 2):
        hw = data[i] | (data[i + 1] << 8)
        if hw in (0xBF30, 0xBF20):
            print(f"  0x{FLASH_BASE + i:08X}: {'WFI' if hw == 0xBF30 else 'WFE'}")

    # 2) Literal-pool references to interesting peripheral addresses.
    print("\n== literal-pool refs to power/standby/GPIO regs ==")
    seen = {}
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, i)[0]
        if w in INTERESTING:
            seen.setdefault(w, []).append(FLASH_BASE + i)
    for addr, name in INTERESTING.items():
        locs = seen.get(addr, [])
        if locs:
            shown = " ".join(f"0x{l:08X}" for l in locs[:8])
            more = f"  (+{len(locs) - 8} more)" if len(locs) > 8 else ""
            print(f"  0x{addr:08X} {name:32s}: {shown}{more}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
