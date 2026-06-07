#!/usr/bin/env python3
"""
cook_font.py — extract Fusion Pixel 10px glyphs from the OTF source into the
formats the chainloader consumes.

The device never rasterizes: this build-time tool reads the OTF (i18n/fonts/
fusion-pixel-10px-proportional-<script>.otf), rasterizes each glyph at its
native 10px grid with PIL/freetype, thresholds to 1bpp, and emits either:

  --probe            ASCII-art preview + width/metrics report (calibration aid)
  --ascii            generated C source for the in-core printable-ASCII font
                     (src/chainloader/ui/gui_font.{c,h}) — the always-present
                     emergency-English fallback
  --blob <script>    build/i18n/fonts/<script>.fnt — sorted codepoint index +
                     packed 1bpp bitmaps, read live from LittleFS on device

Fusion Pixel is a true pixel font: at 10 ppem the outline lands exactly on the
pixel grid, so thresholding is lossless (no anti-aliasing guesswork).
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

# .fnt blob magic: bytes 'F','N','T','1' (little-endian u32).
FNT_MAGIC = 0x31544E46

# Default coverage for the shared "latin" script blob (everything beyond the
# in-core ASCII): Latin-1 Supplement + Latin Extended-A, Greek, and Cyrillic, plus
# a few General Punctuation marks (curly quotes, en/em dash, ellipsis). The Fusion
# "latin" face covers all of these scripts, so European + Greek + Russian/Ukrainian
# share one blob; the cooker drops any codepoint the OTF happens to lack.
LATIN_COVERAGE = (list(range(0xA1, 0x180)) +       # Latin-1 Supplement + Extended-A
                  list(range(0x0180, 0x0250)) +    # Latin Extended-B (Romanian ș/ț, etc.)
                  list(range(0x0370, 0x0400)) +    # Greek and Coptic
                  list(range(0x0400, 0x0500)) +    # Cyrillic
                  [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026])

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(REPO, "i18n", "fonts")

# Font variant to cook from, set per-invocation via --style. The leading integer
# is the native pixel size (the ppem to rasterize at). e.g. "12px-monospaced"
# or "10px-proportional" -> i18n/fonts/fusion-pixel-<style>-<script>.otf.
STYLE = "12px-monospaced"
PPEM = 12

# Printable ASCII for the in-core fallback font.
ASCII_LO, ASCII_HI = 0x20, 0x7E


def set_style(style):
    global STYLE, PPEM
    STYLE = style
    PPEM = int("".join(c for c in style.split("px")[0] if c.isdigit()))


def font_path(script):
    return os.path.join(FONT_DIR, f"fusion-pixel-{STYLE}-{script}.otf")


def load(script):
    p = font_path(script)
    if not os.path.exists(p):
        sys.exit(f"missing font: {p}")
    return ImageFont.truetype(p, PPEM)


def raster_glyph(font, ascent, cellH, ch):
    """Render one character at the native grid. Returns (advance, rows[]) where
    each row is an int bitmask, bit (msb-first) col c = leftmost pixel. The cell
    is `advance` wide x cellH tall, ink placed at its natural side bearing."""
    adv = round(font.getlength(ch))
    if adv <= 0:
        return 0, [0] * cellH
    # Render onto a canvas a bit wider than the advance so nothing clips, with
    # the pen origin at x=0 and the ascender line at y=0 (anchor 'la').
    pad = 4
    img = Image.new("L", (adv + pad, cellH), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=font, fill=255, anchor="la")
    px = img.load()
    rows = []
    for y in range(cellH):
        bits = 0
        for x in range(adv):
            if px[x, y] >= 128:
                bits |= 1 << (15 - x)  # msb-first in a 16-bit row
        rows.append(bits)
    return adv, rows


def trim_cell(glyphs, cellH):
    """Find the global vertical ink band so we store only rows that any glyph
    uses (keeps the in-core table small without per-glyph y-offsets)."""
    top, bot = cellH, 0
    for _, rows in glyphs:
        for y, r in enumerate(rows):
            if r:
                top = min(top, y)
                bot = max(bot, y)
    if bot < top:  # nothing inked
        return 0, cellH
    return top, bot - top + 1


def cmd_probe(args):
    font = load(args.script)
    ascent, descent = font.getmetrics()
    cellH = ascent + descent
    print(f"script={args.script} ppem={PPEM} ascent={ascent} descent={descent} cellH={cellH}")
    sample = args.text or "AaBbGg0123 !?@#áñü€©"
    maxw = 0
    glyphs = []
    for ch in sample:
        adv, rows = raster_glyph(font, ascent, cellH, ch)
        glyphs.append((ch, adv, rows))
        maxw = max(maxw, adv)
    top, h = trim_cell([(a, r) for _, a, r in glyphs], cellH)
    print(f"max advance={maxw}  inked band: top={top} height={h}")
    for ch, adv, rows in glyphs:
        print(f"\n'{ch}' U+{ord(ch):04X} adv={adv}")
        for r in rows:
            line = "".join("#" if (r >> (15 - x)) & 1 else "." for x in range(maxw))
            print("  " + line)


C_HEADER = """\
#ifndef UI_GUI_FONT_H
#define UI_GUI_FONT_H

#include <stdint.h>

/*
 * In-core Fusion Pixel {STYLE} ASCII font (printable 0x20-0x7E), the
 * always-present emergency-English fallback. Generated by
 * scripts/build/cook_font.py ascii from i18n/fonts/fusion-pixel-{STYLE}-latin.otf
 * (OFL-1.1). DO NOT EDIT BY HAND.
 *
 * Each glyph is `w` columns wide (advance) by GUI_FONT_H rows; rows are 8-bit,
 * MSB-first (bit 0x80 = leftmost pixel). Row 0 sits at screen `y`; full-cell row
 * of row 0 is GUI_FONT_REF_TOP (so external blobs align by drawing at a y offset
 * relative to this same reference, whatever their own vertical band).
 */

#define GUI_FONT_FIRST    0x20u
#define GUI_FONT_LAST     0x7Eu
#define GUI_FONT_H        {H}u    /* rows stored per glyph */
#define GUI_FONT_REF_TOP  {TOP}u  /* full-{CELLH}px-cell row that maps to draw-y */
#define GUI_FONT_MAX_W    {MAXW}u

typedef struct {{
    uint8_t w;                 /* advance width in pixels */
    uint8_t rows[GUI_FONT_H];  /* MSB-first, bit 0x80 = leftmost column */
}} gui_glyph_t;

extern const gui_glyph_t gui_font_ascii[GUI_FONT_LAST - GUI_FONT_FIRST + 1];

#endif // UI_GUI_FONT_H
"""


def cmd_ascii(args):
    font = load("latin")
    ascent, descent = font.getmetrics()
    cellH = ascent + descent
    chars = [chr(c) for c in range(ASCII_LO, ASCII_HI + 1)]
    glyphs = [(ch, *raster_glyph(font, ascent, cellH, ch)) for ch in chars]
    top, H = trim_cell([(a, r) for _, a, r in glyphs], cellH)
    maxw = max(a for _, a, _ in glyphs)
    assert maxw <= 8, f"ASCII glyph wider than 8px ({maxw}) — needs 2 bytes/row"

    lines = [
        "#include \"gui_font.h\"",
        "",
        "/* Generated by scripts/build/cook_font.py ascii. DO NOT EDIT BY HAND. */",
        f"const gui_glyph_t gui_font_ascii[GUI_FONT_LAST - GUI_FONT_FIRST + 1] = {{",
    ]
    for ch, adv, rows in glyphs:
        band = rows[top:top + H]
        body = ", ".join(f"0x{(r >> 8) & 0xFF:02X}" for r in band)  # high byte = cols 0..7
        disp = ch if 0x20 < ord(ch) < 0x7F else "SPC"
        # Quote the glyph in the trailing comment so a backslash glyph can't act
        # as a line continuation (-Wcomment).
        lines.append(f"    {{ {adv}, {{ {body} }} }},  // 0x{ord(ch):02X} '{disp}'")
    lines.append("};")
    c_src = "\n".join(lines) + "\n"
    h_src = C_HEADER.format(H=H, TOP=top, MAXW=maxw, STYLE=STYLE, CELLH=cellH)

    ui = os.path.join(REPO, "src", "chainloader", "ui")
    with open(os.path.join(ui, "gui_font.c"), "w") as f:
        f.write(c_src)
    with open(os.path.join(ui, "gui_font.h"), "w") as f:
        f.write(h_src)
    nbytes = len(glyphs) * (1 + H)
    print(f"wrote gui_font.{{c,h}}: {len(glyphs)} glyphs, H={H} rows, top={top}, "
          f"maxw={maxw} -> {nbytes} bytes table")


def row_bytes(r, stride):
    """Pack a 16-bit row mask (bit 15-x = column x) into `stride` MSB-first bytes."""
    return bytes(((r >> (8 * (1 - b))) & 0xFF) for b in range(stride))


def cmd_blob(args):
    """Emit build/i18n/<script>.fnt: a sorted codepoint index + packed 1bpp
    bitmaps the device reads whole into RAM (no on-device rasterizer)."""
    font = load(args.script)
    cmap = TTFont(font_path(args.script)).getBestCmap()
    ascent, descent = font.getmetrics()
    cellH = ascent + descent

    if args.chars:
        with open(args.chars, encoding="utf-8") as f:
            cps = sorted({ord(c) for c in f.read() if ord(c) >= 0x80})
    else:
        cps = LATIN_COVERAGE
    cps = [c for c in cps if c in cmap]   # only codepoints the OTF actually has

    glyphs = []
    for c in cps:
        adv, rows = raster_glyph(font, ascent, cellH, chr(c))
        if adv > 0:
            glyphs.append((c, adv, rows))
    if not glyphs:
        sys.exit("blob: no glyphs to emit")
    top, H = trim_cell([(a, r) for _, a, r in glyphs], cellH)

    gc = len(glyphs)
    cps_out, widths, offsets, bitmaps = [], [], [], bytearray()
    for c, adv, rows in glyphs:
        stride = (adv + 7) // 8
        offsets.append(len(bitmaps))
        for r in rows[top:top + H]:
            bitmaps += row_bytes(r, stride)
        cps_out.append(c)
        widths.append(adv)

    w_pad = (-gc) % 4
    bitmaps_off = 12 + 4 * gc + gc + w_pad + 4 * gc
    blob = bytearray()
    blob += struct.pack("<IHBBI", FNT_MAGIC, gc, H, top, bitmaps_off)
    blob += struct.pack(f"<{gc}I", *cps_out)
    blob += bytes(widths) + bytes(w_pad)
    blob += struct.pack(f"<{gc}I", *offsets)
    blob += bitmaps

    out = args.out or os.path.join(REPO, "build", "i18n", "fonts", f"{args.script}.fnt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out}: {gc} glyphs, cellH={H}, ref_top={top}, "
          f"{len(bitmaps)} bitmap bytes, {len(blob)} total")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--style", default="10px-proportional",
                    help="font variant: <px>px-<proportional|monospaced> "
                         "(picks i18n/fonts/fusion-pixel-<style>-<script>.otf)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe")
    p.add_argument("--script", default="latin")
    p.add_argument("--text", default=None)
    p.set_defaults(func=cmd_probe)
    a = sub.add_parser("ascii")
    a.set_defaults(func=cmd_ascii)
    b = sub.add_parser("blob")
    b.add_argument("--script", default="latin")
    b.add_argument("--chars", default=None,
                   help="UTF-8 file; its unique >=0x80 codepoints define coverage "
                        "(default: built-in European coverage)")
    b.add_argument("--out", default=None)
    b.set_defaults(func=cmd_blob)
    args = ap.parse_args()
    set_style(args.style)
    args.func(args)


if __name__ == "__main__":
    main()
