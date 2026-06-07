#!/usr/bin/env python3
"""Generate src/common/ofw_crc.h from the patched OFW build artifacts.

The chainloader bakes a CRC-32 signature of each OFW backup image and its paired
external asset blob, then re-checks those signatures on the device before copying
an OFW into Bank 2 and booting it (see src/chainloader/system/ofw_verify.c and
DESIGN.md). This script computes the expected values from the deterministic patch
outputs (build/patched_internal_<game>.bin and build/patched_external_<game>.bin)
and writes them into a committed header. Regenerate + recommit after re-patching
(`make patch && make ofw-crc`); the values are device-invariant (the stock ROMs
are SHA1-pinned and the save regions are excluded), so this is a once-per-patch
step, not part of the normal build.

CRC convention: the STM32H7 hardware CRC unit in its reset-default configuration
(poly 0x04C11DB7, init 0xFFFFFFFF, 32-bit, no input/output reflection, no final
XOR -- i.e. CRC-32/MPEG-2), fed one little-endian 32-bit word at a time exactly as
the device does (`CRC->DR = *(uint32_t*)p`). The specific polynomial is irrelevant
-- this is a shared integrity token; host and device only have to agree, and they
agree by both modelling the same hardware operation. Window lengths are multiples
of 4 (asserted) so the word feed is exact.

Static-window geometry (the bytes that never change at runtime):
  - Mario : blob flashed at extflash 0x400000. The only runtime-mutable region is
            the 8 KiB NVRAM, which the patcher leaves as the trailing 8 KiB of the
            shortened blob (nvram = blob_len - 0x2000; see mario.py shorten()). So
            the static window is [0, blob_len - 0x2000).
  - Zelda : blob flashed at extflash 0x0, never shortened (absolute pointers). The
            static ROM/asset/code region is exactly the stock encrypted span
            [0x20000, 0x3254A0); save data lives before 0x20000 and after 0x3E0000.
  - Internal backup: the full 128 KiB image (never written at runtime).
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import flashio  # noqa: E402 (path insert above)

OFW_INTERNAL_SIZE = 128 * 1024
MARIO_NVRAM_SIZE = 8192  # two 4 KiB NVRAM banks, the trailing slice of the blob

# Per-console geometry. spi_offset / asset_offset are the extflash offsets the
# Makefile flashes the artifacts to (Makefile.common flash-ofw targets); they MUST
# match src/common/memory_map.h (ZELDA_SPI_OFFSET / MARIO_SPI_OFFSET) and the
# flash --offset values. The asset window is computed below per the geometry notes.
CONSOLES = {
    "mario": {"spi_offset": 0x520000, "asset_offset": 0x400000},
    "zelda": {"spi_offset": 0x500000, "asset_offset": 0x000000},
}


def crc32_mpeg2_words(data: bytes) -> int:
    """Model the STM32H7 HW CRC (default config) fed LE 32-bit words.

    Mirrors the device loop `CRC->DR = *(uint32_t*)p` for each word: init
    0xFFFFFFFF, poly 0x04C11DB7, no reflection, no final XOR.
    """
    if len(data) % 4:
        raise ValueError(f"CRC window length {len(data)} is not a multiple of 4")
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        crc ^= int.from_bytes(data[i : i + 4], "little")
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def asset_window(console: str, ext_len: int) -> tuple[int, int]:
    """Return (win_start, win_len) of the static (immutable) asset window."""
    if console == "mario":
        return 0, ext_len - MARIO_NVRAM_SIZE
    if console == "zelda":
        reg = flashio.info("zelda")
        return reg["stock_enc_start"], reg["stock_enc_end"] - reg["stock_enc_start"]
    raise SystemExit(f"no asset window defined for {console!r}")


def build_record(console: str) -> dict:
    reg = flashio.info(console)
    geom = CONSOLES[console]

    internal = flashio.load_raw(reg["patched_internal"])
    if len(internal) < OFW_INTERNAL_SIZE:
        raise SystemExit(f"{console} internal image is {len(internal)} B (< {OFW_INTERNAL_SIZE})")
    internal = internal[:OFW_INTERNAL_SIZE]

    external = flashio.load_patched_external(console)
    win_start, win_len = asset_window(console, len(external))
    if win_start + win_len > len(external):
        raise SystemExit(
            f"{console} asset window [{win_start:#x}, {win_start + win_len:#x}) "
            f"exceeds the {len(external):#x}-byte blob"
        )

    return {
        "console": console,
        "spi_offset": geom["spi_offset"],
        "internal_len": OFW_INTERNAL_SIZE,
        "internal_crc": crc32_mpeg2_words(internal),
        "asset_offset": geom["asset_offset"],
        "asset_win_start": win_start,
        "asset_win_len": win_len,
        "asset_crc": crc32_mpeg2_words(external[win_start : win_start + win_len]),
    }


def emit(records: list[dict]) -> str:
    lines = [
        "#ifndef COMMON_OFW_CRC_H",
        "#define COMMON_OFW_CRC_H",
        "",
        "/*",
        " * GENERATED by scripts/build/gen_ofw_crc.py -- DO NOT EDIT BY HAND.",
        " * Regenerate after re-patching the OFW:  make patch && make ofw-crc",
        " *",
        " * Baked CRC-32/MPEG-2 signatures of each patched OFW backup image and its",
        " * paired external asset blob (save regions excluded). The chainloader",
        " * re-checks these on the device before copying an OFW into Bank 2 and",
        " * booting it, refusing a recognized-but-mismatched image. See",
        " * src/chainloader/system/ofw_verify.c and DESIGN.md.",
        " */",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint32_t spi_offset;      /* extflash offset of the 128 KiB OFW backup (selector key) */",
        "    uint32_t internal_len;    /* bytes of the backup image to CRC */",
        "    uint32_t internal_crc;    /* expected CRC of the backup image */",
        "    uint32_t asset_offset;    /* extflash offset of the asset-blob base */",
        "    uint32_t asset_win_start; /* static-window start within the blob (save holes excluded) */",
        "    uint32_t asset_win_len;   /* static-window length */",
        "    uint32_t asset_crc;       /* expected CRC of the static asset window */",
        "} ofw_crc_record_t;",
        "",
        f"#define OFW_CRC_RECORD_COUNT {len(records)}",
        "static const ofw_crc_record_t OFW_CRC_RECORDS[OFW_CRC_RECORD_COUNT] = {",
    ]
    for r in records:
        lines.append(
            f"    {{ /* {r['console']} */ "
            f"0x{r['spi_offset']:08X}, 0x{r['internal_len']:08X}, 0x{r['internal_crc']:08X}, "
            f"0x{r['asset_offset']:08X}, 0x{r['asset_win_start']:08X}, 0x{r['asset_win_len']:08X}, "
            f"0x{r['asset_crc']:08X} }},"
        )
    lines += ["};", "", "#endif /* COMMON_OFW_CRC_H */", ""]
    return "\n".join(lines)


def main() -> None:
    records = [build_record(c) for c in ("mario", "zelda")]
    out = Path(__file__).resolve().parents[2] / "src" / "common" / "ofw_crc.h"
    out.write_text(emit(records))
    for r in records:
        print(
            f"{r['console']:6s}  internal_crc=0x{r['internal_crc']:08X}  "
            f"asset[0x{r['asset_win_start']:X}+0x{r['asset_win_len']:X}]=0x{r['asset_crc']:08X}"
        )
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
