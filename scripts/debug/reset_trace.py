#!/usr/bin/env python3
"""Trace the stub RESET path: inject RESET at RG_MAGIC_ADDR, reset, resume, and
sample the PC + the magic cell over time. Confirms whether the chainloader stub
jumps to Retro-Go (PC enters the 0x08 flash range) regardless of what Retro-Go
decides to do next — isolating a chainloader regression from Retro-Go's own state.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend


def region(pc):
    r = pc >> 24
    return {0x08: "RETRO-GO/stub-flash", 0x24: "CHAINLOADER (AXI-SRAM)"}.get(r, f"0x{r:02X}")


be = OpenOCDBackend()
be.open()
try:
    be.halt()
    be.write_uint32(0x20000000, 0x1FA1AFE1)
    be.reset_and_halt()
    print(f"at reset vector: magic@0x20000000 = 0x{be.read_uint32(0x20000000):08X}")
    be.resume()
    for t in (0.1, 0.3, 0.6, 1.2, 2.5):
        time.sleep(0.1)
        be.halt()
        pc = be.read_register("pc") & ~1
        mg = be.read_uint32(0x20000000)
        print(f"  t~{t:>4}: PC=0x{pc:08X}  [{region(pc)}]  magic=0x{mg:08X}")
        be.resume()
        time.sleep(t)
finally:
    be.close()
