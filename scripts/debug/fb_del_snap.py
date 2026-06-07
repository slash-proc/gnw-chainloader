#!/usr/bin/env python3
"""Debug: snapshot the file (not folder) context menu + where UP x2 lands, to see why
the single-file delete didn't take. Saves /tmp/d1.png (menu open), d2 (after 1 UP),
d3 (after 2 UP). Assumes /fbtest_a.bin present on the main LittleFS."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from common import harness as h
from common import ocrnav as nav
from common import remote_input as ri
from common import device
from ocr_nav_test import detect_language


def snap(dev, path):
    img, _ = device.read_framebuffer(dev.backend)
    img.save(path)
    print("saved", path)


def main():
    with ri.session() as dev:
        h.wake(dev); h.settle(0.3)
        for _ in range(4):
            dev.button_press([ri.BTN_B]); h.settle(0.25)
        _, s = detect_language(dev)
        nav.enter(dev, s["STR_TITLE_TOOLS"].strip())
        nav.enter(dev, s["STR_FILE_BROWSER"].strip())
        lfs = s["STR_FS_LITTLEFS"].strip()
        for _ in range(6):
            if nav.present(dev, lfs, wake=False).get(lfs):
                break
            h.settle(0.5)
        nav.enter(dev, lfs, wake=False); h.settle(0.4)
        if not nav.navigate(dev, "fbtest_a.bin", wake=False):
            print("could not select fbtest_a.bin"); return 1
        dev.button_press([ri.BTN_PAUSE]); h.settle(0.6); snap(dev, "/tmp/d1.png")
        dev.button_press([ri.BTN_UP]);    h.settle(0.4); snap(dev, "/tmp/d2.png")
        dev.button_press([ri.BTN_UP]);    h.settle(0.4); snap(dev, "/tmp/d3.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
