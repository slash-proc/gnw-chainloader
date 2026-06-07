#!/usr/bin/env python3
"""Recover the device/probe after a debug session and report the halt state.

A crashed hardware-breakpoint session (e.g. catch_flash_write.py killed mid-run)
can leave breakpoints armed in the debug unit. The OFW then halts on its next
flash write and looks frozen. This tool halts, dumps the CPU/fault state (so if
it IS stuck at a flash-write breakpoint we capture the destination + caller),
clears the leftover breakpoints, and -- only if asked -- resumes or resets.

Default: diagnose + clear breakpoints, leave the core halted.
  --resume   resume execution after clearing (unfreeze in place)
  --reset    reset+halt then resume (clean reboot; abandons any in-flight write)

Usage: python3 scripts/debug/recover_probe.py [--bp ADDR ...] [--resume | --reset]
"""
import argparse
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

DEFAULT_BPS = [0x0800E2D6, 0x0800E35C]


def rd(be, name):
    try:
        return be.read_register(name)
    except Exception:
        return None


def h(v):
    return "????????" if v is None else f"0x{v:08X}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bp", type=lambda x: int(x, 0), action="append", default=None,
                    help="breakpoint address(es) to remove (default: the flash primitives)")
    ap.add_argument("--resume", action="store_true", help="resume after clearing")
    ap.add_argument("--reset", action="store_true", help="reset+halt+resume after clearing")
    args = ap.parse_args()
    bps = args.bp if args.bp else DEFAULT_BPS

    be = OpenOCDBackend()
    be.open()
    be.halt()

    regs = {n: rd(be, n) for n in ("pc", "sp", "lr", "r0", "r1", "r2", "r3")}
    print("=== Halt-state ===")
    print(f"  PC={h(regs['pc'])}  SP={h(regs['sp'])}  LR={h(regs['lr'])}")
    print(f"  r0={h(regs['r0'])}  r1={h(regs['r1'])}  r2={h(regs['r2'])}  r3={h(regs['r3'])}")
    try:
        cfsr = be.read_uint32(0xE000ED28)
        hfsr = be.read_uint32(0xE000ED2C)
        print(f"  CFSR=0x{cfsr:08X}  HFSR=0x{hfsr:08X}  (nonzero => a fault occurred)")
    except Exception:
        pass

    pc = regs["pc"]
    if pc is not None and (pc & ~1) in (0x0800E2D6, 0x0800E35C):
        print("  >>> Halted INSIDE a flash primitive -- r0..r3 hold its args (destination among them).")

    try:
        print("Breakpoints:\n" + str(be("bp", decode=False)).rstrip())
    except Exception:
        pass
    for bp in bps:
        try:
            be(f"rbp 0x{bp:08X}", decode=False)
            print(f"  removed breakpoint 0x{bp:08X}")
        except Exception as e:
            print(f"  rbp 0x{bp:08X} failed: {e}")

    if args.reset:
        be("reset halt", decode=False)
        print("reset+halt done")
        be.resume()
        print("resumed after reset")
    elif args.resume:
        be.resume()
        print("resumed")
    else:
        print("Left core HALTED (breakpoints cleared). Re-run with --resume or --reset to recover.")


if __name__ == "__main__":
    main()
