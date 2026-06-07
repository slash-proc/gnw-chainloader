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


def pack_endonym(code: str) -> str:
    """The endonym AS THE DEVICE DRAWS IT, read from the built .lang pack header
    (LNG2, char[32] at offset 0x2C). For RTL languages this is the PRE-SHAPED,
    visual-order form (e.g. ar = presentation forms) -- the bytes the screen
    actually renders -- which is what an OCR read sees, unlike the logical-order
    endonym in langs.json. Returns "" if the pack isn't built."""
    p = REPO / "build" / "i18n" / f"{code}.lang"
    if not p.is_file():
        return ""
    d = p.read_bytes()
    return d[0x2C:0x4C].split(b"\x00", 1)[0].decode("utf-8", "ignore")


def rtl_codes() -> list:
    """Codes whose script is RTL (Arabic-family), from langs.json."""
    return [e["code"] for e in langs_meta() if e.get("script") == "arabic"]


def _is_latin(s: str) -> bool:
    """True if every char is in the Latin range (Basic + Latin-1 + Extended-A/B);
    excludes CJK, Cyrillic, and Greek, which don't OCR-template-match yet."""
    return all(ord(c) <= 0x024F for c in s)


def active_code_suffix(dev, tries: int = 3):
    """Read the ASCII '(code)' suffix the Language selector renders on its row
    ('< Deutsch (de_DE) >'). A FAST per-step check inside a language-cycling loop,
    where a full detect_language() per step is too slow. ASCII-only read, matched
    against the known codes. Requires being ON the Language row.

    Changing the language reloads a pack/font and redraws, so a single fresh frame
    can land mid-redraw (blank / top-bar only); reads up to `tries` frames and
    returns the first valid code, else None."""
    import re
    import time
    from common import ocrnav
    from common import ocr as _ocr
    codes = {e["code"] for e in langs_meta()} | {"en"}
    for i in range(tries):
        sc = _ocr.Screen(ocrnav._frame(dev), cp_ranges=[(0x20, 0x7F)])
        text = " ".join(t for _, t in sc.read_rows())
        m = re.search(r"\(([A-Za-z_]{2,6})\)", text)
        if m:
            tok = m.group(1)
            if tok in codes:
                return tok
            low = tok.casefold()
            c = next((c for c in codes if c.casefold() == low), None)
            if c:
                return c
        if i + 1 < tries:
            time.sleep(0.25)        # transient redraw frame; try the next one
    return None


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
    from common import ocr as _ocr
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

    # Robust fallback (any screen): read the ACTUAL rendered rows with a Latin-
    # restricted glyph set (fast) and rank candidates by how many of their
    # distinctive labels appear as substrings of the read text. This follows the
    # device's own layout, so it is strongly discriminative -- it fixes the old
    # rigid-locate ranking that reported it_IT for a German screen (Italian
    # "Impostazioni" scored a lucky partial hit higher than German "Einstellungen").
    # Reading the text also separates the German/Dutch title near-collision
    # ("Einstellungen" is not a substring of "Instellingen" or vice versa).
    latin = _ocr.Screen(sc.frame, fg=sc.fg, max_cp=0x600)
    rows = [t for _, t in latin.read_rows()]
    keys = ("STR_TITLE_SETTINGS", "STR_LAUNCH", "STR_TITLE_TOOLS", "STR_POWER_OFF",
            "STR_LANGUAGE", "STR_THEME")

    def _score(labels):
        # Weight EXACT-row matches (^label$) heavily over loose substring: that is
        # what separates same-family languages whose labels share substrings. The
        # old joined-text substring count reported German for a Polish screen off a
        # lucky partial hit; an exact "Ustawienia" row beats a German substring.
        exact = sum(1 for l in labels if any(_ocr.row_match(r, "^" + l + "$") for r in rows))
        sub = sum(1 for l in labels if any(_ocr.row_match(r, l) for r in rows))
        return exact * 10 + sub

    best_code, best_score = None, 0
    # Real language dirs first, the baked-English "en" sentinel LAST, so on a tie
    # (English screen: en/en_US/en_UK share identical labels) a real installed code
    # wins rather than the sentinel ('>' keeps the first to reach the max).
    codes = sorted(p.name for p in (REPO / "i18n" / "lang").iterdir()
                   if (p / "strings.json").is_file()) + ["en"]
    for code in codes:
        s = strings_for(code)
        labels = [l for l in (s.get(k, "").strip() for k in keys) if l and _is_latin(l)]
        sc_score = _score(labels)
        if sc_score > best_score:
            best_code, best_score = code, sc_score
    if best_code and best_score > 0:
        return best_code, strings_for(best_code)

    # CJK recognition: read with a glyph set restricted to the CANDIDATE label
    # codepoints (a few dozen glyphs, so read_rows is fast even though the full CJK
    # font has 31k), then rank by substring matches. Works where Arabic can't
    # because CJK characters are discrete blocks (no cursive joining) and the
    # layout is LTR. Validated on a real ja_JP frame (起動/ツール/電源オフ read).
    def _is_cjk(c):
        o = ord(c)
        return (0x2E80 <= o <= 0x9FFF or 0xAC00 <= o <= 0xD7AF
                or 0x3000 <= o <= 0x30FF or 0xFF00 <= o <= 0xFFEF)
    cand = {}
    cjk_cps = set()
    for code in codes:
        s = strings_for(code)
        labels = [l for l in (s.get(k, "").strip() for k in keys)
                  if l and any(_is_cjk(c) for c in l)]
        if labels:
            cand[code] = labels
            cjk_cps.update(ord(c) for l in labels for c in l if _is_cjk(c))
    if cjk_cps:
        ranges = [(0x20, 0x7F)] + [(cp, cp) for cp in cjk_cps]
        crows = [t for _, t in _ocr.Screen(sc.frame, fg=sc.fg,
                                           cp_ranges=ranges).read_rows()]
        bc, bn = None, 0
        for code, labels in cand.items():
            exact = sum(1 for l in labels if any(_ocr.row_match(r, "^" + l + "$") for r in crows))
            sub = sum(1 for l in labels if any(_ocr.row_match(r, l) for r in crows))
            n = exact * 10 + sub
            if n > bn:
                bc, bn = code, n
        if bc and bn >= 1:
            return bc, strings_for(bc)

    # RTL recognition (enough not to get tripped up when the carousel cycles
    # through ar/fa): the layout is mirrored and the text is pre-shaped, so the
    # Latin pass above misses it. Read with the Arabic glyph set and look for each
    # RTL language's PRE-SHAPED endonym (the exact visual-order bytes the device
    # draws, from the built pack). Recovery from RTL is deterministic elsewhere
    # (provision.set_active_language rewrites /i18n/.active), so this only needs to
    # RECOGNIZE, not navigate.
    rtl = rtl_codes()
    if rtl:
        ar_text = " ".join(t for _, t in
                           _ocr.Screen(sc.frame, fg=sc.fg, cp_ranges=_ocr.CPS_ARABIC)
                           .read_rows())
        for code in rtl:
            endo = pack_endonym(code)
            if endo and endo in ar_text:
                return code, strings_for(code)
        # Couldn't pin ar vs fa, but Arabic ink is clearly present -> report the
        # first RTL code rather than a wrong LTR guess (the caller can recover).
        if any(0xFB50 <= ord(c) <= 0xFEFF for c in ar_text):
            return rtl[0], strings_for(rtl[0])
    return ("en_US", strings_for("en_US"))
