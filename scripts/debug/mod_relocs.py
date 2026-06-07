#!/usr/bin/env python3
"""Report a PIE module's R_ARM_RELATIVE relocations split by the data_offset boundary.

For XIP, a module's .text is mapped in external flash (read-only): the loader CANNOT apply a
relocation that targets it. The two-base loader treats relocations below __data_start (data_offset)
as text-base and at/above as data-base, so any R_ARM_RELATIVE BELOW data_offset is an unapplied
.text fixup that breaks XIP. A fully position-independent module (-fPIC / -msingle-pic-base, all
data reached via the GOT) has ZERO of them -- every fixup lands in .got/.data (RAM, relocatable).

Usage: mod_relocs.py <module.elf>
Exit 0 if XIP-safe (no .text relocations), 1 otherwise.
"""
import re
import subprocess
import sys


HEADER_SIZE = 596  # sizeof(module_header_t) in system/module.h; the loader skips relocs below it


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    elf = sys.argv[1]

    nm = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    data_off = None
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == "__data_start":
            data_off = int(parts[0], 16)
    if data_off is None:
        print("error: no __data_start symbol in", elf)
        return 2

    rel = subprocess.run(["arm-none-eabi-readelf", "-r", elf], capture_output=True, text=True).stdout
    header_relocs, text_relocs, data_relocs = [], [], []
    for line in rel.splitlines():
        m = re.match(r"^([0-9a-fA-F]{8})\s+\S+\s+R_ARM_RELATIVE", line)
        if m:
            off = int(m.group(1), 16)
            if off < HEADER_SIZE:
                header_relocs.append(off)   # entry/reloc/bss/data offsets the loader consumes itself
            elif off < data_off:
                text_relocs.append(off)     # a true .text fixup: unapplyable in read-only flash
            else:
                data_relocs.append(off)     # .got/.data: relocated into the RAM slot, fine

    print(f"{elf}")
    print(f"  data_offset (__data_start) = 0x{data_off:x}")
    print(f"  header  (< 0x{HEADER_SIZE:x}, consumed + skipped by loader): {len(header_relocs)}")
    print(f"  .data/.got (>= data_offset, OK):                   {len(data_relocs)}")
    print(f"  .text   (XIP-BAD, can't relocate read-only flash):  {len(text_relocs)}")
    if text_relocs:
        print("  offending .text reloc offsets:", ", ".join(f"0x{o:x}" for o in text_relocs[:24]))
        print("  -> NOT XIP-safe.")
        return 1
    print("  -> XIP-safe: every non-header fixup lands in RAM (.got/.data).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
