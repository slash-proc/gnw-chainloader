#!/usr/bin/env python3
import elftools.elf.elffile as elffile
import sys
from pathlib import Path

def main():
    elf_path = Path("build/modules/features/mp3/mp3.elf")
    if not elf_path.exists():
        print(f"File not found: {elf_path}")
        return
        
    pattern = b"\xd4\xf8\x50\x32"
    with open(elf_path, "rb") as f:
        elf = elffile.ELFFile(f)
        for section in elf.iter_sections():
            if section.name == ".text":
                data = section.data()
                idx = data.find(pattern)
                while idx != -1:
                    offset = section['sh_addr'] + idx
                    print(f"Found pattern at relative address 0x{offset:08X} in section .text")
                    idx = data.find(pattern, idx + 1)

if __name__ == "__main__":
    main()
