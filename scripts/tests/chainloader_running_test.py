#!/usr/bin/env python3
"""Assert the device is actually running the bank-1 chainloader.

A successful flash or file push leaves the gnwmanager RAM flasher resident; it
does NOT mean control reached the chainloader. This is the go-to precondition for
any probe-driven test: it proves the chainloader's main loop is live (its SysTick
uwTick is advancing), which distinguishes a running chainloader from the RAM
flasher, a hang, or an OFW. Reusable from any test via
`from common.harness import chainloader_running`.

  python3 scripts/tests/chainloader_running_test.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.harness import chainloader_running, TestRun, time_budget, recover_probe

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend


def main() -> int:
    t = TestRun("chainloader-running")
    be = OpenOCDBackend()
    try:
        with time_budget(30.0, "openocd connect"):
            be.open()
    except TimeoutError as e:
        t.check(False, f"openocd connect timed out ({e})")
        return t.summary()
    try:
        ok, detail = chainloader_running(be)   # self-bounded
        print(f"  {detail}")
        t.check(ok, "chainloader main loop is live (uwTick advancing, not parked in the RAM flasher)")
        return t.summary()
    finally:
        try:
            with time_budget(10.0, "backend close"):
                be.close()
        except TimeoutError:
            recover_probe()


if __name__ == "__main__":
    sys.exit(main())
