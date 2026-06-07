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

SWD-authoritative (ui_theme_slot + detect_language), so it is language-agnostic;
it records the starting language + theme and RESTORES them at the end.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common import i18n_strings as i18n
from common import observe
from common import remote_input as ri

MAIN_ITEMS, MM_SETTINGS = 4, 2
# Settings list order (menu.c SET_IDX_*): LANGUAGE=0, THEME=1, FASTBOOT=2, then
# spliced feature entries, RESET last. (These were previously swapped, so set_theme
# drove the Language row and vice-versa.) SET_COUNT is a fallback; _set_count reads
# the live num_items (robust to feature modules adding rows).
SET_LANGUAGE, SET_THEME, SET_COUNT = 0, 1, 4
THEME_DEFAULT, THEME_FALLBACK = 0, 1


def theme_slot(be):
    return h.read_u32_symbol(be, "ui_theme_slot") & 0xFF


def ensure_main(dev, tries=8):
    """Back out to the main menu. SWD-driven (g_stack_ptr == 0) rather than the
    rigid OCR header match, which is unreliable on dense real frames."""
    return h.go_home(dev)


def open_settings(dev):
    ensure_main(dev)
    h.navigate_to(dev, MM_SETTINGS, MAIN_ITEMS)
    dev.button_press([ri.BTN_A]); h.settle(0.4)


def _set_count(dev):
    """Live item count of the Settings list (robust to feature modules splicing in
    extra rows, which a hardcoded SET_COUNT misses)."""
    return observe.menu_count(dev.backend, "g_list_settings") or SET_COUNT


def set_theme(dev, slot, max_cycles=10):
    """Closed-loop to the THEME row, then cycle until ui_theme_slot == slot.
    SWD-authoritative (ui_theme_slot) and therefore language-agnostic: no OCR of
    the theme name, which is translated into the active language."""
    h.navigate_to(dev, SET_THEME, _set_count(dev), "g_list_settings")
    for _ in range(max_cycles + 1):
        if theme_slot(dev.backend) == slot:
            return True
        dev.button_press([ri.BTN_RIGHT]); h.settle(0.4)
    return False


def set_language(dev, code, max_cycles=22):
    """Closed-loop to the LANGUAGE row, then cycle until the active code == code.
    Uses the FAST ASCII '(code)' suffix per step (full detect_language per step is
    too slow across ~19 languages); the suffix is the selector's own ground truth."""
    h.navigate_to(dev, SET_LANGUAGE, _set_count(dev), "g_list_settings")
    for _ in range(max_cycles + 1):
        if i18n.active_code_suffix(dev) == code:    # multi-frame read, redraw-robust
            return True
        dev.button_press([ri.BTN_RIGHT])
        h.settle(0.5)          # changing language reloads a pack/font; let it render
    return False


def main():
    fails = 0
    with ri.session() as dev:
        be = dev.backend
        # Record the starting state so we can restore it (the device is shared).
        orig_lang = i18n.detect_language(dev)[0]
        orig_theme = theme_slot(be)
        print(f"recognized start: language={orig_lang!r}  theme_slot={orig_theme}")

        def step(ok, msg):
            nonlocal fails
            print(("  PASS  " if ok else "  FAIL  ") + msg)
            if not ok:
                fails += 1

        step(ensure_main(dev), "reached the main menu (from wherever it was)")
        open_settings(dev)
        step(set_theme(dev, THEME_FALLBACK), "theme -> FALLBACK (ui_theme_slot)")
        step(set_language(dev, "de_DE"), "language -> de_DE (detect_language)")
        step(set_theme(dev, THEME_DEFAULT), "theme -> DEFAULT")
        # Verify from the MAIN menu: detect_language is reliable there (Settings rows
        # read as "label: value", which defeats the exact-row language ranking).
        ensure_main(dev)
        step(i18n.detect_language(dev)[0] == "de_DE" and theme_slot(be) == THEME_DEFAULT,
             "changes applied: German + DEFAULT theme")

        # Restore the original language + theme, leaving the device as found.
        open_settings(dev)
        set_language(dev, orig_lang)
        set_theme(dev, orig_theme)
        ensure_main(dev)
        end_lang, end_theme = i18n.detect_language(dev)[0], theme_slot(be)
        print(f"restored: language={end_lang!r}  theme_slot={end_theme}")
        step(end_lang == orig_lang and end_theme == orig_theme,
             f"restored original state ({orig_lang} + theme {orig_theme})")

    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
