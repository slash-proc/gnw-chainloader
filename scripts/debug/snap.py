#!/usr/bin/env python3
"""Wake the device UI (idle-hide fades the menu after 30s) and capture the
framebuffer to a PNG — a menu-visible frame for OCR work / validation."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import device

out = sys.argv[1] if len(sys.argv) > 1 else "build/i18n_test/snap.png"
with ri.session() as dev:
    h.wake(dev)
    h.settle(0.3)
    img, _ = device.read_framebuffer(dev.backend)
    img.save(out)
    print("saved", out)
