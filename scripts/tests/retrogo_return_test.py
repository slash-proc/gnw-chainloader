#!/usr/bin/env python3
"""Retro-Go "Return to Main Menu" regression guard (the round trip that has
broken 5+ times).

Retro-Go signals "go back to my launcher" by leaving a magic word in the DTCM
cell at RG_MAGIC_ADDR (0x20000000) and warm-resetting:

  * RESET (0x1FA1AFE1) — the warm-reset *trace* Retro-Go writes on every boot
    (retro-go-sd .../main.c:312). A bank-1 "Return to Main Menu"
    (odroid_system_switch_app(0)) just bare-resets, so this is what the chainloader
    actually sees. It is consumed by the *stub* (stub_main.c), which must jump to
    RETROGO_BASE.
  * CORE (0x434F5245) — the explicit emulator/quit marker (bank-2 builds). Consumed
    by main.c app_early_logic() §2.1, which must also jump to RETROGO_BASE.

Either way the chainloader MUST re-launch Retro-Go so ITS launcher reloads — never
fall through to the chainloader's own menu. (Quitting *to* the chainloader is a
different signal: BOOT at 0x2001FFF8 + target, untouched here.)

This injects each magic, warm-resets, resumes, and asserts Retro-Go came up — the
"GNW CHAINLOADER" header is gone and the PC is no longer in the chainloader's
AXI-SRAM (0x24xxxxxx). It FAILS on the old "fall through to the menu" behavior.

Requires Retro-Go flashed at RETROGO_BASE (skips with a note otherwise — the
fall-through-to-menu path is the *correct* behavior when Retro-Go is absent).

  python3 scripts/tests/retrogo_return_test.py
"""
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import device
from common import ocr

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

RG_MAGIC_ADDR = 0x20000000
RETROGO_BASE = 0x0800A000
BOOT_MAGIC_RESET = 0x1FA1AFE1     # stub path (stub_main.c)
BOOT_MAGIC_RETROGO = 0x434F5245   # "CORE" — main.c §2.1 path
CHAINLOADER_REGION = 0x24         # app runs from AXI-SRAM; Retro-Go does not


def screen(be):
    img, _ = device.read_framebuffer(be)
    return ocr.Screen(np.asarray(img.convert("RGB")))


def retrogo_present(be):
    sp = be.read_uint32(RETROGO_BASE)
    pc = be.read_uint32(RETROGO_BASE + 4)
    return (sp >> 24) in (0x20, 0x24) and (pc >> 24) in (0x08, 0x24)


def launch_via(be, magic, name):
    """Inject *magic* at RG_MAGIC_ADDR, warm-reset, resume, and report whether
    Retro-Go (not the chainloader menu) came up."""
    be.halt()
    be.write_uint32(RG_MAGIC_ADDR, magic)
    be.reset_and_halt()                       # SYSRESETREQ; DTCM survives
    survived = be.read_uint32(RG_MAGIC_ADDR)
    be.resume()
    time.sleep(1.2)                            # the stub/main.c hands off to Retro-Go
    be.halt()                                  # within ~1s; Retro-Go is reliably running
    pc = be.read_register("pc") & ~1           # by then (sampling at 3s can catch its
    on_menu = screen(be).has("GNW CHAINLOADER") # own later launcher->game transitions)
    be.resume()
    rg = (not on_menu) and (pc >> 24) != CHAINLOADER_REGION
    print(f"  [{name}] magic survived reset=0x{survived:08X}  PC=0x{pc:08X}  "
          f"chainloader-header={'shown' if on_menu else 'gone'}  "
          f"-> {'Retro-Go launched' if rg else 'CHAINLOADER MENU (regression!)'}")
    return rg


def restore_menu(be):
    """Leave the device on the chainloader menu, not re-launching Retro-Go."""
    be.halt()
    be.write_uint32(RG_MAGIC_ADDR, 0)
    be.reset_and_halt()
    be.resume()
    time.sleep(2.0)


def main():
    be = OpenOCDBackend()
    be.open()
    try:
        be.halt()
        if not retrogo_present(be):
            print("SKIP: Retro-Go not flashed at RETROGO_BASE "
                  "(fall-through to the chainloader menu is correct when absent).")
            be.resume()
            return 0
        be.resume()

        fails = 0
        if not launch_via(be, BOOT_MAGIC_RESET, "RESET / stub path"):
            fails += 1
        if not launch_via(be, BOOT_MAGIC_RETROGO, "CORE / main.c §2.1 path"):
            fails += 1

        restore_menu(be)
        print("RESULT:", "ALL PASS — Retro-Go re-launches on return-to-menu"
              if fails == 0 else f"{fails} FAILED — regressed to the chainloader menu")
        return 1 if fails else 0
    finally:
        be.close()


if __name__ == "__main__":
    sys.exit(main())
