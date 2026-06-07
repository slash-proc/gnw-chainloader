#!/usr/bin/env python3
"""i18n language carousel — drive Settings -> LANGUAGE and screenshot every step.

Enters Settings, moves the cursor to the LANGUAGE value-selector, then presses
RIGHT N times, capturing the screen after each cycle. Each shot shows the whole
Settings UI re-rendered in the newly selected language (translated labels +
endonym in its own script), so it visually verifies live language switching,
the .lang packs, and the per-script .fnt fonts (accented Latin + CJK).

Run with the device on the main menu (REMOTE_INPUT build, i18n assets deployed):
  python3 scripts/tests/i18n_switch.py [--steps 10]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import numpy as np

from common import remote_input as ri
from common import harness as h
from common import device
from common import ocr
from common import i18n_strings as i18n

OUT = Path(__file__).resolve().parents[2] / "build" / "i18n_test"
MAIN_ITEMS = 5
MM_SETTINGS = 3
SET_LANGUAGE, SET_COUNT = 1, 4   # Settings list: THEME=0, LANGUAGE=1, FAST-BOOT=2, RESET=3


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=10)
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    with ri.session() as dev:
        def grab(name):
            img, _ = device.read_framebuffer(dev.backend)
            p = OUT / f"lang_{name}.png"
            img.save(p)
            print(f"  captured {p.name}")

        h.wake(dev)
        # Back out to a known main menu, then into Settings.
        for _ in range(3):
            dev.button_press([ri.BTN_B]); h.settle(0.2)
        h.wake(dev)
        h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle(0.4)                          # enter Settings
        # Closed-loop to the LANGUAGE row (was a blind DOWN that desynced if the
        # cursor wasn't on THEME — it would toggle FAST-BOOT instead).
        h.navigate_to(dev, SET_LANGUAGE, SET_COUNT, "g_list_settings")

        fails = 0
        for i in range(args.steps):
            img, _ = device.read_framebuffer(dev.backend)
            img.save(OUT / f"lang_{i:02d}.png")
            # OCR-detect which language is live from the ASCII "(code)" suffix the
            # Language selector renders ("< English (en_US) >", "< فارسی (fa_IR) >").
            # Being ASCII it matches even when the endonym is Arabic/CJK, and it replaces
            # the old g_current SWD read, which broke once the language state moved into
            # the PIE module (the symbol is no longer in the core ELF).
            code, strings = i18n.detect_language(dev, wake=False)
            lbl = (strings.get("STR_LANGUAGE", "") or "").strip()
            # The LANGUAGE label text only template-matches for Latin scripts; for
            # ar / fa_IR / CJK a confident code detection is itself the render proof.
            latin = bool(lbl) and all(ord(c) <= 0x024F for c in lbl)
            ok = bool(code) and (not latin or ocr.Screen(np.asarray(img.convert("RGB"))).has(lbl))
            print(f"  [{i:02d}] {str(code):8} LANGUAGE={lbl!r:14} render={'OK' if ok else 'MISSING'}")
            fails += 0 if ok else 1
            dev.button_press([ri.BTN_RIGHT]); h.settle(0.5)                  # next language

        grab("settings_final")
        for _ in range(3):
            dev.button_press([ri.BTN_B]); h.settle(0.2)
        grab("main_final")

    print(f"\nwrote carousel screenshots to {OUT}")
    print("RESULT:", "ALL RENDERED" if fails == 0 else f"{fails}/{args.steps} MISSING")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
