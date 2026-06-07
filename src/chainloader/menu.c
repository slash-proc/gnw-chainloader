#include "menu.h"
#include "board.h"
#include "gui.h"
#include "assets.h"
#include <string.h>
#include "partition.h"
#include "flash.h"
#include "utils.h"
#include "input.h"
#include "ui.h"
#include "ui_list.h"
#include "strings.h"
#include "i18n.h"
#include "theme.h"
#include "system/bench.h"
#include "system/crash_log.h"   /* crash_log_init + the boot-time crash notice (always) */
#include "../common/memory_map.h"
#include "../common/boot_magic.h"
#include "storage/vfs.h"
#include "system/loader.h"
#include "system/installer.h"
#include "system/module.h"
#include "system/feature.h"
#include "system/ofw_verify.h"   /* retrogo_bootable (Retro-Go boot-time CRC gate) */

#ifdef ABI_SELFTEST
/* On-device ABI-gate self-test (scripts/tests/test_abi_reject.py). Compiled in
 * ONLY with `make ABI_SELFTEST=1`; never in a release. Runs the REAL gates once
 * the filesystems are up and leaves two SWD-readable results:
 *   g_abi_selftest_mod  = vfs_read_module("/modules/_selftest.bin", ...) return:
 *                         0 = a valid-ABI module was found (accepted),
 *                         0xFFFFFFFF (-1) = rejected or absent.
 *   g_abi_selftest_pack = vfs_lfs_lang_version("/i18n/_selftest.lang", ...):
 *                         >0 = pack version (accepted), 0 = rejected or absent.
 * Both start as 0x5A5A5A5A so the harness can tell "hook has not run yet" apart
 * from a real result. */
#include "storage/vfs.h"
volatile uint32_t g_abi_selftest_mod  __attribute__((used)) = 0x5A5A5A5Au;
volatile uint32_t g_abi_selftest_pack __attribute__((used)) = 0x5A5A5A5Au;
static uint8_t abi_selftest_buf[2048];
static void abi_selftest_run(void) {
    uint32_t sz = 0;
    g_abi_selftest_mod  = (uint32_t)vfs_read_module("/modules/_selftest.bin",
                                                    abi_selftest_buf, sizeof(abi_selftest_buf), &sz);
    g_abi_selftest_pack = vfs_lfs_lang_version("/i18n/_selftest.lang", 0);
}
#endif

/* --- Action Stubs --- */
extern ui_window_t PAGE_BROWSER;
void browser_open(void);   /* ui_file_browser.c: open with full ops (default config) */
static void action_browser_enter(void) { browser_open(); }
static void action_retro_go(void) { board_request_jump(RETROGO_BASE); }
static void action_ofw(void)      { board_jump_to_app(OFW_INTERNAL_BASE); }
static void action_pwr(void)      { menu_enter_standby(); }

/* Unified boot selector: one main-menu item cycles among the bootable targets
 * (Retro-Go in Bank 1, plus whichever OFW backups exist). LEFT/RIGHT only changes
 * the selection; A boots it, flashing an OFW backup into Bank 2 first if that OFW
 * isn't already the active one (a deliberate A-press, so progress only, no prompt). */
typedef enum { BT_RETROGO, BT_MARIO, BT_ZELDA, BT_COUNT } boot_target_t;
static boot_target_t g_boot_target = BT_RETROGO;

/* An OFW backup is present on external flash when its stock reset vector sits at
 * the known SPI offset (the same signature the partition scanner keys on).
 * Guarded by the detected flash size so we never read past the chip. */
static bool ofw_backup_present(board_console_type_t c) {
    uint32_t off = (c == CONSOLE_ZELDA) ? ZELDA_SPI_OFFSET : MARIO_SPI_OFFSET;
    uint32_t pc  = (c == CONSOLE_ZELDA) ? 0x0801B3E1UL : 0x08018101UL;
    if (total_ext_flash_size == 0 || off + 8 > total_ext_flash_size) return false;
    return *(volatile uint32_t *)(EXTFLASH_BASE + off + 4) == pc;
}

/* A console is bootable if it's the active OFW (valid in Bank 2) or has a backup
 * on external flash that can be flashed in first. */
static bool ofw_bootable(board_console_type_t c) {
    if (board_console_type == c && board_is_valid_app(OFW_INTERNAL_BASE)) return true;
    return ofw_backup_present(c);
}

/* Is a boot target reachable right now? Retro-Go lives in Bank 1; Mario/Zelda are
 * bootable when active in Bank 2 or restorable from an external-flash backup. */
static bool target_bootable(boot_target_t t) {
    switch (t) {
        case BT_RETROGO: return retrogo_bootable();
        case BT_MARIO:   return ofw_bootable(CONSOLE_MARIO);
        case BT_ZELDA:   return ofw_bootable(CONSOLE_ZELDA);
        default:         return false;
    }
}
static bool any_target_bootable(void) {
    return target_bootable(BT_RETROGO) || target_bootable(BT_MARIO) || target_bootable(BT_ZELDA);
}
/* Selector value name. Retro-Go / Mario / Zelda are proper nouns, kept literal
 * (not part of the translatable set), as MARIO/ZELDA are elsewhere. */
static const char *target_name(boot_target_t t) {
    switch (t) {
        case BT_RETROGO: return "Retro-Go";
        case BT_MARIO:   return "Mario";
        case BT_ZELDA:   return "Zelda";
        default:         return "";
    }
}
/* Step to the next bootable target in `dir` (+1 / -1), wrapping; stays put if none. */
static boot_target_t next_bootable(boot_target_t t, int dir) {
    int step = (dir < 0) ? -1 : 1;
    for (int i = 0; i < BT_COUNT; i++) {
        t = (boot_target_t)(((int)t + step + BT_COUNT) % BT_COUNT);
        if (target_bootable(t)) return t;
    }
    return t;
}

/* --- Forward Declarations --- */
static void menu_main_update(ui_window_t *self);
static void menu_main_draw(ui_window_t *self);
static void menu_tools_update(ui_window_t *self);
static void menu_tools_draw(ui_window_t *self);
static void menu_settings_update(ui_window_t *self);
static void menu_settings_draw(ui_window_t *self);
static void action_settings_enter(void);
static void menu_apply_language(void);
static void menu_notify_installed(int nl, int nm);

static ui_window_t PAGE_MAIN = {
    .title = NULL,   /* set via tr() in menu_main_enter (language-switch safe) */
    .x = 50, .y = 55, .w = 220, .h = 130,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 1,
    .draw_content = menu_main_draw,
    .update_content = menu_main_update,
    .exit = NULL
};

static ui_window_t PAGE_TOOLS = {
    .title = NULL,   /* set via tr() in menu_tools_enter */
    .x = 50, .y = 55, .w = 220, .h = 130,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 0,
    .draw_content = menu_tools_draw,
    .update_content = menu_tools_update,
    .exit = NULL
};

static ui_window_t PAGE_SETTINGS = {
    .title = NULL,   /* set via tr() in menu_settings_enter */
    .x = 50, .y = 55, .w = 220, .h = 130,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 0,
    .draw_content = menu_settings_draw,
    .update_content = menu_settings_update,
    .exit = NULL
};

/* --- State --- */
static ui_list_t g_list_main;
static ui_list_t g_list_tools;
static ui_list_t g_list_settings;

/* --- Main Menu Implementation --- */

static void action_tools_enter(void) { ui_push(&PAGE_TOOLS); }

enum { MM_BOOT, MM_TOOLS, MM_SETTINGS, MM_POWER, MM_COUNT };

/* Compose a value-selector label into `buf`: "label: < value >" (bracketed cycler,
 * e.g. LAUNCH/THEME/LANGUAGE) or "label: value" (plain toggle, e.g. FAST-BOOT). The
 * single composition point for these selectors -- Phase B makes it RTL-aware. */
static void compose_value(const char *label, const char *value, bool bracketed, char *buf, int cap) {
    /* Frame the cycled value with the selector brackets (decoration), then place the
     * label, separator and value in reading order. gui_compose handles LTR vs RTL
     * ordering (reversing the pieces for RTL) and is bounded, so an over-long translated
     * label/value truncates rather than overflowing (stability is law). */
    char framed[96];
    if (bracketed) {
        /* The cycler arrows always point OUTWARD ("< value >") in both LTR and RTL --
         * only the value's POSITION in the line mirrors (the gui_compose below), not the
         * bracket glyphs. Built directly rather than through gui_compose so the RTL piece-
         * reversal can't flip "< >" into the inward-pointing "> <" (the swapped-arrow look).
         * Bounded: an over-long value truncates rather than overflowing (stability is law). */
        framed[0] = '\0';
        str_lcat(framed, sizeof(framed), "< ");
        str_lcat(framed, sizeof(framed), value);
        str_lcat(framed, sizeof(framed), " >");
        value = framed;
    }
    const char *p[3] = { label, ": ", value };
    gui_compose(buf, cap, p, 3);
}

static const char* get_main_label(int idx) {
    switch (idx) {
        case MM_BOOT: {
            /* "LAUNCH: < target >" value-selector, matching THEME/LANGUAGE: the
             * translated label both names the action and buffers the menu cursor
             * (">") from the value bracket; LEFT/RIGHT cycle the target and A
             * launches it. Greyed + label-only when nothing is bootable. */
            if (!any_target_bootable()) return tr(STR_LAUNCH);
            static char buf[96];
            compose_value(tr(STR_LAUNCH), target_name(g_boot_target), true, buf, sizeof(buf));
            return buf;
        }
        case MM_TOOLS:    return tr(STR_TITLE_TOOLS);
        case MM_SETTINGS: return tr(STR_TITLE_SETTINGS);
        case MM_POWER:    return tr(STR_POWER_OFF);
    }
    return "";
}

/* Grey out boot targets that aren't reachable. TOOLS / POWER OFF are always
 * enabled, so the menu can never become fully unnavigable (boot-reachability). */
static bool main_is_enabled(int idx) {
    switch (idx) {
        case MM_BOOT: return any_target_bootable();
        default:      return true;
    }
}

/* LEFT/RIGHT only changes which target is selected (no flash); A boots it. */
static void on_main_adjust(int idx, int dir) {
    if (idx != MM_BOOT) return;
    g_boot_target = next_bootable(g_boot_target, dir);
}

/* Flash the selected console's OFW backup into Bank 2 when it isn't already the valid
 * active OFW. Returns true if a flash happened; false (no-op) when the OFW is already
 * active or no external backup exists. The single shared flash point for both A-boot and
 * the GAME-button theme preview: a deliberate button press is the confirmation, so it
 * shows a progress bar only (no confirm prompt). Reuses the trusted partition_flash_ofw
 * path -- it targets Bank 2 only and never touches Bank 1 / the boot path. */
static bool ensure_ofw_flashed(board_console_type_t want) {
    bool active_valid = (want == board_console_type) && board_is_valid_app(OFW_INTERNAL_BASE);
    if (active_valid || !ofw_backup_present(want)) return false;
    /* "<Console> OFW" flash-progress title -- the console name is a proper noun (kept
     * literal); the "OFW" word/order comes from the translated template. */
    bool zelda = (want == CONSOLE_ZELDA);
    static char ofw_title[32];
    str_fmt1_str(ofw_title, sizeof(ofw_title), tr(STR_OFW_SUFFIX), zelda ? "Zelda" : "Mario");
    /* Returns false (and Bank 2 is left untouched) if CRC verification of the
     * backup/asset blob failed -- propagate that so callers don't boot or re-theme. */
    return partition_flash_ofw(ofw_title, zelda ? ZELDA_SPI_OFFSET : MARIO_SPI_OFFSET, OFW_INTERNAL_SIZE);
}

/* True when the selected console can be booted right now: it's already the valid
 * active OFW in Bank 2, or a verified backup was just flashed in. A failed CRC
 * verification (or no backup) returns false so we never jump to an OFW that isn't
 * actually bootable. */
static bool ofw_ready_to_boot(board_console_type_t want) {
    if (want == board_console_type && board_is_valid_app(OFW_INTERNAL_BASE)) return true;
    return ensure_ofw_flashed(want);
}

/* Boot the selected target. Retro-Go jumps straight to Bank 1. For an OFW, flash its
 * backup into Bank 2 first if needed (deliberate A-press), then jump. */
static void boot_selected_target(void) {
    if (g_boot_target == BT_RETROGO) { action_retro_go(); return; }
    board_console_type_t want = (g_boot_target == BT_ZELDA) ? CONSOLE_ZELDA : CONSOLE_MARIO;
    /* Only jump if Bank 2 holds a verified OFW -- a failed CRC check leaves it
     * untouched and we stay in the menu (partition_flash_ofw already showed why). */
    if (ofw_ready_to_boot(want)) action_ofw();
}

/* GAME button on the Launch row: flash the selected OFW backup into Bank 2 to preview
 * its theme -- no boot, no bank swap. Never for Retro-Go (not an OFW), and only when the
 * OFW isn't already active (ensure_ofw_flashed guards both -> nothing to do otherwise).
 * After a flash, re-detect the console and reload its theme colours/sprites -- the same
 * trio board_init runs at boot -- so the chainloader UI re-themes live for the preview. */
static void on_main_game(int idx) {
    if (idx != MM_BOOT || g_boot_target == BT_RETROGO) return;
    board_console_type_t want = (g_boot_target == BT_ZELDA) ? CONSOLE_ZELDA : CONSOLE_MARIO;
    if (ensure_ofw_flashed(want)) {
        board_detect_console_type();    /* Bank 2 now holds the new OFW */
        board_load_dynamic_assets();    /* reload theme colours + sprites for the preview */
    }
}

static void on_main_action(int idx) {
    switch (idx) {
        case MM_BOOT:     boot_selected_target(); break;
        case MM_SETTINGS: action_settings_enter(); break;
        case MM_TOOLS:    action_tools_enter(); break;
        case MM_POWER:    action_pwr(); break;
    }
}

static void menu_main_enter(void) {
    /* Default the selector to Retro-Go when it's bootable (the common, no-flash
     * target); otherwise to the active OFW, else any bootable target. */
    if (target_bootable(BT_RETROGO))                                           g_boot_target = BT_RETROGO;
    else if (board_console_type == CONSOLE_MARIO && target_bootable(BT_MARIO)) g_boot_target = BT_MARIO;
    else if (board_console_type == CONSOLE_ZELDA && target_bootable(BT_ZELDA)) g_boot_target = BT_ZELDA;
    else if (target_bootable(BT_MARIO))                                        g_boot_target = BT_MARIO;
    else                                                                       g_boot_target = BT_ZELDA;

    PAGE_MAIN.title = tr(STR_TITLE_MAIN);
    ui_list_init(&g_list_main, tr(STR_TITLE_MAIN), MM_COUNT, get_main_label, on_main_action);
    g_list_main.visible_lines = 6;
    g_list_main.on_adjust = on_main_adjust;
    g_list_main.on_game = on_main_game;   /* GAME flashes the selected OFW (theme preview) */
    g_list_main.is_enabled = main_is_enabled;
}

/* --- SD->LittleFS language install (transient installer module) --------------
 *
 * The install lives in /modules/installer.bin (MOD_FLAG_TRANSIENT): the core loads
 * it on demand, runs it, and reclaims its pool slot, so its code + ~16 KB scratch
 * buffer are absent for the rest of the session. scan() counts new packs; if any,
 * one confirm; on accept commit() installs them, gated by the running firmware's
 * ABI (the install gate mirrors the loader's load gate). */
static const installer_host_t g_installer_host = {
    .read_header   = vfs_read_header,
    .copy          = vfs_install_copy,
    .unlink        = vfs_install_unlink,
    .part_count    = partition_get_count,
    .part_info     = partition_get_info,
    .part_is_sd    = partition_is_sd,
    .part_fs       = partition_fs_code,
    .sd_dir_exists = vfs_sd_dir_exists,
    .sd_list_langs = vfs_sd_list_langs,
    .progress      = NULL,
    .strings_abi   = STRINGS_ABI_VERSION,
    .str_count     = (uint16_t)STR_COUNT,
    .module_abi    = MODULE_ABI_VERSION,
};

#define INSTALLER_PATH "/modules/installer.bin"

/* True if an SD /i18n folder is present (the only case worth loading the installer).
 * Loads the LFN FAT module RESIDENT first -- so it sits below the installer's pool
 * mark and survives the transient reclaim; freeing a driver the vfs still points at
 * would be a use-after-free. */
static bool installer_prep_fs(void) {
    if (!vfs_sd_dir_exists("/i18n")) return false;
    if (!vfs_is_fat_rw_loaded() && vfs_module_available("/fs/fat.bin"))
        vfs_load_dynamic_driver("FAT", "/fs/fat.bin");
    return true;
}

/* Confirm accept: reload the transient installer, commit, reclaim its slot, and fold
 * the new packs into the language list. */
static void menu_do_install(void) {
    if (!installer_prep_fs()) return;
    /* The commit WRITES to LittleFS via the lfs_rw module; load it RESIDENT here,
     * before the installer's pool mark, so the commit's first write doesn't load it
     * ABOVE the mark where mod_pool_reset would then free it -- a use-after-free on
     * the next LittleFS write (e.g. the language-switch persist). Same reasoning as
     * the FAT load in installer_prep_fs. */
    if (!vfs_is_lfs_rw_loaded() && vfs_module_available("/fs/lfs.bin"))
        vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    uint32_t mark = mod_pool_mark();
    installer_api_t api = {0};
    int nl = 0, nm = 0;
    if (mod_load_installer(INSTALLER_PATH, &g_installer_host, &api) && api.commit) {
        nl = api.commit(&g_installer_host, INSTALLER_KIND_LANG);
        nm = api.commit(&g_installer_host, INSTALLER_KIND_MODULE);
    }
    mod_pool_reset(mark);
    if (nl > 0) { i18n_rediscover(); menu_apply_language(); }   /* languages apply live */
    if (nl > 0 || nm > 0) menu_notify_installed(nl, nm);        /* installed modules apply on reboot */
}

/* One-shot at boot once the filesystems are up: count new SD language packs and, if
 * any, ask to install them. The installer is reclaimed after the scan; the menu is
 * untouched when there is nothing new (or no SD / no installer module). */
/* Build the translatable "<N languages>[, <M modules>]" item phrase into buf: each
 * count phrase ("%d languages" / "%d modules") is spliced per-language; the comma is
 * structural. Shared by the install prompt and the post-install notice. */
static void build_install_phrase(char *buf, int cap, int nl, int nm) {
    char tmp[32];
    buf[0] = '\0';
    if (nl > 0) { str_fmt1_int(tmp, sizeof(tmp), tr(STR_N_LANGUAGES), nl); str_lcat(buf, cap, tmp); }
    if (nl > 0 && nm > 0) str_lcat(buf, cap, ", ");
    if (nm > 0) { str_fmt1_int(tmp, sizeof(tmp), tr(STR_N_MODULES), nm); str_lcat(buf, cap, tmp); }
}

static void menu_offer_sd_install(void) {
    if (!installer_prep_fs()) return;
    uint32_t mark = mod_pool_mark();
    installer_api_t api = {0};
    int nl = 0, nm = 0;
    if (mod_load_installer(INSTALLER_PATH, &g_installer_host, &api) && api.scan) {
        nl = api.scan(&g_installer_host, INSTALLER_KIND_LANG);     /* languages */
        nm = api.scan(&g_installer_host, INSTALLER_KIND_MODULE);   /* modules */
    }
    mod_pool_reset(mark);
    if (nl <= 0 && nm <= 0) return;

    /* "Install <N languages>[, <M modules>] from SD?" -- class-aware, so a
     * module-only SD prompts for modules, not languages. */
    static char msg[96];
    char items[64];
    build_install_phrase(items, sizeof(items), nl, nm);
    str_fmt1_str(msg, sizeof(msg), tr(STR_INSTALL_FROM_SD), items);
    ui_show_confirm(msg, menu_do_install);
}

static void menu_main_update(ui_window_t *self) {
    ui_list_update(&g_list_main);
    partition_scan_update();

    /* Load the bundled theme module as soon as the module sources (SD + the
     * LittleFS at its known offset) are registered — the front-loaded probe sets
     * this well before the full sweep finishes, which then continues in the
     * background for the Partition Viewer. One-shot; a saved module-theme then
     * restores via ui_update_theme. */
    static bool themes_loaded = false;
    if (!themes_loaded && partition_modules_ready()) {
        themes_loaded = true;
        BENCH_MARK(3);   /* module sources ready (theme-load trigger) */
        theme_modules_init();
        BENCH_MARK(4);   /* theme fully applied */
        /* Filesystems are now readable: build the language list (English +
         * packs already on LittleFS) and apply the persisted choice, so the menu
         * is immediately usable. Then offer to install any new SD language packs
         * (a transient installer module; confirm, then commit). Until here the menu
         * draws in in-core English. */
        i18n_init();
        menu_apply_language();
        feature_discover();   /* scan /modules/features -> dynamic Tools/Settings entries */
        menu_offer_sd_install();
#ifdef ABI_SELFTEST
        abi_selftest_run();   /* exercise the real module + pack ABI gates once FS is up */
#endif
    }
}

static void menu_main_draw(ui_window_t *self) {
    ui_list_draw(&g_list_main, self->x, self->y, self->w, self->h);
}

/* --- Tools Sub-menu Implementation --- */

/* Module Overview is disabled in-core (MODOVERVIEW_INCORE 0) to hold the 40K stub ceiling: the
 * stub embeds the LZMA-compressed app, so app growth grows the flash image. It is slated to become
 * an XIP feature module (basic UI, fine from slower flash). Flip to 1 to restore the in-core view. */
#define MODOVERVIEW_INCORE 0

extern ui_window_t PAGE_PARTITION;
static void diag_action_partition(void) { ui_push(&PAGE_PARTITION); }
#if MODOVERVIEW_INCORE
extern ui_window_t PAGE_MODOVERVIEW;
static void diag_action_modoverview(void) { ui_push(&PAGE_MODOVERVIEW); }
#endif

static void (*TOOLS_ACTIONS[])(void) = {
    action_browser_enter, diag_action_partition,
#if MODOVERVIEW_INCORE
    diag_action_modoverview,
#endif
};
#define TOOLS_FIXED (2 + MODOVERVIEW_INCORE)   /* File Browser, Partition Viewer (+ Module Overview) */

static const char* get_tools_label(int idx) {
    if (idx == 0) return tr(STR_FILE_BROWSER);
    if (idx == 1) return tr(STR_PARTITION_VIEWER);
#if MODOVERVIEW_INCORE
    if (idx == 2) return "Module Overview";
#endif
    return feature_label(MODULE_MENU_TOOLS, idx - TOOLS_FIXED);   /* feature entries after the fixed tools */
}
static void on_tools_action(int idx) {
    if (idx < TOOLS_FIXED) TOOLS_ACTIONS[idx]();
    else feature_launch(MODULE_MENU_TOOLS, idx - TOOLS_FIXED);
}

static void menu_tools_enter(void) {
    PAGE_TOOLS.title = tr(STR_TITLE_TOOLS);
    ui_list_init(&g_list_tools, tr(STR_TITLE_TOOLS), TOOLS_FIXED, get_tools_label, on_tools_action);
    g_list_tools.visible_lines = 6;
    g_list_tools.on_back = ui_pop;
}

static void menu_tools_update(ui_window_t *self) {
    /* Feature modules register Tools entries after the first paint (in menu_main_update),
     * so recompute the count each frame; the list widget honors a runtime num_items. */
    g_list_tools.num_items = TOOLS_FIXED + feature_count(MODULE_MENU_TOOLS);
    ui_list_update(&g_list_tools);
}

static void menu_tools_draw(ui_window_t *self) {
    ui_list_draw(&g_list_tools, self->x, self->y, self->w, self->h);
}

/* --- Settings Sub-menu Implementation --- */

static void action_fastboot_toggle(void) {
    /* Flip the fast-boot bit, preserving the per-OFW theme slots + signature. */
    uint32_t w = board_rtc_read_settings();
    board_rtc_write_settings(settings_make(!settings_fastboot(w),
                                           settings_mario_slot(w), settings_zelda_slot(w),
                                           settings_lang(w)));
}

static void do_reset_defaults(void) {
    board_rtc_write_settings(0);   /* invalid signature -> all defaults (fast-boot off, themes default) */
    ui_update_theme();             /* reflect the reset live: back to the OFW default theme */
}
static void action_reset_defaults(void) {
    ui_show_confirm(tr(STR_RESET_CONFIRM), do_reset_defaults);
}

/* Language is pinned FIRST so a user who lands in a script they can't read can
 * always find the Language selector at the top of Settings and switch back. */
#define SET_IDX_LANGUAGE 0
#define SET_IDX_THEME    1
#define SET_IDX_FASTBOOT 2
#define SET_STATIC_TOP   3   /* fixed rows before the feature splice (Language/Theme/Fast-Boot) */
/* Feature-module Settings entries splice in at [SET_STATIC_TOP .. SET_STATIC_TOP+N-1];
 * Reset Defaults is ALWAYS the last row (index SET_STATIC_TOP+N), so it stays at the
 * bottom no matter how many modules register. */
static int settings_reset_idx(void) { return SET_STATIC_TOP + feature_count(MODULE_MENU_SETTINGS); }
static int settings_count(void)     { return settings_reset_idx() + 1; }

/* The active language is persisted by CODE (/i18n/.active), committed on Settings
 * exit rather than on every cycle tick — a file write per keypress would
 * re-introduce navigation lag. */
static bool g_lang_dirty = false;

/* One-shot boot notice after the SD install copied/updated packs/modules (the modal
 * appends "press any button"). */
static void menu_notify_installed(int nl, int nm) {
    static char buf[96];
    char items[64];
    build_install_phrase(items, sizeof(items), nl, nm);
    str_fmt1_str(buf, sizeof(buf), tr(STR_UPDATED), items);
    ui_show_notice(buf);
}

/* Refresh the cached window titles after a live language change (the menu-item
 * labels themselves are accessor functions, so they re-translate on their own). */
static void menu_apply_language(void) {
#ifndef RTL_TEST
    gui_rtl = i18n_is_rtl();   /* mirror the UI for RTL languages (RTL_TEST forces it on) */
#endif
    PAGE_MAIN.title     = tr(STR_TITLE_MAIN);
    PAGE_TOOLS.title    = tr(STR_TITLE_TOOLS);
    PAGE_SETTINGS.title = tr(STR_TITLE_SETTINGS);
}

/* Cycle the active language (in sorted display order), load its pack + font live,
 * persist, re-render. Switching live (not on a later A-press) means each language's
 * own script font loads as you scroll, so non-Latin endonyms render immediately. */
static void ui_lang_cycle(int dir) {
    i18n_set(i18n_cycle(i18n_current(), dir));
    g_lang_dirty = true;            /* committed to /i18n/.active on Settings exit */
    menu_apply_language();
}

static const char* get_settings_label(int idx) {
    if (idx == SET_IDX_LANGUAGE) {
        /* "LANGUAGE: < endonym (code) >" — the endonym is in the language's own
         * script; the ASCII "(de_DE)" suffix disambiguates the two English packs
         * and stays readable even when the script's glyphs can't be. */
        static char buf[96];
        char codegrp[24], val[64];
        int cur = i18n_current();
        str_lcpy(codegrp, sizeof(codegrp), "(");   /* one LTR run "(de_DE)" so the code */
        str_lcat(codegrp, sizeof(codegrp), i18n_code(cur));  /* reads L-to-R even in RTL */
        str_lcat(codegrp, sizeof(codegrp), ")");
        const char *vp[3] = { i18n_endonym(cur), " ", codegrp };
        gui_compose(val, sizeof(val), vp, 3);      /* "endonym (code)" / RTL: "(code) endonym" */
        compose_value(tr(STR_LANGUAGE), val, true, buf, sizeof(buf));
        return buf;
    }
    if (idx == SET_IDX_THEME) {
        /* "THEME: < X >" value-selector: A cycles forward, LEFT/RIGHT adjust.
         * Theme/module names are proper nouns, shown literally. */
        static char buf[96];
        compose_value(tr(STR_THEME), ui_theme_slot_name(ui_theme_slot), true, buf, sizeof(buf));
        return buf;
    }
    if (idx == SET_IDX_FASTBOOT) {
        static char buf[96];
        compose_value(tr(STR_FASTBOOT),
                      settings_fastboot(board_rtc_read_settings()) ? tr(STR_ON) : tr(STR_OFF),
                      false, buf, sizeof(buf));
        return buf;
    }
    if (idx >= SET_STATIC_TOP && idx < settings_reset_idx())
        return feature_label(MODULE_MENU_SETTINGS, idx - SET_STATIC_TOP);   /* spliced entries */
    return tr(STR_RESET_DEFAULTS);   /* always the last row */
}
static void on_settings_action(int idx) {
    if (idx == SET_IDX_THEME)              ui_theme_cycle(+1);
    else if (idx == SET_IDX_LANGUAGE)      ui_lang_cycle(+1);
    else if (idx == SET_IDX_FASTBOOT)      action_fastboot_toggle();
    else if (idx == settings_reset_idx())  action_reset_defaults();
    else feature_launch(MODULE_MENU_SETTINGS, idx - SET_STATIC_TOP);
}
static void on_settings_adjust(int idx, int dir) {
    if (idx == SET_IDX_THEME)         ui_theme_cycle(dir);
    else if (idx == SET_IDX_LANGUAGE) ui_lang_cycle(dir);
}

static void action_settings_enter(void) { ui_push(&PAGE_SETTINGS); }

/* Leaving Settings commits a pending language change to /i18n/.active (debounced
 * so rapid cycling doesn't write a file per tick). */
static void settings_back(void) {
    if (g_lang_dirty) { i18n_persist_active(); g_lang_dirty = false; }
    ui_pop();
}

static void menu_settings_enter(void) {
    PAGE_SETTINGS.title = tr(STR_TITLE_SETTINGS);
    ui_list_init(&g_list_settings, tr(STR_TITLE_SETTINGS), settings_count(), get_settings_label, on_settings_action);
    g_list_settings.visible_lines = 6;
    g_list_settings.on_back = settings_back;
    g_list_settings.on_adjust = on_settings_adjust;
}

static void menu_settings_update(ui_window_t *self) {
    /* Feature modules register Settings entries after the first paint, so recompute the
     * count each frame (same as Tools); Reset Defaults rides at the bottom either way. */
    g_list_settings.num_items = settings_count();
    ui_list_update(&g_list_settings);
}

static void menu_settings_draw(ui_window_t *self) {
    ui_list_draw(&g_list_settings, self->x, self->y, self->w, self->h);
}

void menu_run(void) {
    BENCH_MARK(1);   /* entering menu loop (after OSPI/LCD/asset init) */
    ui_init();
    menu_main_enter();
    menu_tools_enter();    // Pre-initialize lists
    menu_settings_enter();
    ui_push(&PAGE_MAIN);
    crash_log_init();                       /* enable precise fault capture (Mem/Bus/Usage) */
    if (crash_log_pending()) {              /* a recorded crash survived to boot -> surface it once */
        static char crashbuf[48];
        crash_log_summary(crashbuf, sizeof(crashbuf));
        ui_show_error(crashbuf);
        crash_log_ack();
    }
#ifdef CRASH_TEST
    crash_test_init();   /* clear the deliberate-fault cell before the loop polls it */
#endif
#ifdef BOOT_BENCH
    bool first_frame = true;
#endif
    while (1) {
        ui_update();
        ui_draw();
#ifdef CRASH_TEST
        crash_test_check();   /* deliberate fault on host request (build-gated) */
#endif
#ifdef BOOT_BENCH
        if (first_frame) { first_frame = false; BENCH_MARK(2); }  /* first menu frame drawn */
#endif
    }
}

void menu_enter_standby(void) {
    gui_deinit();
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_LOW);
    HAL_PWREx_ClearWakeupFlag(PWR_FLAG_WKUP1);
    HAL_PWR_EnterSTANDBYMode();
    while (1) HAL_NVIC_SystemReset();
}
