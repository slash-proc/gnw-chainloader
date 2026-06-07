#!/usr/bin/env python3
"""Disassemble a function from a stock OFW backup internal-flash image.

Originally written to read the button bit-packing out of stock read_buttons();
generalized to disassemble at any flash address so we can also trace the stock
reset handler / early init (e.g. the power-on gating that powers the device down
on a warm reset into the OFW).

Backups are raw internal flash starting at 0x08000000, so the file offset of an
address is (addr - 0x08000000).

Usage:
    # named function (read_buttons / reset = the stock Reset_Handler):
    python3 scripts/debug/disasm_stock_buttons.py mario read_buttons
    python3 scripts/debug/disasm_stock_buttons.py mario reset --bytes 200
    # arbitrary address:
    python3 scripts/debug/disasm_stock_buttons.py zelda 0x0801ad48 --bytes 300
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import resolve

FLASH_BASE = 0x08000000

# Named peripheral/SRAM addresses so resolved literal-pool loads read as meaning,
# not magic numbers. Extend freely as the RE turns up more registers.
KNOWN_ADDRS = {
    0x58024800: "PWR_CR1", 0x58024804: "PWR_CSR1", 0x58024808: "PWR_CR2",
    0x5802480C: "PWR_CR3", 0x58024810: "PWR_CPUCR(SBF=b6,STOPF=b5,CSSF=b9)",
    0x58024818: "PWR_SRDCR", 0x58024820: "PWR_WKUPCR", 0x58024824: "PWR_WKUPFR",
    0x58024828: "PWR_WKUPEPR",
    0x580244D0: "RCC_RSR", 0x58024530: "RCC_RSR(mirror)",
    0xE000ED10: "SCB_SCR(SLEEPDEEP=b2)",
    0x58020000: "GPIOA", 0x58020010: "GPIOA_IDR", 0x58020400: "GPIOB",
    0x58020410: "GPIOB_IDR", 0x58020800: "GPIOC", 0x58020810: "GPIOC_IDR",
    0x58020C00: "GPIOD", 0x58020C10: "GPIOD_IDR", 0x58021000: "GPIOE",
    0x58021010: "GPIOE_IDR",
    0x20001034: "g_state_struct (state byte @+0)",
}

_PC_LOAD = re.compile(r"\[pc, #(-?)0x([0-9a-fA-F]+)\]")


def read_u32(data: bytes, addr: int):
    """Read a little-endian 32-bit word at flash *addr* from the backup image."""
    off = addr - FLASH_BASE
    if 0 <= off <= len(data) - 4:
        return struct.unpack_from("<I", data, off)[0]
    return None


def literal_note(data: bytes, insn) -> str:
    """If *insn* is a PC-relative literal load, resolve and show the loaded value."""
    if "pc" not in insn.op_str:
        return ""
    m = _PC_LOAD.search(insn.op_str)
    if not m:
        return ""
    sign = -1 if m.group(1) == "-" else 1
    imm = sign * int(m.group(2), 16)
    pool_addr = ((insn.address + 4) & ~3) + imm   # Align(PC,4)+imm for Thumb literal loads
    val = read_u32(data, pool_addr)
    if val is None:
        return f"  ; lit@0x{pool_addr:08X} (out of image)"
    name = KNOWN_ADDRS.get(val)
    tag = f" {name}" if name else ""
    return f"  ; =0x{val:08X}{tag} (lit@0x{pool_addr:08X})"

# Stock function addresses (Thumb; the body is addr & ~1).
TARGETS = {
    "mario": {"backup": "backup/internal_flash_backup_mario.bin",
              "read_buttons": 0x08010D48, "reset": 0x08017A44},
    "zelda": {"backup": "backup/internal_flash_backup_zelda.bin",
              "read_buttons": 0x08016808, "reset": 0x0801AD48},
}

# GPIO peripheral base addresses (STM32H7) -> port letter, so IDR reads in the
# disassembly are legible. IDR is at base+0x10.
GPIO_BASES = {
    0x58020000: "A", 0x58020400: "B", 0x58020800: "C", 0x58020C00: "D",
    0x58021000: "E", 0x58021400: "F", 0x58021800: "G", 0x58021C00: "H",
}


def annotate(addr: int, operands: str) -> str:
    """Tag GPIO IDR references and bit masks to speed up manual mapping."""
    notes = []
    for base, letter in GPIO_BASES.items():
        if f"0x{base + 0x10:x}" in operands.lower() or f"0x{base:x}" in operands.lower():
            notes.append(f"GPIO{letter}")
    return ("  ; " + ", ".join(notes)) if notes else ""


def resolve_addr(info, what: str) -> int:
    """Map a named function ('read_buttons'/'reset') or a hex string to an address."""
    if what in info:
        return info[what]
    return int(what, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", choices=list(TARGETS))
    ap.add_argument("what", nargs="?", default="read_buttons",
                    help="named func (read_buttons|reset) or a hex address like 0x08017a44")
    ap.add_argument("--bytes", type=int, default=512, help="how many bytes to disassemble")
    ap.add_argument("--stop-at-return", action="store_true",
                    help="stop at the first bx/pop pc (single-function view)")
    ap.add_argument("--words", type=int, default=0, metavar="N",
                    help="dump N 32-bit words instead of disassembling (e.g. a jump table)")
    ap.add_argument("--file", default=None,
                    help="disassemble this raw flash image instead of the stock backup "
                         "(e.g. build/patched_internal_mario.bin to verify applied patches)")
    args = ap.parse_args()

    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    except ImportError:
        print("capstone not installed (pip install capstone)")
        return 1

    info = TARGETS[args.target]
    src = resolve(args.file) if args.file else resolve(info["backup"])
    data = src.read_bytes()
    func = resolve_addr(info, args.what) & ~1
    off = func - FLASH_BASE
    blob = data[off:off + args.bytes]

    if args.words:
        print(f"# {args.target} word dump @ 0x{func:08X} ({args.words} words)\n")
        for i in range(args.words):
            a = func + i * 4
            v = read_u32(data, a)
            if v is None:
                print(f"  [{i}] 0x{a:08X}: (out of image)")
                continue
            tag = KNOWN_ADDRS.get(v, "")
            # A plausible Thumb code pointer into this image (odd, within flash).
            if not tag and (v & 1) and FLASH_BASE <= (v & ~1) < FLASH_BASE + len(data):
                tag = f"-> code 0x{v & ~1:08X}"
            print(f"  [{i}] 0x{a:08X}: 0x{v:08X}  {tag}")
        return 0

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = False
    print(f"# {args.target} {args.what} @ 0x{func:08X} (file offset 0x{off:X})\n")
    for insn in md.disasm(blob, func):
        note = annotate(insn.address, insn.op_str) + literal_note(data, insn)
        print(f"0x{insn.address:08X}:  {insn.mnemonic:<7s} {insn.op_str}{note}")
        if args.stop_at_return and insn.mnemonic in ("bx", "pop") and \
           ("pc" in insn.op_str or "lr" in insn.op_str):
            print("# --- likely function end ---")
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
