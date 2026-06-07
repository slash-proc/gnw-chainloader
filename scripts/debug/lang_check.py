#!/usr/bin/env python3
"""Drive Settings -> LANGUAGE and cycle the selector, capturing each step to
build/lang_<n>.png — verifies the language module loaded + discovered packs +
renders non-English (the i18n-as-module path). Uses the core list-cursor symbol
(g_list_settings) for closed-loop nav; the i18n state now lives in the module.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import device

OUT = Path(__file__).resolve().parents[2] / "build"
MAIN_ITEMS, MM_SETTINGS = 5, 3
SET_LANGUAGE, SET_COUNT = 1, 4

with ri.session() as dev:
    h.wake(dev)
    for _ in range(3):
        dev.button_press([ri.BTN_B]); h.settle(0.2)
    h.wake(dev)
    h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
    dev.button_press([ri.BTN_A]); h.settle(0.4)               # enter Settings
    h.navigate_to(dev, SET_LANGUAGE, SET_COUNT, "g_list_settings")
    for i in range(5):
        img, _ = device.read_framebuffer(dev.backend)
        img.save(OUT / f"lang_{i}.png")
        print(f"saved lang_{i}.png")
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.6)       # cycle language
