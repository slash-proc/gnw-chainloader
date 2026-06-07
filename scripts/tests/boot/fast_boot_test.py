#!/usr/bin/env python3
"""Fast-boot setting: BKP3R fast-boot bit boots straight to Retro-Go; PAUSE/START
forces the menu.

CAUTION -- NOT in the default automated set (automated=False). Enabling fast-boot
makes the device boot Retro-Go on every reset; the clean way back is the physical
PAUSE/START-at-boot override (read from GPIO, NOT the shadow cell), so a fully
headless restore is not guaranteed. Run this WITH a present operator who can hold
PAUSE at power-on if the SWD restore does not take. The test tries to restore by
clearing the bit and rebooting, but flags the risk.

Asserts:
  1. fast-boot set + a valid Retro-Go -> a reset boots Retro-Go (PC leaves the
     chainloader's AXI-SRAM region, no menu),
  2. clearing the bit returns to the chainloader menu.

  python3 scripts/tests/boot/fast_boot_test.py        # run only with an operator present
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import remote_input as ri         # noqa: E402
from common import harness as h               # noqa: E402
from common import observe                    # noqa: E402
from common import provision                  # noqa: E402

TEST_META = dict(tier="L3", subsystem="boot", envs=["ENV-RG", "ENV-DOCS"],
                 build="REMOTE_INPUT=1", observable="swd", automated=False, goal=[1])

CHAINLOADER_REGION = 0x24      # the chainloader runs from AXI-SRAM; Retro-Go does not


def main() -> int:
    t = h.TestRun("fast-boot setting")
    with ri.session() as dev:
        be = dev.backend
        ok, detail = h.chainloader_running(be)
        t.require(ok, f"chainloader live ({detail})")
        original = observe.settings_word(be)
        t.require(original is not None, "read settings word")

        prov = provision.Provisioner(backend=be)
        try:
            # 1. Enable fast-boot, clear magics, reset -> Retro-Go should boot.
            prov.set_settings_word(fastboot=True)
            prov.clear_magics()
            with h.time_budget(15.0, "reset for fast-boot"):
                be.halt(); be.reset_and_halt(); be.resume()
            time.sleep(1.6)
            for _ in range(5):
                try:
                    dev.reconnect(); break
                except Exception:
                    time.sleep(0.4)
            try:
                be.halt(); pc = be.read_register("pc") & ~1; be.resume()
            except Exception:
                pc = None
            t.check(pc is not None and (pc >> 24) != CHAINLOADER_REGION,
                    f"fast-boot launched Retro-Go (PC=0x{(pc or 0):08X}, left the "
                    f"chainloader AXI-SRAM region)")
        finally:
            # Restore: clear the bit, reset back to the menu. If SWD can't write
            # BKP3R while Retro-Go runs, the operator must hold PAUSE at power-on.
            try:
                with h.time_budget(15.0, "restore settings"):
                    be.halt(); be.write_uint32(observe.TAMP_BKP3R, original); be.resume()
                prov.clean_reboot()
                back = observe.menu_selection(be)
                t.check(back is not None and 0 <= back < 8,
                        f"cleared fast-boot -> chainloader menu reachable (sel={back})")
            except Exception as e:
                t.note(f"SWD restore failed ({e}); HOLD PAUSE at power-on to force the menu")

    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
