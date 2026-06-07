#!/usr/bin/env python3
"""OFFLINE (no device): figure out how to OCR-read the context-menu modal from the
saved screenshot /tmp/fb_ctx.png, so the test can navigate it closed-loop. Tries the
full frame vs a center crop, and auto-fg vs an explicit/ sampled fg."""
import sys
from pathlib import Path
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import ocr

img = np.asarray(Image.open("/tmp/fb_ctx.png").convert("RGB"))
print("frame", img.shape)
NEEDLES = ["Options", "Copy", "Paste", "Delete", "Cancel"]


def report(tag, frame, fg=None):
    sc = ocr.Screen(frame, fg=fg)
    fgv = None if sc.fg is None else np.asarray(sc.fg).astype(int).tolist()
    res = {n: bool(sc.has(n)) for n in NEEDLES}
    try:
        sr = sc.selected_row()
    except Exception as e:
        sr = f"err:{e}"
    print(f"[{tag}] fg={fgv} selected_row={sr}")
    print(f"        {res}")
    for n in NEEDLES:
        loc = sc.locate(n)
        if loc:
            print(f"        locate {n!r} -> x={loc[0]} y={loc[1]} score={loc[2]:.2f}")


# 1) full frame, auto fg (what the test does today)
report("full/auto", img)

# 2) center crop around the modal (from the screenshot it sits ~x[88:232] y[56:168])
crop = img[56:168, 88:232]
report("crop/auto", crop)

# 3) sample the brightest cluster in the crop as an explicit fg, full + crop
flat = crop.reshape(-1, 3)
bright = flat[flat.sum(1) > 360]            # light text pixels
if len(bright):
    fg = np.median(bright, axis=0).astype(np.uint8)
    print("sampled bright fg =", fg.tolist())
    report("full/sampledfg", img, fg=fg)
    report("crop/sampledfg", crop, fg=fg)
