#!/usr/bin/env python3
"""Build the external-flash FAT module-store image (build/fat.bin).

The Stage-3 tiered-memory design puts an optional FAT partition on the external flash
(memory_map.h: MODULE_FAT_OFFSET 0x540000, MODULE_FAT_SIZE ~4.75 MiB) holding the full RW
filesystem drivers and, later, XIP module code. This tool creates a fresh FAT16 image of
MODULE_FAT_SIZE and injects the drivers at /fs/fat.bin + /fs/lfs.bin -- both 8.3-clean so the
no-LFN in-core RO FatFs bootstrap can find them before the full FAT driver (with LFN) is loaded.

Pure-Python via pyfatfs (no mtools/mkfs.fat shelling). The image is a bare FAT filesystem with no
partition table; the chainloader reads its boot sector directly at the mapped offset.

  make prepare-fat-bin   # runs this -> build/fat.bin
  make flash-fatfs       # flashes build/fat.bin to MODULE_FAT_OFFSET
"""
import argparse
import pathlib
import struct
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build"

# Keep in sync with memory_map.h MODULE_FAT_SIZE (0x540000 .. MODULE_LFS_OFFSET_SD 0xA00000).
FAT_SIZE = 0x004C0000  # 4.75 MiB

# device path  <-  build artifact. Full RW drivers; 8.3-clean for the no-LFN bootstrap.
DRIVERS = {
    "/fs/fat.bin": BUILD / "fatfs.bin",
    "/fs/lfs.bin": BUILD / "lfs_rw.bin",
}

# Feature/resident modules, injected so the chainloader can execute them in place from flash
# (the point of the store). Same paths the LittleFS uses; pyfatfs lays each file contiguous on
# the fresh volume, so they are XIP-able (verify with scripts/debug/fat_contig.py). Missing
# build artifacts are skipped.
MODULES = {
    "/modules/theme/theme.bin":      BUILD / "theme.bin",
    "/modules/language.bin":         BUILD / "language.bin",
    "/modules/installer.bin":        BUILD / "installer.bin",
    "/modules/fileops.bin":          BUILD / "fileops.bin",
    # ONLY XIP-capable feature apps belong in the FAT store (they execute in place). mp3 + modview are r9-pic.
    "/modules/features/mp3.bin":     BUILD / "mp3.bin",
    "/modules/features/modview.bin": BUILD / "modview.bin",   # trivial list view -> XIP; 8.3-clean name
    # picture.bin is RAM-only (its JPEG/PNG decode IS latency-sensitive) -> LittleFS, deployed separately.
}

# Per-script fonts, injected so font_ext.c reads them IN PLACE from flash (no RAM buffer, no
# paging). The complete CJK fonts are ~0.7-1.1 MB each (~3.3 MB total), which fits the store
# alongside the modules, so ALL fonts XIP. FONT_XIP_MAX is only a sanity cap on a single font;
# the real limit is the total store size (pyfatfs errors if the content overflows). Cooked by
# `make i18n` into build/i18n/fonts/.
FONT_XIP_MAX = 2 * 1024 * 1024  # bytes (sanity cap; all current fonts are well under)
FONTS_DIR    = BUILD / "i18n" / "fonts"


def build_image(out: pathlib.Path, size: int) -> int:
    out.parent.mkdir(parents=True, exist_ok=True)
    # pyfatfs.mkfs() FORMATS an existing file (it does not create one), so pre-create it at the
    # target size first.
    with open(out, "wb") as f:
        f.truncate(size)

    # Fresh, empty FAT16 filesystem of `size` bytes (FAT16 comfortably spans 4.75 MiB).
    pf = PyFat()
    pf.mkfs(str(out), PyFat.FAT_TYPE_FAT16, size=size, label="MODULES")
    pf.close()

    injected = 0
    def _fat83(devpath):
        # Only files the LFN-less readers resolve must be 8.3: the XIP/feature-discovery mapper
        # (fat_ro_map / vfs_map_file), the XIP'd fonts, and the /fs bootstrap drivers. Path-loaded
        # full-copy modules (e.g. /modules/installer.bin) go through the LFN-capable reader and are
        # NOT constrained. Catch a too-long name at build time instead of letting it vanish silently.
        if not (devpath.startswith("/fs/") or "/features/" in devpath or "/i18n/fonts/" in devpath):
            return
        name = devpath.rsplit("/", 1)[-1]
        n, _, e = name.partition(".")
        if len(n) > 8 or len(e) > 3:
            raise SystemExit(f"make_fat_image: '{name}' exceeds FAT 8.3. The XIP/discovery/bootstrap "
                             f"readers are LFN-less, so it would silently fail to map it. Use an 8.3 name.")
    fs = PyFatFS(str(out))
    try:
        for dest, src in {**DRIVERS, **MODULES}.items():
            _fat83(dest)
            if not src.exists():
                print(f"  skip {dest}: {src.relative_to(REPO)} missing (run `make` first)",
                      file=sys.stderr)
                continue
            parent = dest.rsplit("/", 1)[0]
            if parent:
                fs.makedirs(parent, recreate=True)
            fs.writebytes(dest, src.read_bytes())
            print(f"  + {dest}  ({src.stat().st_size} B  <-  {src.name})")
            injected += 1
        # Small fonts read in place from flash; the big CJK fonts stay paged on LittleFS.
        for f in (sorted(FONTS_DIR.glob("*.fnt")) if FONTS_DIR.is_dir() else []):
            _fat83(f"/i18n/fonts/{f.name}")
            if f.stat().st_size > FONT_XIP_MAX:
                print(f"  skip /i18n/fonts/{f.name}: {f.stat().st_size} B too big for XIP (stays paged on LFS)",
                      file=sys.stderr)
                continue
            fs.makedirs("/i18n/fonts", recreate=True)
            fs.writebytes(f"/i18n/fonts/{f.name}", f.read_bytes())
            print(f"  + /i18n/fonts/{f.name}  ({f.stat().st_size} B)")
            injected += 1
    finally:
        fs.close()

    # pyfatfs leaves the legacy CHS geometry (BPB_SecPerTrk @0x18, BPB_NumHeads @0x1A) zeroed.
    # FatFs ignores it (it uses LBA), but pickier host tools (mtools) reject a zero-geometry image,
    # so set sane values to keep the image standard and host-verifiable.
    with open(out, "r+b") as f:
        f.seek(0x18)
        f.write(struct.pack("<HH", 63, 255))  # SecPerTrk=63, NumHeads=255
    return injected


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the FAT module-store image (build/fat.bin).")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=FAT_SIZE,
                    help="image size in bytes (default MODULE_FAT_SIZE = 4.75 MiB)")
    ap.add_argument("--out", default=str(BUILD / "fat.bin"), help="output image path")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    n = build_image(out, args.size)
    print(f"FAT store image: {out.relative_to(REPO)}  ({args.size} B, FAT16, {n} driver(s) injected)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
