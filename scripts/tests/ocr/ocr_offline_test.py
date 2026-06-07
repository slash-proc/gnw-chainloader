#!/usr/bin/env python3
"""Offline OCR oracle — diagnose and harden ocr.py WITHOUT a device.

The suite asserts on-screen text via `ocr.Screen.locate(expected_string)`, which
renders our own glyph bitmaps and template-matches them — robust by construction
for any script. This test proves that capability per script with zero hardware,
so ocr.py can be hardened by re-running it, not by re-flashing.

Three checks per representative string (real language endonyms from
i18n/lang/langs.json — no hard-coded translation map):

  1. glyph presence  — every codepoint has a real glyph, not the '?' fallback.
     A failure here means the script font is missing (a clean build wiped
     build/i18n/fonts; run `make i18n`).  Gating.
  2. synthetic locate — render the string onto a blank frame in a chosen colour,
     then locate() it. This is exactly the capability the suite relies on. Gating.
  3. read round-trip  — render a row, then read_rows() it back and compare. This
     is the discriminating diagnostic for the non-Latin greedy matcher (does
     Cyrillic 'Р' come back as Latin 'P'?). Reported, not gating: the suite uses
     locate, not blind reading, but a low score here is the hardening target.

If a captured corpus exists under scripts/tests/ocr/corpus/<lang>/<screen>.png
(+ .json with `expect`), those device-realistic frames are checked too.

  python3 scripts/tests/ocr/ocr_offline_test.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))   # scripts/ on path
from common import ocr                       # noqa: E402
from common import i18n_strings as i18n      # noqa: E402
from common.harness import TestRun           # noqa: E402

TEST_META = dict(tier="L0", subsystem="ocr", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[10])

CORPUS = Path(__file__).resolve().parent / "corpus"

# A few ASCII UI labels for the Latin baseline (the device's own menu strings).
ASCII_LABELS = ["SETTINGS", "LAUNCH", "TOOLS", "POWER OFF", "Language"]


def synth_frame(font, s, fg=(228, 228, 236), bg=(16, 18, 28),
                x=24, y=60, W=320, H=240):
    """Draw `s` with the device's exact glyphs onto a blank frame; return
    (frame_rgb, (x, y_drawline))."""
    mask, top = ocr.render_text(font, s)
    frame = np.empty((H, W, 3), np.uint8)
    frame[:] = bg
    mh, mw = mask.shape
    if mh == 0 or mw == 0 or x + mw >= W or y + mh >= H:
        return frame, (x, y)
    region = frame[y:y + mh, x:x + mw]
    region[mask] = fg
    return frame, (x, y - top)        # y of the text baseline-row top, for locate hint


def glyphs_present(font, s):
    """(ok, missing) — every codepoint has a non-'?' glyph."""
    q = font.glyphs.get(ord("?"))
    missing = []
    for ch in s:
        g = font.glyphs.get(ord(ch))
        if g is None or (q is not None and g is q):
            missing.append(ch)
    return (not missing), missing


def char_recovery(expected, got):
    """Fraction of expected chars that appear in the read-back string (order-free,
    a forgiving similarity for the read round-trip diagnostic)."""
    if not expected:
        return 1.0
    got_chars = list(got)
    hit = 0
    for ch in expected:
        if ch in got_chars:
            got_chars.remove(ch)
            hit += 1
    return hit / len(expected)


# Scripts the device SHAPES (joins/reorders) before drawing, so the raw endonym
# codepoints are not the glyphs actually rendered. OCR of these uses the
# distinct-ink-run proxy, not glyph matching, so a "missing glyph" here is an
# expected limitation rather than a failure.
SHAPED_SCRIPTS = {"arabic"}


def check_string(t, font, label, s, *, script=None, do_read=False):
    """Glyph presence + synthetic locate (gating for non-shaped scripts).
    Optionally the read round-trip (slow with the full CJK set, so opt-in)."""
    ok_g, missing = glyphs_present(font, s)
    if not ok_g and script in SHAPED_SCRIPTS:
        t.note(f"{label}: base codepoints not drawable ({''.join(missing)!r}); the "
               f"device shapes {script} to presentation forms — OCR uses the "
               f"distinct-ink-run proxy here (known limitation, not a failure)")
        return dict(glyphs=False, located=None, score=None, recovery=None, shaped=True)
    t.check(ok_g, f"{label}: all glyphs present"
            + (f" (missing {''.join(missing)!r})" if missing else ""))
    frame, (hx, hy) = synth_frame(font, s)
    sc = ocr.Screen(frame, fg=np.array([228, 228, 236]))
    loc = sc.locate(s, y_hint=hy)
    score = loc[2] if loc else -1.0
    t.check(loc is not None,
            f"{label}: synthetic locate found (score {score:.2f})")
    rec = None
    if do_read:
        rows = sc.read_rows()
        got = rows[0][1] if rows else ""
        rec = char_recovery(s, got)
        t.note(f"{label}: read-roundtrip {rec*100:3.0f}%  got={got!r}")
    return dict(glyphs=ok_g, located=loc is not None, score=score, recovery=rec)


def run_synthetic(t, roundtrip=False):
    if not ocr.ensure_fonts():
        t.note("script fonts unavailable even after `make i18n`; non-Latin checks "
               "will fail — Latin baseline only.")
    font = ocr.Font.load()
    by_script = {}
    seen_scripts = set()

    # Latin baseline from real UI labels.
    for lbl in ASCII_LABELS:
        r = check_string(t, font, f"latin:{lbl}", lbl, script="latin",
                         do_read=roundtrip and "latin" not in seen_scripts)
        seen_scripts.add("latin")
        by_script.setdefault("latin", []).append(r)

    # Every language endonym, grouped by its declared script. The read round-trip
    # (when enabled) runs only ONCE per script — it is minutes-slow with the full
    # CJK glyph set, and one sample per script is enough to spot the greedy-matcher
    # lookalike bug (Cyrillic Р -> Latin P).
    for meta in i18n.langs_meta():
        endo = meta.get("endonym", "")
        script = meta.get("script", "?")
        if not endo:
            continue
        do_read = roundtrip and script not in seen_scripts
        seen_scripts.add(script)
        r = check_string(t, font, f"{script}:{meta['code']}={endo}", endo,
                         script=script, do_read=do_read)
        by_script.setdefault(script, []).append(r)

    # Per-script rollup (the hardening scoreboard).
    print("\n  --- per-script OCR scoreboard (synthetic) ---")
    for script in sorted(by_script):
        rs = by_script[script]
        loc = sum(1 for r in rs if r["located"])
        gly = sum(1 for r in rs if r["glyphs"])
        recs = [r["recovery"] for r in rs if r["recovery"] is not None]
        rt = f"read avg {sum(recs)/len(recs)*100:3.0f}%" if recs else "read n/a"
        print(f"    {script:10} glyphs {gly}/{len(rs)}  locate {loc}/{len(rs)}  {rt}")


def run_corpus(t):
    """Check any captured device frames (scripts/tests/ocr/corpus/<lang>/*.png)."""
    if not CORPUS.is_dir():
        t.note("no captured corpus yet (scripts/tests/ocr/corpus/); run "
               "capture_corpus.py on the device to add device-realistic frames.")
        return
    pngs = sorted(CORPUS.glob("*/*.png"))
    if not pngs:
        t.note("corpus directory present but empty.")
        return
    from PIL import Image
    for png in pngs:
        meta_path = png.with_suffix(".json")
        if not meta_path.is_file():
            continue
        meta = json.loads(meta_path.read_text())
        frame = np.asarray(Image.open(png).convert("RGB"))
        exp = meta.get("expect", [])
        tag = f"{meta.get('code', meta.get('lang', '?'))}/{meta.get('screen', '?')}"
        # Latin-family frames: read the rows with a restricted glyph set (fast) and
        # substring-check -- the discriminative matcher proven on real device frames.
        # A CJK frame falls back to the sequential per-glyph find().
        latin = all(all(ord(c) < 0x600 for c in s) for s in exp)
        sc = ocr.Screen(frame, max_cp=0x600 if latin else None)
        for expect in exp:
            ok = sc.contains(expect) if latin else (sc.find(expect) is not None)
            t.check(ok, f"corpus {tag}: read {expect!r}")


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Offline OCR oracle.")
    ap.add_argument("--roundtrip", action="store_true",
                    help="also run the slow read_rows round-trip (one sample per "
                         "script) — the non-Latin greedy-matcher diagnostic")
    args = ap.parse_args()
    t = TestRun("ocr offline (synthetic + corpus)")
    run_synthetic(t, roundtrip=args.roundtrip)
    run_corpus(t)
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
