#!/usr/bin/env python3
"""L0 host unit test: the packed settings word (TAMP->BKP3R).

Locks the Python mirror in observe.py against the C definition in
src/common/boot_magic.h: signature, field placement, getters, and the graceful
"invalid word -> all defaults" behavior (what happens after a VBAT loss). Pure
logic, no device.

  python3 scripts/tests/host/test_settings_word.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import observe                    # noqa: E402
from common.harness import TestRun            # noqa: E402

TEST_META = dict(tier="L0", subsystem="settings", envs=["ANY"], build=None,
                 observable="host", automated=True, goal=[1, 4, 10])

HEADER = Path(__file__).resolve().parents[3] / "src" / "common" / "boot_magic.h"


def _hdr_const(name, default):
    """Read a #define <name> <int> from boot_magic.h (cross-check the mirror)."""
    m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)", HEADER.read_text())
    return int(m.group(1), 0) if m else default


def main() -> int:
    t = TestRun("settings word (BKP3R)")

    # 1. The mirror's constants match the C header.
    t.check(observe.SETTINGS_SIG == _hdr_const("SETTINGS_SIG", 0xA6),
            "SETTINGS_SIG matches boot_magic.h")
    t.check(observe.SETTINGS_LANG_SHIFT == _hdr_const("SETTINGS_LANG_SHIFT", 16),
            "SETTINGS_LANG_SHIFT matches boot_magic.h")
    t.check(observe.SETTINGS_MARIO_SHIFT == _hdr_const("SETTINGS_MARIO_SHIFT", 8),
            "SETTINGS_MARIO_SHIFT matches boot_magic.h")
    t.check(observe.SETTINGS_ZELDA_SHIFT == _hdr_const("SETTINGS_ZELDA_SHIFT", 12),
            "SETTINGS_ZELDA_SHIFT matches boot_magic.h")

    # 2. make/decode round-trips across the field ranges.
    for fb in (False, True):
        for m in range(8):           # slots are 0..7 (3-bit usable, 4-bit field)
            for z in (0, 3, 7):
                for lang in (0, 1, 18, 255):
                    w = observe.settings_make(fb, m, z, lang)
                    ok = (observe.settings_valid(w)
                          and observe.settings_fastboot(w) == fb
                          and observe.settings_mario_slot(w) == m
                          and observe.settings_zelda_slot(w) == z
                          and observe.settings_lang(w) == lang)
                    if not ok:
                        t.check(False, f"round-trip fb={fb} m={m} z={z} lang={lang} -> 0x{w:08X}")
                        break
                else:
                    continue
                break
    t.check(True, "make/decode round-trips across fastboot x mario x zelda x lang")

    # 3. A specific known encoding (guards against silent shift drift).
    w = observe.settings_make(True, 2, 1, 5)
    t.check(w == 0xA6051201, f"settings_make(1,2,1,5) == 0xA6051201 (got 0x{w:08X})")

    # 4. Field isolation: changing one field never disturbs another.
    base = observe.settings_make(False, 0, 0, 0)
    only_lang = observe.settings_make(False, 0, 0, 9)
    t.check(observe.settings_mario_slot(only_lang) == 0
            and observe.settings_zelda_slot(only_lang) == 0
            and observe.settings_fastboot(only_lang) is False
            and observe.settings_lang(only_lang) == 9,
            "setting lang leaves slots/fastboot untouched")

    # 5. Graceful invalid word (post-VBAT-loss reads 0 / wrong sig) -> all defaults.
    for bad in (0x00000000, 0xFFFFFFFF, 0x52000000, 0x12345678):
        t.check(not observe.settings_valid(bad)
                and observe.settings_fastboot(bad) is False
                and observe.settings_mario_slot(bad) == 0
                and observe.settings_zelda_slot(bad) == 0
                and observe.settings_lang(bad) == 0,
                f"invalid word 0x{bad:08X} -> safe defaults (no fast-boot, slot 0, English)")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
