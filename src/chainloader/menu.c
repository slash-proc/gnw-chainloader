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
#ifdef CRASH_TEST
#include "system/crash_log.h"
#endif
#include "../common/memory_map.h"
#include "../common/boot_magic.h"
#include "storage/vfs.h"
#include "system/loader.h"
#include "system/installer.h"
#include "system/module.h"

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
static void action_browser_enter(void) { ui_push(&PAGE_BROWSER); }
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
        case BT_RETROGO: return board_is_valid_app(RETROGO_BASE);
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
        case BT_RETROGO: return "RETRO-GO";
        case BT_MARIO:   return "MARIO";
        case BT_ZELDA:   return "ZELDA";
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

static const char* get_main_label(int idx) {
    switch (idx) {
        case MM_BOOT: {
            /* "LAUNCH: < target >" value-selector, matching THEME/LANGUAGE: the
             * translated label both names the action and buffers the menu cursor
             * (">") from the value bracket; LEFT/RIGHT cycle the target and A
             * launches it. Greyed + label-only when nothing is bootable. */
            if (!any_target_bootable()) return tr(STR_LAUNCH);
            static char buf[48];
            strcpy(buf, tr(STR_LAUNCH));
            strcat(buf, ": < ");
            strcat(buf, target_name(g_boot_target));
            strcat(buf, " >");
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

/* Boot the selected target. Retro-Go jumps straight to Bank 1. For an OFW, flash
 * its backup into Bank 2 first if it isn't already the valid active OFW (a
 * deliberate A-press, so a progress bar only, no confirm prompt), then jump. */
static void boot_selected_target(void) {
    if (g_boot_target == BT_RETROGO) { action_retro_go(); return; }
    board_console_type_t want = (g_boot_target == BT_ZELDA) ? CONSOLE_ZELDA : CONSOLE_MARIO;
    bool active_valid = (want == board_console_type) && board_is_valid_app(OFW_INTERNAL_BASE);
    if (!active_valid && ofw_backup_present(want)) {
        if (want == CONSOLE_ZELDA) partition_flash_ofw("Zelda OFW", ZELDA_SPI_OFFSET, OFW_INTERNAL_SIZE);
        else                        partition_flash_ofw("Mario OFW", MARIO_SPI_OFFSET, OFW_INTERNAL_SIZE);
    }
    action_ofw();
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
    .sd_read_header  = vfs_sd_read_header,
    .lfs_read_header = vfs_lfs_read_header,
    .copy_sd_to_lfs  = vfs_copy_sd_to_lfs,
    .lfs_has         = vfs_lfs_has,
    .sd_dir_exists   = vfs_sd_dir_exists,
    .sd_list_langs   = vfs_sd_list_langs,
    .progress        = NULL,
    .strings_abi     = STRINGS_ABI_VERSION,
    .str_count       = (uint16_t)STR_COUNT,
    .module_abi      = MODULE_ABI_VERSION,
};

#define INSTALLER_PATH "/modules/installer.bin"

/* True if an SD /i18n folder is present (the only case worth loading the installer).
 * Loads the LFN FAT module RESIDENT first -- so it sits below the installer's pool
 * mark and survives the transient reclaim; freeing a driver the vfs still points at
 * would be a use-after-free. */
static bool installer_prep_fs(void) {
    if (!vfs_sd_dir_exists("/i18n")) return false;
    if (!vfs_is_fat_rw_loaded() && vfs_module_available("/modules/filesystems/fatfs.bin"))
        vfs_load_dynamic_driver("FAT", "/modules/filesystems/fatfs.bin");
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
    if (!vfs_is_lfs_rw_loaded() && vfs_module_available("/modules/filesystems/lfs_rw.bin"))
        vfs_load_dynamic_driver("LFS", "/modules/filesystems/lfs_rw.bin");
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
/* Append "N word" to buf (manual int-to-str; the install prompt + notice are
 * English, not translated -- a translated prompt would need new strings + a
 * STRINGS_ABI bump). */
static void append_count(char *buf, int n, const char *word) {
    char tmp[8], num[8];
    int v = n, t = 0, i = 0;
    while (v > 0 && t < 7) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) num[i++] = tmp[--t];
    num[i] = '\0';
    strcat(buf, num); strcat(buf, " "); strcat(buf, word);
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

    /* "Install [N language(s)][, ][M module(s)] from SD?" -- class-aware, so a
     * module-only SD prompts for modules, not languages. */
    static char msg[56];
    strcpy(msg, "Install ");
    if (nl > 0) append_count(msg, nl, nl == 1 ? "language" : "languages");
    if (nl > 0 && nm > 0) strcat(msg, ", ");
    if (nm > 0) append_count(msg, nm, nm == 1 ? "module" : "modules");
    strcat(msg, " from SD?");
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

extern ui_window_t PAGE_PARTITION;

static void diag_action_partition(void) { ui_push(&PAGE_PARTITION); }

static void (*TOOLS_ACTIONS[])(void) = {
    action_browser_enter, diag_action_partition
};

static const char* get_tools_label(int idx) {
    return tr(idx == 0 ? STR_FILE_BROWSER : STR_PARTITION_VIEWER);
}
static void on_tools_action(int idx) { TOOLS_ACTIONS[idx](); }

static void menu_tools_enter(void) {
    PAGE_TOOLS.title = tr(STR_TITLE_TOOLS);
    ui_list_init(&g_list_tools, tr(STR_TITLE_TOOLS), 2, get_tools_label, on_tools_action);
    g_list_tools.visible_lines = 6;
    g_list_tools.on_back = ui_pop;
}

static void menu_tools_update(ui_window_t *self) {
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

#define SET_IDX_THEME    0
#define SET_IDX_LANGUAGE 1
#define SET_IDX_FASTBOOT 2
#define SET_IDX_RESET    3
#define SET_COUNT        4

/* The active language is persisted by CODE (/i18n/.active), committed on Settings
 * exit rather than on every cycle tick — a file write per keypress would
 * re-introduce navigation lag. */
static bool g_lang_dirty = false;

/* One-shot boot notice after the SD install copied/updated N languages (English;
 * the modal appends "press any button"). */
static void menu_notify_installed(int nl, int nm) {
    static char buf[48];
    strcpy(buf, "Updated ");
    if (nl > 0) append_count(buf, nl, nl == 1 ? "language" : "languages");
    if (nl > 0 && nm > 0) strcat(buf, ", ");
    if (nm > 0) append_count(buf, nm, nm == 1 ? "module" : "modules");
    ui_show_notice(buf);
}

/* Refresh the cached window titles after a live language change (the menu-item
 * labels themselves are accessor functions, so they re-translate on their own). */
static void menu_apply_language(void) {
    PAGE_MAIN.title     = tr(STR_TITLE_MAIN);
    PAGE_TOOLS.title    = tr(STR_TITLE_TOOLS);
    PAGE_SETTINGS.title = tr(STR_TITLE_SETTINGS);
}

/* Cycle the active language (in sorted display order), load its pack + font live,
 * persist, re-render. */
static void ui_lang_cycle(int dir) {
    i18n_set(i18n_cycle(i18n_current(), dir));
    g_lang_dirty = true;            /* committed to /i18n/.active on Settings exit */
    menu_apply_language();
}

static const char* get_settings_label(int idx) {
    if (idx == SET_IDX_THEME) {
        /* "THEME: < X >" value-selector: A cycles forward, LEFT/RIGHT adjust.
         * Theme/module names are proper nouns, shown literally. */
        static char buf[48];
        strcpy(buf, tr(STR_THEME));
        strcat(buf, ": < ");
        strcat(buf, ui_theme_slot_name(ui_theme_slot));
        strcat(buf, " >");
        return buf;
    }
    if (idx == SET_IDX_LANGUAGE) {
        /* "LANGUAGE: < endonym >" — endonym is in the language's own script. */
        static char buf[48];
        strcpy(buf, tr(STR_LANGUAGE));
        strcat(buf, ": < ");
        strcat(buf, i18n_endonym(i18n_current()));
        strcat(buf, " >");
        return buf;
    }
    if (idx == SET_IDX_FASTBOOT) {
        static char buf[40];
        strcpy(buf, tr(STR_FASTBOOT));
        strcat(buf, ": ");
        strcat(buf, settings_fastboot(board_rtc_read_settings()) ? tr(STR_ON) : tr(STR_OFF));
        return buf;
    }
    return tr(STR_RESET_DEFAULTS);
}
static void on_settings_action(int idx) {
    if (idx == SET_IDX_THEME)         ui_theme_cycle(+1);
    else if (idx == SET_IDX_LANGUAGE) ui_lang_cycle(+1);
    else if (idx == SET_IDX_FASTBOOT) action_fastboot_toggle();
    else if (idx == SET_IDX_RESET)    action_reset_defaults();
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
    ui_list_init(&g_list_settings, tr(STR_TITLE_SETTINGS), SET_COUNT, get_settings_label, on_settings_action);
    g_list_settings.visible_lines = 6;
    g_list_settings.on_back = settings_back;
    g_list_settings.on_adjust = on_settings_adjust;
}

static void menu_settings_update(ui_window_t *self) {
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
