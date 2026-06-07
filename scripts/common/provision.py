"""Provisioning library — compose the device into a known environment.

Rebuild-from-source (decision 3): no golden image. Two layers:

  * Primitives — cheap state toggles (magic cells, the BKP3R settings word, the
    Retro-Go vector, targeted region writes) plus the heavier full flashes, each a
    thin wrapper over an existing tool. The cheap ones are plain RAM/register
    writes over a short-lived backend; the heavy ones SHELL OUT to push_batched.py
    (LittleFS) and `gnwmanager flash` (internal/external flash) so we never retype
    the swap/erase/push sequences whose bugs are already solved.

  * Recipe layer — a Recipe is declarative DATA (which modules, which langs, which
    settings, RG present?, …). `apply(recipe)` runs the primitives in dependency
    order, prompts the manual steps, then `envprobe.probe()` + runs the recipe's
    `expect` closure to VERIFY the device reached the intended state before any
    test trusts it.

Serialization (memory: one SWD/probe process at a time): cheap writes use a
short-lived OpenOCD backend that is closed before any gnwmanager/push subprocess
(each spawns its own openocd). Never hold a backend across a heavy op.

`dry=True` makes every primitive print what it WOULD do without touching the
device, so the orchestration is verifiable off-hardware.
"""
from __future__ import annotations

import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path

if __package__ in (None, ""):                     # allow `python3 provision.py` (CLI)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    __package__ = "common"

from . import REPO_ROOT, resolve, lfs_gnwmanager_offset
from . import harness as h
from . import observe
from . import envprobe

PUSH = REPO_ROOT / "scripts" / "build" / "push_batched.py"
BUILD = REPO_ROOT / "build"
# Retro-Go bank-1 payload (in-repo build artifact) for the "present" direction.
RETROGO_INTFLASH = REPO_ROOT / "retro-go-sd" / "build" / "gw_retro_go_intflash.bin"
CHAINLOADER_BIN = BUILD / "gnw_chainloader.bin"


class Provisioner:
    """Mutate the device toward a target state. Reuses observe.* constants and the
    existing flash/push tools; adds no new SWD primitive of its own."""

    def __init__(self, backend=None, dry: bool = False):
        self._backend = backend          # injected (caller-owned) or None
        self.dry = dry

    # -- backend lifecycle for cheap writes --------------------------------
    @contextmanager
    def _session(self):
        if self._backend is not None:
            yield self._backend
            return
        from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
        be = OpenOCDBackend(); be.open()
        try:
            yield be
        finally:
            try:
                be.close()
            except Exception:
                pass

    def _log(self, msg):
        print(f"  [provision]{' (dry)' if self.dry else ''} {msg}", flush=True)

    # ===================== cheap state toggles ==========================
    def set_magic(self, *, boot=None, target=None, rg=None):
        """Write the boot-magic cells (RAM, simple writes)."""
        self._log(f"set_magic boot={_hx(boot)} target={_hx(target)} rg={_hx(rg)}")
        if self.dry:
            return
        with self._session() as be, h.time_budget(15.0, "set_magic"):
            be.halt()
            if boot is not None:
                be.write_uint32(observe.SRAM_MAGIC_ADDR, boot)
            if target is not None:
                be.write_uint32(observe.SRAM_MAGIC_TARGET, target)
            if rg is not None:
                be.write_uint32(observe.RG_MAGIC_ADDR, rg)
            be.resume()

    def clear_magics(self):
        """Zero all three magic cells — the clean-boot precondition."""
        self.set_magic(boot=0, target=0, rg=0)

    def set_settings_word(self, *, fastboot=None, lang=None,
                          mario_slot=None, zelda_slot=None):
        """Read-modify-write the BKP3R settings word; only the named fields change.
        Applied on the next boot, so follow with clean_reboot()."""
        self._log(f"set_settings_word fastboot={fastboot} lang={lang} "
                  f"mario_slot={mario_slot} zelda_slot={zelda_slot}")
        if self.dry:
            return
        with self._session() as be, h.time_budget(15.0, "set_settings"):
            be.halt()
            cur = observe.settings_word(be) or 0
            fb = observe.settings_fastboot(cur) if fastboot is None else fastboot
            m = observe.settings_mario_slot(cur) if mario_slot is None else mario_slot
            z = observe.settings_zelda_slot(cur) if zelda_slot is None else zelda_slot
            lg = observe.settings_lang(cur) if lang is None else lang
            word = observe.settings_make(fb, m, z, lg)
            be.write_uint32(observe.TAMP_BKP3R, word)
            be.resume()
            self._log(f"  BKP3R <- 0x{word:08X}")

    def clean_reboot(self, settle: float = 0.4):
        """Clear the magic cells and reset into the chainloader menu, then confirm
        the chainloader is actually live. The single source for the clean-reboot
        pattern (test_abi_reject and the runner call this). Returns (ok, detail)."""
        self._log("clean_reboot (clear magics + reset-halt + resume)")
        if self.dry:
            return True, "dry"
        with self._session() as be:
            with h.time_budget(20.0, "clean reboot"):
                be.halt()
                be.write_uint32(observe.SRAM_MAGIC_ADDR, 0)
                be.write_uint32(observe.RG_MAGIC_ADDR, 0)
                be.reset_and_halt()
                be.resume()
            time.sleep(settle)
            ok, detail = h.chainloader_running(be)
        self._log(f"  chainloader_running -> {ok} ({detail})")
        return ok, detail

    # ===================== littlefs content =============================
    def _push(self, pairs, no_skip=False, retries=3):
        """push_batched.py for [(gnw_path, local_path), ...].

        Retries on a failed push: the probe occasionally returns a malformed
        read_uint32 response ("Error decoding expected hex response", the
        known-flaky glitch in CLAUDE.md), which fails one push but succeeds on a
        fresh openocd session. Without this a single transient glitch aborts a
        whole restore mid-way (and leaves the device half-provisioned)."""
        if not pairs:
            return
        args = [sys.executable, str(PUSH)]
        if no_skip:
            args.append("--no-skip")
        args += [f"{g}={Path(l)}" for g, l in pairs]
        self._log("push " + ", ".join(g for g, _ in pairs))
        if self.dry:
            return
        for attempt in range(1, retries + 1):
            try:
                subprocess.run(args, cwd=REPO_ROOT, check=True, timeout=300)
                return
            except subprocess.CalledProcessError:
                if attempt >= retries:
                    raise
                self._log(f"push failed (attempt {attempt}/{retries}; likely a "
                          f"transient probe glitch) -- retrying on a fresh session")
                time.sleep(2)

    def set_active_language(self, code: str):
        """Set the UI language by writing /i18n/.active on LittleFS, then reboot.

        The deterministic, layout-immune way to set OR RECOVER the language: it is
        a file write + reset, with no OCR, no menu navigation, and no dependence on
        the carousel -- so it escapes Arabic/Farsi (RTL) or any stuck state every
        time. The language module reads /i18n/.active at boot (lang_mgr.c)."""
        self._log(f"set active language -> {code!r} (/i18n/.active + reboot)")
        if self.dry:
            return
        f = BUILD / "_active_lang.txt"
        f.write_text(code)
        self._push([("/i18n/.active", f)], no_skip=True)
        self.clean_reboot()

    def recover_to_english(self):
        """Convenience: force the UI back to en_US (e.g. after an RTL test)."""
        self.set_active_language("en_US")

    def wipe_lfs_content(self, tops=("/modules", "/i18n")):
        """Destructively remove ALL files under the given LittleFS trees by
        DISCOVERING what is actually on the device and removing each (robust to the
        path layout, unlike a hardcoded list that drifts -- e.g. mp3 lives at
        /modules/features/mp3.bin, not .../mp3/mp3.bin). bank1 / Retro-Go are
        untouched, so the launcher menu MUST stay reachable (the stability
        invariant) and the UI falls back to English with no sprite theme. Restore
        with push_all_modules() + push_i18n_packs(). Returns the count removed; -1
        if the gnwmanager session failed."""
        import posixpath
        self._log(f"WIPE LittleFS content under {', '.join(tops)} (discover + remove)")
        if self.dry:
            return 0
        try:
            from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
            from gnwmanager.gnw import GnW
            from gnwmanager.filesystem import get_filesystem
        except Exception as e:                              # noqa: BLE001
            self._log(f"WARNING gnwmanager import failed: {e}")
            return -1
        backend = OpenOCDBackend(); backend.open()
        n = 0
        try:
            gnw = GnW(backend)
            with h.time_budget(120.0, "start_gnwmanager"):
                gnw.start_gnwmanager()
            fs = get_filesystem(gnw, offset=lfs_gnwmanager_offset(gnw.external_flash_size))
            files = []
            for top in tops:
                for root, _dirs, fnames in envprobe._safe_walk(fs, top):
                    files += [posixpath.join(root, fn) for fn in fnames]
            for path in files:
                try:
                    fs.remove(path); n += 1
                    self._log(f"  removed {path}")
                except Exception as e:                      # noqa: BLE001
                    self._log(f"  not removed {path} ({type(e).__name__})")
            with h.time_budget(30.0, "start bank1"):        # leave the chainloader runnable
                gnw.reset_and_halt()
                gnw.backend.write_register("msp", gnw.read_uint32(0x08000000))
                gnw.backend.write_register("pc", gnw.read_uint32(0x08000004))
                gnw.backend.resume()
        except Exception as e:                              # noqa: BLE001
            self._log(f"WARNING wipe failed: {type(e).__name__}: {e}")
            return -1
        finally:
            try:
                backend.close()
            except Exception:
                pass
        self._log(f"WIPE removed {n} files")
        return n

    def remove_paths(self, paths):
        if not paths:
            return
        self._log("remove " + ", ".join(paths))
        if self.dry:
            return
        args = [sys.executable, str(PUSH), "--rm"] + list(paths)
        subprocess.run(args, cwd=REPO_ROOT, check=False, timeout=240)

    # module name -> (device path, local build artifact). Mirrors the exact set +
    # paths that `make push_modules` deploys (Makefile.common) so push and wipe
    # agree with reality (mp3 is /modules/features/mp3.bin, NOT .../mp3/mp3.bin).
    MODULE_PATHS = {
        "fatfs":       ("/modules/filesystems/fatfs.bin",     BUILD / "fatfs.bin"),
        "lfs_rw":      ("/modules/filesystems/lfs_rw.bin",    BUILD / "lfs_rw.bin"),
        "theme":       ("/modules/theme/theme.bin",           BUILD / "theme.bin"),
        "language":    ("/modules/language.bin",              BUILD / "language.bin"),
        "installer":   ("/modules/installer.bin",             BUILD / "installer.bin"),
        "fileops":     ("/modules/fileops.bin",               BUILD / "fileops.bin"),
        "example":     ("/modules/features/example.bin",      BUILD / "example.bin"),
        "example_set": ("/modules/features/example_set.bin",  BUILD / "example_set.bin"),
        "mp3":         ("/modules/features/mp3.bin",          BUILD / "mp3.bin"),
    }

    def push_modules(self, names):
        """Push named PIE modules from their build artifacts (batched 3-4)."""
        pairs, missing = [], []
        for n in names:
            gp_lp = self.MODULE_PATHS.get(n)
            if gp_lp is None:
                missing.append(n); continue
            gp, lp = gp_lp
            if Path(lp).is_file() or self.dry:
                pairs.append((gp, lp))
            else:
                missing.append(f"{n} (build artifact {lp.name} absent; run make)")
        if missing:
            self._log("WARNING modules not pushed: " + ", ".join(missing))
        for i in range(0, len(pairs), 3):           # batch in groups of 3
            self._push(pairs[i:i + 3])

    def push_langs(self, codes):
        """Push /i18n/<code>.lang from build/i18n plus each pack's script font."""
        i18n = BUILD / "i18n"
        pairs, fonts_needed = [], set()
        for c in codes:
            pack = i18n / f"{c}.lang"
            if pack.is_file():
                pairs.append((f"/i18n/{c}.lang", pack))
                fonts_needed.add(_pack_script(pack))
            elif self.dry:
                pairs.append((f"/i18n/{c}.lang", pack))
            else:
                self._log(f"WARNING lang {c}: {pack.name} absent (run make i18n)")
        for scr in sorted(f for f in fonts_needed if f):
            fnt = i18n / "fonts" / f"{scr}.fnt"
            if fnt.is_file() or self.dry:
                pairs.append((f"/i18n/fonts/{scr}.fnt", fnt))
        for i in range(0, len(pairs), 3):
            self._push(pairs[i:i + 3])

    # ===================== flash regions (heavy) ========================
    def _gnw_flash(self, location, file, offset=0):
        self._log(f"gnwmanager flash {location} {Path(file).name} offset={offset}")
        if self.dry:
            return
        args = ["gnwmanager", "flash", str(location), "--file", str(file)]
        if offset:
            args += ["--offset", str(offset)]
        subprocess.run(args, cwd=REPO_ROOT, check=True, timeout=600)

    def flash_chainloader(self):
        """Flash the freshly built chainloader to Bank 1 (0x08000000)."""
        if not (CHAINLOADER_BIN.is_file() or self.dry):
            self._log(f"WARNING {CHAINLOADER_BIN.name} absent; run make first")
            return
        self._gnw_flash("bank1", CHAINLOADER_BIN)

    def set_retrogo_present(self, present: bool):
        """Toggle Retro-Go presence. present=True flashes the in-repo intflash
        payload to RETROGO_BASE; present=False writes a tiny zero blob over the
        reset-vector pair so board_is_valid_app(RETROGO_BASE) fails (cheap, one
        sector erase, avoids re-flashing 216 KiB)."""
        if present:
            if not (RETROGO_INTFLASH.is_file() or self.dry):
                self._log(f"WARNING cannot make RG present: {RETROGO_INTFLASH} absent")
                return
            self._gnw_flash(hex(envprobe.RETROGO_BASE), RETROGO_INTFLASH)
        else:
            blob = BUILD / "_rg_absent.bin"
            self._log("make RG absent: zero the reset-vector sector at RETROGO_BASE")
            if self.dry:
                return
            blob.write_bytes(b"\x00" * 16)
            self._gnw_flash(hex(envprobe.RETROGO_BASE), blob)

    def erase_bank2(self):
        """Invalidate the Bank-2 OFW image: flash a tiny zero blob at
        OFW_INTERNAL_BASE so its reset vector reads 0 and the slot looks empty.
        One sector erase, not a full 128 KiB write. Destructive (rebuild-from-
        source recovers by re-flashing the OFW through the LAUNCH menu)."""
        blob = BUILD / "_bank2_erase.bin"
        self._log("erase Bank 2: zero the OFW reset-vector sector")
        if self.dry:
            return
        blob.write_bytes(b"\x00" * 16)
        self._gnw_flash("bank2", blob)

    def erase_extflash_region(self, offset: int, size: int):
        """Erase an external-flash region by flashing 0xFF (for ENV-NO-EXTFLASH /
        ENV-CORRUPT). Destructive; only used by recipes that rebuild from source."""
        blob = BUILD / "_ext_erase.bin"
        self._log(f"erase ext flash @0x{offset:08X} size {size} (write 0xFF)")
        if self.dry:
            return
        blob.write_bytes(b"\xFF" * size)
        self._gnw_flash("ext", blob, offset=offset)

    # ---- full rebuild-from-source via the project's own Make targets ----
    # These wrap Makefile.common / Makefile.patch so the solved build+flash recipes
    # (OFW patching, the multi-region ext layout, the Retro-Go build) are reused,
    # never re-typed. They are HEAVY (a Retro-Go build + tens of MB of ext flashing
    # over SWD), so timeouts are generous and they are run only by ENV-DOCS and the
    # destructive-restore path.
    def _make(self, target, extra=None, timeout=2400):
        cmd = ["make", target] + (extra or [])
        self._log("$ " + " ".join(cmd))
        if self.dry:
            return True
        r = subprocess.run(cmd, cwd=REPO_ROOT, timeout=timeout)
        if r.returncode != 0:
            self._log(f"WARNING `{' '.join(cmd)}` returned {r.returncode}")
        return r.returncode == 0

    def flash_all_from_source(self):
        """make flash_all: build + patch OFW, then flash the chainloader (bank1)
        and the patched Mario/Zelda images to external flash. Unswaps banks first."""
        return self._make("flash-all", timeout=1200)

    def flash_retrogo(self):
        """make flash_rg: build Retro-Go and flash its intflash payload (RETROGO_BASE)
        + LittleFS image to external flash. Heavy (a Retro-Go build + a multi-MB flash)."""
        return self._make("flash-rg", timeout=3000)

    def push_all_modules(self):
        """make push_modules: deploy every PIE module to LittleFS."""
        return self._make("push-modules", timeout=900)

    def push_i18n_packs(self):
        """make push_i18n: cook + deploy all language packs + fonts to LittleFS."""
        return self._make("push-i18n", timeout=1200)

    def restore_from_build(self):
        """Restore all modules + i18n from the EXISTING build/ artifacts via direct
        pushes -- no `make`, so it NEVER recompiles src/. Safe to run alongside a
        source-editing session (and faster than the make targets). Assumes build/
        is already populated (it normally is)."""
        self.push_modules(list(self.MODULE_PATHS))
        i18n = BUILD / "i18n"
        codes = sorted(p.stem for p in i18n.glob("*.lang")) if i18n.is_dir() else []
        self.push_langs(codes)

    def full_provision_docs(self):
        """Rebuild the full ENV-DOCS golden image from source: chainloader + patched
        OFW + Retro-Go + all modules + all i18n packs. The canonical heavy restore
        after a destructive test. Returns True only if every stage succeeded."""
        self._log("FULL provision (rebuild-from-source: flash_all + flash_rg + "
                  "push_modules + push_i18n) -- heavy, minutes")
        ok = self.flash_all_from_source()
        ok = self.flash_retrogo() and ok
        ok = self.push_all_modules() and ok
        ok = self.push_i18n_packs() and ok
        if not self.dry:
            self.clean_reboot()
        return ok


# ============================ recipe layer ==============================
@dataclass
class Recipe:
    name: str
    build_flags: str | None = None        # None: reuse the flashed firmware
    retrogo: bool | None = None           # None: leave as-is
    modules: list | None = None           # exact /modules set; None: leave
    langs: list | None = None             # exact /i18n set; None: leave
    settings: dict | None = None          # {'fastboot':False,'lang':0,...}
    extflash: str | None = None           # None|'erase'
    bank2: str | None = None              # None|'erase'|'mario'|'zelda'
    full_image: bool = False              # rebuild the whole golden image from source
    manual_steps: list = field(default_factory=list)
    # expect(env) -> list[str] of failure messages ([] == verified)
    expect: object = None


def apply(recipe: Recipe, *, dry: bool = False, prompt=input) -> tuple:
    """Provision the device per `recipe`, prompt manual steps, then verify with
    envprobe. Returns (Environment, failures:list[str]). failures empty == the
    device reached the intended state."""
    p = Provisioner(dry=dry)
    print(f"\n=== provision {recipe.name} ===", flush=True)

    if recipe.full_image:
        # ENV-DOCS / destructive restore: rebuild everything from source via the
        # project's Make targets (chainloader + patched OFW + Retro-Go + modules +
        # i18n). Subsumes the piecemeal steps below.
        p.full_provision_docs()
        if recipe.settings is not None:
            p.set_settings_word(**recipe.settings)
    else:
        _apply_targeted(p, recipe)

    # No manual intervention: manual_steps are logged as assumptions, never
    # blocked on. A test that genuinely needs physical state (e.g. a specific SD
    # card) checks it via envprobe and skips/fails rather than waiting for a human.
    for step in recipe.manual_steps:
        p._log(f"ASSUMES (no manual step taken): {step}")
    p.clean_reboot()

    if dry:
        print("  (dry run: skipping envprobe verification)")
        return None, []

    env = envprobe.probe(with_lfs=(recipe.modules is not None or recipe.langs is not None
                                   or recipe.full_image))
    print(envprobe.summarize(env))
    failures = list(recipe.expect(env)) if callable(recipe.expect) else []
    if failures:
        print("  PROVISION VERIFY FAILURES:")
        for f in failures:
            print(f"    - {f}")
    else:
        print(f"  provision {recipe.name}: verified OK")
    return env, failures


def _apply_targeted(p, recipe):
    """The cheap, targeted provisioning path (everything except full_image)."""
    if recipe.build_flags is not None:
        # Reflash only when the recipe pins specific firmware.
        p.flash_chainloader()
    if recipe.retrogo is not None:
        p.set_retrogo_present(recipe.retrogo)
    if recipe.bank2 == "erase":
        p.erase_bank2()
    elif recipe.bank2 in ("mario", "zelda"):
        p._log(f"bank2={recipe.bank2}: flash the OFW through the LAUNCH menu "
               f"(OFW-lifecycle test) or gnwmanager flash-patch; not auto-flashed here")
    if recipe.extflash == "erase":
        p.erase_extflash_region(0x00000000, 0x00010000)   # wipe the partition head
    if recipe.modules is not None:
        p.push_modules(recipe.modules)
    if recipe.langs is not None:
        p.push_langs(recipe.langs)
    if recipe.settings is not None:
        p.set_settings_word(**recipe.settings)


# ----------------------------- helpers --------------------------------
def _hx(v):
    return "None" if v is None else f"0x{v:08X}"


def _pack_script(pack: Path) -> str | None:
    """Read the script name (LNG2 header, char[16] at offset 0x0C) of a .lang."""
    try:
        d = pack.read_bytes()
        return d[0x0C:0x1C].split(b"\x00", 1)[0].decode("ascii", "ignore") or None
    except Exception:
        return None


if __name__ == "__main__":
    # Dry-run a recipe to verify orchestration off-hardware:
    #   python3 scripts/common/provision.py
    demo = Recipe(name="DEMO", retrogo=True, modules=["theme", "language"],
                  langs=["it_IT", "de_DE"], settings={"fastboot": False, "lang": 0})
    apply(demo, dry=True)
