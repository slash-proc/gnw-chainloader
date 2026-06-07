#!/usr/bin/env python3
"""Boot-timing readout for the gnw-chainloader (BOOT_BENCH builds).

Reads the firmware's `g_boot_bench[]` / `g_scan_bench[]` milestone arrays back
over SWD and prints the per-stage durations, so we can measure startup latency
empirically (before/after the module-probe speedup — see
docs/startup-module-probe.md).

REQUIRES a firmware built with the instrumentation compiled in:

    make clean && make BOOT_BENCH=1 -j16 && make BOOT_BENCH=1 flash_chainloader

A default (golden) build omits the arrays entirely, so the symbols won't resolve
(this prints a clear hint in that case).

What it does
------------
By default it forces a fresh boot (reset-halt + resume via the canonical
trace.py, which also auto-unswaps banks), waits for the menu + scan + theme load
to settle, then halts just long enough to read the two arrays and resumes. The
arrays live in AXI-SRAM (0x240xxxxx); their addresses move every rebuild, so they
are resolved fresh from build/app/app.elf via `nm` (never hardcoded).

    python3 scripts/debug/boot_bench.py             # reset, settle, read, print
    python3 scripts/debug/boot_bench.py --no-reset  # read the current values only
    python3 scripts/debug/boot_bench.py --wait 4    # seconds to settle after reset
    python3 scripts/debug/boot_bench.py --json       # machine-readable
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import REPO_ROOT  # noqa: E402  (also makes gnwmanager importable)
from common.harness import resolve_symbol, recover_probe, APP_ELF  # noqa: E402
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend  # noqa: E402

# Coarse boot milestones — keep in sync with the BENCH_MARK() calls and the index
# map in system/bench.h / docs/startup-module-probe.md.
BOOT_BENCH_N = 16
BOOT_LABELS = {
    0: "launcher commit",
    1: "menu loop entry",
    2: "first frame drawn",
    3: "modules ready",
    4: "theme applied",
    5: "first themed frame",
    6: "board_init done",
    7: "ospi_init done",
    8: "gui_init done",
    9: "SD probe start",
    10: "SD detect done",
    11: "SD FAT mount done",
    12: "SD statfs done",
    13: "SD unmount done",
}

# Partition-scan stages — one slot per scan_state_internal_t (partition.c). Order
# MUST match that enum.
SCAN_BENCH_N = 12
SCAN_LABELS = {
    0: "IDLE",
    1: "PROBE_SD",
    2: "PROBE_LFS",
    3: "SCAN_INT1",
    4: "GAP_INT1",
    5: "SCAN_INT2",
    6: "GAP_INT2",
    7: "SCAN_EXT",
    8: "GAP_EXT",
    9: "COMPLETE",
}


def read_u32_array(backend, addr: int, n: int) -> list[int]:
    data = backend.read_memory(addr, n * 4)
    return list(struct.unpack(f"<{n}I", data))


def fmt_boot(boot: list[int]) -> str:
    """Coarse milestones with the interesting derived intervals."""
    lines = ["=== boot milestones (ms since app start) ==="]
    for i in range(BOOT_BENCH_N):
        if i not in BOOT_LABELS:
            continue
        t = boot[i]
        lines.append(f"  [{i}] {BOOT_LABELS[i]:<18} t={t:>6} ms" + ("" if t else "   (unset)"))
    # Derived intervals (only when both endpoints were stamped).
    def d(a, b):
        return boot[b] - boot[a] if boot[a] and boot[b] else None
    intervals = [
        ("  board_init      (6-0)", d(0, 6)),
        ("  ospi_init       (7-6)", d(6, 7)),
        ("  gui_init/LCD    (8-7)", d(7, 8)),
        ("  vfs+scan_start  (1-8)", d(8, 1)),
        ("heavy init total  (1-0)", d(0, 1)),
        ("first paint       (2-1)", d(1, 2)),
        ("RECOGNITION DELAY (3-2)", d(2, 3)),
        ("theme load        (4-3)", d(3, 4)),
        ("total to theme    (4-1)", d(1, 4)),
        ("themed frame shown(5-1)", d(1, 5)),
        ("  SD detect      (10-9)", d(9, 10)),
        ("  SD FAT mount  (11-10)", d(10, 11)),
        ("  SD statfs     (12-11)", d(11, 12)),
        ("  SD unmount    (13-12)", d(12, 13)),
        ("SD probe total  (13-9)", d(9, 13)),
    ]
    lines.append("  --")
    for name, v in intervals:
        lines.append(f"  {name}: {('%d ms' % v) if v is not None else '—'}")
    return "\n".join(lines)


def fmt_scan(scan: list[int]) -> str:
    """Per-stage entry ticks + durations (duration = gap to the next stage that ran)."""
    lines = ["=== partition scan stages (entry tick / duration) ==="]
    # Indices that were actually stamped, in order.
    ran = [i for i in range(SCAN_BENCH_N) if scan[i]]
    for pos, i in enumerate(ran):
        nxt = ran[pos + 1] if pos + 1 < len(ran) else None
        dur = (scan[nxt] - scan[i]) if nxt is not None else None
        dur_s = f"dur={dur:>5} ms" if dur is not None else "dur=    — (last)"
        lines.append(f"  {SCAN_LABELS[i]:<10} t={scan[i]:>6} ms   {dur_s}")
    if ran:
        first, last = ran[0], ran[-1]
        if scan[last] > scan[first]:
            lines.append(f"  --\n  total ({SCAN_LABELS[last]}-{SCAN_LABELS[first]}): "
                         f"{scan[last] - scan[first]} ms")
    else:
        lines.append("  (no stages stamped — did the device boot to the menu?)")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Read gnw-chainloader BOOT_BENCH milestones over SWD.")
    ap.add_argument("--no-reset", action="store_true",
                    help="read the current array values without forcing a fresh boot")
    ap.add_argument("--wait", type=float, default=3.0,
                    help="seconds to let the device settle after reset (default 3)")
    ap.add_argument("--elf", default=str(APP_ELF), help="app ELF to resolve symbols from")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = ap.parse_args()

    try:
        boot_addr = resolve_symbol("g_boot_bench", args.elf)
        scan_addr = resolve_symbol("g_scan_bench", args.elf)
    except KeyError:
        print("error: g_boot_bench/g_scan_bench not found — build with "
              "`make BOOT_BENCH=1 ...` and flash it first.", file=sys.stderr)
        return 2

    if not args.no_reset:
        # Fresh boot so the ticks reflect this run from reset (recover_probe does
        # reset-halt then resume, and auto-unswaps banks).
        if not recover_probe():
            print("warning: trace.py reset failed; reading current state instead.",
                  file=sys.stderr)
        time.sleep(args.wait)

    backend = OpenOCDBackend()
    backend.open()
    try:
        backend.halt()
        boot = read_u32_array(backend, boot_addr, BOOT_BENCH_N)
        scan = read_u32_array(backend, scan_addr, SCAN_BENCH_N)
        backend.resume()
    finally:
        backend.close()

    if args.json:
        out = {
            "boot": {BOOT_LABELS.get(i, str(i)): boot[i] for i in range(BOOT_BENCH_N)},
            "scan": {SCAN_LABELS.get(i, str(i)): scan[i] for i in range(SCAN_BENCH_N)},
        }
        print(json.dumps(out, indent=2))
    else:
        print(fmt_boot(boot))
        print()
        print(fmt_scan(scan))
    return 0


if __name__ == "__main__":
    sys.exit(main())
