#ifndef SYSTEM_GUI_API_H
#define SYSTEM_GUI_API_H

#include <stdint.h>
#include <stdbool.h>

/*
 * GUI-services vtable: the core hands PIE modules its drawing primitives + modals so a
 * module can render arbitrary UI in the core's style WITHOUT bundling its own font or
 * renderer, and with right-to-left handled for free (is_rtl / mirror_x / the aligned
 * text draw honour the active language's direction, so a module's UI mirrors exactly
 * like the core's). The core fills it once (gui_api()) pointing at the gui_* / ui_show_*
 * functions; a module receives a pointer through its host-API struct (e.g.
 * theme_host_api_t.gui) and calls host->gui->draw_text(...), host->gui->is_rtl(), etc.
 *
 * Coordinates are screen pixels (320x240); colors are RGB565. APPEND-ONLY: add new
 * entries at the END so a module built against an older copy stays valid.
 */
typedef struct gui_api {
    /* direction + measurement */
    bool (*is_rtl)(void);
    int  (*mirror_x)(int x, int elem_w, int box_x, int box_w);
    int  (*text_width)(const char *str);
    /* text */
    void (*draw_text)(int x, int y, const char *str, uint16_t color);
    void (*draw_text_aligned)(int x, int y, int w, const char *str, uint16_t color, bool is_active, uint32_t tick);
    void (*draw_text_marquee)(int x, int y, int max_w, const char *str, uint16_t color, bool is_active, uint32_t tick);
    void (*draw_char)(int x, int y, uint32_t cp, uint16_t color);
    void (*draw_selector)(int x, int y, uint16_t color);
    /* primitives */
    void (*draw_rect)(int x, int y, int w, int h, uint16_t color);
    void (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    void (*blend_rect)(int x, int y, int w, int h);
    void (*draw_progress_bar)(int x, int y, int w, int h, int percent, uint16_t border, uint16_t fill);
    void (*draw_sprite)(int x, int y, int w, int h, const uint8_t *data, bool transparent, bool flip, int pitch);
    /* theme colors (track the active theme) */
    uint16_t (*color_bg)(void);
    uint16_t (*color_fg)(void);
    uint16_t (*color_accent)(void);
    uint16_t (*color_border)(void);
    uint16_t (*color_footer)(void);
    /* frame */
    uint16_t *(*framebuffer)(void);   /* current 320x240 RGB565 back buffer (swaps each frame) */
    void (*refresh)(void);            /* present the frame (the core drives this in its loop) */
    /* modals (async: push a window and return; the callback is resident module code) */
    void (*confirm)(const char *message, void (*on_yes)(void));
    void (*error)(const char *message);
    void (*notice)(const char *message);
    void (*context_menu)(const char *title, const char **options, int count, void (*on_select)(int index));
    /* i18n: reuse the core's ACTIVE translations (id is a string_id_t) for shared words, and
     * read the active locale code (e.g. "de_DE", "en_US" default) so a module can pick its own
     * compiled-in translation column for its module-specific strings. APPEND-ONLY. */
    const char *(*tr)(int id);
    const char *(*lang_code)(void);
} gui_api_t;

/* The core's GUI-services vtable (filled with the RTL-aware gui_* / ui_show_* fns). */
const gui_api_t *gui_api(void);

#endif /* SYSTEM_GUI_API_H */
