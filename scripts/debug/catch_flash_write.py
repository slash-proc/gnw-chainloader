#!/usr/bin/env python3
"""Catch the external-flash write that clobbers the Link's Awakening ROM.

The Zelda OFW's flash writes all funnel through two primitives (addresses given
for the OFW running bank-swapped at 0x0800_0000):
    0x0800e2d6  erase-check  (destination sector typically in r2)
    0x0800e35c  program      (destination typically in r1)

This arms hardware breakpoints on both, resumes, and on every hit prints the
register file + caller (lr). Legit saves target the documented save region
(< 0x12000) or the save-state region (>= 0x3E0000); the bug writes into the LA
ROM region [0x0D2000, 0x1F4C00). When a destination register lands in that ROM
window the tool STOPS and reports the caller, so we learn exactly which routine
issued the bad write and how it computed the address.

Usage:
    python3 scripts/debug/catch_flash_write.py [--timeout SEC] [--max-hits N]
"""
import argparse
import sys
import time

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

BPS = [0x0800E2D6, 0x0800E35C]          # erase-check, program
ROM_LO, ROM_HI = 0x000D2000, 0x001F4C00  # LA ROM region (ext-flash offsets)
EXT_LO, EXT_HI = 0x00000000, 0x04000000  # plausible ext-flash offset range


def in_rom(v):
    # The dest may be an ext-flash offset (0x000D9000) or memory-mapped
    # (0x900D9000); normalise the high byte and test the ROM window.
    off = v & 0x0FFFFFFF
    return ROM_LO <= off < ROM_HI


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--timeout", type=float, default=90.0, help="overall seconds to wait")
    ap.add_argument("--max-hits", type=int, default=400, help="max breakpoint hits to inspect")
    args = ap.parse_args()

    be = OpenOCDBackend()
    be.open()
    try:
        be.halt()
        for bp in BPS:
            be(f"bp 0x{bp:08X} 2 hw", decode=False)
        print(f"Armed HW breakpoints: {', '.join(f'0x{b:08X}' for b in BPS)}")
        print("Resuming OFW. >>> Create the user profile now. <<<\n")
        be.resume()

        # Non-intrusive halt detection: poll DHCSR.S_HALT (works while the core
        # runs, so the game stays full-speed). wait_halt is unreliable here (it
        # doesn't raise on timeout, which left read_register reading a running
        # core). Guard every probe read against transient empty responses.
        DHCSR = 0xE000EDF0
        S_HALT = 0x00020000
        deadline = time.time() + args.timeout
        hits = 0
        caught = False
        last_beat = 0.0
        while time.time() < deadline and hits < args.max_hits:
            time.sleep(0.03)
            try:
                if not (be.read_uint32(DHCSR) & S_HALT):
                    now = time.time()
                    if now - last_beat > 15:        # liveness heartbeat
                        last_beat = now
                        print(f"... waiting ({int(deadline - now)}s left, {hits} writes seen)", flush=True)
                    continue                        # still running; no breakpoint yet
                pc = be.read_register("pc") & ~1
            except Exception:
                continue                            # transient probe read; retry
            if pc not in BPS:
                be.resume()                         # halted for another reason
                continue

            r = {n: be.read_register(n) for n in ("r0", "r1", "r2", "r3", "lr", "sp")}
            hits += 1
            which = "erase" if pc == 0x0800E2D6 else "program"
            # Flag any register sitting in the ext-flash range as a candidate dest.
            cands = {n: v for n, v in r.items()
                     if n in ("r0", "r1", "r2", "r3") and EXT_LO <= (v & 0x0FFFFFFF) < EXT_HI}
            tag = ""
            if any(in_rom(v) for v in r.values()):
                tag = "   <<< ROM-REGION WRITE — CLOBBER CAUGHT"
                caught = True
            print(f"[{hits:3d}] {which} pc=0x{pc:08X} "
                  f"r0=0x{r['r0']:08X} r1=0x{r['r1']:08X} r2=0x{r['r2']:08X} "
                  f"r3=0x{r['r3']:08X} lr=0x{r['lr']:08X}{tag}")
            if caught:
                print(f"\n>>> Caller (lr) = 0x{r['lr']:08X}. Dest candidates: "
                      + ", ".join(f"{n}=0x{v:08X}" for n, v in cands.items()))
                print(">>> Leaving CPU halted here for inspection.")
                break
            be.resume()

        if not caught:
            print(f"\nNo ROM-region write seen in {hits} hit(s) / {args.timeout}s.")
    finally:
        for bp in BPS:
            try:
                be(f"rbp 0x{bp:08X}", decode=False)
            except Exception:
                pass


if __name__ == "__main__":
    main()
