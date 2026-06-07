#!/usr/bin/env python3
"""Trace the device state across a chainloader -> OFW switch (the power-off bug).

Drives the menu to BOOT ACTIVE OFW over the remote-input shadow cell, dumping
every register that could explain the "device goes off, press power to start the
OFW" behavior: PWR (standby/wakeup), RCC->RSR (reset cause), the SRAM/backup
magic cells, and bank-swap state. After triggering the switch it polls across the
reset (reconnecting the probe each attempt) to see whether the OFW lands in
standby (reads stay empty), hangs, or runs.

Requires a REMOTE_INPUT build flashed (make REMOTE_INPUT=1 ...). Leave the device
on the chainloader main menu before running.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h

# --- registers of interest (STM32H7B0) ---
REGS = {
    "RCC_RSR     ": 0x580244D0,   # reset cause flags
    "PWR_CR1     ": 0x58024800,
    "PWR_CSR1    ": 0x58024804,
    "PWR_CR2     ": 0x58024808,
    "PWR_CR3     ": 0x5802480C,
    "PWR_CPUCR   ": 0x58024810,   # SBF (bit6) standby, STOPF (bit5), PDDS_D1/D3
    "PWR_D3CR    ": 0x58024818,
    "PWR_WKUPCR  ": 0x58024820,
    "PWR_WKUPFR  ": 0x58024824,   # pending wakeup flags (WKUPF1 = power button)
    "PWR_WKUPEPR ": 0x58024830,   # wakeup pin enable / polarity
    "SRAM_SHADOW ": 0x2001FFF4,
    "SRAM_MAGIC  ": 0x2001FFF8,
    "SRAM_TARGET ": 0x2001FFFC,
    "RG_MAGIC    ": 0x20000000,
    "BKP0R       ": 0x58004900,
    "BKP3R       ": 0x5800490C,
    "FLASH_OPTSR ": 0x5200201C,   # OPTSR_CUR (SWAP_BANK bit31)
}

MAIN_ITEMS = 6
IDX_BOOT_OFW = 1


def dump(dev, label):
    print(f"\n----- {label} -----")
    be = dev.backend
    for name, addr in REGS.items():
        v = h.safe_read_u32(be, addr)
        print(f"  {name} 0x{addr:08X} = " + ("(unreadable)" if v is None else f"0x{v:08X}"))


def decode_rsr(v):
    if v is None:
        return "unreadable"
    flags = []
    if v & (1 << 23): flags.append("PORRSTF")
    if v & (1 << 24): flags.append("SFTRSTF")
    if v & (1 << 22): flags.append("PINRSTF")
    if v & (1 << 21): flags.append("BORRSTF")
    if v & (1 << 25): flags.append("WWDG1RSTF")
    if v & (1 << 19): flags.append("D1RSTF")
    if v & (1 << 18): flags.append("D2RSTF")
    if v & (1 << 16): flags.append("CPURSTF")
    return " ".join(flags)


def main():
    with ri.session() as dev:
        be = dev.backend
        # Orient: where's the cursor?
        h.wake(dev)
        sel = h.read_menu_selection(be)
        print(f"menu selection = {sel}")
        dump(dev, "BASELINE (at menu, pre-switch)")
        rsr = h.safe_read_u32(be, 0x580244D0)
        print(f"\n  RSR decode: {decode_rsr(rsr)}")

        # Navigate to BOOT ACTIVE OFW.
        cur = h.navigate_to(dev, IDX_BOOT_OFW, MAIN_ITEMS)
        print(f"\nnavigated to index {cur} (want {IDX_BOOT_OFW})")
        if cur != IDX_BOOT_OFW:
            print("!! navigation failed; aborting before pressing A")
            return 1
        dump(dev, "RIGHT BEFORE pressing A (on BOOT ACTIVE OFW)")

        # Trigger the switch. This swaps banks + NVIC_SystemReset -> probe drops.
        print("\n>>> pressing A (BOOT ACTIVE OFW) — swap + reset incoming <<<")
        dev.button_press([ri.BTN_A])

        # Poll across the reset. Reconnect each attempt; capture whatever answers.
        print("\n----- POLLING across the switch (reconnect each try) -----")
        for i in range(12):
            time.sleep(1.2)
            try:
                dev.reconnect()
            except Exception as e:
                print(f"  [{i}] reconnect failed: {e}")
                continue
            be = dev.backend
            rsr = h.safe_read_u32(be, 0x580244D0)
            cpucr = h.safe_read_u32(be, 0x58024810)
            wkupfr = h.safe_read_u32(be, 0x58024824)
            vec = h.safe_read_u32(be, 0x08000004)   # running reset vector @ bank mapped to 0x08000000
            optsr = h.safe_read_u32(be, 0x5200201C)
            def fx(v): return "(empty)" if v is None else f"0x{v:08X}"
            swapped = "?" if optsr is None else ("SWAPPED" if (optsr & (1 << 31)) else "unswapped")
            print(f"  [{i:2d}] RSR={fx(rsr)} ({decode_rsr(rsr)})  CPUCR={fx(cpucr)}  "
                  f"WKUPFR={fx(wkupfr)}  vec@08000004={fx(vec)}  bank={swapped}")
        print("\n(If all reads are (empty): true standby / powered-down domain.\n"
              " If reads succeed with vec=Mario(0x08018101)/Zelda(0x0801B3E1): OFW is\n"
              " resident but parked. CPUCR SBF/STOPF + WKUPFR tell us the power state.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
