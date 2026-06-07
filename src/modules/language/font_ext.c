/*
 * External script font reader. Each .fnt blob (scripts/build/cook_font.py blob) is
 * FNT1 (little-endian, 4-aligned arrays):
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
 * TWO load modes, chosen automatically per font by size:
 *   - WHOLE-LOAD: small fonts (Latin base, Arabic, the UI subsets) are read entirely
 *     into a per-slot RAM buffer and parsed in place — no per-glyph I/O.
 *   - PAGED: a COMPLETE CJK font is far too big for RAM, so it stays on LittleFS and
 *     is streamed glyph-by-glyph: the header is parsed, an LFS stream is held open,
 *     and each glyph is located by binary-searching codepoints[] with seek+read and
 *     its bitmap read on demand into a small LRU cache. open_slot() tries whole-load
 *     first and falls to paged when the font does not fit the buffer.
 * Fonts are read LFS-ONLY (vfs_lfs_read / vfs_open_stream skip the SD): they are
 * LFS-authoritative (installed there from the SD), and SD per-glyph latency would be
 * unacceptable for paging.
 *
 * THREE slots are tried in order: SCRIPT (the active language's script font, e.g.
 * ja/ko/zh), BASE (the shared Latin font, always resident + whole-loaded), then AUX
 * (a non-Latin .fnt loaded ON DEMAND by Unicode range so a CJK/Arabic filename renders
 * under a Latin UI). Each degrades independently: a missing slot just isn't consulted,
 * so the fallback ends at the '?' box and the menu stays up (stability is law).
 */
#include <string.h>             /* strcmp, strcpy (aux paths are short fixed literals) */
#include "ui/font_ext.h"
#include "ui/gui_font.h"        /* GUI_FONT_REF_TOP */
#include "storage/vfs.h"        /* vfs_lfs_read, vfs_stream_* */

#define FNT_MAGIC      0x31544E46u   /* 'F','N','T','1' */

enum { SLOT_SCRIPT = 0, SLOT_BASE = 1, SLOT_AUX = 2, FNT_SLOTS = 3 };

typedef struct {
    bool active;
    bool paged;                /* true: streamed from LFS glyph-by-glyph (a complete CJK font) */
    uint16_t count;
    uint8_t cell_h;
    int8_t  yoff;
    /* WHOLE-LOAD (paged == false): pointers into the slot's RAM buffer. */
    const uint32_t *cps;       /* sorted codepoints */
    const uint8_t  *widths;
    const uint32_t *offsets;
    const uint8_t  *bitmaps;
    /* PAGED (paged == true): an open LFS stream + the in-file array byte offsets. */
    vfs_stream_t stream;
    uint32_t fw_off;           /* widths[]    file offset */
    uint32_t fo_off;           /* offsets[]   file offset */
    uint32_t fbmoff;           /* bitmaps[]   file offset */
} font_t;

static font_t g_font[FNT_SLOTS];
/* These whole-load buffers are now only a FALLBACK. Factory fonts live CONTIGUOUS in the
 * memory-mapped FAT store, so open_slot XIPs them in place with NO buffer at all (even the base
 * Latin blob and complete CJK fonts). A buffer is touched only by a NON-XIP font: a small one
 * installed to LittleFS from the SD, or the first few KB of a big one during the size probe before
 * it falls to paging. A complete CJK font is always PAGED, never buffered; a non-XIP font too big
 * for its buffer simply falls to paging. Keeping the resident language module small matters (the
 * transient pool is tight -- the MP3 module alone is ~90 KB), so the script/aux buffers stay small;
 * the base buffer is sized to whole-load the ~13 KB Latin base font into RAM (the hot path). */
static uint8_t g_buf_script[8u * 1024u]  __attribute__((aligned(4)));
static uint8_t g_buf_base[16u * 1024u]   __attribute__((aligned(4)));
static uint8_t g_buf_aux[8u * 1024u]     __attribute__((aligned(4)));
static uint8_t *const g_buf[FNT_SLOTS]      = { g_buf_script, g_buf_base, g_buf_aux };
static const uint32_t g_buf_cap[FNT_SLOTS]  = { sizeof(g_buf_script), sizeof(g_buf_base), sizeof(g_buf_aux) };
static char g_aux_path[64];   /* path currently in SLOT_AUX ("" = none); caches the load */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- paged-font stream helpers + glyph cache -------------------------------- */

/* Read exactly `n` bytes at file offset `off` from a paged font's open stream. */
static bool sread_at(vfs_stream_t *s, uint32_t off, void *buf, uint32_t n) {
    if (vfs_stream_seek(s, off) != 0) return false;
    return vfs_stream_read(s, buf, (uint32_t)n) == (int)n;
}
static bool sread_u32(vfs_stream_t *s, uint32_t off, uint32_t *v) {
    uint8_t b[4];
    if (!sread_at(s, off, b, 4)) return false;
    *v = rd32(b);
    return true;
}

/* Small LRU cache of paged glyph bitmaps. A resolved paged glyph must persist past
 * slot_glyph's return (the draw blits gi->rows afterwards) and stay cheap to redraw
 * (marquee/scroll), so each entry holds the bitmap bytes. Keyed by (slot, cp) so the
 * SCRIPT and AUX paged fonts never alias; invalidated per slot when that slot reloads. */
#define PGLYPH_MAX  48u          /* >= ceil(w/8)*cell_h for any cooked glyph (w<=16, cell_h<=12 -> 24) */
#define PCACHE_N    64
typedef struct {
    uint32_t cp;                 /* 0 = empty (no real glyph is codepoint 0) */
    uint32_t lru;
    int8_t   slot;
    uint8_t  w;
    uint8_t  stride;
    uint8_t  bytes[PGLYPH_MAX];
} pcache_ent;
static pcache_ent g_pcache[PCACHE_N];
static uint32_t   g_pcache_tick;

static void pcache_invalidate(int slot) {
    for (int i = 0; i < PCACHE_N; i++)
        if (g_pcache[i].slot == (int8_t)slot) { g_pcache[i].cp = 0; g_pcache[i].lru = 0; }
}

static bool parse_into(font_t *f, const uint8_t *buf, uint32_t sz) {
    f->active = false;
    f->paged  = false;
    if (sz < 12u || rd32(buf) != FNT_MAGIC) return false;

    uint16_t gc      = (uint16_t)(buf[4] | (buf[5] << 8));
    uint8_t  cell_h  = buf[6];
    int8_t   ref_top = (int8_t)buf[7];   /* signed: a cook-time `raise` lifts glyphs up */
    uint32_t bmoff   = rd32(buf + 8);
    if (gc == 0u || cell_h == 0u) return false;

    uint32_t w_off = 12u + 4u * gc;
    uint32_t o_off = (w_off + gc + 3u) & ~3u;
    if (bmoff != o_off + 4u * gc || bmoff > sz) return false;   /* too big for the buffer -> caller pages */

    f->count   = gc;
    f->cell_h  = cell_h;
    f->yoff    = (int8_t)((int)ref_top - (int)GUI_FONT_REF_TOP);
    f->cps     = (const uint32_t *)(buf + 12);
    f->widths  = (const uint8_t  *)(buf + w_off);
    f->offsets = (const uint32_t *)(buf + o_off);
    f->bitmaps = buf + bmoff;
    /* The LAST glyph's bitmap must lie within the loaded bytes: a big font read into a
     * too-small buffer truncates here -> fail so open_slot falls to paged mode. */
    {
        uint8_t  lw   = f->widths[gc - 1];
        uint32_t lend = bmoff + f->offsets[gc - 1] + ((uint32_t)((lw + 7u) / 8u)) * cell_h;
        if (lend > sz) return false;
    }
    f->active  = true;
    return true;
}

/* Open `path` on LittleFS for PAGED access (a complete font too big to whole-load): hold
 * a stream open, parse the header, record the in-file array offsets. */
static bool open_paged(int slot, const char *path) {
    font_t *f = &g_font[slot];
    vfs_stream_close(&f->stream);
    f->active = false;
    f->paged  = false;
    if (vfs_open_stream(&f->stream, path) != 0) return false;

    uint8_t hdr[12];
    if (!sread_at(&f->stream, 0, hdr, 12) || rd32(hdr) != FNT_MAGIC) { vfs_stream_close(&f->stream); return false; }
    uint16_t gc      = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint8_t  cell_h  = hdr[6];
    int8_t   ref_top = (int8_t)hdr[7];
    uint32_t bmoff   = rd32(hdr + 8);
    uint32_t w_off   = 12u + 4u * gc;
    uint32_t o_off   = (w_off + gc + 3u) & ~3u;
    if (gc == 0u || cell_h == 0u || bmoff != o_off + 4u * gc) { vfs_stream_close(&f->stream); return false; }

    f->count  = gc;
    f->cell_h = cell_h;
    f->yoff   = (int8_t)((int)ref_top - (int)GUI_FONT_REF_TOP);
    f->fw_off = w_off;
    f->fo_off = o_off;
    f->fbmoff = bmoff;
    f->paged  = true;
    f->active = true;
    return true;
}

static bool open_slot(int slot, const char *path) {
    pcache_invalidate(slot);
    vfs_stream_close(&g_font[slot].stream);   /* drop any prior paged stream for this slot */
    g_font[slot].active = false;
    g_font[slot].paged  = false;

    /* XIP: a .fnt living CONTIGUOUS in memory-mapped flash (the FAT module store) is parsed in
     * place; the slot's pointers index straight into flash, so there is no RAM buffer and even a
     * multi-MB CJK font needs no paging. fsz is the whole file, so parse_into sees the complete
     * font and never truncates. */
    uint32_t faddr = 0, fsz = 0;
    if (vfs_map_file(path, &faddr, &fsz) == 0 &&
        parse_into(&g_font[slot], (const uint8_t *)faddr, fsz))
        return true;

    /* Not XIP-able (not in the store, or fragmented): whole-load into the slot buffer if it fits,
     * else stream it glyph-by-glyph from LittleFS. Never the SD (per-glyph latency). */
    uint32_t sz = 0;
    if (vfs_lfs_read(path, g_buf[slot], g_buf_cap[slot], &sz) == 0 &&
        parse_into(&g_font[slot], g_buf[slot], sz))
        return true;
    return open_paged(slot, path);
}

/* Whole-load lookup: binary-search the in-RAM codepoints[]. */
static bool slot_glyph(const font_t *f, uint32_t cp, gui_glyph_info_t *gi) {
    if (!f->active || f->paged) return false;
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

/* Paged lookup: binary-search codepoints[] in the file (seek+read each probe). */
static bool paged_index(font_t *f, uint32_t cp, uint32_t *out_idx) {
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v;
        if (!sread_u32(&f->stream, 12u + 4u * (uint32_t)mid, &v)) return false;
        if (cp < v) { hi = mid - 1; continue; }
        if (cp > v) { lo = mid + 1; continue; }
        *out_idx = (uint32_t)mid;
        return true;
    }
    return false;
}

/* Paged lookup with LRU cache: hit -> return cached bitmap; miss -> locate + read it. */
static bool paged_glyph(font_t *f, int slot, uint32_t cp, gui_glyph_info_t *gi) {
    if (!f->active || !f->paged) return false;

    pcache_ent *victim = &g_pcache[0];
    for (int i = 0; i < PCACHE_N; i++) {
        pcache_ent *e = &g_pcache[i];
        if (e->cp == cp && e->slot == (int8_t)slot) {        /* cache hit */
            e->lru = ++g_pcache_tick;
            gi->rows = e->bytes; gi->w = e->w; gi->h = f->cell_h;
            gi->stride = e->stride; gi->yoff = f->yoff;
            return true;
        }
        if (e->lru < victim->lru) victim = e;
    }

    uint32_t idx, off;
    uint8_t  w;
    if (!paged_index(f, cp, &idx)) return false;
    if (!sread_at(&f->stream, f->fw_off + idx, &w, 1)) return false;
    if (!sread_u32(&f->stream, f->fo_off + 4u * idx, &off)) return false;
    uint8_t  stride = (uint8_t)((w + 7u) / 8u);
    uint32_t nbytes = (uint32_t)stride * f->cell_h;
    if (nbytes == 0u || nbytes > PGLYPH_MAX) return false;   /* unexpected size -> '?' */
    if (!sread_at(&f->stream, f->fbmoff + off, victim->bytes, nbytes)) return false;

    victim->cp = cp; victim->slot = (int8_t)slot; victim->w = w; victim->stride = stride;
    victim->lru = ++g_pcache_tick;
    gi->rows = victim->bytes; gi->w = w; gi->h = f->cell_h;
    gi->stride = stride; gi->yoff = f->yoff;
    return true;
}

/* Resolve cp in `slot` regardless of load mode. */
static bool any_glyph(int slot, uint32_t cp, gui_glyph_info_t *gi) {
    font_t *f = &g_font[slot];
    if (!f->active) return false;
    return f->paged ? paged_glyph(f, slot, cp, gi) : slot_glyph(f, cp, gi);
}

/* Public API ------------------------------------------------------------- */

bool font_ext_open(const char *path) { return open_slot(SLOT_SCRIPT, path); }
bool font_ext_open_base(const char *path) { return open_slot(SLOT_BASE, path); }

/* Activate a blob already in `buf` as the SCRIPT font (host-testable, no I/O). */
bool font_ext_parse(const uint8_t *buf, uint32_t sz) {
    vfs_stream_close(&g_font[SLOT_SCRIPT].stream);
    pcache_invalidate(SLOT_SCRIPT);
    return parse_into(&g_font[SLOT_SCRIPT], buf, sz);
}

void font_ext_close(void) {
    g_font[SLOT_SCRIPT].active = false;
    vfs_stream_close(&g_font[SLOT_SCRIPT].stream);
    pcache_invalidate(SLOT_SCRIPT);
}

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
    if (any_glyph(SLOT_SCRIPT, cp, gi)) return true;
    if (any_glyph(SLOT_BASE, cp, gi)) return true;
    /* Phase D on-demand: a glyph from neither the active script nor the Latin base.
     * Try the AUX cache first; on a miss, load each candidate .fnt for this codepoint's
     * script into AUX (skipping the one already cached) until one resolves the glyph.
     * A failed load or an exhausted candidate list ends at '?' -- never faults (stability
     * is law). A run of same-script text loads its font once and then hits the cache. */
    if (any_glyph(SLOT_AUX, cp, gi)) return true;
    const char *cand[4];
    int n = aux_candidates(cp, cand);
    for (int i = 0; i < n; i++) {
        if (g_font[SLOT_AUX].active && strcmp(cand[i], g_aux_path) == 0) continue;
        if (open_slot(SLOT_AUX, cand[i])) {
            strcpy(g_aux_path, cand[i]);
            if (any_glyph(SLOT_AUX, cp, gi)) return true;
        } else {
            g_aux_path[0] = '\0';
        }
    }
    return false;
}
