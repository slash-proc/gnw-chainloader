#!/usr/bin/env python3
"""Capture the displayed LTDC framebuffer over SWD to a PNG (build/menu_shot.png)
for a quick visual check of what the device is rendering, and OCR-report whether
the English header + a few known labels are present (English vs a loaded language).
"""
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import device, ocr
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

RESET = "--reset" in sys.argv

be = OpenOCDBackend()
be.open()
try:
    if RESET:
        be.reset_and_halt()
        be.resume()
        time.sleep(3.5)
    be.halt()
    img, info = device.read_framebuffer(be)
    pc = be.read_register("pc") & ~1
    be.resume()
    print(f"PC=0x{pc:08X}")
finally:
    be.close()

out = Path(__file__).resolve().parents[2] / "build" / "menu_shot.png"
out.parent.mkdir(parents=True, exist_ok=True)
img.save(out)
sc = ocr.Screen(np.asarray(img.convert("RGB")))
print(f"saved {out}  ({info['width']}x{info['height']} {info['format']})")
print("GNW CHAINLOADER header:", "present" if sc.has("GNW CHAINLOADER") else "ABSENT")
for label in ("SETTINGS", "TOOLS", "EINSTELLUNGEN", "WERKZEUGE", "Deutsch"):
    if sc.has(label):
        print(f"  found: {label!r}")
