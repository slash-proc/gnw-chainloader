/*
 * Hardware JPEG decoder for the Picture Viewer. Brings up the STM32H7 JPEG codec in DECODE mode,
 * streams the compressed file in, and nearest-neighbour resamples each decoded YCbCr MCU straight
 * into the framebuffer (converting only the displayed pixels). Software tjpgd remains the caller's
 * fallback for anything the hardware can't handle (PNG, unsupported subsampling). Pull-based input
 * like the software decoders: the caller supplies a read function + a staging buffer.
 */
#ifndef PICTURE_JPEG_HW_H
#define PICTURE_JPEG_HW_H

#include <stdint.h>
#include <stddef.h>

typedef size_t (*hwjpeg_read_fn)(void *dev, uint8_t *buf, size_t len);   /* 0 = EOF */

/* The display mapping the decoder needs to resample each MCU onto the screen. Mirrors the
 * viewer's pic_ctx_t view fields; the caller fills it in its on_info callback (once it knows
 * the image size). NN-map: decoded (dw x dh) -> virtual (vw x vh) -> the shown svw x svh window
 * at svx,svy -> screen at ddx,ddy, clipped to [cx0,cx1) x [cy0,cy1). */
typedef struct {
    uint16_t *fb;             /* destination framebuffer (back buffer) */
    int stride;              /* framebuffer row stride in pixels */
    int dw, dh;              /* decoded image dims (full resolution; the HW does not descale) */
    int vw, vh;              /* virtual/displayed dims (aspect-fit * zoom) */
    int svx, svy, svw, svh;  /* shown window within the virtual image */
    int ddx, ddy;            /* where the shown window lands on screen */
    int cx0, cy0, cx1, cy1;  /* clip rect (viewport) */
} hwjpeg_view_t;

/* Called once after the header is parsed (image size + component count known); the caller fills
 * *view with the display mapping for this image. */
typedef void (*hwjpeg_info_fn)(void *ctx, int w, int h, int ncomp, hwjpeg_view_t *view);

/* M2: full hardware JPEG decode. Brings up the codec, streams the JPEG, and nearest-neighbour
 * resamples each decoded MCU straight into view->fb (converting only the displayed pixels).
 * Handles 4:2:0, 4:2:2, 4:4:4 and grayscale. Returns 0 on success, <0 if bring-up failed or the
 * subsampling is unsupported (caller falls back to software tjpgd). Leaves the codec stopped.
 * inbuf/inbuf_cap: a caller-provided input staging buffer; bigger = fewer (multi-block) SD reads
 * of the compressed stream, which dominates the decode for large files. */
int hwjpeg_decode(hwjpeg_read_fn rd, void *dev, hwjpeg_info_fn on_info, void *ctx,
                  uint8_t *inbuf, int inbuf_cap);

#endif /* PICTURE_JPEG_HW_H */
