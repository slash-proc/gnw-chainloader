/*
 * Streaming PNG -> RGB565 decoder for the Picture Viewer. See png.h.
 *
 * Pipeline: read the signature + IHDR, then feed the concatenated IDAT payload (across chunks,
 * skipping headers/CRCs) into miniz's tinfl streaming inflate. As tinfl produces decompressed
 * bytes we assemble them into scanlines (1 filter byte + row data), reverse the PNG line filter
 * against the previous raw row, convert each pixel to RGB565, and hand the row to row_cb. Only
 * the tinfl state + 32 KB window + two scanlines + a palette + one RGB565 row are held (the
 * caller's scratch); the image is never buffered whole. Pure portable C -> host-unit-tested.
 */
#include "png.h"
#include "miniz.h"          /* tinfl_decompress (NO_MALLOC/NO_STDIO set via the build CFLAGS) */
#include <string.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define PNG_MAX_ROWBYTES (PNG_MAX_WIDTH * 4)   /* widest filtered scanline we accept */

/* All scratch the decoder needs, overlaid on the caller's buffer. */
typedef struct {
    tinfl_decompressor decomp;                 /* ~11 KB inflate state */
    uint8_t  dict[TINFL_LZ_DICT_SIZE];         /* 32 KB sliding window (also tinfl output) */
    uint8_t  sl[2][PNG_MAX_ROWBYTES + 1];      /* current + previous scanline (filter byte + data) */
    uint16_t rgb[PNG_MAX_WIDTH];               /* one converted RGB565 row */
    uint16_t plte[256];                        /* palette pre-converted to RGB565 */
} png_work_t;

_Static_assert(sizeof(png_work_t) <= PNG_SCRATCH_SIZE, "PNG_SCRATCH_SIZE too small for png_work_t");

size_t png_scratch_size(void) { return sizeof(png_work_t); }

typedef struct {
    png_dec_t   *d;
    png_work_t  *w;
    int depth, ctype, channels, bpp, rowbytes;
    uint32_t idat_remain;          /* bytes left in the current IDAT chunk */
    int idat_done;                 /* hit IEND / a non-IDAT chunk / EOF */
    int cur_fill, curbuf, have_prev, rows_done;
    png_result_t status;
    uint8_t  in[2048];             /* compressed-input staging for tinfl */
    size_t   in_avail;
    const uint8_t *in_ptr;
} ctx_t;

/* ---- byte I/O ---- */
static int rd_full(png_dec_t *d, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = d->read(d->dev, buf + got, n - got);
        if (r == 0) return 0;       /* EOF / error */
        got += r;
    }
    return 1;
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int skip_bytes(png_dec_t *d, uint32_t n) {
    uint8_t tmp[256];
    while (n) {
        uint32_t k = n < sizeof(tmp) ? n : (uint32_t)sizeof(tmp);
        if (!rd_full(d, tmp, k)) return 0;
        n -= k;
    }
    return 1;
}

/* Pull up to `want` bytes of IDAT payload, transparently crossing IDAT chunk boundaries
 * (skipping each chunk's CRC + the next chunk header); stops at a non-IDAT chunk / EOF. */
static size_t idat_read(ctx_t *c, uint8_t *dst, size_t want) {
    png_dec_t *d = c->d;
    size_t total = 0;
    while (want > 0 && !c->idat_done) {
        if (c->idat_remain == 0) {
            uint8_t hdr[8];
            if (!rd_full(d, hdr, 8)) { c->idat_done = 1; break; }
            if (memcmp(hdr + 4, "IDAT", 4) == 0) c->idat_remain = be32(hdr);
            else { c->idat_done = 1; break; }     /* IEND or other -> image data ends */
            if (c->idat_remain == 0) continue;
        }
        size_t n = want < c->idat_remain ? want : c->idat_remain;
        if (!rd_full(d, dst, n)) { c->idat_done = 1; break; }
        dst += n; total += n; want -= n; c->idat_remain -= (uint32_t)n;
        if (c->idat_remain == 0) { uint8_t crc[4]; if (!rd_full(d, crc, 4)) c->idat_done = 1; }
    }
    return total;
}

/* ---- scanline -> RGB565 ---- */
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}
static void convert_row(ctx_t *c, const uint8_t *raw) {
    png_work_t *w = c->w;
    int W = c->d->width, ct = c->ctype, dp = c->depth;
    uint16_t *o = w->rgb;
    if (dp == 8) {
        switch (ct) {
            case 0: for (int x = 0; x < W; x++) { int g = raw[x];            o[x] = RGB565(g, g, g); } break;
            case 2: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 3; o[x] = RGB565(p[0], p[1], p[2]); } break;
            case 3: for (int x = 0; x < W; x++) { o[x] = w->plte[raw[x]]; } break;
            case 4: for (int x = 0; x < W; x++) { int g = raw[x * 2];        o[x] = RGB565(g, g, g); } break;
            case 6: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 4; o[x] = RGB565(p[0], p[1], p[2]); } break;
        }
    } else if (dp == 16) {                                   /* take the high byte of each sample */
        switch (ct) {
            case 0: for (int x = 0; x < W; x++) { int g = raw[x * 2];        o[x] = RGB565(g, g, g); } break;
            case 2: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 6; o[x] = RGB565(p[0], p[2], p[4]); } break;
            case 4: for (int x = 0; x < W; x++) { int g = raw[x * 4];        o[x] = RGB565(g, g, g); } break;
            case 6: for (int x = 0; x < W; x++) { const uint8_t *p = raw + x * 8; o[x] = RGB565(p[0], p[2], p[4]); } break;
        }
    } else {                                                 /* depth 1/2/4: gray ramp or palette */
        int maxv = (1 << dp) - 1;
        for (int x = 0; x < W; x++) {
            int bit = x * dp, by = bit >> 3, sh = 8 - dp - (bit & 7);
            int idx = (raw[by] >> sh) & maxv;
            if (ct == 3) o[x] = w->plte[idx];
            else { int g = idx * 255 / maxv; o[x] = RGB565(g, g, g); }
        }
    }
}
/* Reverse the line filter on the just-completed scanline (in place), convert, emit.
 * Returns 1 continue, 0 caller-abort, -1 bad filter. */
static int process_scanline(ctx_t *c) {
    png_work_t *w = c->w;
    uint8_t *cur = w->sl[c->curbuf], *prev = w->sl[c->curbuf ^ 1];
    int filt = cur[0], bpp = c->bpp, rb = c->rowbytes;
    uint8_t *x = cur + 1, *pr = prev + 1;
    if (filt > 4) return -1;
    for (int i = 0; i < rb; i++) {
        int a = i >= bpp ? x[i - bpp] : 0;
        int b = c->have_prev ? pr[i] : 0;
        int cc = (c->have_prev && i >= bpp) ? pr[i - bpp] : 0;
        int v = x[i];
        switch (filt) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += paeth(a, b, cc); break;
            default: break;                              /* 0 = None */
        }
        x[i] = (uint8_t)v;
    }
    convert_row(c, x);
    return c->d->row_cb ? c->d->row_cb(c->d->user, c->rows_done, w->rgb, c->d->width) : 1;
}

/* Consume `n` freshly decompressed bytes into scanlines. Returns 0 to stop (done / abort /
 * error, with c->status set), 1 to continue. */
static int feed(ctx_t *c, const uint8_t *buf, size_t n) {
    png_work_t *w = c->w;
    int rb = c->rowbytes;
    while (n > 0) {
        if (c->rows_done >= c->d->height) return 0;       /* all rows already produced */
        uint8_t *sl = w->sl[c->curbuf];
        int need = (1 + rb) - c->cur_fill;
        int take = (int)n < need ? (int)n : need;
        memcpy(sl + c->cur_fill, buf, (size_t)take);
        c->cur_fill += take; buf += take; n -= (size_t)take;
        if (c->cur_fill == 1 + rb) {
            int pr = process_scanline(c);
            c->cur_fill = 0; c->curbuf ^= 1; c->have_prev = 1; c->rows_done++;
            if (pr < 0) { c->status = PNG_ERR_FMT; return 0; }
            if (pr == 0) { c->status = PNG_ERR_ABORT; return 0; }
            if (c->rows_done >= c->d->height) return 0;   /* done; status stays PNG_OK */
        }
    }
    return 1;
}

static png_result_t run_inflate(ctx_t *c) {
    png_work_t *w = c->w;
    tinfl_init(&w->decomp);
    size_t dict_ofs = 0;
    c->in_avail = 0; c->in_ptr = c->in;
    for (;;) {
        if (c->in_avail == 0 && !c->idat_done) {
            c->in_avail = idat_read(c, c->in, sizeof(c->in));
            c->in_ptr = c->in;
        }
        size_t in_bytes = c->in_avail, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
        /* PNG IDAT is a zlib stream (header + adler32), not raw DEFLATE -> PARSE_ZLIB_HEADER. */
        mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (c->idat_done ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        tinfl_status st = tinfl_decompress(&w->decomp, c->in_ptr, &in_bytes,
                                           w->dict, w->dict + dict_ofs, &out_bytes, flags);
        c->in_ptr += in_bytes; c->in_avail -= in_bytes;
        if (out_bytes) {
            if (!feed(c, w->dict + dict_ofs, out_bytes)) return c->status;
            dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (st < 0) return PNG_ERR_INFLATE;
        if (st == TINFL_STATUS_DONE) return (c->rows_done >= c->d->height) ? PNG_OK : PNG_ERR_FMT;
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && c->in_avail == 0 && c->idat_done)
            return PNG_ERR_FMT;     /* stream truncated */
    }
}

png_result_t png_decode(png_dec_t *d) {
    static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!d || !d->read || !d->scratch || d->scratch_size < png_scratch_size()) return PNG_ERR_FMT;

    ctx_t c;
    memset(&c, 0, sizeof c);
    c.d = d; c.w = (png_work_t *)d->scratch; c.status = PNG_OK;

    uint8_t buf[13], crc[4];
    if (!rd_full(d, buf, 8)) return PNG_ERR_READ;
    if (memcmp(buf, SIG, 8) != 0) return PNG_ERR_SIG;

    /* IHDR must be the first chunk */
    if (!rd_full(d, buf, 8)) return PNG_ERR_READ;
    if (be32(buf) != 13 || memcmp(buf + 4, "IHDR", 4) != 0) return PNG_ERR_FMT;
    if (!rd_full(d, buf, 13)) return PNG_ERR_READ;
    if (!rd_full(d, crc, 4)) return PNG_ERR_READ;
    uint32_t W = be32(buf), H = be32(buf + 4);
    int depth = buf[8], ctype = buf[9], interlace = buf[12];
    if (W < 1 || H < 1) return PNG_ERR_FMT;
    if (interlace != 0) return PNG_ERR_UNSUP;
    if (W > PNG_MAX_WIDTH) return PNG_ERR_TOOWIDE;

    int ch, ok = 0;
    switch (ctype) {
        case 0: ch = 1; ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16); break;
        case 2: ch = 3; ok = (depth == 8 || depth == 16); break;
        case 3: ch = 1; ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8); break;
        case 4: ch = 2; ok = (depth == 8 || depth == 16); break;
        case 6: ch = 4; ok = (depth == 8 || depth == 16); break;
        default: return PNG_ERR_UNSUP;
    }
    if (!ok) return PNG_ERR_UNSUP;
    c.depth = depth; c.ctype = ctype; c.channels = ch;
    c.bpp = (ch * depth + 7) / 8; if (c.bpp < 1) c.bpp = 1;
    c.rowbytes = (int)(((uint32_t)W * (uint32_t)ch * (uint32_t)depth + 7) / 8);
    if (c.rowbytes > PNG_MAX_ROWBYTES) return PNG_ERR_TOOWIDE;
    d->width = (uint16_t)W; d->height = (uint16_t)H;

    /* chunk loop until the first IDAT (then run_inflate streams the rest) */
    for (;;) {
        if (!rd_full(d, buf, 8)) return PNG_ERR_READ;
        uint32_t len = be32(buf);
        if (memcmp(buf + 4, "PLTE", 4) == 0) {
            uint32_t ne = len / 3; if (ne > 256) ne = 256;
            for (uint32_t i = 0; i < ne; i++) {
                uint8_t p[3];
                if (!rd_full(d, p, 3)) return PNG_ERR_READ;
                c.w->plte[i] = RGB565(p[0], p[1], p[2]);
            }
            if (!skip_bytes(d, len - ne * 3) || !rd_full(d, crc, 4)) return PNG_ERR_READ;
        } else if (memcmp(buf + 4, "IDAT", 4) == 0) {
            c.idat_remain = len;
            return run_inflate(&c);
        } else if (memcmp(buf + 4, "IEND", 4) == 0) {
            return PNG_ERR_FMT;     /* no image data */
        } else {
            if (!skip_bytes(d, len) || !rd_full(d, crc, 4)) return PNG_ERR_READ;
        }
    }
}
