#!/usr/bin/env python3
"""Press a sequence of buttons over SWD (no OCR), for stepping the UI between fastcap shots.

Usage: btn.py down down a        names: up down left right a b start select game
"""
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))  # scripts/
from common import remote_input as ri


def main() -> int:
    dev = ri.connect()
    for name in sys.argv[1:]:
        dev.button_press([getattr(ri, "BTN_" + name.upper())])
        time.sleep(0.35)   # let the UI consume each press (back-to-back presses get dropped)
    print("pressed:", " ".join(sys.argv[1:]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
