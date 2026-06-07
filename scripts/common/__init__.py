"""Shared library for the gnw-chainloader debug/asset tooling.

This package is the DRY foundation for the consolidated debug tools in
``scripts/debug/`` (``extract.py``, ``assets.py``, ``inspect.py``, ``render.py``,
``romcheck.py``, ``findtiles.py``, ``capture.py``). It centralises the
logic that used to be copy-pasted across ~70 one-off scripts:

- :mod:`flashio`  — flash/backup loading, decryption, and the known-offset registry.
- :mod:`nesgfx`   — NES CHR 2bpp decoding, iNES parsing, metasprite layouts, G&W tiles.
- :mod:`palette`  — BGRA->RGB conversion, NES master palette, colour-frequency helpers.
- :mod:`compress` — zlib-block scanning and LZMA inflation with header reconstruction.
- :mod:`imaging`  — PNG tile grids (plain/labelled), ASCII tile art, BMP, sprite assembly.
- :mod:`device`   — OpenOCD backend context manager + framebuffer read (hardware only).

All file paths passed to these helpers are resolved relative to the repository root
(see :data:`REPO_ROOT`), so the tools work regardless of the current directory — unlike
the originals, which assumed the CWD was the repo root.
"""
from __future__ import annotations

import sys
from pathlib import Path

# scripts/common/__init__.py -> parents[2] is the repository root.
REPO_ROOT = Path(__file__).resolve().parents[2]

# Make the vendored gnwmanager submodule importable (ZeldaGnW / MarioGnW / OpenOCDBackend).
_GNWMANAGER = REPO_ROOT / "gnwmanager"
if _GNWMANAGER.is_dir() and str(_GNWMANAGER) not in sys.path:
    sys.path.insert(0, str(_GNWMANAGER))


def resolve(path) -> Path:
    """Resolve *path* against the repo root unless it is already absolute."""
    p = Path(path)
    return p if p.is_absolute() else (REPO_ROOT / p)
