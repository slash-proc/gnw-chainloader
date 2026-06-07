/*
 * Streaming BMP -> RGB565 decoder for the Picture Viewer. See bmp.h.
 *
 * Pipeline: read the 14-byte file header + the DIB (BITMAPINFOHEADER) header, pre-convert the
 * palette (indexed depths), skip forward to the pixel-data offset, then read one 4-byte-aligned
 * scanline at a time, convert each pixel to RGB565, de-flip the row index (BMP is bottom-up by
 * default), and hand the row to row_cb. Only one source scanline + one RGB565 row + the palette
 * are held (the caller's scratch); the image is never buffered whole. Forward-only (no seek):
 * gaps are skipped by reading-and-discarding. Pure portable C -> host-unit-tested.
 */
#include "bmp.h"
#include <string.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Widest padded source scanline we accept (32 bpp, 4-byte aligned). */
#define BMP_MAX_ROWBYTES (BMP_MAX_WIDTH * 4)

/* All scratch the decoder needs, overlaid on the caller's buffer. */
typedef struct {
    uint8_t  row[BMP_MAX_ROWBYTES];    /* one padded source scanline */
    uint16_t rgb[BMP_MAX_WIDTH];       /* one converted RGB565 row */
    uint16_t plte[256];                /* palette pre-converted to RGB565 */
} bmp_work_t;

/* Pixel-format description resolved from the header (drives convert_row). For the bitfield path
 * (16/32 bpp BI_BITFIELDS, and 16 bpp BI_RGB = RGB555) each channel is (value & mask) >> shift,
 * scaled from its mask width up to 8 bits. */
typedef struct {
    int bpp;
    int bitfield;                      /* 1 = mask-based (16/32bpp); 0 = packed/palette */
    uint32_t rm, gm, bm;               /* channel masks */
    int rs, gs, bs;                    /* right-shift to each mask's LSB */
    int rmax, gmax, bmax;              /* mask >> shift (max raw channel value) */
} bmp_fmt_t;

_Static_assert(sizeof(bmp_work_t) <= BMP_SCRATCH_SIZE, "BMP_SCRATCH_SIZE too small for bmp_work_t");

size_t bmp_scratch_size(void) { return sizeof(bmp_work_t); }

/* ---- byte I/O ---- */
static int rd_full(bmp_dec_t *d, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = d->read(d->dev, buf + got, n - got);
        if (r == 0) return 0;       /* EOF / error */
        got += r;
    }
    return 1;
}
static int skip_bytes(bmp_dec_t *d, uint32_t n) {
    uint8_t tmp[256];
    while (n) {
        uint32_t k = n < sizeof(tmp) ? n : (uint32_t)sizeof(tmp);
        if (!rd_full(d, tmp, k)) return 0;
        n -= k;
    }
    return 1;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- mask helpers (bitfield formats) ---- */
static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_max(uint32_t m)   { return (int)(m >> mask_shift(m)); }
static int chan(uint32_t v, uint32_t m, int sh, int mx) {
    if (!m || !mx) return 0;
    return (int)((v & m) >> sh) * 255 / mx;
}

/* ---- scanline -> RGB565 ---- */
static void convert_row(bmp_work_t *w, int W, const bmp_fmt_t *f) {
    const uint8_t *raw = w->row;
    uint16_t *o = w->rgb;
    if (f->bitfield) {
        if (f->bpp == 16) {
            for (int x = 0; x < W; x++) {
                uint32_t v = (uint32_t)raw[x * 2] | ((uint32_t)raw[x * 2 + 1] << 8);
                o[x] = RGB565(chan(v, f->rm, f->rs, f->rmax), chan(v, f->gm, f->gs, f->gmax), chan(v, f->bm, f->bs, f->bmax));
            }
        } else {   /* 32 */
            for (int x = 0; x < W; x++) {
                uint32_t v = le32(raw + x * 4);
                o[x] = RGB565(chan(v, f->rm, f->rs, f->rmax), chan(v, f->gm, f->gs, f->gmax), chan(v, f->bm, f->bs, f->bmax));
            }
        }
        return;
    }
    switch (f->bpp) {
        case 24: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 3; o[x] = RGB565(p[2], p[1], p[0]); } break;  /* BGR */
        case 32: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 4; o[x] = RGB565(p[2], p[1], p[0]); } break;  /* BGRA */
        case 8:  for (int x = 0; x < W; x++) { o[x] = w->plte[raw[x]]; } break;
        case 4:  for (int x = 0; x < W; x++) { int idx = (x & 1) ? (raw[x >> 1] & 0xF) : (raw[x >> 1] >> 4); o[x] = w->plte[idx]; } break;
        case 1:  for (int x = 0; x < W; x++) { int idx = (raw[x >> 3] >> (7 - (x & 7))) & 1; o[x] = w->plte[idx]; } break;
        default: break;
    }
}

bmp_result_t bmp_decode(bmp_dec_t *d) {
    if (!d || !d->read || !d->scratch || d->scratch_size < bmp_scratch_size()) return BMP_ERR_FMT;
    bmp_work_t *w = (bmp_work_t *)d->scratch;

    /* BITMAPFILEHEADER (14 bytes): "BM", size, reserved, pixel-data offset @10. */
    uint8_t fh[14];
    if (!rd_full(d, fh, 14)) return BMP_ERR_READ;
    if (fh[0] != 'B' || fh[1] != 'M') return BMP_ERR_SIG;
    uint32_t pix_off = le32(fh + 10);

    /* DIB header: biSize, then the BITMAPINFOHEADER core (we read the first 40 bytes). */
    uint8_t bh[40];
    if (!rd_full(d, bh, 40)) return BMP_ERR_READ;
    uint32_t dib = le32(bh + 0);
    if (dib < 40) return BMP_ERR_UNSUP;          /* OS/2 BITMAPCOREHEADER (12) not supported */
    int32_t  Ws = (int32_t)le32(bh + 4);
    int32_t  Hs = (int32_t)le32(bh + 8);
    uint16_t planes = le16(bh + 12);
    uint16_t bpp = le16(bh + 14);
    uint32_t comp = le32(bh + 16);
    uint32_t clr_used = le32(bh + 32);
    uint32_t consumed = 14 + 40;

    /* BI_BITFIELDS (comp 3): three channel masks sit at DIB offset 40 (file offset 54) -- after a
     * 40-byte BITMAPINFOHEADER they are appended (not counted in biSize); in a V4/V5 header they
     * live inside it at the same position. Either way we read them here, then skip any remaining
     * header bytes. */
    uint32_t rmask = 0, gmask = 0, bmask = 0;
    if (comp == 3) {
        uint8_t m[12];
        if (!rd_full(d, m, 12)) return BMP_ERR_READ;
        rmask = le32(m); gmask = le32(m + 4); bmask = le32(m + 8);
        consumed += 12;
        if (dib > 52) { if (!skip_bytes(d, dib - 52)) return BMP_ERR_READ; consumed += dib - 52; }
    } else if (dib > 40) {
        if (!skip_bytes(d, dib - 40)) return BMP_ERR_READ;
        consumed += dib - 40;
    }

    int top_down = 0;
    if (Hs < 0) { top_down = 1; Hs = -Hs; }
    if (Ws < 1 || Hs < 1) return BMP_ERR_FMT;
    if (planes != 1) return BMP_ERR_UNSUP;

    /* Resolve the pixel format. comp 0 = BI_RGB (packed/palette, plus 16bpp = RGB555);
     * comp 3 = BI_BITFIELDS (16/32bpp via the masks just read). Anything else (RLE, JPEG/PNG
     * embedded) is unsupported. */
    bmp_fmt_t f;
    memset(&f, 0, sizeof f);
    f.bpp = (int)bpp;
    if (comp == 3) {
        if (!(bpp == 16 || bpp == 32)) return BMP_ERR_UNSUP;
        f.bitfield = 1; f.rm = rmask; f.gm = gmask; f.bm = bmask;
    } else if (comp == 0) {
        if (bpp == 16) {                          /* BI_RGB 16bpp = XRGB1555 */
            f.bitfield = 1; f.rm = 0x7C00; f.gm = 0x03E0; f.bm = 0x001F;
        } else if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) {
            return BMP_ERR_UNSUP;
        }
    } else {
        return BMP_ERR_UNSUP;                      /* RLE / embedded codecs */
    }
    if (f.bitfield) {
        if (!f.rm || !f.gm || !f.bm) return BMP_ERR_UNSUP;
        f.rs = mask_shift(f.rm); f.gs = mask_shift(f.gm); f.bs = mask_shift(f.bm);
        f.rmax = mask_max(f.rm); f.gmax = mask_max(f.gm); f.bmax = mask_max(f.bm);
    }

    if (Ws > BMP_MAX_WIDTH) return BMP_ERR_TOOWIDE;
    int W = (int)Ws, H = (int)Hs;
    d->width = (uint16_t)W; d->height = (uint16_t)H;

    /* Palette (indexed depths): clrUsed entries, else 2^bpp; each is BGRA/BGRx. */
    if (bpp <= 8) {
        int pal = clr_used ? (int)clr_used : (1 << bpp);
        if (pal > (1 << bpp)) pal = 1 << bpp;
        for (int i = 0; i < pal; i++) {
            uint8_t e[4];
            if (!rd_full(d, e, 4)) return BMP_ERR_READ;
            w->plte[i] = RGB565(e[2], e[1], e[0]);
        }
        consumed += (uint32_t)pal * 4;
    }

    /* Forward-skip any gap to the pixel data. */
    if (pix_off < consumed) return BMP_ERR_FMT;
    if (pix_off > consumed) { if (!skip_bytes(d, pix_off - consumed)) return BMP_ERR_READ; }

    int rowbytes = (int)(((uint32_t)W * bpp + 31) / 32 * 4);   /* 4-byte-aligned scanline */
    if (rowbytes > BMP_MAX_ROWBYTES) return BMP_ERR_TOOWIDE;

    for (int r = 0; r < H; r++) {
        if (!rd_full(d, w->row, (size_t)rowbytes)) return BMP_ERR_READ;
        convert_row(w, W, &f);
        int y = top_down ? r : (H - 1 - r);    /* de-flip: bottom-up rows arrive last-first */
        if (d->row_cb && !d->row_cb(d->user, y, w->rgb, W)) return BMP_ERR_ABORT;
    }
    return BMP_OK;
}
