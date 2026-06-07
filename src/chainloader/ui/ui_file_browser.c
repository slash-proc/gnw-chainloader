#include "ui.h"
#include "ui_list.h"
#include "storage/vfs.h"
#include "partition.h"
#include "input.h"
#include "gui.h"
#include "utils.h"
#include "board.h"
#include <string.h>

extern ui_window_t PAGE_BROWSER;

#define BROWSER_TYPE_FILE 0
#define BROWSER_TYPE_DIR  1

typedef enum {
    BROWSER_MODE_SCANNING,
    BROWSER_MODE_FS_LIST,
    BROWSER_MODE_NAVIGATE
} browser_mode_t;

typedef struct {
    partition_info_t* active_partition;
    char current_path[96];
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
static uint32_t fs_total_space[16];
static uint32_t fs_used_space[16];
static uint32_t fs_free_space[16];
static bool fs_space_valid[16];
static partition_info_t* active_partition = NULL;

static char file_names[16][48];
static uint8_t file_types[16];
static uint32_t file_sizes[16];
static int file_count = 0;
static int mount_res = 0;
static char current_path[96] = "/";

static char browser_title[96];

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
        strcpy(tab_indicator, "< ");
    }

    if (!active_partition) {
        strcpy(browser_title, tab_indicator);
        strcat(browser_title, "SELECT FS");
        if (g_active_tab == 0) {
            strcat(browser_title, " >");
        }
        PAGE_BROWSER.title = browser_title;
        return;
    }

    strcpy(browser_title, tab_indicator);
    if (strcmp(active_partition->type, "LittleFS") == 0) {
        if (vfs_is_lfs_rw_loaded()) {
            strcat(browser_title, "LITTLEFS RW");
        } else {
            strcat(browser_title, "LITTLEFS");
        }
    } else {
        int start = strlen(browser_title);
        int len = strlen(active_partition->type);
        for (int j = 0; j < len; j++) {
            char c = active_partition->type[j];
            browser_title[start + j] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
        }
        browser_title[start + len] = '\0';
    }
    strcat(browser_title, current_path);
    int len = strlen(browser_title);
    if (len > 0 && browser_title[len - 1] != '/') {
        strcat(browser_title, "/");
    }
    if (g_active_tab == 0) {
        strcat(browser_title, " >");
    }
    PAGE_BROWSER.title = browser_title;
}

static void add_file_entry(const char *name, uint8_t type, uint32_t size) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return;
    if (file_count < 16) {
        file_types[file_count] = type;
        file_sizes[file_count] = (type == BROWSER_TYPE_DIR) ? 0 : size;
        strncpy(file_names[file_count], name, 47);
        file_names[file_count][47] = '\0';
        file_count++;
    }
}

static void scan_files(void) {
    file_count = 0;
    if (!active_partition) return;

    if (strcmp(current_path, "/") != 0 && current_path[0] != '\0') {
        file_types[0] = BROWSER_TYPE_DIR;
        file_sizes[0] = 0;
        strcpy(file_names[0], "..");
        file_count = 1;
    }

    char t0 = active_partition->type[0];
    char t1 = active_partition->type[1];
    
    const char *driver_name = NULL;
    if (t0 == 'L') driver_name = "LFS";
    else if (t0 == 'F' && t1 == 'r') driver_name = "FROGFS";
    else if (t0 == 'F' && t1 == 'A') driver_name = "FAT";
    
    vfs_driver_t *drv = vfs_get_driver(driver_name);
    if (!drv) return;
    
    void *dir_ctx = NULL;
    if (drv->opendir(current_path, &dir_ctx) == 0) {
        vfs_dirent_t ent;
        while (drv->readdir(dir_ctx, &ent) > 0) {
            add_file_entry(ent.name, (ent.type == VFS_TYPE_DIR) ? BROWSER_TYPE_DIR : BROWSER_TYPE_FILE, ent.size);
        }
        drv->closedir(dir_ctx);
    }
}

static const char* get_file_label(int idx) {
    return file_names[idx];
}

static void browser_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (selected_idx < 0 || selected_idx >= file_count) return;

    (void)selected_tick;

    gui_draw_text(198, 40, "TYPE.", COLOR_FG);
    const char *ext = "";
    if (file_types[selected_idx] == BROWSER_TYPE_DIR) {
        ext = "DIR";
    } else {
        const char *name = file_names[selected_idx];
        const char *dot = NULL;
        for (const char *p = name; *p; p++) {
            if (*p == '.') dot = p;
        }
        if (dot && dot != name) {
            ext = dot + 1;
        } else {
            ext = "FILE";
        }
    }
    gui_draw_text(198, 55, ext, COLOR_FG);

    gui_draw_text(198, 75, "SIZE.", COLOR_FG);
    if (file_types[selected_idx] == BROWSER_TYPE_DIR) {
        gui_draw_text(198, 90, "-", COLOR_FG);
    } else {
        char size_buf[16];
        format_size(file_sizes[selected_idx], size_buf);
        gui_draw_text(198, 90, size_buf, COLOR_FG);
    }
}

static void fs_list_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (selected_idx < 0 || selected_idx >= fs_partition_count) return;
    (void)selected_tick;
    
    gui_draw_text(198, 40, "FREE.", COLOR_FG);
    if (fs_space_valid[selected_idx]) {
        char buf[16];
        format_size(fs_free_space[selected_idx], buf);
        gui_draw_text(198, 55, buf, COLOR_FG);
    } else {
        gui_draw_text(198, 55, "UNKNOWN", COLOR_FG);
    }

    gui_draw_text(198, 75, "USED.", COLOR_FG);
    if (fs_space_valid[selected_idx]) {
        char buf[16];
        format_size(fs_used_space[selected_idx], buf);
        gui_draw_text(198, 90, buf, COLOR_FG);
    } else {
        gui_draw_text(198, 90, "-", COLOR_FG);
    }
}

static void on_file_action(int idx) {
    if (idx >= 0 && idx < file_count) {
        if (strcmp(file_names[idx], "..") == 0) {
            menu_browser_back();
            return;
        }
        if (file_types[idx] == BROWSER_TYPE_DIR) {
            int len = strlen(current_path);
            if (len > 1 && current_path[len - 1] != '/') {
                current_path[len++] = '/';
            }
            strncpy(current_path + len, file_names[idx], sizeof(current_path) - len - 1);
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
                        if (ro_drv->open("/drivers/fs/lfs_rw.bin", 1, &f) == 0) {
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
                        fs_total_space[fs_partition_count] = p->size;
                        fs_used_space[fs_partition_count] = bin_sz;
                        fs_free_space[fs_partition_count] = p->size > bin_sz ? (p->size - bin_sz) : 0;
                        fs_space_valid[fs_partition_count] = true;
                    }
                } else if (t0 == 'F' && t1 == 'A') {
                    fs_is_rw[fs_partition_count] = true;
                    // For FAT, temporarily load driver and mount to query
                    if (vfs_get_driver("FAT") == NULL) {
                        vfs_load_dynamic_driver("FAT", "/drivers/fs/fatfs.bin");
                    }
                    vfs_driver_t *fat_drv = vfs_get_driver("FAT");
                    if (fat_drv && fat_drv->mount(p->address, p->size) == 0) {
                        uint32_t tot = 0, fre = 0;
                        if (fat_drv->statfs && fat_drv->statfs(&tot, &fre) == 0) {
                            fs_total_space[fs_partition_count] = tot;
                            fs_used_space[fs_partition_count] = tot - fre;
                            fs_free_space[fs_partition_count] = fre;
                            fs_space_valid[fs_partition_count] = true;
                        }
                        fat_drv->unmount();
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
            if (fs_is_rw[idx]) {
                strcpy(buf, "LITTLEFS RW");
            } else {
                strcpy(buf, "LITTLEFS");
            }
        } else {
            int len = strlen(p->type);
            for (int j = 0; j < len; j++) {
                char c = p->type[j];
                buf[j] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
            }
            buf[len] = '\0';
        }
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
            char t0 = tab->active_partition->type[0];
            char t1 = tab->active_partition->type[1];
            const char *drv_name = (t0 == 'L') ? "LFS" : ((t0 == 'F' && t1 == 'r') ? "FROGFS" : "FAT");
            vfs_driver_t *drv = vfs_get_driver(drv_name);
            if (drv && tab->mount_res == 0) {
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
    char t0 = tab->active_partition->type[0];
    char t1 = tab->active_partition->type[1];
    
    const char *driver_name = NULL;
    if (t0 == 'L') {
        driver_name = "LFS";
        if (!vfs_is_lfs_rw_loaded()) {
            vfs_load_dynamic_driver("LFS", "/drivers/fs/lfs_rw.bin");
        }
    } else if (t0 == 'F' && t1 == 'r') {
        driver_name = "FROGFS";
    } else if (t0 == 'F' && t1 == 'A') {
        driver_name = "FAT";
        if (vfs_get_driver("FAT") == NULL) {
            vfs_load_dynamic_driver("FAT", "/drivers/fs/fatfs.bin");
        }
    }
    
    vfs_driver_t *drv = vfs_get_driver(driver_name);
    if (drv) {
        tab->mount_res = drv->mount(tab->active_partition->address, tab->active_partition->size);
    } else {
        tab->mount_res = -1;
    }
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
    if (partition_scan_get_state() == PARTITION_SCAN_IN_PROGRESS) {
        g_mode = BROWSER_MODE_SCANNING;
        ui_list_init(&g_list_browser, "FILE BROWSER", 0, fs_list_get_label, fs_list_on_action);
        g_list_browser.on_back = ui_pop;
    } else {
        init_tabs_if_needed();
        g_active_tab = 0;
        load_tab_state();
    }
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
        browser_tab_t *old_tab = &g_tabs[g_active_tab];
        if (old_tab->active_partition) {
            char old_t0 = old_tab->active_partition->type[0];
            char old_t1 = old_tab->active_partition->type[1];
            const char *old_drv_name = (old_t0 == 'L') ? "LFS" : ((old_t0 == 'F' && old_t1 == 'r') ? "FROGFS" : "FAT");
            vfs_driver_t *old_drv = vfs_get_driver(old_drv_name);
            if (old_drv && old_tab->mount_res == 0) {
                old_drv->unmount();
            }
        }
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
            gui_draw_text(20, 60, "NO FILESYSTEMS FOUND", COLOR_RED);
        }
    } else if (g_mode == BROWSER_MODE_NAVIGATE) {
        ui_draw_footer("PAUSE: OPTS   A: SEL");
        if (file_count == 0) {
            if (mount_res != 0) {
                char buf[32];
                strcpy(buf, "MOUNT FAIL: ");
                int_to_str(mount_res, buf + strlen(buf));
                gui_draw_text(20, 60, buf, COLOR_RED);
            } else {
                gui_draw_text(20, 60, "EMPTY DIRECTORY", COLOR_RED);
            }
        }
    }
}

static partition_info_t* copy_src_partition = NULL;
static char copy_src_path[128];
static char copy_src_name[64];
static uint32_t copy_src_size = 0;

#define ACT_CANCEL 0
#define ACT_VIEW   1
#define ACT_COPY   2
#define ACT_PASTE  3
#define ACT_DELETE 4

static int context_actions[8];
static const char *g_file_opts[8];
static int g_file_opts_count = 0;

static void perform_delete(void) {
    int sel_idx = g_list_browser.selected;
    if (sel_idx < 0 || sel_idx >= file_count) return;
    
    static char path_to_del[128];
    strcpy(path_to_del, current_path);
    int len = strlen(path_to_del);
    if (len > 1 && path_to_del[len - 1] != '/') {
        strcat(path_to_del, "/");
    }
    strcat(path_to_del, file_names[sel_idx]);
    
    char t0 = active_partition->type[0];
    char t1 = active_partition->type[1];
    
    const char *driver_name = (t0 == 'L') ? "LFS" : ((t0 == 'F' && t1 == 'r') ? "FROGFS" : "FAT");
    vfs_driver_t *drv = vfs_get_driver(driver_name);
    
    if (drv && drv->unlink) {
        if (drv->unlink(path_to_del) == 0) {
            scan_files();
            g_list_browser.num_items = file_count;
            if (g_list_browser.selected >= file_count) {
                g_list_browser.selected = file_count - 1;
            }
            if (g_list_browser.selected < 0) g_list_browser.selected = 0;
        } else {
            ui_show_error("DELETE FAILED");
        }
    } else {
        ui_show_error("DELETE UNSUPPORTED");
    }
}

static void perform_paste(void) {
    if (!copy_src_partition || !active_partition) return;
    
    static char dst_path[128];
    strcpy(dst_path, current_path);
    int len = strlen(dst_path);
    if (len > 1 && dst_path[len - 1] != '/') {
        strcat(dst_path, "/");
    }
    strcat(dst_path, copy_src_name);

    ui_operation_in_progress = true;

    char src_t0 = copy_src_partition->type[0];
    char src_t1 = copy_src_partition->type[1];
    
    // Ensure source driver is loaded
    if (src_t0 == 'F' && src_t1 == 'A') {
        if (vfs_get_driver("FAT") == NULL) {
            vfs_load_dynamic_driver("FAT", "/drivers/fs/fatfs.bin");
        }
    } else if (src_t0 == 'L') {
        if (!vfs_is_lfs_rw_loaded()) {
            vfs_load_dynamic_driver("LFS", "/drivers/fs/lfs_rw.bin");
        }
    }
    
    const char *src_driver_name = (src_t0 == 'L') ? "LFS" : ((src_t0 == 'F' && src_t1 == 'r') ? "FROGFS" : "FAT");
    vfs_driver_t *src_drv = vfs_get_driver(src_driver_name);
    
    char dst_t0 = active_partition->type[0];
    char dst_t1 = active_partition->type[1];
    const char *dst_driver_name = (dst_t0 == 'L') ? "LFS" : ((dst_t0 == 'F' && dst_t1 == 'r') ? "FROGFS" : "FAT");
    vfs_driver_t *dst_drv = vfs_get_driver(dst_driver_name);

    if (!src_drv || !dst_drv) {
        ui_show_error("DRIVER NOT AVAILABLE");
        ui_operation_in_progress = false;
        return;
    }

    bool need_src_mount = (copy_src_partition != active_partition);
    if (need_src_mount) {
        if (src_drv->mount(copy_src_partition->address, copy_src_partition->size) != 0) {
            ui_show_error("FAILED TO MOUNT SRC");
            ui_operation_in_progress = false;
            return;
        }
    }

    void *f_src = NULL;
    void *f_dst = NULL;

    if (src_drv->open(copy_src_path, 1, &f_src) != 0) {
        if (need_src_mount) src_drv->unmount();
        ui_show_error("FAILED TO OPEN SRC FILE");
        ui_operation_in_progress = false;
        return;
    }

    if (dst_drv->open(dst_path, 2, &f_dst) != 0) {
        src_drv->close(f_src);
        if (need_src_mount) src_drv->unmount();
        ui_show_error("FAILED TO CREATE DST FILE");
        ui_operation_in_progress = false;
        return;
    }

    uint32_t sz = copy_src_size;
    uint32_t copied = 0;
    bool success = true;
    static uint8_t copy_buf[4096];

    while (copied < sz) {
        if (input_just_pressed(INPUT_PWR) || input_just_pressed(INPUT_B)) {
            success = false;
            break;
        }
        
        uint32_t to_read = sz - copied;
        if (to_read > sizeof(copy_buf)) to_read = sizeof(copy_buf);
        
        size_t read_len = 0;
        if (src_drv->read(f_src, copy_buf, to_read, &read_len) != 0 || read_len == 0) {
            success = false;
            break;
        }
        
        size_t written_len = 0;
        if (dst_drv->write(f_dst, copy_buf, read_len, &written_len) != 0 || written_len != read_len) {
            success = false;
            break;
        }
        
        copied += read_len;
        
        int pct = (sz > 0) ? (copied * 100 / sz) : 100;
        update_progress_ui(pct, "COPYING FILE...", copy_src_name);
    }

    src_drv->close(f_src);
    dst_drv->close(f_dst);
    if (need_src_mount) {
        src_drv->unmount();
    }
    ui_operation_in_progress = false;

    if (success) {
        scan_files();
        g_list_browser.num_items = file_count;
    } else {
        if (dst_drv->unlink) {
            dst_drv->unlink(dst_path);
        }
        scan_files();
        g_list_browser.num_items = file_count;
    }
}

static void file_context_menu_callback(int index) {
    if (index < 0 || index >= g_file_opts_count) return;
    int action = context_actions[index];
    int sel_idx = g_list_browser.selected;
    
    if (action == ACT_COPY) {
        copy_src_partition = active_partition;
        strcpy(copy_src_path, current_path);
        int len = strlen(copy_src_path);
        if (len > 1 && copy_src_path[len - 1] != '/') {
            strcat(copy_src_path, "/");
        }
        strcat(copy_src_path, file_names[sel_idx]);
        strcpy(copy_src_name, file_names[sel_idx]);
        copy_src_size = file_sizes[sel_idx];
    } else if (action == ACT_PASTE) {
        perform_paste();
    } else if (action == ACT_DELETE) {
        ui_show_confirm("DELETE FILE?", perform_delete);
    }
}

static void open_file_context_menu(void) {
    int sel_idx = g_list_browser.selected;
    bool is_parent_link = (sel_idx >= 0 && sel_idx < file_count && strcmp(file_names[sel_idx], "..") == 0);
    g_file_opts_count = 0;
    bool is_rw = false;
    if (active_partition) {
        char t0 = active_partition->type[0];
        char t1 = active_partition->type[1];
        if (t0 == 'F' && t1 == 'A') {
            is_rw = true;
        } else if (t0 == 'L') {
            is_rw = vfs_is_lfs_rw_loaded();
        }
    }
    bool can_paste = (copy_src_partition != NULL && is_rw);
    
    if (file_count > 0) {
        if (!is_parent_link && file_types[sel_idx] == BROWSER_TYPE_FILE) {
            bool can_access = true;
            if (active_partition && active_partition->type[0] == 'F' && active_partition->type[1] == 'r') {
                void *f_temp = NULL;
                char temp_path[128];
                strcpy(temp_path, current_path);
                int len = strlen(temp_path);
                if (len > 1 && temp_path[len - 1] != '/') {
                    strcat(temp_path, "/");
                }
                strcat(temp_path, file_names[sel_idx]);
                
                vfs_driver_t *drv = vfs_get_driver("FROGFS");
                if (drv && drv->open(temp_path, 1, &f_temp) == 0) {
                    drv->close(f_temp);
                } else {
                    can_access = false;
                }
            }
            
            if (can_access) {
                g_file_opts[g_file_opts_count] = "COPY";
                context_actions[g_file_opts_count++] = ACT_COPY;
            }
        }
        
        if (can_paste) {
            g_file_opts[g_file_opts_count] = "PASTE";
            context_actions[g_file_opts_count++] = ACT_PASTE;
        }
        
        if (!is_parent_link && is_rw) {
            g_file_opts[g_file_opts_count] = "DELETE";
            context_actions[g_file_opts_count++] = ACT_DELETE;
        }
    } else {
        if (can_paste) {
            g_file_opts[g_file_opts_count] = "PASTE";
            context_actions[g_file_opts_count++] = ACT_PASTE;
        }
    }
    
    g_file_opts[g_file_opts_count] = "CANCEL";
    context_actions[g_file_opts_count++] = ACT_CANCEL;
    
    ui_show_context_menu("OPTIONS", g_file_opts, g_file_opts_count, file_context_menu_callback);
}

static void menu_browser_exit(ui_window_t *self) {
    save_tab_state();
    for (int i = 0; i < 2; i++) {
        browser_tab_t *tab = &g_tabs[i];
        if (tab->active_partition && tab->mount_res == 0) {
            char t0 = tab->active_partition->type[0];
            char t1 = tab->active_partition->type[1];
            const char *drv_name = (t0 == 'L') ? "LFS" : ((t0 == 'F' && t1 == 'r') ? "FROGFS" : "FAT");
            vfs_driver_t *drv = vfs_get_driver(drv_name);
            if (drv) {
                drv->unmount();
            }
        }
    }
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
