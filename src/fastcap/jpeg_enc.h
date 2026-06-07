/*
 * Hardware JPEG encoder for fastcap (STM32H7B0 JPEG codec).
 *
 * Encodes an MCU-aligned sub-rectangle of an RGB565 framebuffer to a baseline
 * JPEG file using the on-chip JPEG peripheral (YCbCr 4:2:0).  The fastcap codec
 * uses this for BOTH whole-frame keyframes and dirty-region delta frames — every
 * byte sent over SWD is hardware-compressed.
 *
 * jpeg_enc_init() programs the quant/Huffman tables and core once;
 * jpeg_encode_region() programs the per-call image geometry, converts the
 * sub-rect, and runs the polled feed/drain loop.  Region width/height must be
 * multiples of 16 (the 4:2:0 MCU size) and the rect must lie within 320×240.
 *
 * Scratch: the RGB565→YCbCr MCU-ordered input is staged in a D2 AHB-SRAM1
 * buffer (YCC_BUF at 0x30000400); nothing in AXI is touched.  Change detection
 * is the caller's per-tile hash, so there is no prev-frame buffer to resync.
 */
#ifndef FASTCAP_JPEG_ENC_H
#define FASTCAP_JPEG_ENC_H

#include <stdint.h>

/* Program clock, core enable, quant/Huffman tables. Call once. `quality` (1..100,
   clamped) scales the standard Annex K quant tables — higher = crisper/larger. */
void jpeg_enc_init(uint32_t quality);

/* Encode the w×h sub-rectangle of fb (320-wide RGB565) at (x,y) into out
   (capacity out_cap). w and h must be multiples of 16 and the rect must fit in
   320×240. If with_header is 0 the codec emits only the entropy scan (no JPEG
   markers) — the host re-wraps it with a cached header to save ~600 B/tile; if
   1 it emits a complete standalone JPEG. Returns the byte length, or 0 on
   error/overflow/timeout. */
uint32_t jpeg_encode_region(const uint16_t *fb, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, uint8_t *out, uint32_t out_cap,
                            int with_header);

/* Whole-frame (320×240) convenience wrapper, for keyframes / single grabs. */
uint32_t jpeg_encode_full(const uint16_t *fb, uint8_t *out, uint32_t out_cap);

#endif /* FASTCAP_JPEG_ENC_H */
