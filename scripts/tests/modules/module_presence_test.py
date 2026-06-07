#!/usr/bin/env python3
"""Module subsystem loaded correctly, asserted over pure SWD (no OCR).

A consolidated free-observable check that the boot-time scan completed and the
expected modules/drivers came up: partition scan done, VFS drivers registered,
theme + feature modules discovered, module pool sane. Counts are environment-
dependent, so most are noted; the hard requirements are the ones that must hold
wherever modules exist on LittleFS.

  python3 scripts/tests/modules/module_presence_test.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402

TEST_META = dict(tier="L2", subsystem="modules", envs=["ANY"], build="REMOTE_INPUT=1",
                 observable="swd", automated=True, goal=[4, 9, 13, 16])


def main() -> int:
    t = h.TestRun("module subsystem (SWD)")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")

        ready = observe.modules_ready(be)
        t.check(ready is True, f"boot-time partition/module scan completed (modules_ready={ready})")

        lfs_rw, fat_rw, drivers = observe.rw_drivers(be)
        t.note(f"VFS drivers: count={drivers} lfs_rw_loaded={lfs_rw} fat_rw_loaded={fat_rw}")
        t.check(drivers is not None and drivers >= 1,
                f"at least one VFS driver is registered (count={drivers})")

        theme_n = observe.theme_module_count(be)
        feat_n = observe.feature_count(be)
        t.note(f"theme modules registered: {theme_n}")
        t.note(f"feature modules discovered: {feat_n}")
        # If a theme module is loaded it must have registered at least one sprite
        # theme; if features were discovered the count is positive. Both are noted
        # rather than required because a bare device legitimately has none.
        if theme_n is not None and theme_n > 0:
            slot = observe.active_theme_slot(be)
            t.check(slot is not None, f"active theme slot readable ({slot})")

        pool = observe.pool_used(be)
        t.check(pool is not None and 0 <= pool < (256 * 1024),
                f"module pool usage is sane ({pool} B of 256 KiB)")
        err = observe.last_load_err(be)
        t.check(err == 0 or err is None,
                f"no module-load error recorded (g_mod_load_err={err})")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
