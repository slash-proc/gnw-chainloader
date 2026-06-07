/*
 * Host unit test for the i18n text layer — UTF-8 decode, glyph resolution,
 * proportional width, and the string table. Pure C, no HAL: built and run on the
 * host via `make test_host`. Links the real gui_text.c / gui_font.c / strings.c.
 *
 * Exit 0 on success, 1 on any failure; prints PASS/FAIL per check.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"
#include "ui/gui_font.h"
#include "ui/strings.h"
#include "ui/font_ext.h"
#include "storage/vfs.h"      /* vfs_stream_t (paged font test) */

/* Host stub for the device vfs: read a local file into dst so font_ext_open() can
 * load the real cooked blob during the test. */
int vfs_read_file(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(dst, 1, max, f);
    fclose(f);
    *out_size = (uint32_t)n;
    return n > 0 ? 0 : -1;
}
/* font_ext reads fonts LFS-only now -> alias the same local-file stub. */
int vfs_lfs_read(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
    return vfs_read_file(path, dst, max, out_size);
}
/* Streaming stubs for the PAGED font path: back a vfs_stream_t by a host FILE* (in ctx). */
int vfs_open_stream(vfs_stream_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    s->drv = NULL; s->ctx = f;
    return 0;
}
int vfs_stream_read(vfs_stream_t *s, void *buf, uint32_t n) {
    return (s && s->ctx) ? (int)fread(buf, 1, n, (FILE *)s->ctx) : -1;
}
int vfs_stream_seek(vfs_stream_t *s, uint32_t off) {
    return (s && s->ctx) ? fseek((FILE *)s->ctx, (long)off, SEEK_SET) : -1;
}
void vfs_stream_close(vfs_stream_t *s) {
    if (s && s->ctx) fclose((FILE *)s->ctx);
    if (s) { s->drv = NULL; s->ctx = NULL; }
}

static int g_fail = 0;

static void check(int cond, const char *name) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

/* Decode the first scalar of `s`; report how many bytes were consumed. */
static uint32_t dec1(const char *s, int *adv) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp = gui_utf8_next(&p);
    if (adv) *adv = (int)(p - (const uint8_t *)s);
    return cp;
}

static void test_utf8(void) {
    int adv;
    check(dec1("A", &adv) == 0x41 && adv == 1, "utf8: ASCII 'A'");
    check(dec1("\xC3\xA4", &adv) == 0x00E4 && adv == 2, "utf8: 2-byte 'a-umlaut'");
    check(dec1("\xE2\x82\xAC", &adv) == 0x20AC && adv == 3, "utf8: 3-byte euro");
    check(dec1("\xF0\x9F\x98\x80", &adv) == 0x1F600 && adv == 4, "utf8: 4-byte emoji");
    check(dec1("\xFF", &adv) == 0xFFFD && adv == 1, "utf8: stray byte -> U+FFFD, +1");
    check(dec1("\xC3\x41", &adv) == 0xFFFD && adv == 1, "utf8: truncated -> U+FFFD, +1");
}

/* Decode the first DISPLAY token of `s` via gui_text_next; report bytes consumed. */
static uint32_t tnext(const char *s, int *adv) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp = gui_text_next(&p);
    if (adv) *adv = (int)(p - (const uint8_t *)s);
    return cp;
}

static void test_shortcode(void) {
    int adv;
    check(tnext("{BTN_A}", &adv) == 0xE050 && adv == 7, "shortcode: {BTN_A} -> U+E050, consumes 7");
    check(tnext("{MAC}x", &adv) == 0xF8FF && adv == 5, "shortcode: {MAC} -> U+F8FF, consumes 5");
    check(tnext("{asdf}", &adv) == '{' && adv == 1, "shortcode: unknown {asdf} -> literal '{'");
    check(tnext("{}", &adv) == '{' && adv == 1, "shortcode: empty {} -> literal '{'");
    check(tnext("{BTN_A", &adv) == '{' && adv == 1, "shortcode: unterminated -> literal '{'");
    check(tnext("A", &adv) == 'A' && adv == 1, "shortcode: plain ASCII passes through");
    /* A whole '{asdf}' renders as its 6 literal characters (no codepoint, never '?'). */
    check(gui_text_width("{asdf}") == gui_text_width("\x7B" "asdf}"), "shortcode: unknown token = literal chars");
    /* A known token counts as exactly ONE glyph advance, not its 7 source bytes. */
    gui_set_ext_glyph(font_ext_glyph);
    if (font_ext_open("build/i18n/fonts/latin.fnt")) {
        gui_glyph_info_t ba;
        check(font_ext_glyph(0xE050, &ba), "shortcode: BTN_A glyph baked into latin.fnt");
        check(gui_text_width("{BTN_A}") == ba.w, "shortcode: width of {BTN_A} = one glyph advance");
        font_ext_close();
    } else {
        printf("SKIP shortcode width: latin.fnt absent (run `make i18n`)\n");
    }
    gui_set_ext_glyph(NULL);
}

static void test_glyph(void) {
    gui_glyph_info_t g;
    const gui_glyph_t *A = &gui_font_ascii['A' - GUI_FONT_FIRST];
    gui_glyph('A', &g);
    check(g.w == A->w && g.h == GUI_FONT_H && g.stride == 1 && g.yoff == 0 && g.rows == A->rows,
          "glyph: ASCII 'A' resolves to its in-core bitmap");

    const gui_glyph_t *Q = &gui_font_ascii['?' - GUI_FONT_FIRST];
    gui_glyph(0x20AC, &g);   /* euro: not in-core -> '?' box */
    check(g.rows == Q->rows && g.w == Q->w, "glyph: non-ASCII falls back to '?'");
}

static void test_width(void) {
    int wA = gui_font_ascii['A' - GUI_FONT_FIRST].w;
    int wB = gui_font_ascii['B' - GUI_FONT_FIRST].w;
    int wQ = gui_font_ascii['?' - GUI_FONT_FIRST].w;
    int wH = gui_font_ascii['H' - GUI_FONT_FIRST].w;
    check(gui_text_width("") == 0, "width: empty string is 0");
    check(gui_text_width("AB") == wA + wB, "width: 'AB' = w(A)+w(B)");
    /* "H" + a-umlaut(2-byte) -> H + '?' fallback width */
    check(gui_text_width("H\xC3\xA4") == wH + wQ, "width: accent falls back to '?' width");
}

static void test_font_table(void) {
    int ok = 1;
    for (unsigned c = GUI_FONT_FIRST; c <= GUI_FONT_LAST; c++) {
        uint8_t w = gui_font_ascii[c - GUI_FONT_FIRST].w;
        if (w < 1 || w > GUI_FONT_MAX_W) { ok = 0; printf("  glyph 0x%02X width %u out of range\n", c, w); }
    }
    check(ok, "font: all 95 ASCII advances in [1, GUI_FONT_MAX_W]");
}

static void test_strings(void) {
    int all = 1;
    for (int i = 0; i < STR_COUNT; i++) {
        const char *s = tr((string_id_t)i);
        if (!s || s[0] == '\0') { all = 0; printf("  string id %d is empty/NULL\n", i); }
    }
    check(all, "strings: every id has non-empty English text");
    check(tr((string_id_t)STR_COUNT)[0] == '\0', "strings: out-of-range id -> \"\"");
    check(STRINGS_ABI_VERSION == 4u, "strings: ABI version is 4");

    /* Active-pack override: a sparse table overrides only what it sets. */
    static const char *pack[STR_COUNT];
    for (int i = 0; i < STR_COUNT; i++) pack[i] = NULL;
    pack[STR_ON] = "X-ON";
    strings_set_active(pack);
    int on_ok  = strcmp(tr(STR_ON), "X-ON") == 0;
    int off_ok = strcmp(tr(STR_OFF), "OFF") == 0;   /* unset -> English fallback */
    strings_set_active(NULL);
    int restored = strcmp(tr(STR_ON), "ON") == 0;
    check(on_ok && off_ok && restored, "strings: active pack overrides per-id, English fallback, clears");
}

/* Round-trip the cooked en_US.lang: parse the 76-byte header and assert every
 * offset is non-fallback -- the en_US pack must FULLY override the in-core all-caps
 * English (no id left to fall back), proving the un-skipped cook produced a complete
 * mixed-case pack. The .fnt/.lang are build artifacts; absence is a SKIP, not a fail. */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void test_en_pack(void) {
    static uint8_t buf[8192];
    FILE *f = fopen("build/i18n/en_US.lang", "rb");
    if (!f) {
        printf("SKIP en_pack: build/i18n/en_US.lang absent (run `make i18n`) — pack untested\n");
        return;
    }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    check(n >= 76 && rd32le(buf) == 0x32474E4Cu, "en_pack: magic 'LNG2'");
    uint16_t abi = (uint16_t)(buf[4] | (buf[5] << 8));
    uint16_t cnt = (uint16_t)(buf[6] | (buf[7] << 8));
    check(abi == STRINGS_ABI_VERSION, "en_pack: header ABI matches the core");
    check(cnt == (uint16_t)STR_COUNT, "en_pack: header str_count == STR_COUNT");
    int all_set = 1;
    for (uint16_t i = 0; i < cnt; i++) {
        if (rd32le(buf + 76 + 4u * i) == 0xFFFFFFFFu) {
            all_set = 0;
            printf("  en_US id %u falls back to English (should be authored)\n", i);
        }
    }
    check(all_set, "en_pack: every id is authored (en_US fully overrides English)");
}

/* The box-aware RTL mirror (gui_mirror_x, in gui_text.c): identity in LTR, and a
 * correct reflection within the given box in RTL (full-screen OR a centered widget). */
static void test_rtl_mirror(void) {
    gui_rtl = false;
    check(gui_mirror_x(20, 6, 0, 320) == 20, "mirror: identity when LTR");
    check(gui_mirror_x(72, 100, 50, 220) == 72, "mirror: identity in a box when LTR");
    gui_rtl = true;
    check(gui_mirror_x(20, 6, 0, 320) == 294, "mirror: full-screen reflect (320-20-6)");
    check(gui_mirror_x(50, 6, 50, 220) == 264, "mirror: box-relative reflect at box edge");
    check(gui_mirror_x(72, 100, 50, 220) == 148, "mirror: box-relative interior, not screen");
    gui_rtl = false;   /* restore global state for later tests */
}

static void test_font_ext(void) {
    gui_glyph_info_t q, g;
    gui_glyph('?', &q);

    /* gui_glyph only consults the external blob through the registerable seam
     * that the language module installs on-device (gui.c / gui_text.c). Wire it
     * here so the test exercises that real delegation; without it gui_glyph would
     * always return '?' and the resolution below could never pass. */
    gui_set_ext_glyph(font_ext_glyph);

    /* Without an active blob, 'a-umlaut' (U+00E4) falls back to '?'. */
    gui_glyph(0x00E4, &g);
    check(g.rows == q.rows, "font_ext: accent is '?' before a blob is loaded");

    if (!font_ext_open("build/i18n/fonts/latin.fnt")) {
        printf("SKIP font_ext: build/i18n/fonts/latin.fnt absent (run `make i18n`) — reader untested\n");
        return;   /* the .fnt is a build artifact; absence is not a failure */
    }
    check(1, "font_ext: loaded latin.fnt");

    /* Now U+00E4 resolves to a real external glyph, not the box. */
    gui_glyph(0x00E4, &g);
    check(g.rows != q.rows && g.w >= 1 && g.w <= 16 && g.h >= 10,
          "font_ext: U+00E4 resolves to an external glyph");
    /* yoff aligns the external cell to the in-core baseline (ref_top - REF_TOP). */
    check(g.yoff <= 0, "font_ext: external yoff lifts the taller cell to baseline");
    /* An ASCII codepoint never comes from the external blob. */
    check(!font_ext_glyph('A', &g), "font_ext: ASCII not served by the blob");
    /* An unmapped non-ASCII codepoint still falls back to '?'. */
    gui_glyph(0x1F600, &g);
    check(g.rows == q.rows, "font_ext: unmapped codepoint -> '?'");

    /* gui_text_width now counts the real accent width, not '?'. */
    int w_acc = gui_text_width("\xC3\xA4");
    check(w_acc >= 1, "font_ext: width uses the external glyph advance");

    font_ext_close();
    gui_glyph(0x00E4, &g);
    check(g.rows == q.rows, "font_ext: close() reverts accents to '?'");

    gui_set_ext_glyph(NULL);   /* unwire the seam, leaving global state clean */
}

/* The PAGED reader: a COMPLETE CJK font is far too big for the slot buffer, so font_ext
 * streams it from LFS glyph-by-glyph (binary-search codepoints[] by seek+read, bitmap on
 * demand, LRU-cached). Exercises that whole path on the real cooked ja.fnt. SKIP if the
 * blob is absent (a build artifact). */
static void test_paged_font(void) {
    gui_set_ext_glyph(font_ext_glyph);
    gui_glyph_info_t q;
    gui_glyph('?', &q);

    if (!font_ext_open("build/i18n/fonts/ja.fnt")) {
        printf("SKIP paged_font: build/i18n/fonts/ja.fnt absent (run `make i18n`)\n");
        gui_set_ext_glyph(NULL);
        return;
    }
    check(1, "paged: opened the complete ja.fnt (too big for RAM -> streamed)");

    gui_glyph_info_t g, g2;
    /* U+3042 HIRAGANA A -- resolved by paging, not whole-load. */
    gui_glyph(0x3042, &g);
    check(g.rows != q.rows && g.w >= 1 && g.h >= 10, "paged: U+3042 hiragana pages in");
    /* U+65E5 'day' -- deep in CJK Unified; exercises the seek-based binary search. */
    gui_glyph(0x65E5, &g);
    check(g.rows != q.rows && g.w >= 1, "paged: U+65E5 kanji pages in via binary search");
    /* Repeat -> LRU cache hit (same backing bytes). */
    gui_glyph(0x65E5, &g2);
    check(g2.rows == g.rows && g2.w == g.w, "paged: repeat lookup hits the LRU cache");
    /* A codepoint the font lacks -> '?'. */
    gui_glyph(0x0001, &g);
    check(g.rows == q.rows, "paged: unmapped codepoint -> '?'");

    font_ext_close();
    gui_set_ext_glyph(NULL);
}

int main(void) {
    test_utf8();
    test_shortcode();
    test_glyph();
    test_width();
    test_font_table();
    test_strings();
    test_en_pack();
    test_rtl_mirror();
    test_font_ext();
    test_paged_font();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
