#!/usr/bin/env python3
"""L0 host unit test: the boot-magic decision table.

Locks the boot-magic constants in src/common/boot_magic.h (any accidental change
trips this, so every consumer is updated together) and models the documented
decision PRIORITY as a regression guard:

  1. RG cell == CORE or RESET  -> re-launch Retro-Go (RETROGO_BASE)   [the law]
  2. SRAM cell == STANDBY      -> RETROGO_BASE (Retro-Go standby resume)
  3. SRAM cell == BOOT         -> jump SRAM_MAGIC_TARGET
  4. SRAM cell == FORCE        -> forced bank-swap jump to target
  5. fast-boot set & RG valid  -> Retro-Go
  6. otherwise                 -> chainloader menu

Physical god-mode buttons (GPIO) sit ABOVE all of this and are out of scope for a
magic-cell table. Pure logic, no device.

  python3 scripts/tests/host/test_boot_magic_table.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common.harness import TestRun            # noqa: E402

TEST_META = dict(tier="L0", subsystem="boot", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[1, 15])

HEADER = Path(__file__).resolve().parents[3] / "src" / "common" / "boot_magic.h"

# The contract, locked. (Mirrors boot_magic.h; an intentional change updates both.)
EXPECT = {
    "BOOT_MAGIC_BOOT":     0x544F4F42,
    "BOOT_MAGIC_FORCE":    0x45435246,
    "BOOT_MAGIC_STANDBY":  0xFEDEBEDA,
    "BOOT_MAGIC_RETROGO":  0x434F5245,   # "CORE"
    "BOOT_MAGIC_RESET":    0x1FA1AFE1,
    "BOOT_MAGIC_FASTBOOT": 0x5254524F,   # "RTRO"
}


def _parse():
    txt = HEADER.read_text()
    out = {}
    for name in EXPECT:
        m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+)", txt)
        out[name] = int(m.group(1), 16) if m else None
    return out


def decide(rg, sram, target, fastboot, rg_valid, M):
    """Model of app_early_logic + stub priority (magic-cell paths only)."""
    if rg in (M["BOOT_MAGIC_RETROGO"], M["BOOT_MAGIC_RESET"]):
        return ("RETROGO", None)
    if sram == M["BOOT_MAGIC_STANDBY"]:
        return ("RETROGO", None)
    if sram == M["BOOT_MAGIC_BOOT"]:
        return ("JUMP", target)
    if sram == M["BOOT_MAGIC_FORCE"]:
        return ("JUMP", target)
    if fastboot and rg_valid:
        return ("RETROGO", None)
    return ("MENU", None)


def main() -> int:
    t = TestRun("boot-magic decision table")
    M = _parse()

    # 1. Constants present and match the locked contract.
    for name, val in EXPECT.items():
        t.check(M[name] == val,
                f"{name} == 0x{val:08X}" + ("" if M[name] == val else f" (got {M[name]})"))

    # 2. All six are distinct (no collision could divert boot ambiguously).
    vals = [v for v in M.values() if v is not None]
    t.check(len(set(vals)) == len(vals), "all boot-magic values are distinct")

    # 3. Decision priority across representative cases.
    TGT = 0x08100000
    cases = [
        # (rg, sram, fastboot, rg_valid) -> expected
        ((M["BOOT_MAGIC_RETROGO"], 0, False, True),  ("RETROGO", None), "CORE re-launches Retro-Go"),
        ((M["BOOT_MAGIC_RESET"],   0, False, True),  ("RETROGO", None), "RESET re-launches Retro-Go"),
        # CORE wins even if a BOOT-to-target is also staged (the law has priority).
        ((M["BOOT_MAGIC_RETROGO"], M["BOOT_MAGIC_BOOT"], False, True), ("RETROGO", None),
         "CORE outranks a staged BOOT target"),
        ((0, M["BOOT_MAGIC_STANDBY"], False, True),  ("RETROGO", None), "STANDBY resumes Retro-Go"),
        ((0, M["BOOT_MAGIC_BOOT"],    False, True),  ("JUMP", TGT),    "BOOT jumps to target"),
        ((0, M["BOOT_MAGIC_FORCE"],   False, True),  ("JUMP", TGT),    "FORCE jumps to target"),
        ((0, 0, True,  True),  ("RETROGO", None), "fast-boot + valid RG -> Retro-Go"),
        ((0, 0, True,  False), ("MENU", None),    "fast-boot but RG invalid -> menu"),
        ((0, 0, False, True),  ("MENU", None),    "clean boot -> menu"),
    ]
    for (rg, sram, fb, rgv), want, desc in cases:
        got = decide(rg, sram, TGT, fb, rgv, M)
        t.check(got == want, f"{desc} (got {got})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
