#!/usr/bin/env python3
"""Read the chainloader's persistent crash log out of D3 SRD-SRAM over SWD.

On a fault the chainloader's HardFault handler records the faulting context into
D3 SRAM (0x38000000) and halts (see src/chainloader/system/crash_log.c). This
tool reads that record back, decodes the fault-status registers, and prints the
stacked exception frame — so a crash that happened minutes ago can still be
diagnosed without a live debug session attached at fault time.

  crash_log.py            # read + decode the current record
  crash_log.py --clear    # invalidate the record (zero its magic)
  crash_log.py --trigger  # CRASH_TEST builds only: poke the device to fault now
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.common import device

CRASH_LOG_ADDR = 0x38000000
CRASH_LOG_MAGIC = 0xBADC0DE5
LOG_FMT = "<14I"                       # magic,count,hfsr,cfsr,mmfar,bfar,r0..r3,r12,lr,pc,psr
LOG_BYTES = struct.calcsize(LOG_FMT)
CRASH_TEST_CELL = 0x2001FF10           # matches CRASH_TEST_CELL in crash_log.c

# CFSR sub-field bits (ARMv7-M): UFSR[31:16] | BFSR[15:8] | MMFSR[7:0].
CFSR_BITS = [
    (0,  "IACCVIOL  (MemManage: instruction-fetch access violation)"),
    (1,  "DACCVIOL  (MemManage: data access violation)"),
    (3,  "MUNSTKERR (MemManage: fault on exception return unstacking)"),
    (4,  "MSTKERR   (MemManage: fault on exception entry stacking)"),
    (5,  "MLSPERR   (MemManage: fault during lazy FP state save)"),
    (7,  "MMARVALID (MMFAR holds the faulting address)"),
    (8,  "IBUSERR   (BusFault: instruction prefetch)"),
    (9,  "PRECISERR (BusFault: precise data bus error)"),
    (10, "IMPRECISERR (BusFault: imprecise data bus error)"),
    (11, "UNSTKERR  (BusFault: fault on exception-return unstacking)"),
    (12, "STKERR    (BusFault: fault on exception-entry stacking)"),
    (13, "LSPERR    (BusFault: fault during lazy FP state save)"),
    (15, "BFARVALID (BFAR holds the faulting address)"),
    (16, "UNDEFINSTR (UsageFault: undefined instruction)"),
    (17, "INVSTATE  (UsageFault: invalid EPSR/Thumb state)"),
    (18, "INVPC     (UsageFault: invalid PC load on exception return)"),
    (19, "NOCP      (UsageFault: coprocessor access / FP disabled)"),
    (20, "STKOF     (UsageFault: stack overflow)"),
    (24, "UNALIGNED (UsageFault: unaligned access)"),
    (25, "DIVBYZERO (UsageFault: divide by zero)"),
]
HFSR_BITS = [
    (1,  "VECTTBL  (bus fault reading the vector table)"),
    (30, "FORCED   (escalated from a configurable fault — see CFSR)"),
    (31, "DEBUGEVT (debug event)"),
]


def _decode_bits(value: int, table) -> list[str]:
    return [name for bit, name in table if value & (1 << bit)]


def read_log(backend):
    raw = device.swd_read_mem(backend, CRASH_LOG_ADDR, LOG_BYTES)
    f = struct.unpack(LOG_FMT, raw)
    keys = ("magic", "count", "hfsr", "cfsr", "mmfar", "bfar",
            "r0", "r1", "r2", "r3", "r12", "lr", "pc", "psr")
    return dict(zip(keys, f))


def print_log(log: dict) -> None:
    if log["magic"] != CRASH_LOG_MAGIC:
        print(f"No crash recorded (magic 0x{log['magic']:08X} != 0x{CRASH_LOG_MAGIC:08X}).")
        return
    print(f"=== Crash log @ 0x{CRASH_LOG_ADDR:08X} ===")
    print(f"  Crashes recorded : {log['count']}")
    print(f"  HFSR  : 0x{log['hfsr']:08X}")
    for n in _decode_bits(log["hfsr"], HFSR_BITS):
        print(f"            - {n}")
    print(f"  CFSR  : 0x{log['cfsr']:08X}")
    causes = _decode_bits(log["cfsr"], CFSR_BITS)
    for n in causes:
        print(f"            - {n}")
    if not causes:
        print("            (no configurable-fault bits set — likely a pure HardFault)")
    if log["cfsr"] & (1 << 7):
        print(f"  MMFAR : 0x{log['mmfar']:08X}  (MemManage faulting address)")
    if log["cfsr"] & (1 << 15):
        print(f"  BFAR  : 0x{log['bfar']:08X}  (Bus faulting address)")
    print("  Faulting context (stacked exception frame):")
    print(f"    PC =0x{log['pc']:08X}   LR =0x{log['lr']:08X}   xPSR=0x{log['psr']:08X}")
    print(f"    R0 =0x{log['r0']:08X}   R1 =0x{log['r1']:08X}   R2 =0x{log['r2']:08X}   R3 =0x{log['r3']:08X}")
    print(f"    R12=0x{log['r12']:08X}")
    print(f"\n  → PC 0x{log['pc']:08X} is the faulting instruction; resolve it with")
    print(f"    arm-none-eabi-addr2line -e build/app/app.elf 0x{log['pc']:08X}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clear", action="store_true", help="invalidate the record (zero its magic)")
    ap.add_argument("--trigger", action="store_true",
                    help="CRASH_TEST builds only: write the test cell so the device faults now")
    args = ap.parse_args()

    with device.open_backend(halt=False) as backend:
        if args.trigger:
            device.swd_write(backend, CRASH_TEST_CELL, 1)
            print(f"Wrote 1 to 0x{CRASH_TEST_CELL:08X} — device should fault on its next menu frame.")
            print("Re-run without --trigger to read the recorded crash.")
            return
        if args.clear:
            device.swd_write(backend, CRASH_LOG_ADDR, 0)
            print("Crash log cleared (magic zeroed).")
            return
        print_log(read_log(backend))


if __name__ == "__main__":
    main()
