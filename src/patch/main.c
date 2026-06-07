/*
 * OFW recovery patch payload — appended into the stock firmware image.
 *
 * Substantially derived from game-and-watch-patch (BSD-3-Clause), via Marian
 * Muller's fork which this project used as its reference. The LZMA plumbing
 * (memcpy_inflate, rwdata_inflate, bss_rwdata_init), start_app(), and the
 * read_buttons() scaffolding are from those projects and are in part copied
 * verbatim; check_start_button() and the bootloader() hook logic are original
 * to gnw-chainloader. (The entry symbol is named `bootloader` to match the
 * gnwmanager patch script's `replace(0x4, "bootloader")`; it returns control to
 * the chainloader launcher in Bank 1.)
 *
 *   Brian Pugh    — https://github.com/brianpugh/game-and-watch-patch
 *   Marian Muller — https://github.com/marian-m12l/game-and-watch-patch
 *   Original notice: Copyright 2020 Konrad Beckmann, Thomas Roth, STMicroelectronics
 *
 * Redistributed under BSD-3-Clause; see the LICENSE file at the repo root.
 */

#include <stdint.h>
#include <stdbool.h>
#include "banks.h"
#include "memory_map.h"
#include "boot_magic.h"
#include "LzmaDec.h"

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

const uint8_t * const SMB1_ROM __attribute__((used)) = (const uint8_t *)0x90001e60;


/* Magic protocol shared with the chainloader — values come from the common headers. */
#define CHAINLOADER_MAGIC_ADDRESS ((volatile uint32_t *)SRAM_MAGIC_ADDR)
#define CHAINLOADER_JUMP_ADDRESS  ((volatile uint32_t *)SRAM_MAGIC_TARGET)
#define CHAINLOADER_MAGIC         BOOT_MAGIC_BOOT
#define CHAINLOADER_MAGIC_FORCE   BOOT_MAGIC_FORCE

#define GAMEPAD_LEFT     (1 << 1)
#define GAMEPAD_GAME     (1 << 10)

#ifdef REMOTE_INPUT
/* Full remote -> stock button translation. The host writes a bitmask in the
 * chainloader's "unified format" (input_button_t enum bit order); the stock OFW
 * packs its buttons differently. This table maps each shadow-cell bit to the
 * stock gamepad bit, derived from the stock read_buttons() of both Mario
 * (0x08010D48) and Zelda (0x08016808) via scripts/debug/disasm_stock_buttons.py.
 * The shared buttons use identical bit positions in both games, and Zelda's
 * extra START/SELECT bits are simply unread by Mario (so setting them is
 * harmless). Index = input_button_t:
 *   UP DOWN LEFT RIGHT A B START SELECT PAUSE GAME TIME PWR */
static const uint16_t remote_to_stock[12] = {
    0x008, 0x004, 0x002, 0x001,  /* UP DOWN LEFT RIGHT */
    0x080, 0x040,                /* A B                 */
    0x010, 0x020,                /* START SELECT (Zelda) */
    0x200, 0x400, 0x100,         /* PAUSE GAME TIME     */
    0x000                        /* PWR: no gamepad bit  */
};
#endif

/* Per-console stock-firmware addresses. The build MUST supply these via
 * -DSTOCK_RESET_HANDLER / -DSTOCK_READ_BUTTONS (the Makefile compiles this file
 * once per console: Mario 0x08017a45 / 0x08010d49, Zelda 0x0801ad49 / 0x08016809).
 * Erroring out is intentional: a missing flag would otherwise bake Mario's
 * addresses into a Zelda patch and corrupt its reset and button handling. */
#ifndef STOCK_RESET_HANDLER
#error "STOCK_RESET_HANDLER undefined: build the patch per-console (-DSTOCK_RESET_HANDLER=0x...)"
#endif
#ifndef STOCK_READ_BUTTONS
#error "STOCK_READ_BUTTONS undefined: build the patch per-console (-DSTOCK_READ_BUTTONS=0x...)"
#endif

static void __attribute__((naked)) start_app(uint32_t pc, uint32_t sp) {
    __asm__ volatile (
        "msr msp, r1 \n\t"
        "bx r0       \n\t"
    );
}

static inline bool check_start_button(void) {
    // 1. Enable GPIOC clock via RCC_AHB4ENR (bit 2)
    (*(volatile uint32_t *)0x580244E0) |= (1UL << 2);
    // Dummy read for clock stabilization
    (void)(*(volatile uint32_t *)0x580244E0);
    
    // 2. Configure GPIOC Pin 11 as Input (MODER bits 23:22 = 00)
    (*(volatile uint32_t *)0x58020800) &= ~(3UL << 22);
    
    // 3. Configure GPIOC Pin 11 with Pull-up (PUPDR bits 23:22 = 01)
    volatile uint32_t *gpioc_pupdr = (volatile uint32_t *)(0x58020800 + 0x0C);
    *gpioc_pupdr = (*gpioc_pupdr & ~(3UL << 22)) | (1UL << 22);
    
    // 4. Add a short delay for input state to stabilize
    for (volatile int i = 0; i < 2000; i++) {
        __asm__ volatile ("nop");
    }
    
    // 5. Read GPIOC Pin 11 (IDR bit 11). Pressed button pulls Pin 11 to GND (low).
    return ((*(volatile uint32_t *)(0x58020800 + 0x10)) & (1UL << 11)) == 0;
}

void wdog_refresh(void) {
    // Stub for patch payload
}

void bootloader(void) {
    // If START button is held at boot, return to Bank 1 (unswapped)
    if (check_start_button()) {
        ensure_unswapped_banks();
    }

    // Handle the forced chainloader magic
    if (*CHAINLOADER_MAGIC_ADDRESS == CHAINLOADER_MAGIC_FORCE) {
        uint32_t target = *CHAINLOADER_JUMP_ADDRESS;
        uint32_t sp = ((uint32_t*)target)[0];
        uint32_t pc = ((uint32_t*)target)[1];
        ensure_unswapped_banks();
        *CHAINLOADER_MAGIC_ADDRESS = 0;
        start_app(pc, sp);
    }
    
    // Original fw start flow
    *CHAINLOADER_MAGIC_ADDRESS = 0;
    start_app((uint32_t)STOCK_RESET_HANDLER, *(uint32_t *)CHAINLOADER_BASE);
}

uint16_t read_buttons(void) {
    uint16_t (* const stock_read_buttons_func)(void) = (void*)STOCK_READ_BUTTONS;
    uint16_t gamepad = stock_read_buttons_func();
#ifdef REMOTE_INPUT
    uint32_t remote = *(volatile uint32_t *)SRAM_REMOTE_INPUT_ADDR;
    for (int i = 0; i < 12; i++) {
        if (remote & (1u << i)) gamepad |= remote_to_stock[i];
    }
#endif
    if ((gamepad & GAMEPAD_LEFT) && (gamepad & GAMEPAD_GAME)) {
        *CHAINLOADER_MAGIC_ADDRESS = CHAINLOADER_MAGIC_FORCE;
        *CHAINLOADER_JUMP_ADDRESS = CHAINLOADER_BASE;
        ensure_unswapped_banks();
        nvic_system_reset();
    }
    return gamepad;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    char *p = s;
    while (n--) {
        *p++ = c;
    }
    return s;
}

int32_t *bss_rwdata_init(int32_t *table) {
    /* Copy init values from text to data */
    uint32_t *init_values_ptr = &_sidata;
    uint32_t *data_ptr = &_sdata;

    if (init_values_ptr != data_ptr) {
        for (; data_ptr < &_edata;) {
            *data_ptr++ = *init_values_ptr++;
        }
    }

    /* Clear the zero segment */
    for (uint32_t *bss_ptr = &_sbss; bss_ptr < &_ebss;) {
        *bss_ptr++ = 0;
    }
    return table;
}

const uint8_t LZMA_PROP_DATA[5] = {0x5d, 0x00, 0x40, 0x00, 0x00};
#define LZMA_BUF_SIZE            16256

static void *SzAlloc(ISzAllocPtr p, size_t size) {
    void* res = p->Mem;
    return res;
}

static void SzFree(ISzAllocPtr p, void *address) {
}

static unsigned char lzma_heap[LZMA_BUF_SIZE];

void *memcpy_inflate(uint8_t *dst, const uint8_t *src, size_t n) {
    ISzAlloc allocs = {
        .Alloc = SzAlloc,
        .Free = SzFree,
        .Mem = lzma_heap,
    };

    ELzmaStatus status;
    size_t dst_len = 393216;
    LzmaDecode(dst, &dst_len, src, &n, LZMA_PROP_DATA, 5, LZMA_FINISH_ANY, &status, &allocs);
    return dst;
}

int32_t *rwdata_inflate(int32_t *table) {
    uint8_t *data = (uint8_t *)table + table[0];
    int32_t len = table[1];
    uint8_t *ram = (uint8_t *)table[2];
    memcpy_inflate(ram, data, len);
    return table + 3;
}