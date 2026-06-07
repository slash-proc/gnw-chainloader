#ifndef UI_STRINGS_H
#define UI_STRINGS_H

/*
 * UI string table — the translatable text of the chainloader.
 *
 * Every user-facing label is referenced by a string_id_t and resolved at draw
 * time through tr(). The in-core table (strings.c) is English; a loaded language
 * pack (Phase 3) overrides it per-string, with English as the per-string
 * fallback. Proper nouns (MARIO, ZELDA), filesystem identifiers (FAT, LittleFS),
 * and button letters (A/B) stay literal in the code and are not part of this set.
 *
 * ABI CONTRACT: string_id_t order + STR_COUNT are the wire format shared with
 * .lang packs. Packs are emitted against this exact order. APPEND new IDs at the
 * end (before STR_COUNT) and bump STRINGS_ABI_VERSION on any reorder/removal;
 * lang_load() rejects a pack whose ABI version disagrees.
 */

#define STRINGS_ABI_VERSION 3u

typedef enum {
    /* --- main menu / navigation --- */
    STR_TITLE_MAIN,          /* "GNW CHAINLOADER" (header + main list) */
    STR_TITLE_TOOLS,         /* "TOOLS"  (page title + main-menu item) */
    STR_TITLE_SETTINGS,      /* "SETTINGS" (page title + main-menu item) */
    STR_LAUNCH,              /* "LAUNCH": boot-selector label, "LAUNCH: < target >"
                              * (matches THEME/LANGUAGE). Translated context-aware
                              * per language (de Starten, fr Lancer, ja kido, ...). */
    STR_POWER_OFF,           /* "POWER OFF" */
    STR_FILE_BROWSER,        /* "FILE BROWSER" */
    STR_PARTITION_VIEWER,    /* "PARTITION VIEWER" */

    /* --- settings --- */
    STR_THEME,               /* "THEME" (prefix of "THEME: < X >") */
    STR_FASTBOOT,            /* "FAST-BOOT" (prefix of "FAST-BOOT: ON/OFF") */
    STR_ON,                  /* "ON" */
    STR_OFF,                 /* "OFF" */
    STR_LANGUAGE,            /* "LANGUAGE" (prefix of "LANGUAGE: < X >") */
    STR_RESET_DEFAULTS,      /* "RESET DEFAULTS" */
    STR_RESET_CONFIRM,       /* "RESET ALL SETTINGS?" */

    /* --- modal window titles / prompts --- */
    STR_CONFIRM,             /* "CONFIRM" */
    STR_ERROR,               /* "ERROR" */
    STR_OPTIONS,             /* "OPTIONS" (context-menu title) */
    STR_YES_NO,              /* "A: YES   B: NO" */
    STR_PRESS_ANY,           /* "PRESS ANY BUTTON" */
    STR_FOOTER_CANCEL,       /* "B: CANCEL" */
    STR_FOOTER_BROWSER,      /* "PAUSE: OPTS   A: SEL" */

    /* --- right-pane / detail labels (trailing '.' is decorative) --- */
    STR_LBL_TARGET,          /* "TARGET." */
    STR_LBL_SIZE,            /* "SIZE." */
    STR_LBL_ADDR,            /* "ADDR." */
    STR_LBL_DETAILS,         /* "DETAILS." */
    STR_LBL_TYPE,            /* "TYPE." */
    STR_LBL_FREE,            /* "FREE." */
    STR_LBL_USED,            /* "USED." */
    STR_LBL_TOTAL,           /* "TOTAL." */
    STR_LBL_MODE,            /* "MODE." */

    /* --- status / empty states --- */
    STR_SCANNING,            /* "> SCANNING MEMORY..." */
    STR_NO_FILESYSTEMS,      /* "NO FILESYSTEMS FOUND" */
    STR_EMPTY_DIR,           /* "EMPTY DIRECTORY" */
    STR_BATT,                /* "BATT: " (prefix; percent + tags appended) */
    STR_CHARGING,            /* " [CHG]" charging tag */

    /* --- context-menu actions --- */
    STR_ERASE,               /* "ERASE" */
    STR_CANCEL,              /* "CANCEL" */
    STR_COPY,                /* "COPY" */
    STR_PASTE,               /* "PASTE" */
    STR_DELETE,              /* "DELETE" */

    /* --- confirmations --- */
    STR_ERASE_PART_CONFIRM,  /* "ERASE PARTITION?" */
    STR_DELETE_FILE_CONFIRM, /* "DELETE FILE?" */
    STR_DELETE_DIR_CONFIRM,  /* "DELETE FOLDER + CONTENTS?" */

    /* --- errors --- */
    STR_ERR_DELETE_UNSUPPORTED, /* "DELETE UNSUPPORTED" */
    STR_ERR_DELETE_FAILED,      /* "DELETE FAILED" */
    STR_ERR_DRIVER_NA,          /* "DRIVER NOT AVAILABLE" */
    STR_ERR_READ_ONLY,          /* "TARGET IS READ-ONLY" */
    STR_ERR_COPY_SELF,          /* "CANNOT COPY INTO ITSELF" */
    STR_ERR_MOUNT_SRC,          /* "FAILED TO MOUNT SRC" */
    STR_ERR_SRC_READ,           /* "SOURCE READ ERROR" */
    STR_ERR_NO_SPACE,           /* "NOT ENOUGH SPACE" */

    /* --- progress (flash / erase / copy) --- */
    STR_ERASING_EXT,         /* "ERASING EXT FLASH..." */
    STR_ERASING_INT,         /* "ERASING INT FLASH..." */
    STR_PLEASE_WAIT,         /* "PLEASE WAIT..." */
    STR_ERASE_COMPLETE,      /* "ERASE COMPLETE" */
    STR_DONE,                /* "DONE" */
    STR_FLASHING,            /* "FLASHING..." */
    STR_FLASH_FAILED,        /* "FLASH FAILED!" */
    STR_WRITE_ERROR,         /* "WRITE ERROR" */
    STR_FLASH_OK,            /* "FLASH SUCCESSFUL" */
    STR_PREPARING,           /* "PREPARING " (prefix; name + "..." appended) */
    STR_COPYING,             /* "COPYING FILE..." */
    STR_CALCULATING,         /* "CALCULATING..." */
    STR_DELETING,            /* "DELETING..." */

    /* --- file-browser values / copy errors --- */
    STR_FREE_SPACE,          /* "FREE SPACE" (partition target name) */
    STR_MOUNT_FAIL,          /* "MOUNT FAIL: " (prefix; error code appended) */
    STR_ERR_OPEN,            /* "OPEN FAILED" */
    STR_ERR_READ,            /* "READ ERROR" */
    STR_ERR_DISK_FULL,       /* "DISK FULL" */
    STR_ERR_TREE_DEEP,       /* "TREE TOO DEEP" */
    STR_ERR_PATH_LONG,       /* "PATH TOO LONG" */
    STR_WRITING,             /* "WRITING: " (prefix; "<n>KB..." appended) */

    STR_COUNT
} string_id_t;

/* Resolve a string ID to UTF-8 text: active language pack (if loaded) overrides
 * the in-core English table per-string; English is the always-present fallback. */
const char *tr(string_id_t id);

/* Phase 3 hook: install/clear the active language's string table (STR_COUNT
 * entries; a NULL entry means "fall back to English for this id"). NULL clears
 * back to English. */
void strings_set_active(const char *const *table);

#endif /* UI_STRINGS_H */
