import sys
import argparse
from pathlib import Path

import logging

# Put scripts/ on the path so we can import the shared common package. The actual
# gnwmanager resolution happens inside common.flashio, which loads MarioGnW/ZeldaGnW
# explicitly from the vendored submodule (its patch functions diverge from upstream).
# Sourcing them from there keeps the build and the debug tooling on one copy of the
# patch logic; this insert is only about finding `common`, not gnwmanager.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

logging.basicConfig(level=logging.DEBUG)
log = logging.getLogger(__name__)

from argparse import Namespace
from common.flashio import MarioGnW, ZeldaGnW

def patch_ofw(cls, target_name: str, internal_path: Path, external_path: Path, args_ns: Namespace, elf_path: Path, bin_path: Path):
    print(f"Preparing {cls.__name__}...")

    # Instantiate device. This loads the internal/external bins and sets up RWData parsing.
    device = cls(internal_path, elf_path, external_path)
    device.args = args_ns

    # Decrypt external firmware
    device.crypt()

    # Replicate _common_prepare copying over of novel code
    novel_code_start = device.internal.STOCK_ROM_END
    patch_data = bin_path.read_bytes()

    # Fill from novel_code_start to the end of 128KB with zeros first
    device.internal[novel_code_start : 131072] = b"\x00" * (131072 - novel_code_start)

    # Copy novel code
    if len(patch_data) > novel_code_start:
        code_len = len(patch_data) - novel_code_start
        print(f"DEBUG: Copying {code_len} bytes of novel code.")
        device.internal[novel_code_start : novel_code_start + code_len] = patch_data[novel_code_start:]

    # Run the full gnwmanager device patching pipeline
    print("Running device()...")
    device()

    print(f"DEBUG: device.int_pos = {hex(device.int_pos)}")

    return device

def main():
    parser = argparse.ArgumentParser(description="Patch Game & Watch firmware.")
    parser.add_argument("target", choices=["mario", "zelda"], help="Target game")
    parser.add_argument("--default", action="store_true", help="Use default gnwmanager patch binaries instead of custom build")

    args_cli = parser.parse_args()

    target = args_cli.target
    use_default = args_cli.default

    if target == "mario":
        args = Namespace(
            disable_sleep=False,
            sleep_time=None,
            no_save=False,
            no_mario_song=False,
            no_sleep_images=False,
            no_smb2=False,
            compression_ratio=1.4,
            offset_size=0x400000,
            debug=False,
            sd_bootloader=False
        )
        cls = MarioGnW
    elif target == "zelda":
        args = Namespace(
            no_la=False,
            no_sleep_images=False,
            no_second_beep=False,
            no_hour_tune=False,
            offset_size=0,
            debug=False,
            sd_bootloader=False,
            encrypt=True
        )
        cls = ZeldaGnW

    internal_path = Path(f"backup/internal_flash_backup_{target}.bin")
    external_path = Path(f"backup/flash_backup_{target}.bin")

    if not internal_path.exists() or not external_path.exists():
        print(f"Error: Required backup files for {target} not found in backup/ directory.")
        sys.exit(1)

    if use_default:
        pkg_dir = Path("gnwmanager/gnwmanager/cli/gnw_patch/binaries") / target
        elf_path = pkg_dir / "default.elf"
        bin_path = pkg_dir / "default.bin"
    else:
        # Check for target-specific patch binaries (e.g., build/gw_patch_mario.bin) first to support parallel builds safely
        elf_path = Path(f"build/gw_patch_{target}.elf")
        bin_path = Path(f"build/gw_patch_{target}.bin")
        if not elf_path.exists() or not bin_path.exists():
            elf_path = Path("build/gw_patch.elf")
            bin_path = Path("build/gw_patch.bin")

    if not elf_path.exists() or not bin_path.exists():
        print(f"Error: Required patch binaries not found (checked {elf_path} and build/gw_patch.elf).")
        sys.exit(1)

    device = patch_ofw(cls, target, internal_path, external_path, args, elf_path, bin_path)

    out_dir = Path("build")
    out_dir.mkdir(exist_ok=True)
    out_bin = device.internal[:131072]
    if len(out_bin) < 131072:
        out_bin += b"\x00" * (131072 - len(out_bin))
    (out_dir / f"patched_internal_{target}.bin").write_bytes(out_bin)
    (out_dir / f"patched_external_{target}.bin").write_bytes(device.external)
    print(f"{target.capitalize()} patched successfully.")
    print(f"Saved build/patched_internal_{target}.bin and build/patched_external_{target}.bin")

if __name__ == "__main__":
    main()
