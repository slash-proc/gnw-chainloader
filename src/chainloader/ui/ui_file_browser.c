#include "ui.h"
#include "ui_list.h"
#include "storage/vfs.h"
#include "partition.h"
#include "input.h"
#include "gui.h"
#include "utils.h"
#include "board.h"
#include "strings.h"
#include "system/fileops.h"
#include "system/loader.h"
#include "system/feature.h"
#include <string.h>

extern ui_window_t PAGE_BROWSER;

#define BROWSER_TYPE_FILE 0
#define BROWSER_TYPE_DIR  1

/* Label for the parent-directory navigation entry (pinned at the top of every
 * listing). Defined once so the literal isn't repeated across scan/sort/nav. */
#define PARENT_DIR_DOTS  ".."

typedef enum {
    BROWSER_MODE_SCANNING,
    BROWSER_MODE_FS_LIST,
    BROWSER_MODE_NAVIGATE
} browser_mode_t;

typedef struct {
    partition_info_t* active_partition;
    char current_path[256];
    int selected;
    int scroll_y;
    int mount_res;
    int selection_stack[8];
    int stack_ptr;
    bool is_initialized;
} browser_tab_t;

static browser_mode_t g_mode;
static ui_list_t g_list_browser;
static partition_info_t* fs_partitions[16];
static int fs_partition_count = 0;
static bool fs_is_rw[16];
/* All three in 512-byte SECTORS (statfs contract), so multi-GB cards don't
 * overflow a uint32_t; displayed via format_size_sectors. */
static uint32_t fs_total_space[16];
static uint32_t fs_used_space[16];
static uint32_t fs_free_space[16];
static bool fs_space_valid[16];
static partition_info_t* active_partition = NULL;

/* Directory listing: names are packed end-to-end in one pool (no fixed-width
 * slots), with a compact index. Holds full-length names for up to FB_MAX_FILES
 * entries, sorted, all in RAM (~35 KB) so scrolling never stalls. RAM, not the
 * 40K flash budget. Pool offsets are uint16_t, so FB_NAME_POOL must be <= 64 KB. */
#define FB_MAX_FILES   512
#define FB_NAME_POOL   32768
typedef struct { uint16_t off; uint8_t type; uint32_t size; } fb_entry_t;
static char fb_name_pool[FB_NAME_POOL];
static uint32_t fb_pool_used = 0;
static fb_entry_t file_entries[FB_MAX_FILES];
static int file_count = 0;
static inline const char *fb_name(int idx) { return fb_name_pool + file_entries[idx].off; }

static int mount_res = 0;
static char current_path[256] = "/";

static char browser_title[320];

static int selection_stack[8];
static int stack_ptr = 0;

static browser_tab_t g_tabs[2] = { {0}, {0} };
static int g_active_tab = 0;

/* Per-session browser config: capability gates + an optional "pick" mode (A on a
 * file hands its path to a callback instead of opening the ops menu) + an extension
 * filter (only matching files are listed; folders always shown so you can navigate).
 * A feature module reuses the browser as a stripped-down picker by setting these via
 * browser_open_picker(). One live browser at a time, so a single config suffices.
 * Default = the full browser (all ops, no pick, no filter) = today's behaviour. */
typedef struct {
    bool allow_copy, allow_paste, allow_delete;
    bool pick_mode;
    char ext_filter[16];                                 /* comma-separated lowercase exts; "" = all */
    void (*on_pick)(const char *path, bool is_dir, uint32_t size);   /* size: bytes (0 for dirs) */
} fb_cfg_t;

static fb_cfg_t g_fb_cfg = { .allow_copy = true, .allow_paste = true, .allow_delete = true };

static void fb_cfg_reset_default(void) {
    g_fb_cfg = (fb_cfg_t){ .allow_copy = true, .allow_paste = true, .allow_delete = true };
}

/* A nested picker (browser_open_picker, e.g. from the MP3 player's Add) reuses this single
 * browser instance, so it would clobber + unmount the suspended main browser. Snapshot the
 * main state before the picker and restore it (re-mounting) after — kept in .bss (RAM, not
 * the flash budget). */
static struct { browser_tab_t tabs[2]; int active_tab; fb_cfg_t cfg; } g_picker_save;
static bool g_picker_saved = false;

static void menu_browser_back(void);
static void open_file_context_menu(void);
static void scan_files(void);
static void load_tab_state(void);
static void save_tab_state(void);
static void update_browser_title(void);
static void mount_tab_partition(browser_tab_t *tab);
static bool join_path(char *out, size_t cap, const char *base, const char *name);

/* Storage-location prefix for the friendly FS name: EXT (external flash), SD (card),
 * INT (internal flash), MEM (anything else). */
static const char *fs_loc_prefix(const partition_info_t *p) {
    if (partition_is_sd(p)) return "SD";
    uint32_t a = p->address;
    if (a >= 0x90000000UL && a < 0xA0000000UL) return "EXT";
    if (a >= 0x08000000UL && a < 0x08200000UL) return "INT";
    return "MEM";
}

/* Friendly filesystem name "<LOC>-<FS>-<NN>" (e.g. EXT-FAT-01, SD-FAT-01): storage location,
 * type code (FAT/LFS/FROG), and a 2-digit 1-based index per (location, type). The raw base
 * address lives in the detail pane, not the name. A non-FS partition keeps its raw type. */
static void fs_display_name(const partition_info_t *p, char *buf, int cap) {
    const char *fs = partition_fs_code(p);
    if (!fs) { str_lcpy(buf, cap, p->type); return; }
    const char *loc = fs_loc_prefix(p);
    int idx = 0, n = partition_get_count();
    for (int i = 0; i < n; i++) {
        partition_info_t *q = partition_get_info(i);
        if (q == p) break;
        const char *qfs = q ? partition_fs_code(q) : NULL;
        if (qfs && strcmp(qfs, fs) == 0 && strcmp(fs_loc_prefix(q), loc) == 0) idx++;
    }
    str_lcpy(buf, cap, loc);
    str_lcat(buf, cap, "-");
    str_lcat(buf, cap, fs);
    str_lcat(buf, cap, "-");
    char ib[8];
    int_to_str(idx + 1, ib);
    if (ib[1] == '\0') str_lcat(buf, cap, "0");   /* zero-pad to 2 digits */
    str_lcat(buf, cap, ib);
}

static void update_browser_title(void) {
    char tab_indicator[16];
    bool show_tabs = !g_fb_cfg.pick_mode;               /* picker is single-pane (no tabs) */
    if (g_active_tab == 0 || !show_tabs) {
        tab_indicator[0] = '\0';
    } else {
        strcpy(tab_indicator, gui_rtl ? "> " : "< ");   /* LEFT/RIGHT swap in RTL */
    }

    strcpy(browser_title, tab_indicator);
    if (!active_partition) {
        strcat(browser_title, tr(STR_SELECT_FS));
    } else {
        /* FS tag: <LOC>-<FS>-<NN>, e.g. "EXT-FAT-01:/". The RO/RW distinction + the raw
         * address live in the FS-list detail pane, not the title. */
        char name[24];
        fs_display_name(active_partition, name, sizeof name);
        strcat(browser_title, name);
        strcat(browser_title, ":");   /* delimit the FS root, e.g. "EXT-FAT-01:/" */
        strcat(browser_title, current_path);
        int len = strlen(browser_title);
        if (len > 0 && browser_title[len - 1] != '/') {
            strcat(browser_title, "/");
        }
    }
    if (g_active_tab == 0 && show_tabs) {
        strcat(browser_title, gui_rtl ? " <" : " >");   /* trailing tab arrow (RTL-mirrored) */
    }
    PAGE_BROWSER.title = browser_title;
}

/* Append one entry (full name into the pool + its metadata). Stops accepting
 * once either the entry index or the name pool is exhausted. */
static void add_entry_raw(const char *name, uint8_t type, uint32_t size) {
    if (file_count >= FB_MAX_FILES) return;
    size_t len = strlen(name);
    if (fb_pool_used + len + 1 > FB_NAME_POOL) return;   /* name pool full */
    file_entries[file_count].off  = (uint16_t)fb_pool_used;
    file_entries[file_count].type = type;
    file_entries[file_count].size = (type == BROWSER_TYPE_DIR) ? 0 : size;
    memcpy(fb_name_pool + fb_pool_used, name, len + 1);
    fb_pool_used += (uint32_t)len + 1;
    file_count++;
}

/* Pointer just past the last '.' in name (the extension), or NULL if there is no
 * dot or the only dot is the first char (a dotfile/dotfolder has no extension). */
static const char *fb_ext(const char *name) {
    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    return (dot && dot != name) ? dot + 1 : NULL;
}

static void add_file_entry(const char *name, uint8_t type, uint32_t size) {
    /* Hide dot-entries entirely (".", "..", and dotfiles/folders such as the
     * .Trashes / .Spotlight-V100 / .fseventsd / ._* noise on PC-formatted SD
     * cards). The parent-nav ".." is added explicitly by scan_files. */
    if (name[0] == '.') return;
    /* Picker extension filter: when set, list only files with the wanted extension
     * (folders always pass, so the user can still navigate into them). */
    if (g_fb_cfg.ext_filter[0] && type == BROWSER_TYPE_FILE) {
        const char *e = fb_ext(name);
        if (!e || !ext_list_match(g_fb_cfg.ext_filter, e)) return;
    }
    add_entry_raw(name, type, size);
}

/* Case-insensitive name compare (so listings read alphabetically regardless of
 * 8.3-uppercase FAT names vs. mixed-case LFN/LittleFS names). */
static int name_cmp_ci(const char *a, const char *b) {
    for (;;) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (!*a) return 0;
        a++; b++;
    }
}

/* Alphabetize the listing. A leading ".." stays pinned on top; dot-entries
 * (".foo") naturally sort to the front since '.' precedes letters/digits.
 * Shared by every filesystem (FatFS readdir is otherwise unordered). */
static void sort_file_entries(void) {
    /* Sort the index only; names stay put in the pool (offsets reorder). */
    int start = (file_count > 0 && strcmp(fb_name(0), PARENT_DIR_DOTS) == 0) ? 1 : 0;
    for (int i = start; i < file_count - 1; i++) {
        for (int j = start; j < file_count - 1 - (i - start); j++) {
            if (name_cmp_ci(fb_name(j), fb_name(j + 1)) > 0) {
                fb_entry_t t = file_entries[j];
                file_entries[j] = file_entries[j + 1];
                file_entries[j + 1] = t;
            }
        }
    }
}

static void scan_files(void) {
    file_count = 0;
    fb_pool_used = 0;
    if (!active_partition) return;

    if (strcmp(current_path, "/") != 0 && current_path[0] != '\0') {
        add_entry_raw(PARENT_DIR_DOTS, BROWSER_TYPE_DIR, 0);  /* parent-nav, bypasses dot-filter */
    }

    const char *driver_name = partition_driver_name(active_partition);
    vfs_driver_t *drv = driver_name ? vfs_get_driver(driver_name) : NULL;
    if (!drv) return;
    
    void *dir_ctx = NULL;
    if (drv->opendir(current_path, &dir_ctx) == 0) {
        vfs_dirent_t ent;
        while (drv->readdir(dir_ctx, &ent) > 0) {
            add_file_entry(ent.name, (ent.type == VFS_TYPE_DIR) ? BROWSER_TYPE_DIR : BROWSER_TYPE_FILE, ent.size);
        }
        drv->closedir(dir_ctx);
    }

    sort_file_entries();
}

static const char* get_file_label(int idx) {
    return fb_name(idx);
}

static void browser_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (selected_idx < 0 || selected_idx >= file_count) return;

    (void)selected_tick;

    const char *ext = "";
    if (file_entries[selected_idx].type == BROWSER_TYPE_DIR) {
        ext = tr(STR_DIR);
    } else {
        const char *e = fb_ext(fb_name(selected_idx));
        ext = e ? e : tr(STR_FILE);
    }
    ui_list_pane_row(0, tr(STR_LBL_TYPE), ext, false, 0);

    char size_buf[16];
    const char *size_val = "-";
    if (file_entries[selected_idx].type != BROWSER_TYPE_DIR) {
        format_size(file_entries[selected_idx].size, size_buf);
        size_val = size_buf;
    }
    ui_list_pane_row(1, tr(STR_LBL_SIZE), size_val, false, 0);
}

static void fs_list_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (selected_idx < 0 || selected_idx >= fs_partition_count) return;
    (void)selected_tick;
    partition_info_t *p = fs_partitions[selected_idx];
    char buf[32];   /* holds a size string or a translated "UNKNOWN" (3 bytes/char) */

    if (fs_space_valid[selected_idx]) {
        format_size_sectors(fs_free_space[selected_idx], buf);
        ui_list_pane_row(0, tr(STR_LBL_FREE), buf, false, 0);
        format_size_sectors(fs_used_space[selected_idx], buf);
        ui_list_pane_row(1, tr(STR_LBL_USED), buf, false, 0);
    } else {
        /* Free/Used unavailable (read-only FatFs has no f_getfree) — show total
         * capacity instead. SD stores size as sectors (see partition.c). */
        if (p && partition_is_sd(p))  format_size_sectors(p->size, buf);
        else if (p)                   format_size(p->size, buf);
        else                          str_lcpy(buf, sizeof(buf), tr(STR_UNKNOWN));
        ui_list_pane_row(0, tr(STR_LBL_TOTAL), buf, false, 0);
    }

    /* Mode row: the RO/RW distinction (moved here from the title bar). */
    ui_list_pane_row(2, tr(STR_LBL_MODE), fs_is_rw[selected_idx] ? tr(STR_MODE_RW) : tr(STR_MODE_RO), false, 0);

    /* Address row: the raw partition base (the SD shows its sentinel 0xC0000000). */
    if (p) {
        char addr_buf[16];
        str_lcpy(addr_buf, sizeof addr_buf, "0x");
        hex_to_str(p->address, addr_buf + 2, 8);
        ui_list_pane_row(3, tr(STR_LBL_ADDR), addr_buf, false, 0);
    }
}

static void on_file_action(int idx) {
    if (idx >= 0 && idx < file_count) {
        if (strcmp(fb_name(idx), PARENT_DIR_DOTS) == 0) {
            menu_browser_back();
            return;
        }
        if (file_entries[idx].type == BROWSER_TYPE_DIR) {
            int len = strlen(current_path);
            if (len > 1 && current_path[len - 1] != '/') {
                current_path[len++] = '/';
            }
            strncpy(current_path + len, fb_name(idx), sizeof(current_path) - len - 1);
            current_path[sizeof(current_path) - 1] = '\0';

            if (stack_ptr < 8) selection_stack[stack_ptr++] = g_list_browser.selected;

            scan_files();
            update_browser_title();
            g_list_browser.num_items = file_count;
            g_list_browser.selected = 0;
            g_list_browser.scroll_y = 0;
            g_list_browser.selected_tick = HAL_GetTick();
        } else if (g_fb_cfg.pick_mode) {
            /* Picker: hand the chosen file's full path to the callback (e.g. a feature
             * module's pick_file) instead of opening copy/paste/delete. */
            if (g_fb_cfg.on_pick) {
                char buf[512];
                if (join_path(buf, sizeof(buf), current_path, fb_name(idx)))
                    g_fb_cfg.on_pick(buf, false, file_entries[idx].size);
            }
        } else {
            /* If a feature module is registered for this file's extension, launch it
             * with the file (the core names no extension itself); else the ops menu. */
            const char *name = fb_name(idx);
            const char *e = fb_ext(name);
            const char *modpath = e ? feature_path_for_ext(e) : NULL;
            char full[512];
            if (modpath && join_path(full, sizeof(full), current_path, name)) {
                feature_launch_path(modpath, full, active_partition);
                /* The transient module load disturbed our mount; re-establish + refresh. */
                const char *dn = partition_driver_name(active_partition);
                vfs_driver_t *d = dn ? vfs_get_driver(dn) : NULL;
                if (d && d->mount) d->mount(active_partition->address, active_partition->size);
                scan_files();
                g_list_browser.num_items = file_count;
            } else {
                open_file_context_menu();
            }
        }
    }
}

static void build_fs_list(void) {
    fs_partition_count = 0;
    int total = partition_get_count();
    for (int i = 0; i < total; i++) {
        partition_info_t* p = partition_get_info(i);
        char t0 = p->type[0];
        char t1 = p->type[1];
        if (t0 == 'L' || (t0 == 'F' && (t1 == 'r' || t1 == 'A'))) {
            if (fs_partition_count < 16) {
                fs_is_rw[fs_partition_count] = false;
                fs_space_valid[fs_partition_count] = false;
                
                if (t0 == 'L') {
                    // Check if lfs_rw.bin exists on this partition by temporarily mounting it RO
                    vfs_driver_t *ro_drv = vfs_get_driver("LFS");
                    if (ro_drv && ro_drv->mount(p->address, p->size) == 0) {
                        void *f = NULL;
                        if (ro_drv->open("/fs/lfs.bin", 1, &f) == 0) {
                            ro_drv->close(f);
                            fs_is_rw[fs_partition_count] = true;
                        }
                        
                        // Query space
                        uint32_t tot = 0, fre = 0;
                        if (ro_drv->statfs && ro_drv->statfs(&tot, &fre) == 0) {
                            fs_total_space[fs_partition_count] = tot;
                            fs_used_space[fs_partition_count] = tot - fre;
                            fs_free_space[fs_partition_count] = fre;
                            fs_space_valid[fs_partition_count] = true;
                        }

                        ro_drv->unmount();
                    }
                } else if (t0 == 'F' && t1 == 'r') {
                    // FrogFS space query (read directly from flash header)
                    uint32_t magic = *(const uint32_t *)p->address;
                    if (magic == 0x474F5246) { // "FROG"
                        uint32_t bin_sz = *(const uint32_t *)((const uint8_t *)p->address + 8);
                        /* Stored in 512-byte sectors to match the statfs contract. */
                        fs_total_space[fs_partition_count] = p->size / 512;
                        fs_used_space[fs_partition_count] = bin_sz / 512;
                        fs_free_space[fs_partition_count] =
                            (p->size > bin_sz ? (p->size - bin_sz) : 0) / 512;
                        fs_space_valid[fs_partition_count] = true;
                    }
                } else if (t0 == 'F' && t1 == 'A') {
                    /* The in-core FAT driver is RO (no write, no f_getfree). Load
                     * the RW+exFAT module (fatfs.bin) now if it's reachable, so
                     * MODE shows RW and f_getfree yields real Free/Used here in
                     * the selector (not just after the tab is entered). */
                    if (!vfs_is_fat_rw_loaded() &&
                        vfs_module_available("/fs/fat.bin")) {
                        vfs_load_dynamic_driver("FAT", "/fs/fat.bin");
                    }
                    vfs_driver_t *fat_drv = vfs_get_driver("FAT");
                    fs_is_rw[fs_partition_count] = (fat_drv && fat_drv->write != NULL);
                    if (fat_drv && fat_drv->mount && fat_drv->mount(p->address, p->size) == 0) {
                        uint32_t tot = 0, fre = 0;
                        if (fat_drv->statfs && fat_drv->statfs(&tot, &fre) == 0) {
                            fs_total_space[fs_partition_count] = tot;
                            fs_used_space[fs_partition_count] = tot - fre;
                            fs_free_space[fs_partition_count] = fre;
                            fs_space_valid[fs_partition_count] = true;
                        }
                        if (fat_drv->unmount) fat_drv->unmount();
                    }
                }
                
                fs_partitions[fs_partition_count++] = p;
            }
        }
    }
}

static const char* fs_list_get_label(int idx) {
    static char buf[48];
    if (idx >= 0 && idx < fs_partition_count) {
        partition_info_t* p = fs_partitions[idx];
        fs_display_name(p, buf, sizeof(buf));   /* "<LOC>-<FS>-<NN>", e.g. EXT-FAT-01 */
        return buf;
    }
    return "";
}

extern int lfs_mount_at(uint32_t base_addr, uint32_t block_count);

static void fs_list_on_action(int idx) {
    if (idx >= 0 && idx < fs_partition_count) {
        active_partition = fs_partitions[idx];
        g_tabs[g_active_tab].active_partition = active_partition;
        
        mount_tab_partition(&g_tabs[g_active_tab]);
        mount_res = g_tabs[g_active_tab].mount_res;
        
        current_path[0] = '/';
        current_path[1] = '\0';
        g_mode = BROWSER_MODE_NAVIGATE;
        
        if (mount_res == 0) {
            if (stack_ptr < 8) selection_stack[stack_ptr++] = g_list_browser.selected;
            scan_files();
        } else {
            file_count = 0;
        }
        
        update_browser_title();
        ui_list_init(&g_list_browser, browser_title, file_count, get_file_label, on_file_action);
        g_list_browser.on_back = menu_browser_back;
        
        if (mount_res == 0) {
            ui_list_set_split(&g_list_browser, browser_draw_right_pane);
        }
    }
}

static void menu_browser_back(void) {
    int prev_selected = (stack_ptr > 0) ? selection_stack[--stack_ptr] : 0;

    if (current_path[1] == '\0') {
        browser_tab_t *tab = &g_tabs[g_active_tab];
        if (tab->active_partition) {
            const char *drv_name = partition_driver_name(tab->active_partition);
            vfs_driver_t *drv = drv_name ? vfs_get_driver(drv_name) : NULL;
            if (drv && drv->unmount && tab->mount_res == 0) {
                drv->unmount();
            }
        }
        tab->active_partition = NULL;
        tab->mount_res = -1;
        active_partition = NULL;
        mount_res = -1;
        
        g_mode = BROWSER_MODE_FS_LIST;
        update_browser_title();
        ui_list_init(&g_list_browser, browser_title, fs_partition_count, fs_list_get_label, fs_list_on_action);
        ui_list_set_split(&g_list_browser, fs_list_draw_right_pane);
        g_list_browser.on_back = ui_pop;
        g_list_browser.selected = prev_selected;
    } else {
        int len = strlen(current_path);
        while (len > 0 && current_path[--len] != '/');
        if (len == 0) {
            current_path[1] = '\0';
        } else {
            current_path[len] = '\0';
        }
        scan_files();
        update_browser_title();
        g_list_browser.num_items = file_count;
        g_list_browser.selected = prev_selected;
        g_list_browser.selected_tick = HAL_GetTick();
    }
    
    if (g_list_browser.selected < g_list_browser.scroll_y) {
        g_list_browser.scroll_y = g_list_browser.selected;
    } else if (g_list_browser.selected >= g_list_browser.scroll_y + g_list_browser.visible_lines) {
        g_list_browser.scroll_y = g_list_browser.selected - (g_list_browser.visible_lines - 1);
    }
}

static void mount_tab_partition(browser_tab_t *tab) {
    if (!tab->active_partition) return;
    const char *driver_name = partition_driver_name(tab->active_partition);

    /* Each filesystem's RW driver is a PIE module loaded on demand when its tab
     * is entered, registering over the in-core RO driver of the same name. */
    vfs_ensure_rw(driver_name);

    vfs_driver_t *drv = driver_name ? vfs_get_driver(driver_name) : NULL;
    if (drv && drv->mount) {
        tab->mount_res = drv->mount(tab->active_partition->address, tab->active_partition->size);
    } else {
        tab->mount_res = -1;
    }
}

/* Unmount a tab's mounted partition (no-op if it isn't mounted or has no driver). */
static void tab_unmount(browser_tab_t *tab) {
    if (!tab->active_partition || tab->mount_res != 0) return;
    const char *name = partition_driver_name(tab->active_partition);
    vfs_driver_t *drv = name ? vfs_get_driver(name) : NULL;
    if (drv && drv->unmount) drv->unmount();
}

static void load_tab_state(void) {
    browser_tab_t *tab = &g_tabs[g_active_tab];
    active_partition = tab->active_partition;
    strcpy(current_path, tab->current_path);
    mount_res = tab->mount_res;
    stack_ptr = tab->stack_ptr;
    memcpy(selection_stack, tab->selection_stack, sizeof(selection_stack));
    
    mount_tab_partition(tab);
    scan_files();
    
    g_mode = (active_partition == NULL) ? BROWSER_MODE_FS_LIST : BROWSER_MODE_NAVIGATE;
    
    update_browser_title();
    
    if (g_mode == BROWSER_MODE_FS_LIST) {
        ui_list_init(&g_list_browser, browser_title, fs_partition_count, fs_list_get_label, fs_list_on_action);
        ui_list_set_split(&g_list_browser, fs_list_draw_right_pane);
        g_list_browser.on_back = ui_pop;
    } else {
        ui_list_init(&g_list_browser, browser_title, file_count, get_file_label, on_file_action);
        ui_list_set_split(&g_list_browser, browser_draw_right_pane);
        g_list_browser.on_back = menu_browser_back;
    }
    
    g_list_browser.selected = tab->selected;
    g_list_browser.scroll_y = tab->scroll_y;
}

static void save_tab_state(void) {
    browser_tab_t *tab = &g_tabs[g_active_tab];
    tab->active_partition = active_partition;
    strcpy(tab->current_path, current_path);
    tab->mount_res = mount_res;
    memcpy(tab->selection_stack, selection_stack, sizeof(selection_stack));
    tab->stack_ptr = stack_ptr;
    tab->selected = g_list_browser.selected;
    tab->scroll_y = g_list_browser.scroll_y;
}

static void init_tabs_if_needed(void) {
    build_fs_list();
    for (int i = 0; i < 2; i++) {
        if (!g_tabs[i].is_initialized && fs_partition_count > 0) {
            g_tabs[i].active_partition = NULL; // Start at FS overview list
            strcpy(g_tabs[i].current_path, "/");
            g_tabs[i].selected = 0;
            g_tabs[i].scroll_y = 0;
            g_tabs[i].stack_ptr = 0;
            g_tabs[i].is_initialized = true;
        }
    }
}

static void menu_browser_enter(ui_window_t *self) {
    PAGE_BROWSER.title = tr(STR_FILE_BROWSER);   /* header title (translated) */
    if (partition_scan_get_state() == PARTITION_SCAN_IN_PROGRESS) {
        /* Boot scan still running — show progress; it detects the SD itself. */
        g_mode = BROWSER_MODE_SCANNING;
        ui_list_init(&g_list_browser, tr(STR_FILE_BROWSER), 0, fs_list_get_label, fs_list_on_action);
        g_list_browser.on_back = ui_pop;
        return;
    }
    /* Re-check ONLY the SD card on entry (insert/remove/swap; no card-detect
     * line). The full flash scan stays a boot-time thing. If the set changed,
     * reset the tabs so none points at a removed SD partition. */
    if (partition_redetect_sd()) {
        g_tabs[0].is_initialized = false;
        g_tabs[1].is_initialized = false;
    }
    init_tabs_if_needed();
    g_active_tab = 0;
    load_tab_state();
}

static void menu_browser_update(ui_window_t *self) {
    if (g_mode == BROWSER_MODE_SCANNING) {
        partition_scan_update();
        if (partition_scan_get_state() == PARTITION_SCAN_COMPLETE) {
            init_tabs_if_needed();
            g_active_tab = 0;
            load_tab_state();
        }
        if (input_just_pressed(INPUT_B)) {
            ui_pop();
        }
        return;
    }
    
    // Switch tabs with LEFT / RIGHT (single-pane picker has no tabs)
    if (!g_fb_cfg.pick_mode &&
        (input_just_pressed(INPUT_LEFT) || input_just_pressed(INPUT_RIGHT))) {
        save_tab_state();
        tab_unmount(&g_tabs[g_active_tab]);
        g_active_tab = 1 - g_active_tab;
        load_tab_state();
        return;
    }
    
    if (g_mode == BROWSER_MODE_NAVIGATE && input_just_pressed(INPUT_PAUSE)) {
        if (g_fb_cfg.pick_mode) {
            /* Picker: PAUSE adds the HIGHLIGHTED entry (a folder -> add it whole, the caller
             * enumerates it; a file -> add it; ".." -> add the folder you're in). */
            int sel = g_list_browser.selected;
            if (g_fb_cfg.on_pick && sel >= 0 && sel < file_count) {
                if (strcmp(fb_name(sel), PARENT_DIR_DOTS) == 0) {
                    g_fb_cfg.on_pick(current_path, true, 0);
                } else {
                    char buf[512];
                    if (join_path(buf, sizeof(buf), current_path, fb_name(sel))) {
                        bool isdir = (file_entries[sel].type == BROWSER_TYPE_DIR);
                        g_fb_cfg.on_pick(buf, isdir, isdir ? 0 : file_entries[sel].size);
                    }
                }
            }
        } else {
            open_file_context_menu();
        }
        return;
    }
    
    if (g_mode == BROWSER_MODE_NAVIGATE && file_count == 0 && (input_just_pressed(INPUT_B) || input_just_pressed(INPUT_START))) {
        menu_browser_back();
        return;
    }
    ui_list_update(&g_list_browser);
}

static void menu_browser_draw(ui_window_t *self) {
    if (g_mode == BROWSER_MODE_SCANNING) {
        ui_draw_scan_progress();
        return;
    }
    
    ui_list_draw(&g_list_browser, self->x, self->y, self->w, self->h);
    
    if (g_mode == BROWSER_MODE_FS_LIST) {
        if (fs_partition_count == 0) {
            gui_draw_text(20, 60, tr(STR_NO_FILESYSTEMS), COLOR_RED);
        }
    } else if (g_mode == BROWSER_MODE_NAVIGATE) {
        ui_draw_footer(g_fb_cfg.pick_mode ? tr(STR_FOOTER_PICKER)
                                          : tr(STR_FOOTER_BROWSER));
        if (file_count == 0) {
            if (mount_res != 0) {
                char buf[64];
                str_fmt1_int(buf, sizeof(buf), tr(STR_MOUNT_FAIL), mount_res);
                gui_draw_text(20, 60, buf, COLOR_RED);
            } else {
                gui_draw_text(20, 60, tr(STR_EMPTY_DIR), COLOR_RED);
            }
        }
    }
}

static partition_info_t* copy_src_partition = NULL;
static char copy_src_path[512];
static char copy_src_name[256];
static uint32_t copy_src_size = 0;
static bool copy_src_is_dir = false;

/* Map a FILEOPS_* result (from the in-core single-file copy or the fileops module)
 * to a translated error string; NULL = success/cancel (no modal). */
static const char *cp_msg(int r) {
    switch (r) {
        case FILEOPS_OPEN_ERR:  return tr(STR_ERR_OPEN);
        case FILEOPS_READ_ERR:  return tr(STR_ERR_READ);
        case FILEOPS_WRITE_ERR: return tr(STR_WRITE_ERROR);
        case FILEOPS_DISK_FULL: return tr(STR_ERR_DISK_FULL);
        case FILEOPS_TOO_DEEP:  return tr(STR_ERR_TREE_DEEP);
        case FILEOPS_PATH_LONG: return tr(STR_ERR_PATH_LONG);
        case FILEOPS_NO_SPACE:  return tr(STR_ERR_NO_SPACE);
        case FILEOPS_COPY_SELF: return tr(STR_ERR_COPY_SELF);
        case FILEOPS_SRC_READ:  return tr(STR_ERR_SRC_READ);
        default:                return NULL;   /* OK / CANCEL: no modal */
    }
}

/* base + "/" + name -> out (cap bytes). Returns false if it wouldn't fit. */
static bool join_path(char *out, size_t cap, const char *base, const char *name) {
    size_t bl = strlen(base), nl = strlen(name);
    int slash = (bl > 0 && base[bl - 1] != '/') ? 1 : 0;
    if (bl + slash + nl + 1 > cap) return false;
    strcpy(out, base);
    if (slash) { out[bl] = '/'; out[bl + 1] = '\0'; }
    strcat(out, name);
    return true;
}

/* --- Cooperative pump for long copy/size operations ---
 * The recursive copy and the pre-flight size walk run inside one blocking call
 * (the main loop is parked in perform_paste), so they keep the UI alive and
 * cancelable themselves: op_poll() refreshes the buttons every iteration (cheap)
 * and latches a B/PWR cancel; op_progress() repaints at ~30 fps so the screen
 * animates without bottlenecking on a full-frame flush per chunk/entry. */
static bool g_op_cancelled = false;
static uint32_t g_op_ui_tick = 0;

static bool op_poll(void) {
    input_update();   /* update_progress_ui() doesn't, so do it here for cancel */
    if (input_just_pressed(INPUT_B) || input_just_pressed(INPUT_PWR)) g_op_cancelled = true;
    return g_op_cancelled;
}

static void op_progress(int pct, const char *title, const char *name) {
    uint32_t now = HAL_GetTick();
    if (now - g_op_ui_tick >= 33) {   /* ~30 fps */
        update_progress_ui(pct, title, name);
        g_op_ui_tick = now;
    }
}

/* Progress/cancel for the shared in-core streaming copy: pump the UI, honor the
 * PWR/B cancel; `user` is the display name. */
static int fb_copy_progress(int pct, void *user) {
    if (op_poll()) return 1;                        /* cancel */
    op_progress(pct, tr(STR_COPYING), (const char *)user);
    return 0;
}

/* Copy a single regular file (drivers already mounted) via the shared in-core copy
 * loop (vfs_copy_open_file). The browser keeps ONLY this basic single-file copy;
 * folder copy / delete go to the fileops module. Returns a FILEOPS_* code. */
static int copy_one_file(vfs_driver_t *sd, const char *sp,
                         vfs_driver_t *dd, const char *dp,
                         uint32_t size, const char *disp) {
    switch (vfs_copy_open_file(sd, sp, dd, dp, size, fb_copy_progress, (void *)disp)) {
        case VFS_COPY_OK:     return FILEOPS_OK;
        case VFS_COPY_CANCEL: return FILEOPS_CANCEL;
        case VFS_COPY_FULL:   return FILEOPS_DISK_FULL;
        case VFS_COPY_READ:   return FILEOPS_READ_ERR;
        case VFS_COPY_WRITE:  return FILEOPS_WRITE_ERR;
        default:              return FILEOPS_OPEN_ERR;
    }
}

/* --- fileops module seam ---------------------------------------------------
 * The heavy tree ops (recursive copy, delete, the size walk) live in
 * /modules/fileops.bin. The core hands the module already-mounted drivers + a
 * progress/cancel UI seam; the translated strings + the single in-core 4 KiB copy
 * buffer stay here. */
static void fileops_progress(int pct, int phase, const char *name, int count) {
    const char *title = (phase == FILEOPS_PHASE_COPY)   ? tr(STR_COPYING)
                      : (phase == FILEOPS_PHASE_DELETE) ? tr(STR_DELETING)
                      :                                   tr(STR_CALCULATING);
    char namebuf[24];
    if (phase == FILEOPS_PHASE_CALC) {   /* "File N" running count (no real % yet) */
        str_fmt1_int(namebuf, sizeof(namebuf), tr(STR_FILE_N), count);
        name = namebuf;
    }
    op_progress(pct, title, name);
}
static int fileops_poll(void) { return op_poll() ? 1 : 0; }

static const fileops_host_t g_fileops_host = {
    .get_tick       = HAL_GetTick,
    .copy_open_file = vfs_copy_open_file,
    .poll           = fileops_poll,
    .progress       = fileops_progress,
};

/* Transiently load /modules/fileops.bin, run one op, reclaim the slot. The RW
 * driver(s) the op writes through are already loaded RESIDENT (below this mark) by
 * the caller, so the reset can't free a driver the vfs still points at. */
static int run_fileops_copy(vfs_driver_t *src, const char *sp, vfs_driver_t *dst,
                            const char *dp, int is_dir, uint32_t size, int same_vol) {
    uint32_t mark = mod_pool_mark();
    fileops_api_t api = {0};
    int res = FILEOPS_OPEN_ERR;   /* module missing/invalid -> a generic error modal */
    if (mod_load_fileops("/modules/fileops.bin", &g_fileops_host, &api) && api.copy) {
        /* mod_load_fileops scans the filesystems (SD first) to find the image, which
         * leaves the active src/dst mounts disturbed -- so a later mkdir/write hits the
         * wrong (or no) volume. RE-MOUNT both AFTER the load, just before the op. */
        if (copy_src_partition && src->mount)
            src->mount(copy_src_partition->address, copy_src_partition->size);
        if (active_partition && active_partition != copy_src_partition && dst->mount)
            dst->mount(active_partition->address, active_partition->size);
        res = api.copy(&g_fileops_host, src, sp, dst, dp, is_dir, size, same_vol);
    }
    mod_pool_reset(mark);
    return res;
}
static int run_fileops_del(vfs_driver_t *drv, const char *path, int is_dir) {
    uint32_t mark = mod_pool_mark();
    fileops_api_t api = {0};
    int res = FILEOPS_WRITE_ERR;
    if (mod_load_fileops("/modules/fileops.bin", &g_fileops_host, &api) && api.del) {
        /* The module load disturbed the active mount (see run_fileops_copy); re-mount. */
        if (active_partition && drv->mount)
            drv->mount(active_partition->address, active_partition->size);
        res = api.del(&g_fileops_host, drv, path, is_dir);
    }
    mod_pool_reset(mark);
    return res;
}

#define ACT_CANCEL 0
#define ACT_VIEW   1
#define ACT_COPY   2
#define ACT_PASTE  3
#define ACT_DELETE 4

static int context_actions[8];
static const char *g_file_opts[8];
static int g_file_opts_count = 0;

/* Delete the selected file/folder. ALL delete (single file or recursive tree) runs
 * in the transient fileops module; the in-core browser keeps no delete code. The
 * active partition's RW driver is already loaded RESIDENT (mount_tab_partition), so
 * it sits below the fileops pool mark. */
static void perform_delete(void) {
    int sel_idx = g_list_browser.selected;
    if (sel_idx < 0 || sel_idx >= file_count) return;

    static char path_to_del[512];
    join_path(path_to_del, sizeof(path_to_del), current_path, fb_name(sel_idx));

    bool is_dir = (file_entries[sel_idx].type == BROWSER_TYPE_DIR);

    const char *driver_name = partition_driver_name(active_partition);
    vfs_driver_t *drv = driver_name ? vfs_get_driver(driver_name) : NULL;
    if (!drv || !drv->unlink) {
        ui_show_error(tr(STR_ERR_DELETE_UNSUPPORTED));
        return;
    }

    ui_operation_in_progress = true;
    g_op_cancelled = false;
    g_op_ui_tick = 0;

    int res = run_fileops_del(drv, path_to_del, is_dir);

    ui_operation_in_progress = false;

    scan_files();
    g_list_browser.num_items = file_count;
    if (g_list_browser.selected >= file_count) g_list_browser.selected = file_count - 1;
    if (g_list_browser.selected < 0) g_list_browser.selected = 0;

    if (res != FILEOPS_OK && res != FILEOPS_CANCEL) ui_show_error(tr(STR_ERR_DELETE_FAILED));
}

static void perform_paste(void) {
    if (!copy_src_partition || !active_partition) return;

    static char dst_path[512];
    join_path(dst_path, sizeof(dst_path), current_path, copy_src_name);

    ui_operation_in_progress = true;
    g_op_cancelled = false;
    g_op_ui_tick = 0;

    char src_t0 = copy_src_partition->type[0];
    char src_t1 = copy_src_partition->type[1];

    /* Ensure the source RW driver is loaded RESIDENT (it sits below any fileops
     * mark, so the transient reclaim can't free a driver the vfs still uses). */
    if (src_t0 == 'F' && src_t1 == 'A') {
        if (vfs_get_driver("FAT") == NULL)
            vfs_load_dynamic_driver("FAT", "/fs/fat.bin");
    } else if (src_t0 == 'L') {
        if (!vfs_is_lfs_rw_loaded())
            vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    }

    const char *src_driver_name = partition_driver_name(copy_src_partition);
    vfs_driver_t *src_drv = src_driver_name ? vfs_get_driver(src_driver_name) : NULL;
    const char *dst_driver_name = partition_driver_name(active_partition);
    vfs_driver_t *dst_drv = dst_driver_name ? vfs_get_driver(dst_driver_name) : NULL;

    if (!src_drv || !dst_drv) {
        ui_show_error(tr(STR_ERR_DRIVER_NA));
        ui_operation_in_progress = false;
        return;
    }
    /* A directory paste needs the target to support mkdir (RW module loaded). */
    if (copy_src_is_dir && !dst_drv->mkdir) {
        ui_show_error(tr(STR_ERR_READ_ONLY));
        ui_operation_in_progress = false;
        return;
    }

    bool need_src_mount = (copy_src_partition != active_partition);
    if (need_src_mount) {
        if (src_drv->mount(copy_src_partition->address, copy_src_partition->size) != 0) {
            ui_show_error(tr(STR_ERR_MOUNT_SRC));
            ui_operation_in_progress = false;
            return;
        }
    }

    int res;
    if (copy_src_is_dir) {
        /* Folder: the fileops module runs the self-copy guard, free-space pre-flight
         * (tree walk) and the recursive copy. */
        res = run_fileops_copy(src_drv, copy_src_path, dst_drv, dst_path, 1,
                               copy_src_size, copy_src_partition == active_partition);
    } else {
        /* Basic single-file copy stays in-core (the shared 4 KiB loop), with a
         * simple free-space check since the file size is known. */
        uint32_t dtot = 0, dfree = 0;
        if (dst_drv->statfs && dst_drv->statfs(&dtot, &dfree) == 0 &&
            (uint64_t)copy_src_size > (uint64_t)dfree * 512u) {
            res = FILEOPS_NO_SPACE;
        } else {
            res = copy_one_file(src_drv, copy_src_path, dst_drv, dst_path,
                                copy_src_size, copy_src_name);
        }
    }

    if (need_src_mount) src_drv->unmount();
    ui_operation_in_progress = false;

    scan_files();
    g_list_browser.num_items = file_count;

    const char *msg = cp_msg(res);
    if (msg) ui_show_error(msg);   /* real failures only (not OK / user-cancel) */
}

static void file_context_menu_callback(int index) {
    if (index < 0 || index >= g_file_opts_count) return;
    int action = context_actions[index];
    int sel_idx = g_list_browser.selected;
    
    if (action == ACT_COPY) {
        copy_src_partition = active_partition;
        join_path(copy_src_path, sizeof(copy_src_path), current_path, fb_name(sel_idx));
        strcpy(copy_src_name, fb_name(sel_idx));
        copy_src_size = file_entries[sel_idx].size;
        copy_src_is_dir = (file_entries[sel_idx].type == BROWSER_TYPE_DIR);
    } else if (action == ACT_PASTE) {
        perform_paste();
    } else if (action == ACT_DELETE) {
        bool is_dir = (sel_idx >= 0 && sel_idx < file_count &&
                       file_entries[sel_idx].type == BROWSER_TYPE_DIR);
        ui_show_confirm(is_dir ? tr(STR_DELETE_DIR_CONFIRM) : tr(STR_DELETE_FILE_CONFIRM), perform_delete);
    }
}

static void open_file_context_menu(void) {
    int sel_idx = g_list_browser.selected;
    bool is_parent_link = (sel_idx >= 0 && sel_idx < file_count && strcmp(fb_name(sel_idx), PARENT_DIR_DOTS) == 0);
    g_file_opts_count = 0;
    bool is_rw = false;
    if (active_partition) {
        char t0 = active_partition->type[0];
        char t1 = active_partition->type[1];
        if (t0 == 'L') {
            /* LittleFS: the in-core RO driver has non-NULL write/unlink stubs that
             * just error, so gate on whether the RW module is actually loaded. */
            is_rw = vfs_is_lfs_rw_loaded();
        } else if (t0 == 'F' && t1 == 'A') {
            /* FAT: in-core driver is RO (write/unlink NULL); a loaded fatfs_rw
             * module would provide them. (FrogFS, like LFS, has stub write/unlink
             * so it stays RO here by falling through.) */
            vfs_driver_t *drv = vfs_get_driver("FAT");
            is_rw = (drv && drv->write && drv->unlink);
        }
    }
    bool can_paste = (copy_src_partition != NULL && is_rw);
    
    /* The config gates each op (a picker disables them all -> only CANCEL shows). */
    if (file_count > 0) {
        /* COPY works on files AND directories (a directory copies recursively). */
        if (!is_parent_link && g_fb_cfg.allow_copy) {
            g_file_opts[g_file_opts_count] = tr(STR_COPY);
            context_actions[g_file_opts_count++] = ACT_COPY;
        }

        if (can_paste && g_fb_cfg.allow_paste) {
            g_file_opts[g_file_opts_count] = tr(STR_PASTE);
            context_actions[g_file_opts_count++] = ACT_PASTE;
        }

        if (!is_parent_link && is_rw && g_fb_cfg.allow_delete) {
            g_file_opts[g_file_opts_count] = tr(STR_DELETE);
            context_actions[g_file_opts_count++] = ACT_DELETE;
        }
    } else {
        if (can_paste && g_fb_cfg.allow_paste) {
            g_file_opts[g_file_opts_count] = tr(STR_PASTE);
            context_actions[g_file_opts_count++] = ACT_PASTE;
        }
    }

    g_file_opts[g_file_opts_count] = tr(STR_CANCEL);
    context_actions[g_file_opts_count++] = ACT_CANCEL;

    ui_show_context_menu(tr(STR_OPTIONS), g_file_opts, g_file_opts_count, file_context_menu_callback);
}

static void menu_browser_exit(ui_window_t *self) {
    save_tab_state();
    for (int i = 0; i < 2; i++) tab_unmount(&g_tabs[i]);
    fb_cfg_reset_default();   /* a picker session leaves the browser back at full ops */
}

/* Open the file browser normally (full file ops). The Tools menu entry calls this. */
void browser_open(void) {
    fb_cfg_reset_default();
    ui_push(&PAGE_BROWSER);
}

/* Open the browser as a stripped-down PICKER: no file ops, only files matching `ext`
 * shown (NULL/"" = all), and choosing a file calls on_pick(path,false). Reuses the
 * whole browser engine; a feature module selects a file/folder this way with its own
 * callback, leaving the main browser's code (not yet a separate instance) intact. */
void browser_open_picker(const char *ext, void (*on_pick)(const char *path, bool is_dir, uint32_t size)) {
    /* Snapshot the (possibly live) main-browser state so the picker can't corrupt it. */
    save_tab_state();
    memcpy(g_picker_save.tabs, g_tabs, sizeof(g_tabs));
    g_picker_save.active_tab = g_active_tab;
    g_picker_save.cfg = g_fb_cfg;
    g_picker_saved = true;

    fb_cfg_reset_default();
    g_fb_cfg.allow_copy = g_fb_cfg.allow_paste = g_fb_cfg.allow_delete = false;
    g_fb_cfg.pick_mode = true;
    g_fb_cfg.on_pick = on_pick;
    if (ext) str_lcpy(g_fb_cfg.ext_filter, sizeof(g_fb_cfg.ext_filter), ext);
    ui_push(&PAGE_BROWSER);
}

/* Restore the main browser after a nested picker closed (the picker shares this instance
 * and its exit unmounts + resets). Re-mounts + re-scans the saved active tab. Call once
 * after the picker page has popped. No-op if no picker was active. */
void browser_picker_restore(void) {
    if (!g_picker_saved) return;
    g_picker_saved = false;
    memcpy(g_tabs, g_picker_save.tabs, sizeof(g_tabs));
    g_active_tab = g_picker_save.active_tab;
    g_fb_cfg = g_picker_save.cfg;
    load_tab_state();           /* re-mount + re-scan the restored active tab */
}

/* The partition currently open in the browser (the picked file's filesystem right after a
 * pick, before browser_picker_restore). Lets a picker caller learn where to read the file.
 * Returned opaque (void*) to keep ui.h free of partition.h. */
const void *browser_active_partition(void) { return active_partition; }

ui_window_t PAGE_BROWSER = {
    .title = "FILE BROWSER",
    .x = 0, .y = 22, .w = 320, .h = 196,
    .is_modal = 0,
    .show_footer = 1,
    .allow_idle_hide = 0,
    .enter = menu_browser_enter,
    .draw_content = menu_browser_draw,
    .update_content = menu_browser_update,
    .exit = menu_browser_exit
};
