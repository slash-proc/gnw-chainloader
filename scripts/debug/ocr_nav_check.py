#!/usr/bin/env python3
"""Offline validation of the hardened ocrnav row matching against the captured
corpus frames (no device). For each frame, the expected labels must find_row()
(the row whose READ text contains them), and not-on-screen negatives must not --
the discrimination the rigid template match lacked.

  python3 scripts/debug/ocr_nav_check.py
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import ocr        # noqa: E402
from common import ocrnav     # noqa: E402

CORPUS = Path(__file__).resolve().parents[1] / "tests" / "ocr" / "corpus"
NEGATIVES = ["Nintendo", "Zelda", "Sprache", "XYZQWK"]   # not on the main menu


def main():
    frames = sorted(CORPUS.glob("*/*.png"))
    if not frames:
        print("no corpus frames; run capture_corpus.py first")
        return 1
    npass = nfail = 0
    for png in frames:
        meta = json.loads(png.with_suffix(".json").read_text())
        frame = np.asarray(Image.open(png).convert("RGB"))
        print(f"--- {meta.get('code')}/{meta.get('screen')} ---")
        for label in meta.get("expect", []):
            sc = ocrnav._nav_screen(frame, [label])
            row = sc.find_row(label)
            ok = row is not None
            print(f"  [{'PASS' if ok else 'FAIL'}] find_row({label!r}) -> y={row}")
            npass += ok; nfail += (not ok)
        # Negatives: a label that's genuinely not on THIS screen must not match.
        for neg in NEGATIVES:
            if neg in meta.get("expect", []):
                continue
            sc = ocrnav._nav_screen(frame, [neg])
            row = sc.find_row(neg)
            ok = row is None
            print(f"  [{'PASS' if ok else 'FAIL'}] negative {neg!r} not found -> y={row}")
            npass += ok; nfail += (not ok)

    # row_match anchor unit cases (the ^X$ exact match that avoids 'Demo Setting').
    print("--- row_match anchors ---")
    cases = [
        ("Settings", "^Settings$", True), ("Demo Setting", "^Settings$", False),
        ("Demo Setting", "Setting", True), ("Demo Setting", "^Demo", True),
        ("Theme: Default", "^Theme", True), ("Theme: Default", "^Theme$", False),
        ("Sprache", "^Sprache$", True),
    ]
    for text, pat, want in cases:
        got = ocr.row_match(text, pat)
        ok = got == want
        print(f"  [{'PASS' if ok else 'FAIL'}] row_match({text!r}, {pat!r}) = {got} (want {want})")
        npass += ok; nfail += (not ok)

    print(f"\n{npass} passed, {nfail} failed")
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
