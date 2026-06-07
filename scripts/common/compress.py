"""Compressed-block discovery for flash images.

Centralises the zlib/raw-deflate scan (``scan_for_compressed_graphics.py`` et al.)
and the LZMA inflation with a reconstructed header (``scan_lzma.py``,
``verify_palette_compressed_memory.py``).
"""
from __future__ import annotations

import lzma
import struct
import zlib


def lzma_inflate(blob: bytes, dict_size: int = 16384, uncompressed_size: int | None = 65536) -> bytes:
    """Inflate a *headerless* raw LZMA stream by prepending a reconstructed header.

    Mirrors ``scan_lzma.py`` / ``verify_palette_compressed_memory.py``: properties byte
    ``0x5D`` (lc=3, lp=0, pb=2), 4-byte little-endian dictionary size, 8-byte little-endian
    uncompressed size (``0xFFFF_FFFF_FFFF_FFFF`` when *uncompressed_size* is ``None``).
    """
    size = (1 << 64) - 1 if uncompressed_size is None else uncompressed_size
    header = bytes([0x5D]) + struct.pack("<I", dict_size) + struct.pack("<Q", size)
    return lzma.decompress(header + blob)


def find_zlib_block(data: bytes, target_size: int = 65536):
    """Scan *data* for a zlib or raw-deflate block that inflates to *target_size*.

    Returns ``(offset, kind, decompressed)`` for the last match (mirroring the originals,
    which overwrote the output on each hit), or ``(None, None, None)`` if none is found.
    ``kind`` is ``"zlib"`` or ``"deflate"``.
    """
    match = (None, None, None)
    for offset in range(len(data)):
        if data[offset] == 0x78:  # typical zlib header (0x7801/0x789C/0x78DA)
            try:
                dec = zlib.decompress(data[offset:])
                if len(dec) == target_size:
                    match = (offset, "zlib", dec)
            except Exception:
                pass
        try:
            dec = zlib.decompress(data[offset:offset + 30000], -zlib.MAX_WBITS)
            if len(dec) == target_size:
                match = (offset, "deflate", dec)
        except Exception:
            pass
    return match


def find_lzma_block(data: bytes, target_size: int = 65536, dict_size: int = 16384):
    """Scan *data* for a raw LZMA block that inflates to *target_size*.

    Tries the fixed-size header first, then an unknown-size header (matching
    ``scan_lzma.py``). Returns ``(offset, decompressed)`` of the first hit, or
    ``(None, None)``.
    """
    for offset in range(len(data)):
        if len(data) - offset < 100:
            break
        try:
            dec = lzma_inflate(data[offset:offset + 30000], dict_size, target_size)
            if len(dec) == target_size:
                return offset, dec
        except Exception:
            pass
    # Fallback: unknown uncompressed size, accept >= target and truncate.
    for offset in range(len(data)):
        if len(data) - offset < 100:
            break
        try:
            dec = lzma_inflate(data[offset:offset + 30000], dict_size, None)
            if len(dec) >= target_size:
                return offset, dec[:target_size]
        except Exception:
            pass
    return None, None
