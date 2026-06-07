#ifndef COMMON_MEMORY_MAP_H
#define COMMON_MEMORY_MAP_H

/*
 * Single source of truth for the addresses the chainloader and the OFW patch
 * agree on. The human-readable layout is documented in DESIGN.md (§1 internal
 * flash, §2 external SPI flash) — keep this header and that table in sync.
 */

/* --- Internal flash banks (see DESIGN.md §1) --- */
#define CHAINLOADER_BASE   0x08000000UL  /* Bank 1: this chainloader            */
#define RETROGO_BASE       0x08008000UL  /* Bank 1: Retro-Go launcher payload   */
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
#define MARIO_SPI_OFFSET               0x007C0000UL  /* default Mario OFW backup offset  */
#define ZELDA_SPI_OFFSET               0x007E0000UL  /* default Zelda OFW backup offset  */

/* --- SRAM magic cells --- */
/* Survive the system reset triggered by a bank swap, so the chainloader can
   carry the final jump target across the reset. 
   Stored at the top of DTCMRAM, safe from stack due to linker reservation. */
#define SRAM_MAGIC_ADDR    0x2001FFF8UL  /* boot magic word        (see boot_magic.h) */
#define SRAM_MAGIC_TARGET  0x2001FFFCUL  /* jump target address                       */

/* Retro-Go stamps its "CORE" magic here in DTCMRAM while running; surviving the
   reset signals a quit-to-launcher request. */
#define RG_MAGIC_ADDR      0x20000000UL

#endif /* COMMON_MEMORY_MAP_H */
