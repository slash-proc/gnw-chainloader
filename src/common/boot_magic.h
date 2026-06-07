#ifndef COMMON_BOOT_MAGIC_H
#define COMMON_BOOT_MAGIC_H

/*
 * Boot magic values. Most are written to the SRAM magic cell (SRAM_MAGIC_ADDR,
 * 0x2001FFF8) or the RTC backup register. The two Retro-Go return signals —
 * BOOT_MAGIC_RETROGO ("CORE") and BOOT_MAGIC_RESET — instead live in the RG magic
 * cell (RG_MAGIC_ADDR, 0x20000000), where Retro-Go leaves them; both mean
 * "re-launch Retro-Go so its own launcher reloads" (jump RETROGO_BASE), NOT "go to
 * the chainloader menu" (see GEMINI.md, RETRO-GO RETURN-TO-MENU IS INVIOLABLE).
 * Shared by the chainloader and the OFW patch so the two never disagree on the
 * protocol. Documented in CLAUDE.md / GEMINI.md ("Magic Values").
 */

#define BOOT_MAGIC_BOOT     0x544F4F42UL  /* "BOOT" — standard jump to target at SRAM_MAGIC_TARGET */
#define BOOT_MAGIC_FORCE    0x45435246UL  /* "FRCE" — forced bank-swap jump                        */
#define BOOT_MAGIC_STANDBY  0xFEDEBEDAUL  /* Retro-Go standby resume (target defaults to RETROGO_BASE) */
#define BOOT_MAGIC_RETROGO  0x434F5245UL  /* "CORE" — Retro-Go "Return to Main Menu": re-launch Retro-Go (RG cell, main.c §2.1) */
#define BOOT_MAGIC_RESET    0x1FA1AFE1UL  /* Retro-Go warm-reset trace (RG cell): re-launch Retro-Go (stub) */
#define BOOT_MAGIC_FASTBOOT 0x5254524FUL  /* "RTRO" — boot straight to retro-go if fast-boot is enabled */


#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>

/* Helper to check and clear a specific magic value at an address */
static inline bool boot_magic_check(volatile uint32_t *addr, uint32_t expected) {
    if (*addr == expected) {
        *addr = 0;
        return true;
    }
    return false;
}

/* ---- Packed settings word (TAMP->BKP3R) ----
 * Replaces the old standalone fast-boot magic. Battery-backed: survives reset,
 * lost on power/VBAT loss (then reads 0 -> invalid signature -> all defaults,
 * i.e. a graceful full boot + per-OFW default theme). Read-modify-write the whole
 * word to change one field. Per-OFW theme memory, one nibble each:
 *   [31:24] signature 0xA6  (must differ from the legacy 'R' = 0x52)
 *   [15:12] Zelda theme slot (0-7) } 0=default, 1=fallback, 2-7=custom module
 *   [11: 8] Mario theme slot (0-7) }   (up to 6 module themes per console)
 *   [    0] fast-boot enable
 *   (other bits reserved, 0)
 * e.g. low 16 bits 0x1101 = both OFWs on fallback + fast-boot on.
 *
 * Migration: the legacy layout kept the two 2-bit slots at [7:4]; the new getters
 * read [15:8] (zero in an old word), so an upgraded device reads slot 0 (default)
 * for both until the next write — fast-boot (bit 0) and the signature are
 * unchanged, so it survives. settings_make() rebuilds the whole word, clearing
 * the stale legacy bits on the first theme/fast-boot change.
 */
#define SETTINGS_SIG          0xA6u
#define SETTINGS_SIG_SHIFT    24
#define SETTINGS_FASTBOOT_BIT (1u << 0)
#define SETTINGS_MARIO_SHIFT  8
#define SETTINGS_ZELDA_SHIFT  12
#define SETTINGS_SLOT_MASK    0xFu   /* one nibble per OFW; valid values 0-7 */
#define SETTINGS_LANG_SHIFT   16     /* [23:16] UI language index (0 = English) */
#define SETTINGS_LANG_MASK    0xFFu

static inline bool settings_valid(uint32_t w) {
    return ((w >> SETTINGS_SIG_SHIFT) & 0xFFu) == SETTINGS_SIG;
}
static inline uint32_t settings_make(bool fastboot, uint8_t mario_slot, uint8_t zelda_slot,
                                     uint8_t lang) {
    return ((uint32_t)SETTINGS_SIG << SETTINGS_SIG_SHIFT)
         | ((uint32_t)(lang & SETTINGS_LANG_MASK) << SETTINGS_LANG_SHIFT)
         | ((uint32_t)(zelda_slot & SETTINGS_SLOT_MASK) << SETTINGS_ZELDA_SHIFT)
         | ((uint32_t)(mario_slot & SETTINGS_SLOT_MASK) << SETTINGS_MARIO_SHIFT)
         | (fastboot ? SETTINGS_FASTBOOT_BIT : 0u);
}
/* All getters return the safe default (false / slot 0) on an invalid word. */
static inline bool settings_fastboot(uint32_t w) {
    return settings_valid(w) && (w & SETTINGS_FASTBOOT_BIT);
}
static inline uint8_t settings_mario_slot(uint32_t w) {
    return settings_valid(w) ? (uint8_t)((w >> SETTINGS_MARIO_SHIFT) & SETTINGS_SLOT_MASK) : 0u;
}
static inline uint8_t settings_zelda_slot(uint32_t w) {
    return settings_valid(w) ? (uint8_t)((w >> SETTINGS_ZELDA_SHIFT) & SETTINGS_SLOT_MASK) : 0u;
}
static inline uint8_t settings_lang(uint32_t w) {
    return settings_valid(w) ? (uint8_t)((w >> SETTINGS_LANG_SHIFT) & SETTINGS_LANG_MASK) : 0u;
}
#endif

#endif /* COMMON_BOOT_MAGIC_H */
