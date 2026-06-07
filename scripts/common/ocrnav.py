#!/usr/bin/env python3
"""
OCR-driven menu navigation for device tests.

Reads the screen with the font-aware recogniser (common/ocr.py) and drives the
list cursor onto a target by its on-screen TEXT — in ANY UI language, locally,
with zero dependence on a vision model. Built on two robust primitives:

  - Screen.locate(text)     find a known string (renders our exact glyphs, then a
                            binary template match against the detected ink) — works
                            for English, German, Chinese, ... identically;
  - Screen.selected_row()   the cursor row (the theme sprite left of the margin).

The text-colour detection anchors on the always-ASCII "GNW CHAINLOADER" header, so
start navigation from the main menu (the colour is reused for sub-pages).

API:
  shot(dev, fg=None)            -> ocr.Screen of the current (live) frame
  navigate(dev, target, ...)    -> bool   drive the cursor onto `target`
  present(dev, *needles)        -> dict   {needle: bool} on the woken screen
  enter(dev, target, ...)       -> bool   navigate to target, press A
"""
from __future__ import annotations

import os

import numpy as np

_DEBUG = bool(os.environ.get("OCRNAV_DEBUG"))

from common import device
from common import harness as h
from common import ocr
from common import remote_input as ri


def shot(dev, fg=None) -> ocr.Screen:
    img, _ = device.read_framebuffer(dev.backend)
    return ocr.Screen(np.asarray(img.convert("RGB")), fg=fg)


def navigate(dev, target, max_steps=16, settle=0.28, wake=True) -> bool:
    """Drive the current list's cursor onto the row showing `target`. Returns True
    once the cursor row matches the target row (within a couple px)."""
    if wake:
        h.wake(dev)
        h.settle(0.3)
    fg = None
    for _ in range(max_steps):
        sc = shot(dev, fg=fg)
        if sc.fg is not None:
            fg = sc.fg                       # reuse the detected colour on sub-pages
        hit = sc.locate(target)
        cur = sc.selected_row()
        if _DEBUG:
            print(f"    nav {target!r}: fg={None if sc.fg is None else sc.fg.astype(int).tolist()} "
                  f"hit={hit} cur={cur}")
        # locate() returns the text top; selected_row() the band top — they differ
        # by a few px on the same row, so allow up to ~8 (well under the ~20px row
        # pitch, so it can't match an adjacent row).
        if hit is not None and cur is not None and abs(hit[1] - cur) <= 8:
            return True
        # not there yet: if the target isn't visible or we have no cursor, step
        # down; otherwise move toward the target row.
        down = hit is None or cur is None or cur < hit[1]
        dev.button_press([ri.BTN_DOWN] if down else [ri.BTN_UP])
        h.settle(settle)
    return False


def present(dev, *needles, wake=True) -> dict:
    """{needle: is-it-on-screen} for the current (optionally woken) frame."""
    if wake:
        h.wake(dev)
        h.settle(0.3)
    sc = shot(dev)
    return {n: sc.has(n) for n in needles}


def enter(dev, target, **kw) -> bool:
    """Navigate to `target` and press A. False if the target was never reached."""
    if not navigate(dev, target, **kw):
        return False
    dev.button_press([ri.BTN_A])
    h.settle(0.4)
    return True
