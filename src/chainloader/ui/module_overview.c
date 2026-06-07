/*
 * Module Overview: a Tools-menu diagnostic that lists every loaded module -- where it loaded from
 * (XIP in place from flash vs a full RAM copy), its pool footprint, whether it is still resident,
 * and its load model. Clones the Partition Viewer's ui_list (list on the left) + draw_right_pane
 * (detail on the right) over the loader's module registry (system/loader.h). English-only literals
 * (a diagnostic), so no strings/i18n churn.
 */
#include "gui.h"
#include "ui.h"
#include "ui_list.h"
#include "utils.h"
#include "assets.h"
#include "system/loader.h"
#include <string.h>

static ui_list_t g_mod_list;

/* Left column: the module's filename (basename of its load path). */
static const char *modov_label(int idx) {
    static char buf[40];
    if (idx < 0 || (uint32_t)idx >= g_mod_recs_n) return "";
    const char *base = g_mod_recs[idx].path;
    for (const char *q = g_mod_recs[idx].path; *q; q++) if (*q == '/') base = q + 1;
    str_lcpy(buf, sizeof(buf), base);
    return buf;
}

/* Right pane: source / pool RAM / state / type for the selected module. */
static void modov_right_pane(int idx, uint32_t tick) {
    (void)tick;
    if (idx < 0 || (uint32_t)idx >= g_mod_recs_n) return;
    const mod_rec_t *r = &g_mod_recs[idx];
    const int pw = 110, px = 198;
    char buf[24];

    gui_draw_text_aligned(px, 40, pw, "SOURCE", COLOR_FG, false, 0);
    if (r->flash) { str_lcpy(buf, sizeof(buf), "XIP 0x"); hex_to_str(r->flash, buf + strlen(buf), 8); }
    else          str_lcpy(buf, sizeof(buf), "RAM copy");
    gui_draw_text_aligned(px, 55, pw, buf, COLOR_FG, false, 0);

    gui_draw_text_aligned(px, 75, pw, "POOL RAM", COLOR_FG, false, 0);
    format_size(r->ram, buf);
    gui_draw_text_aligned(px, 90, pw, buf, COLOR_FG, false, 0);

    gui_draw_text_aligned(px, 110, pw, "STATE", COLOR_FG, false, 0);
    gui_draw_text_aligned(px, 125, pw, (r->base + r->ram <= g_pool_next) ? "RESIDENT" : "freed", COLOR_FG, false, 0);

    gui_draw_text_aligned(px, 145, pw, "TYPE", COLOR_FG, false, 0);
    gui_draw_text_aligned(px, 160, pw, (r->flags & 1u) ? "transient" : "resident", COLOR_FG, false, 0);
}

static void modov_enter(ui_window_t *self) {
    self->title = "Module Overview";
    ui_list_init(&g_mod_list, "Module Overview", (int)g_mod_recs_n, modov_label, 0);
    g_mod_list.is_split = true;
    g_mod_list.draw_right_pane = modov_right_pane;
    g_mod_list.on_back = ui_pop;
    g_mod_list.visible_lines = 9;
    g_mod_list.selected = 0;
}

/* Page update/draw are the generic ui_list forwarders (user_data -> the list); no per-view wrapper. */
ui_window_t PAGE_MODOVERVIEW = {
    .title = NULL,
    .x = 0, .y = 22, .w = 320, .h = 196,
    .is_modal = 0, .show_footer = 0, .allow_idle_hide = 0,
    .enter = modov_enter,
    .draw_content = ui_list_page_draw, .update_content = ui_list_page_update,
    .user_data = &g_mod_list, .exit = NULL,
};
