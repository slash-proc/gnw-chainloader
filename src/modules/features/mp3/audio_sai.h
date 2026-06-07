/*
 * Minimal register-level SAI1-A + DMA1_Stream0 audio output for the MP3 feature module.
 *
 * The 40 KiB core does NOT compile the HAL SAI/DMA drivers, so the (transient) MP3 module
 * brings the peripheral up itself via CMSIS register access — tiny and self-contained. The
 * hard parts are already done by board.c: PLL2 feeds the SAI1 kernel clock (HSI 64 MHz ->
 * PLL2P = 98.304 MHz; MCKDIV=8 -> 12.288 MHz = 256 x 48 kHz, exact) and the speaker amp
 * (PE3) is enabled at boot. Config mirrors retro-go's proven setup: SAI1 Block A, master
 * TX, I2S Philips, 16-bit, MONO, 48 kHz; pins PE4/PE5/PE6 = AF6_SAI1; DMA1_Stream0,
 * DMAMUX request SAI1_A (87), circular, mem->periph, halfword.
 *
 * D-cache is ON, so a buffer in AXI SRAM (the module pool) must be cleaned by address after
 * each fill before the DMA reads it (audio_clean_range / the caller).
 */
#ifndef MP3_AUDIO_SAI_H
#define MP3_AUDIO_SAI_H

#include <stdint.h>
#include <stddef.h>

#define AUDIO_SAMPLE_RATE 48000

/* Start circular DMA playback of `buf` (mono int16, `nsamp` samples). The buffer loops
 * forever until audio_stop(); the caller refills it (double-buffered) using audio_pos(). */
void audio_start(int16_t *buf, uint32_t nsamp);

/* Stop SAI + DMA and silence the output. */
void audio_stop(void);

/* Clean the D-cache for a just-written PCM range so the DMA sees current data. */
void audio_clean_range(const void *addr, uint32_t bytes);

/* Current DMA read index within the circular buffer (0..nsamp-1): which sample the
 * hardware is playing now, for double-buffered refills. */
uint32_t audio_pos(uint32_t nsamp);

#endif /* MP3_AUDIO_SAI_H */
