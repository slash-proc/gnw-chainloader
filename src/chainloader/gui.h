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
#define COLOR_BG      gui_bg_color
#define COLOR_FG      gui_fg_color
#define COLOR_ACCENT  gui_accent_color
#define COLOR_BORDER  gui_border_color
#define COLOR_GRAY    RGB565(0x60, 0x60, 0x60) // Muted Gray
#define COLOR_GREEN   RGB565(0x2E, 0xCC, 0x71) // Success Green
#define COLOR_RED     RGB565(0xE7, 0x4C, 0x3C) // Error Red

void gui_init(void);
void gui_deinit(void);
void gui_fill(uint16_t color);
void gui_draw_char(int x, int y, uint32_t cp, uint16_t color);
void gui_draw_selector(int x, int y, uint16_t color);
void gui_draw_text(int x, int y, const char *str, uint16_t color);
void gui_draw_char_clipped(int x, int y, int clip_x, int clip_w, uint32_t cp, uint16_t color);
void gui_draw_text_marquee(int x, int y, int max_w, const char *str, uint16_t color, bool is_active, uint32_t tick_offset);
void gui_draw_rect(int x, int y, int w, int h, uint16_t color);
void gui_draw_fill_rect(int x, int y, int w, int h, uint16_t color);
void gui_draw_blend_rect(int x, int y, int w, int h, bool frost);
void gui_draw_progress_bar(int x, int y, int w, int h, int percent, uint16_t border_color, uint16_t fill_color);
void gui_draw_sprite(int x, int y, int w, int h, const uint8_t *data, bool transparent, bool flip, int pitch);

void gui_backlight_on(bool on);
void gui_refresh(void);

#endif // GUI_H
