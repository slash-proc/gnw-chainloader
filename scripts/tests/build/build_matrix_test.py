#!/usr/bin/env python3
"""L1 build gate: every build variant compiles.

Each test/diagnostic build flag must still compile cleanly (a broken variant is a
latent failure that only bites when someone flashes it). This does `make clean`
then `make` for each flag combination, recording pass/fail, and ALWAYS ends by
rebuilding the standard REMOTE_INPUT firmware so the working tree is left in the
normal testable state (app.elf + build/i18n present for the device + OCR tests).

HEAVY: each combo is a clean build (~30-60 s). Default runs a representative
subset; --full runs them all. No device.

  python3 scripts/tests/build/build_matrix_test.py            # representative subset
  python3 scripts/tests/build/build_matrix_test.py --full     # every variant
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common.harness import TestRun            # noqa: E402

TEST_META = dict(tier="L1", subsystem="build", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[12])

REPO = Path(__file__).resolve().parents[3]

# The real build-variant matrix (EXTFLASH_SIZE / RG_SD_CARD are NOT build flags —
# they are runtime-detected / patcher-layout, so they are deliberately absent).
SUBSET = [
    [],                          # the default ship-ish build
    ["REMOTE_INPUT=0"],          # the actual shipping behavior (no shadow cell)
    ["ABI_SELFTEST=1"],          # the ABI self-test hook
]
FULL_EXTRA = [
    ["BOOT_BENCH=1"],
    ["CRASH_TEST=1"],
    ["RTL_TEST=1"],
    ["DUMMY_ABI=1"],
    ["DUMMY_ABI=2"],
    ["REMOTE_INPUT=1", "ABI_SELFTEST=1"],
]
# The build we leave the tree in (standard automation firmware).
RESTORE = ["REMOTE_INPUT=1"]


def build(flags, jobs="-j16", timeout=300):
    subprocess.run(["make", "clean"], cwd=REPO, capture_output=True, text=True, timeout=120)
    r = subprocess.run(["make", jobs] + flags, cwd=REPO,
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode == 0, r


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="build every variant (slow)")
    args = ap.parse_args()
    combos = SUBSET + (FULL_EXTRA if args.full else [])

    t = TestRun("build-variant matrix")
    t.note(f"building {len(combos)} variant(s) + a restore build (each is `make clean`)")
    try:
        for flags in combos:
            label = " ".join(flags) if flags else "(default)"
            t0 = time.time()
            ok, r = build(flags)
            dt = time.time() - t0
            if not ok:
                tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-6:])
                t.note(f"  build {label} FAILED tail:\n{tail}")
            t.check(ok, f"compiles: make {label}  ({dt:.0f}s)")
    finally:
        # Always leave the tree in the standard testable state.
        ok, _ = build(RESTORE)
        t.check(ok, f"restored standard build: make {' '.join(RESTORE)}")
        subprocess.run(["make", "i18n"], cwd=REPO, capture_output=True, text=True, timeout=300)
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
