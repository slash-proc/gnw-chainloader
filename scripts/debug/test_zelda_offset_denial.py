#!/usr/bin/env python3
"""Regression check for the Zelda external-flash-offset denial.

Zelda cannot be relocated (no base register; absolute external pointers), so the
patch must refuse a nonzero ext_offset with a clear ValueError while still
patching normally at offset 0. Run from the repo root after `make patch_zelda`
(so build/gw_patch_zelda.{elf,bin} exist).
"""
import sys
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, "scripts/build")
import patch_firmware  # noqa: E402
from common.flashio import ZeldaGnW  # noqa: E402

INTERNAL = Path("backup/internal_flash_backup_zelda.bin")
EXTERNAL = Path("backup/flash_backup_zelda.bin")
ELF = Path("build/gw_patch_zelda.elf")
BIN = Path("build/gw_patch_zelda.bin")


def args(offset):
    return Namespace(
        no_la=False, no_sleep_images=False, no_second_beep=False,
        no_hour_tune=False, offset_size=offset, debug=False,
        sd_bootloader=False, encrypt=True,
    )


def run(offset):
    return patch_firmware.patch_ofw(ZeldaGnW, "zelda", INTERNAL, EXTERNAL, args(offset), ELF, BIN)


print("== offset = 0 (must succeed) ==")
run(0)
print("  OK: patched normally\n")

print("== offset = 0x800000 (must be denied) ==")
try:
    run(0x800000)
    print("  FAIL: no error raised — denial is not working!")
    sys.exit(1)
except ValueError as e:
    print(f"  OK: denied with ValueError ->\n    {e}")
