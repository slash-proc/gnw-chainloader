/*
 * External script font reader. Each .fnt blob (scripts/build/cook_font.py blob)
 * is read whole into a static RAM buffer and parsed in place — no per-glyph flash
 * I/O, no on-device rasterizer. Layout (little-endian, 4-aligned arrays):
 *
 *   u32 magic 'FNT1'; u16 glyph_count; u8 cell_h; u8 ref_top; u32 bitmaps_off
 *   u32 codepoints[glyph_count]   (sorted ascending — binary searched)
 *   u8  widths[glyph_count]       (padded to a 4-byte boundary)
 *   u32 offsets[glyph_count]      (byte offset of each glyph's bitmap)
 *   u8  bitmaps[]                 (each glyph: ceil(w/8)*cell_h MSB-first bytes)
 *
 * ref_top is the glyph cell's top row within the font's design cell; the per-glyph
 * draw offset yoff = ref_top - GUI_FONT_REF_TOP makes external glyphs share the
 * in-core ASCII baseline exactly, whatever each blob's vertical band.
 *
 * THREE slots are tried in order: SCRIPT (the active language's script font,
 * e.g. ja/ko/zh), BASE (the shared Latin font, always resident), then AUX (Phase D:
 * a non-Latin .fnt loaded ON DEMAND by Unicode range, cached by path, so a CJK or
 * Arabic filename renders under a Latin UI; bounded to the cooked subset). So any available glyph renders regardless of the active language —
 * a CJK menu can still draw accented Latin, and English/Latin filenames render
 * even before a language is chosen. Each degrades independently: a missing slot
 * just isn't consulted, so the fallback ends at the '?' box and the menu stays up.
 */
#include <string.h>             /* strcmp, strcpy (aux paths are short fixed literals) */
#include "ui/font_ext.h"
#include "ui/gui_font.h"        /* GUI_FONT_REF_TOP */
#include "storage/vfs.h"        /* vfs_read_file */

#define FNT_MAGIC      0x31544E46u   /* 'F','N','T','1' */

enum { SLOT_SCRIPT = 0, SLOT_BASE = 1, SLOT_AUX = 2, FNT_SLOTS = 3 };

typedef struct {
    bool active;
    uint16_t count;
    uint8_t cell_h;
    int8_t  yoff;
    const uint32_t *cps;       /* sorted codepoints */
    const uint8_t  *widths;
    const uint32_t *offsets;
    const uint8_t  *bitmaps;
} font_t;

static font_t g_font[FNT_SLOTS];
/* SCRIPT may be a big CJK subset; BASE is the shared Latin blob (Latin-1 +
 * Extended-A + Greek + Cyrillic ~ 10 KB, so 16 KB leaves headroom). */
static uint8_t g_buf_script[24u * 1024u] __attribute__((aligned(4)));
static uint8_t g_buf_base[16u * 1024u]   __attribute__((aligned(4)));
/* AUX holds one on-demand non-Latin font at a time. 16 KB covers the current CJK/Arabic
 * subsets (~5 KB) with headroom; a font that overflows it just fails to load -> '?'. */
static uint8_t g_buf_aux[16u * 1024u]    __attribute__((aligned(4)));
static uint8_t *const g_buf[FNT_SLOTS]      = { g_buf_script, g_buf_base, g_buf_aux };
static const uint32_t g_buf_cap[FNT_SLOTS]  = { sizeof(g_buf_script), sizeof(g_buf_base), sizeof(g_buf_aux) };
static char g_aux_path[64];   /* path currently in SLOT_AUX ("" = none); caches the load */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool parse_into(font_t *f, const uint8_t *buf, uint32_t sz) {
    f->active = false;
    if (sz < 12u || rd32(buf) != FNT_MAGIC) return false;

    uint16_t gc      = (uint16_t)(buf[4] | (buf[5] << 8));
    uint8_t  cell_h  = buf[6];
    int8_t   ref_top = (int8_t)buf[7];   /* signed: a cook-time `raise` lifts glyphs up */
    uint32_t bmoff   = rd32(buf + 8);
    if (gc == 0u || cell_h == 0u) return false;

    uint32_t w_off = 12u + 4u * gc;
    uint32_t o_off = (w_off + gc + 3u) & ~3u;
    if (bmoff != o_off + 4u * gc || bmoff > sz) return false;   /* layout sanity */

    f->count   = gc;
    f->cell_h  = cell_h;
    f->yoff    = (int8_t)((int)ref_top - (int)GUI_FONT_REF_TOP);
    f->cps     = (const uint32_t *)(buf + 12);
    f->widths  = (const uint8_t  *)(buf + w_off);
    f->offsets = (const uint32_t *)(buf + o_off);
    f->bitmaps = buf + bmoff;
    f->active  = true;
    return true;
}

static bool open_slot(int slot, const char *path) {
    g_font[slot].active = false;
    uint32_t sz = 0;
    if (vfs_read_file(path, g_buf[slot], g_buf_cap[slot], &sz) != 0) return false;
    return parse_into(&g_font[slot], g_buf[slot], sz);
}

static bool slot_glyph(const font_t *f, uint32_t cp, gui_glyph_info_t *gi) {
    if (!f->active) return false;
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = f->cps[mid];
        if (cp < v) { hi = mid - 1; continue; }
        if (cp > v) { lo = mid + 1; continue; }
        uint8_t w = f->widths[mid];
        gi->rows   = f->bitmaps + f->offsets[mid];
        gi->w      = w;
        gi->h      = f->cell_h;
        gi->stride = (uint8_t)((w + 7u) / 8u);
        gi->yoff   = f->yoff;
        return true;
    }
    return false;
}

/* Public API ------------------------------------------------------------- */

bool font_ext_open(const char *path) { return open_slot(SLOT_SCRIPT, path); }
bool font_ext_open_base(const char *path) { return open_slot(SLOT_BASE, path); }

/* Activate a blob already in `buf` as the SCRIPT font (host-testable, no I/O). */
bool font_ext_parse(const uint8_t *buf, uint32_t sz) {
    return parse_into(&g_font[SLOT_SCRIPT], buf, sz);
}

void font_ext_close(void) { g_font[SLOT_SCRIPT].active = false; }

/* Phase D: which script .fnt(s) might carry `cp`, by Unicode range, into `out` (cap 4).
 * Han (CJK Unified) is shared across ja/zh/ko, so several are tried in order until one
 * has the glyph. Latin / Latin-Extended / Greek / Cyrillic are NOT here -- they live in
 * the always-resident BASE font and never reach the on-demand path. Returns the count. */
static int aux_candidates(uint32_t cp, const char *out[4]) {
    int n = 0;
    if (cp >= 0xAC00u && cp <= 0xD7A3u) {                 /* Hangul syllables -> Korean */
        out[n++] = "/i18n/fonts/ko.fnt";
    } else if ((cp >= 0x3040u && cp <= 0x30FFu) ||        /* Hiragana + Katakana -> Japanese */
               (cp >= 0x31F0u && cp <= 0x31FFu)) {
        out[n++] = "/i18n/fonts/ja.fnt";
    } else if (cp >= 0x4E00u && cp <= 0x9FFFu) {          /* CJK Unified Han (shared) */
        out[n++] = "/i18n/fonts/ja.fnt";
        out[n++] = "/i18n/fonts/zh_hans.fnt";
        out[n++] = "/i18n/fonts/zh_hant.fnt";
        out[n++] = "/i18n/fonts/ko.fnt";
    } else if ((cp >= 0x0600u && cp <= 0x06FFu) ||        /* Arabic + supplement + forms */
               (cp >= 0x0750u && cp <= 0x077Fu) ||
               (cp >= 0xFB50u && cp <= 0xFDFFu) ||
               (cp >= 0xFE70u && cp <= 0xFEFFu)) {
        out[n++] = "/i18n/fonts/arabic.fnt";
    }
    return n;
}

bool font_ext_glyph(uint32_t cp, gui_glyph_info_t *gi) {
    if (slot_glyph(&g_font[SLOT_SCRIPT], cp, gi)) return true;
    if (slot_glyph(&g_font[SLOT_BASE], cp, gi)) return true;
    /* Phase D on-demand: a glyph from neither the active script nor the Latin base.
     * Try the AUX cache first; on a miss, load each candidate .fnt for this codepoint's
     * script into AUX (skipping the one already cached) until one resolves the glyph.
     * A failed load or an exhausted candidate list ends at '?' -- never faults (stability
     * is law). A run of same-script text loads its font once and then hits the cache. */
    if (slot_glyph(&g_font[SLOT_AUX], cp, gi)) return true;
    const char *cand[4];
    int n = aux_candidates(cp, cand);
    for (int i = 0; i < n; i++) {
        if (g_font[SLOT_AUX].active && strcmp(cand[i], g_aux_path) == 0) continue;
        if (open_slot(SLOT_AUX, cand[i])) {
            strcpy(g_aux_path, cand[i]);
            if (slot_glyph(&g_font[SLOT_AUX], cp, gi)) return true;
        } else {
            g_aux_path[0] = '\0';
        }
    }
    return false;
}
