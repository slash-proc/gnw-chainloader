# Fast Framebuffer Capture Architecture

## Overview

Fast framebuffer capture ("fastcap") streams the Game & Watch screen (320×240 RGB565)
to the host over SWD while the chainloader keeps running. A small position-independent
codec is loaded into spare on-chip RAM and called once per rendered frame; it
hardware-JPEG-compresses only the parts of the screen that changed and hands the host a
compact payload. The host decodes it back into a running frame, saves PNGs, and can
assemble an mp4 or show a live preview.

The design in one breath: **tile the screen, hash each tile to detect change, JPEG-encode
only changed tiles on the STM32's hardware codec, strip the redundant per-tile JPEG header,
stay silent on unchanged frames, and never touch AXI SRAM.** Everything lives in the unused
D2 AHB-SRAM1 bank so it can never collide with the chainloader, its framebuffers, or the
PIE module pool.

---

## Architecture

### 1. The codec (`src/fastcap/main.c`)

`fastcap_frame(const uint16_t *fb)` is built independently via `Makefile.fastcap` and
`STM32H7B0_FASTCAP.ld` into `build/fastcap/fastcap.bin` (~3.7 KB) that the host loads into
**D2 AHB-SRAM1** (`0x3000E000`). It is an ordinary callable C function the chainloader
invokes through a function pointer, so the display stays live the whole time.

Each call:

1. **Reinit (first call, or after a host `RESET`):** enables the D2 SRAM clock, reads the
   `MODE` and `QUALITY` handshake cells, and programs the JPEG core once.
2. **Async drop check:** in async mode, if the host has not yet acked the previous frame
   (`STATUS_FLAG != 0`), the frame is counted as skipped and dropped — the display runs on.
3. **Change detection:** the screen is a fixed **32×16 tile grid** (10×15 = 150 tiles). Each
   tile's pixels are hashed with **FNV-1a** and compared to the stored hash. There is *no
   previous-frame buffer* — the 600-byte hash table is the entire change-detection state, so
   the whole codec fits in 64 KB.
4. **Encode:** changed tiles (and *all* tiles on a keyframe) are hardware-JPEG-encoded and
   packed into the payload. A tile's stored hash is updated only when its tile is actually
   emitted, so a tile dropped at the payload cap is naturally re-sent on the next frames
   (a truncated keyframe self-heals over 2-3 frames).
5. **Emit / silent-skip:** if nothing changed, the device stays silent and increments a
   `skipped` counter; a `tile_count = 0` timing anchor is emitted every `ANCHOR_SKIP` (30)
   silent frames so a long static stretch never opens an unbounded gap in the host timeline.
   Otherwise the payload is cache-cleaned and `STATUS_FLAG` is raised.

A `KEYFRAME_INTERVAL` (240 emitted frames) periodically re-sends every tile to recover from
any host-side miss.

**Drift note:** the device hashes the *true* framebuffer, so change detection is exact; the
host holds JPEG-decoded tiles, so a tile that stops changing settles to its last-decoded
value (fine for UI). The periodic keyframe re-converges both sides.

### 2. The JPEG encoder (`src/fastcap/jpeg_enc.c`)

A direct-register driver for the STM32H7B0 hardware JPEG codec (`0x52003000`, AHB3), modelled
on the STM32H7 HAL (the H7-specific Huffman/quant table packing is ported verbatim; color
conversion, MCU reordering, and the polled feed/drain loop are bespoke). No HAL, no DMA.

- **Tiles are 32×16 = 2 MCU.** The codec's 4:2:0 MCU is 16×16; a single-MCU tile (`CONFR2 = 0`)
  faults the core, so 32×16 is the smallest reliable tile. Raw experiments favored the smallest
  viable tile, and 2 MCU is it.
- **Header-strip:** only the keyframe's first emitted tile is encoded with `CONFR1_HDR` set, so
  it carries the full ~600 B JPEG header (quant + Huffman tables). Every other tile is encoded
  as a headerless entropy scan; the host caches the header prefix from that one tile and
  re-wraps each later scan as `prefix + scan + EOI`. At ~150 tiles/keyframe this avoids
  re-sending an identical 600 B table per tile.
- **Runtime quality:** `jpeg_enc_init(quality)` scales the standard Annex K quant tables by the
  host-written `QUALITY` cell (1..100, clamped; default 98). `scale = 200 − 2·Q` for Q ≥ 50, so
  Q100 → all table entries clamp to 1 (no quantization, max precision/size); Q80 → ~6× smaller
  frames. Quality is the host's lever on the throughput/frame-rate trade-off (see
  [Throughput](#throughput-the-real-bottleneck)).

The YCbCr MCU staging buffer (`YCC_BUF`, `0x30000400`) only ever holds one 32×16 tile (768 B).

### 3. The chainloader hook (`src/chainloader/gui.c`)

`gui_refresh()` reads `FASTCAP_HOOK_ADDR` every frame; if the host has armed it with a non-null
function pointer, it calls `hook(framebuffer)`. The call site is right after
`SCB_CleanDCache_by_Addr()` (the back buffer is cache-coherent and complete) and before the
LTDC reload, so the codec sees a stable, finished, not-yet-visible frame. At startup `main.c`
zeroes `FASTCAP_HOOK_ADDR` (DTCM is not cleared on reset) to prevent a stale pointer call.

### 4. The host tool (`scripts/debug/fastcap.py`)

One tool, three subcommands, sharing the common SWD backend (`scripts/common/device.py`) and
input layer (`scripts/debug/remote_control.py`):

- **`capture`** — load the binary, arm the hook, stream frames to PNGs, optionally assemble an
  mp4 and/or show a live OpenCV window. Can drive the UI while recording (`--input
  auto|gamepad|keyboard`, gamepad preferred). `--mode async|sync`, `--quality 1..100`.
  With **`--live`** the OpenCV window becomes an interactive **cockpit**: watch the stream and
  drive the device in the same window (WASD/arrows + J/K, plus a gamepad if present; `F` toggles
  the FPS overlay, `Q`/`Esc` quits). Keyboard control comes from the window via `waitKeyEx` with a
  hold timer (`remote_control.WindowKeySource`), OR'd with any gamepad. Requires a REMOTE_INPUT
  firmware build (the default).
- **`grab`** — a single frame: a compressed keyframe by default, or `--raw` for a pristine
  uncompressed framebuffer pull (reads LTDC directly, no `fastcap.bin` needed).
- **`analyze`** — per-frame PNG stats (size, non-black %, delta-vs-previous %).

Because D2 AHB-SRAM is unclocked at reset, the host enables its clock (`RCC_AHB2ENR` bits 29/30)
before loading the binary. The live window shows a rolling FPS overlay (toggle with `f`) and the
terminal heartbeat reports live fps; the capture summary reports per-frame size and the keyframe
size vs the payload cap so truncation is always visible.

---

## Handshake cells (DTCM)

All handshake words live in **DTCM** (`0x2001FFxx`), which is tightly-coupled to the Cortex-M7
and bypasses the L1 D-Cache — the CPU and the SWD AHB-AP see the same physical word with no
coherency gap. (A handshake flag in cached AXI SRAM deadlocks: the device spin-reads a hot cache
line while the host writes the ack to physical RAM. `volatile` does not fix this.) Authoritative
definitions live in `src/common/memory_map.h`:

| Cell | Address | Meaning |
|------|---------|---------|
| `FASTCAP_STATUS_FLAG` | `0x2001FF00` | device writes 1 when a frame is ready; host acks with 0 |
| `FASTCAP_HOOK_ADDR`   | `0x2001FF04` | host writes the codec entry; chainloader calls it (0 disarms) |
| `FASTCAP_RESET_FLAG`  | `0x2001FF08` | host writes 1 to force a fresh keyframe / reinit |
| `FASTCAP_QUALITY`     | `0x2001FF0C` | host writes JPEG quality 1..100 (0 = device default 98) |
| `FASTCAP_MODE`        | `0x2001FF14` | host writes 0 = async/live, 1 = sync/frame-perfect |

`MODE` and `QUALITY` are read at reinit, so the host writes them *before* setting `RESET`.

---

## Payload format

One `STATUS_FLAG` raise = one frame's payload at `PAY_BUF` (`0x30002400`, D2):

```
frame header (12 bytes):
  u32 tile_count    number of tile records (0 = pure timing anchor)
  u32 total_bytes   bytes of tile records that follow
  u32 skipped       unchanged/dropped frames silently skipped before this one
then tile_count records (packed, not aligned):
  u32 idx           0..149; pixel origin = (idx%10 * 32, idx/10 * 16)
  u32 jpeg_len
  u8  jpeg[jpeg_len]   keyframe's first tile = full JPEG; others = headerless scan
```

The host replays `skipped` copies of the previous frame to keep the mp4 timeline real-time,
then applies the new tiles. A `tile_count = 0` anchor only carries `skipped` (no tiles).

---

## RAM layout

The codec is **entirely D2 AHB-SRAM1 resident** — the full bank breakdown (tile-hash / YCC
scratch / 47 KB payload / 8 KB code) is in **DESIGN.md §1, "D2 AHB-SRAM1"**. The single
invariant that matters: fastcap never touches AXI SRAM, so a bad fastcap build can only fail
a capture, never the chainloader or a loaded module. (An earlier AXI-resident layout overwrote
a loaded module and bus-faulted — that is why this is D2-only now.)

---

## Capture modes

Selected at runtime via `FASTCAP_MODE`:

- **async (default) — low-latency live monitor.** Non-blocking: a frame raised while the host
  is still reading the previous one is dropped (counted in `skipped`) and the display runs on.
  Capture rate is throughput-bound (see below); good for watching the UI in near-real-time.
- **sync — frame-perfect recorder.** The device raises `STATUS_FLAG` and spin-waits for the host
  ack each frame, so nothing is ever dropped; the device UI runs in slow-motion during capture,
  but every distinct frame lands, yielding a smooth playback at full quality.

---

## Throughput: the real bottleneck

The SWD link, not the encoder or the SWD *clock*, is the wall. Raising the SWD clock does not
help — capture is bound by per-transaction USB-FS overhead and bytes-per-frame, at an effective
**~50–70 KB/s** of payload. The practical consequence:

```
distinct_fps  ≈  link_throughput / bytes_per_frame
```

So the **distinct** (visually-unique) frame rate is set by frame size, which is set by quality:

| `--quality` | KB / changed frame | keyframe | distinct fps (async, menu) |
|-------------|--------------------|----------|----------------------------|
| 80  | ~1.9  | ~14 KB | ~30 |
| 95  | ~5.7  | ~23 KB | ~10 |
| 98  | ~9.4  | ~28 KB | ~7  |
| 100 | ~12.4 | ~35 KB | ~5.5 |

Note the headline number some tools print (`frames_total / elapsed`) counts the duplicate
skip-replays and so *overstates* smoothness — the honest metric is `(frames_total − skips) /
elapsed`. For a **smooth high-quality** clip use **sync mode** (no drops). For **smooth live**
lower the quality (more frames, more artifacting). They are the same lever from opposite ends.

**Adapter & a known limitation.** The ST-Link V2 is the fast, stable probe — it saturates
USB-FS (~178 KB/s raw) by 4 MHz, so `--swd-khz 4000` is the sweet spot (10 MHz buys nothing,
1 MHz starves it). But OpenOCD's TCL RPC wedges after ~2700 SWD transactions regardless of clock
or adapter, so a long capture (especially the interactive `--live` cockpit) periodically drops the
link; the capture loop auto-recovers by killing the OpenOCD process, reopening, and re-arming with
a keyframe reset — a brief visible hitch every wedge. Reducing per-frame transactions or moving to
a persistent-connection backend would remove it.

---

## Build and run

The fastcap binary uses its own Makefile (standalone RAM image, separate linker script):

```bash
# Chainloader (always clean first); it carries the gui_refresh hook call site:
make clean && make -j$(nproc)

# Fastcap RAM binary (after make clean — the main clean wipes build/):
make -f Makefile.fastcap

# Flash the chainloader once (or after chainloader source changes):
python3 scripts/debug/trace.py reset-halt
make flash_chainloader

# Capture 30s, q100, with live preview + mp4:
python3 scripts/debug/fastcap.py capture --duration 30 --quality 100 --live

# Frame-perfect recording (sync, smooth playback at quality):
python3 scripts/debug/fastcap.py capture --duration 10 --mode sync --quality 100

# Single pristine uncompressed frame (no fastcap.bin needed):
python3 scripts/debug/fastcap.py grab --raw -o build/capture/grab.png

# Per-frame stats on a captured set:
python3 scripts/debug/fastcap.py analyze --dir build/capture
```

The chainloader is flashed once; `fastcap.bin` is injected into D2 SRAM at runtime by the host
and never needs flashing. `--swd-khz` defaults to a stable value; the ST-Link V2 is the reliable
adapter for this transaction rate.

---

## Future work: capturing the OFW (via the `src/patch` trampoline)

The codec currently hooks `gui_refresh()` and so only sees chainloader-rendered frames (menu,
theme art). To capture the **stock firmware** (Game & Watch OFW) we cannot hook `gui_refresh`
(no OFW source) — but the patch payload (`src/patch/main.c`) already trampolines the stock
`read_buttons()`, which is called every OFW UI tick. That trampoline is the OFW-side equivalent
of the chainloader hook, and the whole D2 codec + host pipeline can be reused unchanged.

**Plan (no OFW source required):**

1. **Patch-side hook slot.** Add a function-pointer slot at a fixed DTCM address (an
   `FASTCAP_OFW_HOOK_ADDR`, analogous to `FASTCAP_HOOK_ADDR`). In the existing `read_buttons()`
   wrapper, after the stock call returns and before the magic-combo check, call it if non-null.
   Default boots leave the slot zero, so the OFW path is unchanged. ~10 lines of C.
2. **Framebuffer from LTDC.** The chainloader passes `fb` into the codec; the OFW path instead
   reads `LTDC_L1CFBAR` (`0x500010AC`) for the active framebuffer address each call. The host
   already does this in `device.py`'s `read_framebuffer()` as a reference. One MMIO read/frame.
3. **Pixel format.** Confirm the OFW's LTDC layer-1 format (the chainloader uses RGB565). If the
   OFW renders ARGB8888, the codec's color conversion must widen; `device.py` handles both formats
   and is a useful template.
4. **JPEG codec ownership.** `jpeg_enc_init()` flips `RCC_AHB3ENR.JPGDECEN`. The OFW almost
   certainly never touches JPGDEC, so lazy-init is fine; verify against the OFW disassembly, and
   save/restore the JPEG registers around each call if there is any conflict.
5. **Cadence.** `read_buttons` is not VSYNC-locked, so the codec may occasionally sample
   mid-redraw; the tiled hash/JPEG path tolerates a slightly-torn frame as a few extra changed
   tiles. No correctness issue.

The host arms `FASTCAP_OFW_HOOK_ADDR` exactly like today and the protocol is identical — none of
it depends on which firmware was hooked.
