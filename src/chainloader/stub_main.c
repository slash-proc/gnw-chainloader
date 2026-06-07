#include "board.h"
#include "main.h"
#include "LzmaDec.h"
#include "../common/boot_magic.h"
#include "../common/memory_map.h"
#include "../common/stub_services.h"
#include <string.h>

/* These symbols will be provided by the blob object */
extern const uint8_t _binary_build_app_bin_lzma_start[];
extern const uint8_t _binary_build_app_bin_lzma_end[];

#define APP_RAM_BASE 0x24000000
#define LZMA_HEAP_RESET ((void*)1)

/* LZMA Allocator for the stub */
void *stub_lzma_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    static uint8_t lzma_heap[81920] __attribute__((aligned(4)));
    static size_t used = 0;
    void *res = &lzma_heap[used];
    used += (size + 3) & ~3;
    return res;
}
void stub_lzma_free(ISzAllocPtr p, void *address) { (void)p; (void)address; }
ISzAlloc stub_lzma_allocator = { stub_lzma_alloc, stub_lzma_free };

/* Published at a fixed flash address (see stub_services.h + the linker's
 * .stub_services section) so the RAM app reuses this LZMA decoder instead of
 * linking its own copy. The stub's boot decode below is unaffected. */
__attribute__((section(".stub_services"), used))
const stub_services_t g_stub_services = { STUB_SERVICES_MAGIC, LzmaDecode };

/* Inverse of xz's --armthumb BCJ pre-filter (LZMA SDK ARMT_Convert, decode, now_pos=0).
 * The app image is run through the ARMTHUMB filter before LZMA at build time so Thumb
 * BL/BLX branch targets become absolute and compress as repetition (~1.8 KB smaller);
 * here we undo it in place over the freshly-decoded image, before the cache flush + jump.
 * MUST stay in lock-step with the Makefile xz call (--armthumb). now_pos = 0 because the
 * whole image is decoded as one buffer starting at stream offset 0. */
static void armthumb_unfilter(uint8_t *data, size_t size) {
    if (size < 4) return;
    size_t lim = size - 4;
    for (size_t i = 0; i <= lim; i += 2) {
        if ((data[i + 1] & 0xF8) == 0xF0 && (data[i + 3] & 0xF8) == 0xF8) {
            uint32_t src = (((uint32_t)data[i + 1] & 7) << 19)
                         | ((uint32_t)data[i + 0] << 11)
                         | (((uint32_t)data[i + 3] & 7) << 8)
                         |  (uint32_t)data[i + 2];
            src <<= 1;
            uint32_t dest = (src - ((uint32_t)i + 4)) >> 1;   /* decode: subtract position */
            data[i + 1] = (uint8_t)(0xF0 | ((dest >> 19) & 7));
            data[i + 0] = (uint8_t)(dest >> 11);
            data[i + 3] = (uint8_t)(0xF8 | ((dest >> 8) & 7));
            data[i + 2] = (uint8_t)dest;
            i += 2;
        }
    }
}

int main(void) {
    /* 1. Hardware setup needed for logic (Clocks, GPIO, RTC, OSPI) */
    board_early_init();

    /* 2. POR Standby check (Prevent auto-boot on USB connect) */
    if (RCC->RSR & RCC_RSR_PORRSTF) {
        RCC->RSR |= RCC_RSR_RMVF;
        PWR->WKUPEPR |= (PWR_WKUPEPR_WKUPEN1 | PWR_WKUPEPR_WKUPP1);
        PWR->WKUPFR |= PWR_WKUPFR_WKUPF1;

        /* Select Standby mode for D1 and D3 domains */
        PWR->CPUCR |= (PWR_CPUCR_PDDS_D1 | PWR_CPUCR_PDDS_D3);
        SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
        __WFI();
        while (1) { NVIC_SystemReset(); }
    }

    /* 2.5. Retro-Go warm-reset trace (RESET at RG_MAGIC_ADDR): re-launch Retro-Go,
       preserving its live AXI-SRAM state, so its launcher reloads ("Return to Main
       Menu"). Use RETROGO_BASE, never a literal: a hardcoded 0x08008000 here went
       stale when the chainloader ceiling rose 32K->40K (RETROGO_BASE 0x08008000 ->
       0x0800A000) and jumped 8K short, into the chainloader image — that is what
       broke return-to-menu. board_jump_to_app() validates the target and returns
       (falls through to decompress the chainloader -> menu) if Retro-Go is absent. */
    if (boot_magic_check((volatile uint32_t *)RG_MAGIC_ADDR, BOOT_MAGIC_RESET)) {
        board_jump_to_app(RETROGO_BASE);
    }

    /* 3. Launcher Mode: Decompress the RAM image */
    SizeT inSize = (uint32_t)(_binary_build_app_bin_lzma_end - _binary_build_app_bin_lzma_start);
    SizeT outSize = 1024 * 1024; // Assume app fits in 1MB (AXI-SRAM size)
    ELzmaStatus status;


    if (LzmaDecode((uint8_t*)APP_RAM_BASE, &outSize, _binary_build_app_bin_lzma_start,
                   &inSize, (const uint8_t[]){0x37, 0x00, 0x00, 0x02, 0x00}, 5, LZMA_FINISH_ANY, &status, &stub_lzma_allocator) == SZ_OK) {

        /* Undo the ARMTHUMB BCJ pre-filter (applied by xz before LZMA) in place. */
        armthumb_unfilter((uint8_t*)APP_RAM_BASE, outSize);

        /* 2.5. Flush caches to ensure LTDC and CPU see the same RAM content */
        SCB_CleanDCache();
        SCB_InvalidateICache();

        /* 3. Jump to the newly decompressed RAM app */
        board_jump_to_app(APP_RAM_BASE);
    }

    /* Fatal error: Loop until someone notices */
    while (1) {
        wdog_refresh();
    }
}

void exit(int status) {
    (void)status;
    while(1);
}

