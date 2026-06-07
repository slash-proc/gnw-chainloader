"""OpenOCD device-access helpers (hardware only).

Centralises the OpenOCD backend lifecycle and the LTDC framebuffer read/convert
logic shared by ``dump_framebuffer`` and ``capture_animation``. Only ``capture.py``
uses this module; the canonical hardware tools (trace/memory/bank/diagnostic) are
intentionally left untouched.
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
