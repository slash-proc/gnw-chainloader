#!/usr/bin/env python3
"""verify_entry_translated_in_all_languages.py [STR_ID ...]

For each named Settings-menu UI string, cycle the LIVE Language selector through
every language and OCR-assert the string's translation actually renders on the
Settings page. Catches BOTH a missing translation (falls back to English) and a
missing font glyph (renders '?'), in one session with no reboots. The active
language at each step is detected by OCR (which candidate's SETTINGS title is on
screen), since g_current/g_langs moved into the PIE language module.

Defaults to a couple of always-visible Settings labels if no STR_IDs are given.

  python3 scripts/tests/verify_entry_translated_in_all_languages.py STR_FASTBOOT STR_THEME

KNOWN LIMITATION: Latin-script languages pass reliably; Greek / Cyrillic / CJK
currently fail the OCR match even though they render on the device (the glyphs ARE
loaded by ocr.py, so it is a template-match/detection weakness in the recogniser
for non-Latin scripts, not a font gap). Tracked as a separate OCR-harness task;
that fix also unblocks the CJK launch-verb check.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common.i18n_strings import strings_for
from common import ocrnav
from common import remote_input as ri

REPO = Path(__file__).resolve().parents[2]


def all_codes():
    # The SELECTABLE languages: every real pack dir (en_US/en_UK included). The hidden
    # in-core "en" sentinel is never shown once an English pack exists, so it is not a
    # cycle target. De-duplicated (en_US has a dir now, so no "en_US" + dirs prefix).
    return sorted(p.name for p in (REPO / "i18n" / "lang").iterdir()
                  if (p / "strings.json").is_file())


def detect(sc, codes):
    """Which candidate language is active on `sc` (the Language row)? Prefer the ASCII
    "(code)" suffix the selector renders -- it disambiguates en_US/en_UK (both titled
    "Settings") and matches even for non-Latin scripts -- then fall back to the title."""
    for c in codes:
        if sc.has("(" + c + ")", thresh=0.72):
            return c
    for c in codes:
        lbl = strings_for(c).get("STR_TITLE_SETTINGS", "").strip()
        if lbl and sc.has(lbl):
            return c
    return None


def main():
    targets = [a for a in sys.argv[1:] if a.startswith("STR_")] or ["STR_FASTBOOT", "STR_THEME"]
    codes = all_codes()
    t = h.TestRun("i18n-translated-all-languages")
    print(f"targets: {targets}  ({len(codes)} languages)")

    with ri.session() as dev:
        h.wake(dev)
        h.settle(0.3)
        start = detect(ocrnav.shot(dev), codes) or "en_US"
        s0 = strings_for(start)
        if not ocrnav.enter(dev, s0["STR_TITLE_SETTINGS"].strip()):
            t.note("could not enter Settings")
            return t.summary()
        if not ocrnav.navigate(dev, s0["STR_LANGUAGE"].strip(), wake=False):
            t.note("could not reach the Language row")
            return t.summary()

        # The Language row's value cycles the live UI language. Walk it once,
        # detecting the language at each step and checking the targets render.
        results = {}
        for _ in range(len(codes) + 4):
            if len(results) >= len(codes):
                break
            sc = ocrnav.shot(dev)
            # Suffix first across ALL candidates (so en_US isn't mis-read as en_UK,
            # whose "Settings" title also matches the en_US screen), then title.
            cur = next((c for c in codes if c not in results
                        and sc.has("(" + c + ")", thresh=0.72)), None)
            if cur is None:
                cur = next((c for c in codes if c not in results
                            and sc.has(strings_for(c)["STR_TITLE_SETTINGS"].strip())), None)
            if cur:
                cs = strings_for(cur)
                results[cur] = {tid: bool(cs.get(tid, "").strip()) and sc.has(cs[tid].strip())
                                for tid in targets}
            dev.button_press([ri.BTN_RIGHT])
            h.settle(0.3)

        # Restore the original language, then leave Settings (commits .active). Match
        # the start code's "(code)" suffix so en_US/en_UK restore precisely (their
        # shared "Settings" title would otherwise stop one language early).
        for _ in range(len(codes) + 1):
            if ocrnav.shot(dev).has("(" + start + ")", thresh=0.72):
                break
            dev.button_press([ri.BTN_RIGHT])
            h.settle(0.3)
        dev.button_press([ri.BTN_B])
        h.settle(0.3)

    for code in codes:
        r = results.get(code)
        for tid in targets:
            txt = strings_for(code).get(tid, "").strip()
            t.check(bool(r) and r.get(tid, False), f"{code:6} {tid} = {txt!r}")
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
