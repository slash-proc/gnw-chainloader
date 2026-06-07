#!/usr/bin/env python3
"""
On-chip diagnostics dashboard for the Game & Watch (STM32H7B0).
Provides register dumps, fault analysis, option byte check, and LTDC state.
"""

import argparse
import sys
import time
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# SCB Registers
SCB_BASE = 0xE000ED00
SCB_ICSR = SCB_BASE + 0x04
SCB_VTOR = SCB_BASE + 0x08
SCB_CFSR = SCB_BASE + 0x28
SCB_HFSR = SCB_BASE + 0x2C
SCB_MMFAR = SCB_BASE + 0x34
SCB_BFAR = SCB_BASE + 0x38

# FLASH Registers (using correct CD_AHB3PERIPH base)
FLASH_R_BASE = 0x52002000
OPTCR = FLASH_R_BASE + 0x18
OPTSR_CUR = FLASH_R_BASE + 0x1C
OPTSR_PRG = FLASH_R_BASE + 0x20

# LTDC Registers
LTDC_BASE = 0x50001000
LTDC_GCR = LTDC_BASE + 0x018
LTDC_SRCR = LTDC_BASE + 0x024
LTDC_L1CR = LTDC_BASE + 0x084
LTDC_L1CFBAR = LTDC_BASE + 0x0AC
LTDC_L1WHPCR = LTDC_BASE + 0x088
LTDC_L1WVPCR = LTDC_BASE + 0x08C


def parse_cfsr(cfsr):
    reasons = []
    # UsageFault (UFSR) - top 16 bits of CFSR
    ufsr = (cfsr >> 16) & 0xFFFF
    if ufsr:
        reasons.append("  UsageFault:")
        if ufsr & (1 << 9): reasons.append("    - DIVBYZERO (Divide by zero)")
        if ufsr & (1 << 8): reasons.append("    - UNALIGNED (Unaligned access)")
        if ufsr & (1 << 3): reasons.append("    - NOCP (No coprocessor for instruction)")
        if ufsr & (1 << 2): reasons.append("    - INVPC (Invalid PC load or EXC_RETURN)")
        if ufsr & (1 << 1): reasons.append("    - INVSTATE (Invalid EPSR/xPSR state, e.g. ARM mode instead of Thumb)")
        if ufsr & (1 << 0): reasons.append("    - UNDEFINSTR (Undefined instruction executed)")

    # BusFault (BFSR) - middle 8 bits of CFSR
    bfsr = (cfsr >> 8) & 0xFF
    if bfsr:
        reasons.append("  BusFault:")
        if bfsr & (1 << 7): reasons.append("    - BFARVALID (BFAR contains valid fault address)")
        if bfsr & (1 << 5): reasons.append("    - LSPERR (Lazy state preservation error)")
        if bfsr & (1 << 4): reasons.append("    - STKERR (Stacking error on exception entry)")
        if bfsr & (1 << 3): reasons.append("    - UNSTKERR (Unstacking error on exception exit)")
        if bfsr & (1 << 2): reasons.append("    - IMPRESCISERR (Imprecise data bus error)")
        if bfsr & (1 << 1): reasons.append("    - PRECISERR (Precise data bus error)")
        if bfsr & (1 << 0): reasons.append("    - IBUSERR (Instruction bus error)")

    # MemManage (MMFSR) - bottom 8 bits of CFSR
    mmfsr = cfsr & 0xFF
    if mmfsr:
        reasons.append("  MemManageFault:")
        if mmfsr & (1 << 7): reasons.append("    - MMARVALID (MMFAR contains valid fault address)")
        if mmfsr & (1 << 5): reasons.append("    - MLSPERR (MemManage fault during lazy state preservation)")
        if mmfsr & (1 << 4): reasons.append("    - MSTKERR (MemManage fault during stacking)")
        if mmfsr & (1 << 3): reasons.append("    - MUNSTKERR (MemManage fault during unstacking)")
        if mmfsr & (1 << 1): reasons.append("    - DACCVIOL (Data access violation)")
        if mmfsr & (1 << 0): reasons.append("    - IACCVIOL (Instruction access violation)")

    return "\n".join(reasons) if reasons else "  No specific CFSR fault bits set."


def parse_hfsr(hfsr):
    reasons = []
    if hfsr & (1 << 31): reasons.append("    - DEBUGEVT (Debug event occurred)")
    if hfsr & (1 << 30): reasons.append("    - FORCED (Forced HardFault: configurable fault escalation)")
    if hfsr & (1 << 1):  reasons.append("    - VECTTBL (Vector table read fault during exception processing)")
    return "\n".join(reasons) if reasons else "  No specific HFSR fault bits set."


def dump_faults(backend):
    print("\n--- CPU Exception & Fault Status ---")
    try:
        # Read core registers
        regs = {}
        for reg in ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "xpsr"]:
            try:
                regs[reg] = backend.read_register(reg)
            except Exception:
                regs[reg] = 0xDEADBEEF

        print(f"  PC:   0x{regs['pc']:08X}   LR:   0x{regs['lr']:08X}   SP:   0x{regs['sp']:08X}")
        print(f"  R0:   0x{regs['r0']:08X}   R1:   0x{regs['r1']:08X}   R2:   0x{regs['r2']:08X}   R3:   0x{regs['r3']:08X}")
        print(f"  R4:   0x{regs['r4']:08X}   R5:   0x{regs['r5']:08X}   R6:   0x{regs['r6']:08X}   R7:   0x{regs['r7']:08X}")
        print(f"  R8:   0x{regs['r8']:08X}   R9:   0x{regs['r9']:08X}   R10:  0x{regs['r10']:08X}   R11:  0x{regs['r11']:08X}")
        print(f"  R12:  0x{regs['r12']:08X}   xPSR: 0x{regs['xpsr']:08X}")

        # Active exception
        ipsr = regs['xpsr'] & 0x1FF
        if ipsr > 0:
            exc_names = {2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault", 14: "PendSV", 15: "SysTick"}
            name = exc_names.get(ipsr, f"External Interrupt #{ipsr - 16}")
            print(f"  Active Exception: {ipsr} ({name})")
        else:
            print("  Active Exception: None (Thread Mode)")

        # SCB Status
        vtor = backend.read_uint32(SCB_VTOR)
        icsr = backend.read_uint32(SCB_ICSR)
        cfsr = backend.read_uint32(SCB_CFSR)
        hfsr = backend.read_uint32(SCB_HFSR)
        
        print(f"  VTOR: 0x{vtor:08X}")
        print(f"  ICSR: 0x{icsr:08X} (VECTACTIVE: {icsr & 0x1FF}, VECTPENDING: {(icsr >> 12) & 0x1FF})")
        print(f"  CFSR: 0x{cfsr:08X}")
        print(parse_cfsr(cfsr))
        print(f"  HFSR: 0x{hfsr:08X}")
        print(parse_hfsr(hfsr))

        if (cfsr >> 8) & (1 << 7):  # BFARVALID
            bfar = backend.read_uint32(SCB_BFAR)
            print(f"  BFAR (BusFault Address): 0x{bfar:08X}")
        if cfsr & (1 << 7):  # MMARVALID
            mmfar = backend.read_uint32(SCB_MMFAR)
            print(f"  MMFAR (MemManage Address): 0x{mmfar:08X}")

    except Exception as e:
        print(f"  Error reading CPU/fault registers: {e}")


def dump_swap(backend):
    print("\n--- Option Bytes & Bank Swap State ---")
    try:
        optcr = backend.read_uint32(OPTCR)
        optsr_cur = backend.read_uint32(OPTSR_CUR)
        optsr_prg = backend.read_uint32(OPTSR_PRG)

        print(f"  OPTCR:     0x{optcr:08X} (Locked: {bool(optcr & 1)})")
        print(f"  OPTSR_CUR: 0x{optsr_cur:08X}")
        print(f"  OPTSR_PRG: 0x{optsr_prg:08X}")

        swapped = (optsr_cur & 0x80000000) != 0
        swapped_prg = (optsr_prg & 0x80000000) != 0
        print(f"  Current Swap Configuration: {'Bank 2 @ 0x08000000 (OFW)' if swapped else 'Bank 1 @ 0x08000000 (Chainloader)'}")
        print(f"  Programmed Swap (next boot): {'Bank 2 @ 0x08000000' if swapped_prg else 'Bank 1 @ 0x08000000'}")
    except Exception as e:
        print(f"  Error reading Option Bytes: {e}")


def dump_ltdc(backend):
    print("\n--- LTDC (Display Controller) Configuration ---")
    try:
        gcr = backend.read_uint32(LTDC_GCR)
        l1cr = backend.read_uint32(LTDC_L1CR)
        cfbar = backend.read_uint32(LTDC_L1CFBAR)
        whpcr = backend.read_uint32(LTDC_L1WHPCR)
        wvport = backend.read_uint32(LTDC_L1WVPCR)
        srcr = backend.read_uint32(LTDC_SRCR)

        print(f"  LTDC_GCR:     0x{gcr:08X} (Enabled: {bool(gcr & 1)})")
        print(f"  LTDC_L1CR:    0x{l1cr:08X} (Layer Enabled: {bool(l1cr & 1)}, CLUT Enabled: {bool(l1cr & 16)})")
        print(f"  LTDC_L1CFBAR: 0x{cfbar:08X}")
        print(f"  Window Config: Hpos=0x{whpcr:08X}, Vpos=0x{wvport:08X}")
        print(f"  LTDC_SRCR:    0x{srcr:08X} (Reload Pending: {srcr & 3})")
    except Exception as e:
        print(f"  Error reading LTDC: {e}")


def dump_fb_stats(backend, fb_addr, size):
    print(f"\n--- Framebuffer Pixel Stats (Address: 0x{fb_addr:08X}, Size: {size} bytes) ---")
    try:
        fb_data = backend.read_memory(fb_addr, size)
        unique = sorted(list(set(fb_data)))
        counts = {val: fb_data.count(val) for val in unique}
        print(f"  Total pixels sampled: {len(fb_data)}")
        print(f"  Unique color indices ({len(unique)}): {unique[:20]}..." if len(unique) > 20 else f"Unique color indices ({len(unique)}): {unique}")
        print("  Pixel index distribution (top 5):")
        sorted_counts = sorted(counts.items(), key=lambda x: x[1], reverse=True)
        for val, count in sorted_counts[:5]:
            pct = (count / len(fb_data)) * 100
            print(f"    - Index {val:3d}: {count:6d} pixels ({pct:5.1f}%)")
    except Exception as e:
        print(f"  Error reading framebuffer: {e}")


def main():
    parser = argparse.ArgumentParser(
        description="On-chip diagnostics dashboard for the Game & Watch (STM32H7B0)."
    )
    parser.add_argument("--faults", action="store_true", help="Dump CPU registers and fault registers")
    parser.add_argument("--swap", action="store_true", help="Dump Bank Swap Option Bytes status")
    parser.add_argument("--ltdc", action="store_true", help="Dump LTDC display controller config")
    parser.add_argument("--fb-stats", action="store_true", help="Dump framebuffer pixel statistics")
    parser.add_argument("--fb-addr", type=lambda x: int(x, 0), default=0x240002C0, help="Framebuffer start address")
    parser.add_argument("--fb-size", type=int, default=1024, help="Size of framebuffer block to read")
    parser.add_argument("--no-halt", action="store_true", help="Do not halt the target CPU before reading registers")

    args = parser.parse_args()

    # If no specific action is selected, run all diagnostics
    run_all = not (args.faults or args.swap or args.ltdc or args.fb_stats)
    run_faults = args.faults or run_all
    run_swap = args.swap or run_all
    run_ltdc = args.ltdc or run_all
    run_fb_stats = args.fb_stats or run_all

    backend = OpenOCDBackend()
    backend.open()
    try:
        if not args.no_halt:
            print("Halting target CPU...")
            backend.halt()

        if run_faults:
            dump_faults(backend)
        if run_swap:
            dump_swap(backend)
        if run_ltdc:
            dump_ltdc(backend)
        if run_fb_stats:
            dump_fb_stats(backend, args.fb_addr, args.fb_size)

        if not args.no_halt:
            print("\nResuming target CPU...")
            backend.resume()
    finally:
        backend.close()


if __name__ == "__main__":
    main()
