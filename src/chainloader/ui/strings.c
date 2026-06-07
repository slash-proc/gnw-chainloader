#include "strings.h"
#include <stddef.h>

/*
 * In-core English string table — the always-present base layer and per-string
 * fallback. UTF-8 (ASCII here; packs may carry accented/CJK text). Indexed by
 * string_id_t; keep this array in exact enum order.
 */
static const char *const i18n_en[STR_COUNT] = {
    [STR_TITLE_MAIN]            = "GNW CHAINLOADER",
    [STR_TITLE_TOOLS]           = "TOOLS",
    [STR_TITLE_SETTINGS]        = "SETTINGS",
    [STR_LAUNCH]                = "LAUNCH",
    [STR_POWER_OFF]             = "POWER OFF",
    [STR_FILE_BROWSER]          = "FILE BROWSER",
    [STR_PARTITION_VIEWER]      = "PARTITION VIEWER",

    [STR_THEME]                 = "THEME",
    [STR_FASTBOOT]              = "FAST-BOOT",
    [STR_ON]                    = "ON",
    [STR_OFF]                   = "OFF",
    [STR_LANGUAGE]              = "LANGUAGE",
    [STR_RESET_DEFAULTS]        = "RESET DEFAULTS",
    [STR_RESET_CONFIRM]         = "RESET ALL SETTINGS?",

    [STR_CONFIRM]               = "CONFIRM",
    [STR_ERROR]                 = "ERROR",
    [STR_OPTIONS]               = "OPTIONS",
    [STR_YES_NO]                = "A: YES   B: NO",
    [STR_PRESS_ANY]             = "PRESS ANY BUTTON",
    [STR_FOOTER_CANCEL]         = "B: CANCEL",
    [STR_FOOTER_BROWSER]        = "PAUSE: OPTS   A: SEL",

    [STR_LBL_TARGET]            = "TARGET.",
    [STR_LBL_SIZE]              = "SIZE.",
    [STR_LBL_ADDR]              = "ADDR.",
    [STR_LBL_DETAILS]           = "DETAILS.",
    [STR_LBL_TYPE]              = "TYPE.",
    [STR_LBL_FREE]              = "FREE.",
    [STR_LBL_USED]              = "USED.",
    [STR_LBL_TOTAL]             = "TOTAL.",
    [STR_LBL_MODE]              = "MODE.",

    [STR_SCANNING]              = "> SCANNING MEMORY...",
    [STR_NO_FILESYSTEMS]        = "NO FILESYSTEMS FOUND",
    [STR_EMPTY_DIR]             = "EMPTY DIRECTORY",
    [STR_BATT]                  = "BATT: ",
    [STR_CHARGING]              = " [CHG]",

    [STR_ERASE]                 = "ERASE",
    [STR_CANCEL]                = "CANCEL",
    [STR_COPY]                  = "COPY",
    [STR_PASTE]                 = "PASTE",
    [STR_DELETE]                = "DELETE",

    [STR_ERASE_PART_CONFIRM]    = "ERASE PARTITION?",
    [STR_DELETE_FILE_CONFIRM]   = "DELETE FILE?",
    [STR_DELETE_DIR_CONFIRM]    = "DELETE FOLDER + CONTENTS?",

    [STR_ERR_DELETE_UNSUPPORTED]= "DELETE UNSUPPORTED",
    [STR_ERR_DELETE_FAILED]     = "DELETE FAILED",
    [STR_ERR_DRIVER_NA]         = "DRIVER NOT AVAILABLE",
    [STR_ERR_READ_ONLY]         = "TARGET IS READ-ONLY",
    [STR_ERR_COPY_SELF]         = "CANNOT COPY INTO ITSELF",
    [STR_ERR_MOUNT_SRC]         = "FAILED TO MOUNT SRC",
    [STR_ERR_SRC_READ]          = "SOURCE READ ERROR",
    [STR_ERR_NO_SPACE]          = "NOT ENOUGH SPACE",

    [STR_ERASING_EXT]           = "ERASING EXT FLASH...",
    [STR_ERASING_INT]           = "ERASING INT FLASH...",
    [STR_PLEASE_WAIT]           = "PLEASE WAIT...",
    [STR_ERASE_COMPLETE]        = "ERASE COMPLETE",
    [STR_DONE]                  = "DONE",
    [STR_FLASHING]              = "FLASHING...",
    [STR_FLASH_FAILED]          = "FLASH FAILED!",
    [STR_WRITE_ERROR]           = "WRITE ERROR",
    [STR_FLASH_OK]              = "FLASH SUCCESSFUL",
    [STR_PREPARING]             = "PREPARING ",
    [STR_COPYING]               = "COPYING FILE...",
    [STR_CALCULATING]           = "CALCULATING...",
    [STR_DELETING]              = "DELETING...",

    [STR_FREE_SPACE]            = "FREE SPACE",
    [STR_MOUNT_FAIL]            = "MOUNT FAIL: ",
    [STR_ERR_OPEN]              = "OPEN FAILED",
    [STR_ERR_READ]              = "READ ERROR",
    [STR_ERR_DISK_FULL]         = "DISK FULL",
    [STR_ERR_TREE_DEEP]         = "TREE TOO DEEP",
    [STR_ERR_PATH_LONG]         = "PATH TOO LONG",
    [STR_WRITING]               = "WRITING: ",

    [STR_SELECT_FS]             = "SELECT FS",
    [STR_DIR]                   = "DIR",
    [STR_FILE]                  = "FILE",
    [STR_UNKNOWN]               = "UNKNOWN",
    [STR_FS_LITTLEFS]           = "LITTLEFS",
    [STR_MODE_RW]               = "RW",
    [STR_MODE_RO]               = "RO",
    [STR_FILE_N]                = "FILE %d",

    [STR_DIV_INTFLASH]          = "INTFLASH",
    [STR_DIV_EXTFLASH]          = "EXTFLASH",
    [STR_DIV_SDCARD]            = "SD CARD",

    [STR_THEME_DEFAULT]         = "DEFAULT",
    [STR_THEME_FALLBACK]        = "FALLBACK",

    [STR_NOTICE_LANGUAGES]      = "LANGUAGES",
    [STR_N_LANGUAGES]           = "%d languages",
    [STR_N_MODULES]             = "%d modules",
    [STR_INSTALL_FROM_SD]       = "Install %s from SD?",
    [STR_UPDATED]               = "Updated %s",

    [STR_OFW_SUFFIX]            = "%s OFW",

    [STR_DECIMAL_SEP]           = ".",
};

/* Active language pack table (STR_COUNT entries) or NULL for English-only.
 * A NULL entry within an installed table falls back to English per-string. */
static const char *const *g_active = NULL;

void strings_set_active(const char *const *table) {
    g_active = table;
}

const char *tr(string_id_t id) {
    if ((unsigned)id >= STR_COUNT) return "";
    if (g_active && g_active[id]) return g_active[id];
    return i18n_en[id];
}
