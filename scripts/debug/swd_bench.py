#!/usr/bin/env python3
"""SWD read throughput benchmark.

Measures the sustained read bandwidth of the connected SWD adapter by issuing
a series of bulk reads of varying sizes from AXI SRAM and timing each one.
The result reveals the effective data rate available for the fastcap pipeline
under real conditions — accounting for USB round-trip latency, OpenOCD TCL
overhead, and adapter firmware processing.

This is useful for understanding the ceiling on framebuffer capture frame rate
before trying further optimisations (sub-block granularity, payload compression,
BUFFER_FRAMES tuning, etc.).

Usage
-----
    # Basic benchmark with default sizes
    python3 scripts/debug/swd_bench.py

    # Test at 10 MHz (ST-Link V2 recommended)
    python3 scripts/debug/swd_bench.py --swd-khz 10000

    # Test at multiple clock speeds to see the SWD clock scaling
    python3 scripts/debug/swd_bench.py --swd-khz 1000 2000 4000 8000 10000

    # Override the read address (must be stable AXI SRAM — defaults to PREV_BUF)
    python3 scripts/debug/swd_bench.py --addr 0x24072000

The benchmark reads from FASTCAP_PREV_FRAME_BUF (0x24072000) by default — a
150 KB region that is always present in AXI SRAM once the chainloader has
decompressed.  Reading it repeatedly does not disturb device operation.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Add the project root to sys.path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.common import device

# Read sizes to benchmark: from a single word up to a full 150 KB frame.
DEFAULT_SIZES = [
    4,          # 1 word  — minimum (e.g. status flag)
    64,         # 16 words
    512,        # 128 words
    4096,       # 1 KB
    8192,       # 2 KB
    20480,      # 20 KB  — a typical fastcap keyframe
    51200,      # 50 KB
    102400,     # 100 KB
    153600,     # 150 KB — a full 320x240 framebuffer
]

# Default read address: the AXI-SRAM free gap (any readable RAM serves as a
# throughput probe; this sits above the app, below the module pool).
DEFAULT_ADDR = 0x24080000

# How many times to repeat each size for a stable average.
REPS = 3


def benchmark_size(backend, addr: int, size: int, reps: int) -> tuple[float, float]:
    """Return (avg_seconds, avg_kbps) for *reps* reads of *size* bytes."""
    times = []
    for _ in range(reps):
        t0 = time.perf_counter()
        device.swd_read_mem(backend, addr, size)   # timeout-safe (raises device.SWDTimeout on a wedged link)
        times.append(time.perf_counter() - t0)
    avg = sum(times) / len(times)
    kbps = (size / 1024.0) / avg
    return avg, kbps


def run_benchmark(swd_khz: int, addr: int, sizes: list[int], reps: int) -> None:
    print(f"\n{'=' * 60}")
    print(f"SWD clock : {swd_khz} kHz")
    print(f"Read addr : 0x{addr:08X}")
    print(f"Reps each : {reps}")
    print(f"{'=' * 60}")
    print(f"  {'Size':>10}  {'Avg time':>12}  {'Throughput':>14}")
    print(f"  {'-'*10}  {'-'*12}  {'-'*14}")

    with device.open_backend(halt=False) as backend:
        backend.set_frequency(swd_khz * 1000)

        for size in sizes:
            try:
                avg_s, kbps = benchmark_size(backend, addr, size, reps)
                size_str = (f"{size:,} B" if size < 1024
                            else f"{size / 1024:.0f} KB")
                print(f"  {size_str:>10}  {avg_s * 1000:>9.1f} ms  {kbps:>10.1f} KB/s")
            except Exception as e:
                size_str = (f"{size:,} B" if size < 1024
                            else f"{size / 1024:.0f} KB")
                print(f"  {size_str:>10}  ERROR: {e}")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--swd-khz",
        type=int,
        nargs="+",
        default=[4000],
        metavar="KHZ",
        help="SWD clock(s) in kHz to benchmark (default: 4000); "
             "pass multiple values to compare (e.g. --swd-khz 4000 10000)",
    )
    p.add_argument(
        "--addr",
        type=lambda x: int(x, 0),
        default=DEFAULT_ADDR,
        help=f"AXI SRAM address to read from (default: 0x{DEFAULT_ADDR:08X})",
    )
    p.add_argument(
        "--sizes",
        type=int,
        nargs="+",
        default=DEFAULT_SIZES,
        metavar="BYTES",
        help="Read sizes in bytes (default: predefined set from 4 B to 150 KB)",
    )
    p.add_argument(
        "--reps",
        type=int,
        default=REPS,
        help=f"Repetitions per size for averaging (default: {REPS})",
    )
    return p


def main() -> None:
    args = build_parser().parse_args()

    print("SWD Read Throughput Benchmark")
    print(f"  Testing {len(args.swd_khz)} clock speed(s) × "
          f"{len(args.sizes)} size(s) × {args.reps} reps each")

    for khz in args.swd_khz:
        try:
            run_benchmark(khz, args.addr, args.sizes, args.reps)
        except Exception as e:
            print(f"  *** Error at {khz} kHz: {e} ***")

    print()


if __name__ == "__main__":
    main()
