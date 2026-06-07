#!/usr/bin/env python3
"""Debug: open the file-browser context menu on a throwaway file and dump what's on
screen (saved PNG + OCR needle checks), to see whether DELETE is offered and whether
the modal is OCR-readable. Assumes /fbtest_a.bin is present on the main LittleFS."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # scripts/
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from common import harness as h
from common import ocrnav as nav
from common import remote_input as ri
from common import device, ocr
from ocr_nav_test import detect_language

OUT = "/tmp/fb_ctx.png"


def main():
    with ri.session() as dev:
        code, s = detect_language(dev)
        nav.enter(dev, s["STR_TITLE_TOOLS"].strip())
        nav.enter(dev, s["STR_FILE_BROWSER"].strip())
        lfs = s["STR_FS_LITTLEFS"].strip()
        for _ in range(6):
            if nav.present(dev, lfs, wake=False).get(lfs):
                break
            h.settle(0.5)
        nav.enter(dev, lfs, wake=False)
        h.settle(0.4)
        if not nav.navigate(dev, "fbtest_a.bin", wake=False):
            print("could not select fbtest_a.bin"); return 1
        dev.button_press([ri.BTN_PAUSE]); h.settle(0.6)   # open context menu

        img, _ = device.read_framebuffer(dev.backend)
        img.save(OUT)
        import numpy as np
        sc = ocr.Screen(np.asarray(img.convert("RGB")))
        needles = [s["STR_OPTIONS"].strip(), s["STR_COPY"].strip(),
                   s["STR_DELETE"].strip(), s["STR_CANCEL"].strip(),
                   s["STR_PASTE"].strip()]
        print(f"[{code}] saved {OUT}")
        for n in needles:
            print(f"  has({n!r}) = {sc.has(n)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
