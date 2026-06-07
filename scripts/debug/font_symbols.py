#!/usr/bin/env python3
"""font_symbols.py: extract the non-Latin / non-CJK symbol, arrow, shape, emoji and
PUA-logo glyphs a Fusion Pixel OTF actually contains, grouped by Unicode block.

The text scripts (Latin/Greek/Cyrillic/kana/Han/Hangul) are excluded so what's left
is the "icon" set: symbols, arrows, geometric shapes, dingbats, emoji, and the
Private-Use-Area logos / d-pad / button glyphs. For PUA codepoints (no Unicode name)
the font's own glyph name is printed, which usually says what the glyph is.

Usage:
  python3 scripts/debug/font_symbols.py [font.otf ...]   # default: all i18n/fonts/fusion-pixel-*.otf
  python3 scripts/debug/font_symbols.py --names-only      # just block summary counts
"""
import os
import sys
import glob
import unicodedata
from fontTools.ttLib import TTFont

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(REPO, "i18n", "fonts")

# Text blocks to EXCLUDE (handled by the normal script fonts already).
TEXT_RANGES = [
    (0x0000, 0x007F),  # Basic Latin (ASCII letters/digits/punct)
    (0x0080, 0x024F),  # Latin-1 + Latin Extended-A/B
    (0x0250, 0x036F),  # IPA, spacing modifiers, combining diacritics
    (0x0370, 0x03FF),  # Greek
    (0x0400, 0x052F),  # Cyrillic (+ supplement)
    (0x1E00, 0x1EFF),  # Latin Extended Additional
    (0x1100, 0x11FF),  # Hangul Jamo
    (0x3000, 0x303F),  # CJK Symbols & Punctuation
    (0x3040, 0x30FF),  # Hiragana + Katakana
    (0x3100, 0x312F),  # Bopomofo
    (0x3130, 0x318F),  # Hangul Compatibility Jamo
    (0x31F0, 0x31FF),  # Katakana Phonetic Extensions
    (0x3400, 0x4DBF),  # CJK Unified Ext A
    (0x4E00, 0x9FFF),  # CJK Unified
    (0xAC00, 0xD7AF),  # Hangul Syllables
    (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
    (0xFF00, 0xFFEF),  # Halfwidth/Fullwidth forms
]

# Blocks worth labelling in the output (the icon set). Anything not listed prints as "(other)".
BLOCKS = [
    (0x2000, 0x206F, "General Punctuation"),
    (0x2070, 0x209F, "Super/Subscripts"),
    (0x20A0, 0x20CF, "Currency Symbols"),
    (0x2100, 0x214F, "Letterlike Symbols"),
    (0x2150, 0x218F, "Number Forms"),
    (0x2190, 0x21FF, "Arrows"),
    (0x2200, 0x22FF, "Mathematical Operators"),
    (0x2300, 0x23FF, "Misc Technical (media/UI icons)"),
    (0x2400, 0x243F, "Control Pictures"),
    (0x2460, 0x24FF, "Enclosed Alphanumerics"),
    (0x2500, 0x257F, "Box Drawing"),
    (0x2580, 0x259F, "Block Elements"),
    (0x25A0, 0x25FF, "Geometric Shapes"),
    (0x2600, 0x26FF, "Miscellaneous Symbols"),
    (0x2700, 0x27BF, "Dingbats"),
    (0x27C0, 0x27EF, "Misc Math Symbols-A"),
    (0x27F0, 0x27FF, "Supplemental Arrows-A"),
    (0x2900, 0x297F, "Supplemental Arrows-B"),
    (0x2B00, 0x2BFF, "Misc Symbols and Arrows"),
    (0x2E00, 0x2E7F, "Supplemental Punctuation"),
    (0xE000, 0xF8FF, "Private Use Area (logos/d-pad/buttons)"),
    (0x1F000, 0x1F0FF, "Mahjong/Domino/Cards"),
    (0x1F300, 0x1F5FF, "Misc Symbols and Pictographs (emoji)"),
    (0x1F600, 0x1F64F, "Emoticons"),
    (0x1F680, 0x1F6FF, "Transport & Map (emoji)"),
    (0x1F900, 0x1F9FF, "Supplemental Symbols & Pictographs"),
    (0x1FB00, 0x1FBFF, "Symbols for Legacy Computing"),
]


def is_text(cp):
    return any(a <= cp <= b for a, b in TEXT_RANGES)


def block_of(cp):
    for a, b, label in BLOCKS:
        if a <= cp <= b:
            return label
    return "(other)"


def uname(cp):
    try:
        return unicodedata.name(chr(cp))
    except ValueError:
        return ""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    names_only = "--names-only" in sys.argv
    fonts = args or sorted(glob.glob(os.path.join(FONT_DIR, "fusion-pixel-*.otf")))
    union = {}
    for path in fonts:
        tt = TTFont(path, lazy=True)
        cmap = tt.getBestCmap()  # {codepoint: glyphName}
        icons = {cp: gn for cp, gn in cmap.items() if not is_text(cp)}
        print(f"\n===== {os.path.basename(path)} : {len(cmap)} total cmap, "
              f"{len(icons)} non-text icon glyphs =====")
        by_block = {}
        for cp, gn in sorted(icons.items()):
            by_block.setdefault(block_of(cp), []).append((cp, gn))
            union[cp] = gn
        for block, items in by_block.items():
            print(f"  [{block}]  ({len(items)})")
            if names_only:
                continue
            for cp, gn in items:
                ch = chr(cp)
                show = ch if cp >= 0x20 and unicodedata.category(ch)[0] != "C" else " "
                print(f"    U+{cp:04X} '{show}'  glyph={gn:<24} {uname(cp)}")
        tt.close()
    print(f"\n===== UNION across fonts: {len(union)} distinct non-text icon codepoints =====")
    ub = {}
    for cp in sorted(union):
        ub.setdefault(block_of(cp), 0)
        ub[block_of(cp)] += 1
    for block, n in ub.items():
        print(f"  {n:5}  {block}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
