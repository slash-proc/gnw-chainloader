/*
 * Streaming BMP -> RGB565 decoder for the Picture Viewer.
 *
 * Pull-based input like png.c / TJpgDec: the caller supplies a read function; the decoder streams
 * the image top-to-bottom and calls row_cb ONCE per source scanline (already converted to RGB565,
 * and already de-flipped, so y is always the top-down logical row). There is NO whole-image
 * buffer -- only the caller's scratch holds one padded source scanline, one RGB565 row, and a
 * palette. Forward-only: the decoder never seeks, it skips gaps by reading-and-discarding, so it
 * is pure portable C and host-unit-tested against real BMPs alongside png.c.
 *
 * Supported: BITMAPINFOHEADER+ (incl. V4/V5) bitmaps, bottom-up or top-down ---
 *   - BI_RGB: 1/4/8 (palette) / 24 / 32 bpp, plus 16 bpp (XRGB1555).
 *   - BI_BITFIELDS: 16 / 32 bpp via the channel masks (this is what Retro-Go writes for its
 *     screenshots -- 16 bpp RGB565, masks F800/07E0/001F, i.e. the native framebuffer).
 * 32-bit / masked alpha is dropped (shown opaque).
 * Rejected with a clear code: RLE / embedded-codec compression, OS/2 core headers (biSize<40),
 * or a scanline wider than the scratch budget.
 */
#ifndef PICTURE_BMP_H
#define PICTURE_BMP_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    BMP_OK = 0,
    BMP_ERR_READ,        /* input ended early / read error */
    BMP_ERR_SIG,         /* not a "BM" signature */
    BMP_ERR_FMT,         /* malformed header / impossible offsets */
    BMP_ERR_UNSUP,       /* compression / bit depth / header variant we don't decode */
    BMP_ERR_TOOWIDE,     /* a scanline exceeds the scratch budget */
    BMP_ERR_ABORT,       /* row_cb returned 0 (caller asked to stop) */
} bmp_result_t;

/* Widest scanline (in source pixels) the scratch can hold; rejects wider images. */
#define BMP_MAX_WIDTH 2048

/* Compile-time upper bound on bmp_scratch_size() so the caller can statically reserve the buffer
 * (bmp.c _Static_asserts the real struct fits). One 32bpp padded scanline + one RGB565 row +
 * a 256-entry palette, rounded up. */
#define BMP_SCRATCH_SIZE 16384

typedef struct bmp_dec {
    /* --- caller fills these before bmp_decode() --- */
    size_t (*read)(void *dev, uint8_t *buf, size_t len);   /* read up to len bytes; 0 = EOF */
    void   *dev;                                           /* passed back to read() */
    void   *scratch;                                       /* >= bmp_scratch_size() bytes */
    size_t  scratch_size;
    int   (*row_cb)(void *user, int y, const uint16_t *rgb565, int width);  /* 0 => abort */
    void   *user;
    /* --- filled by bmp_decode() once the header is parsed --- */
    uint16_t width, height;
} bmp_dec_t;

/* Bytes of scratch bmp_decode() needs. */
size_t       bmp_scratch_size(void);

/* Decode the whole image, invoking row_cb per scanline. Returns BMP_OK or an error code. */
bmp_result_t bmp_decode(bmp_dec_t *d);

#endif /* PICTURE_BMP_H */
