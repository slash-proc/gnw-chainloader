#!/usr/bin/env python3
"""
OCR-driven navigation + i18n render test — drives the menu with the font-aware
recogniser (common/ocr.py + ocrnav.py), in whatever UI language is active, with no
vision model and no hard-coded button counts.

The active language is detected by OCR (render each candidate language's SETTINGS
label and see which matches the live main menu) instead of reading g_current /
g_langs over SWD — those moved into the PIE language module and are no longer in
the core app.elf. Then: assert the boot selector shows the language's launch verb
(STR_LAUNCH), navigate -> SETTINGS, assert the LANGUAGE row, back out, assert a
main-menu item. Run it, switch language, run it again: the same code passes for
en / de / ja / ...

  python3 scripts/tests/ocr_nav_test.py
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common import ocrnav
from common import remote_input as ri

REPO = Path(__file__).resolve().parents[2]


def english_strings():
    txt = (REPO / "src" / "chainloader" / "ui" / "strings.c").read_text()
    out = {}
    for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", txt):
        out[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return out


def strings_for(code):
    # strings.c holds only the ALL-CAPS fallback; the real UI text (incl. en_US, which
    # has its own Title-Case override) lives in each pack's strings.json. Always layer
    # the override when present, so the rendered template matches the live screen.
    s = english_strings()
    d = REPO / "i18n" / "lang" / code / "strings.json"
    if d.exists():
        s.update({k: v for k, v in json.loads(d.read_text()).items() if v})
    return s


def detect_language(dev):
    """OCR-detect the active UI language: render each candidate's SETTINGS label
    and see which one matches the live main menu. (g_current / g_langs are in the
    language module now, not the core, so we cannot read them over SWD.)"""
    h.wake(dev)
    h.settle(0.3)
    sc = ocrnav.shot(dev)
    codes = ["en_US"] + sorted(p.name for p in (REPO / "i18n" / "lang").iterdir()
                               if (p / "strings.json").is_file())
    for code in codes:
        label = strings_for(code).get("STR_TITLE_SETTINGS", "").strip()
        if label and sc.has(label):
            return code, strings_for(code)
    return "en_US", strings_for("en_US")


def main():
    fails = 0
    with ri.session() as dev:
        code, s = detect_language(dev)
        launch = s.get("STR_LAUNCH", "").strip()
        settings = s["STR_TITLE_SETTINGS"].strip()
        language = s["STR_LANGUAGE"].strip()
        tools = s["STR_TITLE_TOOLS"].strip()
        print(f"[{code}]  LAUNCH={launch!r}  SETTINGS={settings!r}  "
              f"LANGUAGE={language!r}  TOOLS={tools!r}")

        # The boot selector shows the (translated) launch verb on the main menu.
        if launch and ocrnav.present(dev, launch, wake=False).get(launch):
            print(f"  PASS  boot selector shows the launch verb {launch!r}")
        else:
            print(f"  FAIL  boot selector missing the launch verb {launch!r}")
            fails += 1

        if ocrnav.enter(dev, settings):
            print(f"  PASS  navigated to {settings!r} + entered")
        else:
            print(f"  FAIL  could not reach {settings!r}")
            fails += 1
        if ocrnav.present(dev, language, wake=False).get(language):
            print(f"  PASS  Settings page shows {language!r}")
        else:
            print(f"  FAIL  Settings page missing {language!r}")
            fails += 1
        dev.button_press([ri.BTN_B])
        h.settle(0.4)
        if ocrnav.present(dev, tools).get(tools):
            print(f"  PASS  back on main menu, {tools!r} visible")
        else:
            print(f"  FAIL  main menu missing {tools!r}")
            fails += 1

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
