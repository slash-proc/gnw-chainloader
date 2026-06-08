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
#include "i18n.h"
#include "theme.h"
#include "system/gui_api.h"
#include "system/bench.h"
#include "system/feature.h"
#include "../../common/boot_magic.h"
#include <string.h>

#define MAX_WINDOWS 8
#define UI_IDLE_TIMEOUT_MS 30000   /* main menu fades to ambient mode after this idle */
#define UI_FOOTER_TOP 218          /* top Y of the footer bar (accent line / fill start) */

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

/* Title the HEADER bar shows: the running APP window's title, NOT a pop-up's.
 * Modal pop-ups (confirm dialogs, context menus) carry their OWN attached box
 * title via the window chrome; they must not feed the header. So walk down the
 * stack past any modal windows on top to the parent app window and use ITS
 * title — opening a dialog over the File Browser keeps the File Browser's
 * header title showing. Empty stack / all-modal stack yields "". */
static const char *ui_header_title(void) {
    for (int i = g_stack_ptr; i >= 0; i--) {
        if (!g_window_stack[i]->is_modal) return g_window_stack[i]->title;
    }
    return "";
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
    gui_header_color = c->header ? c->header : c->footer;   /* 0 = match the footer */
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
    /* The Default slot paints the OFW look, which needs the decoded sprite
     * assets. If those failed to load (missing/corrupt OFW backup), fall back to
     * the neutral Fallback theme instead of a half-rendered OFW menu. Lives here,
     * not just in board_load_dynamic_assets, so it survives the re-apply that
     * theme_modules_init() triggers. */
    if (slot == THEME_SLOT_DEFAULT && !assets_loaded &&
        (board_console_type == CONSOLE_MARIO || board_console_type == CONSOLE_ZELDA))
        slot = THEME_SLOT_FALLBACK;
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
    if (slot == THEME_SLOT_DEFAULT)  return tr(STR_THEME_DEFAULT);
    if (slot == THEME_SLOT_FALLBACK) return tr(STR_THEME_FALLBACK);
    int mi = (int)slot - THEME_SLOT_MODULE_BASE;
    if (mi >= 0 && mi < g_theme_module_count && g_theme_modules[mi]->name)
        return g_theme_modules[mi]->name;   /* module name: a proper noun, literal */
    return tr(STR_THEME_DEFAULT);
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
    host.gui            = gui_api();   /* RTL-aware GUI services for the module */
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

int ui_stack_depth(void) { return g_stack_ptr; }

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
    feature_bg_tick();
    uint16_t prev_input = input_get_state();
    input_update();
    uint32_t now = HAL_GetTick();

    if (input_get_state() != prev_input) {
        g_last_activity_tick = now;
    }

    if (input_just_pressed(INPUT_PWR)) {
        menu_enter_standby();
    }

    if (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->update_content) {
        if (now - g_last_activity_tick <= UI_IDLE_TIMEOUT_MS || !g_window_stack[g_stack_ptr]->allow_idle_hide) {
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

    // Always draw header & footer on top of background / stack windows. The header
    // title belongs to the running app window, not a modal pop-up layered on top.
    ui_draw_header(ui_header_title());
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

/* Graphical 10-segment battery meter: a black-bordered cylinder with a + nub,
 * drawn entirely from rectangles (no sprite data). Color tracks the charge level;
 * it flashes near-dead and animates a rising fill while charging. */
#define BATT_W 23
#define BATT_H 12
static void ui_draw_battery_icon(int x, int y, int pct) {
    const uint16_t black = RGB565(0, 0, 0);

    uint16_t col;
    if (pct >= 50)      col = COLOR_GREEN;
    else if (pct >= 30) col = RGB565(0xE6, 0x7E, 0x22);   /* orange */
    else if (pct >= 15) col = RGB565(0xF1, 0xC4, 0x0F);   /* yellow */
    else                col = COLOR_RED;

    /* Body outline + the + terminal nub on the right end. */
    gui_draw_rect(x, y, BATT_W, BATT_H, black);
    gui_draw_fill_rect(x + BATT_W, y + 4, 2, 4, black);

    int filled = pct / 10;                 /* 0..10 segments, true level */
    if (filled > 10) filled = 10;

    for (int i = 0; i < filled; i++) {
        gui_draw_fill_rect(x + 2 + i * 2, y + 2, 1, BATT_H - 4, col);
    }
}

/* Tiny lightning bolt (charging indicator), drawn from six 1px rows. Sits left of
 * the battery so a plugged-in state is obvious even at a full, non-animating charge. */
#define BOLT_W 4
#define BOLT_H 6




/* RTL mirror of one piece's offset within the header readout cluster: in RTL the
 * piece is flipped to the opposite end (cluster_w - off - w), in LTR it's unchanged. */
static int mirror_off(int off, int w, int cluster_w) {
    return gui_is_rtl() ? cluster_w - off - w : off;
}

/* Draw the header bar (fill + divider line) and the title + battery/clock cluster.
 * The bar is always filled (matching the always-filled footer bar, so both ends of the
 * screen share the same dark backdrop); `solid` only picks the divider color: COLOR_FG
 * for an app/full-screen page, the gold accent for a floating menu page (mirroring the
 * footer's accent top line). The single header chrome path: the core's app header and a
 * feature module's header both render through it. */
static void ui_draw_header_impl(const char *title, bool solid) {
    /* Per-window header color: the active window may override the bar fill; 0 means
     * "use the theme header color" (gui_header_color, which most themes set to their
     * footer color so both ends match, but Yoshi sets to its sky-blue cloud bg). Gold
     * accent is kept for borders/dividers/selector, not as a fill behind the title. */
    uint16_t hdr_color = (g_stack_ptr >= 0 && g_window_stack[g_stack_ptr]->header_color)
                       ? g_window_stack[g_stack_ptr]->header_color : gui_header_color;

    gui_draw_fill_rect(0, 0, SCREEN_WIDTH, 22, hdr_color);

    // Draw header divider line
    gui_draw_rect(0, 22, SCREEN_WIDTH, 1, solid ? COLOR_FG : gui_accent_color);

    int pct;
    bool charging;
    board_battery_update(&pct, &charging);

    char pctstr[12];
    int_to_str(pct, pctstr);                       /* "100" */
    str_lcat(pctstr, sizeof(pctstr), "%");         /* "100%" */
    int pct_w = gui_text_width(pctstr);

    /* Minimal 24h clock: BCD nibbles of RTC->TR straight to ASCII, no division. */
    uint32_t tr = RTC->TR;
    char clk[6];
    clk[0] = '0' + ((tr >> 20) & 3);               /* hours tens  */
    clk[1] = '0' + ((tr >> 16) & 0xF);             /* hours units */
    clk[2] = ':';
    clk[3] = '0' + ((tr >> 12) & 7);               /* mins tens   */
    clk[4] = '0' + ((tr >>  8) & 0xF);             /* mins units  */
    clk[5] = 0;

    /* The right-side readout is one cluster, left-to-right: [HH:MM] gap [bolt]
     * [icon] gap [NN%]. The bolt's slot is ALWAYS reserved so the clock, icon and
     * % never shift when charging toggles. Anchor the % 20px from the edge (same
     * margin as the title), then mirror the whole box — and each piece — for RTL. */
    const int clk_w = 5 * 6;                       /* monospaced "HH:MM", 5 glyphs */
    const int icon_w = BATT_W + 2;                 /* body + nub */
    const int gap = 6;
    const int clk_gap = 9;                         /* roomier split between clock and battery */
    const int bolt_total = BOLT_W + 3;             /* bolt + 3px gap, always reserved */
    int cluster_w = clk_w + clk_gap + bolt_total + icon_w + gap + pct_w;
    int left = SCREEN_WIDTH - cluster_w - 20;
    int cluster_x = gui_mirror_x(left, cluster_w, 0, SCREEN_WIDTH);
    int max_w = left - 30;
    if (max_w < 50) max_w = 50;

    int clk_off  = mirror_off(0, clk_w, cluster_w);
    int bolt_off = mirror_off(clk_w + clk_gap, BOLT_W, cluster_w);
    int icon_off = mirror_off(clk_w + clk_gap + bolt_total, icon_w, cluster_w);
    int pct_off  = mirror_off(clk_w + clk_gap + bolt_total + icon_w + gap, pct_w, cluster_w);

    gui_draw_text_aligned(gui_mirror_x(20, max_w, 0, SCREEN_WIDTH), 7, max_w, title, COLOR_FG, true, 0);
    gui_draw_text(cluster_x + clk_off, 7, clk, COLOR_FG);
    if (charging) {
        int bx = cluster_x + bolt_off;
        gui_draw_fill_rect(bx + 2, 8,  2, 1, COLOR_FG);
        gui_draw_fill_rect(bx + 1, 9,  2, 1, COLOR_FG);
        gui_draw_fill_rect(bx,     10, 4, 1, COLOR_FG);
        gui_draw_fill_rect(bx + 2, 11, 2, 1, COLOR_FG);
        gui_draw_fill_rect(bx + 1, 12, 2, 1, COLOR_FG);
        gui_draw_fill_rect(bx,     13, 2, 1, COLOR_FG);
    }
    ui_draw_battery_icon(cluster_x + icon_off, 5, pct);
    gui_draw_text(cluster_x + pct_off, 7, pctstr, COLOR_FG);
    if (feature_is_bg_active()) {
        int note_x = gui_is_rtl() ? (cluster_x + cluster_w + 5) : (cluster_x - 12);
        gui_draw_fill_rect(note_x + 2, 8,  1, 5, COLOR_FG);
        gui_draw_fill_rect(note_x + 3, 8,  2, 1, COLOR_FG);
        gui_draw_fill_rect(note_x,     12, 3, 2, COLOR_FG);
    }
}

void ui_draw_header(const char *title) {
    /* The solid filled bar is drawn for a full-width app page (or an empty stack);
     * a floating page over the animated background gets the unfilled, accent-divider
     * look. */
    ui_draw_header_impl(title, ui_app_active() || g_stack_ptr < 0);
}

/* Force the solid (filled) header — a feature module runs over a narrow launching
 * menu (ui_app_active() is false there), but its full-screen chrome needs the filled
 * bar, so it routes through this rather than re-implementing the fill. */
void ui_draw_header_solid(const char *title) {
    ui_draw_header_impl(title, true);
}

/* The themed footer bar (accent line at UI_FOOTER_TOP + footer-color fill below) plus
 * an optional centered hint legend at y=225. Single shared chrome path: the core's
 * non-app footer renders through it (NULL hint, then layers theme decoration on top),
 * and a feature module's footer renders through it with its hint. */
void ui_draw_footer_chrome(const char *hint) {
    gui_draw_rect(0, UI_FOOTER_TOP, SCREEN_WIDTH, 1, gui_accent_color);
    gui_draw_fill_rect(0, UI_FOOTER_TOP + 1, SCREEN_WIDTH, 21, COLOR_FOOTER);
    if (hint && hint[0]) {
        int w = gui_text_width(hint);
        gui_draw_text((SCREEN_WIDTH - w) / 2, 225, hint, COLOR_FG);
    }
}

static void ui_draw_footer_bar(void) {
    bool app_active = ui_app_active();
    bool show_footer = (g_stack_ptr >= 0) ? g_window_stack[g_stack_ptr]->show_footer : false;

    if (app_active) {
        if (show_footer) {
            gui_draw_fill_rect(0, UI_FOOTER_TOP, SCREEN_WIDTH, 22, COLOR_FOOTER);
            gui_draw_rect(0, UI_FOOTER_TOP, SCREEN_WIDTH, 1, COLOR_FG);
        }
    } else {
        /* Solid themed footer bar (accent line + footer color) on every screen... */
        ui_draw_footer_chrome(NULL);
        /* ...then a module theme paints decoration over it (e.g. brick + Yoshi). */
        theme_draw_footer(HAL_GetTick());
    }
}

/* Top Y of the footer bar when the active full-width app window shows one, else 0.
 * The split list divider uses this to stop at the footer line. Only the app-active
 * footer (a list page with show_footer) clips the divider; a non-app overlay footer
 * isn't part of a split-list page. */
int ui_footer_top(void) {
    bool show_footer = (g_stack_ptr >= 0) ? g_window_stack[g_stack_ptr]->show_footer : false;
    return (ui_app_active() && show_footer) ? UI_FOOTER_TOP : 0;
}

static void ui_draw_window_chrome(ui_window_t *win) {
    if (win->is_modal) {
        gui_draw_fill_rect(win->x, win->y, win->w, win->h, COLOR_BG);
    } else {
        gui_draw_blend_rect(win->x, win->y, win->w, win->h);
    }
    gui_draw_rect(win->x, win->y, win->w, win->h, COLOR_BORDER);
    
    // Draw optional title header inside floating window/modal (right-aligned in RTL)
    if (win->title && win->is_modal) {
        gui_draw_text_aligned(win->x + 10, win->y + 8, win->w - 20, win->title, COLOR_FG, false, 0);
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
    gui_draw_text(20, 70, tr(partition_current_phase), COLOR_FG);
    gui_draw_progress_bar(20, 110, 280, 18, pct, COLOR_BORDER, COLOR_BORDER);
    char pct_buf[8];
    int_to_str(pct, pct_buf);
    strcat(pct_buf, "%");
    gui_draw_text(148, 135, pct_buf, COLOR_FG);
    ui_draw_footer(tr(STR_FOOTER_CANCEL));
}

/* Shared geometry/flags for the centered 240x100 modal dialogs (confirm + message):
 * fixed box, modal, no footer, no idle-hide, no exit hook. The differing draw/update
 * callbacks stay at each call site. */
static void init_modal_window(ui_window_t *w, const char *title) {
    w->title = title;
    w->x = 40;
    w->y = 70;
    w->w = 240;
    w->h = 100;
    w->is_modal = 1;
    w->show_footer = 0;
    w->allow_idle_hide = 0;
    w->exit = NULL;
}

static const char *g_confirm_msg = NULL;
static void (*g_confirm_callback)(void) = NULL;
static ui_window_t g_confirm_win;

static void confirm_draw(ui_window_t *self) {
    gui_draw_text_aligned(self->x + 20, self->y + 40, self->w - 40, g_confirm_msg, COLOR_FG, false, 0);
    gui_draw_text_aligned(self->x + 20, self->y + 70, self->w - 40, tr(STR_YES_NO), COLOR_FG, false, 0);
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
    
    init_modal_window(&g_confirm_win, tr(STR_CONFIRM));
    g_confirm_win.draw_content = confirm_draw;
    g_confirm_win.update_content = confirm_update;

    ui_push(&g_confirm_win);
}

static const char *g_error_msg = NULL;
static ui_window_t g_error_win;

static void error_draw(ui_window_t *self) {
    gui_draw_text_aligned(self->x + 20, self->y + 40, self->w - 40, g_error_msg, COLOR_FG, false, 0);
    gui_draw_text_aligned(self->x + 20, self->y + 70, self->w - 40, tr(STR_PRESS_ANY), COLOR_FG, false, 0);
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

    init_modal_window(&g_error_win, title);
    g_error_win.draw_content = error_draw;
    g_error_win.update_content = error_update;

    ui_push(&g_error_win);
}

void ui_show_error(const char *message) {
    show_message_modal(tr(STR_ERROR), message);
}

/* A neutral-titled boot notice (the SD install summary). */
void ui_show_notice(const char *message) {
    show_message_modal(tr(STR_NOTICE_LANGUAGES), message);
}

/* Module UI service: hand PIE modules the core's confirm/error modals so they can
 * talk to the user. See system/mod_ui.h. */
#include "system/mod_ui.h"
static const mod_ui_t g_mod_ui = { ui_show_confirm, ui_show_error };
const mod_ui_t *mod_ui(void) { return &g_mod_ui; }

/* i18n seam for modules (gui_api): reuse the core's active translations + report the active
 * locale code so a module can localize via shared core words AND pick its own compiled-in
 * translation column. */
static const char *gui_api_tr(int id) { return tr((string_id_t)id); }
static const char *gui_api_lang_code(void) { return i18n_code(i18n_current()); }

/* GUI-services vtable handed to PIE modules (see system/gui_api.h): the core's
 * RTL-aware drawing primitives + modals, so a module renders in the core's style with
 * mirroring handled for free. Points at the gui_* / ui_show_* functions. */
static const gui_api_t g_gui_api = {
    .is_rtl            = gui_is_rtl,
    .mirror_x          = gui_mirror_x,
    .text_width        = gui_text_width,
    .draw_text         = gui_draw_text,
    .draw_text_aligned = gui_draw_text_aligned,
    .draw_text_marquee = gui_draw_text_marquee,
    .draw_char         = gui_draw_char,
    .draw_selector     = gui_draw_selector,
    .draw_rect         = gui_draw_rect,
    .fill_rect         = gui_draw_fill_rect,
    .blend_rect        = gui_draw_blend_rect,
    .draw_progress_bar = gui_draw_progress_bar,
    .draw_sprite       = gui_draw_sprite,
    .color_bg          = gui_color_bg,
    .color_fg          = gui_color_fg,
    .color_accent      = gui_color_accent,
    .color_border      = gui_color_border,
    .color_footer      = gui_color_footer,
    .framebuffer       = gui_current_framebuffer,
    .refresh           = gui_refresh,
    .confirm           = ui_show_confirm,
    .error             = ui_show_error,
    .notice            = ui_show_notice,
    .context_menu      = ui_show_context_menu,
    .tr                = gui_api_tr,
    .lang_code         = gui_api_lang_code,
};
const gui_api_t *gui_api(void) { return &g_gui_api; }

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
