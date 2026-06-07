#include "ui_list.h"
#include "gui.h"
#include "input.h"
#include "board.h"
#include "theme.h"
#include <string.h>

/* How long the scroll indicator stays visible after the list last scrolled
 * (the viewport shifted) before it disappears. */
#define SCROLLBAR_LINGER_MS 300

/* Fixed gap between the selection indicator's left edge (the list box's left
 * edge x, where the themed sprite / plain '>' is anchored) and the first
 * character of the row text. Used for BOTH plain menus and split dual-panes so
 * the visible cursor-to-text spacing is identical regardless of layout. The
 * dual-pane's left column is still narrower (its text region width differs),
 * but this indicator-to-text gap is the same constant in both, and mirrors
 * correctly in RTL because start_x is fed through gui_mirror_x. */
#define INDICATOR_TEXT_GAP 22

/* Generic ui_list-backed page: a ui_window_t whose user_data points at its ui_list shares these
 * two forwarders instead of each view re-declaring trivial update/draw wrappers. Views with extra
 * page logic (the Partition Viewer's scan state, the File Browser's file ops) keep their own. */
void ui_list_page_update(ui_window_t *self) { ui_list_update((ui_list_t *)self->user_data); }
void ui_list_page_draw(ui_window_t *self) {
    ui_list_draw((ui_list_t *)self->user_data, self->x, self->y, self->w, self->h);
}

/* One label/value row of a split view's right detail pane: label at one of four fixed slots
 * (y 40/75/110/145) and its value 15px below, in the 110px detail column (x=198 in LTR, mirrored
 * left in RTL). marquee+tick scroll a long value. Collapses the px/pw geometry + the label/value
 * gui_draw_text_aligned pair that the File Browser, FS list, and Partition Viewer right panes all
 * used to inline. */
void ui_list_pane_row(int slot, const char *label, const char *value, bool marquee, uint32_t tick) {
    static const uint8_t LY[4] = { 40, 75, 110, 145 };
    const int pw = 110;
    int px = gui_mirror_x(198, pw, 0, SCREEN_WIDTH);
    gui_draw_text_aligned(px, LY[slot],      pw, label, COLOR_FG, false,   0);
    gui_draw_text_aligned(px, LY[slot] + 15, pw, value, COLOR_FG, marquee, tick);
}

/* Configure a list as a split (two-pane) view: enable the divider, set the taller
 * 9-line visible window the split layout uses, and install the right-pane drawer.
 * The on_back handler varies per view, so it stays at each call site. */
void ui_list_set_split(ui_list_t *l, void (*draw_right_pane)(int, uint32_t)) {
    l->is_split = true;
    l->visible_lines = 9;
    l->draw_right_pane = draw_right_pane;
}

void ui_list_init(ui_list_t *list, const char *title, int num_items,
                  const char* (*get_label)(int), void (*on_action)(int)) {
    list->title = title;
    list->num_items = num_items;
    list->get_label = get_label;
    list->on_action = on_action;
    list->selected = 0;
    
    // Skip dividers starting with '-'
    if (num_items > 0) {
        while (list->selected < num_items) {
            const char *lbl = get_label(list->selected);
            if (lbl && lbl[0] != '-') break;
            list->selected++;
        }
        if (list->selected >= num_items) {
            list->selected = 0;
        }
    }
    
    list->scroll_y = 0;
    list->visible_lines = 6;
    list->selected_tick = HAL_GetTick();
    list->scroll_tick = 0;   // hidden until the list actually scrolls
    list->is_split = false;
    list->draw_right_pane = NULL;
    list->on_back = NULL;
    list->on_adjust = NULL;
    list->on_game = NULL;
    list->is_enabled = NULL;
}

/* A row the cursor may rest on / act on: present, not a divider, not disabled. */
static bool item_selectable(ui_list_t *list, int idx) {
    if (idx < 0 || idx >= list->num_items) return false;
    const char *lbl = list->get_label(idx);
    if (!lbl || lbl[0] == '-') return false;
    if (list->is_enabled && !list->is_enabled(idx)) return false;
    return true;
}

void ui_list_update(ui_list_t *list) {
    if (list->num_items == 0) return;

    /* Keep the cursor off dividers and disabled items. The enable predicate can
     * change at runtime (e.g. an OFW backup appearing), and the initial selection
     * is chosen before is_enabled is wired up, so re-validate each frame. */
    if (!item_selectable(list, list->selected)) {
        int orig = list->selected;
        do {
            list->selected = (list->selected + 1) % list->num_items;
        } while (!item_selectable(list, list->selected) && list->selected != orig);
    }

    if (input_is_repeating(INPUT_UP)) {
        int orig = list->selected;
        do {
            list->selected = (list->selected > 0) ? list->selected - 1 : list->num_items - 1;
            if (item_selectable(list, list->selected)) break;
        } while (list->selected != orig);
        list->selected_tick = HAL_GetTick();
    }
    if (input_is_repeating(INPUT_DOWN)) {
        int orig = list->selected;
        do {
            list->selected = (list->selected < list->num_items - 1) ? list->selected + 1 : 0;
            if (item_selectable(list, list->selected)) break;
        } while (list->selected != orig);
        list->selected_tick = HAL_GetTick();
    }
    
    // Sticky-header-aware auto-scroll. When a section divider is pinned to the
    // top row (see ui_list_draw), it consumes one visible line, so the effective
    // content window is one shorter. Lists without dividers are unaffected
    // (eff == visible_lines).
    int eff = list->visible_lines;
    for (int s = list->scroll_y; s >= 0; s--) {
        if (list->get_label(s)[0] == '-') { if (s < list->scroll_y) eff--; break; }
    }
    int prev_scroll = list->scroll_y;
    if (list->selected < list->scroll_y) list->scroll_y = list->selected;
    if (list->selected >= list->scroll_y + eff) list->scroll_y = list->selected - (eff - 1);
    if (list->scroll_y != prev_scroll) list->scroll_tick = HAL_GetTick();   // viewport shifted

    /* Value-selector items (e.g. "< Theme >") adjust on LEFT/RIGHT. */
    if (list->on_adjust) {
        if (input_is_repeating(INPUT_LEFT))  { list->on_adjust(list->selected, -1); list->selected_tick = HAL_GetTick(); }
        if (input_is_repeating(INPUT_RIGHT)) { list->on_adjust(list->selected, +1); list->selected_tick = HAL_GetTick(); }
    }

    /* GAME on the selected row: a distinct side-action (e.g. flash an OFW to preview
     * its theme). The hook itself gates which rows respond, so no item_selectable
     * requirement here. */
    if (input_just_pressed(INPUT_GAME) && list->on_game) {
        list->on_game(list->selected);
        return;
    }

    if (input_just_pressed(INPUT_A) && item_selectable(list, list->selected)) {
        if (list->on_action) {
            list->on_action(list->selected);
            return;
        }
    }
    
    if (input_just_pressed(INPUT_B) || input_just_pressed(INPUT_START)) {
        if (list->on_back) {
            list->on_back();
            return;
        }
    }
}

void ui_list_draw(ui_list_t *list, int x, int y, int w, int h) {
    uint32_t ticks = HAL_GetTick();

    if (list->is_split) {
        /* Stop the divider at the footer line when a footer is shown, so it doesn't
         * run into the footer area; otherwise it spans the full content height. */
        int footer_top = ui_footer_top();
        int div_h = (footer_top > 0 && footer_top > y) ? (footer_top - y) : h;
        gui_draw_rect(gui_mirror_x(x + 188, 1, x, w), y, 1, div_h, COLOR_BORDER);
        if (list->draw_right_pane && list->num_items > 0) {
            list->draw_right_pane(list->selected, list->selected_tick);
        }
    }

    /* Text indent: a FIXED gap past the indicator anchor (x) in both modes, so the
     * cursor-to-text spacing is the same constant in menus and dual-panes. Only the
     * text region WIDTH differs (the split's left column is narrower, leaving room
     * for the right pane). */
    int start_x = x + INDICATOR_TEXT_GAP;
    /* Split left column ends just shy of the divider (x+188)/scrollbar (x+182); keep
     * the historical right edge at x+178, so only start_x moved (gap widened to match
     * the menu), not the column's right boundary. */
    int max_w = list->is_split ? (178 - INDICATOR_TEXT_GAP) : (w - 32);
    /* RTL: mirror the text region within the list's own box, so rows right-align and
     * the cursor/scrollbar swap sides. Identity in LTR (row_x == start_x). */
    int row_x = gui_mirror_x(start_x, max_w, x, w);

    // Centering vertically based on actual drawn item height (excluding split views)
    int count = list->visible_lines;
    if (list->num_items < count) count = list->num_items;
    int start_y = list->is_split ? (y + 5) : (y + (h - count * 20 + 12) / 2);

    // Sticky section header: if the divider heading the current section has
    // scrolled above the window, pin it to the first visible row. Content then
    // fills rows 1..visible_lines-1. No-op for lists with no dividers.
    int sticky = -1;
    for (int s = list->scroll_y; s >= 0; s--) {
        if (list->get_label(s)[0] == '-') { sticky = s; break; }
    }
    int pinned = (sticky >= 0 && sticky < list->scroll_y);

    for (int i = 0; i < list->visible_lines; i++) {
        int idx;
        if (pinned && i == 0) {
            idx = sticky;                                   // pinned section header
        } else {
            idx = list->scroll_y + (pinned ? i - 1 : i);
            if (idx >= list->num_items) break;
        }

        int item_y = start_y + i * 20;
        const char *label = list->get_label(idx);

        if (idx == list->selected) {
            /* Unified selection indicator for BOTH plain and split (dual-pane) lists:
             * try the active theme's sprite cursor first, falling back to the plain
             * '>' glyph only when no theme sprite is present. The cursor X is anchored
             * to the list's left edge (x) the SAME way in both modes -- the 16px themed
             * sprite at x, the plain '>' 6px in at x+6 -- so the indicator sits
             * identically whether or not the list is split. This is exactly the
             * historical plain-menu placement (themed x / plain x+6), so plain menus
             * stay pixel-identical; split lists previously skipped the themed sprite
             * and drew '>' flush at x, so their '>' now matches plain (6px in). Both
             * mirror to the row's far edge (right in RTL) via gui_mirror_x. */
            int theme_x = gui_mirror_x(x,     16, x, w);
            int plain_x = gui_mirror_x(x + 6, 6,  x, w);
            if (!theme_draw_selector(theme_x, item_y, ticks)) {
                gui_draw_selector(plain_x, item_y, COLOR_ACCENT);
            }
        }

        if (label[0] == '-') {
            int text_w = gui_text_width(label);   /* dividers stay centered in the (mirrored) region */
            gui_draw_text(row_x + (max_w - text_w) / 2, item_y, label, COLOR_ACCENT);
        } else {
            /* Disabled rows are shown greyed (and aren't selectable). */
            uint16_t col = (list->is_enabled && !list->is_enabled(idx))
                         ? RGB565(0x68, 0x6C, 0x70) : COLOR_FG;
            gui_draw_text_aligned(row_x, item_y, max_w, label, col, (idx == list->selected), list->selected_tick);
        }
    }

    // Scroll indicator: a slim SOLID thumb (no track/border) on the list's right
    // edge. It appears only when the list actually scrolls (the viewport shifts)
    // and lingers briefly after, so cursor moves within the window don't trigger
    // it. Only drawn when the content overflows.
    if (list->num_items > list->visible_lines &&
        (ticks - list->scroll_tick) < SCROLLBAR_LINGER_MS) {
        const int bar_w = 4;
        int bar_x = gui_mirror_x(list->is_split ? (x + 182) : (x + w - bar_w - 2), bar_w, x, w);
        int track_y = start_y - 2;
        int track_h = list->visible_lines * 20;

        int thumb_h = track_h * list->visible_lines / list->num_items;
        if (thumb_h < bar_w) thumb_h = bar_w;          /* keep visible for huge lists */
        if (thumb_h > track_h) thumb_h = track_h;
        int max_scroll = list->num_items - list->visible_lines;
        int thumb_y = track_y + (max_scroll > 0 ? (track_h - thumb_h) * list->scroll_y / max_scroll : 0);
        gui_draw_fill_rect(bar_x, thumb_y, bar_w, thumb_h, COLOR_ACCENT);   /* solid */
    }
}
