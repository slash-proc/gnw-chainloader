#!/usr/bin/env python3
"""L1 build gate: the 40 KiB chainloader flash ceiling.

The chainloader bank is hard-capped at 40 KiB (RETROGO_BASE - CHAINLOADER_BASE =
0x0800A000 - 0x08000000 = 0x0000A000 = 40960 bytes). The Makefile already errors
a build that overflows; this records the size and the FREE headroom as a tracked
regression budget so a creeping binary is visible before it hits the wall (the
ceiling must NEVER be raised again — fit features module-side instead).

Needs a built build/gnw_chainloader.bin; skips with a note if absent. No device.

  python3 scripts/tests/build/size_ceiling_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common.harness import TestRun            # noqa: E402

TEST_META = dict(tier="L1", subsystem="build", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[12])

CEILING = 40960     # RETROGO_BASE - CHAINLOADER_BASE
BIN = Path(__file__).resolve().parents[3] / "build" / "gnw_chainloader.bin"


def main() -> int:
    t = TestRun("40 KiB flash ceiling")
    if not BIN.is_file():
        t.note(f"{BIN.name} not built — run `make` first; skipping")
        return t.summary()
    size = BIN.stat().st_size
    free = CEILING - size
    t.note(f"chainloader binary: {size} B used, {free} B free of {CEILING} B")
    t.check(size <= CEILING,
            f"binary fits the 40 KiB ceiling ({size} <= {CEILING})")
    if 0 < free < 256:
        t.note(f"WARNING: only {free} B headroom — fit new work module-side, "
               f"do NOT raise the ceiling")
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
