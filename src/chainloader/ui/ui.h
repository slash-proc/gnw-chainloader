#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UI_THEME_DEFAULT = 0,
    UI_THEME_MARIO,
    UI_THEME_ZELDA,
} ui_theme_t;

extern ui_theme_t ui_current_theme;
extern bool ui_theme_enabled;

extern bool ui_operation_in_progress;

typedef struct ui_window {
    const char *title;
    int16_t x, y, w, h;
    uint8_t is_modal : 1;
    uint8_t show_footer : 1;
    uint8_t allow_idle_hide : 1;

    void (*enter)(struct ui_window *self);
    void (*draw_content)(struct ui_window *self);
    void (*update_content)(struct ui_window *self);
    void (*exit)(struct ui_window *self);

    void *user_data;
} ui_window_t;

void ui_init(void);
void ui_update_theme(void);
void ui_theme_toggle(void);
void ui_push(ui_window_t *win);
void ui_pop(void);
void ui_switch(ui_window_t *win);

/* Pulsed by main loop */
void ui_update(void);
void ui_draw(void);

/* Shared UI Helpers */
void ui_draw_background(void);
void ui_draw_animations(void);
void ui_draw_header(const char *title);
void ui_draw_footer(const char *text);
void ui_draw_scan_progress(void);
void ui_show_confirm(const char *message, void (*on_confirm)(void));
void ui_show_error(const char *message);
void ui_show_context_menu(const char *title, const char **options, int count, void (*on_select)(int index));

#endif // UI_H
