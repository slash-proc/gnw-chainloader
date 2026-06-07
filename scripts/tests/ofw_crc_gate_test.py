#!/usr/bin/env python3
"""OFW CRC gate verification (negative + restore), on hardware.

The chainloader bakes a CRC-32 signature of each patched OFW backup image and its
external asset blob (src/common/ofw_crc.h) and re-checks them on the device before
copying an OFW into Bank 2 and booting it (system/ofw_verify.c). This test proves
the gate actually BLOCKS a recognized-but-wrong image, the realistic way:

  1. Flash the STOCK (official, unpatched) external firmware over Mario's asset
     blob at its real offset (0x400000). The patched internal backup is left in
     place, so the launcher still RECOGNISES a Mario backup (reset vector intact)
     and offers it -- but the asset CRC no longer matches the baked signature.
  2. Select MARIO and press A. The gate must REFUSE: Bank 2 is left untouched and
     the device stays on the chainloader menu (no bank swap, no boot).
  3. Restore the patched asset blob and confirm MARIO boots again (gate passes),
     escaping back with Left+Game.

Probe sessions and gnwmanager flashes are strictly serialized (one owner of the
ST-Link at a time): each flash runs with the OpenOCD session closed.

Run after: make REMOTE_INPUT=1 flash-all   (REMOTE_INPUT is on by default).
Leaves the device on the chainloader main menu with the patched Mario restored.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import harness as h          # noqa: E402
from common import remote_input as ri    # noqa: E402
import boot_selector_test as bs          # noqa: E402  (reuse selector helpers)

MARIO_ASSET_OFFSET = 0x400000
STOCK_EXT_MARIO = "backup/flash_backup_mario.bin"        # official, unpatched
PATCHED_EXT_MARIO = "build/patched_external_mario.bin"   # our patched asset blob


def gnw_flash(rel_path: str, offset: int) -> None:
    """Flash a file to external flash at `offset`, then reboot into the chainloader.

    The probe session must be closed (serialized ST-Link ownership). gnwmanager
    leaves the device halted in the RAM flasher, so we chain `start bank1` to boot
    the chainloader; callers then settle before re-attaching.
    """
    p = REPO / rel_path
    if not p.exists():
        raise SystemExit(f"missing {p}")
    print(f"  [flash] {rel_path} -> ext 0x{offset:06X}  (+ start bank1)")
    subprocess.run(
        ["gnwmanager", "flash", "ext", str(p), f"--offset=0x{offset:X}",
         "--", "start", "bank1"],
        cwd=REPO, check=True,
    )
    time.sleep(2.5)   # let the chainloader boot + run its partition scan


def gnw_boot() -> None:
    """Reset into the chainloader (probe session must be closed). gnwmanager leaves
    the device halted in the RAM flasher between runs, so establish a known running
    chainloader before attaching."""
    print("  [boot] start bank1")
    subprocess.run(["gnwmanager", "start", "bank1"], cwd=REPO, check=True)
    time.sleep(2.5)   # boot + partition scan


def bank2_vec(dev) -> int | None:
    return h.safe_read_u32(dev.backend, bs.BANK2_RESET_VEC_ADDR)


def main() -> int:
    t = h.TestRun("ofw crc gate (stock FW must be refused)")

    # --- Phase A: note the active OFW; require Mario not currently active -------
    gnw_boot()   # establish a running chainloader (a prior run may have left it halted)
    with ri.session() as dev:
        sel = h.read_menu_selection(dev.backend)
        t.require(0 <= sel < bs.MAIN_ITEMS, f"on chainloader main menu (selection {sel})")
        active = bank2_vec(dev)
        t.note(f"Bank 2 reset vector before = 0x{(active or 0):08X}")
        t.require(active != bs.OFW_RESET_VEC["MARIO"],
                  "Mario is not the active OFW (so booting it forces a verified flash)")

    # --- Phase B: flash the STOCK external over Mario's asset blob --------------
    gnw_flash(STOCK_EXT_MARIO, MARIO_ASSET_OFFSET)

    # --- Phase C: boot Mario -> the gate must refuse ---------------------------
    with ri.session() as dev:
        bs.ensure_selector(dev)
        ok = bs.select_target(dev, "MARIO")
        t.require(ok, "MARIO still offered (patched internal backup recognised)")
        before = bank2_vec(dev)
        dev.button_press([ri.BTN_A])
        t.note("A on < MARIO > with stock asset blob: verify must fail before any erase.")
        time.sleep(4.0)                       # verify-fail UI shows ~2s, then returns
        sel = h.read_menu_selection(dev.backend)
        after = bank2_vec(dev)
        t.require(sel is not None and 0 <= sel < bs.MAIN_ITEMS,
                  f"still on the chainloader menu after refusal (selection {sel})")
        t.require(after == before,
                  f"Bank 2 untouched (no swap/boot): 0x{(before or 0):08X} -> 0x{(after or 0):08X}")

    # --- Phase D: restore the patched asset blob -------------------------------
    gnw_flash(PATCHED_EXT_MARIO, MARIO_ASSET_OFFSET)

    # --- Phase E: Mario boots again (gate now passes) --------------------------
    with ri.session() as dev:
        bs.ensure_selector(dev)
        bs.verify_ofw(t, dev, "MARIO")        # flash+verify -> swap -> boot -> Left+Game escape

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
