#!/usr/bin/env python3
"""i18n screen tour — drive the chainloader over SWD and screenshot each screen.

Visual regression aid for the internationalized UI: navigates main menu ->
Settings -> Tools -> File Browser (+ context menu) -> Partition Viewer, saving a
PNG of each into build/i18n_test/. One OpenOCD backend is shared for both input
injection (shadow cell, CPU running) and framebuffer capture (reads the displayed
LTDC buffer — tear-free thanks to double buffering), so there's no probe
contention and nothing to serialize.

Run after flashing a REMOTE_INPUT build and leaving the device on the main menu:
  python3 scripts/tests/i18n_screens.py [--lang NAME]

--lang only tags the output filenames (e.g. --lang de) so language passes don't
overwrite each other; it does not change device state.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import device

OUT = Path(__file__).resolve().parents[2] / "build" / "i18n_test"
MAIN_ITEMS = 4   # LAUNCH (boot selector), TOOLS, SETTINGS, POWER OFF
MM_TOOLS, MM_SETTINGS = 1, 2


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", default="en", help="filename tag for this pass")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    shots = []

    def grab(name: str):
        be = dev.backend
        img, info = device.read_framebuffer(be)
        p = OUT / f"{args.lang}_{name}.png"
        img.save(p)
        shots.append(p.name)
        print(f"  captured {p.name}  ({info['width']}x{info['height']} {info['format']})")

    def to_main():
        # B pops any submenu/modal; PAGE_MAIN binds no on_back, so extra B's are
        # harmless no-ops. Three guarantees we're back at the main list.
        for _ in range(3):
            dev.button_press([ri.BTN_B]); h.settle(0.2)
        h.wake(dev)

    with ri.session() as dev:
        h.wake(dev)

        print("main menu")
        grab("01_main")

        print("settings")
        h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle()
        grab("02_settings")

        print("tools")
        to_main()
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle()
        grab("03_tools")

        print("file browser")
        h.navigate_to(dev, 0, 2, "g_list_tools")       # FILE BROWSER (closed-loop)
        dev.button_press([ri.BTN_A]); h.settle(0.6)
        grab("04_browser")

        print("partition viewer")
        to_main()
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle()       # enter tools
        h.navigate_to(dev, 1, 2, "g_list_tools")       # PARTITION VIEWER (closed-loop)
        dev.button_press([ri.BTN_A]); h.settle(0.9)
        grab("05_partition")

        to_main()

    print(f"\nwrote {len(shots)} screenshots to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
