#ifndef COMMON_BOOT_MAGIC_H
#define COMMON_BOOT_MAGIC_H

/*
 * Boot magic values written to the SRAM magic cell (SRAM_MAGIC_ADDR) or the RTC
 * backup register. Shared by the chainloader and the OFW patch so the two never
 * disagree on the protocol. Documented in CLAUDE.md / GEMINI.md ("Magic Values").
 */

#define BOOT_MAGIC_BOOT     0x544F4F42UL  /* "BOOT" — standard jump to target at SRAM_MAGIC_TARGET */
#define BOOT_MAGIC_FORCE    0x45435246UL  /* "FRCE" — forced bank-swap jump                        */
#define BOOT_MAGIC_STANDBY  0xFEDEBEDAUL  /* Retro-Go standby resume (target defaults to RETROGO_BASE) */
#define BOOT_MAGIC_RETROGO  0x434F5245UL  /* "CORE" — Retro-Go running / quit-to-menu request      */
#define BOOT_MAGIC_RESET    0x1FA1AFE1UL  /* Retro-Go warm reset signature                         */
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
#endif

#endif /* COMMON_BOOT_MAGIC_H */
