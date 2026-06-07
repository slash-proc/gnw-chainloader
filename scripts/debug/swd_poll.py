#!/usr/bin/env python3
"""SWD polling stress test.

Hammers the SWD link by reading a single 32-bit word from one address over and
over at a fixed target rate, for a fixed duration. This is the deliberate
"overload" probe: at high enough rates the adapter / target link wedges, which
is exactly the failure mode this script exists to reproduce. Empirically ~60
polls/second is enough to kill the link fairly quickly.

Unlike swd_bench.py (which measures sustained *throughput* with bulk reads of
varying sizes), this script measures nothing about bandwidth — it just keeps the
link maximally busy with tiny back-to-back word reads to provoke a hang.

Usage
-----
    # Poll the default address at 60 Hz for 30 seconds
    python3 scripts/debug/swd_poll.py

    # Poll at 60 Hz for 2 minutes
    python3 scripts/debug/swd_poll.py --hz 60 --duration 120

    # Poll as fast as the link allows (no pacing) for 30 s
    python3 scripts/debug/swd_poll.py --hz 0

    # Poll a specific address (e.g. the uwTick counter to watch liveness)
    python3 scripts/debug/swd_poll.py --addr 0x24080000

The script reports the polls issued, the value read, and — when the link
wedges — the SWDTimeout that fired, along with how long it survived. A clean
run reports the achieved poll rate so you can confirm the pacing held.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Add the project root to sys.path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.common import device

# Default poll address: the AXI-SRAM free gap above the app, below the module
# pool — always readable once the chainloader has decompressed, and harmless to
# read repeatedly. Same default as swd_bench.py.
DEFAULT_ADDR = 0x24080000

# Default target poll rate (Hz). ~60 was historically enough to wedge the link.
DEFAULT_HZ = 60.0

# Default run duration (seconds).
DEFAULT_DURATION = 30.0


def run_poll(swd_khz: int, addr: int, hz: float, duration: float) -> None:
    period = (1.0 / hz) if hz > 0 else 0.0
    rate_str = f"{hz:g} Hz" if hz > 0 else "unpaced (max rate)"

    print(f"\n{'=' * 60}")
    print(f"SWD clock   : {swd_khz} kHz")
    print(f"Poll addr   : 0x{addr:08X}")
    print(f"Target rate : {rate_str}")
    print(f"Duration    : {duration:g} s")
    print(f"{'=' * 60}")

    polls = 0
    last_value = None
    t_start = time.perf_counter()
    next_due = t_start

    with device.open_backend(halt=False) as backend:
        backend.set_frequency(swd_khz * 1000)

        try:
            while True:
                now = time.perf_counter()
                elapsed = now - t_start
                if elapsed >= duration:
                    break

                # Pace to the target rate (busy-spin to keep it precise at ~60 Hz).
                if period and now < next_due:
                    continue

                last_value = device.swd_read(backend, addr)
                polls += 1
                next_due += period

                # Progress heartbeat once a second.
                if polls % max(1, int(hz) or 100) == 0:
                    print(f"  t={elapsed:6.1f}s  polls={polls:>7}  "
                          f"[0x{addr:08X}]=0x{last_value:08X}")

        except device.SWDTimeout as e:
            elapsed = time.perf_counter() - t_start
            print(f"\n  *** LINK WEDGED after {elapsed:.2f}s / {polls} polls: {e}")
            print("  Recover with: python3 scripts/debug/trace.py reset-halt")
            return
        except KeyboardInterrupt:
            print("\n  (interrupted)")

    elapsed = time.perf_counter() - t_start
    achieved = polls / elapsed if elapsed else 0.0
    print(f"\n  Survived {elapsed:.2f}s, {polls} polls, "
          f"achieved {achieved:.1f} Hz (last value 0x{last_value:08X})")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--addr",
        type=lambda x: int(x, 0),
        default=DEFAULT_ADDR,
        help=f"Address to poll (default: 0x{DEFAULT_ADDR:08X})",
    )
    p.add_argument(
        "--hz",
        type=float,
        default=DEFAULT_HZ,
        help=f"Target poll rate in Hz; 0 = unpaced/max rate (default: {DEFAULT_HZ:g})",
    )
    p.add_argument(
        "--duration",
        type=float,
        default=DEFAULT_DURATION,
        help=f"How long to poll, in seconds (default: {DEFAULT_DURATION:g})",
    )
    p.add_argument(
        "--swd-khz",
        type=int,
        default=4000,
        metavar="KHZ",
        help="SWD clock in kHz (default: 4000)",
    )
    return p


def main() -> None:
    args = build_parser().parse_args()
    run_poll(args.swd_khz, args.addr, args.hz, args.duration)
    print()


if __name__ == "__main__":
    main()
