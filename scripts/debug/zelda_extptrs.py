#!/usr/bin/env python3
"""Enumerate every external-flash pointer in the Zelda OFW internal firmware.

For external-flash OFFSET support, every memory-mapped pointer into Zelda's
external data (a 4-byte word in [0x90000000, 0x90400000)) must be shifted by
offset_size when the external image is relocated. Missing one => that asset or
external function reads from the un-offset address => crash/corruption. So we
enumerate them *reliably* and classify each, instead of eyeballing:

  TABLE  - part of a run of >=2 consecutive 4-byte-aligned external words
           (a pointer table; high confidence it's a real pointer)
  LDR    - the word is loaded by a `ldr rX, [pc, #..] @ (addr)` in the disasm
           (a code-referenced pointer; high confidence real)
  ORPHAN - a lone aligned external word that is neither in a table nor
           ldr-referenced (most likely a misdisassembled code byte; REVIEW
           before offsetting -- offsetting a code word would corrupt the code)

Also flags movw/movt pairs that build an external address inline (a different
mechanism the word-scan can't see).

Usage: python3 scripts/debug/zelda_extptrs.py
       (expects backup/internal_flash_backup_zelda.bin and build/ofw_zelda.dis)
"""
import re
from pathlib import Path

FW = Path("backup/internal_flash_backup_zelda.bin")
DIS = Path("build/ofw_zelda.dis")
LOAD = 0x08000000
LO, HI = 0x90000000, 0x90400000   # Zelda external mapped range (~4 MB)

data = FW.read_bytes()


def word(off):
    return int.from_bytes(data[off:off + 4], "little")


# --- all 4-byte-aligned external words ---
ext = {off: word(off) for off in range(0, len(data) - 3, 4) if LO <= word(off) < HI}

# --- TABLE: clusters of external words spaced <= STRIDE apart (catches strided
#     pointer tables like FW-data struct arrays, not just consecutive +4) ---
STRIDE = 0x1C
table = set()
offs = sorted(ext)
i = 0
while i < len(offs):
    j = i
    while j + 1 < len(offs) and offs[j + 1] - offs[j] <= STRIDE:
        j += 1
    if j > i:  # cluster of >=2
        table.update(offs[i:j + 1])
    i = j + 1

# --- LDR: literal-pool words referenced by `ldr ... @ (0x..)` ---
ldr_ref = set()
if DIS.exists():
    for line in DIS.read_text(errors="ignore").splitlines():
        if "ldr" not in line:
            continue
        m = re.search(r"@ \(0x0?([0-9a-fA-F]+)\)", line)
        if not m:
            continue
        off = int(m.group(1), 16) - LOAD
        if off in ext:
            ldr_ref.add(off)

# --- movw/movt inline external addresses (different mechanism) ---
movw_movt = []
if DIS.exists():
    lines = DIS.read_text(errors="ignore").splitlines()
    for k in range(len(lines) - 1):
        mw = re.search(r":\s+[0-9a-f ]+\s+movw\s+(r\d+), #(\d+)", lines[k])
        mt = re.search(r":\s+[0-9a-f ]+\s+movt\s+(r\d+), #(\d+)", lines[k + 1])
        if mw and mt and mw.group(1) == mt.group(1):
            val = (int(mt.group(2)) << 16) | int(mw.group(2))
            if LO <= val < HI:
                addr = re.match(r"\s*([0-9a-f]+):", lines[k])
                movw_movt.append((addr.group(1) if addr else "?", mw.group(1), val))

# --- report ---
tbl_offs = sorted(table)
ldr_only = sorted(o for o in ext if o in ldr_ref and o not in table)
orphan = sorted(o for o in ext if o not in table and o not in ldr_ref)

print(f"Total aligned external words (0x90000000-0x90400000): {len(ext)}")
print(f"  in TABLES (>=2 consecutive): {len(tbl_offs)}")
print(f"  LDR-referenced singletons:   {len(ldr_only)}")
print(f"  ORPHANS (review!):           {len(orphan)}")
print(f"  movw/movt inline addrs:      {len(movw_movt)}")

print("\n=== TABLES (contiguous pointer runs) ===")
i = 0
while i < len(tbl_offs):
    j = i
    while j + 1 < len(tbl_offs) and tbl_offs[j + 1] == tbl_offs[j] + 4:
        j += 1
    n = j - i + 1
    print(f"  0x{tbl_offs[i]:06X}..0x{tbl_offs[j]+3:06X}  {n} entries  "
          f"(e.g. 0x{ext[tbl_offs[i]]:08X} .. 0x{ext[tbl_offs[j]]:08X})")
    i = j + 1

print("\n=== LDR-referenced singleton pointers ===")
for o in ldr_only:
    print(f"  0x{o:06X} = 0x{ext[o]:08X}")

print("\n=== ORPHANS (lone -- disasm context to call code-vs-data) ===")
import bisect
idx_addr, idx_line = [], []
if DIS.exists():
    for ln in DIS.read_text(errors="ignore").splitlines():
        mm = re.match(r"\s*([0-9a-fA-F]+):", ln)
        if mm:
            idx_addr.append(int(mm.group(1), 16))
            idx_line.append(ln.rstrip())
for o in orphan:
    a = LOAD + o
    p = bisect.bisect_left(idx_addr, a)
    print(f"  -- 0x{o:06X} = 0x{ext[o]:08X} --")
    for k in range(max(0, p - 2), min(len(idx_line), p + 3)):
        mark = ">>" if idx_addr[k] == a else "  "
        print(f"   {mark} {idx_line[k]}")

print("\n=== movw/movt inline external addresses ===")
for addr, reg, val in movw_movt:
    print(f"  0x{addr} {reg} = 0x{val:08X}")
