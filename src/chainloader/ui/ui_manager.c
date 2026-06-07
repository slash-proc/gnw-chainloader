#include "ui.h"
#include "gui.h"
#include "board.h"
#include "input.h"
#include "assets.h"
#include "utils.h"
#include "partition.h"
#include "menu.h"
#include "ui_list.h"
#include <string.h>

#define MAX_WINDOWS 8

ui_theme_t ui_current_theme = UI_THEME_DEFAULT;
bool ui_theme_enabled = true;
bool ui_operation_in_progress = false;

static ui_window_t *g_window_stack[MAX_WINDOWS];
static int g_stack_ptr = -1;
static uint32_t g_last_activity_tick = 0;

static void ui_draw_footer_bar(void);
static void ui_draw_window_chrome(ui_window_t *win);

void ui_init(void) {
    g_stack_ptr = -1;
    ui_operation_in_progress = false;
    g_last_activity_tick = HAL_GetTick();
}

void ui_update_theme(void) {
    if (!ui_theme_enabled) {
        ui_current_theme = UI_THEME_DEFAULT;
        gui_bg_color = RGB565(0x20, 0x24, 0x28);
        gui_fg_color = RGB565(0xD0, 0xD4, 0xD8);
        gui_accent_color = RGB565(0x00, 0xA0, 0xA0);
        gui_border_color = RGB565(0xD0, 0xD4, 0xD8);
        return;
    }

    if (board_console_type == CONSOLE_ZELDA) {
        ui_current_theme = UI_THEME_ZELDA;
        gui_bg_color = RGB565(0x1E, 0x60, 0x30);     // G&W Link Green (Lighter)
        gui_fg_color = RGB565(0xFF, 0xFF, 0xFF);     // Pure White
        gui_accent_color = RGB565(0xE5, 0xB8, 0x3B); // G&W Gold (Brighter)
        gui_border_color = RGB565(0xE5, 0xB8, 0x3B); // G&W Gold (Border)
    } else if (board_console_type == CONSOLE_MARIO) {
        ui_current_theme = UI_THEME_MARIO;
        gui_bg_color = RGB565(0x5F, 0x73, 0xFF);
        gui_fg_color = RGB565(0, 0, 0);
        gui_accent_color = RGB565(0, 0x50, 0xA0);
        gui_border_color = RGB565(0, 0, 0);
    } else {
        ui_current_theme = UI_THEME_DEFAULT;
        gui_bg_color = RGB565(0x20, 0x24, 0x28);
        gui_fg_color = RGB565(0xD0, 0xD4, 0xD8);
        gui_accent_color = RGB565(0x00, 0xA0, 0xA0);
        gui_border_color = RGB565(0xD0, 0xD4, 0xD8);
    }
}

void ui_theme_toggle(void) {
    ui_theme_enabled = !ui_theme_enabled;
    ui_update_theme();
}

void ui_push(ui_window_t *win) {
    if (g_stack_ptr < MAX_WINDOWS - 1) {
        g_window_stack[++g_stack_ptr] = win;
        if (win->enter) win->enter(win);
        g_last_activity_tick = HAL_GetTick();
    }
}

void ui_pop(void) {
    if (g_stack_ptr >= 0) {
        if (g_window_stack[g_stack_ptr]->exit) {
            g_window_stack[g_stack_ptr]->exit(g_window_stack[g_stack_ptr]);
        }
        g_stack_ptr--;
        g_last_activity_tick = HAL_GetTick();
    }
}

void ui_switch(ui_window_t *win) {
    if (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->exit) {
        g_window_stack[g_stack_ptr]->exit(g_window_stack[g_stack_ptr]);
    }
    
    if (g_stack_ptr < 0) g_stack_ptr = 0;
    g_window_stack[g_stack_ptr] = win;
    if (win->enter) win->enter(win);
    g_last_activity_tick = HAL_GetTick();
}

void ui_update(void) {
    uint16_t prev_input = input_get_state();
    input_update();
    uint16_t curr_input = input_get_state();
    uint32_t now = HAL_GetTick();

    bool was_idle = (now - g_last_activity_tick > 30000);
    if (curr_input != prev_input) {
        g_last_activity_tick = now;
    }

    if (input_just_pressed(INPUT_PWR)) {
        menu_enter_standby();
    }

    if (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->update_content) {
        if (!was_idle || !g_window_stack[g_stack_ptr]->allow_idle_hide) {
            g_window_stack[g_stack_ptr]->update_content(g_window_stack[g_stack_ptr]);
        }
    }
}

void ui_draw(void) {
    ui_draw_background();
    
    uint32_t now = HAL_GetTick();
    bool hide_idle = (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->allow_idle_hide && (now - g_last_activity_tick >= 30000));

    if (!hide_idle && g_stack_ptr >= 0) {
        int start_idx = g_stack_ptr;
        while (start_idx > 0 && g_window_stack[start_idx]->is_modal) {
            start_idx--;
        }
        for (int i = start_idx; i <= g_stack_ptr; i++) {
            ui_window_t *win = g_window_stack[i];
            
            if (win->is_modal) {
                // Dim/frost the screen behind the modal (within workspace area)
                gui_draw_blend_rect(0, 22, 320, 196, ui_current_theme == UI_THEME_MARIO);
            }
            
            // Draw window background/borders (chrome) if modal or not fullscreen
            if (win->is_modal || (win->w < 320 || win->h < 196)) {
                ui_draw_window_chrome(win);
            }
            
            if (win->draw_content) {
                win->draw_content(win);
            }
        }
    }

    // Always draw header & footer on top of background / stack windows
    const char *title = (g_stack_ptr >= 0) ? g_window_stack[g_stack_ptr]->title : "";
    ui_draw_header(title);
    ui_draw_footer_bar();

    gui_refresh();
}

void ui_draw_background(void) {
    gui_fill(COLOR_BG);

    bool app_active = (g_stack_ptr >= 0 && !g_window_stack[g_stack_ptr]->is_modal && g_window_stack[g_stack_ptr]->w == 320);
    if (!app_active) {
        ui_draw_animations();
    }
}

void ui_draw_animations(void) {
    if (!assets_loaded || !ui_theme_enabled) return;

    uint32_t ticks = HAL_GetTick();

    if (ui_current_theme == UI_THEME_MARIO) {
        static const int8_t st[] = { 0, 1, 2, 2, 3, 2, 2, 1, 0, -1, -2, -2, -3, -2, -2, -1 };
        static const struct { uint16_t x, y, d, o; } c[] = {
            { 88, 40, 120, 0 }, { 268, 60, 180, 4 }, { 178, 25, 150, 8 }, { 358, 48, 200, 12 }
        };

        for (int i = 0; i < 4; i++) {
            int cx = (c[i].x + 368 - ((ticks / c[i].d) % 368)) % 368 - 48;
            gui_draw_asset(cx, c[i].y + st[((ticks / 250) + c[i].o) % 16], ASSET_MARIO_CLOUDS_LARGE, true, false);
        }
    }
}

void ui_draw_header(const char *title) {
    bool app_active = (g_stack_ptr >= 0 && !g_window_stack[g_stack_ptr]->is_modal && g_window_stack[g_stack_ptr]->w == 320);
    
    if (app_active || g_stack_ptr < 0) {
        gui_draw_fill_rect(0, 0, SCREEN_WIDTH, 22, gui_accent_color);
    }
    
    // Draw header divider line
    gui_draw_rect(0, 22, SCREEN_WIDTH, 1, (app_active || g_stack_ptr < 0) ? COLOR_FG : (ui_theme_enabled ? gui_accent_color : COLOR_FG));

    char batt_str[32];
    int pct;
    bool charging;
    board_battery_update(&pct, &charging);
    
    strcpy(batt_str, "BATT: ");
    int_to_str(pct, batt_str + 6);
    strcat(batt_str, charging ? "% [CHG]" : "%");

    int batt_x = SCREEN_WIDTH - (int)strlen(batt_str) * 8 - 4;
    int max_w = batt_x - 30;
    if (max_w < 50) max_w = 50;

    gui_draw_text_marquee(20, 7, max_w, title, COLOR_FG, true, 0);
    gui_draw_text(batt_x, 7, batt_str, COLOR_FG);
}

static void ui_draw_footer_bar(void) {
    bool app_active = (g_stack_ptr >= 0 && !g_window_stack[g_stack_ptr]->is_modal && g_window_stack[g_stack_ptr]->w == 320);
    bool show_footer = (g_stack_ptr >= 0) ? g_window_stack[g_stack_ptr]->show_footer : false;
    
    if (app_active) {
        if (show_footer) {
            gui_draw_fill_rect(0, 218, SCREEN_WIDTH, 22, gui_accent_color);
            gui_draw_rect(0, 218, SCREEN_WIDTH, 1, COLOR_FG);
        }
    } else {
        if (!assets_loaded || !ui_theme_enabled) {
            gui_draw_rect(0, 218, SCREEN_WIDTH, 1, COLOR_FG);
            return;
        }

        if (ui_current_theme == UI_THEME_ZELDA) {
            gui_draw_rect(0, 218, SCREEN_WIDTH, 1, gui_accent_color);
            gui_draw_fill_rect(0, 219, SCREEN_WIDTH, 21, RGB565(0x0F, 0x30, 0x18));
        } else {
            for (int x = 0; x < SCREEN_WIDTH; x += 16) {
                gui_draw_asset(x, 219, ASSET_MARIO_BRICK_FLOOR, false, false);
                gui_draw_asset(x, 235, ASSET_MARIO_BRICK_FLOOR, false, false);
            }
            
            // Draw green Yoshi animation
            uint32_t ticks = HAL_GetTick();
            int range = SCREEN_WIDTH - 32;
            int t = (ticks / 50) % (2 * range);
            int x = (t < range) ? t : (2 * range - t);
            static const uint8_t *assets[] = { ASSET_MARIO_YOSHI_GREEN_WALKING_1, ASSET_MARIO_YOSHI_GREEN_WALKING_2, ASSET_MARIO_YOSHI_GREEN_WALKING_3 };
            gui_draw_asset(x, 200, assets[(ticks / 150) % 3], true, (t >= range));
        }
    }
}

static void ui_draw_window_chrome(ui_window_t *win) {
    if (win->is_modal) {
        gui_draw_fill_rect(win->x, win->y, win->w, win->h, COLOR_BG);
    } else {
        gui_draw_blend_rect(win->x, win->y, win->w, win->h, ui_current_theme == UI_THEME_MARIO);
    }
    gui_draw_rect(win->x, win->y, win->w, win->h, COLOR_BORDER);
    
    // Draw optional title header inside floating window/modal
    if (win->title && win->is_modal) {
        gui_draw_text(win->x + 10, win->y + 8, win->title, COLOR_FG);
        gui_draw_rect(win->x, win->y + 24, win->w, 1, COLOR_BORDER);
    }
}

void ui_draw_footer(const char *text) {
    if (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->show_footer) {
        int text_len = strlen(text);
        int text_w = text_len * 8;
        int x = (SCREEN_WIDTH - text_w) / 2;
        gui_draw_text(x, 225, text, COLOR_FG);
    }
}

void ui_draw_scan_progress(void) {
    int done, total;
    partition_scan_get_progress(&done, &total);
    int pct = (total > 0) ? (done * 100 / total) : 0;
    if (pct > 100) pct = 100;

    // Draw scanning UI in the workspace area
    gui_draw_text(20, 40, "> SCANNING MEMORY...", COLOR_FG);
    gui_draw_text(20, 70, partition_current_phase, COLOR_FG);
    gui_draw_progress_bar(20, 110, 280, 18, pct, COLOR_BORDER, COLOR_BORDER);
    char pct_buf[8];
    int_to_str(pct, pct_buf);
    strcat(pct_buf, "%");
    gui_draw_text(148, 135, pct_buf, COLOR_FG);
    ui_draw_footer("B: CANCEL");
}

static const char *g_confirm_msg = NULL;
static void (*g_confirm_callback)(void) = NULL;
static ui_window_t g_confirm_win;

static void confirm_draw(ui_window_t *self) {
    gui_draw_text(self->x + 20, self->y + 40, g_confirm_msg, COLOR_FG);
    gui_draw_text(self->x + 20, self->y + 70, "A: YES   B: NO", COLOR_FG);
}

static void confirm_update(ui_window_t *self) {
    if (input_just_pressed(INPUT_A)) {
        ui_pop();
        if (g_confirm_callback) g_confirm_callback();
    } else if (input_just_pressed(INPUT_B)) {
        ui_pop();
    }
}

void ui_show_confirm(const char *message, void (*on_confirm)(void)) {
    g_confirm_msg = message;
    g_confirm_callback = on_confirm;
    
    g_confirm_win.title = "CONFIRM";
    g_confirm_win.x = 40;
    g_confirm_win.y = 70;
    g_confirm_win.w = 240;
    g_confirm_win.h = 100;
    g_confirm_win.is_modal = 1;
    g_confirm_win.show_footer = 0;
    g_confirm_win.allow_idle_hide = 0;
    g_confirm_win.draw_content = confirm_draw;
    g_confirm_win.update_content = confirm_update;
    g_confirm_win.exit = NULL;
    
    ui_push(&g_confirm_win);
}

static const char *g_error_msg = NULL;
static ui_window_t g_error_win;

static void error_draw(ui_window_t *self) {
    gui_draw_text(self->x + 20, self->y + 40, g_error_msg, COLOR_FG);
    gui_draw_text(self->x + 20, self->y + 70, "PRESS ANY BUTTON", COLOR_FG);
}

static void error_update(ui_window_t *self) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (input_just_pressed(i)) {
            ui_pop();
            break;
        }
    }
}

void ui_show_error(const char *message) {
    g_error_msg = message;
    
    g_error_win.title = "ERROR";
    g_error_win.x = 40;
    g_error_win.y = 70;
    g_error_win.w = 240;
    g_error_win.h = 100;
    g_error_win.is_modal = 1;
    g_error_win.show_footer = 0;
    g_error_win.allow_idle_hide = 0;
    g_error_win.draw_content = error_draw;
    g_error_win.update_content = error_update;
    g_error_win.exit = NULL;
    
    ui_push(&g_error_win);
}

static const char **g_context_options = NULL;
static int g_context_count = 0;
static void (*g_context_on_select)(int index) = NULL;
static ui_window_t g_context_win;
static ui_list_t g_context_list;

static const char* context_get_label(int index) {
    return g_context_options[index];
}

static void context_on_action(int index) {
    ui_pop(); // Dismiss context menu before running action
    if (g_context_on_select) g_context_on_select(index);
}

static void context_draw(ui_window_t *self) {
    ui_list_draw(&g_context_list, self->x, self->y + 24, self->w, self->h - 24);
}

static void context_update(ui_window_t *self) {
    ui_list_update(&g_context_list);
}

void ui_show_context_menu(const char *title, const char **options, int count, void (*on_select)(int index)) {
    g_context_options = options;
    g_context_count = count;
    g_context_on_select = on_select;
    
    ui_list_init(&g_context_list, title, count, context_get_label, context_on_action);
    g_context_list.visible_lines = count;
    g_context_list.on_back = ui_pop;
    
    g_context_win.title = title;
    g_context_win.w = 140;
    g_context_win.h = count * 20 + 34; // 24 (title header) + count*20 + 10 (bottom padding)
    g_context_win.x = (320 - g_context_win.w) / 2;
    g_context_win.y = 22 + (196 - g_context_win.h) / 2;
    g_context_win.is_modal = 1;
    g_context_win.show_footer = 0;
    g_context_win.allow_idle_hide = 0;
    g_context_win.draw_content = context_draw;
    g_context_win.update_content = context_update;
    g_context_win.exit = NULL;
    
    ui_push(&g_context_win);
}
