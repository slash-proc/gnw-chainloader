"""Named SWD observables — read device ground truth by intent, not raw addresses.

Per the QA plan (decision 1): tests assert through SWD symbols / registers / magic
cells FIRST and treat OCR as a layered, optional extra. This module is the DRY
home for those reads — each is a one-liner over `harness.read_u32_symbol` /
`safe_read_u32`, so a test says `observe.active_theme_slot(be)` instead of
re-resolving a symbol and masking bits in three different places.

Robustness contract: EVERY reader returns ``None`` when its symbol/cell can't be
read — symbol absent in this firmware variant (e.g. the ABI_SELFTEST globals in a
release build), an LTO rename that didn't match, or a transient probe failure
mid-reset. Callers degrade gracefully (record a note, skip the assertion) instead
of crashing, so the suite keeps moving even when an observable is missing. None of
these halt the CPU: like `read_menu_selection`, they go through the AHB-AP as
background accesses while the menu loop keeps running.

The symbol inventory is verified to exist in build/app/app.elf (with the usual
`.lto_priv.0` suffixes that `resolve_symbol` tolerates). Addresses move every
rebuild, so we always resolve fresh against the currently-built ELF — never
hardcode a symbol address here.
"""
from __future__ import annotations

from . import harness as h

# --- TAMP backup registers (STM32H7B0) -------------------------------------
# TAMP_BASE = SRD_APB4PERIPH_BASE (PERIPH_BASE 0x40000000 + 0x18000000) + 0x4400.
# Backup registers start at TAMP_BASE + 0x100 (BKP0R); BKP3R is +0x10C.
TAMP_BASE   = 0x58004400
TAMP_BKP0R  = TAMP_BASE + 0x100   # 0x58004500 — legacy BOOT-to-target latch (board.c)
TAMP_BKP3R  = TAMP_BASE + 0x10C   # 0x5800450C — packed settings word (boot_magic.h)

# --- Magic cells (src/common/memory_map.h) ---------------------------------
SRAM_MAGIC_ADDR   = 0x2001FFF8    # chainloader BOOT magic word
SRAM_MAGIC_TARGET = 0x2001FFFC    # jump target carried across the bank-swap reset
RG_MAGIC_ADDR     = 0x20000000    # Retro-Go "re-launch" cell (CORE / RESET)

# --- Module pool (src/chainloader/system/loader.c) -------------------------
MODULE_POOL_BASE  = 0x240C0000    # bump-allocator base; g_pool_next grows from here

# --- Packed settings word layout (src/common/boot_magic.h) -----------------
SETTINGS_SIG          = 0xA6
SETTINGS_SIG_SHIFT    = 24
SETTINGS_FASTBOOT_BIT = 1 << 0
SETTINGS_MARIO_SHIFT  = 8
SETTINGS_ZELDA_SHIFT  = 12
SETTINGS_SLOT_MASK    = 0xF
SETTINGS_LANG_SHIFT   = 16
SETTINGS_LANG_MASK    = 0xFF

# --- SD hardware-mod enum (src/chainloader/storage/sdcard.c sd_hw_t) --------
# Values confirmed against the source enum order; index by g_hw.
SD_HW_NAMES = {0: "UNDETECTED", 1: "NONE", 2: "SPI1", 3: "OSPI1"}


# --- generic None-safe readers ---------------------------------------------
def _sym(be, name, offset: int = 0):
    """Read a uint32 at symbol `name` (+offset), or None on any failure."""
    try:
        return h.read_u32_symbol(be, name, offset)
    except Exception:
        return None


def _addr(be, addr: int):
    """Read a uint32 at an absolute address, or None on failure (safe_read_u32)."""
    return h.safe_read_u32(be, addr)


# --- theme ------------------------------------------------------------------
def active_theme_slot(be):
    """Active UI theme slot: 0=DEFAULT, 1=FALLBACK, 2+=module sprite theme."""
    v = _sym(be, "ui_theme_slot")
    return None if v is None else v & 0xFF


def theme_module_count(be):
    """Number of sprite-theme modules the theme PIE module registered (0 if none)."""
    return _sym(be, "g_theme_module_count")


# --- feature modules --------------------------------------------------------
def feature_count(be):
    """Number of feature modules discovered + spliced into the menus."""
    return _sym(be, "g_feat_count")


def features_ran(be):
    """Bitmask/counter of feature modules that have been launched this session."""
    return _sym(be, "g_feat_ran")


# --- storage / partitions ---------------------------------------------------
def modules_ready(be):
    """True once the boot-time partition/module scan has completed."""
    v = _sym(be, "g_modules_ready")
    return None if v is None else bool(v)


def extflash_bytes(be):
    """Detected external OSPI flash size in bytes (0 when erased/absent)."""
    return _sym(be, "total_ext_flash_size")


def extflash_mb(be):
    """Detected external OSPI flash size in MiB, or None if unreadable."""
    v = extflash_bytes(be)
    return None if v is None else v >> 20


# --- SD card ----------------------------------------------------------------
def sd_hw(be):
    """SD hardware-mod state (g_hw): 0 UNDETECTED, 1 NONE, 2 SPI1, 3 OSPI1."""
    return _sym(be, "g_hw")


def sd_hw_name(be):
    v = sd_hw(be)
    return None if v is None else SD_HW_NAMES.get(v, f"HW{v}")


def sd_present(be):
    """True if a card is mounted (g_card_type != 0)."""
    v = _sym(be, "g_card_type")
    return None if v is None else bool(v)


def sd_state(be):
    """(hw_name, present) — the SD detection result over pure SWD, no OCR."""
    return sd_hw_name(be), sd_present(be)


# --- RW filesystem driver modules ------------------------------------------
def lfs_rw_loaded(be):
    v = _sym(be, "g_lfs_rw_loaded")
    return None if v is None else bool(v)


def fat_rw_loaded(be):
    v = _sym(be, "g_fat_rw_loaded")
    return None if v is None else bool(v)


def driver_count(be):
    """Number of VFS drivers currently registered."""
    return _sym(be, "g_driver_count")


def rw_drivers(be):
    """(lfs_rw_loaded, fat_rw_loaded, driver_count)."""
    return lfs_rw_loaded(be), fat_rw_loaded(be), driver_count(be)


# --- module loader ----------------------------------------------------------
def pool_next(be):
    """Current top of the module pool bump allocator."""
    return _sym(be, "g_pool_next")


def pool_used(be):
    """Bytes used in the module pool (g_pool_next - MODULE_POOL_BASE).

    The zero-cost transient-leak check: read it before and after a transient
    module load+return; a correct mod_pool_reset brings it back to the mark.
    """
    v = pool_next(be)
    return None if v is None else (v - MODULE_POOL_BASE) & 0xFFFFFFFF


def last_load_err(be):
    """Last module-loader error code (g_mod_load_err); 0 = no error."""
    return _sym(be, "g_mod_load_err")


# --- menus / navigation -----------------------------------------------------
def boot_target(be):
    """Current LAUNCH selector target (0 RETRO-GO, 1 MARIO, 2 ZELDA)."""
    v = _sym(be, "g_boot_target")
    return None if v is None else v & 0xFF


def menu_selection(be, list_symbol: str = "g_list_main"):
    """Highlighted index of a ui_list_t (None-safe wrapper of read_menu_selection)."""
    try:
        return h.read_menu_selection(be, list_symbol)
    except Exception:
        return None


def menu_count(be, list_symbol: str = "g_list_main"):
    """Number of items in a ui_list_t (num_items, at byte offset 4)."""
    return _sym(be, list_symbol, 4)


# --- power / idle / modals --------------------------------------------------
def uwtick(be):
    """The chainloader's millisecond SysTick counter."""
    return _sym(be, "uwTick")


def last_activity_tick(be):
    """uwTick value of the last user input (idle-hide reference)."""
    return _sym(be, "g_last_activity_tick")


def idle_ticks(be):
    """Milliseconds since the last input (uwTick - g_last_activity_tick).

    The menu auto-hides after ~30000 ms idle; this is how the power/idle test
    observes the timer crossing the threshold without OCR.
    """
    now = uwtick(be)
    last = last_activity_tick(be)
    if now is None or last is None:
        return None
    return (now - last) & 0xFFFFFFFF


def modal_depth(be):
    """Modal/page stack pointer (g_stack_ptr): >0 means a dialog/sub-page is open."""
    return _sym(be, "g_stack_ptr")


# --- file browser -----------------------------------------------------------
def op_cancelled(be):
    """True if the last long file operation was cooperatively cancelled."""
    v = _sym(be, "g_op_cancelled")
    return None if v is None else bool(v)


def active_tab(be):
    """File-browser active filesystem tab index."""
    return _sym(be, "g_active_tab")


# --- packed settings word (TAMP->BKP3R) ------------------------------------
def settings_word(be):
    """Raw battery-backed settings word from TAMP->BKP3R (None if unreadable)."""
    return _addr(be, TAMP_BKP3R)


def settings_valid(w) -> bool:
    return w is not None and ((w >> SETTINGS_SIG_SHIFT) & 0xFF) == SETTINGS_SIG


def settings_fastboot(w) -> bool:
    return settings_valid(w) and bool(w & SETTINGS_FASTBOOT_BIT)


def settings_mario_slot(w) -> int:
    return ((w >> SETTINGS_MARIO_SHIFT) & SETTINGS_SLOT_MASK) if settings_valid(w) else 0


def settings_zelda_slot(w) -> int:
    return ((w >> SETTINGS_ZELDA_SHIFT) & SETTINGS_SLOT_MASK) if settings_valid(w) else 0


def settings_lang(w) -> int:
    return ((w >> SETTINGS_LANG_SHIFT) & SETTINGS_LANG_MASK) if settings_valid(w) else 0


def settings_make(fastboot: bool, mario_slot: int, zelda_slot: int, lang: int) -> int:
    """Rebuild the whole settings word (mirror of boot_magic.h settings_make)."""
    return ((SETTINGS_SIG << SETTINGS_SIG_SHIFT)
            | ((lang & SETTINGS_LANG_MASK) << SETTINGS_LANG_SHIFT)
            | ((zelda_slot & SETTINGS_SLOT_MASK) << SETTINGS_ZELDA_SHIFT)
            | ((mario_slot & SETTINGS_SLOT_MASK) << SETTINGS_MARIO_SHIFT)
            | (SETTINGS_FASTBOOT_BIT if fastboot else 0))


def fastboot_enabled(be):
    """Decoded fast-boot bit from the live settings word."""
    w = settings_word(be)
    return None if w is None else settings_fastboot(w)


def active_lang_index(be):
    """Persisted UI language index (0 = English) from the settings word.

    This is the free replacement for the moved g_current/g_langs symbols: the
    selected language survives reset in BKP3R, readable over pure SWD.
    """
    w = settings_word(be)
    return None if w is None else settings_lang(w)


# --- boot magic cells -------------------------------------------------------
def boot_magic_cells(be) -> dict:
    """All three boot-magic cells: {'sram_magic','sram_target','rg_magic'}."""
    return {
        "sram_magic":  _addr(be, SRAM_MAGIC_ADDR),
        "sram_target": _addr(be, SRAM_MAGIC_TARGET),
        "rg_magic":    _addr(be, RG_MAGIC_ADDR),
    }
