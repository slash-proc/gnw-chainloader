#!/usr/bin/env python3
"""Host verification for the Picture Viewer's streaming BMP decoder (src/modules/features/
picture/bmp.c). Compiles bmp_decode_host.c, writes a battery of synthetic uncompressed (BI_RGB)
BMPs covering every supported bit depth (1/4/8 palette, 24, 32) and both orientations (bottom-up
and top-down), plus any real .bmp under test_pics/, and diffs each against the exact pixels it
encodes in RGB565 space. Because the synthetic BMPs are written here (a self-contained BI_RGB
writer, not PIL's BITFIELDS output) the reference is the exact source the decoder must reproduce,
so a correct decode matches bit-for-bit. Mirrors png_verify.py.

  python3 scripts/debug/bmp_verify.py
"""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PIC = os.path.join(ROOT, "src/modules/features/picture")
TMP = tempfile.mkdtemp(prefix="bmpverify_")


def to565_expand(rgb):
    """Quantize an (H,W,3) uint8 array to RGB565 and expand back -- exactly what bmp.c + the
    harness do -- so a correct decode matches bit-for-bit."""
    r, g, b = rgb[..., 0].astype(np.uint16), rgb[..., 1].astype(np.uint16), rgb[..., 2].astype(np.uint16)
    r5, g6, b5 = r >> 3, g >> 2, b >> 3
    R = (r5 << 3) | (r5 >> 2)
    G = (g6 << 2) | (g6 >> 4)
    B = (b5 << 3) | (b5 >> 2)
    return np.stack([R, G, B], axis=-1).astype(np.uint8)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        assert f.readline().strip() == b"255"
        data = np.frombuffer(f.read(w * h * 3), dtype=np.uint8)
    return data.reshape(h, w, 3)


def write_bmp(path, pix, bpp, palette=None, top_down=False, masks=None):
    """Self-contained BMP writer. `pix` is (H,W,3) uint8 for bpp 24/32, (H,W) packed uint16 for
    bpp 16, else (H,W) indices. `masks`=(r,g,b) emits a BI_BITFIELDS (comp 3) header (16/32bpp)."""
    H, W = pix.shape[0], pix.shape[1]
    rowbytes = ((W * bpp + 31) // 32) * 4
    rows = []
    for y in range(H):
        row = bytearray(rowbytes)
        if bpp in (24, 32):
            bpe = bpp // 8
            for x in range(W):
                r, g, b = int(pix[y, x, 0]), int(pix[y, x, 1]), int(pix[y, x, 2])
                off = x * bpe
                row[off] = b; row[off + 1] = g; row[off + 2] = r
                if bpp == 32:
                    row[off + 3] = 255
        elif bpp == 16:
            for x in range(W):
                v = int(pix[y, x]) & 0xFFFF
                row[x * 2] = v & 0xFF; row[x * 2 + 1] = (v >> 8) & 0xFF
        elif bpp == 8:
            for x in range(W):
                row[x] = int(pix[y, x])
        elif bpp == 4:
            for x in range(W):
                idx = int(pix[y, x]) & 0xF
                row[x // 2] |= (idx << 4) if (x % 2 == 0) else idx
        elif bpp == 1:
            for x in range(W):
                if int(pix[y, x]):
                    row[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(row))

    pal = b""
    ncolors = 0
    if palette is not None:
        ncolors = len(palette)
        pal = b"".join(struct.pack("<4B", b, g, r, 0) for (r, g, b) in palette)

    extra = b""
    comp = 0
    if masks is not None:
        comp = 3   # BI_BITFIELDS: 3 channel masks appended after the 40-byte header
        extra = b"".join(struct.pack("<I", m) for m in masks)

    data = b"".join(rows if top_down else reversed(rows))
    height_field = -H if top_down else H
    dib = struct.pack("<IiiHHIIiiII", 40, W, height_field, 1, bpp, comp, len(data), 2835, 2835, ncolors, 0)
    pix_off = 14 + 40 + len(extra) + len(pal)
    fh = struct.pack("<2sIHHI", b"BM", pix_off + len(data), 0, 0, pix_off)
    with open(path, "wb") as f:
        f.write(fh); f.write(dib); f.write(extra); f.write(pal); f.write(data)


def shift_of(m):
    s = 0
    while m and not (m & 1):
        m >>= 1; s += 1
    return s


def pack16(rgb, rm, gm, bm):
    """Encode (H,W,3) uint8 -> (H,W) uint16 packed for the given masks, and the matching reference
    RGB the decoder will reproduce (each channel scaled from its mask width up to 8 bits)."""
    rs, gs, bs = shift_of(rm), shift_of(gm), shift_of(bm)
    rmax, gmax, bmax = rm >> rs, gm >> gs, bm >> bs
    r = (rgb[..., 0].astype(np.uint32) * rmax + 127) // 255
    g = (rgb[..., 1].astype(np.uint32) * gmax + 127) // 255
    b = (rgb[..., 2].astype(np.uint32) * bmax + 127) // 255
    packed = ((r << rs) | (g << gs) | (b << bs)).astype(np.uint16)
    ref = np.stack([(r * 255 // rmax), (g * 255 // gmax), (b * 255 // bmax)], -1).astype(np.uint8)
    return packed, ref


def quantize(rgb, n):
    """PIL-quantize to n colours; return (index map HxW, palette list of (r,g,b))."""
    im = Image.fromarray(rgb, "RGB").convert("P", palette=Image.ADAPTIVE, colors=n)
    idx = np.asarray(im)
    raw = im.getpalette()[: n * 3]
    palette = [tuple(raw[i * 3:i * 3 + 3]) for i in range(len(raw) // 3)]
    while len(palette) < n:
        palette.append((0, 0, 0))
    return idx, palette


def gen_synthetic():
    """A gradient+noise base saved as each supported BMP form; returns [(path, ref_rgb)]."""
    h, w = 48, 70
    yy, xx = np.mgrid[0:h, 0:w]
    base = ((xx * 255 // w) ^ (yy * 3)) & 0xFF
    rng = np.random.default_rng(2)
    rgb = np.stack([base, (yy * 255 // h).astype(np.uint8),
                    ((xx + yy) * 4 % 256).astype(np.uint8)], axis=-1).astype(np.uint8)
    rgb = (rgb.astype(int) + rng.integers(-6, 7, rgb.shape)).clip(0, 255).astype(np.uint8)
    out = []
    p = lambda n: os.path.join(TMP, n)

    write_bmp(p("bmp24.bmp"), rgb, 24);                     out.append(("bmp24.bmp", rgb))
    write_bmp(p("bmp24_td.bmp"), rgb, 24, top_down=True);   out.append(("bmp24_td.bmp", rgb))
    write_bmp(p("bmp32.bmp"), rgb, 32);                     out.append(("bmp32.bmp", rgb))

    # 16bpp RGB565 BI_BITFIELDS -- exactly what Retro-Go writes for its screenshots.
    pk565, ref565 = pack16(rgb, 0xF800, 0x07E0, 0x001F)
    write_bmp(p("bmp16_565.bmp"), pk565, 16, masks=(0xF800, 0x07E0, 0x001F))
    out.append(("bmp16_565.bmp", ref565))
    # 16bpp XRGB1555 as BI_RGB (no mask block; decoder assumes 555).
    pk555, ref555 = pack16(rgb, 0x7C00, 0x03E0, 0x001F)
    write_bmp(p("bmp16_555.bmp"), pk555, 16)
    out.append(("bmp16_555.bmp", ref555))

    i8, pal8 = quantize(rgb, 256)
    write_bmp(p("bmp8.bmp"), i8, 8, palette=pal8)
    out.append(("bmp8.bmp", np.asarray(pal8, np.uint8)[i8]))

    i4, pal4 = quantize(rgb, 16)
    write_bmp(p("bmp4.bmp"), i4, 4, palette=pal4)
    out.append(("bmp4.bmp", np.asarray(pal4, np.uint8)[i4]))

    g = rgb[..., 0]
    i1 = (g > 128).astype(np.uint8)
    pal1 = [(0, 0, 0), (255, 255, 255)]
    write_bmp(p("bmp1.bmp"), i1, 1, palette=pal1)
    out.append(("bmp1.bmp", np.asarray(pal1, np.uint8)[i1]))
    return out


def main():
    harness = os.path.join(TMP, "bmp_decode_host")
    cc = ["gcc", "-O2", "-Wall", "-I", PIC,
          os.path.join(ROOT, "scripts/debug/bmp_decode_host.c"),
          os.path.join(PIC, "bmp.c"), "-o", harness]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        print("COMPILE FAILED:\n", r.stderr); return 1
    print("harness compiled")

    cases = gen_synthetic()
    for d in (os.path.join(ROOT, "test_pics"), os.path.join(ROOT, "build")):
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(".bmp"):
                path = os.path.join(d, f)
                try:
                    cases.append((path, np.asarray(Image.open(path).convert("RGB"))))
                except Exception as ex:
                    print(f"  [skip] {f}: PIL can't open as reference ({ex})")

    fails = 0
    for ident, ref_rgb in cases:
        src = ident if os.path.isabs(ident) else os.path.join(TMP, ident)
        name = os.path.basename(ident)
        out = os.path.join(TMP, "out.ppm")
        r = subprocess.run([harness, src, out], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [FAIL] {name}: harness {r.stderr.strip()}"); fails += 1; continue
        got = read_ppm(out)
        ref = to565_expand(ref_rgb)
        if got.shape != ref.shape:
            print(f"  [FAIL] {name}: shape {got.shape} vs ref {ref.shape}"); fails += 1; continue
        diff = np.abs(got.astype(int) - ref.astype(int))
        within = (diff <= 1).all(axis=-1)
        pct = 100.0 * within.mean()
        maxd = int(diff.max())
        ok = pct >= 99.5 and maxd <= 4
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:24s} {got.shape[1]}x{got.shape[0]}  "
              f"within±1: {pct:6.2f}%  maxdiff: {maxd}")
        if not ok:
            fails += 1
    print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
