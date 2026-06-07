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

# Per-script overrides for a script whose face is NOT the Fusion Pixel <style> family:
# (filename, ppem, supersample). "arabic" (also used for Farsi/Persian) uses Vazirmatn
# Regular — a quality hinted Persian/Arabic OUTLINE font (SIL OFL 1.1) — at ppem 14 with
# 4x supersampling (render at 56px, then area-downscale + threshold). Supersampling keeps
# the light Regular strokes clean and consistent at 1bpp, which a "pixel" Arabic font
# can't manage; the 19px band fits the 20px menu rows and the weight matches the Latin
# face. See docs/i18n.md. Other scripts use the Fusion Pixel <style> face at SS 1.
SCRIPT_FONT = {
    # (filename, ppem, supersample, raise): `raise` lifts the glyphs up by N pixels
    # (a signed ref_top in the .fnt) so a tall-ascent outline face like Vazirmatn sits
    # on the row baseline instead of a couple pixels low.
    "arabic": ("vazirmatn-regular.ttf", 14, 4, 6),
}
PPEM_OVERRIDE = None    # --ppem override (calibration)
SS_OVERRIDE = None      # --ss override (calibration)
RAISE_OVERRIDE = None   # --raise override (calibration)
SUPERSAMPLE = 1         # effective SS for the current cook; set from ss_for() per script
_HI_CACHE = {}


def _hi_font(font, ss):
    key = (font.path, font.size, ss)
    f = _HI_CACHE.get(key)
    if f is None:
        f = ImageFont.truetype(font.path, font.size * ss)
        _HI_CACHE[key] = f
    return f

# Printable ASCII for the in-core fallback font.
ASCII_LO, ASCII_HI = 0x20, 0x7E


def set_style(style):
    global STYLE, PPEM
    STYLE = style
    PPEM = int("".join(c for c in style.split("px")[0] if c.isdigit()))


def font_path(script):
    spec = SCRIPT_FONT.get(script)
    if spec:
        return os.path.join(FONT_DIR, spec[0])
    return os.path.join(FONT_DIR, f"fusion-pixel-{STYLE}-{script}.otf")


def ppem_for(script):
    if PPEM_OVERRIDE:
        return PPEM_OVERRIDE
    spec = SCRIPT_FONT.get(script)
    return spec[1] if spec else PPEM


def ss_for(script):
    if SS_OVERRIDE:
        return SS_OVERRIDE
    spec = SCRIPT_FONT.get(script)
    return spec[2] if (spec and len(spec) >= 3) else 1


def raise_for(script):
    if RAISE_OVERRIDE is not None:
        return RAISE_OVERRIDE
    spec = SCRIPT_FONT.get(script)
    return spec[3] if (spec and len(spec) >= 4) else 0


def load(script):
    p = font_path(script)
    if not os.path.exists(p):
        sys.exit(f"missing font: {p}")
    return ImageFont.truetype(p, ppem_for(script))


def raster_glyph(font, ascent, cellH, ch):
    """Render one character at the native grid. Returns (advance, rows[]) where
    each row is an int bitmask, bit (msb-first) col c = leftmost pixel. The cell
    is `advance` wide x cellH tall, ink placed at its natural side bearing."""
    # The canvas is one extra descent taller than the nominal cell so a deep glyph bowl
    # (e.g. Arabic final noon/jeem/yeh below the font descent line) is captured, not cut
    # at the bottom edge. trim_cell() then keeps only the rows any glyph inks, so the
    # extra rows are free for scripts that don't use them (Latin/CJK unchanged).
    canvasH = cellH + (cellH - ascent)   # target rows = ascent + 2*descent
    ss = SUPERSAMPLE
    rfont = _hi_font(font, ss) if ss > 1 else font
    adv_hi = round(rfont.getlength(ch))
    if adv_hi <= 0:
        return 0, [0] * canvasH
    # Render at ppem*SS (pen origin x=0, ascender at y=0 via anchor 'la'), then area-
    # downscale to the target grid: LANCZOS averages the high-res coverage into smooth,
    # consistent stems, and the >=128 threshold turns 50%+ coverage into ink. This is the
    # upscale-then-downscale path that tames a quality outline font's small-size jaggies.
    pad = 4 * ss
    img = Image.new("L", (adv_hi + pad, canvasH * ss), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=rfont, fill=255, anchor="la")
    if ss > 1:
        img = img.resize(((adv_hi + pad) // ss, canvasH), Image.LANCZOS)
    adv = round(adv_hi / ss)
    px = img.load()
    W, H = img.size
    rows = []
    for y in range(H):
        bits = 0
        for x in range(min(adv, W)):
            if px[x, y] >= 128:
                bits |= 1 << x  # column x at bit x (LSB-first); arbitrary width, so
                                # glyphs wider than 16px (Arabic) don't overflow
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
    global SUPERSAMPLE
    SUPERSAMPLE = ss_for(args.script)
    font = load(args.script)
    ascent, descent = font.getmetrics()
    cellH = ascent + descent
    print(f"script={args.script} ppem={ppem_for(args.script)} ascent={ascent} descent={descent} cellH={cellH}")
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
            line = "".join("#" if (r >> x) & 1 else "." for x in range(maxw))
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
    global SUPERSAMPLE
    SUPERSAMPLE = ss_for("latin")
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
        body = ", ".join(f"0x{row_bytes(r, 1)[0]:02X}" for r in band)  # one byte = cols 0..7
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
    """Pack a row mask (bit x = column x, LSB-first) into `stride` MSB-first bytes
    (bit 0x80 = leftmost column in each byte), so glyphs wider than 16px (Arabic at
    its native ppem) pack correctly — the device reads ceil(w/8) bytes per row."""
    return bytes(sum((0x80 >> k) for k in range(8) if (r >> (b * 8 + k)) & 1)
                 for b in range(stride))


def cmd_blob(args):
    """Emit build/i18n/<script>.fnt: a sorted codepoint index + packed 1bpp
    bitmaps the device reads whole into RAM (no on-device rasterizer)."""
    global SUPERSAMPLE
    SUPERSAMPLE = ss_for(args.script)
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
    raise_px = raise_for(args.script)
    ref_top = (top - raise_px) & 0xFF      # signed (i8) on the device: lifts glyphs up by raise_px
    blob = bytearray()
    blob += struct.pack("<IHBBI", FNT_MAGIC, gc, H, ref_top, bitmaps_off)
    blob += struct.pack(f"<{gc}I", *cps_out)
    blob += bytes(widths) + bytes(w_pad)
    blob += struct.pack(f"<{gc}I", *offsets)
    blob += bitmaps

    out = args.out or os.path.join(REPO, "build", "i18n", "fonts", f"{args.script}.fnt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out}: {gc} glyphs, cellH={H}, ref_top={top - raise_px} (raise {raise_px}), "
          f"{len(bitmaps)} bitmap bytes, {len(blob)} total")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--style", default="10px-proportional",
                    help="font variant: <px>px-<proportional|monospaced> "
                         "(picks i18n/fonts/fusion-pixel-<style>-<script>.otf)")
    ap.add_argument("--ppem", type=int, default=None,
                    help="override the rasterization ppem (calibrate a pixel grid)")
    ap.add_argument("--ss", type=int, default=None,
                    help="override the per-script supersample factor (render at ppem*SS "
                         "then area-downscale; smooths a quality outline font's stems)")
    ap.add_argument("--raise", type=int, default=None, dest="raise_px",
                    help="override the per-script vertical raise in pixels (lift glyphs up)")
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
    global PPEM_OVERRIDE, SS_OVERRIDE, RAISE_OVERRIDE
    PPEM_OVERRIDE = args.ppem
    SS_OVERRIDE = args.ss        # None unless --ss given; ss_for() uses the per-script value
    RAISE_OVERRIDE = args.raise_px
    args.func(args)


if __name__ == "__main__":
    main()
