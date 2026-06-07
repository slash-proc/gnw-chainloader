#include "system/crash_log.h"
#include "stm32h7xx.h"

#define CRASH_LOG ((volatile crash_log_t *)CRASH_LOG_ADDR)

/*
 * Record the faulting context into D3 SRAM, then halt. Reached from the naked
 * HardFault_Handler below with r0 = the stacked exception frame (MSP or PSP),
 * laid out as [R0, R1, R2, R3, R12, LR, PC, xPSR]. Never returns — returning to
 * a faulted frame is invalid, and halting matches the old Default_Handler (now
 * with the cause recorded for SWD readback). `used` so LTO keeps it despite the
 * only caller being the inline-asm branch.
 */
__attribute__((used, noinline))
void crash_log_capture(uint32_t *frame)
{
    uint32_t n = (CRASH_LOG->magic == CRASH_LOG_MAGIC) ? CRASH_LOG->count : 0u;

    CRASH_LOG->hfsr  = SCB->HFSR;
    CRASH_LOG->cfsr  = SCB->CFSR;
    CRASH_LOG->mmfar = SCB->MMFAR;
    CRASH_LOG->bfar  = SCB->BFAR;
    CRASH_LOG->r0  = frame[0];
    CRASH_LOG->r1  = frame[1];
    CRASH_LOG->r2  = frame[2];
    CRASH_LOG->r3  = frame[3];
    CRASH_LOG->r12 = frame[4];
    CRASH_LOG->lr  = frame[5];
    CRASH_LOG->pc  = frame[6];
    CRASH_LOG->psr = frame[7];
    CRASH_LOG->count = n + 1u;
    CRASH_LOG->magic = CRASH_LOG_MAGIC;   /* written last: a partial log stays invalid */

    __DSB();
    for (;;) { /* halt for SWD inspection */ }
}

/*
 * Strong override of the weak HardFault_Handler alias in startup.s. Selects the
 * stack the exception frame was pushed onto (MSP vs PSP) and tail-calls the
 * C capture routine with it.
 */
__attribute__((naked, used))
void HardFault_Handler(void)
{
    __asm volatile(
        "tst   lr, #4            \n"   /* EXC_RETURN[2]: 0 = MSP, 1 = PSP */
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "b     crash_log_capture \n"
    );
}

#ifdef CRASH_TEST
/* A free DTCM cell the host pokes to request a deliberate fault. */
#define CRASH_TEST_CELL ((volatile uint32_t *)0x2001FF10UL)
void crash_test_init(void)
{
    *CRASH_TEST_CELL = 0u;   /* DTCM isn't zeroed at reset — clear boot garbage */
}
void crash_test_check(void)
{
    if (!*CRASH_TEST_CELL) return;
    *CRASH_TEST_CELL = 0u;
    /* Read unconfigured FMC space → precise BusFault → escalates to HardFault. */
    volatile uint32_t v = *(volatile uint32_t *)0x60000000UL;
    (void)v;
}
#endif
