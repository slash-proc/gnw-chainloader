#ifndef COMMON_MEMORY_MAP_H
#define COMMON_MEMORY_MAP_H

/*
 * Single source of truth for the addresses the chainloader and the OFW patch
 * agree on. The human-readable layout is documented in DESIGN.md (§1 internal
 * flash, §2 external SPI flash) — keep this header and that table in sync.
 */

/* --- Internal flash banks (see DESIGN.md §1) --- */
#define CHAINLOADER_BASE   0x08000000UL  /* Bank 1: this chainloader            */
#define RETROGO_BASE       0x0800A000UL  /* Bank 1: Retro-Go launcher payload (chainloader ceiling raised 32K->40K) */
#define OFW_INTERNAL_BASE  0x08100000UL  /* Bank 2: relocated Mario/Zelda OFW   */
#define OFW_INTERNAL_SIZE  (128U * 1024U)/* 128 KiB OFW image                   */

/* --- External SPI flash (memory-mapped, see DESIGN.md §2) --- */
/*
 * The DEFAULT_* constants below reflect the standard layout produced by the OFW
 * patching pipeline.  They are used as first-try probe hints in the Partition Viewer
 * (see extflash-parsing.md §0).  A partition is registered only when its content
 * signature validates at the probed address — these values are never assumed to be
 * correct.  In the future they will be replaced by a configurable layout table.
 */
#define EXTFLASH_BASE                  0x90000000UL
#define DEFAULT_ZELDA_EXT_BLOCK_OFFSET 0x00000000UL  /* default Zelda asset block start  */
#define ZELDA_EXT_BLOCK_SIZE           (4U * 1024U * 1024U)  /* fixed size (patcher output) */
#define DEFAULT_MARIO_EXT_BLOCK_OFFSET 0x00400000UL  /* default Mario asset block start  */
#define MARIO_EXT_BLOCK_SIZE           (1U * 1024U * 1024U)  /* fixed size (patcher output) */
/* OFW backups, moved up to sit contiguously right behind the asset blocks (Stage 3 reslice).
 * Flash order: Zelda backup then Mario backup, each OFW_INTERNAL_SIZE (128 KiB). */
#define ZELDA_SPI_OFFSET               0x00500000UL  /* Zelda OFW backup: right after the Mario asset block */
#define MARIO_SPI_OFFSET               0x00520000UL  /* Mario OFW backup: right after the Zelda backup */

/* Fast-path probe hints for the Retro-Go LittleFS (the chainloader's
 * themes/config module source), used at boot before the exhaustive partition
 * scan and validated by the on-flash superblock magic — a wrong/absent guess
 * finds nothing and falls through to the full scan (never assumed correct, same
 * philosophy as the DEFAULT_* hints above).
 *
 * IMPORTANT: this LittleFS is stored INVERTED — its superblock lives in the LAST
 * block(s) of flash, and the nominal partition start reads erased (0xFF). So the
 * boot probe leads with the END of flash (last MODULE_LFS_END_WINDOW bytes); the
 * partition scanner back-computes the real start from there. The two start
 * offsets below are kept only as fallbacks for a hypothetical NON-inverted image
 * at the standard patcher layouts (DESIGN.md §2): SD-card variant (no FrogFS) at
 * 10 MiB (after the FAT module store), full 64 MB layout (after FrogFS) at 56 MiB. */
#define MODULE_LFS_END_WINDOW          0x00010000UL  /* 64 KiB end-of-flash window (inverted superblock) */
#define MODULE_LFS_OFFSET_SD           0x00A00000UL  /* 10 MiB: SD-card variant start (after the FAT store); == RG_EXTFLASH_OFFSET */
#define MODULE_LFS_OFFSET_FLASH        0x03800000UL  /* 56 MiB: full 64 MB layout start (non-inverted fallback) */

/* --- Module store FAT partition (Stage 3 of the tiered module-memory design) ---
 * A FAT partition holding loadable module .bin files and the full RW filesystem drivers
 * (/fs/fat.bin, /fs/lfs.bin), and once the split-segment loader lands, XIP module code executed
 * in place from this mapped region. The SD-variant external flash is fully contiguous:
 *   Zelda assets | Mario assets | Zelda backup | Mario backup | FAT store | LittleFS
 * The store spans from just after the two OFW backups to the LittleFS (MODULE_LFS_OFFSET_SD,
 * 10 MiB == RG_EXTFLASH_OFFSET), so ~4.75 MiB. It is OPTIONAL: the chainloader and the installer
 * fall back to LittleFS when no FAT store is present. See docs/memory-architecture.md. */
#define MODULE_FAT_OFFSET              0x00540000UL  /* right after the two 128 KiB OFW backups */
#define MODULE_FAT_SIZE                (MODULE_LFS_OFFSET_SD - MODULE_FAT_OFFSET)  /* ~4.75 MiB, up to the LittleFS */

/* --- SRAM magic cells --- */
/* Survive the system reset triggered by a bank swap, so the chainloader can
   carry the final jump target across the reset.
   Stored at the top of DTCMRAM, safe from stack due to linker reservation. */
#define SRAM_MAGIC_ADDR    0x2001FFF8UL  /* boot magic word        (see boot_magic.h) */
#define SRAM_MAGIC_TARGET  0x2001FFFCUL  /* jump target address                       */

/* Remote-input shadow cell (debug/test instrumentation, -DREMOTE_INPUT builds).
 * One word below the magic cells, in the same top-of-DTCMRAM zone the OFW stack
 * reservation protects. A debug-probe write of a button bitmask here is OR'd into
 * the live button state by the chainloader (input.c) and the OFW patch
 * (patch/main.c), letting scripts/debug/remote_input.py drive the UI over SWD
 * with no hardware mod. Bit positions follow the chainloader's input_button_t
 * enum (the "unified format"); the patch translates the bits it understands. */
#define SRAM_REMOTE_INPUT_ADDR 0x2001FFF4UL

/* --- Fast framebuffer capture (fastcap) handshake cells --- */
/* All live in DTCM, which is tightly-coupled to the Cortex-M7 core and
   completely bypasses the L1 D-Cache.  The CPU and SWD (AHB-AP) therefore
   see the same physical word with no coherency gap, making them safe for
   flag-based spin-wait synchronisation (AXI SRAM is L1 D-Cache eligible and
   would deadlock after the cache line goes hot).
   The fastcap codec and its payload buffer live entirely in D2 AHB-SRAM1
   (0x30000000; see DESIGN.md §1) — only these handshake words sit in DTCM.
   They occupy the end of the stale stub heap/stack region, below the
   reserved boot-magic pair:
     0x20001000–0x2001FEFF  stub heap/stack (free after hand-off)
     0x2001FF00             FASTCAP_STATUS_FLAG  ← STATUS_FLAG in fastcap code
     0x2001FF04             FASTCAP_HOOK_ADDR
     0x2001FF08             FASTCAP_RESET_FLAG
     0x2001FF0C             FASTCAP_QUALITY      ← host writes JPEG quality 1..100
     0x2001FF14             FASTCAP_MODE         ← host writes 0 = async, 1 = sync
     0x2001FF10, 0x2001FF18–0x2001FFF3  (available)
     0x2001FFF4             SRAM_REMOTE_INPUT_ADDR  ← off-limits (REMOTE_INPUT builds)
     0x2001FFF8–0x2001FFFF  SRAM_MAGIC_ADDR / SRAM_MAGIC_TARGET  ← off-limits  */
#define FASTCAP_STATUS_FLAG  0x2001FF00UL  /* u32: device writes 1 when frame ready; host acks with 0 */
#define FASTCAP_HOOK_ADDR    0x2001FF04UL  /* void(*)(const uint16_t *fb): host writes entry; chainloader calls it */
#define FASTCAP_RESET_FLAG   0x2001FF08UL  /* u32: host writes 1 to force a fresh keyframe; device clears on next call */
#define FASTCAP_QUALITY      0x2001FF0CUL  /* u32: host writes JPEG quality 1..100 (0 = device default); read at reinit */
#define FASTCAP_MODE         0x2001FF14UL  /* u32: host writes 0 = async/live, 1 = sync/frame-perfect; read at reinit */

/* --- D2 AHB-SRAM2 borrowable scratch (0x30010000, 64 KB) ---
 * Idle while the launcher menu runs: the OFW patch only touches this bank during an OFW boot, and
 * fastcap only touches D2 AHB-SRAM1 (0x30000000) during a live SWD capture. A transient feature
 * module (only one runs at a time) borrows it via the feature host's scratch_get() for a big
 * CPU-only working buffer (e.g. the PNG inflate window), keeping that buffer out of the AXI module
 * pool. NOT DMA-reachable: SAI1/DMA1 are AXI-connected and cannot reach the D2 domain, so this is
 * CPU access only (audio DMA buffers must stay in AXI). The D2 SRAM clock is enabled at SystemInit. */
#define D2_SCRATCH_BASE  0x30010000UL
#define D2_SCRATCH_SIZE  (64U * 1024U)

/* The RG magic cell. Retro-Go's persistent boot_magic lives here at the very start
   of DTCM (it is the first .persistent var in Retro-Go's link map). A surviving
   "CORE" (Return to Main Menu) or "RESET" (warm-reset trace) means RE-LAUNCH
   Retro-Go so its own launcher reloads -> jump RETROGO_BASE (the stub consumes
   RESET; main.c app_early_logic() §2.1 consumes CORE). It does NOT mean "go to the
   chainloader menu". See GEMINI.md (RETRO-GO RETURN-TO-MENU) and boot_magic.h. */
#define RG_MAGIC_ADDR      0x20000000UL

#endif /* COMMON_MEMORY_MAP_H */
