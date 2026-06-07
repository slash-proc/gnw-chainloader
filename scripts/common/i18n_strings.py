#!/usr/bin/env python3
"""Shared helpers for the i18n device tests: OCR-detect the device's active
language (see detect_language) and load any language's UI strings — so a test can
OCR-navigate / validate by the text the device is actually showing, in any language.

Detection is by OCR, never by SWD symbol read: the active-language state lives in the
PIE language module, not the core, so the old g_current / g_langs symbols are not in the
core ELF. detect_language matches the ASCII "(code)" suffix the Language selector renders,
which template-matches even when the endonym is Arabic / CJK."""
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def english_strings() -> dict:
    """{STR_id: English text} parsed from the in-core table (ui/strings.c)."""
    txt = (REPO / "src" / "chainloader" / "ui" / "strings.c").read_text()
    out = {}
    for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", txt):
        out[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return out


def strings_for(code: str) -> dict:
    """{STR_id: text} for locale `code` — its translations over English fallback.
    en_US/en_UK are real packs now (mixed-case English), so load any code that has a
    strings.json; the reserved "en" sentinel has no dir and stays the baked English."""
    s = english_strings()
    d = REPO / "i18n" / "lang" / code / "strings.json"
    if d.exists():
        s.update({k: v for k, v in json.loads(d.read_text()).items() if v})
    return s


def label(code: str, str_id: str) -> str:
    return strings_for(code).get(str_id, "").strip()


def langs_meta() -> list:
    """[{code, english, endonym, script}, ...] from i18n/lang/langs.json."""
    return json.loads((REPO / "i18n" / "lang" / "langs.json").read_text())


def _is_latin(s: str) -> bool:
    """True if every char is in the Latin range (Basic + Latin-1 + Extended-A/B);
    excludes CJK, Cyrillic, and Greek, which don't OCR-template-match yet."""
    return all(ord(c) <= 0x024F for c in s)


def detect_language(dev, wake: bool = True):
    """OCR-detect the active UI language and return (code, strings).

    The primary signal is the language ENDONYM shown on the Settings -> Language
    row ("< Deutsch >", "< Nederlands >"). Endonyms are distinct words, so -- unlike
    the near-identical Settings TITLES (German "Einstellungen" vs Dutch
    "Instellingen", which both clear the per-glyph threshold against each other and
    then mis-resolve to whichever code sorts first) -- there is no cross-language
    false positive. Every candidate is *scored* and the best confident match wins,
    not the first one over the line (the old first-match title scan reported German
    for a Dutch screen).

    Falls back to the best-scoring Settings title for screens that don't show the
    Language row. Non-Latin endonyms/titles (CJK, Cyrillic, Greek) don't yet
    template-match, so those languages are detected only weakly if at all -- tracked
    with the broader non-Latin OCR work. Detection is by OCR, not a symbol read:
    g_current / g_langs moved into the PIE language module and are no longer
    SWD-readable from the core. Pass wake=False to detect on the current frame
    without re-waking (e.g. inside a cycle loop).
    """
    from common import harness as h
    from common import ocrnav
    if wake:
        h.wake(dev)
        h.settle(0.3)
    sc = ocrnav.shot(dev)

    # Primary: the ASCII "(code)" suffix the Language selector now renders next to the
    # endonym ("< English (en_US) >", "< 中文 (zh_CN) >"). Being ASCII it template-
    # matches even for non-Latin scripts whose glyphs don't, and it cleanly separates
    # en_US from en_UK (identical "English" endonyms). The reserved "en" sentinel only
    # shows when no English pack is installed.
    for code in [e["code"] for e in langs_meta()] + ["en"]:
        if sc.has("(" + code + ")", thresh=0.72):
            return code, strings_for(code)

    # Secondary: the unique Latin endonym, for a Language row whose suffix didn't OCR.
    # Endonyms don't collide across distinct languages (en_US/en_UK do, but the suffix
    # above already separated them). Skip non-Latin endonyms (they don't match yet).
    for e in langs_meta():
        endo = e["endonym"]
        if _is_latin(endo) and sc.has(endo, thresh=0.72):
            return e["code"], strings_for(e["code"])

    # Fallback (a screen without the Language row): the best-scoring -- not the
    # first-matching -- Latin Settings title. Best-match avoids the German/Dutch
    # title collision ("Einstellungen" vs "Instellingen"); non-Latin titles are
    # skipped for the same speed reason as the endonyms. Candidates are the real
    # i18n/lang/* dirs plus the "en" sentinel, de-duplicated (en_US has a dir now).
    best_code, best_score = None, 0.0
    codes = sorted({"en"} | {p.name for p in (REPO / "i18n" / "lang").iterdir()
                             if (p / "strings.json").is_file()})
    for code in codes:
        lbl = strings_for(code).get("STR_TITLE_SETTINGS", "").strip()
        if not lbl or not _is_latin(lbl):
            continue
        loc = sc.locate(lbl, thresh=0.62)
        if loc and loc[2] > best_score:
            best_code, best_score = code, loc[2]
    return (best_code, strings_for(best_code)) if best_code else ("en_US", strings_for("en_US"))
