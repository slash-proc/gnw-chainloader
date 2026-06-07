#include "system/feature.h"
#include "system/module.h"
#include "system/loader.h"
#include "system/mod_ui.h"
#include "system/gui_api.h"
#include "storage/vfs.h"
#include "storage/partition.h"
#include "../../common/memory_map.h"   /* D2_SCRATCH_BASE/_SIZE */
#include "ui/ui.h"           /* ui_draw_header */
#include "ui/ui_list.h"      /* ui_list + ui_list_pane_row: the in-core UI service the seam reuses */
#include "ui/i18n.h"         /* i18n_current / i18n_code -> localize the menu entry */
#include "input.h"
#include "gui.h"
#include "utils.h"
#include <string.h>
#include "stm32h7xx.h"   /* HAL_GetTick */

#define FEATURE_DIR "/modules/features"

/* A discovered feature: its menu placement + label + handled extension + the module
 * path to load on demand. Small fixed table -- mirrors theme_register's pattern. */
#define MAX_FEATURES 8
typedef struct {
    uint8_t menu_id;
    char    label[24];
    char    ext[24];                               /* comma-separated handled extensions */
    char    path[64];
    char    menu_label_xlat[MODULE_MENU_XLAT_MAX]; /* packed per-lang title, resolved live */
} feature_entry_t;

static feature_entry_t g_feat[MAX_FEATURES];
static int g_feat_count = 0;

/* SWD-observable feature-discovery trace: exactly where each /modules/features file is dropped, so
 * a host can read why feature_count ends up 0 (no OCR needed). Read by symbol g_feat_dbg. */
struct {
    uint32_t discover_calls;  /* times feature_discover ran (0 = never called) */
    uint32_t enumerated;      /* files the LFS enum handed to the callback (0 = dir empty/missing) */
    uint32_t hdr_read_fail;   /* vfs_lfs_read_header failed */
    uint32_t magic_bad;       /* magic != MODULE_MAGIC */
    uint32_t abi_bad;         /* abi != MODULE_ABI_VERSION */
    uint32_t not_feature;     /* no menu_id and no file_ext */
    uint32_t registered;      /* added to the menu */
    uint32_t last_magic;      /* last file's header magic */
    uint32_t last_abi;        /* last file's header abi */
    char     last_name[28];   /* last enumerated filename */
} g_feat_dbg;

/* feature_discover() callback: `name` is a file under /modules/features. Peek its
 * header; if it's a valid feature module (magic + ABI ok, declares a menu/ext),
 * register it. */
static void feat_scan_cb(const char *name) {
    g_feat_dbg.enumerated++;
    str_lcpy(g_feat_dbg.last_name, sizeof(g_feat_dbg.last_name), name);
    if (g_feat_count >= MAX_FEATURES) return;
    char path[64];
    str_lcpy(path, sizeof(path), FEATURE_DIR "/");
    str_lcat(path, sizeof(path), name);
    for (int i = 0; i < g_feat_count; i++)            /* dedup: same module on >1 partition = once */
        if (strcmp(g_feat[i].path, path) == 0) return;

    /* Peek the header where the module lives: in place from the FAT store (the XIP source) via
     * vfs_map_file, else from the LFS. */
    module_header_t h;
    uint32_t faddr = 0, fsz = 0;
    if (vfs_map_file(path, &faddr, &fsz) == 0 && fsz >= sizeof(h))
        memcpy(&h, (const void *)faddr, sizeof(h));
    else if (vfs_lfs_read_header(path, &h, sizeof(h)) != 0) { g_feat_dbg.hdr_read_fail++; return; }
    g_feat_dbg.last_magic = h.magic;
    g_feat_dbg.last_abi   = h.abi;
    if (h.magic != MODULE_MAGIC)     { g_feat_dbg.magic_bad++; return; }
    if (h.abi != MODULE_ABI_VERSION) { g_feat_dbg.abi_bad++;   return; }
    if (h.menu_id == MODULE_MENU_NONE && h.file_ext[0] == '\0') { g_feat_dbg.not_feature++; return; }   /* not a feature */
    g_feat_dbg.registered++;

    feature_entry_t *e = &g_feat[g_feat_count++];
    e->menu_id = h.menu_id;
    h.menu_label[sizeof(h.menu_label) - 1] = '\0';
    h.file_ext[sizeof(h.file_ext) - 1] = '\0';
    str_lcpy(e->label, sizeof(e->label), h.menu_label);
    str_lcpy(e->ext, sizeof(e->ext), h.file_ext);
    str_lcpy(e->path, sizeof(e->path), path);
    memcpy(e->menu_label_xlat, h.menu_label_xlat, sizeof(e->menu_label_xlat));
    e->menu_label_xlat[sizeof(e->menu_label_xlat) - 1] = '\0';
}

void feature_discover(void) {
    g_feat_dbg.discover_calls++;
    /* Bring up the full LFN FAT-RW driver from the store BEFORE scanning it, so the store scan (and
     * every later FAT access) goes through it -- not the in-core 8.3 RO reader, which mishandles the
     * LFN directory entries pyfatfs writes (even for 8.3-clean names) and silently drops a second
     * store module. The in-core reader stays only the bootstrap that loads this. Best-effort: a store
     * with no /fs/fat.bin (driver stripped) falls back to the 8.3 reader -- 8.3-clean factory names
     * still resolve. Safe: this lazy load already happens on install/delete without breaking XIP. */
    vfs_ensure_rw("FAT");   /* no-ops if already loaded; gracefully leaves the 8.3 RO reader if absent */
    g_feat_count = 0;
    /* Scan every non-SD partition's /modules/features (the EXT-FAT module store first, then any
     * LFS), so XIP modules living in the FAT store are discovered. feat_scan_cb dedups by path. */
    int n = partition_get_count();
    for (int i = 0; i < n; i++) {
        partition_info_t *p = partition_get_info(i);
        if (!p || partition_is_sd(p)) continue;
        vfs_enum_dir(p, FEATURE_DIR, feat_scan_cb);
    }
}

int feature_count(int menu_id) {
    int c = 0;
    for (int i = 0; i < g_feat_count; i++) if (g_feat[i].menu_id == menu_id) c++;
    return c;
}

static const feature_entry_t *feature_nth(int menu_id, int n) {
    for (int i = 0; i < g_feat_count; i++)
        if (g_feat[i].menu_id == menu_id && n-- == 0) return &g_feat[i];
    return NULL;
}

const char *feature_label(int menu_id, int n) {
    const feature_entry_t *e = feature_nth(menu_id, n);
    if (!e) return "";
    /* The module header carries its title per language ("code\0title\0...\0"); resolve the
     * active language live (re-evaluated each draw, so switching language updates the menu),
     * falling back to the English header label when the module has no matching translation. */
    const char *code = i18n_code(i18n_current());
    const char *end = e->menu_label_xlat + sizeof(e->menu_label_xlat);
    for (const char *p = e->menu_label_xlat; p < end && *p; ) {
        const char *title = p + strlen(p) + 1;
        if (title >= end) break;
        if (code && strcmp(p, code) == 0) return title;
        p = title + strlen(title) + 1;
    }
    return e->label;
}

const char *feature_path_for_ext(const char *ext) {
    if (!ext || !ext[0]) return NULL;
    for (int i = 0; i < g_feat_count; i++)
        if (g_feat[i].ext[0] && ext_list_match(g_feat[i].ext, ext)) return g_feat[i].path;
    return NULL;
}

/* --- the host vtable handed to a running feature module --- */
static int feat_just_pressed(int b) { return input_just_pressed((input_button_t)b) ? 1 : 0; }
static int feat_is_pressed(int b)   { return input_is_pressed((input_button_t)b) ? 1 : 0; }

/* Pull-based file streaming for a file launch. The partition the file lives on is stashed by
 * feat_run; one file is open at a time (a player streams a single media file). */
static const partition_info_t *g_feat_src;
static vfs_stream_t             g_feat_stream;   /* the one open media stream (shared vfs_stream_* mechanism) */
static const char              *g_feat_modpath;   /* the running module's path (for state files) */

/* Per-module state file: /modules/state/<module basename>. */
static void feat_state_path(char *out, int cap) {
    const char *base = g_feat_modpath ? g_feat_modpath : "feature";
    const char *slash = base;
    for (const char *p = base; *p; p++) if (*p == '/') slash = p + 1;
    str_lcpy(out, cap, "/modules/state/");
    str_lcat(out, cap, slash);
}
static bool feat_state_save(const void *blob, uint32_t len) {
    if (!g_feat_modpath || !blob) return false;
    char path[80]; feat_state_path(path, sizeof(path));
    return vfs_lfs_write_file(path, blob, len) == 0;     /* auto-loads lfs_rw + mkdir; -1 on no RW */
}
static int feat_state_load(void *blob, uint32_t cap) {
    if (!g_feat_modpath || !blob) return -1;
    char path[80]; feat_state_path(path, sizeof(path));
    uint32_t got = 0;
    if (vfs_read_file(path, blob, cap, &got) != 0) return -1;
    return (int)got;
}

/* True until the first feature module has run since boot (g_feat_ran is .bss, zeroed at
 * boot). Lets a module distinguish a fresh power-on from a same-session re-entry (e.g. the
 * MP3 player comes up paused on a cold boot instead of auto-playing). */
static int g_feat_ran;
static int feat_is_first_launch(void) { return !g_feat_ran; }

/* Source-partition token = its (stable across reboots) base address. */
static uint32_t feat_source_id(void) { return g_feat_src ? g_feat_src->address : 0; }
static void feat_set_source(uint32_t id) {
    if (!id) return;
    int n = partition_get_count();
    for (int i = 0; i < n; i++) {
        partition_info_t *p = partition_get_info(i);
        if (p && p->address == id) { g_feat_src = p; return; }
    }
}
/* Point the source at the SD-card partition if a card is present, so a Tools-menu launch (no
 * launched file) can default to an on-card directory. Returns true if an SD partition was found. */
static bool feat_set_source_sd(void) {
    int n = partition_get_count();
    for (int i = 0; i < n; i++) {
        partition_info_t *p = partition_get_info(i);
        if (p && partition_is_sd(p)) { g_feat_src = p; return true; }
    }
    return false;
}
/* Borrow idle D2-2 AHB-SRAM (D2_SCRATCH_BASE, 64 KB) as a transient CPU-only scratch buffer, so a
 * feature module's big working buffer doesn't bloat its .bss (which counts against the module pool).
 * Free while the menu runs (the OFW patch only uses D2-2 during an OFW boot; fastcap only uses D2-1).
 * Exclusive to the one live transient module, reclaimed implicitly when it returns. NOT DMA-reachable
 * (SAI1/DMA1 cannot reach D2) — CPU access only. Returns NULL if more than the bank is requested. */
static void *feat_scratch_get(uint32_t size) {
    if (size > D2_SCRATCH_SIZE) return NULL;
    return (void *)D2_SCRATCH_BASE;
}

static void *feat_file_open(const char *path) {
    if (!g_feat_src || !path) return NULL;
    const char *dn = partition_driver_name(g_feat_src);
    /* Ensure the RW driver is loaded so seek (media scrubbing / resume) works -- the in-core
     * RO bootstrap has no seek, which is all that's registered on a cold boot. */
    vfs_ensure_rw(dn);
    vfs_driver_t *d = dn ? vfs_get_driver(dn) : NULL;
    if (!d) return NULL;
    /* The transient module load scanned + mounted all filesystems to find the .bin, disturbing
     * the source mount; vfs_stream_open_drv re-mounts the source partition before opening. One
     * media stream at a time -> the shared g_feat_stream handle (the same vfs_stream_* primitive
     * the font pager uses, on its OWN handle, so the two never clash). */
    if (vfs_stream_open_drv(&g_feat_stream, d, g_feat_src->address, g_feat_src->size, path) != 0)
        return NULL;
    return &g_feat_stream;
}
static int feat_file_read(void *f, void *buf, uint32_t n) {
    (void)f;   /* one media stream at a time -> the shared g_feat_stream handle */
    return vfs_stream_read(&g_feat_stream, buf, n);
}
static int feat_file_seek(void *f, uint32_t off) {
    (void)f;
    return vfs_stream_seek(&g_feat_stream, off);
}
static void feat_file_close(void *f) {
    (void)f;
    vfs_stream_close(&g_feat_stream);
}

static void feat_list_dir(const char *dirpath,
                          void (*cb)(const char *name, int is_dir, uint32_t size, void *user),
                          void *user) {
    if (!g_feat_src || !dirpath || !cb) return;
    const char *dn = partition_driver_name(g_feat_src);
    vfs_driver_t *d = dn ? vfs_get_driver(dn) : NULL;
    if (!d || !d->opendir || !d->readdir) return;
    if (d->mount) d->mount(g_feat_src->address, g_feat_src->size);   /* re-mount past load clobber */
    void *dc = NULL;
    if (d->opendir(dirpath, &dc) != 0) return;
    static vfs_dirent_t ent;   /* 264 B -> keep off the stack */
    while (d->readdir(dc, &ent) == 1)
        cb(ent.name, ent.type == VFS_TYPE_DIR, ent.size, user);
    if (d->closedir) d->closedir(dc);
}

/* Modal file picker: run the in-core browser until the user picks a file or backs out, by
 * pumping the same ui_update/ui_draw the main loop runs. The browser's on_pick captures the
 * path; back-out pops the page (stack depth drops back). */
static char g_pick_out[256];
static int  g_pick_done, g_pick_ok, g_pick_dir;
static uint32_t g_pick_size;
static void feat_pick_cb(const char *path, bool is_dir, uint32_t size) {
    str_lcpy(g_pick_out, sizeof(g_pick_out), path);
    g_pick_size = size; g_pick_dir = is_dir ? 1 : 0;
    g_pick_ok = 1; g_pick_done = 1;
}
static bool feat_pick_file(const char *ext, char *out, int cap, uint32_t *out_size, bool *out_is_dir) {
    if (!out || cap <= 0) return false;
    g_pick_done = 0; g_pick_ok = 0;
    int depth0 = ui_stack_depth();
    browser_open_picker(ext, feat_pick_cb);
    while (!g_pick_done && ui_stack_depth() > depth0) { ui_update(); ui_draw(); }
    if (ui_stack_depth() > depth0) ui_pop();        /* close the picker page after a pick */
    const partition_info_t *picked = (const partition_info_t *)browser_active_partition();
    browser_picker_restore();                       /* un-clobber the suspended main browser */
    if (g_pick_ok) {
        str_lcpy(out, cap, g_pick_out);
        if (out_size)   *out_size = g_pick_size;
        if (out_is_dir) *out_is_dir = g_pick_dir ? true : false;
        /* Point the module's file stream at the picked item's partition, so a Tools launch
         * (no launch file -> g_feat_src was NULL) can still play, and so picks work across
         * filesystems. */
        if (picked) g_feat_src = picked;
        return true;
    }
    return false;
}

/* Themed chrome a feature app draws each frame so it matches the active theme. A
 * feature launches over a <320-wide menu, so the core's app-mode header/footer fill
 * is otherwise skipped -- route through the forced-solid core renderers so the core
 * and modules share ONE chrome path (no re-implemented bar fills here). */
static void feat_draw_header(const char *title) {
    ui_draw_header_solid(title);
}
static void feat_draw_footer(const char *hint) {
    ui_draw_footer_chrome(hint);
}

/* Loader module registry, surfaced to the Module Overview diagnostic (a PIE module can't read the
 * core global g_mod_recs directly). */
static int feat_mod_count(void) { return (int)g_mod_recs_n; }
static int feat_mod_get(int i, mod_view_t *o) {
    if (i < 0 || (uint32_t)i >= g_mod_recs_n) return 0;
    const mod_rec_t *r = &g_mod_recs[i];
    const char *base = r->path;
    for (const char *q = r->path; *q; q++) if (*q == '/') base = q + 1;
    str_lcpy(o->name, sizeof(o->name), base);
    o->flash = r->flash; o->ram = r->ram; o->flags = r->flags;
    o->resident = (r->base + r->ram <= g_pool_next);
    return 1;
}

/* --- unified UI service seam ---
 * Render a module's view descriptor through the SAME in-core ui_list + chrome the File Browser
 * and Partition Viewer use, so the module gets the divider, themed selector, smooth scroll + wrap,
 * and consistent header/footer without hand-rolling a draw loop. Self-contained: it runs its own
 * per-frame input + draw loop (the module's run() blocks here) until B/back, mirroring the in-core
 * dual-pane geometry (content x=0,y=22,w=320,h=196). The desc callbacks are MODULE functions called
 * directly -- r9 stays valid across the crossing (callee-saved + -ffixed-r9), so no trampoline. */
static int g_view_done;
static void feat_view_back(void) { g_view_done = 1; }
static void feat_run_view(const ui_view_desc_t *desc) {
    if (!desc || !desc->get_label) return;
    ui_list_t list;
    ui_list_init(&list, desc->title, desc->count, desc->get_label, desc->on_select);
    if (desc->kind == UI_VIEW_DUALPANE && desc->draw_right_pane)
        ui_list_set_split(&list, desc->draw_right_pane);
    list.on_back = feat_view_back;

    g_view_done = 0;
    while (!g_view_done) {
        input_update();
        ui_list_update(&list);
        if (g_view_done) break;   /* on_back fired this frame */

        /* Chrome + content, drawn explicitly (the module's run() blocks here, so the main loop's
         * ui_draw isn't running). The header fill defaults to the theme header color; a module
         * can override it via desc->header_color just for the header draw, restored afterward so
         * later chrome keeps the real theme header color. */
        gui_draw_fill_rect(0, 23, SCREEN_WIDTH, 195, COLOR_BG);   /* clear content (double-buffered) */
        uint16_t saved_header = gui_header_color;
        if (desc->header_color) gui_header_color = desc->header_color;
        ui_draw_header_solid(desc->title);
        gui_header_color = saved_header;
        ui_list_draw(&list, 0, 22, SCREEN_WIDTH, 196);
        ui_draw_footer_chrome(desc->footer);
        gui_refresh();
    }
}

static feature_host_t g_feature_host = {
    .get_tick      = HAL_GetTick,
    .framebuffer   = gui_current_framebuffer,
    .draw_text     = gui_draw_text,
    .fill_rect     = gui_draw_fill_rect,
    .progress_bar  = gui_draw_progress_bar,
    .present       = gui_refresh,
    .input_update  = input_update,
    .just_pressed  = feat_just_pressed,
    .is_pressed    = feat_is_pressed,
    .ui            = NULL,   /* set in feat_run (mod_ui() isn't a constant) */
    .file_open     = feat_file_open,
    .file_read     = feat_file_read,
    .file_seek     = feat_file_seek,
    .file_close    = feat_file_close,
    .list_dir      = feat_list_dir,
    .pick_file     = feat_pick_file,
    .gui           = NULL,   /* set in feat_run (gui_api() isn't a constant) */
    .draw_header   = feat_draw_header,
    .draw_footer   = feat_draw_footer,
    .state_save    = feat_state_save,
    .state_load    = feat_state_load,
    .source_id     = feat_source_id,
    .set_source    = feat_set_source,
    .set_source_sd = feat_set_source_sd,
    .scratch_get   = feat_scratch_get,
    .is_first_launch = feat_is_first_launch,
    .mod_count     = feat_mod_count,
    .mod_get       = feat_mod_get,
    .run_view      = feat_run_view,
    .pane_row      = ui_list_pane_row,
};

/* Map the loader's failure class to a user-facing message, so a transient launch that
 * fails shows WHY instead of silently doing nothing. */
static const char *feat_load_err_msg(void) {
    switch (mod_load_last_error()) {
        case MOD_ERR_READ:  return "Module read failed";
        case MOD_ERR_ABI:   return "Module version mismatch";
        case MOD_ERR_NOMEM: return "Out of module memory";
        default:            return "Module launch failed";
    }
}

/* Transiently load the module at `modpath`, run it (blocks), reclaim the slot. `src` is the
 * partition `file` lives on (for the host file API), or NULL for a menu launch. */
static void feat_run(const char *modpath, const char *file, const partition_info_t *src) {
    g_feature_host.ui  = mod_ui();
    g_feature_host.gui = gui_api();
    g_feat_src = src;
    g_feat_modpath = modpath;   /* for the per-module state file */
    uint32_t mark = mod_pool_mark();
    feature_api_t api = {0};
    if (mod_load_feature(modpath, &g_feature_host, &api) && api.run)
        mod_invoke_r9(&g_feature_host, file, (void *)api.run);   /* r9 set for an r9-pic feature's run */
    else
        ui_show_error(feat_load_err_msg());   /* no more silent failure */
    mod_pool_reset(mark);
    feat_file_close(NULL);   /* close the stream if the module forgot to */
    g_feat_src = NULL;
    g_feat_ran = 1;          /* a feature has now run this boot (next launch isn't "first") */
}

void feature_launch(int menu_id, int n) {
    const feature_entry_t *e = feature_nth(menu_id, n);
    if (e) feat_run(e->path, NULL, NULL);
}

void feature_launch_path(const char *modpath, const char *file, const void *src_partition) {
    if (modpath) feat_run(modpath, file, (const partition_info_t *)src_partition);
}
