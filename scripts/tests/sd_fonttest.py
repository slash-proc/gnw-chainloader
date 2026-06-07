#!/usr/bin/env python3
"""File Browser test — drive into the SD card and onto the accented `testö` folder
using OCR (navigate by the on-screen name, not blind scrolling), and assert the
UTF-8 name renders via the always-on Latin base font.

  python3 scripts/tests/sd_fonttest.py [--enter]

Replaces the old open-loop "press DOWN an arbitrary number of times and hope"
scroll, which depended on the exact SD contents.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import device
from common import harness as h
from common import ocrnav
from common import remote_input as ri

OUT = Path(__file__).resolve().parents[2] / "build" / "i18n_test"
MAIN_ITEMS, MM_TOOLS = 5, 2
TARGET = "testö"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--enter", action="store_true", help="also enter the folder + shoot")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    fails = 0
    with ri.session() as dev:
        h.wake(dev)
        for _ in range(6):
            dev.button_press([ri.BTN_B]); h.settle(0.2)          # fully out to main
        h.wake(dev)
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle(0.4)              # Tools
        dev.button_press([ri.BTN_A]); h.settle(0.9)              # File Browser (FS-select)

        # OCR: pick the SD card (the FAT filesystem) and enter it.
        if ocrnav.enter(dev, "FAT", wake=False):
            print("  PASS  selected + entered the FAT (SD) filesystem")
        else:
            print("  FAIL  could not select the FAT filesystem"); fails += 1

        # OCR: drive onto the accented folder by its NAME (handles scrolling).
        if ocrnav.navigate(dev, TARGET, wake=False):
            print(f"  PASS  navigated to the {TARGET!r} folder")
        else:
            print(f"  FAIL  could not reach {TARGET!r}"); fails += 1

        sc = ocrnav.shot(dev)
        Image.fromarray(sc.frame).save(OUT / "sd_testo.png")
        if sc.has(TARGET):
            print(f"  PASS  {TARGET!r} renders (UTF-8 via the Latin base font)")
        else:
            print(f"  FAIL  {TARGET!r} does not render"); fails += 1

        if args.enter:
            dev.button_press([ri.BTN_A]); h.settle(0.9)
            img, _ = device.read_framebuffer(dev.backend)
            img.save(OUT / "sd_testo_in.png")
            print("  (entered the folder; shot saved to sd_testo_in.png)")

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
