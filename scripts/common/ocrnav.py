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


def _frame(dev):
    img, _ = device.read_framebuffer(dev.backend)
    return np.asarray(img.convert("RGB"))


def _nav_screen(frame, targets, fg=None) -> ocr.Screen:
    """A Screen whose line reader is restricted to ASCII + the target strings' own
    codepoints. read_rows over the full 31k-glyph CJK font is minutes-slow; this
    tiny set (a few dozen glyphs) makes the read fast, and reading-then-substring
    is far more discriminative than the rigid template match (which lets a wrong
    string score a lucky partial hit). FG detection is unaffected (it anchors on
    the full-font header)."""
    extra = sorted({ord(c) for t in targets for c in t if ord(c) > 0x7F})
    ranges = [(0x20, 0x7F)] + [(cp, cp) for cp in extra]
    return ocr.Screen(frame, fg=fg, cp_ranges=ranges)


def go_home(dev, presses=5, settle=0.35) -> None:
    """Back out to the main menu from anywhere. Sub-pages are a modal window stack and a
    running feature module's run() loop both exit on B; B on the main menu itself is a
    no-op (g_list_main has no on_back), so over-pressing is harmless. Use this at the
    START of every test so navigation begins from a known screen — tests otherwise leak
    state into each other (a prior test left on Settings makes the next 'enter Tools'
    search the wrong page and false-fail)."""
    h.wake(dev)
    h.settle(0.3)
    for _ in range(presses):
        dev.button_press([ri.BTN_B])
        h.settle(settle)


def navigate(dev, target, max_steps=16, settle=0.28, wake=True, fg=None) -> bool:
    """Drive the current list's cursor onto the row showing `target`. Returns True
    once the cursor row matches the target row (within a couple px).

    Pass `fg` (the text colour detected on the main menu) when navigating a sub-page
    that lacks the "GNW CHAINLOADER" header — there a per-shot auto-fg detect is
    unreliable, and a single bad detect can stall the whole walk."""
    if wake:
        h.wake(dev)
        h.settle(0.3)
    for _ in range(max_steps):
        sc = _nav_screen(_frame(dev), [target], fg=fg)
        if fg is None and sc.fg is not None:
            fg = sc.fg                       # latch the detected colour for sub-pages
        row_y = sc.find_row(target)          # row whose READ text contains target
        cur = sc.selected_row()
        if _DEBUG:
            print(f"    nav {target!r}: fg={None if sc.fg is None else sc.fg.astype(int).tolist()} "
                  f"row={row_y} cur={cur} rows={[t for _, t in getattr(sc, '_rows_cache', [])]}")
        # Both row_y and cur are ink_rows() band tops, so the same row gives the
        # same y; allow a small slack for band fragmentation (well under the ~20px
        # row pitch, so it can't match an adjacent row).
        if row_y is not None and cur is not None and abs(row_y - cur) <= 6:
            return True
        # Not there yet: step down if the target isn't visible or we have no
        # cursor, otherwise move toward the target row.
        down = row_y is None or cur is None or cur < row_y
        dev.button_press([ri.BTN_DOWN] if down else [ri.BTN_UP])
        h.settle(settle)
    return False


def present(dev, *needles, wake=True, fg=None) -> dict:
    """{needle: is-it-on-screen} for the current (optionally woken) frame.

    Unlike navigate(), this takes a SINGLE shot, so its auto fg-detection has no
    retries to recover from. On a sub-page that lacks the "GNW CHAINLOADER" header
    (e.g. Tools/Settings), pass `fg` — the text colour detected on the main menu
    (`nav.shot(dev).fg`) — so the match doesn't depend on the lone detect."""
    if wake:
        h.wake(dev)
        h.settle(0.3)
    sc = _nav_screen(_frame(dev), list(needles), fg=fg)
    return {n: sc.contains(n) for n in needles}


def enter(dev, target, **kw) -> bool:
    """Navigate to `target` and press A. False if the target was never reached."""
    if not navigate(dev, target, **kw):
        return False
    dev.button_press([ri.BTN_A])
    h.settle(0.4)
    return True
