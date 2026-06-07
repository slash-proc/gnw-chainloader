#include "ui.h"
#include "gui.h"
#include "board.h"
#include "input.h"
#include "assets.h"
#include "utils.h"
#include "partition.h"
#include "menu.h"
#include "ui_list.h"
#include "strings.h"
#include "theme.h"
#include "system/bench.h"
#include "../../common/boot_magic.h"
#include <string.h>

#define MAX_WINDOWS 8
#define UI_IDLE_TIMEOUT_MS 30000   /* main menu fades to ambient mode after this idle */

bool ui_operation_in_progress = false;
uint8_t ui_theme_slot = THEME_SLOT_DEFAULT;

static ui_window_t *g_window_stack[MAX_WINDOWS];
static int g_stack_ptr = -1;
static uint32_t g_last_activity_tick = 0;

static void ui_draw_footer_bar(void);
static void ui_draw_window_chrome(ui_window_t *win);

/* The "app" is active when the topmost window is a non-modal full-width (320px)
 * page (the main / tools / settings menus), as opposed to a floating modal or
 * dialog. Drives whether the header bar, footer, and idle animations render. */
static bool ui_app_active(void) {
    return g_stack_ptr >= 0 && !g_window_stack[g_stack_ptr]->is_modal
           && g_window_stack[g_stack_ptr]->w == 320;
}

void ui_init(void) {
    g_stack_ptr = -1;
    ui_operation_in_progress = false;
    g_last_activity_tick = HAL_GetTick();
}

/* ------------------------------- Theme model ------------------------------ */
/* Color-only in-core themes (no sprite/blitter code in the 40K core). DEFAULT
 * resolves to the loaded OFW's signature colors; FALLBACK is neutral. Module
 * themes (slots 2+) register via theme_register() and bring their own colors +
 * sprite hooks. */
static const theme_colors_t FALLBACK_COLORS =
    { RGB565(0x20,0x24,0x28), RGB565(0xD0,0xD4,0xD8), RGB565(0x00,0xA0,0xA0),
      RGB565(0xD0,0xD4,0xD8), RGB565(0x14,0x16,0x1A) };
static const theme_colors_t ZELDA_COLORS =
    { RGB565(0x1E,0x60,0x30), RGB565(0xFF,0xFF,0xFF), RGB565(0xE5,0xB8,0x3B),
      RGB565(0xE5,0xB8,0x3B), RGB565(0x0F,0x30,0x18) };
static const theme_colors_t MARIO_COLORS =
    { RGB565(0xA0,0x20,0x18), RGB565(0xFF,0xFF,0xFF), RGB565(0xE5,0xB8,0x3B),
      RGB565(0xE5,0xB8,0x3B), RGB565(0x40,0x0C,0x08) };

/* Up to 6 module themes per OFW; theme selection persists per-OFW in a nibble
 * (see boot_magic.h): slot 0 = Default, 1 = Fallback, 2-7 = module. */
#define MAX_THEME_MODULES 6
static theme_driver_t *g_theme_modules[MAX_THEME_MODULES];
static int g_theme_module_count = 0;
static theme_driver_t *g_active_theme = NULL;   /* non-NULL only for a module theme */

/* True when a MODULE theme is selected but its module isn't loaded yet (early
 * boot, before theme_modules_init). While set, the UI draws only black + a tiny
 * loading bar instead of the menu, so the correct theme appears in one clean
 * transition with no default-color flash. Cleared once the module applies. */
static bool g_theme_pending = false;
/* Set once theme_modules_init() has ATTEMPTED the module load. Before it, an
 * unavailable module slot means "still loading" (-> pending/black). After it, an
 * unavailable slot means the module is genuinely absent -> fall back to default,
 * never staying black, so the menu is always reachable even if theme.bin is
 * missing/corrupt. */
static bool g_modules_init_done = false;

/* Pending-wait rendering: hold a single black frame for the brief common case
 * (no repeated LTDC buffer swaps — those reload at the bottom scanline and
 * flicker the bottom edge), and only raise the loading bar if the wait runs long
 * (empty/odd flash). Timer starts at the FIRST pending frame (menu start). */
#define THEME_PENDING_BAR_MS 200
static uint32_t g_theme_pending_start = 0;
static bool g_pending_painted = false;

void theme_register(theme_driver_t *t) {
    if (t && g_theme_module_count < MAX_THEME_MODULES)
        g_theme_modules[g_theme_module_count++] = t;
}

int ui_theme_slot_count(void) { return THEME_SLOT_MODULE_BASE + g_theme_module_count; }

static const theme_colors_t *default_colors(void) {
    if (board_console_type == CONSOLE_ZELDA) return &ZELDA_COLORS;
    if (board_console_type == CONSOLE_MARIO) return &MARIO_COLORS;
    return &FALLBACK_COLORS;
}

static void apply_colors(const theme_colors_t *c) {
    gui_bg_color     = c->bg;
    gui_fg_color     = c->fg;
    gui_accent_color = c->accent;
    gui_border_color = c->border;
    gui_footer_color = c->footer;
}

void ui_set_theme_slot(uint8_t slot) {
    int mi = (int)slot - THEME_SLOT_MODULE_BASE;
    if (slot >= THEME_SLOT_MODULE_BASE && mi < g_theme_module_count) {
        g_active_theme = g_theme_modules[mi];
        apply_colors(&g_active_theme->colors);
        ui_theme_slot = slot;
    } else if (slot == THEME_SLOT_FALLBACK) {
        g_active_theme = NULL;
        apply_colors(&FALLBACK_COLORS);
        ui_theme_slot = THEME_SLOT_FALLBACK;
    } else {                       /* DEFAULT, or a module slot with no module loaded */
        g_active_theme = NULL;
        apply_colors(default_colors());
        ui_theme_slot = THEME_SLOT_DEFAULT;
    }
}

void ui_update_theme(void) {
    /* Restore the slot persisted for the loaded OFW (Zelda/Mario have their own
     * nibble; an invalid/wiped word yields slot 0 = Default). */
    uint32_t w = board_rtc_read_settings();
    uint8_t slot = (board_console_type == CONSOLE_ZELDA) ? settings_zelda_slot(w)
                 : (board_console_type == CONSOLE_MARIO) ? settings_mario_slot(w)
                 : THEME_SLOT_DEFAULT;
    /* If a module theme is selected but its module isn't registered yet (this
     * runs early in board_load_dynamic_assets, before theme_modules_init), do NOT
     * paint the OFW default as a placeholder — that is the red flash. Stay black +
     * mark pending; ui_draw() then shows only black + the loading bar until the
     * module loads and this runs again with it registered. In-core slots
     * (default/fallback) need no module, so they apply immediately — no flash. */
    if (!g_modules_init_done && slot >= THEME_SLOT_MODULE_BASE &&
        (int)slot - THEME_SLOT_MODULE_BASE >= g_theme_module_count) {
        if (!g_theme_pending) g_pending_painted = false;  /* entering the wait */
        g_theme_pending = true;
        ui_theme_slot = slot;             /* remember the desired slot */
        gui_bg_color = RGB565(0, 0, 0);   /* black wait screen */
        return;
    }
    g_theme_pending = false;
    ui_set_theme_slot(slot);
}

/* Persist the current slot for the active OFW (read-modify-write the shared
 * nibble; other OFW's slot + fast-boot bit preserved). No-op for an unknown OFW
 * (it has no slot in the register). */
static void theme_persist(void) {
    if (board_console_type != CONSOLE_ZELDA && board_console_type != CONSOLE_MARIO) return;
    uint32_t w = board_rtc_read_settings();
    uint8_t mario = settings_mario_slot(w), zelda = settings_zelda_slot(w);
    if (board_console_type == CONSOLE_ZELDA) zelda = ui_theme_slot & SETTINGS_SLOT_MASK;
    else                                     mario = ui_theme_slot & SETTINGS_SLOT_MASK;
    board_rtc_write_settings(settings_make(settings_fastboot(w), mario, zelda, settings_lang(w)));
}

const char *ui_theme_slot_name(uint8_t slot) {
    if (slot == THEME_SLOT_DEFAULT)  return "DEFAULT";
    if (slot == THEME_SLOT_FALLBACK) return "FALLBACK";
    int mi = (int)slot - THEME_SLOT_MODULE_BASE;
    if (mi >= 0 && mi < g_theme_module_count && g_theme_modules[mi]->name)
        return g_theme_modules[mi]->name;
    return "DEFAULT";
}

/* Selector order shown to the user: DEFAULT, module themes..., FALLBACK (last). */
static uint8_t ordered_slot(int pos) {
    if (pos == 0) return THEME_SLOT_DEFAULT;
    if (pos <= g_theme_module_count) return (uint8_t)(THEME_SLOT_MODULE_BASE + (pos - 1));
    return THEME_SLOT_FALLBACK;
}
static int slot_pos(uint8_t slot) {
    if (slot == THEME_SLOT_DEFAULT)  return 0;
    if (slot == THEME_SLOT_FALLBACK) return 1 + g_theme_module_count;
    return 1 + ((int)slot - THEME_SLOT_MODULE_BASE);
}
void ui_theme_cycle(int dir) {
    int n = ui_theme_slot_count();
    ui_set_theme_slot(ordered_slot(((slot_pos(ui_theme_slot) + dir) % n + n) % n));
    theme_persist();
}

/* Render-site helpers: a module theme's sprite hook, else the core's plain look.
 * Hooks blit from the OFW tileset, so they run only when assets are loaded. */
bool theme_draw_selector(int x, int y, uint32_t tick) {
    if (g_active_theme && g_active_theme->draw_selector && assets_loaded) {
        g_active_theme->draw_selector(x, y, tick);
        return true;
    }
    return false;
}
void theme_draw_footer(uint32_t tick) {
    if (g_active_theme && g_active_theme->draw_footer && assets_loaded)
        g_active_theme->draw_footer(tick);
}
void theme_draw_background(uint32_t tick) {
    if (g_active_theme && g_active_theme->draw_background && assets_loaded)
        g_active_theme->draw_background(tick);
}

/* Load the bundled theme module from internal storage. It registers the sprite
 * theme(s) for the loaded OFW (slots 2+); absent/failed load is a graceful no-op
 * (the in-core color themes remain). Called once at startup after vfs_init. */
void theme_modules_init(void) {
    /* Re-runnable: clear the registry so an OFW switch re-registers the new
     * OFW's themes (old module copy lingers harmlessly in the bump pool). */
    g_theme_module_count = 0;
    g_active_theme = NULL;

    static theme_host_api_t host;   /* must outlive the module (it keeps the ptr) */
    host.get_tick       = HAL_GetTick;
    host.register_theme = theme_register;
    host.ofw            = (board_console_type == CONSOLE_ZELDA) ? THEME_OFW_ZELDA
                        : (board_console_type == CONSOLE_MARIO) ? THEME_OFW_MARIO
                        : THEME_OFW_NONE;
    host.framebuffer    = gui_current_framebuffer;
    host.palette        = dynamic_palette;
    host.tileset        = dynamic_tileset;
    mod_load_theme("/modules/theme/theme.bin", &host);
    /* Module load attempted: from here ui_update_theme() applies the real theme,
     * or falls back to default if the module is absent — never leaves the screen
     * pending/black (the menu must always be reachable). */
    g_modules_init_done = true;
    /* Re-apply the persisted slot now that module themes are registered, so a
     * saved module-theme selection takes effect on boot (no-op if it was 0/1). */
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

    bool was_idle = (now - g_last_activity_tick > UI_IDLE_TIMEOUT_MS);
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
    /* Module theme selected but not loaded yet: black screen + a tiny centered
     * white "loading" bar (no menu, no color flash) until it applies. The bar
     * tracks partition-scan progress, so a stall shows rather than freezes. */
    if (g_theme_pending) {
        uint32_t now = HAL_GetTick();
        if (!g_pending_painted) {
            g_theme_pending_start = now;   /* start the wait timer (one-shot) */
            g_pending_painted = true;
        }
        if (now - g_theme_pending_start < THEME_PENDING_BAR_MS) {
            /* Hold the black frame gui_init() already pre-filled: draw nothing and
             * DON'T refresh, so the LTDC performs NO buffer swap during the brief
             * wait. (A redundant black->black swap here reloaded the bottom
             * scanline — the likely bottom flicker.) The first real swap is the
             * themed menu itself. */
            return;
        }
        /* Long wait (empty/odd flash): raise the tiny loading bar so a stall is
         * visible; refresh with scan progress until the module loads. */
        gui_fill(RGB565(0, 0, 0));
        int done = 0, total = 0;
        partition_scan_get_progress(&done, &total);
        int pct = (total > 0) ? (done * 100 / total) : 0;
        gui_draw_progress_bar(140, 117, 40, 6, pct,
                              RGB565(0xFF, 0xFF, 0xFF), RGB565(0xFF, 0xFF, 0xFF));
        gui_refresh();
        return;
    }

#ifdef BOOT_BENCH
    { static bool first_themed = true; if (first_themed) { first_themed = false; BENCH_MARK(5); } }
#endif
    ui_draw_background();
    
    uint32_t now = HAL_GetTick();
    bool hide_idle = (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->allow_idle_hide && (now - g_last_activity_tick >= UI_IDLE_TIMEOUT_MS));

    if (!hide_idle && g_stack_ptr >= 0) {
        int start_idx = g_stack_ptr;
        while (start_idx > 0 && g_window_stack[start_idx]->is_modal) {
            start_idx--;
        }
        for (int i = start_idx; i <= g_stack_ptr; i++) {
            ui_window_t *win = g_window_stack[i];
            
            if (win->is_modal) {
                // Dim/frost the screen behind the modal (within workspace area)
                gui_draw_blend_rect(0, 22, 320, 196);
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

    bool app_active = ui_app_active();
    if (!app_active) {
        ui_draw_animations();
    }
}

void ui_draw_animations(void) {
    theme_draw_background(HAL_GetTick());
}

void ui_draw_header(const char *title) {
    bool app_active = ui_app_active();
    
    if (app_active || g_stack_ptr < 0) {
        gui_draw_fill_rect(0, 0, SCREEN_WIDTH, 22, gui_accent_color);
    }
    
    // Draw header divider line
    gui_draw_rect(0, 22, SCREEN_WIDTH, 1, (app_active || g_stack_ptr < 0) ? COLOR_FG : gui_accent_color);

    char batt_str[32];
    int pct;
    bool charging;
    board_battery_update(&pct, &charging);
    
    strcpy(batt_str, tr(STR_BATT));
    int_to_str(pct, batt_str + strlen(batt_str));
    strcat(batt_str, "%");
    if (charging) strcat(batt_str, tr(STR_CHARGING));

    int batt_x = SCREEN_WIDTH - gui_text_width(batt_str) - 4;
    int max_w = batt_x - 30;
    if (max_w < 50) max_w = 50;

    gui_draw_text_marquee(20, 7, max_w, title, COLOR_FG, true, 0);
    gui_draw_text(batt_x, 7, batt_str, COLOR_FG);
}

static void ui_draw_footer_bar(void) {
    bool app_active = ui_app_active();
    bool show_footer = (g_stack_ptr >= 0) ? g_window_stack[g_stack_ptr]->show_footer : false;
    
    if (app_active) {
        if (show_footer) {
            gui_draw_fill_rect(0, 218, SCREEN_WIDTH, 22, COLOR_FOOTER);
            gui_draw_rect(0, 218, SCREEN_WIDTH, 1, COLOR_FG);
        }
    } else {
        /* Solid themed footer bar (accent line + footer color) on every screen... */
        gui_draw_rect(0, 218, SCREEN_WIDTH, 1, gui_accent_color);
        gui_draw_fill_rect(0, 219, SCREEN_WIDTH, 21, COLOR_FOOTER);
        /* ...then a module theme paints decoration over it (e.g. brick + Yoshi). */
        theme_draw_footer(HAL_GetTick());
    }
}

static void ui_draw_window_chrome(ui_window_t *win) {
    if (win->is_modal) {
        gui_draw_fill_rect(win->x, win->y, win->w, win->h, COLOR_BG);
    } else {
        gui_draw_blend_rect(win->x, win->y, win->w, win->h);
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
        int text_w = gui_text_width(text);
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
    gui_draw_text(20, 40, tr(STR_SCANNING), COLOR_FG);
    gui_draw_text(20, 70, partition_current_phase, COLOR_FG);
    gui_draw_progress_bar(20, 110, 280, 18, pct, COLOR_BORDER, COLOR_BORDER);
    char pct_buf[8];
    int_to_str(pct, pct_buf);
    strcat(pct_buf, "%");
    gui_draw_text(148, 135, pct_buf, COLOR_FG);
    ui_draw_footer(tr(STR_FOOTER_CANCEL));
}

static const char *g_confirm_msg = NULL;
static void (*g_confirm_callback)(void) = NULL;
static ui_window_t g_confirm_win;

static void confirm_draw(ui_window_t *self) {
    gui_draw_text(self->x + 20, self->y + 40, g_confirm_msg, COLOR_FG);
    gui_draw_text(self->x + 20, self->y + 70, tr(STR_YES_NO), COLOR_FG);
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
    
    g_confirm_win.title = tr(STR_CONFIRM);
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
    gui_draw_text(self->x + 20, self->y + 70, tr(STR_PRESS_ANY), COLOR_FG);
}

static void error_update(ui_window_t *self) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (input_just_pressed(i)) {
            ui_pop();
            break;
        }
    }
}

/* Shared modal setup for the dismiss-on-any-key message window (error + notice). */
static void show_message_modal(const char *title, const char *message) {
    g_error_msg = message;

    g_error_win.title = title;
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

void ui_show_error(const char *message) {
    show_message_modal(tr(STR_ERROR), message);
}

/* A neutral-titled boot notice (the language auto-install summary). English. */
void ui_show_notice(const char *message) {
    show_message_modal("LANGUAGES", message);
}

/* Module UI service: hand PIE modules the core's confirm/error modals so they can
 * talk to the user. See system/mod_ui.h. */
#include "system/mod_ui.h"
static const mod_ui_t g_mod_ui = { ui_show_confirm, ui_show_error };
const mod_ui_t *mod_ui(void) { return &g_mod_ui; }

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
