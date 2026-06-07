#!/usr/bin/env python3
"""On-device ABI-rejection test (ABI gating, Stage 3 — the hardware half).

Proves the core's ABI gates fire in the REAL boot flow, for BOTH artifact
classes and BOTH mismatch directions:

  * PIE module gate (system/loader.c + storage/vfs.c `vfs_read_module`): a tiny
    dummy module (src/modules/dummy/) is rebuilt at a matching MODULE_ABI_VERSION
    (accepted) and mismatched ones (rejected), pushed over /modules/_selftest.bin.
  * .lang pack gate (storage/vfs.c `vfs_lfs_lang_version`/`pack_abi_ok`): a real
    cooked pack is pushed as /i18n/_selftest.lang unchanged (accepted) and with
    its ABI bytes (offset 4-5) mutated (rejected).

The device must run the ABI_SELFTEST firmware (`make ABI_SELFTEST=1 ... &&
flash`), whose menu.c hook runs both gates once the filesystems are up and leaves
two SWD-readable globals:
  g_abi_selftest_mod  = vfs_read_module() return  (0 accepted, 0xFFFFFFFF rejected)
  g_abi_selftest_pack = vfs_lfs_lang_version()     (>0 accepted, 0 rejected)
Both start as 0x5A5A5A5A so we can tell "hook has not run yet" from a real result.

Per case: rebuild the dummy + mutate the pack, push both (push_batched.py), force
a CLEAN reboot to the menu (clearing boot magics so a stale Retro-Go/OFW divert
can't skip the hook), wait for the hook, read the two globals, assert. The cases
are ordered so the device is left holding only harmless bad-ABI _selftest.*
artifacts (the release firmware skips a bad-ABI pack and never loads _selftest.bin).

  python3 scripts/tests/test_abi_reject.py
"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common.harness import (resolve_symbol, TestRun, chainloader_running,
                            time_budget, recover_probe)
from common import provision

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

REPO = Path(__file__).resolve().parents[2]
PUSH = REPO / "scripts" / "build" / "push_batched.py"
UNSET = 0x5A5A5A5A

# Boot-magic cells cleared before the clean reboot so a stale signal can't divert
# boot away from the menu (where the hook lives). See boot_magic.h / memory_map.h.
RG_MAGIC_ADDR = 0x20000000     # Retro-Go re-launch trace (stub)
SRAM_MAGIC_ADDR = 0x2001FFF8   # chainloader BOOT magic + target


def build_dummy(abi: int) -> Path:
    """Rebuild the dummy module at MODULE_ABI_VERSION=abi -> build/dummy.bin.
    make can't see the -D change, so wipe the object first."""
    subprocess.run("rm -f build/modules/dummy/* build/dummy.bin",
                   shell=True, cwd=REPO, check=False, timeout=30)
    subprocess.run(["make", "dummy", f"DUMMY_ABI={abi}"], cwd=REPO, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=120)
    return REPO / "build" / "dummy.bin"


def write_pack(src: Path, dst: Path, abi: int) -> Path:
    """Copy a real .lang and force its ABI (LNG2 header offset 4-5, u16) to abi."""
    data = bytearray(src.read_bytes())
    data[4] = abi & 0xFF
    data[5] = (abi >> 8) & 0xFF
    dst.write_bytes(data)
    return dst


def push(pairs) -> None:
    """push_batched.py for one or more gnw_path=local pairs (--no-skip: always send).
    push_batched has its own per-file SIGALRM budget; this subprocess timeout is a
    whole-push backstop so a wedge can't block the test."""
    args = [sys.executable, str(PUSH), "--no-skip"] + [f"{g}={l}" for g, l in pairs]
    subprocess.run(args, cwd=REPO, check=True, timeout=240)


def remove(paths) -> None:
    """push_batched.py --rm: delete device files so the test leaves a clean device."""
    args = [sys.executable, str(PUSH), "--rm"] + list(paths)
    subprocess.run(args, cwd=REPO, check=False, timeout=180)


def clean_boot_and_read(timeout: float = 25.0):
    """Force a clean reboot to the menu, confirm the chainloader actually came up
    (not still parked in the RAM flasher), then wait for the hook and read both
    globals. Opens its own (post-push) OpenOCD session.
    Returns (run_ok, run_detail, mod, pack)."""
    mod_addr = resolve_symbol("g_abi_selftest_mod")
    pack_addr = resolve_symbol("g_abi_selftest_pack")
    be = OpenOCDBackend()
    be.open()
    try:
        # Shared clean-reboot: clears the magic cells (so a stale Retro-Go/OFW
        # divert can't skip the hook), reset-halts into the menu, resumes, and
        # confirms the chainloader actually came up. One implementation, in
        # provision.Provisioner.clean_reboot (DRY across the QA suite).
        run_ok, detail = provision.Provisioner(backend=be).clean_reboot(settle=0.4)
        mod = pack = UNSET
        with time_budget(timeout + 10.0, "poll hook"):
            deadline = time.time() + timeout
            while time.time() < deadline:
                time.sleep(1.0)
                be.halt()
                mod = be.read_uint32(mod_addr)
                pack = be.read_uint32(pack_addr)
                be.resume()
                if mod != UNSET:                   # hook sets both in one call
                    break
        return run_ok, detail, mod, pack
    finally:
        try:
            with time_budget(10.0, "backend close"):
                be.close()
        except TimeoutError:
            pass   # a wedged close is recovered by the caller (recover_probe)


def main() -> int:
    t = TestRun("abi-reject")

    try:
        resolve_symbol("g_abi_selftest_mod")
    except KeyError:
        t.note("g_abi_selftest_mod not in build/app/app.elf — flash the ABI_SELFTEST "
               "firmware first: make ABI_SELFTEST=1 -j16 && (flash it)")
        return t.summary()

    packs = sorted((REPO / "build" / "i18n").glob("*.lang"))
    real = next((p for p in packs if p.name != "en_US.lang"), packs[0] if packs else None)
    if real is None:
        t.note("no build/i18n/*.lang to mutate — run `make i18n` first")
        return t.summary()
    real_abi = real.read_bytes()[4] | (real.read_bytes()[5] << 8)
    print(f"using real pack {real.name} (strings ABI {real_abi}) as the pack vehicle")

    tmp = REPO / "build" / "_selftest.lang"
    # Ordered so the LAST artifacts left on the device are bad-ABI (harmless).
    cases = [
        ("accepted: module ABI matches, pack ABI matches", 1, real_abi,
         lambda mod, pack: mod == 0 and pack > 0),
        ("rejected (newer): module ABI+1, pack ABI+1", 2, real_abi + 1,
         lambda mod, pack: mod == 0xFFFFFFFF and pack == 0),
        ("rejected (older): module ABI 0, pack ABI-1", 0, real_abi - 1,
         lambda mod, pack: mod == 0xFFFFFFFF and pack == 0),
    ]

    for name, mabi, pabi, ok in cases:
        print(f"  [{name}]")
        try:
            dummy = build_dummy(mabi)
            write_pack(real, tmp, pabi)
            push([("/modules/_selftest.bin", dummy), ("/i18n/_selftest.lang", tmp)])
            run_ok, detail, mod, pack = clean_boot_and_read()
        except (TimeoutError, subprocess.TimeoutExpired) as e:
            print(f"      TIMEOUT: {e} -- recovering probe, marking failed, moving on")
            recover_probe()
            t.check(False, f"{name} (timed out)")
            continue
        unset = (mod == UNSET)
        print(f"      {detail}")
        print(f"      module gate: 0x{mod:08X}   pack gate: 0x{pack:08X}"
              f"{'   (hook never ran!)' if unset else ''}")
        # Precondition: trust the gate read only if the chainloader actually booted.
        if not t.check(run_ok, f"chainloader booted into the menu ({detail})"):
            continue
        t.check(not unset and ok(mod, pack), name)

    # Clean up after ourselves so the device is left without test artifacts. The
    # release firmware would reject these bad-ABI files anyway, but a clean device
    # is better hygiene. Cleanup failure is non-fatal: the leftovers are harmless.
    try:
        remove(["/modules/_selftest.bin", "/i18n/_selftest.lang"])
        t.note("removed the _selftest artifacts from the device")
    except Exception as e:
        t.note(f"could not remove _selftest artifacts ({e}); they are harmless "
               "(release firmware rejects a bad-ABI pack and never loads _selftest.bin)")
    t.note("reflash the release firmware when done: make clean && make -j16 && (flash)")
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
