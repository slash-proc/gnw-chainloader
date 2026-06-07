"""OpenOCD device-access helpers (hardware only).

Centralises the OpenOCD backend lifecycle, the LTDC framebuffer read/convert,
the SWD timeout wrappers, and mp4 assembly used by the fastcap capture tooling
(``fastcap.py``). The canonical hardware tools (trace/memory/bank/diagnostic)
are intentionally left untouched.
"""
from __future__ import annotations

from contextlib import contextmanager

from . import REPO_ROOT  # noqa: F401  (ensures gnwmanager is on sys.path)

# LTDC layer-1 registers (STM32H7B0).
LTDC_BASE = 0x50001000
LTDC_L1CFBAR = LTDC_BASE + 0x0AC
LTDC_L1CFBLR = LTDC_BASE + 0x0B0
LTDC_L1WHPCR = LTDC_BASE + 0x088
LTDC_L1WVPCR = LTDC_BASE + 0x08C
LTDC_L1PFCR = LTDC_BASE + 0x094

_PIXEL_FORMATS = {0x00: (4, "ARGB8888"), 0x01: (3, "RGB888"), 0x02: (2, "RGB565"),
                  0x04: (2, "ARGB4444"), 0x07: (2, "AL88")}


def pixel_format(pfcr: int):
    """Return ``(bytes_per_pixel, name)`` for an LTDC L1PFCR value."""
    return _PIXEL_FORMATS.get(pfcr & 0x07, (0, "Unknown"))


@contextmanager
def open_backend(halt: bool = True):
    """Open an OpenOCD backend, optionally halting, and always close it afterwards."""
    from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
    backend = OpenOCDBackend()
    backend.open()
    if halt:
        backend.halt()
    try:
        yield backend
    finally:
        if halt:
            backend.resume()
        backend.close()


def read_framebuffer(backend):
    """Read the LTDC layer-1 framebuffer from a halted device into a PIL image.

    Returns ``(image, info)`` where *info* carries the detected geometry/format.
    Reproduces ``dump_framebuffer.py`` (RGB565 + ARGB8888 supported).
    """
    import numpy as np
    from PIL import Image

    fb_addr = backend.read_uint32(LTDC_L1CFBAR)
    cfblr = backend.read_uint32(LTDC_L1CFBLR)
    whpcr = backend.read_uint32(LTDC_L1WHPCR)
    wvpcr = backend.read_uint32(LTDC_L1WVPCR)
    pfcr = backend.read_uint32(LTDC_L1PFCR)

    width = ((whpcr >> 16) & 0x0FFF) - (whpcr & 0x0FFF)
    height = ((wvpcr >> 16) & 0x07FF) - (wvpcr & 0x07FF)
    bpp, fmt = pixel_format(pfcr)
    pitch = (cfblr >> 16) & 0x1FFF
    info = {"fb_addr": fb_addr, "width": width, "height": height, "pitch": pitch,
            "format": fmt, "bpp": bpp, "size": pitch * height}

    data = backend.read_memory(fb_addr, pitch * height)
    if fmt == "RGB565":
        img = np.zeros((height, width, 3), dtype=np.uint8)
        for y in range(height):
            row = np.frombuffer(data[y * pitch:y * pitch + width * 2][:width * 2], dtype=np.uint16)
            img[y, :, 0] = ((row >> 11) & 0x1F) << 3
            img[y, :, 1] = ((row >> 5) & 0x3F) << 2
            img[y, :, 2] = (row & 0x1F) << 3
        return Image.fromarray(img), info
    if fmt == "ARGB8888":
        img = np.zeros((height, width, 4), dtype=np.uint8)
        for y in range(height):
            row = np.frombuffer(data[y * pitch:y * pitch + width * 4][:width * 4], dtype=np.uint32)
            img[y, :, 0] = (row >> 16) & 0xFF
            img[y, :, 1] = (row >> 8) & 0xFF
            img[y, :, 2] = row & 0xFF
            img[y, :, 3] = (row >> 24) & 0xFF
        return Image.fromarray(img), info
    raise SystemExit(f"framebuffer format {fmt} conversion not implemented")


# ---- SWD timeout wrappers (shared by fastcap + swd_bench) ----
#
# OpenOCD can hang indefinitely on a wedged SWD link, so every device access is
# wrapped in a SIGALRM itimer.  The handler is installed once at import; the
# itimer is only armed inside the helpers below, so it is inert for every other
# consumer of this module.  SIGALRM is main-thread + process-global — keep the
# capture/bench loops single-threaded.
import signal as _signal

SWD_TIMEOUT_S = 0.5     # per-op timeout for word reads/writes
BULK_TIMEOUT_S = 15.0   # floor timeout for bulk reads (scaled up by size)


class SWDTimeout(Exception):
    pass


def _swd_alarm(_signum, _frame):
    raise SWDTimeout("SWD operation timed out")


_signal.signal(_signal.SIGALRM, _swd_alarm)


def swd_read(backend, addr, timeout=SWD_TIMEOUT_S):
    """read_uint32 guarded by a SIGALRM timeout (raises SWDTimeout on hang)."""
    _signal.setitimer(_signal.ITIMER_REAL, timeout)
    try:
        return backend.read_uint32(addr)
    finally:
        _signal.setitimer(_signal.ITIMER_REAL, 0)


def swd_write(backend, addr, value, timeout=SWD_TIMEOUT_S):
    """write_uint32 guarded by a SIGALRM timeout."""
    _signal.setitimer(_signal.ITIMER_REAL, timeout)
    try:
        backend.write_uint32(addr, value)
    finally:
        _signal.setitimer(_signal.ITIMER_REAL, 0)


def swd_read_mem(backend, addr, size, timeout=None):
    """Bulk read_memory guarded by a timeout (default max(BULK_TIMEOUT_S, size/50000))."""
    if timeout is None:
        timeout = max(BULK_TIMEOUT_S, size / 50000.0)
    _signal.setitimer(_signal.ITIMER_REAL, timeout)
    try:
        return bytes(backend.read_memory(addr, size))
    finally:
        _signal.setitimer(_signal.ITIMER_REAL, 0)


# ---- Frames → mp4 (used by fastcap.py capture) ----

def frames_to_mp4(frames_dir, mp4_path, fps, scale=1):
    """Assemble ``frame_*.png`` in *frames_dir* into an H.264 mp4 via ffmpeg.

    Uses a glob input so gaps in the zero-padded frame numbering are tolerated;
    yuv420p for broad player compatibility.  Optional integer *scale* upsamples
    with nearest-neighbour.  Returns True on success.
    """
    import shutil
    import subprocess
    from pathlib import Path

    if shutil.which("ffmpeg") is None:
        print("  *** ffmpeg not found on PATH — skipping mp4 assembly ***")
        return False
    d = Path(frames_dir)
    frames = sorted(d.glob("frame_*.png"))
    if not frames:
        print("  *** no frames to assemble into mp4 ***")
        return False

    fps = max(float(fps), 1.0)
    mp4_path = Path(mp4_path)
    mp4_path.parent.mkdir(parents=True, exist_ok=True)

    vf = []
    if scale and scale != 1:
        vf = ["-vf", f"scale=iw*{scale}:ih*{scale}:flags=neighbor,"
                     "pad=ceil(iw/2)*2:ceil(ih/2)*2"]
    cmd = ["ffmpeg", "-y", "-framerate", f"{fps:.3f}", "-pattern_type", "glob",
           "-i", str(d / "frame_*.png"), *vf,
           "-c:v", "libx264", "-pix_fmt", "yuv420p", str(mp4_path)]
    print(f"  Assembling {len(frames)} frames → {mp4_path} at {fps:.1f} fps...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  *** ffmpeg failed (exit {r.returncode}) ***\n{r.stderr[-600:]}")
        return False
    print(f"  Wrote {mp4_path}")
    return True
