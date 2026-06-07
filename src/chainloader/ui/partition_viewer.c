#include "gui.h"
#include "input.h"
#include "partition.h"
#include "ui.h"
#include "utils.h"
#include "board.h"
#include "assets.h"
#include "ui_list.h"
#include <string.h>

typedef enum {
    PART_MODE_SCANNING,
    PART_MODE_LIST
} part_mode_t;

static struct {
    part_mode_t mode;
    partition_info_t *p_target;
    ui_list_t list;
    
    // Virtual item mapping
    struct {
        bool is_divider;
        const char *divider_label;
        int partition_idx;
    } virtual_items[24];
    int virtual_count;
} g_part_view;

static void rebuild_virtual_list(void) {
    g_part_view.virtual_count = 0;
    int count = partition_get_count();
    
    // 1. Add internal flash header if there are any internal partitions
    bool has_internal = false;
    for (int i = 0; i < count; i++) {
        partition_info_t* p = partition_get_info(i);
        if (p->address >= 0x08000000 && p->address < 0x08200000) {
            has_internal = true;
            break;
        }
    }
    if (has_internal) {
        int v_idx = g_part_view.virtual_count++;
        g_part_view.virtual_items[v_idx].is_divider = true;
        g_part_view.virtual_items[v_idx].divider_label = "-INTFLASH-";
        
        for (int i = 0; i < count; i++) {
            partition_info_t* p = partition_get_info(i);
            if (p->address >= 0x08000000 && p->address < 0x08200000) {
                int item = g_part_view.virtual_count++;
                g_part_view.virtual_items[item].is_divider = false;
                g_part_view.virtual_items[item].partition_idx = i;
            }
        }
    }
    
    // 2. Add external flash header if there are any external partitions
    bool has_external = false;
    for (int i = 0; i < count; i++) {
        partition_info_t* p = partition_get_info(i);
        if (p->address >= 0x90000000) {
            has_external = true;
            break;
        }
    }
    if (has_external) {
        int v_idx = g_part_view.virtual_count++;
        g_part_view.virtual_items[v_idx].is_divider = true;
        g_part_view.virtual_items[v_idx].divider_label = "-EXTFLASH-";
        
        for (int i = 0; i < count; i++) {
            partition_info_t* p = partition_get_info(i);
            if (p->address >= 0x90000000) {
                int item = g_part_view.virtual_count++;
                g_part_view.virtual_items[item].is_divider = false;
                g_part_view.virtual_items[item].partition_idx = i;
            }
        }
    }
}

static const char* partition_get_label(int idx) {
    if (g_part_view.virtual_items[idx].is_divider) {
        return g_part_view.virtual_items[idx].divider_label;
    }
    
    static char buf[32];
    int p_idx = g_part_view.virtual_items[idx].partition_idx;
    partition_info_t* p = partition_get_info(p_idx);
    if (p->address >= 0x08000000 && p->address < 0x08200000) {
        int bank = (p->address >= 0x08100000) ? 2 : 1;
        strcpy(buf, "BANK");
        int_to_str(bank, buf + 4);
        strcat(buf, " ");
        strcat(buf, p->type);
    } else {
        strcpy(buf, "EXT ");
        strcat(buf, p->type);
    }
    return buf;
}

static void execute_erase(void) {
    g_part_view.mode = PART_MODE_SCANNING;
    partition_erase(g_part_view.p_target->address, g_part_view.p_target->size);
    partition_scan_start();
    while (partition_scan_get_state() == PARTITION_SCAN_IN_PROGRESS) {
        partition_scan_update();
        wdog_refresh();
    }
    g_part_view.mode = PART_MODE_LIST;
    rebuild_virtual_list();
    g_part_view.list.num_items = g_part_view.virtual_count;
    if (g_part_view.list.selected >= g_part_view.list.num_items) {
        g_part_view.list.selected = g_part_view.list.num_items - 1;
    }
    input_clear_all();
}

static void confirm_erase_callback(void) {
    execute_erase();
}

static void context_menu_callback(int index) {
    if (index == 0) { // ERASE
        ui_show_confirm("ERASE PARTITION?", confirm_erase_callback);
    }
}

static const char *PARTITION_CONTEXT_OPTS[] = { "ERASE", "CANCEL" };

static void partition_on_action(int idx) {
    if (g_part_view.virtual_items[idx].is_divider) return;
    
    int p_idx = g_part_view.virtual_items[idx].partition_idx;
    g_part_view.p_target = partition_get_info(p_idx);
    // Don't erase boot sector at 0x08000000 or free space
    if (g_part_view.p_target->address != 0x08000000 && strcmp(g_part_view.p_target->type, "FREE") != 0) {
        ui_show_context_menu("OPTIONS", PARTITION_CONTEXT_OPTS, 2, context_menu_callback);
    }
}

static void partition_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (g_part_view.virtual_items[selected_idx].is_divider) return;
    
    int p_idx = g_part_view.virtual_items[selected_idx].partition_idx;
    partition_info_t* p = partition_get_info(p_idx);
    gui_draw_text(198, 40, "TARGET.", COLOR_FG);
    
    const char* target_name = (strcmp(p->type, "FREE") == 0) ? "FREE SPACE" : p->type;
    gui_draw_text_marquee(198, 55, 110, target_name, COLOR_FG, true, selected_tick);

    gui_draw_text(198, 75, "SIZE.", COLOR_FG);
    char size_buf[16];
    format_size(p->size, size_buf);
    gui_draw_text(198, 90, size_buf, COLOR_FG);

    gui_draw_text(198, 110, "ADDR.", COLOR_FG);
    char addr_buf[16];
    hex_to_str(p->address, addr_buf, 8);
    gui_draw_text(198, 125, addr_buf, COLOR_FG);

    gui_draw_text(198, 145, "DETAILS.", COLOR_FG);
    gui_draw_text_marquee(198, 160, 110, p->details, COLOR_FG, true, selected_tick);
}

static void menu_partition_enter(ui_window_t *self) {
    g_part_view.mode = (partition_scan_get_state() == PARTITION_SCAN_IN_PROGRESS) ? PART_MODE_SCANNING : PART_MODE_LIST;
    rebuild_virtual_list();
    ui_list_init(&g_part_view.list, "PARTITION VIEWER", g_part_view.virtual_count, partition_get_label, partition_on_action);
    g_part_view.list.is_split = true;
    g_part_view.list.draw_right_pane = partition_draw_right_pane;
    g_part_view.list.on_back = ui_pop;
    g_part_view.list.visible_lines = 9;
    
    // Ensure selection doesn't start on a divider
    while (g_part_view.list.selected < g_part_view.list.num_items && g_part_view.virtual_items[g_part_view.list.selected].is_divider) {
        g_part_view.list.selected++;
    }
    if (g_part_view.list.selected >= g_part_view.list.num_items) g_part_view.list.selected = 0;
}

static void menu_partition_update(ui_window_t *self) {
    if (g_part_view.mode == PART_MODE_SCANNING) {
        partition_scan_update();
        if (partition_scan_get_state() == PARTITION_SCAN_COMPLETE) {
            g_part_view.mode = PART_MODE_LIST;
            rebuild_virtual_list();
            g_part_view.list.num_items = g_part_view.virtual_count;
        }
        if (input_just_pressed(INPUT_B)) ui_pop();
        return;
    }
    
    ui_list_update(&g_part_view.list);
}

static void menu_partition_draw(ui_window_t *self) {
    if (g_part_view.mode == PART_MODE_SCANNING) {
        ui_draw_scan_progress();
        return;
    }
    ui_list_draw(&g_part_view.list, self->x, self->y, self->w, self->h);
}

ui_window_t PAGE_PARTITION = {
    .title = "PARTITION VIEWER",
    .x = 0, .y = 22, .w = 320, .h = 196,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 0,
    .enter = menu_partition_enter,
    .draw_content = menu_partition_draw,
    .update_content = menu_partition_update,
    .exit = NULL
};
