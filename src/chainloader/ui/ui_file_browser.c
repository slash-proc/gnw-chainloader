#include "ui.h"
#include "ui_list.h"
#include "storage/vfs.h"
#include "partition.h"
#include "input.h"
#include "gui.h"
#include "utils.h"
#include "board.h"
#include "strings.h"
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

static void menu_browser_back(void);
static void open_file_context_menu(void);
static void scan_files(void);
static void load_tab_state(void);
static void save_tab_state(void);
static void update_browser_title(void);
static void mount_tab_partition(browser_tab_t *tab);

static void update_browser_title(void) {
    char tab_indicator[16];
    if (g_active_tab == 0) {
        tab_indicator[0] = '\0';
    } else {
        strcpy(tab_indicator, gui_rtl ? "> " : "< ");   /* LEFT/RIGHT swap in RTL */
    }

    if (!active_partition) {
        strcpy(browser_title, tab_indicator);
        strcat(browser_title, tr(STR_SELECT_FS));
        if (g_active_tab == 0) {
            strcat(browser_title, gui_rtl ? " <" : " >");
        }
        PAGE_BROWSER.title = browser_title;
        return;
    }

    strcpy(browser_title, tab_indicator);
    {
        /* FS tag: <CODE>-<NN>, NN = 2-digit 1-based per-type index. e.g. "LFS-01/".
         * The RO/RW distinction lives in the FS-list properties pane ("MODE."),
         * not here. */
        char t0 = active_partition->type[0];
        char t1 = active_partition->type[1];
        const char *code = partition_fs_code(active_partition);
        if (!code) code = "FS";
        strcat(browser_title, code);

        /* Per-type instance index (1-based): count same-type filesystems before this one. */
        int fs_index = 0;
        int pcount = partition_get_count();
        for (int i = 0; i < pcount; i++) {
            partition_info_t *q = partition_get_info(i);
            if (q == active_partition) break;
            if (q && q->type[0] == t0 && q->type[1] == t1) fs_index++;
        }
        char idx_buf[8];
        int_to_str(fs_index + 1, idx_buf);
        strcat(browser_title, "-");
        if (idx_buf[1] == '\0') strcat(browser_title, "0"); /* zero-pad to 2 digits */
        strcat(browser_title, idx_buf);
    }
    strcat(browser_title, ":");   /* delimit the FS root, e.g. "FAT-01:/" */
    strcat(browser_title, current_path);
    int len = strlen(browser_title);
    if (len > 0 && browser_title[len - 1] != '/') {
        strcat(browser_title, "/");
    }
    if (g_active_tab == 0) {
        strcat(browser_title, gui_rtl ? " <" : " >");
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

static void add_file_entry(const char *name, uint8_t type, uint32_t size) {
    /* Hide dot-entries entirely (".", "..", and dotfiles/folders such as the
     * .Trashes / .Spotlight-V100 / .fseventsd / ._* noise on PC-formatted SD
     * cards). The parent-nav ".." is added explicitly by scan_files. */
    if (name[0] == '.') return;
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

    const int pw = 110;
    int px = gui_mirror_x(198, pw, 0, SCREEN_WIDTH);   /* detail column mirrors left in RTL */
    gui_draw_text_aligned(px, 40, pw, tr(STR_LBL_TYPE), COLOR_FG, false, 0);
    const char *ext = "";
    if (file_entries[selected_idx].type == BROWSER_TYPE_DIR) {
        ext = tr(STR_DIR);
    } else {
        const char *name = fb_name(selected_idx);
        const char *dot = NULL;
        for (const char *p = name; *p; p++) {
            if (*p == '.') dot = p;
        }
        if (dot && dot != name) {
            ext = dot + 1;
        } else {
            ext = tr(STR_FILE);
        }
    }
    gui_draw_text_aligned(px, 55, pw, ext, COLOR_FG, false, 0);

    gui_draw_text_aligned(px, 75, pw, tr(STR_LBL_SIZE), COLOR_FG, false, 0);
    if (file_entries[selected_idx].type == BROWSER_TYPE_DIR) {
        gui_draw_text_aligned(px, 90, pw, "-", COLOR_FG, false, 0);
    } else {
        char size_buf[16];
        format_size(file_entries[selected_idx].size, size_buf);
        gui_draw_text_aligned(px, 90, pw, size_buf, COLOR_FG, false, 0);
    }
}

static void fs_list_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (selected_idx < 0 || selected_idx >= fs_partition_count) return;
    (void)selected_tick;
    partition_info_t *p = fs_partitions[selected_idx];
    char buf[32];   /* holds a size string or a translated "UNKNOWN" (3 bytes/char) */
    const int pw = 110;
    int px = gui_mirror_x(198, pw, 0, SCREEN_WIDTH);   /* detail column mirrors left in RTL */

    if (fs_space_valid[selected_idx]) {
        gui_draw_text_aligned(px, 40, pw, tr(STR_LBL_FREE), COLOR_FG, false, 0);
        format_size_sectors(fs_free_space[selected_idx], buf);
        gui_draw_text_aligned(px, 55, pw, buf, COLOR_FG, false, 0);

        gui_draw_text_aligned(px, 75, pw, tr(STR_LBL_USED), COLOR_FG, false, 0);
        format_size_sectors(fs_used_space[selected_idx], buf);
        gui_draw_text_aligned(px, 90, pw, buf, COLOR_FG, false, 0);
    } else {
        /* Free/Used unavailable (read-only FatFs has no f_getfree) — show total
         * capacity instead. SD stores size as sectors (see partition.c). */
        gui_draw_text_aligned(px, 40, pw, tr(STR_LBL_TOTAL), COLOR_FG, false, 0);
        if (p && partition_is_sd(p))  format_size_sectors(p->size, buf);
        else if (p)                   format_size(p->size, buf);
        else                          str_lcpy(buf, sizeof(buf), tr(STR_UNKNOWN));
        gui_draw_text_aligned(px, 55, pw, buf, COLOR_FG, false, 0);
    }

    /* Mode row: the RO/RW distinction (moved here from the title bar). */
    gui_draw_text_aligned(px, 110, pw, tr(STR_LBL_MODE), COLOR_FG, false, 0);
    gui_draw_text_aligned(px, 125, pw, fs_is_rw[selected_idx] ? tr(STR_MODE_RW) : tr(STR_MODE_RO), COLOR_FG, false, 0);
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
        } else {
            open_file_context_menu();
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
                        if (ro_drv->open("/modules/filesystems/lfs_rw.bin", 1, &f) == 0) {
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
                        vfs_module_available("/modules/filesystems/fatfs.bin")) {
                        vfs_load_dynamic_driver("FAT", "/modules/filesystems/fatfs.bin");
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
        if (strcmp(p->type, "LittleFS") == 0) {
            str_lcpy(buf, sizeof(buf), tr(STR_FS_LITTLEFS));
        } else {
            /* Raw FS type code (FAT, Frog, ...) stays literal -- not translatable.
             * Force-upper the raw code only; never the translated label above. */
            int len = strlen(p->type);
            for (int j = 0; j < len; j++) {
                char c = p->type[j];
                buf[j] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
            }
            buf[len] = '\0';
        }
        /* Show "RW" right after the type when the RW module loaded successfully
         * (e.g. "LITTLEFS RW" / "FAT RW"), so it's recognizable at a glance; the
         * MODE pane carries the same RO/RW too. */
        if (fs_is_rw[idx]) { str_lcat(buf, sizeof(buf), " "); str_lcat(buf, sizeof(buf), tr(STR_MODE_RW)); }
        strcat(buf, " @ 0x");
        hex_to_str(p->address, buf + strlen(buf), 8);
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
            g_list_browser.is_split = true;
            g_list_browser.draw_right_pane = browser_draw_right_pane;
            g_list_browser.visible_lines = 9;
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
        g_list_browser.is_split = true;
        g_list_browser.draw_right_pane = fs_list_draw_right_pane;
        g_list_browser.visible_lines = 9;
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
    if (driver_name && driver_name[0] == 'L' && !vfs_is_lfs_rw_loaded()) {
        vfs_load_dynamic_driver("LFS", "/modules/filesystems/lfs_rw.bin");
    } else if (driver_name && driver_name[0] == 'F' && driver_name[1] == 'A' &&
               !vfs_is_fat_rw_loaded()) {
        vfs_load_dynamic_driver("FAT", "/modules/filesystems/fatfs.bin");
    }

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
        g_list_browser.is_split = true;
        g_list_browser.draw_right_pane = fs_list_draw_right_pane;
        g_list_browser.visible_lines = 9;
        g_list_browser.on_back = ui_pop;
    } else {
        ui_list_init(&g_list_browser, browser_title, file_count, get_file_label, on_file_action);
        g_list_browser.is_split = true;
        g_list_browser.draw_right_pane = browser_draw_right_pane;
        g_list_browser.visible_lines = 9;
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
    
    // Switch tabs with LEFT / RIGHT
    if (input_just_pressed(INPUT_LEFT) || input_just_pressed(INPUT_RIGHT)) {
        save_tab_state();
        tab_unmount(&g_tabs[g_active_tab]);
        g_active_tab = 1 - g_active_tab;
        load_tab_state();
        return;
    }
    
    if (g_mode == BROWSER_MODE_NAVIGATE) {
        if (input_just_pressed(INPUT_PAUSE)) {
            open_file_context_menu();
            return;
        }
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
        ui_draw_footer(tr(STR_FOOTER_BROWSER));
        if (file_count == 0) {
            if (mount_res != 0) {
                char buf[64], num[12];
                int_to_str(mount_res, num);
                str_lcpy(buf, sizeof(buf), tr(STR_MOUNT_FAIL));
                str_lcat(buf, sizeof(buf), num);
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

#define MAX_COPY_DEPTH 8        /* recursion guard for folder copy (matches nav stack) */

typedef enum {
    CP_OK = 0, CP_CANCEL, CP_OPEN_ERR, CP_READ_ERR, CP_WRITE_ERR, CP_DISK_FULL, CP_TOO_DEEP, CP_PATH_LONG
} cp_result_t;

static const char *cp_msg(cp_result_t r) {
    switch (r) {
        case CP_OPEN_ERR:  return tr(STR_ERR_OPEN);
        case CP_READ_ERR:  return tr(STR_ERR_READ);
        case CP_WRITE_ERR: return tr(STR_WRITE_ERROR);
        case CP_DISK_FULL: return tr(STR_ERR_DISK_FULL);
        case CP_TOO_DEEP:  return tr(STR_ERR_TREE_DEEP);
        case CP_PATH_LONG: return tr(STR_ERR_PATH_LONG);
        default:           return NULL;   /* CP_OK / CP_CANCEL: no modal */
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
static int g_size_count = 0;   /* files counted so far during the pre-flight walk */

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

/* Read the n-th real entry (skipping "." and "..") of `path` on `drv`, re-opening
 * the directory each call so no handle is held across recursion (the driver dir
 * pools are only 2 deep). Returns 1 if found (ent filled), 0 past the end, -1 on
 * opendir failure. O(n) per call — fine for an occasional, progress-shown copy. */
static int read_nth_entry(vfs_driver_t *drv, const char *path, int n, vfs_dirent_t *ent) {
    void *dir = NULL;
    if (drv->opendir(path, &dir) != 0) return -1;
    int count = 0, found = 0;
    vfs_dirent_t e;
    while (drv->readdir(dir, &e) > 0) {
        if (e.name[0] == '.' && (e.name[1] == '\0' ||
            (e.name[1] == '.' && e.name[2] == '\0'))) continue;   /* skip "." / ".." */
        if (count == n) { *ent = e; found = 1; break; }
        count++;
    }
    if (drv->closedir) drv->closedir(dir);
    return found ? 1 : 0;
}

/* Progress/cancel for the shared in-core streaming copy: pump the UI, honor the
 * PWR/B cancel; `user` is the display name. */
static int fb_copy_progress(int pct, void *user) {
    if (op_poll()) return 1;                        /* cancel */
    op_progress(pct, tr(STR_COPYING), (const char *)user);
    return 0;
}

/* Copy a single regular file (drivers already mounted) via the shared in-core copy
 * loop (vfs_copy_open_file), mapping its result to the browser's cp_result_t. */
static cp_result_t copy_one_file(vfs_driver_t *sd, const char *sp,
                                 vfs_driver_t *dd, const char *dp,
                                 uint32_t size, const char *disp) {
    switch (vfs_copy_open_file(sd, sp, dd, dp, size, fb_copy_progress, (void *)disp)) {
        case VFS_COPY_OK:     return CP_OK;
        case VFS_COPY_CANCEL: return CP_CANCEL;
        case VFS_COPY_FULL:   return CP_DISK_FULL;
        case VFS_COPY_READ:   return CP_READ_ERR;
        case VFS_COPY_WRITE:  return CP_WRITE_ERR;
        default:              return CP_OPEN_ERR;
    }
}

/* Sum the byte sizes of every regular file under `path` (recursively). Sets
 * *ok=false on any error (opendir failure / too deep). No-malloc, pool-safe via
 * read_nth_entry (re-scan per entry). */
static uint64_t tree_size_bytes(vfs_driver_t *drv, const char *path, int depth, bool *ok) {
    if (depth > MAX_COPY_DEPTH) { *ok = false; return 0; }
    uint64_t total = 0;
    vfs_dirent_t ent;
    int n = 0, r;
    while ((r = read_nth_entry(drv, path, n, &ent)) == 1) {
        if (op_poll()) { *ok = false; return total; }   /* cancelable */
        if (ent.type == VFS_TYPE_DIR) {
            char child[512];
            if (!join_path(child, sizeof(child), path, ent.name)) { *ok = false; return total; }
            total += tree_size_bytes(drv, child, depth + 1, ok);
            if (!*ok) return total;
        } else {
            total += ent.size;
            g_size_count++;
            /* "Calculating..." with a live running file count (no real % yet —
             * we don't know the total until we finish counting). */
            char status[24];
            str_fmt1_int(status, sizeof(status), tr(STR_FILE_N), g_size_count);
            op_progress((int)((HAL_GetTick() / 20) % 100), tr(STR_CALCULATING), status);
        }
        n++;
    }
    if (r < 0) *ok = false;
    return total;
}

/* Recursively copy the directory tree at `sp` (on sd) to `dp` (on dd): mkdir the
 * target, then copy each file and recurse into each subdir. Pool-safe (no dir
 * handle is held across the recursive call). */
static cp_result_t copy_tree(vfs_driver_t *sd, const char *sp,
                             vfs_driver_t *dd, const char *dp, int depth) {
    if (depth > MAX_COPY_DEPTH) return CP_TOO_DEEP;
    if (dd->mkdir(dp) != 0) return CP_WRITE_ERR;   /* exists or unwritable */

    vfs_dirent_t ent;
    int n = 0, r;
    while ((r = read_nth_entry(sd, sp, n, &ent)) == 1) {
        if (op_poll()) return CP_CANCEL;
        char sc[512], dc[512];
        if (!join_path(sc, sizeof(sc), sp, ent.name) ||
            !join_path(dc, sizeof(dc), dp, ent.name)) return CP_PATH_LONG;
        cp_result_t res = (ent.type == VFS_TYPE_DIR)
            ? copy_tree(sd, sc, dd, dc, depth + 1)
            : copy_one_file(sd, sc, dd, dc, ent.size, ent.name);
        if (res != CP_OK) return res;
        n++;
    }
    return (r < 0) ? CP_READ_ERR : CP_OK;
}

#define ACT_CANCEL 0
#define ACT_VIEW   1
#define ACT_COPY   2
#define ACT_PASTE  3
#define ACT_DELETE 4

static int context_actions[8];
static const char *g_file_opts[8];
static int g_file_opts_count = 0;

/* Recursively delete a directory and everything under it: empty it depth-first
 * (unlink files, recurse into subdirs), then remove the now-empty dir itself —
 * lfs_remove / f_unlink both refuse a non-empty directory. Pool-safe (no dir
 * handle held across recursion) and cancelable. Always re-reads entry 0 because
 * the listing shrinks as we delete. */
static cp_result_t delete_tree(vfs_driver_t *drv, const char *path, int depth) {
    if (depth > MAX_COPY_DEPTH) return CP_TOO_DEEP;

    vfs_dirent_t ent;
    int r;
    while ((r = read_nth_entry(drv, path, 0, &ent)) == 1) {
        if (op_poll()) return CP_CANCEL;
        char child[512];
        if (!join_path(child, sizeof(child), path, ent.name)) return CP_PATH_LONG;
        if (ent.type == VFS_TYPE_DIR) {
            cp_result_t res = delete_tree(drv, child, depth + 1);
            if (res != CP_OK) return res;
        } else {
            op_progress((int)((HAL_GetTick() / 20) % 100), tr(STR_DELETING), ent.name);
            if (drv->unlink(child) != 0) return CP_WRITE_ERR;   /* bail (avoids re-reading the same entry forever) */
        }
    }
    if (r < 0) return CP_READ_ERR;
    return (drv->unlink(path) == 0) ? CP_OK : CP_WRITE_ERR;   /* remove the now-empty dir */
}

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

    cp_result_t res;
    if (is_dir) {
        res = delete_tree(drv, path_to_del, 0);     /* recursive (handles non-empty) */
    } else {
        res = (drv->unlink(path_to_del) == 0) ? CP_OK : CP_WRITE_ERR;
    }

    ui_operation_in_progress = false;

    scan_files();
    g_list_browser.num_items = file_count;
    if (g_list_browser.selected >= file_count) g_list_browser.selected = file_count - 1;
    if (g_list_browser.selected < 0) g_list_browser.selected = 0;

    if (res != CP_OK && res != CP_CANCEL) ui_show_error(tr(STR_ERR_DELETE_FAILED));
}

static void perform_paste(void) {
    if (!copy_src_partition || !active_partition) return;
    
    static char dst_path[512];
    join_path(dst_path, sizeof(dst_path), current_path, copy_src_name);

    ui_operation_in_progress = true;
    g_op_cancelled = false;
    g_op_ui_tick = 0;
    g_size_count = 0;

    char src_t0 = copy_src_partition->type[0];
    char src_t1 = copy_src_partition->type[1];
    
    // Ensure source driver is loaded
    if (src_t0 == 'F' && src_t1 == 'A') {
        if (vfs_get_driver("FAT") == NULL) {
            vfs_load_dynamic_driver("FAT", "/modules/filesystems/fatfs.bin");
        }
    } else if (src_t0 == 'L') {
        if (!vfs_is_lfs_rw_loaded()) {
            vfs_load_dynamic_driver("LFS", "/modules/filesystems/lfs_rw.bin");
        }
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

    /* Guard against pasting a folder into itself or a descendant (same volume):
     * copy_tree would recurse into the copy it's writing and run away. Only a
     * concern when source and destination are the same partition. */
    if (copy_src_is_dir && copy_src_partition == active_partition) {
        size_t sl = strlen(copy_src_path);
        if (strncmp(dst_path, copy_src_path, sl) == 0 &&
            (dst_path[sl] == '/' || dst_path[sl] == '\0')) {
            ui_show_error(tr(STR_ERR_COPY_SELF));
            ui_operation_in_progress = false;
            return;
        }
    }

    bool need_src_mount = (copy_src_partition != active_partition);
    if (need_src_mount) {
        if (src_drv->mount(copy_src_partition->address, copy_src_partition->size) != 0) {
            ui_show_error(tr(STR_ERR_MOUNT_SRC));
            ui_operation_in_progress = false;
            return;
        }
    }

    /* Pre-flight free-space check: refuse upfront (all-or-nothing) rather than
     * writing partial data and aborting mid-stream. For a folder, sum the whole
     * tree. statfs reports 512-byte sectors (overflow-safe). Skipped if the
     * driver can't report free space (the mid-write DISK FULL guard still fires).*/
    uint32_t dtot = 0, dfree = 0;
    if (dst_drv->statfs && dst_drv->statfs(&dtot, &dfree) == 0) {
        uint64_t need = copy_src_size;
        if (copy_src_is_dir) {
            bool ok = true;
            need = tree_size_bytes(src_drv, copy_src_path, 0, &ok);
            if (!ok) {
                if (need_src_mount) src_drv->unmount();
                if (!g_op_cancelled) ui_show_error(tr(STR_ERR_SRC_READ));  /* silent on cancel */
                ui_operation_in_progress = false;
                return;
            }
        }
        if (need > (uint64_t)dfree * 512u) {
            if (need_src_mount) src_drv->unmount();
            ui_show_error(tr(STR_ERR_NO_SPACE));
            ui_operation_in_progress = false;
            return;
        }
    }

    cp_result_t res;
    if (copy_src_is_dir) {
        res = copy_tree(src_drv, copy_src_path, dst_drv, dst_path, 0);
    } else {
        res = copy_one_file(src_drv, copy_src_path, dst_drv, dst_path,
                            copy_src_size, copy_src_name);
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
    
    if (file_count > 0) {
        /* COPY works on files AND directories (a directory copies recursively). */
        if (!is_parent_link) {
            g_file_opts[g_file_opts_count] = tr(STR_COPY);
            context_actions[g_file_opts_count++] = ACT_COPY;
        }

        if (can_paste) {
            g_file_opts[g_file_opts_count] = tr(STR_PASTE);
            context_actions[g_file_opts_count++] = ACT_PASTE;
        }

        if (!is_parent_link && is_rw) {
            g_file_opts[g_file_opts_count] = tr(STR_DELETE);
            context_actions[g_file_opts_count++] = ACT_DELETE;
        }
    } else {
        if (can_paste) {
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
}

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
