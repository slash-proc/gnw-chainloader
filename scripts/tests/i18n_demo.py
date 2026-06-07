#!/usr/bin/env python3
"""SD->LittleFS install demo driver: select a language by cycling the LANGUAGE
selector RIGHT a given number of steps, leave it active (persisted), and shoot.

  python3 scripts/tests/i18n_demo.py --rights N

Used to land on the Polish demo language (whose pack lives only on the SD), so a
reboot then triggers the langsync install prompt.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import device
from common import i18n_strings as i18n

OUT = Path(__file__).resolve().parents[2] / "build" / "i18n_test"
MAIN_ITEMS, MM_SETTINGS = 4, 2
SET_LANGUAGE, SET_COUNT = 1, 4   # Settings list: THEME=0, LANGUAGE=1, FAST-BOOT=2, RESET=3


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rights", type=int, default=0,
                    help="cycle the LANGUAGE selector RIGHT this many steps")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    with ri.session() as dev:
        def grab(name):
            img, _ = device.read_framebuffer(dev.backend)
            p = OUT / f"demo_{name}.png"
            img.save(p)
            print(f"  captured {p.name}")

        h.wake(dev)
        for _ in range(3):
            dev.button_press([ri.BTN_B]); h.settle(0.2)
        h.wake(dev)
        h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle(0.4)      # enter Settings
        # Closed-loop to the LANGUAGE row (replaces the old blind --downs, which
        # desynced when the Settings cursor wasn't where it assumed and toggled
        # FAST-BOOT instead).
        h.navigate_to(dev, SET_LANGUAGE, SET_COUNT, "g_list_settings")
        for _ in range(args.rights):
            dev.button_press([ri.BTN_RIGHT]); h.settle(0.45)
        code, _ = i18n.detect_language(dev, wake=False)
        print(f"  active language = {code}")
        grab("lang")
        for _ in range(3):
            dev.button_press([ri.BTN_B]); h.settle(0.2)
        grab("main")

    print(f"\nwrote demo screenshots to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
