"""Environment probe — read the live device and return one structured snapshot.

The keystone of "run on ANY setup": point it at a bench device and it reports what
is installed (Retro-Go present?, Bank 2 OFW type, extflash size, SD state, loaded
RW drivers, theme/feature module counts, persisted settings, boot-magic cells),
all over SWD ground truth, no OCR in the hot path. The suite uses it two ways:

  * Adaptive run: detect what's on the device, run every test valid for that
    state, prove the invariant, report coverage.
  * Verification: after provisioning a named environment, confirm the device
    actually reached the intended state before any test trusts it.

`probe()` is strictly READ-ONLY and never reboots: it takes the SWD snapshot with
the CPU left running. Listing LittleFS content is heavier (it loads the gnwmanager
RAM flasher and reboots the chainloader afterward), so it is a separate, opt-in
phase (`with_lfs=True` or `list_lfs_content()`), used by provision verification
where a reboot is acceptable.

Serialization rule (see memory: one SWD/probe process at a time): when `probe()`
owns its backend it CLOSES it before any gnwmanager filesystem session; when a
backend is injected, the caller owns serialization and LFS listing is skipped
unless explicitly requested with no live backend.
"""
from __future__ import annotations

from dataclasses import dataclass, field

if __package__ in (None, ""):                     # allow `python3 envprobe.py` (CLI)
    import pathlib
    import sys
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    __package__ = "common"

from . import harness as h
from . import observe

# --- Reset-vector constants (single source; boot_selector_test imports these) ---
# The OFW's reset vector, used to recognize which OFW a Bank-2 image / a running
# core is. Values are the patched-OFW entry points (board.c / boot_selector_test).
OFW_RESET_VEC = {"MARIO": 0x08018101, "ZELDA": 0x0801B3E1}
BANK2_RESET_VEC_ADDR  = 0x08100004   # Bank 2 backup copy, valid when NOT swapped
ACTIVE_RESET_VEC_ADDR = 0x08000004   # whatever is mapped at 0x08000000 right now
RETROGO_BASE = 0x0800A000            # Retro-Go launcher payload (Bank 1)

# --- FLASH option-byte swap bit (keep in sync with scripts/debug/bank.py) ---
# Read-only status check; the dangerous swap *write* path lives in bank.py and is
# never reimplemented here.
FLASH_OPTSR_CUR = 0x5200201C
SWAP_BANK_BIT   = 0x80000000


@dataclass(frozen=True)
class Environment:
    # internal flash / boot
    chainloader_alive: bool = False
    retrogo_present: bool | None = None
    bank_swapped: bool | None = None
    ofw_in_bank2: str | None = None        # 'MARIO'|'ZELDA'|'UNKNOWN'|None
    ofw_running: str | None = None         # set only when bank_swapped
    # external flash / storage
    extflash_mb: int | None = None
    modules_ready: bool | None = None
    sd_hw: str | None = None               # 'UNDETECTED'|'NONE'|'SPI1'|'OSPI1'
    sd_present: bool | None = None
    lfs_rw_loaded: bool | None = None
    fat_rw_loaded: bool | None = None
    driver_count: int | None = None
    theme_module_count: int | None = None
    feature_count: int | None = None
    # littlefs content (heavy, opt-in)
    lfs_modules: frozenset = field(default_factory=frozenset)
    lfs_langs: frozenset = field(default_factory=frozenset)
    lfs_listed: bool = False
    # persistent registers / magic
    fastboot: bool | None = None
    lang_index: int | None = None
    mario_theme_slot: int | None = None
    zelda_theme_slot: int | None = None
    magic_sram: int | None = None
    magic_target: int | None = None
    magic_rg: int | None = None
    # live UI snapshot
    menu_selection: int | None = None
    active_theme_slot: int | None = None
    # metadata
    notes: tuple = ()


def _ofw_name(vec):
    """Map a reset vector to an OFW name; 'UNKNOWN' if it looks like a valid app
    image we don't recognize, None if it's erased/garbage."""
    if vec is None:
        return None
    for name, v in OFW_RESET_VEC.items():
        if vec == v:
            return name
    pc = vec & ~1
    if (pc >> 24) == 0x08 and (vec & 1) == 1 and pc != 0x08FFFFFE:
        return "UNKNOWN"
    return None


def _app_valid(be, base):
    """Cheap board_is_valid_app proxy: a sane initial SP + a thumb reset vector
    in internal flash. None if unreadable."""
    sp = h.safe_read_u32(be, base)
    pc = h.safe_read_u32(be, base + 4)
    if sp is None or pc is None:
        return None
    sp_ok = (sp >> 24) in (0x20, 0x24, 0x30, 0x38) and sp not in (0, 0xFFFFFFFF)
    pc_ok = (pc >> 24) == 0x08 and (pc & 1) == 1 and 0x08000000 <= (pc & ~1) < 0x08200000
    return bool(sp_ok and pc_ok)


def probe(backend=None, *, with_lfs: bool = False, dev=None) -> Environment:
    """Read the device and return an Environment. Read-only (no reboot) for the
    SWD snapshot; LFS listing (with_lfs) is the heavier opt-in phase.

    Pass `backend` to share an open OpenOCD session (preferred inside the runner),
    or `dev` (a RemoteInput) to borrow its backend, or neither to open + close our
    own. Any unreadable field becomes None and is recorded in `notes`.
    """
    own = False
    be = backend or (dev.backend if dev is not None else None)
    if be is None:
        from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
        be = OpenOCDBackend(); be.open(); own = True

    notes = []
    fields = {}
    try:
        with h.time_budget(20.0, "envprobe SWD snapshot"):
            ok, detail = h.chainloader_running(be)
            fields["chainloader_alive"] = ok
            if not ok:
                notes.append(f"chainloader not confirmed live ({detail}); reads may be stale")

            swapped_raw = h.safe_read_u32(be, FLASH_OPTSR_CUR)
            fields["bank_swapped"] = (None if swapped_raw is None
                                      else bool(swapped_raw & SWAP_BANK_BIT))

            fields["retrogo_present"] = _app_valid(be, RETROGO_BASE)
            fields["ofw_in_bank2"] = _ofw_name(h.safe_read_u32(be, BANK2_RESET_VEC_ADDR))
            if fields["bank_swapped"]:
                fields["ofw_running"] = _ofw_name(h.safe_read_u32(be, ACTIVE_RESET_VEC_ADDR))

            fields["extflash_mb"] = observe.extflash_mb(be)
            fields["modules_ready"] = observe.modules_ready(be)
            fields["sd_hw"] = observe.sd_hw_name(be)
            fields["sd_present"] = observe.sd_present(be)
            fields["lfs_rw_loaded"] = observe.lfs_rw_loaded(be)
            fields["fat_rw_loaded"] = observe.fat_rw_loaded(be)
            fields["driver_count"] = observe.driver_count(be)
            fields["theme_module_count"] = observe.theme_module_count(be)
            fields["feature_count"] = observe.feature_count(be)

            w = observe.settings_word(be)
            fields["fastboot"] = None if w is None else observe.settings_fastboot(w)
            fields["lang_index"] = None if w is None else observe.settings_lang(w)
            fields["mario_theme_slot"] = None if w is None else observe.settings_mario_slot(w)
            fields["zelda_theme_slot"] = None if w is None else observe.settings_zelda_slot(w)

            cells = observe.boot_magic_cells(be)
            fields["magic_sram"] = cells["sram_magic"]
            fields["magic_target"] = cells["sram_target"]
            fields["magic_rg"] = cells["rg_magic"]

            fields["menu_selection"] = observe.menu_selection(be)
            fields["active_theme_slot"] = observe.active_theme_slot(be)
    except TimeoutError as e:
        notes.append(f"SWD snapshot timed out: {e}")
    finally:
        if own:
            try:
                with h.time_budget(10.0, "envprobe backend close"):
                    be.close()
            except Exception:
                pass

    if with_lfs:
        # Only safe with no live backend held (gnwmanager spawns its own openocd).
        if backend is not None or (dev is not None and dev.backend is not None):
            notes.append("LFS listing skipped: a live backend is held; run probe() "
                         "standalone (backend=None) to list LittleFS content")
        else:
            mods, langs, lfs_note = list_lfs_content()
            if mods is not None:
                fields["lfs_modules"] = frozenset(mods)
                fields["lfs_langs"] = frozenset(langs)
                fields["lfs_listed"] = True
            if lfs_note:
                notes.append(lfs_note)

    fields["notes"] = tuple(notes)
    return Environment(**fields)


def list_lfs_content():
    """Walk LittleFS for /modules/**.bin and /i18n/*.lang. HEAVY: loads the
    gnwmanager RAM flasher and reboots the chainloader (start bank1) afterward.

    Returns (modules:set|None, langs:set|None, note:str|None). On any failure
    returns (None, None, reason) so callers degrade gracefully.
    """
    try:
        from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
        from gnwmanager.gnw import GnW
        from gnwmanager.filesystem import get_filesystem
    except Exception as e:
        return None, None, f"gnwmanager import failed: {e}"

    backend = OpenOCDBackend()
    try:
        backend.open()
    except Exception as e:
        return None, None, f"openocd open failed: {e}"
    mods, langs = set(), set()
    note = None
    try:
        gnw = GnW(backend)
        with h.time_budget(90.0, "start_gnwmanager"):
            gnw.start_gnwmanager()
        fs = get_filesystem(gnw, offset=0)
        for root, _dirs, files in _safe_walk(fs, "/modules"):
            for fn in files:
                if fn.endswith(".bin"):
                    mods.add(fn)
        for root, _dirs, files in _safe_walk(fs, "/i18n"):
            for fn in files:
                if fn.endswith(".lang"):
                    langs.add(fn[:-5])     # 'it_IT.lang' -> 'it_IT'
        # Reboot the chainloader so the device is left runnable (mirror push_batched).
        with h.time_budget(30.0, "start bank1"):
            gnw.reset_and_halt()
            BANK1 = 0x08000000
            gnw.backend.write_register("msp", gnw.read_uint32(BANK1))
            gnw.backend.write_register("pc", gnw.read_uint32(BANK1 + 4))
            gnw.backend.resume()
    except Exception as e:
        note = f"LFS walk failed: {type(e).__name__}: {e}"
        return None, None, note
    finally:
        try:
            backend.close()
        except Exception:
            pass
    return mods, langs, note


def _safe_walk(fs, top):
    """fs.walk(top) that yields nothing if `top` is absent."""
    try:
        yield from fs.walk(top)
    except Exception:
        return


def summarize(env: Environment) -> str:
    """A one-screen human dump of an Environment (adaptive mode + post-provision)."""
    def y(v):
        return "?" if v is None else ("yes" if v is True else ("no" if v is False else str(v)))

    lines = [
        "--- device environment ---",
        f"  chainloader alive : {y(env.chainloader_alive)}    bank swapped: {y(env.bank_swapped)}",
        f"  retro-go present  : {y(env.retrogo_present)}",
        f"  bank2 OFW         : {y(env.ofw_in_bank2)}" + (
            f"   running OFW: {y(env.ofw_running)}" if env.ofw_running else ""),
        f"  extflash MB       : {y(env.extflash_mb)}    modules scanned: {y(env.modules_ready)}",
        f"  SD                : hw={y(env.sd_hw)} present={y(env.sd_present)}",
        f"  RW drivers        : lfs={y(env.lfs_rw_loaded)} fat={y(env.fat_rw_loaded)} count={y(env.driver_count)}",
        f"  modules           : theme={y(env.theme_module_count)} feature={y(env.feature_count)}",
        f"  settings (BKP3R)  : fastboot={y(env.fastboot)} lang_idx={y(env.lang_index)} "
        f"mario_slot={y(env.mario_theme_slot)} zelda_slot={y(env.zelda_theme_slot)}",
        f"  magic cells       : SRAM={_hx(env.magic_sram)} TGT={_hx(env.magic_target)} RG={_hx(env.magic_rg)}",
        f"  live UI           : menu_sel={y(env.menu_selection)} theme_slot={y(env.active_theme_slot)}",
    ]
    if env.lfs_listed:
        lines.append(f"  LFS modules       : {', '.join(sorted(env.lfs_modules)) or '(none)'}")
        lines.append(f"  LFS langs         : {', '.join(sorted(env.lfs_langs)) or '(none)'}")
    for n in env.notes:
        lines.append(f"  note: {n}")
    return "\n".join(lines)


def _hx(v):
    return "?" if v is None else f"0x{v:08X}"


if __name__ == "__main__":
    # Standalone: print the environment of whatever is on the bench.
    import argparse
    ap = argparse.ArgumentParser(description="Probe and print the device environment.")
    ap.add_argument("--lfs", action="store_true", help="also list LittleFS content (heavier; reboots chainloader)")
    args = ap.parse_args()
    print(summarize(probe(with_lfs=args.lfs)))
