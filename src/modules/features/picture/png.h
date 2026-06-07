/*
 * Streaming PNG -> RGB565 decoder for the Picture Viewer.
 *
 * Pull-based input like TJpgDec: the caller supplies a read function; the decoder streams the
 * image and calls row_cb ONCE per source scanline with that row already converted to RGB565.
 * There is NO whole-image buffer (a full frame won't fit the module pool) -- only the caller's
 * scratch holds the inflate state: a tinfl decompressor + its 32 KB window + two working
 * scanlines + a palette + one RGB565 row. The host can decode straight to a row callback too,
 * so png.c is pure portable C and is unit-tested on the build host against real PNGs.
 *
 * Supported: non-interlaced PNG, bit depth 1/2/4/8/16, colour types grayscale / RGB / palette /
 * gray+alpha / RGBA. 16-bit samples are truncated to 8; alpha is dropped (shown opaque).
 * Rejected with a clear code: interlaced (Adam7), or a scanline wider than the scratch budget.
 */
#ifndef PICTURE_PNG_H
#define PICTURE_PNG_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    PNG_OK = 0,
    PNG_ERR_READ,        /* input ended early / read error */
    PNG_ERR_SIG,         /* not a PNG signature */
    PNG_ERR_FMT,         /* malformed chunk / stream */
    PNG_ERR_UNSUP,       /* interlaced or an unsupported depth/colour type */
    PNG_ERR_TOOWIDE,     /* a scanline exceeds the scratch budget */
    PNG_ERR_INFLATE,     /* DEFLATE error */
    PNG_ERR_ABORT,       /* row_cb returned 0 (caller asked to stop) */
} png_result_t;

/* Widest scanline (in source pixels) the scratch can hold; rejects wider images. */
#define PNG_MAX_WIDTH 2048

/* Compile-time upper bound on png_scratch_size() so the caller can statically reserve the
 * buffer (png.c _Static_asserts the real struct fits). ~= tinfl(11K)+window(32K)+2 scanlines
 * +palette+one RGB565 row, rounded up. */
#define PNG_SCRATCH_SIZE 65536

typedef struct png_dec {
    /* --- caller fills these before png_decode() --- */
    size_t (*read)(void *dev, uint8_t *buf, size_t len);   /* read up to len bytes; 0 = EOF */
    void   *dev;                                           /* passed back to read() */
    void   *scratch;                                       /* >= png_scratch_size() bytes */
    size_t  scratch_size;
    int   (*row_cb)(void *user, int y, const uint16_t *rgb565, int width);  /* 0 => abort */
    void   *user;
    /* --- filled by png_decode() once the header is parsed --- */
    uint16_t width, height;
} png_dec_t;

/* Bytes of scratch png_decode() needs (sized for PNG_MAX_WIDTH RGBA). */
size_t       png_scratch_size(void);

/* Decode the whole image, invoking row_cb per scanline. Returns PNG_OK or an error code. */
png_result_t png_decode(png_dec_t *d);

#endif /* PICTURE_PNG_H */
