#!/usr/bin/env python3
"""glyph_select.py: page through Fusion Pixel icon glyphs and curate which to bake
into latin.fnt (and expose to the {TOKEN} text replacer).

Shows a PAGE of glyphs at once as half-block ASCII art, each numbered with its
page-local index + codepoint. Per page you type assignments, then advance:

    <idx> <name>    keep glyph #idx on this page, tagged NAME   (e.g.  5 BTN_A)
    <idx>           keep glyph #idx, name it later (blank)
    n  or  <Enter>  next page
    p               previous page
    q               quit and save

You can enter several "<idx> <name>" lines on a page before moving on. Output
(default i18n/glyphs_selected.txt) is 'U+XXXX  NAME' lines the font build consumes;
glyphs already kept are skipped on a re-run.

RUN IN A REAL TERMINAL (it reads input line by line):
    python3 scripts/build/glyph_select.py

Options:
    --out FILE    answer file (default i18n/glyphs_selected.txt)
    --font OTF    source font (default the latin Fusion Pixel OTF)
    --per N       glyphs per page (default 16, laid out 4 across)
    --range A-B   candidate codepoint range(s), hex, repeatable
                  (default = PUA icon set: controllers E000-E2FF + logos F8EC-F8FF)
"""
import argparse
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(REPO, "i18n", "fonts")
DEFAULT_FONT = os.path.join(FONT_DIR, "fusion-pixel-12px-monospaced-latin.otf")
DEFAULT_OUT = os.path.join(REPO, "i18n", "glyphs_selected.txt")
PPEM = 12
CELL_W = 16   # glyph render/display box width in columns
COLS = 4      # glyphs across per terminal row


def cell_art(font, cp):
    """Render one glyph as half-block rows (2 vertical pixels per char), CELL_W wide."""
    box = 16
    img = Image.new("L", (CELL_W, box), 0)
    ImageDraw.Draw(img).text((1, 0), chr(cp), fill=255, font=font)
    px = img.load()
    on = [[px[x, y] >= 128 for x in range(CELL_W)] for y in range(box)]
    ys = [y for y in range(box) if any(on[y])]
    if not ys:
        return [" " * CELL_W]
    y0, y1 = min(ys), max(ys)
    rows, y = [], y0
    while y <= y1:
        top = on[y]
        bot = on[y + 1] if y + 1 < box else [False] * CELL_W
        rows.append("".join("█" if (top[x] and bot[x]) else
                            "▀" if top[x] else
                            "▄" if bot[x] else " " for x in range(CELL_W)))
        y += 2
    return rows


def show_page(font, page):
    """page: list of (local_idx, cp). Print in rows of COLS."""
    pad = CELL_W + 2
    for r in range(0, len(page), COLS):
        chunk = page[r:r + COLS]
        print("".join(f"{li:>2}.U+{cp:04X}".ljust(pad) for li, cp in chunk))
        arts = [cell_art(font, cp) for _, cp in chunk]
        h = max(len(a) for a in arts)
        for a in arts:
            a += [" " * CELL_W] * (h - len(a))
        for y in range(h):
            print("".join(arts[c][y].ljust(pad) for c in range(len(chunk))))
        print()


def load_kept(path):
    kept = set()
    if os.path.isfile(path):
        for ln in open(path, encoding="utf-8"):
            ln = ln.split("#", 1)[0].strip()
            if not ln:
                continue
            try:
                kept.add(int(ln.replace("U+", "").split()[0], 16))
            except (ValueError, IndexError):
                pass
    return kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--per", type=int, default=16)
    ap.add_argument("--range", action="append", default=[])
    args = ap.parse_args()

    tt = TTFont(args.font, lazy=True)
    have = set(tt.getBestCmap())
    tt.close()
    if args.range:
        cand = []
        for rg in args.range:
            a, b = (int(x, 16) for x in rg.split("-"))
            cand += range(a, b + 1)
    else:
        cand = list(range(0xE000, 0xE2FF + 1)) + list(range(0xF8EC, 0xF8FF + 1))

    kept = load_kept(args.out)
    cand = [cp for cp in cand if cp in have and cp not in kept]
    if not os.path.isfile(args.out) or os.path.getsize(args.out) == 0:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("# selected glyphs: 'U+XXXX  TOKEN_NAME'  (consumed by the font build)\n")
    if not cand:
        print("nothing left to review (all candidates already kept).")
        return 0

    font = ImageFont.truetype(args.font, PPEM)
    per = max(COLS, args.per)
    npages = math.ceil(len(cand) / per)
    print(f"{len(cand)} glyphs to review, {npages} pages of {per} "
          f"({len(kept)} already kept).")
    print("Assign with '<idx> <name>' (e.g. 5 BTN_A). n/Enter=next, p=prev, q=quit.\n")

    out = open(args.out, "a", encoding="utf-8")
    pi = 0
    try:
        while 0 <= pi < npages:
            page = [(i + 1, cp) for i, cp in enumerate(cand[pi * per:(pi + 1) * per])]
            show_page(font, page)
            step = None
            while step is None:
                try:
                    cmd = input(f"page {pi + 1}/{npages}  [idx name | n | p | q]: ").strip()
                except EOFError:
                    print("\n(no interactive input - run this in a real terminal)")
                    return 0
                low = cmd.lower()
                if low in ("n", ""):
                    step = 1
                elif low == "p":
                    step = -1 if pi > 0 else 0
                elif low == "q":
                    step = "q"
                else:
                    parts = cmd.split(maxsplit=1)
                    try:
                        li = int(parts[0])
                    except ValueError:
                        print("  ? expected '<idx> <name>', n, p, or q")
                        continue
                    if not (1 <= li <= len(page)):
                        print(f"  idx out of range (1-{len(page)})")
                        continue
                    cp = page[li - 1][1]
                    name = parts[1].strip() if len(parts) > 1 else ""
                    out.write(f"U+{cp:04X}  {name}\n")
                    out.flush()
                    print(f"  kept #{li} U+{cp:04X}{(' as ' + name) if name else ' (name later)'}")
            if step == "q":
                break
            pi += step
    finally:
        out.close()
    print(f"\nsaved -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
