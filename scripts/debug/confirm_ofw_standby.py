#!/usr/bin/env python3
"""Confirm the OFW powers off via the standby routine on a warm switch.

Sets a hardware breakpoint at the OFW standby sequence (0x08003198 = PWR_CPUCR
PDDS + SCB SLEEPDEEP + WFI; after the swap, bank2/OFW is mapped at 0x08000000 so
the address is unchanged), drives menu -> BOOT ACTIVE OFW over the remote-input
shadow cell, then checks whether execution stops at the breakpoint. If it does,
the OFW is deliberately entering standby on the warm boot -> root cause confirmed.
Also reads GPIOA->IDR bit0 (the power button) at the stop to confirm the
"button-not-held" branch.

Requires a REMOTE_INPUT build flashed; device on the chainloader main menu.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h

STANDBY_SEQ = 0x08003198   # PWR_CPUCR/SLEEPDEEP/WFI in the OFW (bank2 mapped low after swap)
MAIN_ITEMS = 6
IDX_BOOT_OFW = 1
GPIOA_IDR = 0x58020010


def main() -> int:
    with ri.session() as dev:
        be = dev.backend
        h.wake(dev)
        cur = h.navigate_to(dev, IDX_BOOT_OFW, MAIN_ITEMS)
        if cur != IDX_BOOT_OFW:
            print(f"!! could not navigate to BOOT ACTIVE OFW (at {cur})")
            return 1
        print("on BOOT ACTIVE OFW; pressing A (swap+reset)")
        dev.button_press([ri.BTN_A])

        # The swap resets the MCU and drops the SWD session. Reconnect, then set a
        # HW breakpoint at the standby sequence and let the OFW run into it.
        time.sleep(2.5)
        hit = False
        for attempt in range(8):
            try:
                dev.reconnect()
            except Exception:
                time.sleep(1.0)
                continue
            be = dev.backend
            try:
                # halt, plant hw breakpoint, resume, then poll for the stop
                be("halt", decode=False)
                be(f"bp 0x{STANDBY_SEQ:08X} 2 hw", decode=False)
                be("resume", decode=False)
            except Exception as e:
                print(f"  [{attempt}] setup failed: {e}")
                time.sleep(1.0)
                continue
            # give it a moment to reach the breakpoint
            time.sleep(1.5)
            try:
                be("halt", decode=False)
                pc = be.read_register("pc") & ~1
            except Exception:
                pc = None
            print(f"  [{attempt}] PC = " + ("(unreadable)" if pc is None else f"0x{pc:08X}"))
            if pc is not None and abs(pc - STANDBY_SEQ) < 0x20:
                hit = True
                idr = h.safe_read_u32(be, GPIOA_IDR)
                btn = "(unreadable)" if idr is None else \
                    ("HELD (bit0=0)" if not (idr & 1) else "NOT held (bit0=1)")
                print(f"\n  >>> BREAKPOINT HIT at standby seq 0x{pc:08X}")
                print(f"      GPIOA->IDR = " +
                      ("?" if idr is None else f"0x{idr:08X}") + f"  power button {btn}")
                print("      => CONFIRMED: OFW enters standby on warm switch, "
                      "gated on power-button not being held.")
                try:
                    be(f"rbp 0x{STANDBY_SEQ:08X}", decode=False)
                except Exception:
                    pass
                break
            time.sleep(0.8)
        if not hit:
            print("\n  breakpoint not observed to hit (device may have already "
                  "powered down before we could plant/resume, or path differs).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
