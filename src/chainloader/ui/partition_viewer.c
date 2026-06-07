#include "gui.h"
#include "input.h"
#include "partition.h"
#include "ui.h"
#include "utils.h"
#include "board.h"
#include "assets.h"
#include "ui_list.h"
#include "strings.h"
#include "system/ofw_verify.h"
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

/* Cached OFW/asset CRC-verification result for the currently highlighted row. The
 * CRC sweep (up to ~3 MiB over memory-mapped flash) is far too heavy to run every
 * frame, so it runs once when the selection lands on a recognized OFW backup or
 * asset blob and is reused until the selection moves. Reset on viewer entry. */
static int s_verify_idx = -1;
static ofw_verify_status_t s_verify_status = OFW_VERIFY_NA;

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

/* Divider labels render as "-<word>-": the leading/trailing '-' is the divider
 * sentinel ui_list keys on (kept literal); the inner word is translated. Static so
 * the pointer emit_group stores outlives every draw, and rebuilt each call so a live
 * language switch re-translates them. */
static char div_int[40], div_ext[40], div_sd[40];
static void make_divider(char *buf, int cap, string_id_t id) {
    str_lcpy(buf, cap, "-");
    str_lcat(buf, cap, tr(id));
    str_lcat(buf, cap, "-");
}

static void rebuild_virtual_list(void) {
    g_part_view.virtual_count = 0;
    make_divider(div_int, sizeof(div_int), STR_DIV_INTFLASH);
    make_divider(div_ext, sizeof(div_ext), STR_DIV_EXTFLASH);
    make_divider(div_sd,  sizeof(div_sd),  STR_DIV_SDCARD);
    emit_group(div_int, pred_internal);
    emit_group(div_ext, pred_external);
    emit_group(div_sd,  pred_sd);
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
    s_verify_idx = -1;   /* partition indices changed; invalidate the verify cache */
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

    /* type doubles as a classification key, so translate only the displayed form here
     * (proper nouns Mario/Zelda/Retro-Go/FAT/LittleFS/FrogFS stay literal). */
    const char* target_name = p->type;
    if      (strcmp(p->type, "FREE") == 0)     target_name = tr(STR_FREE_SPACE);
    else if (strcmp(p->type, "Firmware") == 0) target_name = tr(STR_TYPE_FIRMWARE);
    ui_list_pane_row(0, tr(STR_LBL_TARGET), target_name, true, selected_tick);

    char size_buf[16];
    if (partition_is_sd(p)) format_size_sectors(p->size, size_buf);  /* SD: size is sectors */
    else                    format_size(p->size, size_buf);
    ui_list_pane_row(1, tr(STR_LBL_SIZE), size_buf, false, 0);

    char addr_buf[16];
    hex_to_str(p->address, addr_buf, 8);   /* SD shows its sentinel (0xC0000000) for consistency */
    ui_list_pane_row(2, tr(STR_LBL_ADDR), addr_buf, false, 0);

    /* For a recognized OFW backup / asset blob, verify its baked CRC signature
     * (cached per selection). A mismatch means it is not our patched build, so we
     * label it UNKNOWN instead of "Assets"/"OFW Backup" -- the same image the boot
     * path refuses to copy into Bank 2. */
    if (s_verify_idx != p_idx) {
        s_verify_idx = p_idx;
        s_verify_status = ofw_verify_addr(p->address);
    }

    char detail_buf[40];
    if (s_verify_status == OFW_VERIFY_BAD)
        str_lcpy(detail_buf, sizeof(detail_buf), tr(STR_UNKNOWN));
    else
        str_fmt1_int(detail_buf, sizeof(detail_buf), tr(p->detail_id), (int)p->detail_num);
    ui_list_pane_row(3, tr(STR_LBL_DETAILS), detail_buf, true, selected_tick);
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
    s_verify_idx = -1;   /* drop any cached verification from a prior session */
    rebuild_virtual_list();
    self->title = tr(STR_PARTITION_VIEWER);
    ui_list_init(&g_part_view.list, tr(STR_PARTITION_VIEWER), g_part_view.virtual_count, partition_get_label, partition_on_action);
    ui_list_set_split(&g_part_view.list, partition_draw_right_pane);
    g_part_view.list.on_back = ui_pop;
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
    /* Footer legend, mirroring the File Browser: A on a row opens its OPTIONS menu.
     * Reuses the already-translated browser legend ("...   A: SEL") so the hint is
     * localized in every language; only renders when show_footer is set (it is). */
    ui_draw_footer(tr(STR_FOOTER_BROWSER));
}

ui_window_t PAGE_PARTITION = {
    .title = NULL,   /* set via tr() in menu_partition_enter */
    .x = 0, .y = 22, .w = 320, .h = 196,
    .is_modal = 0,
    .show_footer = 1,
    .allow_idle_hide = 0,
    .enter = menu_partition_enter,
    .draw_content = menu_partition_draw,
    .update_content = menu_partition_update,
    .exit = NULL
};
