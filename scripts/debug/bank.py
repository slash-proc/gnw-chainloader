#!/usr/bin/env python3
"""
Option Bytes & Bank Swapping utility for the Game & Watch (STM32H7B0).
Supports checking swap status, unlocking option bytes, and triggering bank swaps.
"""

import argparse
import sys
import time
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# FLASH Registers (using correct CD_AHB3PERIPH base)
FLASH_R_BASE = 0x52002000
OPTKEYR = FLASH_R_BASE + 0x08
OPTCR = FLASH_R_BASE + 0x18
OPTSR_CUR = FLASH_R_BASE + 0x1C
OPTSR_PRG = FLASH_R_BASE + 0x20
OPTCCR = FLASH_R_BASE + 0x24
FLASH_CCR1 = FLASH_R_BASE + 0x14
FLASH_CCR2 = FLASH_R_BASE + 0x114

OPTKEY1 = 0x08192A3B
OPTKEY2 = 0x4C5D6E7F
SWAP_BANK = 0x80000000
OPT_BUSY = 0x00000001
OPTLOCK = 0x00000001
OPTSTART = 0x00000002
CLR_OPTCHANGEERR = 0x40000000
FLASH_ERRORS_BANK1 = 0x05FF0000
FLASH_ERRORS_BANK2 = 0x05FF0000


def wait_not_busy(backend, timeout=2.0):
    deadline = time.time() + timeout
    while backend.read_uint32(OPTSR_CUR) & OPT_BUSY:
        if time.time() > deadline:
            print("  WARNING: OPT_BUSY did not clear within timeout.")
            return False
        time.sleep(0.01)
    return True


def do_status(backend):
    optcr = backend.read_uint32(OPTCR)
    optsr_cur = backend.read_uint32(OPTSR_CUR)
    optsr_prg = backend.read_uint32(OPTSR_PRG)
    ccr1 = backend.read_uint32(FLASH_CCR1)
    ccr2 = backend.read_uint32(FLASH_CCR2)
    
    swapped_cur = bool(optsr_cur & SWAP_BANK)
    swapped_prg = bool(optsr_prg & SWAP_BANK)
    
    print("\n--- Option Bytes & Bank Swap State ---")
    print(f"  FLASH Base Address: 0x{FLASH_R_BASE:08X}")
    print(f"  OPTCR:              0x{optcr:08X} (Locked: {bool(optcr & OPTLOCK)})")
    print(f"  OPTSR_CUR:          0x{optsr_cur:08X} (SWAP_BANK: {swapped_cur}, BUSY: {bool(optsr_cur & OPT_BUSY)})")
    print(f"  OPTSR_PRG:          0x{optsr_prg:08X} (SWAP_BANK: {swapped_prg})")
    print(f"  FLASH_CCR1:         0x{ccr1:08X}")
    print(f"  FLASH_CCR2:         0x{ccr2:08X}")
    print(f"  Active Bank Mapping: {'Bank 2 @ 0x08000000 (OFW)' if swapped_cur else 'Bank 1 @ 0x08000000 (Chainloader)'}")
    print(f"  Next Boot Mapping:   {'Bank 2 @ 0x08000000' if swapped_prg else 'Bank 1 @ 0x08000000'}")


def do_unlock(backend):
    optcr = backend.read_uint32(OPTCR)
    if optcr & OPTLOCK:
        print("Unlocking Option Bytes...")
        backend.write_uint32(OPTKEYR, OPTKEY1)
        backend.write_uint32(OPTKEYR, OPTKEY2)
        optcr = backend.read_uint32(OPTCR)
        if optcr & OPTLOCK:
            print("FAILED to unlock Option Bytes (still locked).")
            return False
        else:
            print("SUCCESS: Option Bytes unlocked.")
            return True
    else:
        print("Option Bytes already unlocked.")
        return True


def do_lock(backend):
    optcr = backend.read_uint32(OPTCR)
    if not (optcr & OPTLOCK):
        print("Locking Option Bytes...")
        backend.write_uint32(OPTCR, optcr | OPTLOCK)
        optcr = backend.read_uint32(OPTCR)
        if optcr & OPTLOCK:
            print("SUCCESS: Option Bytes locked.")
            return True
        else:
            print("FAILED to lock Option Bytes.")
            return False
    else:
        print("Option Bytes already locked.")
        return True


def do_clear_err(backend):
    print("Clearing Option Byte change errors and FLASH errors...")
    backend.write_uint32(FLASH_CCR1, FLASH_ERRORS_BANK1)
    backend.write_uint32(FLASH_CCR2, FLASH_ERRORS_BANK2)
    backend.write_uint32(OPTCCR, CLR_OPTCHANGEERR)
    print("Errors cleared.")


def do_swap(backend, swap_to_bank2, reset=True, halt=False):
    optsr_cur = backend.read_uint32(OPTSR_CUR)
    is_currently_swapped = bool(optsr_cur & SWAP_BANK)
    
    action = "Bank 2 @ 0x08000000 (OFW)" if swap_to_bank2 else "Bank 1 @ 0x08000000 (Chainloader)"
    print(f"Initiating swap to {action}...")
    
    # 1. Clear error registers to ensure the write is accepted
    do_clear_err(backend)
    
    # 2. Unlock option bytes
    if not do_unlock(backend):
        return False
        
    # 3. Read OPTSR_PRG, set/clear SWAP_BANK bit, write back
    optsr_prg = backend.read_uint32(OPTSR_PRG)
    if swap_to_bank2:
        new_optsr_prg = optsr_prg | SWAP_BANK
    else:
        new_optsr_prg = optsr_prg & ~SWAP_BANK
        
    backend.write_uint32(OPTSR_PRG, new_optsr_prg)
    print(f"OPTSR_PRG updated to: 0x{new_optsr_prg:08X} (readback: 0x{backend.read_uint32(OPTSR_PRG):08X})")
    
    # 4. Launch change by setting OPTSTART in OPTCR
    optcr = backend.read_uint32(OPTCR)
    print("Launching Option Byte write (OPTSTART)...")
    backend.write_uint32(OPTCR, optcr | OPTSTART)
    
    # 5. Wait for busy to clear
    if not wait_not_busy(backend):
        print("Option Byte write timed out.")
        
    # 6. Lock Option Bytes back
    do_lock(backend)
    
    # 7. Check status
    optsr_cur = backend.read_uint32(OPTSR_CUR)
    print(f"Post-launch OPTSR_CUR: 0x{optsr_cur:08X}")
    
    if reset:
        if halt:
            print("Resetting and halting CPU to apply new bank mapping...")
            backend.reset_and_halt()
            pc = backend.read_register("pc")
            sp = backend.read_register("sp")
            cur = backend.read_uint32(OPTSR_CUR)
            print(f"Halted: PC=0x{pc:08X} SP=0x{sp:08X}")
            print(f"OPTSR_CUR after reset: 0x{cur:08X} (SWAP {'SET' if cur & SWAP_BANK else 'CLEARED'})")
        else:
            print("Resetting CPU to apply new bank mapping...")
            backend.reset()
            
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Option Bytes & Bank Swapping utility for the Game & Watch (STM32H7B0)."
    )
    parser.add_argument("--no-halt", action="store_true", help="Do not halt the target CPU before operations")
    subparsers = parser.add_subparsers(dest="command", required=True, help="Subcommand to execute")

    # Status
    subparsers.add_parser("status", help="Read and print option byte and bank swap status")

    # Unlock
    subparsers.add_parser("unlock", help="Unlock Option Bytes register modifications")

    # Lock
    subparsers.add_parser("lock", help="Lock Option Bytes register modifications")

    # Clear-err
    subparsers.add_parser("clear-err", help="Clear latched Option Byte change errors and Flash errors")

    # Swap
    swap_parser = subparsers.add_parser("swap", help="Configure swap so Bank 2 (OFW) maps to 0x08000000")
    swap_parser.add_argument("--no-reset", action="store_true", help="Do not perform system reset after configuring")
    swap_parser.add_argument("--halt", action="store_true", help="Reset and halt (instead of normal run)")

    # Unswap
    unswap_parser = subparsers.add_parser("unswap", help="Configure swap so Bank 1 (Chainloader) maps to 0x08000000")
    unswap_parser.add_argument("--no-reset", action="store_true", help="Do not perform system reset after configuring")
    unswap_parser.add_argument("--halt", action="store_true", help="Reset and halt (instead of normal run)")

    args = parser.parse_args()

    backend = OpenOCDBackend()
    backend.open()
    try:
        if not args.no_halt:
            backend.halt()
            
        if args.command == "status":
            do_status(backend)
        elif args.command == "unlock":
            do_unlock(backend)
        elif args.command == "lock":
            do_lock(backend)
        elif args.command == "clear-err":
            do_clear_err(backend)
        elif args.command == "swap":
            do_swap(backend, swap_to_bank2=True, reset=not args.no_reset, halt=args.halt)
        elif args.command == "unswap":
            do_swap(backend, swap_to_bank2=False, reset=not args.no_reset, halt=args.halt)
            
        if not args.no_halt and args.command in ["status", "unlock", "lock", "clear-err"]:
            backend.resume()
    finally:
        backend.close()


if __name__ == "__main__":
    main()
