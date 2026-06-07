#!/usr/bin/env python3
"""Boot-selector verification: the unified LAUNCH selector boots each available
target (RETRO-GO / MARIO / ZELDA) over the probe -- no physical button.

  python3 scripts/tests/boot_selector_test.py [--target all|retrogo|mario|zelda]

Selection is driven closed-loop on the firmware's own g_boot_target (read over
SWD), never by guessing from OCR -- an earlier OCR-driven version mis-detected the
target and "found" a Zelda boot bug that did not exist (the firmware had MARIO
selected and correctly booted Mario). The selector skips targets that aren't
bootable on this unit (next_bootable in menu.c), so g_boot_target never lands on
them and the test reports those as not offered rather than failing.

Per target it cycles the selector to it, presses A, and:

  RETRO-GO  action_retro_go() -> board_request_jump(RETROGO_BASE): sets the BOOT
            magic + target and resets; the stub re-launches Retro-Go from Bank 1
            (no bank swap). Verified the way retrogo_return_test does -- the
            chainloader header is gone and the PC has left the chainloader's
            AXI-SRAM (0x24xxxxxx) for the Retro-Go flash region. Restored by
            clearing the magic cells and resetting back to the menu.

  MARIO /   flashes the patched OFW backup into Bank 2 if it isn't already the
  ZELDA     active OFW, bank-swaps, and boots it (no manual power-on, thanks to the
            warm-switch fix). Verified by the reset vector at 0x08000004 matching
            the OFW. Restored by injecting Left+Game, whose patched read_buttons()
            FRCE-resets and unswaps back to the chainloader launcher -- this runs
            even if the wrong OFW booted, so a mis-boot can never strand the banks
            swapped.

Run after: make REMOTE_INPUT=1 flash_all  (REMOTE_INPUT is on by default).
Leaves the device on the chainloader main menu.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import ocrnav
from common import device
from common import ocr

# Unified main menu (menu.c): MM_BOOT=0 (the LAUNCH selector), TOOLS, SETTINGS, POWER.
MM_BOOT = 0
MAIN_ITEMS = 4
BT_COUNT = 3                       # RETRO-GO / MARIO / ZELDA targets

RG_MAGIC_ADDR = 0x20000000         # DTCM "re-launch Retro-Go" cell
SRAM_MAGIC_ADDR = 0x2001FFF8       # generic BOOT-to-target cell (board_request_jump)
RETROGO_BASE = 0x0800A000
CHAINLOADER_REGION = 0x24          # the chainloader runs from AXI-SRAM; Retro-Go does not

# boot_target_t order in menu.c.
TARGET_ID = {"RETRO-GO": 0, "MARIO": 1, "ZELDA": 2}
ID_LABEL = {v: k for k, v in TARGET_ID.items()}
MATCH = {"RETRO-GO": "RETRO", "MARIO": "MARIO", "ZELDA": "ZELDA"}  # OCR sanity stems

# OFW reset vectors used to recognize what is running after the swap (board.c).
OFW_RESET_VEC = {"MARIO": 0x08018101, "ZELDA": 0x0801B3E1}
BANK2_RESET_VEC_ADDR = 0x08100004  # the backup copy in Bank 2, before the swap

_GBT_ADDR = None


def g_boot_target(dev):
    """The firmware's current LAUNCH selection (0=RETRO-GO,1=MARIO,2=ZELDA), read
    from the menu.c file-static over SWD -- ground truth, not an OCR guess."""
    global _GBT_ADDR
    if _GBT_ADDR is None:
        _GBT_ADDR = h.resolve_symbol("g_boot_target")   # tolerates the LTO suffix
    v = h.safe_read_u32(dev.backend, _GBT_ADDR)
    return None if v is None else (v & 0xFF)


def ensure_selector(dev):
    """Reach the main menu (OCR-confirmed) and put the cursor on the LAUNCH row."""
    h.wake(dev)
    for _ in range(5):
        if ocrnav.shot(dev).has("GNW CHAINLOADER"):
            break
        dev.button_press([ri.BTN_B]); h.settle(0.25)
    h.navigate_to(dev, MM_BOOT, MAIN_ITEMS)
    h.settle(0.3)


def select_target(dev, label, max_steps=2 * BT_COUNT) -> bool:
    """RIGHT-cycle the LAUNCH selector until g_boot_target is the wanted target.
    next_bootable skips non-bootable targets, so one this unit can't boot never
    becomes current and this returns False."""
    want = TARGET_ID[label]
    for _ in range(max_steps + 1):
        if g_boot_target(dev) == want:
            return True
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.4)
    return False


def offered_targets(dev) -> list:
    """Cycle a couple of laps and report which target ids the selector lands on
    (exactly the bootable set on this unit)."""
    seen = []
    for _ in range(2 * BT_COUNT):
        lbl = ID_LABEL.get(g_boot_target(dev))
        if lbl and lbl not in seen:
            seen.append(lbl)
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.3)
    return seen


def screen(be):
    img, _ = device.read_framebuffer(be)
    return ocr.Screen(np.asarray(img.convert("RGB")))


def verify_retrogo(t, dev):
    """A on < RETRO-GO > resets into Retro-Go; confirm Retro-Go (not the menu) runs."""
    if not select_target(dev, "RETRO-GO"):
        t.note("RETRO-GO: not selectable -- skipping"); return
    dev.button_press([ri.BTN_A])
    t.note("A on < RETRO-GO >: board_request_jump sets BOOT magic + resets; SWD drops.")
    time.sleep(1.6)                                  # reset + stub hand-off to Retro-Go
    for _ in range(5):                               # the reset killed openocd; reopen
        try:
            dev.reconnect(); break
        except Exception:
            time.sleep(0.4)
    be = dev.backend
    try:
        be.halt()
        pc = be.read_register("pc") & ~1
        on_menu = screen(be).has("GNW CHAINLOADER")
        be.resume()
    except Exception as e:
        t.check(False, f"RETRO-GO: could not sample the core after the jump ({e})"); return
    t.check((not on_menu) and (pc >> 24) != CHAINLOADER_REGION,
            f"RETRO-GO launched (PC=0x{pc:08X}, chainloader header "
            f"{'shown' if on_menu else 'gone'})")
    # Restore: clear both magic cells and reset back to the chainloader menu.
    try:
        be.halt()
        be.write_uint32(RG_MAGIC_ADDR, 0)
        be.write_uint32(SRAM_MAGIC_ADDR, 0)
        be.reset_and_halt(); be.resume()
    except Exception:
        pass
    time.sleep(2.0)


def verify_ofw(t, dev, label):
    """A on < label > flashes-if-needed + swaps + boots the OFW; Left+Game escapes."""
    vec = OFW_RESET_VEC[label]
    if not select_target(dev, label):
        t.note(f"{label}: not selectable on this unit -- skipping"); return
    on_screen = ocrnav.shot(dev).has(MATCH[label])
    t.note(f"{label}: selected (g_boot_target={TARGET_ID[label]}; label on screen: {on_screen})")
    bank2_before = h.safe_read_u32(dev.backend, BANK2_RESET_VEC_ADDR)
    t.note(f"{label}: bank2 reset vector before = "
           + (f"0x{bank2_before:08X}" if bank2_before is not None else "(unreadable)"))

    dev.button_press([ri.BTN_A])
    t.note(f"A on < {label} >: flash-if-needed + bank swap + reset; SWD session drops.")
    ofw_vecs = set(OFW_RESET_VEC.values())
    running = h.wait_u32(dev, 0x08000004, lambda v: v in ofw_vecs, timeout=80)
    booted = running == vec
    t.check(booted, f"{label} OFW running after the swap (0x08000004 = "
            + ("(unreadable)" if running is None else f"0x{running:08X}")
            + ("" if booted else f"; expected 0x{vec:08X}") + ")")
    try:
        h.capture_frame(dev.backend, f"build/boot_sel_{label.lower()}.png")
    except Exception:
        pass

    # Escape whatever OFW is running -- the wanted one, or the wrong one if it
    # mis-booted -- so the device always returns to the chainloader and the next
    # case starts clean. A wrong-OFW boot must never strand the banks swapped.
    if running not in ofw_vecs:
        t.note("no OFW confirmed running; nothing to escape.")
        return
    with dev.button_press([ri.BTN_LEFT, ri.BTN_GAME], hold=True):
        time.sleep(0.8)
    t.note("Injected Left+Game; the OFW patch should FRCE-reset + unswap into the chainloader.")
    time.sleep(3.0)
    sel_addr = h.resolve_symbol("g_list_main") + h.UI_LIST_SELECTED_OFFSET
    sel = h.wait_u32(dev, sel_addr, lambda v: 0 <= v < MAIN_ITEMS, timeout=30)
    t.check(sel is not None and 0 <= sel < MAIN_ITEMS,
            f"Left+Game from the running OFW returned to the chainloader menu (selection={sel})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="all",
                    choices=["all", "retrogo", "mario", "zelda"])
    args = ap.parse_args()
    want = {"all": ["RETRO-GO", "MARIO", "ZELDA"], "retrogo": ["RETRO-GO"],
            "mario": ["MARIO"], "zelda": ["ZELDA"]}[args.target]

    t = h.TestRun("boot selector -> each target")
    with ri.session() as dev:
        sel = h.read_menu_selection(dev.backend)
        t.require(0 <= sel < MAIN_ITEMS,
                  f"on chainloader main menu (selection {sel}) -- REMOTE_INPUT build?")

        ensure_selector(dev)
        offered = offered_targets(dev)
        t.note("selector offers: " + (", ".join(offered) if offered else "(none?!)"))

        for label in want:
            if label not in offered:
                t.note(f"{label}: not offered on this unit -- skipping")
                continue
            ensure_selector(dev)
            if label == "RETRO-GO":
                verify_retrogo(t, dev)
            else:
                verify_ofw(t, dev, label)

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
