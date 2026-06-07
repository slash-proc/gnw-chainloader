#!/usr/bin/env python3
"""Re-launch Retro-Go by injecting the CORE magic and warm-resetting.

Writes BOOT_MAGIC_RETROGO ("CORE") to the RG magic cell (0x20000000) and
warm-resets; the chainloader's app_early_logic() consumes it and jumps to
RETROGO_BASE so Retro-Go's launcher comes up. Used to get Retro-Go running for
apples-to-apples comparisons (e.g. reading its live ADC over SWD). DTCM survives
the warm reset, so the magic is seen.

  python3 scripts/debug/launch_retrogo.py
"""
import sys
import time

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

RG_MAGIC_ADDR = 0x20000000
BOOT_MAGIC_RETROGO = 0x434F5245  # "CORE"


def main():
    be = OpenOCDBackend()
    be.open()
    try:
        be.write_uint32(RG_MAGIC_ADDR, BOOT_MAGIC_RETROGO)
        be.reset_and_halt()
        be.write_uint32(RG_MAGIC_ADDR, BOOT_MAGIC_RETROGO)  # re-arm post-reset
        be.resume()
        # let Retro-Go boot
        t = time.time()
        while time.time() - t < 4.0:
            be.read_memory(RG_MAGIC_ADDR, 4)
        print("Retro-Go launch issued (CORE magic + warm reset).")
        return 0
    finally:
        be.close()


if __name__ == "__main__":
    sys.exit(main())
