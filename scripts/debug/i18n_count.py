#!/usr/bin/env python3
"""Read i18n runtime state off the device: g_lang_count, g_current, and the
discovered language codes — a quick check that filesystem discovery ran."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import harness as h
from common import device

with device.open_backend(halt=False) as backend:
    cnt = h.read_u32_symbol(backend, "g_lang_count")
    cur = h.read_u32_symbol(backend, "g_current") & 0xFF
    print(f"g_lang_count = {cnt}")
    print(f"g_current    = {cur}")
    addr = h.resolve_symbol("g_langs")   # {char code[16]; char endonym[32]; char script[16];} = 64 B
    n = cnt if 0 < cnt < 33 else 1
    for i in range(min(n, 8)):
        raw = bytes(backend.read_memory(addr + i * 64, 16))
        code = raw.split(b"\x00")[0].decode("ascii", "replace")
        print(f"  [{i}] code={code!r}")
