#!/usr/bin/env python3
"""Host verification for the Picture Viewer's streaming PNG decoder (src/modules/features/
picture/png.c). Compiles png_decode_host.c, decodes a battery of synthetic PNGs (every
supported colour type / bit depth, with filter-exercising content) plus any real .png under
test_pics/, and diffs each against a PIL reference in RGB565 space. This proves the
device-bound decoder on the build host -- the on-device path only adds the (shared, JPEG-tested)
resampler + framebuffer blit.

  python3 scripts/debug/png_verify.py
"""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PIC = os.path.join(ROOT, "src/modules/features/picture")
TMP = tempfile.mkdtemp(prefix="pngverify_")


def to565_expand(rgb):
    """Quantize an (H,W,3) uint8 array to RGB565 and expand back -- exactly what png.c +
    the harness do -- so a correct decode matches bit-for-bit."""
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


def ref_rgb(path):
    """PIL reference as the source RGB png.c would see (alpha dropped, 16-bit -> high byte)."""
    im = Image.open(path)
    if im.mode in ("I", "I;16", "I;16B"):
        a = np.asarray(im).astype(np.uint32)
        g = (a >> 8).astype(np.uint8)
        return np.stack([g, g, g], axis=-1)
    return np.asarray(im.convert("RGB"))


def gen_synthetic():
    """A gradient+noise base (triggers Sub/Up/Average/Paeth filters) saved in each form."""
    h, w = 64, 100
    yy, xx = np.mgrid[0:h, 0:w]
    base = ((xx * 255 // w) ^ (yy * 3)) & 0xFF
    rng = np.random.default_rng(1)
    rgb = np.stack([base, (yy * 255 // h).astype(np.uint8),
                    ((xx + yy) * 4 % 256).astype(np.uint8)], axis=-1).astype(np.uint8)
    rgb = (rgb.astype(int) + rng.integers(-6, 7, rgb.shape)).clip(0, 255).astype(np.uint8)
    cases = []
    p = lambda n: os.path.join(TMP, n)
    Image.fromarray(rgb, "RGB").save(p("rgb8.png")); cases.append("rgb8.png")
    Image.fromarray(rgb[..., 0], "L").save(p("gray8.png")); cases.append("gray8.png")
    a = np.dstack([rgb, np.full((h, w), 200, np.uint8)])
    Image.fromarray(a, "RGBA").save(p("rgba8.png")); cases.append("rgba8.png")
    Image.fromarray(rgb, "RGB").convert("P", palette=Image.ADAPTIVE).save(p("pal8.png")); cases.append("pal8.png")
    Image.fromarray((rgb[..., 0] > 128).astype(np.uint8) * 255, "L").convert("1").save(p("gray1.png")); cases.append("gray1.png")
    Image.fromarray(rgb, "RGB").convert("P", palette=Image.ADAPTIVE, colors=16).save(p("pal4.png"), bits=4); cases.append("pal4.png")
    Image.fromarray((rgb[..., 0].astype(np.uint16) << 8) | rgb[..., 1], "I;16").save(p("gray16.png")); cases.append("gray16.png")
    return cases


def main():
    harness = os.path.join(TMP, "png_decode_host")
    cc = ["gcc", "-O2", "-Wall", "-I", PIC,
          os.path.join(ROOT, "scripts/debug/png_decode_host.c"),
          os.path.join(PIC, "png.c"), os.path.join(PIC, "miniz.c"),
          "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME", "-DMINIZ_NO_ARCHIVE_APIS",
          "-DMINIZ_NO_ZLIB_APIS", "-DMINIZ_NO_MALLOC", "-o", harness]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        print("COMPILE FAILED:\n", r.stderr); return 1
    print("harness compiled")

    cases = [os.path.join(TMP, c) for c in gen_synthetic()]
    real = os.path.join(ROOT, "test_pics")
    if os.path.isdir(real):
        cases += [os.path.join(real, f) for f in sorted(os.listdir(real)) if f.lower().endswith(".png")]

    fails = 0
    for src in cases:
        out = os.path.join(TMP, "out.ppm")
        r = subprocess.run([harness, src, out], capture_output=True, text=True)
        name = os.path.basename(src)
        if r.returncode != 0:
            print(f"  [FAIL] {name}: harness {r.stderr.strip()}"); fails += 1; continue
        got = read_ppm(out)
        ref = to565_expand(ref_rgb(src))
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
