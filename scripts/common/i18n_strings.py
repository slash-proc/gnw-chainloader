#!/usr/bin/env python3
"""Shared helpers for the i18n device tests: read the device's active language
code over SWD, and load any language's UI strings — so a test can OCR-navigate /
validate by the text the device is actually showing, in any language."""
import json
import re
from pathlib import Path

from common import harness as h

REPO = Path(__file__).resolve().parents[2]


def english_strings() -> dict:
    """{STR_id: English text} parsed from the in-core table (ui/strings.c)."""
    txt = (REPO / "src" / "chainloader" / "ui" / "strings.c").read_text()
    out = {}
    for m in re.finditer(r"\[(STR_[A-Z0-9_]+)\]\s*=\s*\"((?:[^\"\\]|\\.)*)\"", txt):
        out[m.group(1)] = m.group(2).encode().decode("unicode_escape")
    return out


def strings_for(code: str) -> dict:
    """{STR_id: text} for locale `code` — its translations over English fallback."""
    s = english_strings()
    d = REPO / "i18n" / "lang" / code / "strings.json"
    if code != "en_US" and d.exists():
        s.update({k: v for k, v in json.loads(d.read_text()).items() if v})
    return s


def active_code(backend) -> str:
    """The device's active language code, read from g_current / g_langs over SWD.
    g_langs entries are { char code[16]; char endonym[32]; char script[16]; }."""
    cur = h.read_u32_symbol(backend, "g_current") & 0xFF
    addr = h.resolve_symbol("g_langs")
    raw = bytes(backend.read_memory(addr + cur * 64, 16))
    return raw.split(b"\x00")[0].decode("ascii", "replace") or "en_US"


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
    with the broader non-Latin OCR work. Replaces active_code(): g_current/g_langs
    moved into the PIE language module and are no longer SWD-readable from the core.
    Pass wake=False to detect on the current frame without re-waking (e.g. inside a
    cycle loop).
    """
    from common import harness as h
    from common import ocrnav
    if wake:
        h.wake(dev)
        h.settle(0.3)
    sc = ocrnav.shot(dev)

    # Primary: the unique endonym on the Language row. Endonyms don't collide, so
    # the first one over a confident threshold is conclusive -- early-exit rather
    # than scoring all 16 every call (that is far too slow inside set_language's
    # cycle loop, which calls this ~20 times). Skip non-Latin endonyms: they don't
    # template-match yet and their large CJK/Cyrillic/Greek masks are the slowest to
    # scan, so excluding them keeps each call cheap.
    for e in langs_meta():
        endo = e["endonym"]
        if _is_latin(endo) and sc.has(endo, thresh=0.72):
            return e["code"], strings_for(e["code"])

    # Fallback (a screen without the Language row): the best-scoring -- not the
    # first-matching -- Latin Settings title. Best-match avoids the German/Dutch
    # title collision ("Einstellungen" vs "Instellingen"); non-Latin titles are
    # skipped for the same speed reason as the endonyms.
    best_code, best_score = None, 0.0
    codes = ["en_US"] + sorted(p.name for p in (REPO / "i18n" / "lang").iterdir()
                               if (p / "strings.json").is_file())
    for code in codes:
        lbl = strings_for(code).get("STR_TITLE_SETTINGS", "").strip()
        if not lbl or not _is_latin(lbl):
            continue
        loc = sc.locate(lbl, thresh=0.62)
        if loc and loc[2] > best_score:
            best_code, best_score = code, loc[2]
    return (best_code, strings_for(best_code)) if best_code else ("en_US", strings_for("en_US"))
