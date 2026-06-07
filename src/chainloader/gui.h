#ifndef GUI_H
#define GUI_H

#include <stdint.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

/* The LTDC layer runs in RGB565 (16bpp), the same format the stock OFW and
   retro-go use, so handing off to them needs no special color setup. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

extern uint16_t gui_bg_color;
extern uint16_t gui_fg_color;
extern uint16_t gui_accent_color;
extern uint16_t gui_border_color;
extern uint16_t gui_footer_color;
#define COLOR_BG      gui_bg_color
#define COLOR_FG      gui_fg_color
#define COLOR_ACCENT  gui_accent_color
#define COLOR_BORDER  gui_border_color
#define COLOR_FOOTER  gui_footer_color
#define COLOR_GRAY    RGB565(0x60, 0x60, 0x60) // Muted Gray
#define COLOR_GREEN   RGB565(0x2E, 0xCC, 0x71) // Success Green
#define COLOR_RED     RGB565(0xE7, 0x4C, 0x3C) // Error Red

void gui_init(void);
void gui_deinit(void);
/* Optional hook for gui_init()'s LCD settle delays: the app may point this at a
 * cooperative idle that advances background work (the partition/SD scan) while
 * the panel settles, overlapping that wait with useful work. It MUST spin at
 * least the requested ms (running longer is safe — more panel settle). NULL
 * (default) => plain HAL_Delay. */
extern void (*gui_settle_hook)(uint32_t ms);
void gui_fill(uint16_t color);

/* Resolved glyph: a `w`-wide x `h`-tall 1bpp bitmap, `stride` bytes per row,
 * rows MSB-first (bit 0x80 = leftmost column). `yoff` shifts row 0 down from the
 * draw-y (accents that ink above the cap line are negative). In-core ASCII is
 * stride 1 / yoff 0; Phase-2 external (accented/CJK) glyphs may be wider and set
 * a yoff to keep a shared baseline. */
typedef struct {
    const uint8_t *rows;
    uint8_t w;
    uint8_t h;
    uint8_t stride;
    int8_t  yoff;
} gui_glyph_info_t;

/* Decode one UTF-8 scalar, advancing *p past it; malformed bytes yield U+FFFD
 * and consume one byte (so callers always terminate). Defined in gui_text.c. */
uint32_t gui_utf8_next(const uint8_t **p);
/* Resolve a Unicode codepoint to a drawable glyph: in-core ASCII fast-path, then
 * the registered external resolver (the language module's script font), else the
 * '?' box. Always succeeds. */
bool gui_glyph(uint32_t cp, gui_glyph_info_t *gi);
/* Install/clear the non-ASCII glyph resolver. The language module registers its
 * font_ext here when loaded; NULL (the default) = ASCII-only core rendering. */
typedef bool (*gui_ext_glyph_fn)(uint32_t cp, gui_glyph_info_t *gi);
void gui_set_ext_glyph(gui_ext_glyph_fn fn);
/* Pixel width of a UTF-8 string = sum of per-glyph proportional advances. */
int gui_text_width(const char *str);

void gui_draw_char(int x, int y, uint32_t cp, uint16_t color);
void gui_draw_selector(int x, int y, uint16_t color);
void gui_draw_text(int x, int y, const char *str, uint16_t color);
void gui_draw_char_clipped(int x, int y, int clip_x, int clip_w, uint32_t cp, uint16_t color);
void gui_draw_text_marquee(int x, int y, int max_w, const char *str, uint16_t color, bool is_active, uint32_t tick_offset);
void gui_draw_rect(int x, int y, int w, int h, uint16_t color);
void gui_draw_fill_rect(int x, int y, int w, int h, uint16_t color);
void gui_draw_blend_rect(int x, int y, int w, int h);
void gui_draw_progress_bar(int x, int y, int w, int h, int percent, uint16_t border_color, uint16_t fill_color);
void gui_draw_sprite(int x, int y, int w, int h, const uint8_t *data, bool transparent, bool flip, int pitch);

void gui_backlight_on(bool on);
void gui_refresh(void);
/* Current draw buffer (swaps each frame) — handed to theme modules so they can
 * blit sprites into the frame the UI is rendering. */
uint16_t *gui_current_framebuffer(void);

#endif // GUI_H
