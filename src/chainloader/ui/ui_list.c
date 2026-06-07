#include "ui_list.h"
#include "gui.h"
#include "input.h"
#include "board.h"
#include "assets.h"
#include <string.h>

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
    list->is_split = false;
    list->draw_right_pane = NULL;
    list->on_back = NULL;
}

void ui_list_update(ui_list_t *list) {
    if (list->num_items == 0) return;

    if (input_is_repeating(INPUT_UP)) {
        int orig = list->selected;
        do {
            list->selected = (list->selected > 0) ? list->selected - 1 : list->num_items - 1;
            if (list->get_label(list->selected)[0] != '-') break;
        } while (list->selected != orig);
        list->selected_tick = HAL_GetTick();
    }
    if (input_is_repeating(INPUT_DOWN)) {
        int orig = list->selected;
        do {
            list->selected = (list->selected < list->num_items - 1) ? list->selected + 1 : 0;
            if (list->get_label(list->selected)[0] != '-') break;
        } while (list->selected != orig);
        list->selected_tick = HAL_GetTick();
    }
    
    // Auto-scroll
    if (list->selected < list->scroll_y) list->scroll_y = list->selected;
    if (list->selected >= list->scroll_y + list->visible_lines) list->scroll_y = list->selected - (list->visible_lines - 1);

    if (input_just_pressed(INPUT_A)) {
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
        gui_draw_rect(x + 188, y, 1, h, COLOR_BORDER);
        if (list->draw_right_pane && list->num_items > 0) {
            list->draw_right_pane(list->selected, list->selected_tick);
        }
    }

    int start_x = list->is_split ? (x + 12) : (x + 22);
    int max_w = list->is_split ? 166 : (w - 32);

    // Centering vertically based on actual drawn item height (excluding split views)
    int count = list->visible_lines;
    if (list->num_items < count) count = list->num_items;
    int start_y = list->is_split ? (y + 5) : (y + (h - count * 20 + 12) / 2);

    for (int i = 0; i < list->visible_lines; i++) {
        int idx = list->scroll_y + i;
        if (idx >= list->num_items) break;
        
        int item_y = start_y + i * 20;
        const char *label = list->get_label(idx);

        if (idx == list->selected) {
            if (assets_loaded && ui_theme_enabled && !list->is_split) {
                if (ui_current_theme == UI_THEME_ZELDA) {
                    int fairy_f = (ticks / 150) % 2;
                    const uint8_t *fairy_assets[] = {
                        ASSET_ZELDA_ENTITIES_FAIRY_LEFT,
                        ASSET_ZELDA_ENTITIES_FAIRY_RIGHT
                    };
                    gui_draw_asset(x + 6, item_y - 4, fairy_assets[fairy_f], true, false);
                } else {
                    int coin_f = (ticks / 100) % 4;
                    const uint8_t *coin_assets[] = {
                        ASSET_MARIO_COIN_FRAME_1, ASSET_MARIO_COIN_FRAME_2, 
                        ASSET_MARIO_COIN_FRAME_3, ASSET_MARIO_COIN_FRAME_4
                    };
                    gui_draw_asset(x + 2, item_y - 4, coin_assets[coin_f], true, false);
                }
            } else {
                gui_draw_selector(list->is_split ? x : (x + 6), item_y, COLOR_ACCENT);
            }
        }
        
        if (label[0] == '-') {
            int text_w = strlen(label) * 8;
            gui_draw_text(start_x + (max_w - text_w) / 2, item_y, label, COLOR_ACCENT);
        } else {
            gui_draw_text_marquee(start_x, item_y, max_w, label, COLOR_FG, (idx == list->selected), list->selected_tick);
        }
    }
}
