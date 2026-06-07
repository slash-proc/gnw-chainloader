#!/usr/bin/env python3
"""probe_arabic_font.py — Arabic font pre-flight for the i18n cook pipeline.

Decides which offline-shaping path the Arabic .lang/.fnt cook can use:

  * presentation-form path (simple): the device renders pre-shaped Unicode
    presentation forms, so the font's cmap MUST map Forms-B (U+FE70..U+FEFF) and
    ideally Forms-A (U+FB50..U+FDFF). arabic_reshaper.reshape() emits exactly these.
  * HarfBuzz->PUA fallback (font-agnostic): only needed if the font carries just base
    Arabic (U+0600..U+06FF) + OpenType GSUB shaping and NO presentation forms in cmap.

Reports cmap coverage of each block + the unit/grid so we can pick the cook ppem.
Read-only (fontTools only). Usage: python3 scripts/debug/probe_arabic_font.py [path]
"""
import os
import sys

DEFAULT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "i18n", "fonts", "unixel-regular-ar.otf")


def block(cmap, lo, hi):
    present = [c for c in range(lo, hi + 1) if c in cmap]
    return len(present), (hi - lo + 1)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    if not os.path.isfile(path):
        sys.exit(f"font not found: {path}")
    from fontTools.ttLib import TTFont
    f = TTFont(path)
    cmap = f.getBestCmap()
    upem = f["head"].unitsPerEm
    has_gsub = "GSUB" in f

    print(f"font: {path}")
    print(f"unitsPerEm: {upem}   (pixel-grid hint: a 16px design grid often has upem 16/256/2048)")
    print(f"GSUB (OpenType shaping) present: {has_gsub}")
    print(f"total cmap entries: {len(cmap)}")

    blocks = {
        "base Arabic        U+0600-06FF": (0x0600, 0x06FF),
        "Forms-B (shaped)   U+FE70-FEFF": (0xFE70, 0xFEFF),
        "Forms-A (ligs)     U+FB50-FDFF": (0xFB50, 0xFDFF),
        "ASCII              U+0020-007E": (0x0020, 0x007E),
    }
    cov = {}
    for name, (lo, hi) in blocks.items():
        n, tot = block(cmap, lo, hi)
        cov[name] = n
        print(f"  {name}: {n}/{tot} mapped")

    formsb = cov["Forms-B (shaped)   U+FE70-FEFF"]
    base = cov["base Arabic        U+0600-06FF"]
    print()
    if formsb >= 0x60:   # most of FE70..FEFF present
        print("VERDICT: presentation-form path OK -- the cmap maps the shaped Forms-B. "
              "Use arabic_reshaper.reshape()+bidi in cook_lang.py; no HarfBuzz needed.")
        return 0
    if base >= 0x40 and has_gsub:
        print("VERDICT: NO presentation forms in cmap, but base Arabic + GSUB are present. "
              "Use the HarfBuzz->PUA fallback (uharfbuzz) in the cook.")
        return 2
    print("VERDICT: font lacks both shaped forms and a usable base+GSUB -- wrong/incomplete "
          "Arabic font; reselect (Unixel / Eternal Dream).")
    return 3


if __name__ == "__main__":
    sys.exit(main())
