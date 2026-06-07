#!/usr/bin/env python3
"""SD-card detection state, asserted over pure SWD (no OCR).

The SD block device probes SPI1 ("Tim") then SoftSPI-over-OSPI ("Yota9") and
records the result in g_hw / g_card_type. This reads them directly: the detection
enum must be a valid value (not garbage), and if a card is mounted the hardware
path must be a real one. Which mod / whether a card is inserted is environment-
dependent, so presence is noted, not hard-required, unless a card is expected.

  python3 scripts/tests/sd/sd_detect_test.py            # assert the mechanism
  python3 scripts/tests/sd/sd_detect_test.py --expect-card   # also require a card
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L2", subsystem="sd", envs=["ANY"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[13])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-card", action="store_true",
                    help="also require a card to be present (ENV with SD inserted)")
    args = ap.parse_args()

    t = h.TestRun("SD detection (SWD)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")

        hw, present = observe.sd_state(be)
        t.note(f"g_hw = {hw}   card present = {present}")

        t.check(hw in observe.SD_HW_NAMES.values(),
                f"SD hardware state is a valid enum value ({hw})")

        if present:
            t.check(hw in ("SPI1", "OSPI1"),
                    f"a mounted card sits on a real bus ({hw}, not UNDETECTED/NONE)")
        else:
            t.note("no card mounted (UNDETECTED/NONE) — detection mechanism still valid")

        if args.expect_card:
            t.check(present is True, "a card is present (as required by --expect-card)")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
