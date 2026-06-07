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

/* Right-to-left layout flag: set on a language change (menu_apply_language) when the
 * active language is RTL (arabic), or forced on by an RTL_TEST build. Only LAYOUT and
 * directional chrome mirror; glyphs are never pixel-flipped or reordered, so LTR output
 * is byte-for-byte unchanged when it is false. Lives here (HAL-free) so the mirror math
 * is host-unit-testable. */
#ifdef RTL_TEST
bool gui_rtl = true;     /* force-mirror with English for pre-Arabic verification */
#else
bool gui_rtl = false;
#endif

/* Box-aware horizontal mirror: reflect an element of width `elem_w` placed at `x`
 * within the box [box_x, box_x+box_w]. Identity when !gui_rtl. Full-screen surfaces
 * pass (0, SCREEN_WIDTH); centered widgets / modals pass their own (x, w) box so a
 * non-full-width menu mirrors within itself rather than across the whole screen. */
int gui_mirror_x(int x, int elem_w, int box_x, int box_w) {
    return gui_rtl ? box_x + box_w - (x - box_x) - elem_w : x;
}

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

/* Icon shortcodes: a "{NAME}" in any drawn string expands to the mapped codepoint
 * (a Fusion Pixel controller/logo glyph baked into latin.fnt). Source of record:
 * i18n/glyphs_selected.txt (cook_font bakes the same codepoints into latin.fnt).
 * An unknown or malformed "{...}" is left literal, so arbitrary text and filenames
 * are never altered. Glyphs come from the always-resident latin base font, so tokens
 * render under any active language. */
static const struct { const char *name; uint32_t cp; } g_icon_tokens[] = {
    { "BTN_UP", 0xE001 }, { "BTN_DOWN", 0xE002 }, { "BTN_LEFT", 0xE003 },
    { "BTN_RIGHT", 0xE004 }, { "BTN_UPDOWN", 0xE005 }, { "BTN_LEFTRIGHT", 0xE006 },
    { "BTN_A", 0xE050 }, { "BTN_B", 0xE051 }, { "KEYBOARD", 0xE0CB },
    { "INSTAGRAM", 0xF8EC }, { "TWITCH", 0xF8ED }, { "YOUTUBE", 0xF8EF },
    { "TWITTER", 0xF8F1 }, { "TWITTER_DUMB", 0xF8F2 }, { "FACEBOOK", 0xF8F4 },
    { "DISCORD", 0xF8F5 }, { "ANDROID", 0xF8F7 }, { "XBOX", 0xF8F8 },
    { "PLAYSTATION", 0xF8F9 }, { "STEAM", 0xF8FB }, { "WINDOWS", 0xF8FD },
    { "LINUX", 0xF8FE }, { "MAC", 0xF8FF }, { "SWITCH", 0xF8FA },
};

static uint32_t icon_lookup(const char *name, int len) {
    for (unsigned i = 0; i < sizeof(g_icon_tokens) / sizeof(g_icon_tokens[0]); i++) {
        const char *n = g_icon_tokens[i].name;
        int j = 0;
        while (j < len && n[j] && n[j] == name[j]) j++;
        if (j == len && n[j] == '\0') return g_icon_tokens[i].cp;
    }
    return 0;
}

uint32_t gui_text_next(const uint8_t **p) {
    const uint8_t *s = *p;
    if (*s == '{') {
        int n = 0;
        while (s[1 + n] && s[1 + n] != '}' && n < 31) n++;
        if (n > 0 && s[1 + n] == '}') {
            uint32_t cp = icon_lookup((const char *)(s + 1), n);
            if (cp) { *p = s + n + 2; return cp; }
        }
    }
    return gui_utf8_next(p);
}

int gui_text_width(const char *str) {
    int w = 0;
    const uint8_t *u = (const uint8_t *)str;
    while (*u) {
        gui_glyph_info_t g;
        gui_glyph(gui_text_next(&u), &g);
        w += g.w;
    }
    return w;
}
