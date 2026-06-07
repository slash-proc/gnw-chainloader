#!/usr/bin/env python3
"""Shared device-state helpers for OCR-driven tests.

OCR reliability depends on the ACTIVE THEME, whose palette and cursor vary:
  - DEFAULT (slot 0): the per-OFW palette with a plain ">" text cursor. In a high-contrast
    OFW context (Zelda = green) it is the most OCR-readable theme; for Mario it is red with
    a gold ">" selector, which the matcher reads less well.
  - FALLBACK (slot 1): despite the name this is a DARK, low-contrast theme -- observed to
    BREAK OCR (the header and rows don't separate from the background). Do NOT use it as the
    OCR baseline.
  - module themes (slots 2+): a moving SPRITE cursor over a busy background (e.g. Yoshi +
    clouds) -- also fights OCR.

So tests force the plain DEFAULT theme (away from sprite/module themes) for a deterministic
baseline. The theme is set COLOUR-INDEPENDENTLY: read the core symbol `ui_theme_slot` over
SWD and cycle the selector until it matches -- no OCR in that loop, so it works from any
unreadable starting theme. List navigation here is closed-loop on the list selection symbol
(h.navigate_to), not blind tap counts.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common import ocrnav as nav
from common import remote_input as ri

THEME_DEFAULT, THEME_FALLBACK = 0, 1          # in-core theme slots (theme.h)
_MAIN_ITEMS, _MM_SETTINGS = 4, 2              # main menu: Boot/Tools/Settings/Power
_SET_THEME = 1                               # Settings rows: Language=0, Theme=1 (both
                                             # above the feature splice, so fixed)


def theme_slot(be):
    """Active theme slot from the core symbol (0=Default, 1=Fallback, 2+=module)."""
    return h.read_u32_symbol(be, "ui_theme_slot") & 0xFF


def open_settings(dev):
    """Back out to the main menu (blind B is safe there), then open Settings closed-loop
    on g_list_main. Leaves the device in the Settings page."""
    nav.go_home(dev)
    h.navigate_to(dev, _MM_SETTINGS, _MAIN_ITEMS, "g_list_main")
    dev.button_press([ri.BTN_A])
    h.settle(0.4)


def cycle_theme_to(dev, slot, max_cycles=12):
    """In the Settings page, land on the Theme row and cycle the selector until
    ui_theme_slot == slot. SWD-driven (no OCR), so independent of the current theme's
    colours. Returns True once reached."""
    h.navigate_to(dev, _SET_THEME, 8, "g_list_settings")
    for _ in range(max_cycles + 1):
        if theme_slot(dev.backend) == slot:
            return True
        dev.button_press([ri.BTN_RIGHT])
        h.settle(0.35)
    return False


def use_default_theme(dev):
    """Force the plain DEFAULT theme (no module sprite cursor / busy background), then
    return to the main menu. Call this at the START of any OCR test so a leftover module
    theme (e.g. Yoshi) can't false-negative the matcher. Returns True if it was set.

    Note: in a Mario OFW context DEFAULT is red/gold and still reads less well -- the
    fully robust path is to test in a high-contrast OFW context (Zelda) -- but this at
    least guarantees the plain, sprite-free theme."""
    open_settings(dev)
    ok = cycle_theme_to(dev, THEME_DEFAULT)
    nav.go_home(dev)
    return ok
