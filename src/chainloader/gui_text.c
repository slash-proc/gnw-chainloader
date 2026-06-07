/*
 * Text-layer helpers: UTF-8 decode, codepoint -> glyph resolution, and string
 * pixel-width. Deliberately HAL-free (no framebuffer, no board) so they compile
 * and run on the host for unit tests (see tests/test_gui_text.c); the actual
 * blitters that touch the framebuffer stay in gui.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include "gui.h"
#include "ui/gui_font.h"

/* Non-ASCII glyph resolver, installed by the language module when loaded
 * (NULL = ASCII-only core). Keeps gui_text.c free of any font_ext dependency. */
static gui_ext_glyph_fn g_ext_glyph;
void gui_set_ext_glyph(gui_ext_glyph_fn fn) { g_ext_glyph = fn; }

uint32_t gui_utf8_next(const uint8_t **p) {
    const uint8_t *s = *p;
    uint32_t c = *s++;
    if (c < 0x80) { *p = s; return c; }
    uint32_t cp; int extra;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *p = s; return 0xFFFD; }            /* stray continuation / 5-6 byte */
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *p = s; return 0xFFFD; }   /* truncated */
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    *p = s;
    return cp;
}

bool gui_glyph(uint32_t cp, gui_glyph_info_t *gi) {
    if (cp >= GUI_FONT_FIRST && cp <= GUI_FONT_LAST) {
        const gui_glyph_t *g = &gui_font_ascii[cp - GUI_FONT_FIRST];
        gi->rows = g->rows; gi->w = g->w; gi->h = GUI_FONT_H; gi->stride = 1; gi->yoff = 0;
        return true;
    }
    /* Non-ASCII: the registered external resolver (accented Latin / CJK), if any. */
    if (g_ext_glyph && g_ext_glyph(cp, gi)) return true;
    /* Unknown codepoint -> the '?' box. */
    const gui_glyph_t *g = &gui_font_ascii['?' - GUI_FONT_FIRST];
    gi->rows = g->rows; gi->w = g->w; gi->h = GUI_FONT_H; gi->stride = 1; gi->yoff = 0;
    return true;
}

int gui_text_width(const char *str) {
    int w = 0;
    const uint8_t *u = (const uint8_t *)str;
    while (*u) {
        gui_glyph_info_t g;
        gui_glyph(gui_utf8_next(&u), &g);
        w += g.w;
    }
    return w;
}
