#!/usr/bin/env python3
"""
State-aware device test — from ANY starting screen / theme / language, it:
  1. recognizes where it is (OCR) and gets to the main menu,
  2. recognizes the current theme + language (SWD symbols),
  3. in Settings: sets the FALLBACK theme, switches the UI language to German,
     then sets the DEFAULT theme,
validating each step with OCR. Nothing is a blind button count: list rows are
reached closed-loop (navigate_to on the list symbol) and the value selectors are
cycled until the live symbol (ui_theme_slot / g_current) hits the target, so it
works no matter the random state the device was left in.

  python3 scripts/tests/theme_lang_test.py

Leaves the device on German + the DEFAULT theme.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common import i18n_strings as i18n
from common import ocrnav
from common import remote_input as ri

MAIN_ITEMS, MM_SETTINGS = 4, 2
SET_THEME, SET_LANGUAGE, SET_COUNT = 0, 1, 4
THEME_DEFAULT, THEME_FALLBACK = 0, 1


def theme_slot(be):
    return h.read_u32_symbol(be, "ui_theme_slot") & 0xFF


def ensure_main(dev, tries=8):
    """Recognize the screen and back out to the main menu (OCR confirms the
    'GNW CHAINLOADER' header, which is literal in every language)."""
    h.wake(dev); h.settle(0.3)
    for _ in range(tries):
        if ocrnav.shot(dev).has("GNW CHAINLOADER"):
            return True
        dev.button_press([ri.BTN_B]); h.settle(0.25)
    return False


def open_settings(dev):
    ensure_main(dev)
    h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
    dev.button_press([ri.BTN_A]); h.settle(0.4)


def set_theme(dev, slot, name, max_cycles=10):
    """Closed-loop to the THEME row, then cycle until ui_theme_slot == slot;
    confirm the value '< NAME >' is on screen with OCR."""
    h.navigate_to(dev, SET_THEME, SET_COUNT, "g_list_settings")
    for _ in range(max_cycles + 1):
        if theme_slot(dev.backend) == slot:
            return ocrnav.shot(dev).has(f"< {name} >")
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.4)
    return False


def set_language(dev, code, endonym, max_cycles=20):
    """Closed-loop to the LANGUAGE row, then cycle until the active code == code;
    confirm the endonym (e.g. 'Deutsch') is on screen with OCR."""
    h.navigate_to(dev, SET_LANGUAGE, SET_COUNT, "g_list_settings")
    for _ in range(max_cycles + 1):
        if i18n.detect_language(dev, wake=False)[0] == code:
            return ocrnav.shot(dev).has(endonym)
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.4)
    return False


def main():
    fails = 0
    with ri.session() as dev:
        be = dev.backend
        print(f"recognized start: language={i18n.detect_language(dev)[0]!r}  theme_slot={theme_slot(be)}")

        if ensure_main(dev):
            print("  PASS  recognized + reached the main menu (from wherever it was)")
        else:
            print("  FAIL  could not reach the main menu"); fails += 1

        open_settings(dev)

        if set_theme(dev, THEME_FALLBACK, "FALLBACK"):
            print("  PASS  theme -> FALLBACK")
        else:
            print("  FAIL  theme -> FALLBACK"); fails += 1

        if set_language(dev, "de_DE", "Deutsch"):
            print("  PASS  language -> German (Deutsch)")
        else:
            print("  FAIL  language -> German"); fails += 1

        if set_theme(dev, THEME_DEFAULT, "DEFAULT"):
            print("  PASS  theme -> DEFAULT")
        else:
            print("  FAIL  theme -> DEFAULT"); fails += 1

        end_lang, end_theme = i18n.detect_language(dev)[0], theme_slot(be)
        print(f"final: language={end_lang!r}  theme_slot={end_theme}")
        if end_lang == "de_DE" and end_theme == THEME_DEFAULT:
            print("  PASS  final state: German + DEFAULT theme")
        else:
            print("  FAIL  final state wrong"); fails += 1

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
