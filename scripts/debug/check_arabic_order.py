#!/usr/bin/env python3
"""Diagnose Arabic byte order: does the cook produce VISUAL (RTL-correct, reversed)
order, or LOGICAL (LTR/backwards) order? Checks BOTH the shaper and the cooked pack."""
import os
import struct
import sys

import arabic_reshaper
from bidi.algorithm import get_display

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Logical-order source strings (as authored in ar/strings.json).
SAMPLES = {
    "STR_BATT (battery)":   "البطارية: ",
    "STR_TITLE_SETTINGS":   "الإعدادات",
    "STR_LAUNCH":           "تشغيل",
}

print("=== shaper: does get_display() reverse to visual order? ===")
for name, s in SAMPLES.items():
    reshaped = arabic_reshaper.reshape(s)          # logical order, presentation forms
    visual = get_display(reshaped)                 # should be VISUAL order
    rev = list(visual) == list(reversed(reshaped))
    print(f"\n{name}: {s!r}")
    print(f"  logical : {[f'{ord(c):04X}' for c in s]}")
    print(f"  reshaped: {[f'{ord(c):04X}' for c in reshaped]}")
    print(f"  visual  : {[f'{ord(c):04X}' for c in visual]}")
    print(f"  visual == reversed(reshaped)?  {rev}   "
          f"({'VISUAL/RTL-correct' if rev else 'NOT reversed -> would render LTR/backwards'})")

# --- decode the cooked ar.lang and show STR_BATT's stored bytes ---
print("\n=== cooked pack: build/i18n/ar.lang STR_BATT stored order ===")
lang = os.path.join(REPO, "build", "i18n", "ar.lang")
if not os.path.isfile(lang):
    sys.exit("ar.lang not cooked; run `make i18n`")
buf = open(lang, "rb").read()
# header: u32 magic; u16 abi; u16 count; u32 ver; char script[16] code[16] endonym[32]
count = struct.unpack_from("<H", buf, 6)[0]
# STR_BATT index: parse from ui/strings.h enum order
hdr = open(os.path.join(REPO, "src", "chainloader", "ui", "strings.h"), encoding="utf-8").read()
import re
enum = re.search(r"typedef enum\s*\{(.*?)\}\s*string_id_t", hdr, re.S).group(1)
ids = [m.group(1) for m in re.finditer(r"\b(STR_[A-Z0-9_]+)\b", enum) if m.group(1) != "STR_COUNT"]
idx = ids.index("STR_BATT")
off = struct.unpack_from("<I", buf, 76 + 4 * idx)[0]
blob_start = 76 + 4 * count
s = bytearray()
i = blob_start + off
while buf[i] != 0:
    s.append(buf[i]); i += 1
stored = s.decode("utf-8")
print(f"  STR_BATT stored: {[f'{ord(c):04X}' for c in stored]}")
expected_visual = get_display(arabic_reshaper.reshape("البطارية: "))
print(f"  expected visual: {[f'{ord(c):04X}' for c in expected_visual]}")
print(f"  MATCH (pack holds visual order)?  {stored == expected_visual}")
