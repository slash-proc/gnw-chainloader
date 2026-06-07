#include "gui.h"
#include "input.h"
#include "partition.h"
#include "ui.h"
#include "utils.h"
#include "board.h"
#include "assets.h"
#include "ui_list.h"
#include "strings.h"
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

/* Partition grouping predicates: internal NOR (banks 1/2), external NOR (SD is
 * grouped separately), and the synthetic SD partition. */
static bool pred_internal(const partition_info_t *p) { return p->address >= 0x08000000 && p->address < 0x08200000; }
static bool pred_external(const partition_info_t *p) { return p->address >= 0x90000000 && !partition_is_sd(p); }
static bool pred_sd(const partition_info_t *p)       { return partition_is_sd(p); }

/* Append a "-LABEL-" divider then every matching partition, but only if any match. */
static void emit_group(const char *label, bool (*match)(const partition_info_t *)) {
    int count = partition_get_count();
    bool any = false;
    for (int i = 0; i < count; i++) if (match(partition_get_info(i))) { any = true; break; }
    if (!any) return;
    int v = g_part_view.virtual_count++;
    g_part_view.virtual_items[v].is_divider = true;
    g_part_view.virtual_items[v].divider_label = label;
    for (int i = 0; i < count; i++) {
        if (match(partition_get_info(i))) {
            int item = g_part_view.virtual_count++;
            g_part_view.virtual_items[item].is_divider = false;
            g_part_view.virtual_items[item].partition_idx = i;
        }
    }
}

static void rebuild_virtual_list(void) {
    g_part_view.virtual_count = 0;
    emit_group("-INTFLASH-", pred_internal);
    emit_group("-EXTFLASH-", pred_external);
    emit_group("-SD CARD-", pred_sd);
}

static const char* partition_get_label(int idx) {
    if (g_part_view.virtual_items[idx].is_divider) {
        return g_part_view.virtual_items[idx].divider_label;
    }
    
    static char buf[32];
    int p_idx = g_part_view.virtual_items[idx].partition_idx;
    partition_info_t* p = partition_get_info(p_idx);
    if (partition_is_sd(p)) {
        strcpy(buf, "SD ");
        strcat(buf, p->type);            // "SD FAT"
    } else if (p->address >= 0x08000000 && p->address < 0x08200000) {
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
        ui_show_confirm(tr(STR_ERASE_PART_CONFIRM), confirm_erase_callback);
    }
}

/* Filled from tr() at show time; static so it outlives ui_show_context_menu,
 * which keeps the array pointer (not a copy). */
static const char *PARTITION_CONTEXT_OPTS[2];

static void partition_on_action(int idx) {
    if (g_part_view.virtual_items[idx].is_divider) return;
    
    int p_idx = g_part_view.virtual_items[idx].partition_idx;
    g_part_view.p_target = partition_get_info(p_idx);
    // Don't offer ERASE for the boot sector, free space, or the SD card
    // (SD is read-only here and partition_erase() is a FLASH erase).
    if (g_part_view.p_target->address != 0x08000000 &&
        !partition_is_sd(g_part_view.p_target) &&
        strcmp(g_part_view.p_target->type, "FREE") != 0) {
        PARTITION_CONTEXT_OPTS[0] = tr(STR_ERASE);
        PARTITION_CONTEXT_OPTS[1] = tr(STR_CANCEL);
        ui_show_context_menu(tr(STR_OPTIONS), PARTITION_CONTEXT_OPTS, 2, context_menu_callback);
    }
}

static void partition_draw_right_pane(int selected_idx, uint32_t selected_tick) {
    if (g_part_view.virtual_items[selected_idx].is_divider) return;
    
    int p_idx = g_part_view.virtual_items[selected_idx].partition_idx;
    partition_info_t* p = partition_get_info(p_idx);
    gui_draw_text(198, 40, tr(STR_LBL_TARGET), COLOR_FG);

    const char* target_name = (strcmp(p->type, "FREE") == 0) ? tr(STR_FREE_SPACE) : p->type;
    gui_draw_text_marquee(198, 55, 110, target_name, COLOR_FG, true, selected_tick);

    gui_draw_text(198, 75, tr(STR_LBL_SIZE), COLOR_FG);
    char size_buf[16];
    if (partition_is_sd(p)) format_size_sectors(p->size, size_buf);  /* SD: size is sectors */
    else                    format_size(p->size, size_buf);
    gui_draw_text(198, 90, size_buf, COLOR_FG);

    gui_draw_text(198, 110, tr(STR_LBL_ADDR), COLOR_FG);
    if (partition_is_sd(p)) {
        gui_draw_text(198, 125, "SD", COLOR_FG);   /* sentinel addr is meaningless */
    } else {
        char addr_buf[16];
        hex_to_str(p->address, addr_buf, 8);
        gui_draw_text(198, 125, addr_buf, COLOR_FG);
    }

    gui_draw_text(198, 145, tr(STR_LBL_DETAILS), COLOR_FG);
    gui_draw_text_marquee(198, 160, 110, p->details, COLOR_FG, true, selected_tick);
}

/* Land the selection on the first non-divider row. */
static void select_first_item(void) {
    g_part_view.list.selected = 0;
    while (g_part_view.list.selected < g_part_view.list.num_items &&
           g_part_view.virtual_items[g_part_view.list.selected].is_divider) {
        g_part_view.list.selected++;
    }
    if (g_part_view.list.selected >= g_part_view.list.num_items) g_part_view.list.selected = 0;
}

static void menu_partition_enter(ui_window_t *self) {
    /* Re-check ONLY the SD card on entry (the full flash scan stays a boot-time
     * thing). Skipped while the boot scan is still running — that detects SD. */
    if (partition_scan_get_state() != PARTITION_SCAN_IN_PROGRESS) {
        partition_redetect_sd();
    }
    g_part_view.mode = (partition_scan_get_state() == PARTITION_SCAN_IN_PROGRESS) ? PART_MODE_SCANNING : PART_MODE_LIST;
    rebuild_virtual_list();
    self->title = tr(STR_PARTITION_VIEWER);
    ui_list_init(&g_part_view.list, tr(STR_PARTITION_VIEWER), g_part_view.virtual_count, partition_get_label, partition_on_action);
    g_part_view.list.is_split = true;
    g_part_view.list.draw_right_pane = partition_draw_right_pane;
    g_part_view.list.on_back = ui_pop;
    g_part_view.list.visible_lines = 9;
    select_first_item();
}

static void menu_partition_update(ui_window_t *self) {
    if (g_part_view.mode == PART_MODE_SCANNING) {
        partition_scan_update();
        if (partition_scan_get_state() == PARTITION_SCAN_COMPLETE) {
            g_part_view.mode = PART_MODE_LIST;
            rebuild_virtual_list();
            g_part_view.list.num_items = g_part_view.virtual_count;
            select_first_item();
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
    .title = NULL,   /* set via tr() in menu_partition_enter */
    .x = 0, .y = 22, .w = 320, .h = 196,
    .is_modal = 0,
    .show_footer = 0,
    .allow_idle_hide = 0,
    .enter = menu_partition_enter,
    .draw_content = menu_partition_draw,
    .update_content = menu_partition_update,
    .exit = NULL
};
