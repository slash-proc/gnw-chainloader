#!/usr/bin/env python3
"""Diagnose the unified boot selector's target state over SWD.

Explains boot-selector surprises (e.g. selecting ZELDA booting Mario) by showing
the detected console type against what bank 2 and the external-flash OFW backups
actually hold, plus the resulting target-bootability the menu would compute.

  python3 scripts/debug/boot_target_state.py

board_console_type is detected once at chainloader boot (board_detect_console_type
reads bank 2's reset vector). boot_selected_target() then treats `want ==
board_console_type` as "bank 2 already holds the wanted OFW" and skips re-flashing
the backup -- so a stale/mismatched console type is exactly what makes the wrong
OFW boot. This dump makes that visible.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

EXTFLASH_BASE = 0x90000000
MARIO_SPI_OFFSET = 0x007C0000
ZELDA_SPI_OFFSET = 0x007E0000
OFW_INTERNAL_BASE = 0x08100000

CONSOLE = {0: "NONE", 1: "MARIO", 2: "ZELDA", 3: "UNKNOWN"}
VEC = {0x08018101: "MARIO", 0x0801B3E1: "ZELDA"}


def vname(v):
    return VEC.get(v, "??") if v is not None else "(unreadable)"


def main():
    be = OpenOCDBackend()
    be.open()
    try:
        be.halt()
        cct = h.read_u32_symbol(be, "board_console_type") & 0xFF
        try:
            ext_sz = h.read_u32_symbol(be, "total_ext_flash_size")
        except Exception:
            ext_sz = None
        bank2 = h.safe_read_u32(be, OFW_INTERNAL_BASE + 4)
        mario_bk = h.safe_read_u32(be, EXTFLASH_BASE + MARIO_SPI_OFFSET + 4)
        zelda_bk = h.safe_read_u32(be, EXTFLASH_BASE + ZELDA_SPI_OFFSET + 4)
        be.resume()
    finally:
        be.close()

    print(f"board_console_type   = {CONSOLE.get(cct, cct)}")
    print(f"total_ext_flash_size = {ext_sz if ext_sz is None else hex(ext_sz)}")
    print(f"bank2  (0x{OFW_INTERNAL_BASE + 4:08X}) = "
          f"{'(unreadable)' if bank2 is None else f'0x{bank2:08X}'} ({vname(bank2)})")
    print(f"Mario backup (0x{EXTFLASH_BASE + MARIO_SPI_OFFSET + 4:08X}) = "
          f"{'(unreadable)' if mario_bk is None else f'0x{mario_bk:08X}'} ({vname(mario_bk)})")
    print(f"Zelda backup (0x{EXTFLASH_BASE + ZELDA_SPI_OFFSET + 4:08X}) = "
          f"{'(unreadable)' if zelda_bk is None else f'0x{zelda_bk:08X}'} ({vname(zelda_bk)})")

    # Mirror boot_selected_target()'s active_valid heuristic for each OFW target.
    for want_name, want_id in (("MARIO", 1), ("ZELDA", 2)):
        active_valid = (want_id == cct) and bank2 in VEC
        bank2_is_want = vname(bank2) == want_name
        verdict = ("boots bank2 as-is" if active_valid else "re-flashes backup first")
        warn = "" if (not active_valid or bank2_is_want) else \
            "  <-- MISMATCH: would boot bank2 which is NOT this target"
        print(f"select {want_name:5} -> active_valid={active_valid} ({verdict}){warn}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
