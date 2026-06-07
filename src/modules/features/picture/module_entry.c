/*
 * Picture Viewer feature module (PIE, transient) -- second consumer of the feature-module
 * framework, modelled on the MP3 player (src/modules/features/mp3/).
 *
 * Registers a Tools entry "Picture Viewer" AND claims ".jpg,.jpeg,.png,.bmp" (the module
 * header's file_ext carries a comma-separated extension list, matched by the core's
 * ext_list_match), so the file browser dispatches all four to the viewer. THEME-NATIVE: in
 * windowed mode the
 * core's themed header (title + battery) and footer are drawn and the image fills the content
 * area (y 23..217), matching the active theme.
 *
 * Decode + scale (no whole-image buffer -- it would overflow the module pool):
 *   - JPEG via TJpgDec (vendored, ChaN R0.03), told to DESCALE during decode (1/2..1/8) to ~the
 *     on-screen size -- the speed win for big photos (the IDCT does far less work).
 *   - PNG  via a streaming inflate (png.c + miniz tinfl): scanline-at-a-time, constant memory.
 *     PNG has NO in-decode descale, so a large PNG pays a full inflate+un-filter per view change
 *     (one-time for fit/fullscreen; slow when zoom-panning) -- inherent to PNG.
 *   - BMP  via a streaming reader (bmp.c): scanline-at-a-time, constant memory, no descale.
 * Both feed ONE nearest-neighbour resampler that fits the decoded pixels EXACTLY to the view
 * (up or down), so the image fills the screen aspect-correct. The JPEG work pool and the PNG
 * inflate scratch are UNIONED (only one decoder runs per image), keeping the module pool-safe.
 *
 * View modes (unified scaler -- aspect-fit + optional zoom/pan):
 *   windowed : fit to the content area, themed header + footer (default)
 *   fullscreen (A) : fit to the whole 320x240, chrome hidden
 *   zoom (GAME) : magnify 2x/4x/8x and PAN with the d-pad
 * Controls: LEFT/RIGHT prev/next (unless zoomed) -- A fullscreen -- GAME cycle zoom -- B is
 *   "back" (leaves fullscreen/zoom to the windowed view; only QUITS from the windowed view).
 * Gallery = the sibling .jpg/.jpeg/.png/.bmp in the launched file's directory.
 */
#include "system/module.h"
#include "system/feature.h"
#include "system/input.h"   /* INPUT_B / A / GAME / LEFT / RIGHT / UP / DOWN */
#include <string.h>
#include "tjpgd.h"
#include "png.h"
#include "bmp.h"
#include "picture_strings_gen.h"   /* compiled-in translations (cook_modstrings: i18n/modules/picture/*.json) */

MODULE_HEADER;

/* ---- screen + content geometry (RGB565 320x240; chrome top/bottom) ---- */
#define SCRW   320
#define SCRH   240
#define CONT_Y 23                     /* content area top (below the header)   */
#define CONT_H 195                    /* content area height (CONT_Y..217)     */
#define ZSTEPS 4                      /* zoom steps g_z 0..3 -> Z = 1,2,4,8    */
#define PAN_STEP 48                   /* virtual px moved per d-pad press while zoomed */
#define SETTLE_MS 180                 /* idle after the last pan before the crisp re-decode */
#define COARSE_MIN_PX 300000L         /* only render a coarse preview while panning images
                                       * big enough to be slow (~670x450+); small ones decode
                                       * fast, so a coarse flash would just look worse */
#define SLOW_PIXELS 350000L           /* images bigger than ~700x500 px get a "Loading..." +
                                       * cancel overlay (a decode this big takes a noticeable beat).
                                       * Decided from the header's pixel count, NOT the filesystem
                                       * size (FAT readdir reports 0 bytes), and px count is the
                                       * real predictor of decode time anyway. */
#define CANCEL_POLL_MS 40             /* how often the decode checks for a cancel press */

/* ---- decoder scratch: borrowed from idle D2 SRAM at decode time (host scratch_get), so the 64 KB
 *      PNG inflate window / JPEG work pool never sits in this module's .bss (which would count
 *      against the AXI module pool). One decoder runs at a time, so the single region is reused. ---- */
#define JPEG_POOL_SZ 8192             /* ample for TJpgDec R0.03 FASTDECODE=1 (~3.1 KB needed) */
static JDEC g_jdec;

/* ---- view state ---- */
static int g_full;                    /* fullscreen: chrome hidden, fit the whole screen */
static int g_z;                       /* zoom step (Z = 1 << g_z); 0 = no zoom            */
static int g_panx, g_pany;            /* pan offset in virtual-image px while zoomed       */
static int g_imgw, g_imgh;            /* native dimensions of the current image            */
static int g_png_setup;               /* PNG view-ctx initialised for the current decode   */
static int g_cancel;                  /* set when the user aborts a slow decode (B)         */
static int g_cancel_en;               /* poll for cancel only during a slow decode          */
static uint32_t g_poll_tick;          /* throttle the cancel input poll                     */

/* Shared decode/blit context. The nearest-neighbour resampler maps each decoded pixel from
 * decoded space (dw x dh) through the virtual/displayed image (vw x vh) into the screen. */
typedef struct {
    const feature_host_t *h;
    void     *fp;                     /* open file handle (host VFS) */
    uint32_t  pos;                    /* absolute byte position (PNG/JPEG skips) */
    uint16_t *fb;                     /* destination framebuffer (back buffer) */
    int       dw, dh;                 /* decoded image dimensions (JPEG: native>>s; PNG: native) */
    int       vw, vh;                 /* virtual/displayed dimensions = aspect-fit * zoom */
    int       svx, svy;               /* top-left of the shown window within the virtual image */
    int       svw, svh;               /* size of the shown window (<= viewport) */
    int       ddx, ddy;               /* where the shown window lands on screen */
    int       cx0, cy0, cx1, cy1;     /* clip rect (the viewport) */
} pic_ctx_t;
static pic_ctx_t g_ctx;

/* ---- TJpgDec callbacks ---- */
static size_t in_func(JDEC *jd, uint8_t *buf, size_t len) {
    pic_ctx_t *c = (pic_ctx_t *)jd->device;
    if (buf) {
        int n = c->h->file_read(c->fp, buf, (uint32_t)len);
        if (n < 0) n = 0;
        c->pos += (uint32_t)n;
        return (size_t)n;
    }
    if (c->h->file_seek && c->h->file_seek(c->fp, c->pos + (uint32_t)len) == 0) {
        c->pos += (uint32_t)len;
        return len;
    }
    uint8_t tmp[64];
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        int n = c->h->file_read(c->fp, tmp, (uint32_t)chunk);
        if (n <= 0) break;
        done += (size_t)n;
        c->pos += (uint32_t)n;
    }
    return done;
}
/* During a slow decode, poll (throttled) for a cancel press so the user can bail out. Returns
 * nonzero once cancelled; the decode callbacks then abort. No-op on fast decodes (g_cancel_en
 * off), so quick images never poll input or fight the main loop. */
static int check_cancel(void) {
    if (!g_cancel_en || g_cancel) return g_cancel;
    uint32_t now = g_ctx.h->get_tick();
    if ((uint32_t)(now - g_poll_tick) < CANCEL_POLL_MS) return 0;
    g_poll_tick = now;
    g_ctx.h->input_update();
    if (g_ctx.h->just_pressed(INPUT_B)) g_cancel = 1;
    return g_cancel;
}

/* JPEG: nearest-neighbour-resample one decoded block onto the screen (see header comment). */
static int out_func(JDEC *jd, void *bitmap, JRECT *rect) {
    pic_ctx_t *c = (pic_ctx_t *)jd->device;
    if (check_cancel()) return 0;        /* abort -> jd_decomp returns JDR_INTR */
    const uint16_t *src = (const uint16_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    int i_lo = (int)((long)rect->left  * c->vw / c->dw) - c->svx - 1;
    int i_hi = (int)((long)(rect->right + 1) * c->vw / c->dw) - c->svx + 1;
    int j_lo = (int)((long)rect->top   * c->vh / c->dh) - c->svy - 1;
    int j_hi = (int)((long)(rect->bottom + 1) * c->vh / c->dh) - c->svy + 1;
    if (i_lo < 0) i_lo = 0;
    if (i_hi > c->svw) i_hi = c->svw;
    if (j_lo < 0) j_lo = 0;
    if (j_hi > c->svh) j_hi = c->svh;
    for (int j = j_lo; j < j_hi; j++) {
        int dy = (int)((long)(c->svy + j) * c->dh / c->vh);
        if (dy < rect->top || dy > rect->bottom) continue;
        int sy = c->ddy + j;
        if (sy < c->cy0 || sy >= c->cy1) continue;
        const uint16_t *srow = src + (dy - rect->top) * rw;
        uint16_t *frow = c->fb + (uint32_t)sy * SCRW;
        for (int i = i_lo; i < i_hi; i++) {
            int dx = (int)((long)(c->svx + i) * c->dw / c->vw);
            if (dx < rect->left || dx > rect->right) continue;
            int sx = c->ddx + i;
            if (sx < c->cx0 || sx >= c->cx1) continue;
            frow[sx] = srow[dx - rect->left];
        }
    }
    return 1;
}

/* ---- view geometry (shared by both decoders) ---- */
/* Aspect-fit `iw x ih` into `tw x th` (fills the box as much as possible). */
static void compute_base(int iw, int ih, int tw, int th, int *bw, int *bh) {
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    if ((long)iw * th >= (long)ih * tw) { *bw = tw; *bh = (int)((long)ih * tw / iw); }
    else                                { *bh = th; *bw = (int)((long)iw * th / ih); }
    if (*bw < 1) *bw = 1;
    if (*bh < 1) *bh = 1;
}
/* Viewport + virtual (displayed) size for the current mode, from the native image size. */
static void compute_view(int iw, int ih, int *vw, int *vh, int *tx, int *ty, int *tw, int *th) {
    int chrome = !(g_full || g_z > 0);
    *tx = 0; *ty = chrome ? CONT_Y : 0;
    *tw = SCRW; *th = chrome ? CONT_H : SCRH;
    int bw, bh;
    compute_base(iw, ih, *tw, *th, &bw, &bh);
    int Z = 1 << g_z;
    *vw = bw * Z; *vh = bh * Z;
}
/* Fill the shown-window / dest / clip given decoded + virtual dims; clamps the pan. */
static void fill_ctx(pic_ctx_t *c, uint16_t *fb, int dw, int dh, int vw, int vh,
                     int tx, int ty, int tw, int th) {
    c->fb = fb; c->dw = dw; c->dh = dh; c->vw = vw; c->vh = vh;
    int svw = vw < tw ? vw : tw, svh = vh < th ? vh : th;
    int maxpx = vw > tw ? vw - tw : 0, maxpy = vh > th ? vh - th : 0;
    if (g_panx < 0) g_panx = 0; else if (g_panx > maxpx) g_panx = maxpx;
    if (g_pany < 0) g_pany = 0; else if (g_pany > maxpy) g_pany = maxpy;
    c->svx = g_panx; c->svy = g_pany; c->svw = svw; c->svh = svh;
    c->ddx = tx + (tw - svw) / 2; c->ddy = ty + (th - svh) / 2;
    c->cx0 = tx; c->cy0 = ty; c->cx1 = tx + tw; c->cy1 = ty + th;
}

#if PICTURE_HW_JPEG
#include "jpeg_hw.h"
static size_t hw_read(void *dev, uint8_t *buf, size_t len);                 /* defined below */
static void   hw_on_info(void *ctx, int w, int h, int ncomp, hwjpeg_view_t *view);
#endif

/* JPEG: open + decode straight to the framebuffer. With PICTURE_HW_JPEG the STM32 JPEG codec
 * decodes (4:2:0/4:2:2/4:4:4/grayscale) much faster; anything it can't handle falls back to the
 * software tjpgd path below. `coarse` is retained for the software path (currently always 0).
 * Returns JDR_OK (0) on success. */
static int decode_jpeg(const feature_host_t *h, const char *path, uint16_t *fb, int coarse) {
    uint8_t *scr = (uint8_t *)h->scratch_get(PNG_SCRATCH_SIZE);
    if (!scr) return JDR_INP;
#if PICTURE_HW_JPEG
    g_ctx.h = h; g_ctx.fb = fb; g_ctx.pos = 0;
    g_ctx.fp = h->file_open(path);
    if (g_ctx.fp) {
        int rc = hwjpeg_decode(hw_read, &g_ctx, hw_on_info, &g_ctx, scr, (int)PNG_SCRATCH_SIZE);
        h->file_close(g_ctx.fp);
        if (rc == 0) return JDR_OK;            /* hardware decoded straight to the framebuffer */
    }                                          /* unsupported/failed -> software tjpgd below */
#endif
    g_ctx.h = h; g_ctx.fb = fb; g_ctx.pos = 0;
    g_ctx.fp = h->file_open(path);
    if (!g_ctx.fp) return JDR_INP;
    JRESULT r = jd_prepare(&g_jdec, in_func, scr, JPEG_POOL_SZ, &g_ctx);
    if (r == JDR_OK) {
        g_imgw = g_jdec.width; g_imgh = g_jdec.height;
        int vw, vh, tx, ty, tw, th;
        compute_view(g_imgw, g_imgh, &vw, &vh, &tx, &ty, &tw, &th);
        int s = 0;
        while (s < 3 && (g_imgw >> (s + 1)) >= vw && (g_imgh >> (s + 1)) >= vh) s++;
        if (coarse && (long)g_imgw * g_imgh > COARSE_MIN_PX) { s += 2; if (s > 3) s = 3; }
        int dw = g_imgw >> s; if (dw < 1) dw = 1;
        int dh = g_imgh >> s; if (dh < 1) dh = 1;
        fill_ctx(&g_ctx, fb, dw, dh, vw, vh, tx, ty, tw, th);
        r = jd_decomp(&g_jdec, out_func, (uint8_t)s);
    }
    h->file_close(g_ctx.fp);
    return (int)r;
}

/* PNG: nearest-neighbour-resample one source scanline (sy) onto the screen rows it maps to. */
static void png_blit_row(pic_ctx_t *c, int sy, const uint16_t *row) {
    int j_lo = (int)((long)sy * c->vh / c->dh) - c->svy - 1;
    int j_hi = (int)((long)(sy + 1) * c->vh / c->dh) - c->svy + 1;
    if (j_lo < 0) j_lo = 0;
    if (j_hi > c->svh) j_hi = c->svh;
    for (int j = j_lo; j < j_hi; j++) {
        int dy = (int)((long)(c->svy + j) * c->dh / c->vh);
        if (dy != sy) continue;
        int scrY = c->ddy + j;
        if (scrY < c->cy0 || scrY >= c->cy1) continue;
        uint16_t *frow = c->fb + (uint32_t)scrY * SCRW;
        for (int i = 0; i < c->svw; i++) {
            int dx = (int)((long)(c->svx + i) * c->dw / c->vw);
            int scrX = c->ddx + i;
            if (scrX < c->cx0 || scrX >= c->cx1) continue;
            frow[scrX] = row[dx];
        }
    }
}
static size_t png_read(void *dev, uint8_t *buf, size_t len) {
    pic_ctx_t *c = (pic_ctx_t *)dev;
    int n = c->h->file_read(c->fp, buf, (uint32_t)len);
    if (n > 0) { c->pos += (uint32_t)n; return (size_t)n; }
    return 0;
}
static int png_row(void *user, int y, const uint16_t *row, int width) {
    png_dec_t *d = (png_dec_t *)user;
    (void)width;
    if (check_cancel()) return 0;             /* abort -> png_decode returns PNG_ERR_ABORT */
    if (!g_png_setup) {                       /* dims known after IHDR, before any row */
        g_imgw = d->width; g_imgh = d->height;
        int vw, vh, tx, ty, tw, th;
        compute_view(g_imgw, g_imgh, &vw, &vh, &tx, &ty, &tw, &th);
        fill_ctx(&g_ctx, g_ctx.fb, g_imgw, g_imgh, vw, vh, tx, ty, tw, th);
        g_png_setup = 1;
    }
    png_blit_row(&g_ctx, y, row);
    return 1;
}
static int decode_png(const feature_host_t *h, const char *path, uint16_t *fb) {
    uint8_t *scr = (uint8_t *)h->scratch_get(PNG_SCRATCH_SIZE);
    if (!scr) return PNG_ERR_READ;
    g_ctx.h = h; g_ctx.fb = fb; g_ctx.pos = 0;
    g_ctx.fp = h->file_open(path);
    if (!g_ctx.fp) return PNG_ERR_READ;
    g_png_setup = 0;
    png_dec_t d;
    memset(&d, 0, sizeof d);
    d.read = png_read; d.dev = &g_ctx;
    d.scratch = scr; d.scratch_size = PNG_SCRATCH_SIZE;
    d.row_cb = png_row; d.user = &d;
    png_result_t r = png_decode(&d);
    h->file_close(g_ctx.fp);
    return (int)r;
}

/* BMP: same streaming row-callback shape as PNG. bmp.c already de-flips bottom-up rows, so the
 * shared resampler sees a top-down image; dims are known before any row (set in bmp_decode). */
static int bmp_row(void *user, int y, const uint16_t *row, int width) {
    bmp_dec_t *d = (bmp_dec_t *)user;
    (void)width;
    if (check_cancel()) return 0;             /* abort -> bmp_decode returns BMP_ERR_ABORT */
    if (!g_png_setup) {                        /* dims known after the header, before any row */
        g_imgw = d->width; g_imgh = d->height;
        int vw, vh, tx, ty, tw, th;
        compute_view(g_imgw, g_imgh, &vw, &vh, &tx, &ty, &tw, &th);
        fill_ctx(&g_ctx, g_ctx.fb, g_imgw, g_imgh, vw, vh, tx, ty, tw, th);
        g_png_setup = 1;
    }
    png_blit_row(&g_ctx, y, row);
    return 1;
}
static int decode_bmp(const feature_host_t *h, const char *path, uint16_t *fb) {
    uint8_t *scr = (uint8_t *)h->scratch_get(PNG_SCRATCH_SIZE);
    if (!scr) return BMP_ERR_READ;
    g_ctx.h = h; g_ctx.fb = fb; g_ctx.pos = 0;
    g_ctx.fp = h->file_open(path);
    if (!g_ctx.fp) return BMP_ERR_READ;
    g_png_setup = 0;
    bmp_dec_t d;
    memset(&d, 0, sizeof d);
    d.read = png_read; d.dev = &g_ctx;        /* shares the PNG byte-reader (same signature) */
    d.scratch = scr; d.scratch_size = PNG_SCRATCH_SIZE;
    d.row_cb = bmp_row; d.user = &d;
    bmp_result_t r = bmp_decode(&d);
    h->file_close(g_ctx.fp);
    return (int)r;
}

/* ---- gallery: sibling images in the launched file's directory ---- */
#define Q_MAX  64
#define Q_NAME 192
static char g_dir[256];
static char g_name[Q_MAX][Q_NAME];    /* full paths */
static int  g_n;

/* lowercased extension (no dot) of `name`, into e[cap]; skips AppleDouble "._name". */
static int ext_of(const char *name, char *e, int cap) {
    if (name[0] == '.' && name[1] == '_') { e[0] = 0; return 0; }
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) { e[0] = 0; return 0; }
    int i = 0;
    for (const char *p = dot + 1; *p && i < cap - 1; p++) e[i++] = (char)(*p | 32);
    e[i] = 0;
    return 1;
}
static int is_png(const char *name)  { char e[6]; return ext_of(name, e, sizeof e) && strcmp(e, "png") == 0; }
static int is_bmp(const char *name)  { char e[6]; return ext_of(name, e, sizeof e) && strcmp(e, "bmp") == 0; }
static int is_image(const char *name) {
    char e[6];
    if (!ext_of(name, e, sizeof e)) return 0;
    return strcmp(e, "jpg") == 0 || strcmp(e, "jpeg") == 0 || strcmp(e, "png") == 0 || strcmp(e, "bmp") == 0;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t i = 0;
    for (const char *p = dir; *p && i < cap - 1; p++) out[i++] = *p;
    if (i == 0 || out[i - 1] != '/') { if (i < cap - 1) out[i++] = '/'; }
    for (const char *p = name; *p && i < cap - 1; p++) out[i++] = *p;
    out[i] = 0;
}
static void split_path(const char *path, char *dir, size_t dcap, const char **base) {
    const char *slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    size_t dl = (size_t)(slash - path);
    if (dl >= dcap) dl = dcap - 1;
    memcpy(dir, path, dl); dir[dl] = 0;
    *base = (*slash == '/') ? slash + 1 : slash;
}
static int ci_less(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y;
        if (!x) return 0;
    }
}
static void g_add(const char *name, int is_dir, uint32_t size, void *user) {
    (void)size; (void)user;
    if (is_dir || g_n >= Q_MAX || !is_image(name)) return;
    char full[Q_NAME];
    join_path(full, sizeof(full), g_dir[0] ? g_dir : "/", name);
    size_t n = strlen(full);
    if (n >= Q_NAME) return;
    memcpy(g_name[g_n], full, n + 1);
    g_n++;
}
static void g_sort(void) {
    for (int i = 1; i < g_n; i++) {
        char tn[Q_NAME]; memcpy(tn, g_name[i], Q_NAME);
        int j = i - 1;
        while (j >= 0 && ci_less(tn, g_name[j])) { memcpy(g_name[j + 1], g_name[j], Q_NAME); j--; }
        memcpy(g_name[j + 1], tn, Q_NAME);
    }
}

static const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    return base;
}
static void compose_title(char *out, size_t cap, const char *path, int idx, int n) {
    size_t k = 0;
    for (const char *p = basename_of(path); *p && k < cap - 12; p++) out[k++] = *p;
    if (n > 1 && k < cap - 10) {
        out[k++] = ' '; out[k++] = ' '; out[k++] = '(';
        int a = idx + 1;
        if (a >= 10) out[k++] = (char)('0' + (a / 10) % 10);
        out[k++] = (char)('0' + a % 10);
        out[k++] = '/';
        if (n >= 10) out[k++] = (char)('0' + (n / 10) % 10);
        out[k++] = (char)('0' + n % 10);
        out[k++] = ')';
    }
    out[k] = 0;
}

/* Decode the current image (JPEG / PNG / BMP by extension) into fb. `coarse` requests a fast soft
 * preview (JPEG only; PNG/BMP can't descale, so they ignore it). Returns 0 on success. */
static int decode_current(const feature_host_t *h, const char *path, uint16_t *fb, int coarse) {
    if (is_bmp(path)) return decode_bmp(h, path, fb);
    return is_png(path) ? decode_png(h, path, fb) : decode_jpeg(h, path, fb, coarse);
}

#if PICTURE_HW_JPEG
/* M2 hardware-decode glue: stream the file into the codec, and resample each decoded MCU onto
 * the screen through the same view geometry the software path uses (forward-declared above). */
static size_t hw_read(void *dev, uint8_t *buf, size_t len) {
    pic_ctx_t *c = (pic_ctx_t *)dev;
    int n = c->h->file_read(c->fp, buf, (uint32_t)len);
    if (n > 0) { c->pos += (uint32_t)n; return (size_t)n; }
    return 0;
}
static void hw_on_info(void *ctx, int w, int h, int ncomp, hwjpeg_view_t *view) {
    pic_ctx_t *c = (pic_ctx_t *)ctx;
    (void)ncomp;
    g_imgw = w; g_imgh = h;
    int vw, vh, tx, ty, tw, th;
    compute_view(w, h, &vw, &vh, &tx, &ty, &tw, &th);
    fill_ctx(c, c->fb, w, h, vw, vh, tx, ty, tw, th);
    view->fb = c->fb; view->stride = SCRW;
    view->dw = c->dw; view->dh = c->dh; view->vw = c->vw; view->vh = c->vh;
    view->svx = c->svx; view->svy = c->svy; view->svw = c->svw; view->svh = c->svh;
    view->ddx = c->ddx; view->ddy = c->ddy;
    view->cx0 = c->cx0; view->cy0 = c->cy0; view->cx1 = c->cx1; view->cy1 = c->cy1;
}
#endif

/* Cheap header peek for the current image's pixel dimensions, to decide whether to show the
 * "Loading..." overlay BEFORE the (possibly slow) full decode. PNG: read W/H from IHDR. BMP:
 * read W/H from the DIB header (little-endian, height may be negative for top-down). JPEG:
 * jd_prepare reads only to the SOF marker (fast). Leaves nothing open. */
static void peek_dims(const feature_host_t *h, const char *path, int *pw, int *ph) {
    *pw = 0; *ph = 0;
    void *fp = h->file_open(path);
    if (!fp) return;
    if (is_png(path)) {
        uint8_t b[24];
        if (h->file_read(fp, b, 24) >= 24 && b[0] == 0x89 && b[1] == 'P') {
            *pw = (int)(((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19]);
            *ph = (int)(((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23]);
        }
    } else if (is_bmp(path)) {
        uint8_t b[26];   /* width @18, height @22 (LE int32; height<0 = top-down) */
        if (h->file_read(fp, b, 26) >= 26 && b[0] == 'B' && b[1] == 'M') {
            int32_t w = (int32_t)((uint32_t)b[18] | ((uint32_t)b[19] << 8) | ((uint32_t)b[20] << 16) | ((uint32_t)b[21] << 24));
            int32_t hh = (int32_t)((uint32_t)b[22] | ((uint32_t)b[23] << 8) | ((uint32_t)b[24] << 16) | ((uint32_t)b[25] << 24));
            if (hh < 0) hh = -hh;
            *pw = (int)w; *ph = (int)hh;
        }
    } else {
        uint8_t *scr = (uint8_t *)h->scratch_get(JPEG_POOL_SZ);
        g_ctx.h = h; g_ctx.fp = fp; g_ctx.pos = 0;
        if (scr && jd_prepare(&g_jdec, in_func, scr, JPEG_POOL_SZ, &g_ctx) == JDR_OK) {
            *pw = g_jdec.width; *ph = g_jdec.height;
        }
    }
    h->file_close(fp);
}

/* A small "Loading... / cancel" DIALOG BOX (modal style: themed border + centred text), drawn
 * synchronously here -- the gui_api modals (notice/confirm) are async (queued for the core UI
 * loop), so they can't render during this blocking decode; we compose the same look from the
 * primitives. Presented BEFORE a slow decode so it stays up while the image decodes into the
 * (now hidden) back buffer. */
static void draw_loading(const feature_host_t *h) {
    const gui_api_t *g = h->gui;
    const int bw = 208, bh = 54, bx = (SCRW - bw) / 2, by = (SCRH - bh) / 2;
    g->fill_rect(0, 0, SCRW, SCRH, g->color_bg());            /* clean backdrop */
    g->fill_rect(bx, by, bw, bh, g->color_bg());              /* box panel       */
    g->draw_rect(bx, by, bw, bh, g->color_accent());          /* accent frame    */
    g->draw_rect(bx + 1, by + 1, bw - 2, bh - 2, g->color_border());
    g->draw_text(bx + (bw - g->text_width(ms(MS_LOADING))) / 2, by + 13, ms(MS_LOADING), g->color_fg());
    g->draw_text(bx + (bw - g->text_width(ms(MS_CANCEL)))  / 2, by + 32, ms(MS_CANCEL),  g->color_footer());
    h->present();
}

/* Render the current image + (windowed) chrome and present (once -- the presented buffer holds
 * while idle, so there's no per-frame re-decode). `coarse` = the progressive-pan fast preview.
 * Big files first paint a cancelable "Loading..." overlay; if the user cancels, g_cancel is set
 * and we return without presenting the partial image (the caller treats it as quit). */
static void render(const feature_host_t *h, int cur, int coarse) {
    const gui_api_t *g = h->gui;
    int chrome = !(g_full || g_z > 0);
    int pw, ph;
    peek_dims(h, g_name[cur], &pw, &ph);
    int slow = (long)pw * ph > SLOW_PIXELS;

    g_cancel = 0;
    if (slow) { draw_loading(h); g_poll_tick = h->get_tick(); }   /* shown during the decode */

    uint16_t *fb = g->framebuffer();        /* re-fetched: after draw_loading's present, the hidden buffer */
    if (chrome) g->fill_rect(0, CONT_Y, SCRW, CONT_H, g->color_bg());
    else        g->fill_rect(0, 0, SCRW, SCRH, g->color_bg());

    g_cancel_en = slow;
    int err = decode_current(h, g_name[cur], fb, coarse);
    g_cancel_en = 0;
    if (g_cancel) return;                   /* aborted: leave the partial hidden; caller quits */

    if (err) {
        const char *msg = ms(MS_ERR_DECODE);
        int my = (chrome ? CONT_Y + CONT_H / 2 : SCRH / 2) - 4;
        g->draw_text((SCRW - g->text_width(msg)) / 2, my, msg, g->color_fg());
    }
    if (chrome) {
        char title[Q_NAME];
        compose_title(title, sizeof(title), g_name[cur], cur, g_n);
        h->draw_header(title);
        h->draw_footer(ms(MS_FOOTER));
    }
    h->present();
}

static void render_empty(const feature_host_t *h) {
    const gui_api_t *g = h->gui;
    g->fill_rect(0, CONT_Y, SCRW, CONT_H, g->color_bg());
    g->draw_text((SCRW - g->text_width(ms(MS_EMPTY))) / 2, CONT_Y + 70, ms(MS_EMPTY), g->color_fg());
    g->draw_text((SCRW - g->text_width(ms(MS_HELP_PICK))) / 2, CONT_Y + 94, ms(MS_HELP_PICK), g->color_fg());
    h->draw_header(ms(MS_TITLE));
    h->draw_footer(ms(MS_FOOTER));
    h->present();
}

/* Center the pan when entering / changing zoom (zoom uses the full-screen viewport). */
static void recenter_pan(void) {
    int bw, bh;
    compute_base(g_imgw, g_imgh, SCRW, SCRH, &bw, &bh);
    int Z = 1 << g_z;
    int vw = bw * Z, vh = bh * Z;
    g_panx = vw > SCRW ? (vw - SCRW) / 2 : 0;
    g_pany = vh > SCRH ? (vh - SCRH) / 2 : 0;
}

/* Run the viewer UI over the already-built gallery (g_dir / g_name / g_n), starting at `cur`. */
static void viewer_show(const feature_host_t *h, int cur) {
    g_full = 0; g_z = 0; g_panx = 0; g_pany = 0;

    if (g_n == 0) { render_empty(h); }
    else          { render(h, cur, 0); if (g_cancel) return; }

    while (1) {
        h->input_update();
        if (g_n == 0) { if (h->just_pressed(INPUT_B)) break; continue; }

        int changed = 0;
        if (h->just_pressed(INPUT_B)) {                  /* "back": leave zoom/fullscreen, else quit */
            if (g_full || g_z > 0) { g_full = 0; g_z = 0; g_panx = g_pany = 0; changed = 1; }
            else break;
        } else if (h->just_pressed(INPUT_A)) {           /* toggle fullscreen fit */
            g_full = !g_full; changed = 1;
        } else if (h->just_pressed(INPUT_GAME)) {        /* cycle zoom 1x/2x/4x/8x */
            g_z = (g_z + 1) % ZSTEPS;
            recenter_pan();
            changed = 1;
        } else if (g_z > 0) {                            /* zoomed: d-pad pans */
            if (h->just_pressed(INPUT_LEFT))       { g_panx -= PAN_STEP; changed = 1; }
            else if (h->just_pressed(INPUT_RIGHT)) { g_panx += PAN_STEP; changed = 1; }
            else if (h->just_pressed(INPUT_UP))    { g_pany -= PAN_STEP; changed = 1; }
            else if (h->just_pressed(INPUT_DOWN))  { g_pany += PAN_STEP; changed = 1; }
        } else if (g_n > 1) {                            /* not zoomed: LEFT/RIGHT change image */
            if (h->just_pressed(INPUT_LEFT))       { cur = (cur - 1 + g_n) % g_n; changed = 1; }
            else if (h->just_pressed(INPUT_RIGHT)) { cur = (cur + 1) % g_n;        changed = 1; }
        }

        /* One full-quality decode per change. (A software "progressive" coarse-then-sharp pan
         * was tried and reverted -- the coarse pass still pays JPEG's Huffman cost, so it was a
         * second slow step, not a fast preview. Snappy pan needs the HW decode + cache.) */
        if (changed) { render(h, cur, 0); if (g_cancel) break; }
    }
}

/* Build the gallery from `initial`'s directory and show it, selecting `initial`. */
static void viewer(const feature_host_t *h, const char *initial) {
    const char *base;
    if (initial) split_path(initial, g_dir, sizeof(g_dir), &base);
    else         g_dir[0] = 0;
    g_n = 0;
    if (initial && h->list_dir) h->list_dir(g_dir[0] ? g_dir : "/", g_add, 0);
    g_sort();

    int cur = 0;
    if (initial) {
        for (int i = 0; i < g_n; i++)
            if (!ci_less(g_name[i], initial) && !ci_less(initial, g_name[i])) { cur = i; break; }
        if (g_n == 0) {
            size_t n = strlen(initial);
            if (n < Q_NAME) { memcpy(g_name[0], initial, n + 1); g_n = 1; cur = 0; }
        }
    }
    viewer_show(h, cur);
}

static void run(const feature_host_t *h, const char *path) {
    ms_set_lang(h->gui->lang_code());   /* select the compiled-in translation column for this run */
    if (path) { viewer(h, path); return; }

    /* Tools-menu launch (no file): default to the SD card's /screenshots/ when it exists and holds
     * images; otherwise (no card, no folder, or no images) fall through to the file picker. */
    if (h->set_source_sd && h->set_source_sd()) {
        memcpy(g_dir, "/screenshots", sizeof("/screenshots"));
        g_n = 0;
        if (h->list_dir) h->list_dir(g_dir, g_add, 0);
        g_sort();
        if (g_n > 0) { viewer_show(h, 0); return; }
    }

    if (h->pick_file) {
        char pbuf[256];
        uint32_t psize = 0; bool pdir = false;
        if (h->pick_file("jpg,jpeg,png,bmp", pbuf, sizeof(pbuf), &psize, &pdir) && !pdir) {
            viewer(h, pbuf);
            return;
        }
    }
    viewer(h, NULL);   /* nothing picked -> empty prompt, B to quit */
}

void init_module(const feature_host_t *host, feature_api_t *out) {
    (void)host;
    out->run = run;
}
