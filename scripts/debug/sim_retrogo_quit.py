#!/usr/bin/env python3
"""Simulate Retro-Go's "Return to Main Menu": write BOOT_MAGIC_RETROGO ('CORE') to
RG_MAGIC_ADDR (0x20000000, DTCM — survives a warm reset), reset, and resume.
app_early_logic (main.c §2.1) must consume the magic and RE-LAUNCH Retro-Go (jump
RETROGO_BASE) so its OWN launcher reloads — never fall through to the chainloader
menu. (Bank-1 builds actually leave the RESET trace, handled by the stub; this pokes
the CORE path.) This is a manual inject+resume; for an automated pass/fail covering
both the RESET/stub and CORE/main.c paths, use scripts/tests/retrogo_return_test.py."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

RG_MAGIC_ADDR = 0x20000000
BOOT_MAGIC_RETROGO = 0x434F5245

be = OpenOCDBackend()
be.open()
try:
    be.halt()
    be.write_uint32(RG_MAGIC_ADDR, BOOT_MAGIC_RETROGO)
    print(f"wrote magic 0x{be.read_uint32(RG_MAGIC_ADDR):08X} @ 0x{RG_MAGIC_ADDR:08X}")
    be("reset halt", decode=False)
    surv = be.read_uint32(RG_MAGIC_ADDR)
    print(f"after warm reset: 0x{surv:08X} ({'SURVIVED' if surv == BOOT_MAGIC_RETROGO else 'lost'})")
    be.resume()
    print("resumed — firmware boots, should consume magic + re-launch Retro-Go")
finally:
    be.close()
