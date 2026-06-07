#!/usr/bin/env python3
"""Capture a labelled OCR reference corpus from the device (the labelling step).

For each language and canonical screen it forces the OCR-friendly DEFAULT theme,
captures the framebuffer, and writes:

    scripts/tests/ocr/corpus/<code>/<screen>.png
    scripts/tests/ocr/corpus/<code>/<screen>.json   # {code, screen, theme, expect[...]}

`expect` is auto-derived from the device's own labels (i18n_strings.strings_for),
so it is never a hand-typed translation. ocr_offline_test.py then asserts every
`expect` string locate()s in its frame — device-realistic regression coverage and
the hardening target for ocr.py, all replayable with no hardware.

Run on the device (REMOTE_INPUT build). By default captures the CURRENTLY active
language; `--langs it_IT,de_DE` cycles the live Language selector to each.

  python3 scripts/tests/ocr/capture_corpus.py
  python3 scripts/tests/ocr/capture_corpus.py --langs it_IT,de_DE,ja_JP
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri        # noqa: E402
from common import harness as h               # noqa: E402
from common import ocrnav                     # noqa: E402
from common import devstate                   # noqa: E402
from common import i18n_strings as i18n       # noqa: E402

CORPUS = Path(__file__).resolve().parent / "corpus"

# Canonical screens -> the STR_IDs we expect rendered there (filtered to those
# that actually exist for the language before writing).
SCREENS = {
    "main":     ["STR_LAUNCH", "STR_TITLE_TOOLS", "STR_TITLE_SETTINGS", "STR_POWER_OFF"],
    "settings": ["STR_TITLE_SETTINGS", "STR_LANGUAGE", "STR_THEME", "STR_FASTBOOT"],
    "tools":    ["STR_TITLE_TOOLS"],
}


def _git_rev():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return "?"


# Unified main menu order (menu.c): LAUNCH=0, TOOLS=1, SETTINGS=2, POWER=3.
MM_TOOLS, MM_SETTINGS, MAIN_ITEMS = 1, 2, 4


def goto(dev, screen, strings):
    """Navigate to a canonical screen, SWD-driven (no OCR, so it doesn't depend on
    the very thing we're capturing to test). Return True on success."""
    if screen == "main":
        return h.go_home(dev)
    if screen == "settings":
        devstate.open_settings(dev)     # go_home + navigate_to(SETTINGS) + A, all SWD
        return True
    if screen == "tools":
        if not h.go_home(dev):
            return False
        h.navigate_to(dev, MM_TOOLS, MAIN_ITEMS)
        dev.button_press([ri.BTN_A]); h.settle(0.4)
        return True
    return False


def set_language(dev, code) -> bool:
    """Cycle the live Language selector until detect_language reports `code`."""
    devstate.open_settings(dev)
    label = i18n.strings_for(code).get("STR_LANGUAGE", "Language")
    ocrnav.navigate(dev, label)
    for _ in range(2 * len(i18n.langs_meta()) + 2):
        cur, _ = i18n.detect_language(dev, wake=False)
        if cur == code:
            return True
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.3)
    return False


def capture_one(dev, code):
    strings = i18n.strings_for(code)
    out_dir = CORPUS / code
    out_dir.mkdir(parents=True, exist_ok=True)
    saved = 0
    for screen, ids in SCREENS.items():
        if not goto(dev, screen, strings):
            print(f"  {code}/{screen}: could not navigate, skipping")
            continue
        png = out_dir / f"{screen}.png"
        try:
            h.capture_frame(dev.backend, png)
        except Exception as e:
            print(f"  {code}/{screen}: frame capture failed ({e})")
            continue
        expect = [strings[i] for i in ids if strings.get(i)]
        meta = dict(code=code, screen=screen, theme="DEFAULT",
                    expect=expect, fw_git=_git_rev())
        png.with_suffix(".json").write_text(json.dumps(meta, ensure_ascii=False, indent=2))
        print(f"  saved {png.relative_to(CORPUS.parent)}  expect={expect}")
        saved += 1
    return saved


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture the OCR reference corpus.")
    ap.add_argument("--langs", help="comma list of codes to capture (default: current)")
    args = ap.parse_args()

    codes = args.langs.split(",") if args.langs else None
    total = 0
    with ri.session() as dev:
        ok, detail = h.chainloader_running(dev.backend)
        if not ok:
            print(f"chainloader not live ({detail}); flash a REMOTE_INPUT build first")
            return 1
        devstate.use_default_theme(dev)         # DEFAULT theme is the most OCR-legible
        if codes is None:
            cur, _ = i18n.detect_language(dev)
            print(f"capturing current language: {cur}")
            total += capture_one(dev, cur)
        else:
            for code in codes:
                if set_language(dev, code):
                    print(f"language set to {code}")
                    total += capture_one(dev, code)
                else:
                    print(f"could not set language to {code}; skipping")
    print(f"\ncaptured {total} frame(s) into {CORPUS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
