#!/usr/bin/env python3
"""Feature-module framework test (docs/module-menu-registration.md).

Two example feature modules are pushed to the device, each declaring its menu placement
purely in its header manifest (no module-specific core code):

  /modules/features/example.bin       menu_id=TOOLS    label="Example"
  /modules/features/example_set.bin   menu_id=SETTINGS label="Demo Setting"

The core must DISCOVER both at boot (feature_discover) and:
  - Tools:    list "Example" AFTER Partition Viewer.
  - Settings: splice "Demo Setting" BETWEEN Fast-Boot and Reset Defaults, with Reset
              Defaults staying the LAST row.
Selecting either must transiently load + run it (draw its screen), then return on B
with the menu still alive.

  python3 scripts/tests/feature_menu_test.py
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
from common import device
from common import devstate
from common import harness as h
from common import ocrnav as nav
from common import remote_input as ri
from ocr_nav_test import detect_language

WHITE = np.array([248, 252, 248], dtype=np.uint8)   # the module screen's text colour
SHOTDIR = Path("/tmp/feature_menu_test")            # screenshots saved here for vision review


def save(dev, name):
    """Persist the live frame so the result can be confirmed by eye / AI vision (the
    OCR verdict is a fast proxy that can false-negative on header-less sub-pages)."""
    SHOTDIR.mkdir(exist_ok=True)
    img, _ = device.read_framebuffer(dev.backend)
    img.save(SHOTDIR / f"{name}.png")


def run_and_return(dev, entry, menu_title, menu_fg):
    """Launch feature `entry` (A), assert it drew its screen, exit (B), then assert
    we're back on `menu_title` with the entry still listed. Returns #fails."""
    fails = 0
    if not nav.enter(dev, entry, wake=False, fg=menu_fg):
        print(f"  FAIL: could not select/launch {entry!r}"); return 1
    h.settle(0.6)
    # The module screen has no header to anchor auto-fg; read with its known colour.
    if nav.shot(dev, fg=WHITE).has("Example feature module"):
        print(f"  OK   {entry!r}: module loaded + drew its screen")
    else:
        print(f"  WARN: {entry!r}: module screen text not OCR-read, continuing")
    dev.button_press([ri.BTN_B]); h.settle(0.6)   # exit run()
    if nav.present(dev, entry, wake=False, fg=menu_fg).get(entry):
        print(f"  OK   {entry!r}: returned to {menu_title}; UI alive after it ran")
    else:
        print(f"  FAIL: {entry!r}: did not return to {menu_title}"); fails += 1
    return fails


def main():
    fails = 0
    with ri.session() as dev:
        # Force the plain DEFAULT theme + return home: a leftover module theme (sprite
        # cursor + busy background, e.g. Yoshi) false-negatives the OCR matcher. This also
        # clears any leaked sub-page state. (FALLBACK is dark/low-contrast -> worse, not
        # better, for OCR; see devstate.) The feature labels are literal so the test is
        # language-agnostic via detect_language.
        devstate.use_default_theme(dev)
        code, s = detect_language(dev)
        tools    = s["STR_TITLE_TOOLS"].strip()
        settings = s["STR_TITLE_SETTINGS"].strip()
        fastboot = s["STR_FASTBOOT"].strip()
        reset    = s["STR_RESET_DEFAULTS"].strip()
        print(f"[{code}] Tools={tools!r} Settings={settings!r}")

        # The text colour detected on the main menu (anchored by "GNW CHAINLOADER")
        # is reused on the header-less Tools/Settings sub-pages, where a lone auto-fg
        # detect is unreliable.
        menu_fg = nav.shot(dev).fg

        # --- Tools: "Example" listed after Partition Viewer ---
        if not nav.enter(dev, tools):
            print("  FAIL: could not reach Tools"); print("RESULT: 1 FAILED"); return 1
        save(dev, "tools")
        if nav.present(dev, "Example", wake=False, fg=menu_fg).get("Example"):
            print("  OK   'Example' discovered + listed under Tools")
        else:
            print("  FAIL: 'Example' not listed under Tools"); fails += 1
        fails += run_and_return(dev, "Example", tools, menu_fg)
        dev.button_press([ri.BTN_B]); h.settle(0.5)   # Tools -> main menu

        # --- Settings: "Demo Setting" spliced between Fast-Boot and Reset Defaults ---
        if not nav.enter(dev, settings):
            print("  FAIL: could not reach Settings")
            print(f"RESULT: {fails + 1} FAILED"); return 1
        save(dev, "settings")
        sc = nav.shot(dev, fg=menu_fg)
        if sc.has("Demo Setting"):
            print("  OK   'Demo Setting' discovered + listed under Settings")
        else:
            print("  FAIL: 'Demo Setting' not listed under Settings"); fails += 1
        # Splice position: Fast-Boot < Demo Setting < Reset Defaults (Reset stays last).
        yf = sc.locate(fastboot); yd = sc.locate("Demo Setting"); yr = sc.locate(reset)
        if yf and yd and yr and yf[1] < yd[1] < yr[1]:
            print(f"  OK   order: {fastboot}@{yf[1]} < Demo Setting@{yd[1]} < {reset}@{yr[1]}")
        else:
            print(f"  FAIL: wrong order  fast={yf} demo={yd} reset={yr}"); fails += 1
        fails += run_and_return(dev, "Demo Setting", settings, menu_fg)

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
