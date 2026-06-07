#!/usr/bin/env python3
"""font_sheet.py: render a codepoint range from a Fusion Pixel OTF into a labelled
PNG contact sheet, so PUA logos / d-pad / button glyphs (which have no Unicode names)
can be identified by eye.

Glyphs are rasterized at the font's native pixel ppem and thresholded to 1bpp (exactly
what the device draws), then scaled up nearest-neighbour for visibility, tiled into a
grid, and labelled with their codepoint.

Usage:
  python3 scripts/debug/font_sheet.py <font.otf> <out.png> [start_hex] [end_hex] [scale] [ppem]
  defaults: start=E000 end=E0FF scale=5 ppem=12
"""
import sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    path, out = sys.argv[1], sys.argv[2]
    start = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xE000
    end   = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xE0FF
    scale = int(sys.argv[5]) if len(sys.argv) > 5 else 5
    ppem  = int(sys.argv[6]) if len(sys.argv) > 6 else 12

    tt = TTFont(path, lazy=True)
    have = set(tt.getBestCmap().keys())
    tt.close()
    cps = [cp for cp in range(start, end + 1) if cp in have]
    if not cps:
        print(f"no glyphs in U+{start:04X}..U+{end:04X}"); return 1

    font = ImageFont.truetype(path, ppem)
    cell = ppem + 2                      # native render box
    gw, gh = cell * scale, cell * scale  # scaled glyph box
    labelh = 10
    cw, ch = gw + 6, gh + labelh + 4      # full cell incl. label
    cols = 16
    rows = (len(cps) + cols - 1) // cols
    img = Image.new("RGB", (cols * cw, rows * ch), (40, 44, 52))
    draw = ImageDraw.Draw(img)
    label_font = ImageFont.load_default()

    for i, cp in enumerate(cps):
        cx, cy = (i % cols) * cw, (i // cols) * ch
        # render glyph at native ppem on a 1bpp-ish mono canvas, then scale up
        g = Image.new("L", (cell, cell), 0)
        ImageDraw.Draw(g).text((1, 0), chr(cp), fill=255, font=font)
        g = g.point(lambda v: 255 if v >= 128 else 0)      # threshold to 1bpp
        g = g.resize((gw, gh), Image.NEAREST)
        img.paste(Image.merge("RGB", (g, g, g)), (cx + 3, cy + 2))
        draw.rectangle([cx + 2, cy + 1, cx + 2 + gw + 1, cy + 1 + gh + 1], outline=(90, 96, 110))
        draw.text((cx + 3, cy + gh + 3), f"{cp:04X}", fill=(170, 200, 160), font=label_font)

    img.save(out)
    print(f"saved {out}: {len(cps)} glyphs U+{start:04X}..U+{end:04X}, {cols}x{rows} grid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
