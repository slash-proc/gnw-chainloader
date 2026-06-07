# MP3 Player Plan

Status: IMPLEMENTED, hardware-verified 2026-06-04 (normal-speed playback of an SD `.mp3`
launched by extension dispatch; a 480 Hz Tools-launch tone for the audio-path check). The
MP3 player is the first consumer of the feature module framework in
[docs/module-menu-registration.md](module-menu-registration.md); read that first. This
doc covers only what is MP3-specific. Complements [ACTIVE_WORK.md](../ACTIVE_WORK.md);
defers memory-map detail to [DESIGN.md](../DESIGN.md). Invariant: STABILITY IS LAW. The
speaker is silent the instant the player returns to the menu, including on decode error
or mid-track abort.

Source: `src/modules/features/mp3/` (`module_entry.c` = framework manifest + streaming
decode/resample/double-buffer; `audio_sai.c` = register-level SAI1+DMA; `minimp3.h`
vendored). Built into `/modules/features/mp3.bin` (~20 KB; the 40 KiB core is untouched).

### Audio clock: two facts that cost real debugging (verified by measuring DMA1_Stream0
NDTR over SWD = the true hardware sample rate, independent of the feed loop)

The SAI1 kernel clock must be **PLL2P = 98.304 MHz** (= 2048 x 48 kHz). board.c brings
PLL2 up (for the ADC) but does NOT route SAI1 to it, so SAI1 defaults to PLL1Q (~280 MHz)
and playback runs ~2.85x fast. The module sets `RCC->CDCCIP1R` `SAI1SEL = 001` (PLL2P).
And **`SAI_xCR1_MCKEN` must be set**: with it `FS = ker/(MCKDIV*256)` (MCKDIV=8 -> 48 kHz);
without it MCKDIV divides the bit clock (`FS = ker/(MCKDIV*32)`), another 8x fast. The
speaker amp (PE3) is already enabled by board.c. Diagnostic method when audio speed is
wrong: read NDTR (`0x40020014`) over a short window and compute the consumption rate;
compare against 48000 to separate an SAI-clock bug from a feed-rate bug.

## What the framework gives us for free

The MP3 player ships as a transient feature module `/modules/features/mp3.bin` whose
header manifest declares `menu_id = MODULE_MENU_TOOLS`, `menu_label = "MP3 Player"`,
`file_ext = "mp3"`. From the framework it gets, with zero MP3-specific core code:
- A Tools entry "MP3 Player" (listed after Partition Viewer) that runs the module.
- File-browser launch: selecting any `*.mp3` runs the module with that path.
- A `feature_host_t`: tick, VFS streaming, framebuffer + draw primitives, input
  polling, `pick_file("mp3", ...)` reusing the core browser, and `mod_ui`.
- Transient lifecycle: loaded on select, freed on return (`mod_pool_mark/reset`).

So this doc has no core menu/string/file-browser tasks. The core changes are entirely
in the framework doc.

## Module behavior

`init_module(host, out)` sets `out->run = mp3_run`. The header carries the manifest;
`init_module` only wires `run`.

`mp3_run(host, path)`:
- If `path != NULL` (launched from the file browser), play it; the queue is the
  sibling `*.mp3` files in its directory (via `host->list_dir`), with this file
  selected, so next/prev work.
- If `path == NULL` (launched from the Tools entry), call
  `host->pick_file("mp3", buf, sizeof buf)`; on a selection, play as above; on cancel,
  return.
- Every exit path routes through one cleanup that calls `audio_stop()` before
  returning, so the menu resumes silent.

## Audio sink (ported from gw_audio.c, direct register)

`src/modules/features/mp3/audio.c`. Unchanged from the prior analysis:
- Ring `int16_t dma_buf[960 * 2]` (1920 samples, 3840 bytes) in the module `.bss`
  inside the pool (AXI-SRAM, DMA1-reachable; verify the reach early).
  `AUDIO_BUFFER_LENGTH = 48000/50 = 960`.
- Enable the SAI1 peripheral clock; do NOT touch PLL2 (the core's
  `board.c SystemClock_Config` already configures PLL2 with Retro-Go's M/N/P/Q/R and
  lists `RCC_PERIPHCLK_SAI1`, and enables the GPIOE clock). Compute MCKDIV for 48 kHz.
- SAI1 Block A: master TX, I2S, 16-bit, mono, FIFO full (from `MX_SAI1_Init`). GPIOE
  PE4/PE5/PE6 AF6 (from `HAL_SAI_MspInit`). Speaker enable PE3 high on start, low on
  stop (`GPIO_Speaker_enable_Pin`, confirm polarity).
- DMA1 Stream0: `DMA_REQUEST_SAI1_A`, mem->periph, circular, halfword, FIFO full.
- No IRQ (the vector table is the core's): the feeder polls `DMA1->LISR` HTIF0/TCIF0
  and clears `DMA1->LIFCR`.
- `audio_stop()`: disable DMA, clear SAI enable, PE3 low, zero the ring. Runs on every
  exit path.

Feeder loop:

```
audio_start(): zero ring, PE3 high, start SAI + DMA circular
loop until B or queue end:
    host->input_update(); A pause, B stop, LEFT/RIGHT prev/next
    poll DMA1->LISR -> the now-free half
    fill it: if no PCM, decode one MPEG frame (minimp3); downmix stereo->mono;
             resample src_rate->48000 (nearest); copy in
    clear the flag; redraw ~30 fps via host draw + present()
on exit: audio_stop()
```

## Decoder (minimp3)

- `src/modules/features/mp3/minimp3.h` (single header, public domain), one TU defines
  `MINIMP3_IMPLEMENTATION`, `MINIMP3_NO_STDIO`, `MINIMP3_ONLY_MP3`.
- Stream via `host->open/read`: a ~16 KiB refill buffer fed by `read()`;
  `mp3dec_decode_frame()` reports `frame_bytes`, `samples`, `hz`, `channels`; slide
  and refill. Never buffer a whole track.
- RAM peak inside the pool: code ~20-30 KiB + `mp3dec_t` ~6.5 KiB + refill 16 KiB +
  PCM scratch ~4.6 KiB + ring 3.8 KiB, well under the pool budget. Resident cost is
  zero (transient).

## Now-playing screen (module-drawn)

Composed each frame via host draw callbacks: file name (track N/M for a queue),
play/pause state, a progress bar from bytes-read over `host->file_size`, and a footer
"A play/pause   B stop   < > prev/next". Cap redraw to ~30 fps so drawing never
starves the decoder.

## Build + deploy

- Build under `src/modules/features/mp3/` to `build/mp3.bin`, linked with
  `src/modules/module.ld`, flags: the menu manifest `-D`s
  (`MODULE_MENU_ID=MODULE_MENU_TOOLS`, `MODULE_MENU_LABEL='"MP3 Player"'`,
  `MODULE_FILE_EXT='"mp3"'`) plus `-DMODULE_FLAGS=MOD_FLAG_TRANSIENT`,
  `-DMODULE_VERSION=1`, ABI inherited (2).
- Deploy to `/modules/features/mp3.bin`; add to the installer's module list and the
  flash chain (per the framework doc Task 6).

## Verification (hardware, stability first)

1. `make clean && make -j16`; `build/mp3.bin` exists; core size unchanged.
2. Boot path + Retro-Go return intact with the module present but unused.
3. OCR-nav: "MP3 Player" appears in Tools after Partition Viewer; selecting it opens
   the picker; selecting an `.mp3` in the file browser also launches it.
4. Audio heard: correct pitch (validates 48 kHz + resample), pause/resume, next/prev.
5. Clean return = silence: B returns to the menu, no residual tone, a second launch
   works (slot reclaimed), and Retro-Go audio still works afterward.
6. Decode error / truncated file: graceful stop to menu, speaker silent.

## Risks (MP3-specific)

SAI clock mux actually selecting PLL2; PE3 polarity; DMA1 reach to the pool ring;
realtime decode budget (one 960-sample half per 20 ms); mono downmix. (Same detail as
the framework risks plus these hardware unknowns; confirm each during bring-up.)
Optional bring-up aid: a raw 16-bit PCM `.wav` passthrough (declare `file_ext="wav"`
too) validates the SAI/DMA chain before trusting the decoder.

## Critical files (MP3-specific)

Create: `src/modules/features/mp3/module_entry.c` (manifest via build `-D`s,
`init_module`, `mp3_run`, queue, now-playing UI), `.../mp3/audio.c` (SAI+DMA),
`.../mp3/minimp3.h`. No new core files (the framework already added `system/feature.*`
and the host vtable). Modify: `Makefile` (mp3 feature target), installer module list.
