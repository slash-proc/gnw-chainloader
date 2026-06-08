#ifndef SYSTEM_FEATURE_H
#define SYSTEM_FEATURE_H

#include <stdint.h>
#include <stdbool.h>
#include "system/module.h"   /* MODULE_MENU_* */
#include "system/mod_ui.h"   /* mod_ui_t */
#include "system/gui_api.h"  /* gui_api_t: theme-aware draw toolkit */

/*
 * Feature modules: PIE modules that add their own Tools/Settings entries (and/or
 * handle a file type) WITHOUT the core knowing them by name. Each declares its menu
 * placement + label + handled extension in its header manifest (system/module.h); the
 * core peeks that at boot (feature_discover), lists the entry, and transiently loads +
 * runs the module on selection. See docs/module-menu-registration.md.
 */

/* --- unified UI view descriptor (the in-core UI service seam) --- */
/* A module (or the core) describes a screen with this descriptor and hands it to the in-core
 * service via host->run_view(). The service renders it through the SAME ui_list + chrome the
 * core's File Browser / Partition Viewer use, so the module gets the divider, themed selector,
 * smooth scroll + wrap, and consistent header/footer for free -- without hand-rolling a draw
 * loop or growing the gui_api per operation. The running app owns the header title; the service
 * draws it. The callbacks are MODULE functions, called directly by the core: r9 (the module's
 * GOT base) is callee-saved across run() -> service -> callback, so no trampoline is needed
 * (see loader.c mod_invoke_r9 / call_feat_r9). */
#define UI_VIEW_MENU     0   /* list only (a plain menu) */
#define UI_VIEW_DUALPANE 1   /* left list + right detail pane (File Browser / Partition Viewer style) */
typedef struct {
    uint8_t kind;          /* UI_VIEW_MENU or UI_VIEW_DUALPANE */
    const char *title;     /* header title (the running app owns it; the service draws it) */
    const char *footer;    /* footer legend, NULL = none */
    uint16_t header_color; /* 0 = theme header color */
    int count;             /* row count */
    const char *(*get_label)(int i);                 /* row text */
    void (*on_select)(int i);                         /* A on row i; NULL = back-only view */
    void (*draw_right_pane)(int sel, uint32_t tick);  /* dual-pane detail; NULL for a plain menu */
} ui_view_desc_t;

/* One module's registry entry, surfaced to the Module Overview feature module (a module cannot read
 * the core's g_mod_recs directly -- it is PIC and never links core globals). */
typedef struct {
    char     name[44];   /* basename of the load path */
    uint32_t flash;      /* XIP mapped-flash address; 0 = full RAM copy */
    uint32_t ram;        /* pool bytes */
    uint32_t flags;      /* header flags (bit0 = transient) */
    int      resident;   /* still in the pool (not yet reclaimed) */
} mod_view_t;

/* Host services a transient feature module receives so it can run its own screen and
 * read input without linking the core UI. (VFS streaming + a file picker arrive with
 * the MP3 phase; kept minimal here.) */
typedef struct {
    uint32_t (*get_tick)(void);
    uint16_t *(*framebuffer)(void);                 /* current back buffer; re-fetch each present */
    void (*draw_text)(int x, int y, const char *s, uint16_t color);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*progress_bar)(int x, int y, int w, int h, int pct, uint16_t border, uint16_t fill);
    void (*present)(void);                           /* flush + swap the framebuffer */
    void (*input_update)(void);
    int  (*just_pressed)(int button);                /* INPUT_* values (system/input.h) */
    int  (*is_pressed)(int button);
    const mod_ui_t *ui;                              /* confirm/error dialogs (mod_ui) */

    /* Theme-aware drawing so a module's UI matches the active theme by default: the full
     * gui_api toolkit (theme colors color_bg/fg/accent/border/footer, aligned + marquee
     * text, selector, blend_rect, progress bar, modals) plus the standard chrome. Draw the
     * themed header (title + battery) and footer (centered hint) each frame; content lives
     * between them (y 23..217, full 320 wide). */
    const gui_api_t *gui;
    void (*draw_header)(const char *title);
    void (*draw_footer)(const char *hint);

    /* Pull-based streaming of the launched file, backed by the core VFS on the partition
     * the file was launched from (so multi-MB media never loads whole into the pool). For a
     * menu launch (path == NULL) file_open returns NULL. One file open at a time. */
    void *(*file_open)(const char *path);            /* opaque handle, or NULL */
    int   (*file_read)(void *f, void *buf, uint32_t n); /* bytes read; 0 = EOF; <0 = error */
    int   (*file_seek)(void *f, uint32_t off);       /* seek to a byte offset; <0 if unsupported */
    void  (*file_close)(void *f);
    /* Enumerate a directory on the launched file's partition: cb(name, is_dir, size, user)
     * per entry (size in bytes, for e.g. a duration estimate). For building a play queue.
     * Best-effort; no-op for a menu launch. Don't call with a file open. */
    void  (*list_dir)(const char *dirpath,
                      void (*cb)(const char *name, int is_dir, uint32_t size, void *user),
                      void *user);
    /* Run the in-core minimal file browser as a modal picker (filtered to extension `ext`,
     * "" = all) and block until the user picks a file (A) or a whole folder (PAUSE), or
     * backs out. On a pick, the absolute path is copied into out[cap], *out_size gets the
     * file's byte size (0 for a folder), *out_is_dir flags a folder pick, and true is
     * returned. The caller should stop its own audio/IO around this (the browser may
     * re-mount filesystems). out_size / out_is_dir may be NULL. */
    bool  (*pick_file)(const char *ext, char *out, int cap, uint32_t *out_size, bool *out_is_dir);

    /* Persist a small per-module state blob to internal LFS, keyed by the running module's
     * name (/modules/state/<module>). Survives power-off. state_save returns true on
     * success; state_load returns the byte count read, or <0 if absent / no writable LFS.
     * Both degrade gracefully when LFS RW is unavailable (save no-ops, load returns <0), so
     * a module simply runs without persistence. */
    bool  (*state_save)(const void *blob, uint32_t len);
    int   (*state_load)(void *blob, uint32_t cap);
    /* Opaque token for the current file-source partition (0 = none) and a way to re-select
     * it by token. Persist source_id() with saved state, then set_source() it on restore so
     * stored paths resolve to the right filesystem after a reboot. */
    uint32_t (*source_id)(void);
    void     (*set_source)(uint32_t id);
    /* True only on the first feature launch since power-on (boot). Lets a module avoid
     * auto-resuming on a cold boot while still resuming on a same-session re-entry. */
    int      (*is_first_launch)(void);
    /* Point the file source at the SD card if one is present, so a Tools-menu launch (no
     * launched file) can default to an on-card directory. true if an SD partition was found. */
    bool     (*set_source_sd)(void);
    /* Borrow a CPU-only scratch buffer from idle D2 SRAM, so a big working buffer doesn't bloat the
     * module's .bss (which counts against the AXI module pool). Returns the region base for
     * size <= 64 KB, else NULL (caller keeps its own buffer). Exclusive to the running module; no
     * free needed. NOT DMA-reachable (D2 is off the AXI bus) — CPU access only. */
    void    *(*scratch_get)(uint32_t size);
    /* Loader module registry, for the Module Overview diagnostic. mod_count entries; mod_get fills
     * `out` for index i and returns 1 (0 if i is out of range). APPEND-ONLY. */
    int  (*mod_count)(void);
    int  (*mod_get)(int i, mod_view_t *out);

    /* Unified UI service seam (APPEND-ONLY). run_view renders the descriptor through the
     * in-core ui_list + chrome and runs its own input loop until B/back, calling the desc
     * callbacks each frame (get_label/on_select/draw_right_pane). pane_row draws one
     * right-pane label/value row (wraps ui_list_pane_row) for a dual-pane detail pane. */
    void (*run_view)(const ui_view_desc_t *desc);
    void (*pane_row)(int slot, const char *label, const char *value, bool marquee, uint32_t tick);
    void (*register_bg_tick)(void (*tick)(void));
    void (*set_io_busy)(int busy);
    int  (*is_io_busy)(void);
} feature_host_t;

typedef struct {
    /* Run the feature. `path` is a file the user selected (file-type launch) or NULL
     * (menu launch). Blocks until the user backs out. */
    void (*run)(const feature_host_t *host, const char *path);
} feature_api_t;

/* loader.c: transiently load a feature module + call its init_module(host, out). The
 * CALLER brackets with mod_pool_mark/reset. */
bool mod_load_feature(const char *path, const feature_host_t *host, feature_api_t *out);

/* --- core registry (feature.c) --- */
/* Scan /modules/features, peek each header, register those declaring a menu id/ext.
 * Run once after the filesystems are up. */
void feature_discover(void);
bool feature_is_bg_active(void);
void feature_bg_tick(void);
/* Count of registered entries for a menu (MODULE_MENU_TOOLS / _SETTINGS). */
int  feature_count(int menu_id);
/* Label of the nth entry within a menu (in registration order). "" if out of range. */
const char *feature_label(int menu_id, int n);
/* Transiently load + run the nth entry of `menu_id` (path = NULL). */
void feature_launch(int menu_id, int n);
/* Module path of the feature registered for a (lowercase) file extension, or NULL. */
const char *feature_path_for_ext(const char *ext);
/* Transiently load + run the feature at `modpath`, handing it `file`. `src_partition` (a
 * const partition_info_t*, opaque here) is where `file` lives so the host file API can
 * stream it; pass NULL for a menu launch. */
void feature_launch_path(const char *modpath, const char *file, const void *src_partition);

#endif /* SYSTEM_FEATURE_H */
