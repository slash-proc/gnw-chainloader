#!/usr/bin/env python3
"""Diagnose OCR locate scores on a captured corpus frame.

For a corpus PNG (+ its .json `expect` list), prints the best locate score for
each expected string under the auto-detected foreground and a few candidate
foreground colours, so we can see WHY a real device frame scores lower than the
synthetic ideal (wrong fg colour? threshold too high? baseline off?).

  python3 scripts/debug/ocr_corpus_diag.py scripts/tests/ocr/corpus/it_IT/main.png
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import ocr


def scores(frame, expect, fg=None):
    sc = ocr.Screen(frame, fg=fg)
    out = []
    for s in expect:
        r = sc.locate(s, thresh=0.0)
        out.append((s, r[2] if r else None))
    return (sc.fg.astype(int).tolist() if sc.fg is not None else None), out


def main():
    png = Path(sys.argv[1])
    frame = np.asarray(Image.open(png).convert("RGB"))
    meta = json.loads(png.with_suffix(".json").read_text())
    expect = meta.get("expect", [])
    print(f"{png.name}  ({meta.get('code')}/{meta.get('screen')})  expect={expect}")

    auto_fg, auto = scores(frame, expect)
    print(f"\nauto fg = {auto_fg}")
    for s, sc in auto:
        print(f"   {sc if sc is None else f'{sc:.3f}'}  {s!r}")

    # Candidate text colours seen in these themes (near-white, gold accent, green).
    for fg in ([248, 252, 248], [224, 184, 56], [40, 204, 112], [232, 232, 232]):
        _, res = scores(frame, expect, fg=np.array(fg, np.float32))
        line = "  ".join(f"{(sc if sc is not None else float('nan')):.2f}:{s[:8]}" for s, sc in res)
        print(f"fg={fg}: {line}")

    # Negative controls: strings NOT on screen should score well below the
    # correct ones, so we can size the gap and pick a safe threshold.
    print("\nnegative controls (should be low):")
    sc = ocr.Screen(frame)
    for s in ["Zelda", "XQZWVK", "Nintendo", "qqqqqqqq"]:
        r = sc.locate(s, thresh=0.0)
        print(f"   {(r[2] if r else 0):.3f}  {s!r}")

    # rigid locate vs sequential per-glyph (locate_seq): the fix should lift the
    # CORRECT strings and sink the negative controls, restoring separation.
    print("\nrigid locate vs sequential locate_seq:")
    negs = ["Zelda", "Nintendo", "qqqqqqqq", "XQZWVK"]
    print("  CORRECT (want high):")
    for s in expect:
        r = sc.locate(s, thresh=0.0)
        rs = sc.locate_seq(s, thresh=0.0)
        print(f"    rigid {(r[2] if r else 0):.2f}  seq {(rs[2] if rs else 0):.2f}  {s!r}")
    print("  NEGATIVE (want low):")
    for s in negs:
        r = sc.locate(s, thresh=0.0)
        rs = sc.locate_seq(s, thresh=0.0)
        print(f"    rigid {(r[2] if r else 0):.2f}  seq {(rs[2] if rs else 0):.2f}  {s!r}")

    # Read the actual rows with a Latin-restricted glyph set (fast) and substring-
    # check: this follows the device layout exactly, so it is strongly
    # discriminative (a not-on-screen word is simply not a substring of any row).
    import time
    t0 = time.time()
    scl = ocr.Screen(frame, max_cp=0x600)
    rows = scl.read_rows()
    print(f"\nread_rows (latin subset, {time.time()-t0:.1f}s):")
    for y, txt in rows:
        print(f"   y={y:3d}  {txt!r}")
    norm = " | ".join(t for _, t in rows).lower()
    print("  substring check:")
    for s in expect:
        print(f"    {'HIT ' if s.lower() in norm else 'miss'}  {s!r}")
    for s in negs:
        print(f"    {'HIT ' if s.lower() in norm else 'miss'}  {s!r}  (negative)")


if __name__ == "__main__":
    main()
