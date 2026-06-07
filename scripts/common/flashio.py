"""Flash / backup loading, decryption, and the known-offset registry.

Centralises three things that were duplicated across the debug scripts:

1. The per-console registry of file paths and magic offsets (:data:`CONSOLES`).
2. Loading decrypted external flash via gnwmanager's ``ZeldaGnW`` / ``MarioGnW``
   (the ``device.crypt()`` dance repeated in ~13 scripts).
3. The standalone OTFDEC/AES stock-flash decryptor (from ``decrypt_stock_flash.py``),
   which mirrors ``ExtFirmware.crypt()`` in gnwmanager's ``patches/firmware.py``.
"""
from __future__ import annotations

import hashlib
import importlib
import sys
from pathlib import Path

from . import REPO_ROOT, resolve


# MarioGnW / ZeldaGnW MUST come from the vendored gnwmanager submodule, never the
# pip-installed package: the submodule's patch functions diverge from upstream. We load
# them explicitly from <repo>/gnwmanager via importlib so resolution never depends on
# sys.path ordering or on whatever gnwmanager happens to be installed. flashio is the
# single chokepoint for these classes — both the build (scripts/build/patch_firmware.py)
# and the debug tooling import them from here, so every MarioGnW/ZeldaGnW is the
# submodule's.
def _load_submodule_patch_classes():
    pkg_parent = REPO_ROOT / "gnwmanager"  # directory that *contains* the gnwmanager package
    if not (pkg_parent / "gnwmanager" / "__init__.py").exists():
        raise SystemExit(
            f"vendored gnwmanager submodule not found under {pkg_parent} "
            "(run `git submodule update --init`)")

    # If gnwmanager was already imported from elsewhere (e.g. the pip-installed package),
    # refuse rather than silently patch with upstream's divergent logic.
    already = sys.modules.get("gnwmanager")
    if already is not None and getattr(already, "__file__", None):
        if pkg_parent.resolve() not in Path(already.__file__).resolve().parents:
            raise SystemExit(
                f"gnwmanager already imported from {already.__file__}, not the submodule "
                f"under {pkg_parent}; import common.flashio before any other gnwmanager use.")

    p = str(pkg_parent)
    if p in sys.path:
        sys.path.remove(p)
    sys.path.insert(0, p)

    mario = importlib.import_module("gnwmanager.cli.gnw_patch.mario")
    zelda = importlib.import_module("gnwmanager.cli.gnw_patch.zelda")
    return mario.MarioGnW, zelda.ZeldaGnW


MarioGnW, ZeldaGnW = _load_submodule_patch_classes()

# --------------------------------------------------------------------------- #
# Known-offset / path registry. One row per console; every magic number that
# used to be hardcoded in individual scripts lives here so it can be corrected
# in exactly one place.
# --------------------------------------------------------------------------- #
CONSOLES = {
    "zelda": {
        # backups (stock, encrypted) + patch ELF used to decrypt them
        "internal": "backup/internal_flash_backup_zelda.bin",
        "external": "backup/flash_backup_zelda.bin",
        "elf": "build/gw_patch_zelda.elf",
        # patched (plaintext) build artifacts
        "patched_external": "build/patched_external_zelda.bin",
        "patched_internal": "build/patched_internal_zelda.bin",
        # G&W menu tileset (8bpp) + its BGRA palette, in the *patched external* image.
        # Palette offset is clock palette #8 (0x2E8B24) — the value the build's KING
        # extractor (formerly inline in patch_firmware.py) uses for Zelda I tiles.
        "gnw_tileset_offset": 0x20000,
        "gnw_palette_offset": 0x2E8B24,
        # embedded Zelda II NES ROM (iNES magic) in decrypted external flash
        "rom_offset": 0x70000,
        # NES master palette (64 colours x 3 bytes RGB) in decrypted external flash
        "nes_master_palette_offset": 0x2D8160,
        # OTFDEC stock-decrypt parameters (decrypt_stock_flash.py)
        "stock_key_offset": 0x165A4,
        "stock_nonce_offset": 0x16590,
        "stock_enc_start": 0x20000,
        "stock_enc_end": 0x3254A0,
        "stock_sha1": "1c1c0ed66d07324e560dcd9e86a322ec5e4c1e96",
    },
    "mario": {
        "internal": "backup/internal_flash_backup_mario.bin",
        "external": "backup/flash_backup_mario.bin",
        "elf": "build/gw_patch_mario.elf",
        "patched_external": "build/patched_external_mario.bin",
        "patched_internal": "build/patched_internal_mario.bin",
        "gnw_tileset_offset": 0x98B84,
        "gnw_palette_offset": 0xBEC68,
        # Mario has no embedded second NES ROM region of interest here.
        "rom_offset": None,
        "nes_master_palette_offset": None,
        "stock_key_offset": 0x106F4,
        "stock_nonce_offset": 0x106E4,
        "stock_enc_start": 0,
        "stock_enc_end": 0xFE000,
        "stock_sha1": "eea70bb171afece163fb4b293c5364ddb90637ae",
    },
}

# NES "Link" sprite palette indices into the master palette (Zelda II walking Link):
# 0x0F black/transparent, 0x37 skin, 0x28 gold tunic accent, 0x1A green tunic.
LINK_COLOR_INDICES = [0x0F, 0x37, 0x28, 0x1A]

FLASH_BASE = 0x9000_0000


def info(console: str) -> dict:
    """Return the registry row for *console*, raising a clear error if unknown."""
    try:
        return CONSOLES[console]
    except KeyError:
        raise SystemExit(f"unknown console {console!r}; expected one of {list(CONSOLES)}")


def load_raw(path) -> bytes:
    """Read *path* (resolved against the repo root) as raw bytes."""
    p = resolve(path)
    if not p.exists():
        raise SystemExit(f"file not found: {p}")
    return p.read_bytes()


def load_patched_external(console: str) -> bytes:
    """Raw bytes of ``build/patched_external_<console>.bin`` (plaintext build artifact)."""
    return load_raw(info(console)["patched_external"])


def load_decrypted(console: str, *, internal=None, external=None, elf=None) -> bytes:
    """Decrypt the *stock* external backup in memory and return its bytes.

    Wraps gnwmanager's ``ZeldaGnW`` / ``MarioGnW`` ``.crypt()`` — the pattern that
    was repeated verbatim across the extraction/render scripts. Paths default to the
    registry; override for non-standard backups.
    """
    reg = info(console)
    internal = str(resolve(internal or reg["internal"]))
    external = str(resolve(external or reg["external"]))
    elf = str(resolve(elf or reg["elf"]))
    for label, p in (("internal", internal), ("external", external), ("elf", elf)):
        if not Path(p).exists():
            raise SystemExit(f"{console} {label} backup not found: {p}")

    if console == "zelda":
        GnW = ZeldaGnW
    elif console == "mario":
        GnW = MarioGnW
    else:
        raise SystemExit(f"no decryptor for console {console!r}")

    device = GnW(internal, elf, external)
    device.crypt()
    return bytes(device.external)


# --------------------------------------------------------------------------- #
# Standalone OTFDEC/AES stock decryptor (no gnwmanager device needed).
# Ported verbatim from decrypt_stock_flash.py to keep byte-for-byte behaviour.
# --------------------------------------------------------------------------- #
def _nonce_to_iv(nonce: bytes) -> bytes:
    nonce = nonce[::-1]
    return nonce + b"\x00\x00" + b"\x71\x23" + b"\x20\x00" + b"\x00\x00"


def otfdec_crypt(buf: bytearray, key: bytes, nonce: bytes, enc_start: int, enc_end: int) -> None:
    """In-place OTFDEC counter-mode XOR over ``buf[enc_start:enc_end]``."""
    from Crypto.Cipher import AES

    key = bytes(key[::-1])
    iv = bytearray(_nonce_to_iv(nonce))
    aes = AES.new(key, AES.MODE_ECB)
    for offset in range(enc_start, enc_end, 16):
        cb = iv.copy()
        counter = (FLASH_BASE + offset) >> 4
        cb[12] = ((counter >> 24) & 0x0F) | (cb[12] & 0xF0)
        cb[13] = (counter >> 16) & 0xFF
        cb[14] = (counter >> 8) & 0xFF
        cb[15] = (counter >> 0) & 0xFF
        block = aes.encrypt(bytes(cb))
        for i, cb_byte in enumerate(reversed(block)):
            buf[offset + i] ^= cb_byte


def decrypt_stock(console: str, *, internal=None, external=None, verify=True):
    """Decrypt a stock external backup via OTFDEC and return ``(bytes, sha1_ok)``.

    Reproduces ``decrypt_stock_flash.py``: pulls KEY/NONCE from the internal backup,
    runs counter-mode AES, and (optionally) checks the encrypted input's SHA-1 against
    the known-good device hash.
    """
    reg = info(console)
    internal_b = load_raw(internal or reg["internal"])
    external_b = bytearray(load_raw(external or reg["external"]))
    es, ee = reg["stock_enc_start"], reg["stock_enc_end"]

    sha1_ok = None
    if verify:
        if console == "mario":
            h = hashlib.sha1(bytes(external_b[:-8192])).hexdigest()
        else:
            h = hashlib.sha1(bytes(external_b[es:ee])).hexdigest()
        sha1_ok = h == reg["stock_sha1"]

    key = internal_b[reg["stock_key_offset"]:reg["stock_key_offset"] + 16]
    nonce = internal_b[reg["stock_nonce_offset"]:reg["stock_nonce_offset"] + 8]
    otfdec_crypt(external_b, key, nonce, es, ee)
    return bytes(external_b), sha1_ok
