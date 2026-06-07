#!/usr/bin/env python3
"""Input-injection + menu-navigation test (chainloader only, no flashing).

Proves the core mechanism and — crucially — the *timing*: that a `button_press`
is a clean single tap, not a multi-second hold that auto-repeats across the whole
menu. It reads the live menu selection index out of RAM and asserts each tap moves
it by exactly one.

Run after: make clean && make REMOTE_INPUT=1 -j16 && make REMOTE_INPUT=1 flash_chainloader
Then leave the device sitting on the main menu.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h

MAIN_ITEMS = 4  # LAUNCH (unified boot selector), TOOLS, SETTINGS, POWER OFF


def main() -> int:
    t = h.TestRun("remote input — timing & navigation")
    with ri.session() as dev:
        be = dev.transport.backend

        h.wake(dev)   # dismiss idle-hide so the first real tap counts
        sel0 = h.read_menu_selection(be)
        t.require(0 <= sel0 < MAIN_ITEMS,
                  f"on main menu, selection index in range (got {sel0}) — "
                  "is a REMOTE_INPUT build flashed and the menu showing?")

        # One DOWN tap moves exactly one row (this is the regression that bit us:
        # a too-long 'tap' scrolled multiple rows via firmware auto-repeat).
        dev.button_press([ri.BTN_DOWN])
        h.settle()
        sel1 = h.read_menu_selection(be)
        t.check(sel1 == (sel0 + 1) % MAIN_ITEMS,
                f"single DOWN tap moved selection by exactly 1 ({sel0} -> {sel1})")

        # One UP tap moves back.
        dev.button_press([ri.BTN_UP])
        h.settle()
        sel2 = h.read_menu_selection(be)
        t.check(sel2 == sel0, f"single UP tap returned to start ({sel1} -> {sel2})")

        # repeat=3 should advance exactly 3 (taps stay distinct, no merge).
        dev.button_press([ri.BTN_DOWN], repeat=3)
        h.settle()
        sel3 = h.read_menu_selection(be)
        t.check(sel3 == (sel0 + 3) % MAIN_ITEMS,
                f"DOWN repeat=3 advanced exactly 3 rows ({sel0} -> {sel3})")

        # Put it back where we found it.
        steps_back = (sel3 - sel0) % MAIN_ITEMS
        if steps_back:
            dev.button_press([ri.BTN_UP], repeat=steps_back)
            h.settle()
        t.check(h.read_menu_selection(be) == sel0, "restored original selection")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
