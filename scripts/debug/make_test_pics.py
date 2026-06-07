#!/usr/bin/env python3
"""Generate attractive sample images for the Picture Viewer, into test_pics/ at the repo root.

Smooth gradients (HSV sweeps, radial vignettes, bilinear corner blends) -- no sharp edges or
text, so JPEG shows no ringing and they look intentional rather than like test patterns. High
JPEG quality (95) plus a lossless PNG. Varied sizes exercise the viewer's fit: smaller than the
320x240 screen (upscale), about screen size (1:1), and larger (descale).

  python3 scripts/debug/make_test_pics.py
"""
import os
import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                   "test_pics")


def hsv_to_rgb(h, s, v):
    """Vectorised HSV->RGB; h,s,v in [0,1]. Returns (H,W,3) uint8."""
    i = np.floor(h * 6).astype(int)
    f = h * 6 - i
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    i = i % 6
    r = np.choose(i, [v, q, p, p, t, v])
    g = np.choose(i, [t, v, v, q, p, p])
    b = np.choose(i, [p, p, t, v, v, q])
    return (np.stack([r, g, b], axis=-1) * 255).clip(0, 255).astype(np.uint8)


def spectrum(w, h):
    """Horizontal hue sweep, gently darkened top+bottom for depth."""
    xx, yy = np.meshgrid(np.linspace(0, 1, w), np.linspace(0, 1, h))
    v = 1.0 - 0.35 * (2 * np.abs(yy - 0.5)) ** 2
    return hsv_to_rgb(xx, np.full_like(xx, 0.85), v)


def radial(w, h, hue):
    """Warm radial glow -> dark vignette."""
    xx, yy = np.meshgrid(np.linspace(-1, 1, w), np.linspace(-1, 1, h))
    r = np.sqrt(xx ** 2 + yy ** 2) / np.sqrt(2)
    v = (1 - r) ** 1.3
    s = 0.55 + 0.4 * r
    return hsv_to_rgb(np.clip(hue + 0.12 * r, 0, 1), s, v)


def corners(w, h, c00, c10, c01, c11):
    """Bilinear blend between four corner colours -- a smooth abstract."""
    xx, yy = np.meshgrid(np.linspace(0, 1, w), np.linspace(0, 1, h))
    c = lambda a: (c00[a] * (1 - xx) * (1 - yy) + c10[a] * xx * (1 - yy)
                   + c01[a] * (1 - xx) * yy + c11[a] * xx * yy)
    return np.stack([c(0), c(1), c(2)], axis=-1).clip(0, 255).astype(np.uint8)


def main():
    os.makedirs(OUT, exist_ok=True)
    jobs = [
        ("01_spectrum.jpg", spectrum(640, 400)),                                  # larger -> descale
        ("02_dawn.jpg",     corners(320, 240, (255, 180, 120), (255, 120, 170),   # ~screen 1:1
                                    (120, 170, 255), (190, 140, 230))),
        ("03_ember.png",    radial(384, 384, 0.05)),                              # lossless PNG
        ("04_lagoon.jpg",   corners(240, 160, (40, 90, 140), (40, 160, 150),      # small -> upscale
                                    (90, 200, 190), (200, 230, 180))),
    ]
    for name, arr in jobs:
        img = Image.fromarray(arr, "RGB")
        path = os.path.join(OUT, name)
        if name.lower().endswith(".png"):
            img.save(path, "PNG")
        else:
            img.save(path, "JPEG", quality=95, subsampling=0)   # 4:4:4, high quality
        print(f"  {name:18s} {img.size[0]}x{img.size[1]}  ({os.path.getsize(path)} B)")


if __name__ == "__main__":
    main()
