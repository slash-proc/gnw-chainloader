/*
 * Bare-metal STM32H7 option-byte bank-swap helpers.
 *
 * Reimplements, in direct register form (no HAL), the internal-flash bank-swap
 * technique from Marian Muller's game-and-watch-patch fork (BSD-3-Clause):
 *   https://github.com/marian-m12l/game-and-watch-patch
 * The option-byte program/launch sequence itself follows the STM32H7 reference
 * manual. See the LICENSE file at the repo root.
 */

#ifndef COMMON_BANKS_H
#define COMMON_BANKS_H

#include <stdint.h>
#include <stdbool.h>

#ifndef FLASH_R_BASE
#define FLASH_R_BASE          0x52002000UL
#endif
#define FLASH_OPTCR           (*(volatile uint32_t *)(FLASH_R_BASE + 0x18))
#define FLASH_OPTSR_CUR       (*(volatile uint32_t *)(FLASH_R_BASE + 0x1C))
#define FLASH_OPTSR_PRG       (*(volatile uint32_t *)(FLASH_R_BASE + 0x20))
#define FLASH_OPTCCR          (*(volatile uint32_t *)(FLASH_R_BASE + 0x24))
#define FLASH_OPTKEYR         (*(volatile uint32_t *)(FLASH_R_BASE + 0x08))

#define FLASH_CCR1            (*(volatile uint32_t *)(FLASH_R_BASE + 0x14))
#define FLASH_CCR2            (*(volatile uint32_t *)(FLASH_R_BASE + 0x114))

#ifndef FLASH_OPTKEY1
#define FLASH_OPTKEY1         0x08192A3B
#endif
#ifndef FLASH_OPTKEY2
#define FLASH_OPTKEY2         0x4C5D6E7F
#endif
#ifndef FLASH_OPTCR_OPTLOCK
#define FLASH_OPTCR_OPTLOCK   (1UL << 0)
#endif
#ifndef FLASH_OPTCR_OPTSTART
#define FLASH_OPTCR_OPTSTART  (1UL << 1)
#endif
#ifndef FLASH_OPTSR_OPT_BUSY
#define FLASH_OPTSR_OPT_BUSY  (1UL << 0)
#endif
#ifndef FLASH_OPTSR_SWAP_BANK
#define FLASH_OPTSR_SWAP_BANK (1UL << 31)
#endif
#ifndef FLASH_OPTCCR_CLR_OPTCHANGEERR
#define FLASH_OPTCCR_CLR_OPTCHANGEERR (1UL << 30)
#endif

#define FLASH_ERRORS_BANK1    0x05FF0000UL
#define FLASH_ERRORS_BANK2    0x05FF0000UL

#define AIRCR                 (*(volatile uint32_t *)0xE000ED0C)
#define AIRCR_VECTKEY_MASK    0x05FA0000UL
#define AIRCR_SYSRESETREQ     (1UL << 2)

static inline void nvic_system_reset(void) {
    __asm__ volatile ("dsb 0xF":::"memory");
    AIRCR  = (uint32_t)((AIRCR_VECTKEY_MASK) | (AIRCR_SYSRESETREQ));
    __asm__ volatile ("dsb 0xF":::"memory");
    for(;;) { }
}

static inline void unlock_flash_ob(void) {
    if ((FLASH_OPTCR & FLASH_OPTCR_OPTLOCK) != 0) {
        FLASH_OPTKEYR = FLASH_OPTKEY1;
        FLASH_OPTKEYR = FLASH_OPTKEY2;
    }
}

static inline void lock_flash_ob(void) {
    FLASH_OPTCR |= FLASH_OPTCR_OPTLOCK;
}

static inline bool set_bank_swap(bool swap) {
    uint32_t current_swap = (FLASH_OPTSR_CUR & FLASH_OPTSR_SWAP_BANK);
    if ((swap && current_swap != 0) || (!swap && current_swap == 0)) {
        return false;
    }

    FLASH_CCR1 = FLASH_ERRORS_BANK1;
    FLASH_CCR2 = FLASH_ERRORS_BANK2;
    FLASH_OPTCCR |= FLASH_OPTCCR_CLR_OPTCHANGEERR;

    unlock_flash_ob();
    if (swap) FLASH_OPTSR_PRG |= FLASH_OPTSR_SWAP_BANK;
    else      FLASH_OPTSR_PRG &= ~FLASH_OPTSR_SWAP_BANK;
    FLASH_OPTCR |= FLASH_OPTCR_OPTSTART;
    
    while ((FLASH_OPTSR_CUR & FLASH_OPTSR_OPT_BUSY) != 0);
    
    lock_flash_ob();
    nvic_system_reset();
    return true;
}

static inline bool ensure_unswapped_banks(void) {
    return set_bank_swap(false);
}

static inline bool ensure_swapped_banks(void) {
    return set_bank_swap(true);
}

#endif // COMMON_BANKS_H
