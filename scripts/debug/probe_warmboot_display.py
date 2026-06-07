#!/usr/bin/env python3
"""Test the warm-boot display fix: does the patched OFW come up WITHOUT a power press?

Background (docs/ofw-poweroff-on-warm-switch.md): a warm switch into the stock OFW
normally leaves it headless and then it powers off, so a manual power-button press
is required. The candidate fix NOPs the PWR_CPUCR.SBF gate (0x08006038) so the
OFW takes the full cold-boot init path on a warm reset, plus keeps the state-6
standby arm patched (0x08005F08 bpl->b).

This drives the switch over the probe and then deliberately does NOT press power.
It reconnects across the swap-reset and reads, objectively:
  - is the core alive at all (reconnect succeeds and 0x08000004 reads the Mario
    reset vector) — vs. powered fully off (reconnect keeps failing);
  - LTDC GCR.LTDCEN (display controller enabled);
  - GPIOD ODR bit4 (PD4 = the 3.3V LCD rail the OFW drives high during display init);
  - a few PC samples (running the game loop vs. parked).

Verdict:
  * core alive + LTDCEN=1 + PD4 high   -> FIXED: display inits on a warm switch.
  * core alive + LTDCEN=0 / PD4 low    -> still headless (SBF-gate fix insufficient).
  * core dead for the whole window      -> still powers off (fix didn't take / wrong).

Requires a REMOTE_INPUT chainloader on bank1 and the patched Mario staged in
external flash (make flash_chainloader flash_mario REMOTE_INPUT=1). Device on the
chainloader main menu.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h

# Main-menu item indices (menu.c MAIN_LABELS order).
IDX_BOOT_OFW = 1
MAIN_ITEMS = 6

# Per-console: SWITCH menu index + the reset vector each patched OFW carries (the
# chainloader stub entry appended to the image). The switch flashes that OFW into
# bank2; the reset vector identifies it (see boot_selector_test.py).
CONSOLES = {
    "mario": {"switch_idx": 2, "reset_vec": 0x08018101},
    "zelda": {"switch_idx": 3, "reset_vec": 0x0801B3E1},
}

BANK2_RESET_VEC_ADDR = 0x08100004  # bank2 reset vec while UNswapped
RUNNING_VEC_ADDR = 0x08000004      # reset vec of whatever is mapped low (after swap)

LTDC_GCR = 0x50001018              # LTDC global control; bit0 = LTDCEN
GPIOD_ODR = 0x58020C14             # bit4 = PD4 (3.3V LCD rail)
PWR_CPUCR = 0x58024810             # bit6 SBF, bit5 STOPF


def sample_pc(be, n=5):
    """Halt/read-PC/resume a few times; returns the list of PCs (alive => varies)."""
    pcs = []
    for _ in range(n):
        try:
            be("halt", decode=False)
            pcs.append(be.read_register("pc") & ~1)
            be("resume", decode=False)
        except Exception:
            pcs.append(None)
        time.sleep(0.2)
    return pcs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("console", choices=list(CONSOLES), default="mario", nargs="?")
    args = ap.parse_args()
    cfg = CONSOLES[args.console]
    switch_idx, reset_vec = cfg["switch_idx"], cfg["reset_vec"]
    name = args.console.upper()

    with ri.session() as dev:
        be = dev.backend

        sel = h.read_menu_selection(be)
        if not (0 <= sel < MAIN_ITEMS):
            print(f"!! not on chainloader main menu (selection {sel}); flash REMOTE_INPUT build")
            return 1

        # --- 1. SWITCH TO <console>: flash the (newly patched) image into bank2 ---
        cur = h.navigate_to(dev, switch_idx, MAIN_ITEMS)
        if cur != switch_idx:
            print(f"!! could not reach SWITCH TO {name} (at {cur})")
            return 1
        print(f"on SWITCH TO {name}; pressing A (erase+write bank2)...")
        dev.button_press([ri.BTN_A])
        deadline = time.time() + 30
        vec = None
        while time.time() < deadline:
            time.sleep(1.0)
            vec = h.safe_read_u32(be, BANK2_RESET_VEC_ADDR)
            if vec == reset_vec:
                break
        if vec != reset_vec:
            print(f"!! bank2 did not become {name} (reset vec {vec}); aborting")
            return 1
        print(f"   bank2 now holds patched {name} (reset vec 0x{vec:08X})")

        # --- 2. BOOT ACTIVE OFW: bank swap + reset = the WARM switch under test ---
        cur = h.navigate_to(dev, IDX_BOOT_OFW, MAIN_ITEMS)
        if cur != IDX_BOOT_OFW:
            print(f"!! could not reach BOOT ACTIVE OFW (at {cur})")
            return 1
        print("on BOOT ACTIVE OFW; pressing A (warm swap+reset). NOT pressing power.")
        dev.button_press([ri.BTN_A])

        # --- 3. Observe WITHOUT a power press: is it alive + display up? ---
        time.sleep(2.5)
        alive = False
        for attempt in range(10):          # ~ up to 20 s of reconnect attempts
            try:
                dev.reconnect()
            except Exception:
                time.sleep(1.2)
                continue
            be = dev.backend
            vec = h.safe_read_u32(be, RUNNING_VEC_ADDR)
            if vec is not None:
                print(f"   [{attempt}] core ALIVE; reset vec @0x08000004 = 0x{vec:08X}")
                alive = True
                break
            print(f"   [{attempt}] core not responding yet...")
            time.sleep(1.2)

        print("\n" + "=" * 64)
        if not alive:
            print("  VERDICT: core stayed DEAD through the whole window.")
            print("  => device still powers off on a warm switch (fix didn't take).")
            print("  (Press the power button to recover the probe for the next run.)")
            print("=" * 64)
            return 2

        ltdc = h.safe_read_u32(be, LTDC_GCR)
        odrd = h.safe_read_u32(be, GPIOD_ODR)
        cpucr = h.safe_read_u32(be, PWR_CPUCR)
        ltdcen = None if ltdc is None else (ltdc & 1)
        pd4 = None if odrd is None else ((odrd >> 4) & 1)
        pcs = sample_pc(be)

        def hx(v):
            return "?" if v is None else f"0x{v:08X}"

        print(f"  LTDC_GCR     = {hx(ltdc)}  (LTDCEN={ltdcen})")
        print(f"  GPIOD_ODR    = {hx(odrd)}  (PD4={pd4})")
        print(f"  PWR_CPUCR    = {hx(cpucr)}  (SBF={None if cpucr is None else (cpucr>>6)&1}, "
              f"STOPF={None if cpucr is None else (cpucr>>5)&1})")
        print("  PC samples   = " + ", ".join(hx(p) for p in pcs))
        if ltdcen == 1 and pd4 == 1:
            print(f"\n  VERDICT: FIXED -- {name} display initialized on a warm switch, no power press.")
            try:
                p, info = h.capture_frame(be, f"build/warmboot_{args.console}.png")
                print(f"  framebuffer saved: {p} ({info})")
            except Exception as e:
                print(f"  (framebuffer capture failed: {e})")
        else:
            print("\n  VERDICT: core alive but display NOT initialized (still headless).")
            print("  => SBF-gate fix alone insufficient; init path still diverges.")
        print("=" * 64)
    return 0


if __name__ == "__main__":
    sys.exit(main())
