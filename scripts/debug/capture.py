#!/usr/bin/env python3
"""Capture the device framebuffer to a PNG or an MP4 animation (hardware required).

Consolidates the OpenOCD screen-capture scripts behind one tool + a shared backend
helper (lib/device.py):

    dump_framebuffer                 -> `frame`
    capture_animation(_old)          -> `animation`

Examples
--------
    python3 scripts/debug/capture.py frame -o build/framebuffer.png
    python3 scripts/debug/capture.py animation --duration 5 --fps 30 --scale 2 out.mp4
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import device, resolve


def _unique(path: Path) -> Path:
    if not path.exists():
        return path
    base, ext, n = path.with_suffix(""), path.suffix, 0
    while True:
        cand = Path(f"{base}_{n}{ext}")
        if not cand.exists():
            return cand
        n += 1


def cmd_frame(args):
    """Single framebuffer grab (== dump_framebuffer.py)."""
    out = _unique(resolve(args.output))
    out.parent.mkdir(parents=True, exist_ok=True)
    with device.open_backend(halt=True) as backend:
        img, info = device.read_framebuffer(backend)
    print(f"Detected Framebuffer:\n  Base 0x{info['fb_addr']:08X}  {info['width']}x{info['height']}"
          f"  pitch {info['pitch']}  {info['format']} ({info['bpp']} bpp)  size {info['size']}")
    img.save(out)
    print(f"Saved {out}")


def cmd_animation(args):
    """Capture N frames and assemble an MP4 via ffmpeg (== capture_animation.py)."""
    frames_dir = resolve("build/frames")
    if frames_dir.exists():
        for f in frames_dir.iterdir():
            f.unlink()
    else:
        frames_dir.mkdir(parents=True, exist_ok=True)

    total = args.duration * args.fps
    print(f"Capturing {args.duration}s at {args.fps} FPS ({total} frames)...")
    with device.open_backend(halt=True) as backend:
        for i in range(total):
            img, _ = device.read_framebuffer(backend)
            img.save(frames_dir / f"frame_{i:04d}.png")
            print(f"Captured {i + 1}/{total}", end="\r")
            backend.resume()
            time.sleep(1.0 / args.fps)
            backend.halt()

    scale_filter = (f"scale=w=iw*{args.scale}:h=ih*{args.scale}:flags=neighbor,"
                    "pad=ceil(iw/2)*2:ceil(ih/2)*2")
    subprocess.run(["ffmpeg", "-y", "-framerate", str(args.fps),
                    "-i", str(frames_dir / "frame_%04d.png"), "-vf", scale_filter,
                    "-c:v", "libx264", "-crf", "20", "-preset", "slow", "-pix_fmt", "yuv420p",
                    str(resolve(args.output))], check=True)
    print("\nDone.")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("frame", help="grab one framebuffer to a PNG")
    sp.add_argument("-o", "--output", default="build/framebuffer.png")
    sp.set_defaults(func=cmd_frame)

    sp = sub.add_parser("animation", help="capture frames and assemble an MP4")
    sp.add_argument("output", help="output MP4 path")
    sp.add_argument("--duration", type=int, required=True, help="seconds")
    sp.add_argument("--fps", type=int, default=60)
    sp.add_argument("--scale", type=int, default=1)
    sp.set_defaults(func=cmd_animation)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    args.func(args)
