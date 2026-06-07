#!/usr/bin/env python3
"""font_cp.py: inspect a cooked .fnt glyph set.

Usage:
  python3 scripts/debug/font_cp.py <font.fnt> [chars_or_U+hex ...]

With no extra args: print magic, glyph_count, cell_h, ref_top and the full sorted
codepoint list (hex + char). With chars/codepoints: report present/absent for each.
Reusable for diagnosing missing-glyph ("?") rendering: pass the characters you see
dropping and it tells you whether the cooked font actually contains them.
"""
import struct
import sys


def load_cps(path):
    b = open(path, "rb").read()
    magic, gc, cell_h, ref_top, bmoff = struct.unpack_from("<IHBBI", b, 0)
    assert magic == 0x31544E46, f"bad magic {magic:#x} (not FNT1)"
    cps = list(struct.unpack_from(f"<{gc}I", b, 12))
    return cps, gc, cell_h, ref_top


def render(path, chars):
    """ASCII-art the bitmap of each char so a .notdef/'?' placeholder is visible."""
    b = open(path, "rb").read()
    magic, gc, cell_h, ref_top, bmoff = struct.unpack_from("<IHBBI", b, 0)
    cps = list(struct.unpack_from(f"<{gc}I", b, 12))
    w_off = 12 + 4 * gc
    o_off = (w_off + gc + 3) & ~3
    widths = b[w_off:w_off + gc]
    offs = list(struct.unpack_from(f"<{gc}I", b, o_off))
    for ch in chars:
        cp = ord(ch)
        if cp not in cps:
            print(f"U+{cp:04X} {ch!r}: ABSENT"); continue
        i = cps.index(cp); w = widths[i]; stride = (w + 7) // 8
        base = bmoff + offs[i]
        print(f"U+{cp:04X} {ch!r}  w={w} h={cell_h}")
        for row in range(cell_h):
            bits = ""
            for x in range(w):
                byte = b[base + row * stride + (x >> 3)]
                bits += "#" if (byte >> (7 - (x & 7))) & 1 else "."
            print("   " + bits)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    if len(sys.argv) > 2 and sys.argv[2] == "--render":
        render(path, "".join(sys.argv[3:]))
        return 0
    cps, gc, cell_h, ref_top = load_cps(path)
    cpset = set(cps)
    print(f"{path}: {gc} glyphs, cell_h={cell_h}, ref_top={ref_top}")
    if len(sys.argv) == 2:
        for cp in cps:
            ch = chr(cp)
            print(f"  U+{cp:04X} {ch!r}")
        return 0
    for arg in sys.argv[2:]:
        targets = []
        if arg.upper().startswith("U+"):
            targets = [int(arg[2:], 16)]
        else:
            targets = [ord(c) for c in arg]
        for cp in targets:
            mark = "OK " if cp in cpset else "MISSING"
            print(f"  {mark} U+{cp:04X} {chr(cp)!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
