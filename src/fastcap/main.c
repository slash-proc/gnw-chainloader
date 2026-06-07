/*
 * fastcap_frame(const uint16_t *fb) — on-device tiled-JPEG framebuffer capture.
 *
 * Lives ENTIRELY in D2 AHB-SRAM1 (0x30000000, 64 KB) — a bank the chainloader
 * never uses — so it can never collide with the app, framebuffers, or the AXI
 * module pool (an earlier AXI-resident layout overwrote a loaded module and
 * bus-faulted; see DESIGN.md §1 "Runtime RAM Map").  The host must enable the
 * D2 clock (RCC_AHB2ENR bits 29/30) before loading this binary; the hook also
 * enables it on reinit, belt-and-suspenders.
 *
 * Called once per frame from gui_refresh() after the framebuffer is flushed.
 * The host arms the hook by writing this function's address to FASTCAP_HOOK_ADDR
 * (DTCM); gui_refresh() then calls it every frame with the live framebuffer.
 *
 * Codec — fixed 32x16 tile grid (10 x 15 = 150 tiles), everything JPEG:
 *   Each frame, hash every tile's pixels (FNV-1a) and compare to the stored
 *   hash.  Changed tiles (and ALL tiles on a keyframe) are hardware-JPEG-encoded
 *   and packed into the payload; unchanged tiles are skipped.  No previous-frame
 *   buffer — change detection is the 600-byte hash table, so the whole codec
 *   fits in 64 KB.  Tiles are 2 MCU (32x16) — the smallest the JPEG core encodes
 *   reliably, since a lone 16x16 MCU (CONFR2=0) faults the codec.
 *
 *   Header-strip: only the keyframe's first emitted tile carries the full JPEG
 *   header (quant+Huffman tables); the host caches that prefix and re-wraps every
 *   later headerless scan, saving ~600 B/tile.
 *
 *   Silent-skip: a frame whose tiles are all unchanged is NOT emitted at all —
 *   the device stays silent and counts it, reporting the run length in `skipped`
 *   on the next real frame so the host can replay duplicates onto the timeline.
 *   A tile_count=0 timing anchor fires every ANCHOR_SKIP silent frames so a long
 *   static stretch never opens an unbounded gap in the host's clock.
 *
 *   Drift: the device hashes the TRUE framebuffer, so detection is exact; the
 *   host holds JPEG-decoded tiles, so a tile that stops changing settles to its
 *   last-decoded value (fine for UI).  A periodic keyframe re-sends every tile.
 *
 * Payload at PAY_BUF (D2 AHB-SRAM1; one frame = one STATUS raise = one host PNG):
 *   u32 tile_count        (0 = pure timing anchor; host only replays `skipped`)
 *   u32 total_bytes       (bytes of tile records that follow)
 *   u32 skipped           (unchanged frames silently dropped before this one)
 *   tile_count x:
 *     u32 idx             (0..149; pixel origin = (idx%10 * 32, idx/10 * 16))
 *     u32 jpeg_len
 *     u8  jpeg[jpeg_len]  (packed, not aligned; next record follows immediately)
 *
 * Two capture modes, selected at RUNTIME via FASTCAP_MODE (read at reinit):
 *   async (0, default): non-blocking; if STATUS_FLAG is still set, count & drop
 *     the frame (display runs on).  ~49 fps on the live menu.
 *   sync  (1): raise STATUS_FLAG and spin-wait for the host ack each frame; no
 *     frame missed, display runs slower than real-time (frame-perfect).
 *
 * DTCM cells (keep in sync with src/common/memory_map.h):
 *   FASTCAP_STATUS_FLAG 0x2001FF00 | FASTCAP_HOOK_ADDR 0x2001FF04
 *   FASTCAP_RESET_FLAG  0x2001FF08 | FASTCAP_QUALITY   0x2001FF0C
 *   FASTCAP_MODE        0x2001FF14
 */

#include <stdint.h>
#include "jpeg_enc.h"

/* ---- DTCM synchronisation cells (cache-bypassing) ---- */
#define STATUS_FLAG  ((volatile uint32_t *)0x2001FF00UL)
#define HOOK_PTR     ((volatile uint32_t *)0x2001FF04UL)
#define RESET_FLAG   ((volatile uint32_t *)0x2001FF08UL)
#define QUALITY_CFG  ((volatile uint32_t *)0x2001FF0CUL)
#define MODE_CFG     ((volatile uint32_t *)0x2001FF14UL)

/* JPEG quality used when the host left the QUALITY cell unset (0). */
#define QUALITY_DEFAULT 98u

/* ---- RCC: enable the D2 AHB-SRAM1/2 clock (bits 29/30) ---- */
#define RCC_AHB2ENR  ((volatile uint32_t *)0x580244DCUL)
#define D2SRAM_EN    0x60000000UL

/* ---- DWT cycle counter (spin-wait + JPEG-encode timeouts) ---- */
#define DEMCR        ((volatile uint32_t *)0xE000EDFC)
#define DWT_CTRL     ((volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT   ((volatile uint32_t *)0xE0001004)
#define SCB_DCCMVAC  ((volatile uint32_t *)0xE000EF4C)

#define CPU_HZ             280000000UL
#define SPIN_TIMEOUT_MS    2000UL
#define SPIN_TIMEOUT_CYCS  ((uint32_t)(CPU_HZ / 1000UL * SPIN_TIMEOUT_MS))

/* Signal the host every FRAME_DIVISOR frames (1 = every gui_refresh). */
#define FRAME_DIVISOR 1
/* During a fully-silent stretch, emit a tile_count=0 timing anchor every
   ANCHOR_SKIP skipped frames so the host's timeline never gets a huge gap. */
#define ANCHOR_SKIP 30u
/* Force a full keyframe (all tiles) this often, to recover any host-side miss. */
#define KEYFRAME_INTERVAL 240u

/* ---- Screen / tile geometry (32x16 tiles, 2 MCU each) ---- */
#define SCREEN_W   320
#define SCREEN_H   240
#define TILE_W     32
#define TILE_H     16
#define TILES_X    (SCREEN_W / TILE_W)   /* 10  */
#define TILES_Y    (SCREEN_H / TILE_H)   /* 15  */
#define TILE_COUNT (TILES_X * TILES_Y)   /* 150 (32x16 = 2 MCU per tile, smallest viable) */

/* ---- D2 AHB-SRAM1 buffers (see DESIGN.md §1) ---- */
#define TILE_HASH  ((uint32_t *)0x30000000UL)  /* [TILE_COUNT] FNV-1a per tile; 1 KB reserve, YCC follows at 0x30000400 */
#define PAY_BUF    ((uint8_t  *)0x30002400UL)  /* record header + packed tile JPEGs */
#define PAYLOAD_CAP 0xBC00UL                   /* 47 KB up to the code region at 0x3000E000 */

/* ---- Magic: statics live in BSS, not zeroed for a host-loaded RAM image ---- */
#define FASTCAP_MAGIC 0xFC502345UL

/* ---- Payload frame header ---- */
typedef struct {
    uint32_t tile_count;    /* number of tile records (> 0 for an emitted frame) */
    uint32_t total_bytes;   /* bytes of tile records following */
    uint32_t skipped;       /* unchanged frames silently skipped before this one */
} fastcap_frame_hdr_t;

static uint32_t s_async;     /* 1 = async/non-blocking, 0 = sync/spin-wait */

/* Clean AXI/D2 cache lines to PoC so the DAP reads fresh data. */
static void cache_clean(void *addr, uint32_t size)
{
    uint32_t a   = (uint32_t)addr & ~31u;
    uint32_t end = ((uint32_t)addr + size + 31u) & ~31u;
    while (a < end) { *SCB_DCCMVAC = a; a += 32u; }
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

/* Raise STATUS_FLAG and spin-wait for the host ack; 1 if it timed out. */
static int spin_wait_ack(void)
{
    *STATUS_FLAG = 1;
    uint32_t t0 = *DWT_CYCCNT;
    while (*STATUS_FLAG != 0) {
        if ((*DWT_CYCCNT - t0) >= SPIN_TIMEOUT_CYCS) {
            *HOOK_PTR    = 0;
            *STATUS_FLAG = 0;
            return 1;
        }
    }
    return 0;
}

/* FNV-1a hash of one tile's pixels (exact change detection against the true fb). */
static uint32_t hash_tile(const uint16_t *fb, uint32_t tx, uint32_t ty)
{
    uint32_t h = 2166136261u;
    uint32_t x0 = tx * TILE_W, y0 = ty * TILE_H;
    for (uint32_t row = 0; row < TILE_H; row++) {
        const uint16_t *p = fb + (y0 + row) * SCREEN_W + x0;
        for (uint32_t col = 0; col < TILE_W; col++)
            h = (h ^ p[col]) * 16777619u;
    }
    return h;
}

void fastcap_frame(const uint16_t *fb)
{
    static uint32_t magic;
    static uint32_t frame_div;
    static uint32_t prev_valid;
    static uint32_t since_keyframe;
    static uint32_t skipped;        /* unchanged/dropped frames since the last emit */

    if (*RESET_FLAG) { *RESET_FLAG = 0; magic = 0; }

    if (magic != FASTCAP_MAGIC) {
        magic          = FASTCAP_MAGIC;
        frame_div      = 0;
        prev_valid     = 0;
        since_keyframe = 0;
        skipped        = 0;
        *RCC_AHB2ENR  |= D2SRAM_EN;     /* ensure D2 SRAM clocked */
        (void)*RCC_AHB2ENR;
        s_async = (*MODE_CFG == 1u) ? 0u : 1u;
        uint32_t q = *QUALITY_CFG;      /* host-selected JPEG quality (0 = unset) */
        jpeg_enc_init((q == 0u) ? QUALITY_DEFAULT : q);
    }

    if (++frame_div < FRAME_DIVISOR) return;
    frame_div = 0;

    *DEMCR    |= (1u << 24);
    *DWT_CTRL |= 1u;

    if (s_async && *STATUS_FLAG != 0) { skipped++; return; }   /* host still reading → count & drop */

    fastcap_frame_hdr_t *hdr = (fastcap_frame_hdr_t *)PAY_BUF;
    uint8_t *cursor = PAY_BUF + sizeof(fastcap_frame_hdr_t);
    uint8_t *limit  = PAY_BUF + PAYLOAD_CAP;

    /* Standalone --load (no live fb): nothing to capture, just count it. */
    const int fb_ok = ((uint32_t)fb >= 0x24000000UL && (uint32_t)fb < 0x24100000UL);
    if (!fb_ok) { skipped++; return; }

    const int keyframe = (!prev_valid || since_keyframe >= KEYFRAME_INTERVAL);
    uint32_t count = 0;
    int header_sent = 0;

    for (uint32_t t = 0; t < TILE_COUNT; t++) {
        uint32_t tx = t % TILES_X, ty = t / TILES_X;
        uint32_t h = hash_tile(fb, tx, ty);
        if (!keyframe && h == TILE_HASH[t]) continue;   /* unchanged */

        /* Need room for the 8-byte record header + this tile's JPEG. */
        if (cursor + 8 >= limit) break;
        uint32_t cap = (uint32_t)(limit - (cursor + 8));
        /* Only the keyframe's first emitted tile carries the JPEG header; the
           host caches it and re-wraps every subsequent headerless tile scan. */
        int with_header = (keyframe && !header_sent);
        uint32_t jlen = jpeg_encode_region(fb, tx * TILE_W, ty * TILE_H,
                                           TILE_W, TILE_H, cursor + 8, cap, with_header);
        if (jlen == 0) continue;                        /* encode failed/overflow → skip tile */
        if (with_header) header_sent = 1;

        ((uint32_t *)cursor)[0] = t;
        ((uint32_t *)cursor)[1] = jlen;
        cursor += 8 + jlen;
        TILE_HASH[t] = h;                               /* commit the new hash only on emit */
        count++;
    }

    /* Nothing changed → stay silent and count it, but emit a tile_count=0 timing
       anchor once the silent run reaches ANCHOR_SKIP so the host stays in step. */
    if (count == 0 && ++skipped < ANCHOR_SKIP) return;

    hdr->tile_count  = count;
    hdr->total_bytes = (uint32_t)(cursor - (PAY_BUF + sizeof(fastcap_frame_hdr_t)));
    hdr->skipped     = skipped;
    skipped = 0;
    if (count > 0) {            /* real frame (not a pure anchor) advances the delta state */
        prev_valid = 1;
        since_keyframe = keyframe ? 0u : (since_keyframe + 1u);
    }

    cache_clean(PAY_BUF, (uint32_t)(cursor - PAY_BUF));
    if (s_async) *STATUS_FLAG = 1; else (void)spin_wait_ack();
}

/*
 * Standalone vector table (fastcap.py --load test path). In normal operation the
 * host writes fastcap_frame's address to FASTCAP_HOOK_ADDR and gui_refresh()
 * calls it; this entry is unused. _estack is the top of D2 AHB-SRAM1.
 */
extern uint32_t _estack;

__attribute__((section(".isr_vector"), used))
uint32_t vector_table[] = {
    (uint32_t)&_estack,
    (uint32_t)fastcap_frame + 1,   /* Thumb bit set */
};
