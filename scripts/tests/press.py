#!/usr/bin/env python3
"""Press a sequence of buttons over SWD, then screenshot. Tiny manual driver.

  python3 scripts/tests/press.py A           # press A, shoot
  python3 scripts/tests/press.py --name foo --settle 3 A
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import device

OUT = Path(__file__).resolve().parents[2] / "build" / "i18n_test"
BTN = {"A": ri.BTN_A, "B": ri.BTN_B, "UP": ri.BTN_UP, "DOWN": ri.BTN_DOWN,
       "LEFT": ri.BTN_LEFT, "RIGHT": ri.BTN_RIGHT, "START": ri.BTN_START,
       "SELECT": ri.BTN_SELECT, "PAUSE": ri.BTN_PAUSE}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("buttons", nargs="*")
    ap.add_argument("--name", default="press")
    ap.add_argument("--settle", type=float, default=1.0, help="seconds after the last press")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    with ri.session() as dev:
        for b in args.buttons:
            dev.button_press([BTN[b.upper()]]); h.settle(0.4)
        h.settle(args.settle)
        img, _ = device.read_framebuffer(dev.backend)
        p = OUT / f"{args.name}.png"
        img.save(p)
        print(f"  captured {p.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
