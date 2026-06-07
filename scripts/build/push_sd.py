#!/usr/bin/env python3
"""Push a built SD-card content tree to the device's SD card via gnwmanager `sdpush`.

The chainloader analogue of retro-go-sd's `flash_sd` target. Walks --root, skips any --exclude
basenames (e.g. retro-go's `update_bank2.bin` updater, which the chainloader does not use), and
pushes every remaining file to the SD card preserving its relative directory layout
(`--dest-path /<reldir>/`), then starts bank1. Driven by `make flash-sd`.

Doing this in Python (rather than an inline Makefile loop) handles three things cleanly: the tree
is enumerated at run time (it does not exist until Retro-Go is built), it recurses into
subdirectories, and it copes with spaces in filenames (e.g. "Super Mario World.bin"). gnwmanager
chains all the pushes into a single openocd session.

  python3 scripts/build/push_sd.py --root retro-go-sd/sd_content --exclude update_bank2.bin
  python3 scripts/build/push_sd.py --root retro-go-sd/sd_content --dry-run      # list, do not flash
"""
import argparse
import subprocess
import sys
from pathlib import Path

# Put scripts/ on the path so importing `common` applies the gnwmanager openocd-stderr-drain env
# (GNWMANAGER_OPENOCD_DEBUG / VERBOSITY) that prevents the host<->probe pipe deadlock.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common  # noqa: E402,F401  (import for its environment side effects)


def plan(root: Path, exclude: set[str]):
    """Return [(file_path, dest_dir)] for every pushable file, sorted for stable output."""
    out = []
    for f in sorted(root.rglob("*")):
        if not f.is_file() or f.name in exclude:
            continue
        rel_dir = f.parent.relative_to(root).as_posix()      # "" at root, "cores", "cores/mappers"
        dest = "/" + (rel_dir + "/" if rel_dir else "")       # "/", "/cores/", "/cores/mappers/"
        out.append((f, dest))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, help="directory tree to push (e.g. retro-go-sd/sd_content)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="basename(s) to skip (repeatable)")
    ap.add_argument("--dry-run", action="store_true", help="list the planned sdpush set; do not flash")
    ap.add_argument("--no-start", action="store_true", help="do not 'start bank1' after pushing")
    args = ap.parse_args()

    root = Path(args.root)
    if not root.is_dir():
        sys.exit(f"root not found: {root} (build Retro-Go first: make build-rg-sd)")
    exclude = set(args.exclude)

    items = plan(root, exclude)
    if not items:
        print(f"push_sd: nothing to push under {root} (excluded: {sorted(exclude)})")
        return 0

    if args.dry_run:
        for f, dest in items:
            print(f"sdpush {f} -> {dest}")
        print(f"\n{len(items)} file(s); start bank1={not args.no_start}")
        return 0

    # One gnwmanager session: sdpush each file, then start bank1. Use an arg list (no shell) so
    # filenames with spaces are passed intact.
    cmd = ["gnwmanager"]
    for i, (f, dest) in enumerate(items):
        if i:
            cmd.append("--")
        cmd += ["sdpush", "--file", str(f), "--dest-path", dest]
    if not args.no_start:
        cmd += ["--", "start", "bank1"]

    print(f"push_sd: pushing {len(items)} file(s) from {root} to the SD card "
          f"(unchanged files are skipped; first run can be slow) ...", flush=True)
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
