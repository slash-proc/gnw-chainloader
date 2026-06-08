#!/usr/bin/env python3
"""Push files to the device filesystem via gnwmanager's in-process Python API.

Shelling out to the `gnwmanager` CLI (even one push at a time) was hanging. The
OpenOCD TCL receive loop is `while True: recv()` until a completion token and
catches only broken-pipe errors, so neither a socket timeout (if openocd keeps
spewing partial data the token never arrives) nor gnwmanager's own poll timeouts
can break a wedged transfer. This drives the push through the bindings with a
HARD per-operation wall-clock bound (SIGALRM) so a stall ABORTS and names the
file, plus optional --debug logging.

Uses the installed gnwmanager (it ships the on-device firmware.bin RAM flasher
that start_gnwmanager() loads before any transfer). The multi-file push hangs
were a subprocess pipe-buffer deadlock, not a firmware fault: gnwmanager spawns
openocd, and if nobody drains openocd's stdout the pipe fills (~64 KB) and
openocd blocks on write(), wedging the push on the slow first LittleFS write.
GNWMANAGER_OPENOCD_DEBUG=1 (set unconditionally below) makes gnwmanager read that
pipe, so the installed firmware pushes reliably; the earlier submodule-fork build
preference was unnecessary ceremony and has been removed.

Usage:
  push_batched.py [--debug] [--no-skip] [--timeout S] [--start-timeout S] <gnw_path>=<local_path> ...
  push_batched.py --rm [--timeout S] [--start-timeout S] <gnw_path> ...   # remove device files
"""
import argparse
import logging
import os
import signal
import sys
import time
from contextlib import contextmanager
from pathlib import Path

# Add the gnwmanager submodule to sys.path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gnwmanager"))

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # scripts/ -> import `common`

BANK1 = 0x08000000


@contextmanager
def time_budget(seconds, label):
    """Hard wall-clock bound via SIGALRM (main thread, Unix). Aborts a stuck
    recv OR a busy openocd spew that a socket timeout can't catch."""
    def _fire(signum, frame):
        raise TimeoutError(f"{label}: exceeded {seconds:.0f}s wall-clock budget")
    old = signal.signal(signal.SIGALRM, _fire)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def start_bank1(gnw):
    """Mirror `gnwmanager start bank1`: boot the chainloader from internal flash."""
    gnw.reset_and_halt()
    gnw.backend.write_register("msp", gnw.read_uint32(BANK1))
    gnw.backend.write_register("pc", gnw.read_uint32(BANK1 + 4))
    gnw.backend.resume()


def device_status(gnw):
    for attr in ("_get_status", "get_status"):
        fn = getattr(gnw, attr, None)
        if fn:
            try:
                return fn()
            except Exception as e:  # noqa: BLE001
                return f"<status read failed: {e}>"
    return "<no status accessor>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true",
                    help="also stream the debug log to the console (it is ALWAYS written to build/push_batched_debug.log)")
    ap.add_argument("--no-skip", action="store_true", help="push even if the device copy is unchanged (skips the hash read)")
    ap.add_argument("--start-timeout", type=float, default=90.0, help="start_gnwmanager wall-clock budget (s)")

    ap.add_argument("--rm", action="store_true",
                    help="remove the given device paths instead of pushing (positional args are device paths)")
    ap.add_argument("pairs", nargs="+", metavar="gnw_path[=local_path]")
    args = ap.parse_args()

    # Always capture the full DEBUG log (gnwmanager + openocd transactions) to a
    # file so a wedge is always diagnosable after the fact, independent of console
    # noise. --debug additionally streams it to the console. (Keep debug always on.)
    if args.debug:
        os.environ.setdefault("GNWMANAGER_OPENOCD_DEBUG", "1")
        fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
        root = logging.getLogger()
        root.setLevel(logging.DEBUG)
        debug_log = Path(__file__).resolve().parents[2] / "build" / "push_batched_debug.log"
        debug_log.parent.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(debug_log, mode="w")
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(fmt)
        root.addHandler(fh)
        ch = logging.StreamHandler()
        ch.setLevel(logging.DEBUG if args.debug else logging.INFO)
        ch.setFormatter(fmt)
        root.addHandler(ch)
        print(f"(full debug log always at: {debug_log})", flush=True)

    files = []
    if not args.rm:
        for a in args.pairs:
            gnw_path, sep, local = a.partition("=")
            if not sep or not gnw_path or not local:
                sys.exit(f"bad arg (want gnw_path=local_path): {a!r}")
            p = Path(local)
            if not p.is_file():
                sys.exit(f"local file missing: {local}")
            files.append((gnw_path, p))

    from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
    from gnwmanager.gnw import GnW
    from gnwmanager.filesystem import gnw_sha256
    from gnwmanager.utils import sha256
    from gnwmanager.time import timestamp_now

    print("opening openocd backend ...", flush=True)
    backend = OpenOCDBackend()
    backend.open()
    try:
        gnw = GnW(backend)
        t0 = time.time()
        print("loading gnwmanager device app (start_gnwmanager) ...", flush=True)
        with time_budget(args.start_timeout, "start_gnwmanager"):
            gnw.start_gnwmanager()
        print(f"  ready in {time.time() - t0:.1f}s; ext flash {gnw.external_flash_size >> 20} MB", flush=True)
        fs = gnw.filesystem()

        if args.rm:
            removed = 0
            for i, gnw_path in enumerate(args.pairs, 1):
                t = time.time()
                try:
                    with time_budget(60.0, f"rm {gnw_path}"):
                        try:
                            fs.remove(gnw_path)
                            print(f"[{i}/{len(args.pairs)}] removed {gnw_path} ({time.time() - t:.1f}s)", flush=True)
                            removed += 1
                        except Exception as e:  # absent or already-clean: fine for cleanup
                            print(f"[{i}/{len(args.pairs)}] {gnw_path}: not removed ({type(e).__name__}: {e})", flush=True)
                except TimeoutError as e:
                    print(f"  !! {e}; device status = {device_status(gnw)!r}", flush=True)
                    raise
            print(f"removed {removed}/{len(args.pairs)} file(s); starting bank1 ...", flush=True)
            with time_budget(30.0, "start_bank1"):
                start_bank1(gnw)
            print("done.", flush=True)
            return

        pushed = 0
        for i, (gnw_path, local) in enumerate(files, 1):
            data = local.read_bytes()
            # Dynamic timeout: assume ~50 KB/s minimum speed, with a 60s floor
            timeout = max(60.0, len(data) / (50 * 1024) + 10.0)
            t = time.time()
            try:
                with time_budget(timeout, f"push {gnw_path}"):
                    if not args.no_skip and sha256(data) == gnw_sha256(fs, Path(gnw_path)):
                        print(f"[{i}/{len(files)}] {gnw_path}: unchanged, skip", flush=True)
                        continue
                    print(f"[{i}/{len(files)}] push {gnw_path} ({len(data)} B) ...", flush=True)
                    parent = Path(gnw_path).parent.as_posix()
                    if parent and parent not in (".", "/"):
                        fs.makedirs(parent, exist_ok=True)
                    with fs.open(gnw_path, "wb") as f:
                        f.write(data)
                    fs.setattr(gnw_path, "t", timestamp_now().to_bytes(4, "little"))
                    gnw.wait_for_all_contexts_complete(timeout=args.timeout)
            except TimeoutError as e:
                print(f"  !! {e} (elapsed {time.time() - t:.1f}s); device status = {device_status(gnw)!r}", flush=True)
                raise
            print(f"  ok ({time.time() - t:.1f}s)", flush=True)
            pushed += 1

        print(f"pushed {pushed}/{len(files)} file(s); starting bank1 ...", flush=True)
        with time_budget(30.0, "start_bank1"):
            start_bank1(gnw)
        print("done.", flush=True)
    finally:
        backend.close()


if __name__ == "__main__":
    main()
