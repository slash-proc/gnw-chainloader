#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>

/* Theme model lives in theme.h (slot-based: Default/Fallback in-core + module
 * themes). ui_update_theme() below applies the OFW-appropriate / persisted slot. */

extern bool ui_operation_in_progress;

typedef struct ui_window {
    const char *title;
    int16_t x, y, w, h;
    uint8_t is_modal : 1;
    uint8_t show_footer : 1;
    uint8_t allow_idle_hide : 1;
    /* Per-window header bar fill color. 0 = use the theme header color (gui_header_color),
     * so existing screens are unchanged; a screen sets this to override the bar. */
    uint16_t header_color;

    void (*enter)(struct ui_window *self);
    void (*draw_content)(struct ui_window *self);
    void (*update_content)(struct ui_window *self);
    void (*exit)(struct ui_window *self);

    void *user_data;
} ui_window_t;

void ui_init(void);
void ui_update_theme(void);          /* apply the OFW-appropriate / persisted theme slot */
void ui_push(ui_window_t *win);
void ui_pop(void);
void ui_switch(ui_window_t *win);
int  ui_stack_depth(void);   /* current window-stack index (-1 = empty); for modal pumps */

/* File browser (ui_file_browser.c). browser_open_picker: pick-only mode filtered to `ext`
 * ("" = all); choosing a file invokes on_pick(path, is_dir). */
void browser_open(void);
void browser_open_picker(const char *ext, void (*on_pick)(const char *path, bool is_dir, uint32_t size));
void browser_picker_restore(void);   /* restore the main browser after a nested picker closed */
const void *browser_active_partition(void);  /* picked file's partition_info_t (opaque here) */

/* Pulsed by main loop */
void ui_update(void);
void ui_draw(void);

/* Shared UI Helpers */
void ui_draw_background(void);
void ui_draw_animations(void);
void ui_draw_header(const char *title);
/* Force the solid (filled-bar, COLOR_FG divider) header regardless of the active
 * window — a feature module's full-screen chrome routes through this. */
void ui_draw_header_solid(const char *title);
void ui_draw_footer(const char *text);
/* Top Y of the footer bar when the active window shows one, else 0. Lets the split
 * list divider stop at the footer line instead of running into the footer area. */
int  ui_footer_top(void);
/* Draw the themed footer bar (accent line + footer fill) with an optional centered
 * hint legend (NULL = no legend). The single chrome path the core's non-app footer
 * and a feature module's footer both render through, so the bar fill lives in one
 * place. Theme footer decoration is layered by the caller, not here. */
void ui_draw_footer_chrome(const char *hint);
void ui_draw_scan_progress(void);
void ui_show_confirm(const char *message, void (*on_confirm)(void));
void ui_show_error(const char *message);
/* Like ui_show_error but with a neutral "LANGUAGES" title — a dismissible boot
 * notice (e.g. "Updated N languages"). Press any button to clear. */
void ui_show_notice(const char *message);
void ui_show_context_menu(const char *title, const char **options, int count, void (*on_select)(int index));

#endif // UI_H
