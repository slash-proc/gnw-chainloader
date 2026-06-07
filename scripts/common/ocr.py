#!/usr/bin/env python3
"""
Font-aware OCR for the Game & Watch UI — reads the chainloader menu text off a
captured framebuffer, in ANY UI language, with zero dependence on a vision model.

The UI is a tiny pixel font (12px) that generic OCR engines mangle, but we own the
exact glyph bitmaps and the rendering is pixel-exact (no anti-aliasing). So we
recognise text by matching our own glyphs:

  - load the in-core ASCII font (ui/gui_font.c) + every script .fnt blob
    (build/i18n/fonts/*.fnt), keyed by codepoint — that is the whole glyph set the
    device can draw, so coverage is multilingual by construction;
  - the text colour is theme-dependent (gui_fg_color), so we DON'T assume a colour:
    a candidate glyph matches where its "ink" pixels are one consistent colour that
    is distinct from the surrounding background (shape match, colour-agnostic).

Public API:
  Font.load()                      -> glyph set (cached)
  render_text(font, s)             -> (mask: HxW bool, top)  the device's exact pixels
  read_rows(frame_rgb)             -> [(y, text), ...]       OCR of every menu text row
  locate(frame_rgb, s)            -> (x, y, score) | None   find a known string
  find_selected_row(frame_rgb)     -> y | None               the cursor ('>') row

CLI:  python3 scripts/common/ocr.py <frame.png>     # dump recognised rows
"""
from __future__ import annotations

import os
import re
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
ASCII_C = REPO / "src" / "chainloader" / "ui" / "gui_font.c"
FONT_DIR = REPO / "build" / "i18n" / "fonts"

GUI_FONT_REF_TOP = 1            # ui/gui_font.h
FNT_MAGIC = 0x31544E46          # 'FNT1'


class Font:
    _cache = None

    def __init__(self):
        self.glyphs: dict[int, tuple] = {}   # cp -> (w, h, yoff, mask: HxW bool)

    @classmethod
    def load(cls) -> "Font":
        if cls._cache is None:
            f = cls()
            f._load_ascii(ASCII_C)
            for p in sorted(FONT_DIR.glob("*.fnt")):
                f._load_fnt(p)
            cls._cache = f
        return cls._cache

    def _store(self, cp, w, h, yoff, rows, stride):
        m = np.zeros((h, w), bool)
        for r in range(h):
            rb = rows[r]
            for c in range(w):
                if rb[c >> 3] & (0x80 >> (c & 7)):
                    m[r, c] = True
        self.glyphs.setdefault(cp, (w, h, yoff, m))

    def _load_ascii(self, path):
        txt = path.read_text()
        # entries look like:  { 6, { 0x00, 0x20, ... } },  // 0x41 'A'
        for m in re.finditer(r"\{\s*(\d+)\s*,\s*\{([^}]*)\}\s*\},\s*//\s*0x([0-9A-Fa-f]+)", txt):
            w = int(m.group(1))
            rows = [bytes([int(x, 16)]) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(2))]
            self._store(int(m.group(3), 16), w, len(rows), 0, rows, 1)

    def _load_fnt(self, path):
        d = path.read_bytes()
        magic, gc, H, top, bm = struct.unpack_from("<IHBBI", d, 0)
        if magic != FNT_MAGIC:
            return
        o = 12
        cps = struct.unpack_from(f"<{gc}I", d, o); o += 4 * gc
        widths = d[o:o + gc]; o += gc + ((-gc) % 4)
        offs = struct.unpack_from(f"<{gc}I", d, o)
        yoff = top - GUI_FONT_REF_TOP
        for i, cp in enumerate(cps):
            w = widths[i]
            stride = (w + 7) // 8
            g = bm + offs[i]
            rows = [d[g + r * stride: g + (r + 1) * stride] for r in range(H)]
            self._store(cp, w, H, yoff, rows, stride)

    def glyph(self, cp):
        return self.glyphs.get(cp) or self.glyphs.get(ord("?"))


def render_text(font: Font, s: str):
    """The exact pixels the device draws for `s`: (mask HxW bool, top) where the
    mask's row 0 sits `top` rows above the text baseline (top is usually -1)."""
    cells, x = [], 0
    for ch in s:
        w, h, yoff, m = font.glyph(ord(ch))
        cells.append((x, yoff, m))
        x += w
    if not cells:
        return np.zeros((0, 0), bool), 0
    top = min(yo for _, yo, _ in cells)
    bot = max(yo + m.shape[0] for _, yo, m in cells)
    out = np.zeros((bot - top, x), bool)
    for xo, yo, m in cells:
        out[yo - top: yo - top + m.shape[0], xo: xo + m.shape[1]] |= m
    return out, top


# ---- matching ---------------------------------------------------------------
#
# The UI's text colour (gui_fg_color) is theme-set but constant within a session,
# and the rendering is pixel-exact, so we detect that colour once and binarise the
# frame into an "ink" mask. Glyph matching is then a fast binary template compare,
# robust to the translucent panel + background animation behind the text.

def _color_score(region: np.ndarray, mask: np.ndarray) -> float:
    """Colour-agnostic glyph fit — only used to anchor the header for FG detection
    (clean top bar). Ink pixels should be one tight colour, distinct from paper."""
    ink, paper = region[mask], region[~mask]
    if ink.shape[0] < 2 or paper.shape[0] < 2:
        return -1e9
    mi, mp = ink.mean(0), paper.mean(0)
    sep = float(np.linalg.norm(mi - mp))
    spread = float(np.sqrt(((ink - mi) ** 2).sum(1)).mean())
    return sep - 0.7 * spread


def detect_fg(frame_rgb, font, anchor="GNW CHAINLOADER"):
    """Find the UI foreground (text) colour by locating the always-ASCII header in
    the top bar and reading the colour of its ink pixels. Returns an RGB array or
    None (caller can pass a colour explicitly)."""
    mask, _ = render_text(font, anchor)
    if mask.size == 0:
        return None
    H, W = mask.shape
    f = frame_rgb.astype(np.float32)
    best = (-1e9, 0, 0)
    for y in range(0, min(24, frame_rgb.shape[0] - H)):
        for x in range(0, frame_rgb.shape[1] - W):
            sc = _color_score(f[y:y + H, x:x + W], mask)
            if sc > best[0]:
                best = (sc, x, y)
    if best[0] >= 120:
        _, x, y = best
        ink = frame_rgb[y:y + H, x:x + W][mask]
        return np.median(ink, axis=0)
    # Fallback (sub-pages, where the title is translated so the anchor misses):
    # the UI draws light text on darker themes, so take the brightest pixels of
    # the highest-edge-energy row (a text row) as the foreground colour.
    g = frame_rgb.astype(np.float32).mean(2)
    energy = np.abs(np.diff(g, axis=1)).sum(1)
    y = int(energy.argmax())
    band = frame_rgb[max(0, y - 1):y + 11].reshape(-1, 3)
    lum = band.astype(np.float32).mean(1)
    bright = band[lum >= np.percentile(lum, 97)]
    return np.median(bright, axis=0) if len(bright) >= 5 else None


def binarize(frame_rgb, fg, tol=72):
    d = np.abs(frame_rgb.astype(np.int16) - np.asarray(fg, np.int16)).sum(2)
    return d < tol


class Screen:
    """An OCR view of one captured frame: detects the FG colour + ink mask once,
    then answers locate / read / selected-row queries against it."""

    def __init__(self, frame_rgb, font=None, fg=None):
        self.frame = frame_rgb
        self.font = font or Font.load()
        self.fg = fg if fg is not None else detect_fg(frame_rgb, self.font)
        self.ink = binarize(frame_rgb, self.fg) if self.fg is not None else None
        self._by_w = {}
        for cp, (w, h, yoff, m) in self.font.glyphs.items():
            self._by_w.setdefault(w, []).append((cp, yoff, m))
        self._widths = sorted(self._by_w, reverse=True)

    def _fit(self, mask, x, y):
        H, W = mask.shape
        reg = self.ink[y:y + H, x:x + W]
        if reg.shape != mask.shape:
            return -1.0
        ink = mask.sum()
        if ink == 0:
            return -1.0
        hit = float((mask & reg).sum()) / ink
        non = (~mask).sum()
        fp = float((~mask & reg).sum()) / non if non else 0.0
        return hit - fp

    def locate(self, s, y_hint=None, thresh=0.60):
        """(x, y, score) of string `s`, or None. y_hint limits the y search."""
        if self.ink is None:
            return None
        mask, _ = render_text(self.font, s)
        if mask.size == 0:
            return None
        H, W = mask.shape
        fh, fw = self.ink.shape
        ys = (range(max(0, y_hint - 3), min(fh - H, y_hint + 4))
              if y_hint is not None else range(fh - H))
        best = (-1.0, 0, 0)
        for y in ys:
            for x in range(0, fw - W):
                sc = self._fit(mask, x, y)
                if sc > best[0]:
                    best = (sc, x, y)
        return (best[1], best[2], best[0]) if best[0] >= thresh else None

    def has(self, s, **kw):
        return self.locate(s, **kw) is not None

    def selected_row(self, left_col=170):
        """y of the cursor row. The selected list item carries a theme sprite (a
        coin, a '>' selector, ...) in the gap just left of the text margin, drawn
        in the accent colour — NOT the text colour — so we detect it as
        non-background content there rather than as 'ink'. Colour/theme/layout
        agnostic: works for the main menu (text x~72) and the file browser (x~12),
        ignoring any right-hand detail pane and the title bar."""
        if self.ink is None:
            return None
        rows = self.ink_rows()
        if len(rows) < 2:
            return None
        lefts = []
        for y0, y1 in rows[1:]:                           # skip the title/header bar
            cols = np.where(self.ink[y0:y1, :left_col].any(0))[0]
            if len(cols):
                lefts.append((y0, y1, int(cols[0])))
        if not lefts:
            return None
        vals = [l for *_, l in lefts]
        margin = max(set(vals), key=vals.count)           # shared text margin (mode)
        z0, z1 = max(0, margin - 16), max(1, margin - 1)  # the cursor gap, left of text
        if z1 <= z0:
            return None
        zone = self.frame[lefts[0][0]:lefts[-1][1], z0:z1].reshape(-1, 3)
        uq, cn = np.unique(zone, axis=0, return_counts=True)
        bg = uq[cn.argmax()].astype(np.int16)             # the panel/background colour
        scored = []
        for y0, y1, _ in lefts:
            z = self.frame[y0:y1, z0:z1].reshape(-1, 3).astype(np.int16)
            scored.append((y0, int((np.abs(z - bg).sum(1) > 60).sum())))
        scored.sort(key=lambda t: -t[1])
        second = scored[1][1] if len(scored) > 1 else 0
        return scored[0][0] if scored[0][1] >= 4 and scored[0][1] >= 2 * second + 2 else None

    def ink_rows(self):
        # A row is "text" if it has a few ink pixels. Use a low absolute floor: a
        # max-relative threshold gets skewed by one dense row (e.g. the full-width
        # title bar), which then fragments the sparser body rows into nothing.
        # Inter-row gaps are genuinely 0 ink, so this cleanly separates rows.
        rs = self.ink.sum(1)
        thr = max(2, rs.max() * 0.008)
        on = rs > thr
        bands, y, fh = [], 0, len(on)
        while y < fh:
            if on[y]:
                y0 = y
                while y < fh and on[y]:
                    y += 1
                if y - y0 >= 6:
                    bands.append((y0, y))
            else:
                y += 1
        return bands

    def _best_glyph(self, x, draw_y):
        """Best glyph whose top-left cell sits at (x, draw_y+yoff). Greedy, widest
        first so a narrow glyph can't shadow a wide letter. None if nothing fits."""
        fh, fw = self.ink.shape
        best = None
        for w in self._widths:
            if x + w > fw:
                continue
            for cp, yoff, m in self._by_w[w]:
                yy = draw_y + yoff
                if yy < 0 or yy + m.shape[0] > fh:
                    continue
                sc = self._fit(m, x, yy)
                if sc >= 0.74 and (best is None or sc > best[0] or
                                   (abs(sc - best[0]) < 1e-6 and w > best[2])):
                    best = (sc, cp, w)
        return best

    def read_line(self, y_band_top):
        fh, fw = self.ink.shape
        band = self.ink[max(0, y_band_top - 3):y_band_top + 14]
        cols = np.where(band.any(0))[0]
        if len(cols) == 0:
            return ""
        x0 = int(cols[0])
        # band-top trims a row or two off the true draw-y; pin draw_y by the best
        # first-glyph match across a small vertical window, then read the line.
        draw_y, score = y_band_top, -1.0
        for dy in range(y_band_top - 3, y_band_top + 2):
            b = self._best_glyph(x0, dy)
            if b and b[0] > score:
                score, draw_y = b[0], dy
        out, x, gap = [], x0, 0
        while x < fw - 1:
            b = self._best_glyph(x, draw_y)
            if b:
                out.append(chr(b[1]))
                x += b[2]
                gap = 0
            else:
                x += 1
                gap += 1
                if out and gap == 4:
                    out.append(" ")
                if gap > 36 and out:
                    break
        return "".join(out).strip()

    def read_rows(self):
        return [(y0, t) for y0, y1 in self.ink_rows() if (t := self.read_line(y0))]


def _load_png(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB"))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: ocr.py <frame.png> [string-to-locate]")
    frame = _load_png(sys.argv[1])
    sc = Screen(frame)
    print(f"loaded {len(sc.font.glyphs)} glyphs; fg={None if sc.fg is None else sc.fg.astype(int).tolist()}")
    if "--ink" in sys.argv:
        from PIL import Image
        Image.fromarray((sc.ink * 255).astype(np.uint8)).save("build/i18n_test/ink.png")
        print("ink rows:", sc.ink_rows())
        # dominant colours per ~20px vertical slab (helps spot the text colour)
        for y in range(0, frame.shape[0], 24):
            band = frame[y:y + 24].reshape(-1, 3)
            uniq, cnt = np.unique(band, axis=0, return_counts=True)
            top = uniq[np.argsort(cnt)[-4:]][::-1]
            print(f"  y={y:3d} top colours: {top.tolist()}")
        sys.exit(0)
    if len(sys.argv) >= 3:
        print(f"locate {sys.argv[2]!r}: {sc.locate(sys.argv[2])}")
        print(f"selected row y: {sc.selected_row()}")
    else:
        for y, txt in sc.read_rows():
            print(f"  y={y:3d}  {txt!r}")
