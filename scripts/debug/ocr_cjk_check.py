#!/usr/bin/env python3
"""Check whether a CJK frame can be read with a glyph set restricted to a
language's own label codepoints (the fast path: read_rows over the full 31k-glyph
CJK font is minutes-slow, but a few dozen candidate glyphs is quick, and CJK
segments cleanly so the greedy reader works).

  python3 scripts/debug/ocr_cjk_check.py build/cjk_ja_main.png ja_JP
"""
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import ocr
from common import i18n_strings as i18n

KEYS = ["STR_LAUNCH", "STR_TITLE_TOOLS", "STR_TITLE_SETTINGS", "STR_POWER_OFF",
        "STR_LANGUAGE", "STR_THEME"]


def main():
    png, code = Path(sys.argv[1]), sys.argv[2]
    frame = np.asarray(Image.open(png).convert("RGB"))
    s = i18n.strings_for(code)
    labels = [l for l in (s.get(k, "").strip() for k in KEYS) if l]

    # Candidate glyph set = ASCII + only the codepoints in this language's labels.
    ranges = [(0x20, 0x7F)] + [(ord(c), ord(c)) for l in labels for c in l if ord(c) > 0x7F]
    t0 = time.time()
    sc = ocr.Screen(frame, cp_ranges=ranges)
    rows = sc.read_rows()
    dt = time.time() - t0
    ng = sum(len(v) for v in sc._by_w.values())
    print(f"{png.name} ({code}): read in {dt:.2f}s with {ng} candidate glyphs")
    for y, txt in rows:
        print(f"   y={y:3d}  {txt!r}")
    joined = " ".join(t for _, t in rows)
    print("substring check (label -> on screen?):")
    for l in labels:
        print(f"   {'HIT ' if l in joined else 'miss'}  {l!r}")


if __name__ == "__main__":
    main()
