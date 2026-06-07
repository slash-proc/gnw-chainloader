#!/usr/bin/env python3
"""Drive the menu to launch the Picture Viewer via SWD button injection (no OCR).

Robust nav: the main menu's boot targets can be greyed out (shifting indices), but Power Off is
always last and selectable, and Tools is two up from it. Tools lists File Browser, Partition
Viewer, then the feature entries, with picture last. So: go to the bottom of the main menu, up to
Tools, enter, go to the bottom of Tools (picture), select.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))  # scripts/
from common import remote_input as ri


def main() -> int:
    dev = ri.connect()
    dev.button_press([ri.BTN_B], repeat=3)      # back out of any feature/submenu to the main menu
    dev.button_press([ri.BTN_DOWN], repeat=6)   # main menu -> Power Off (bottom)
    dev.button_press([ri.BTN_UP], repeat=2)     # Power -> Settings -> Tools
    dev.button_press([ri.BTN_A])                # enter Tools
    dev.button_press([ri.BTN_DOWN], repeat=6)   # Tools -> bottom (picture, the last feature)
    dev.button_press([ri.BTN_A])                # launch Picture Viewer
    print("nav sent: main bottom -> up2 (Tools) -> enter -> bottom (picture) -> select")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
