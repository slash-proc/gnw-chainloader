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
 * Strong overrides of the weak fault-handler aliases in startup.s. Each selects the
 * stack the exception frame was pushed onto (MSP vs PSP) and tail-calls the C capture
 * routine. crash_log_init() enables the configurable faults so a MemManage/Bus/Usage
 * fault is taken by its own precise handler instead of escalating to HardFault (CFSR
 * identifies the cause either way).
 */
#define FAULT_HANDLER(name)                          \
    __attribute__((naked, used)) void name(void) {   \
        __asm volatile(                              \
            "tst   lr, #4            \n"             \
            "ite   eq                \n"             \
            "mrseq r0, msp           \n"             \
            "mrsne r0, psp           \n"             \
            "b     crash_log_capture \n");           \
    }
FAULT_HANDLER(HardFault_Handler)
FAULT_HANDLER(MemManage_Handler)
FAULT_HANDLER(BusFault_Handler)
FAULT_HANDLER(UsageFault_Handler)

/* Enable precise capture of the configurable faults. Call once at boot. */
void crash_log_init(void)
{
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
}

/* --- Surfacing: show a recorded crash to the user once at boot ----------------------
 * The D3 record survives a warm reset, so a crash that reset (rather than froze) shows
 * a one-line summary on the next boot. crash_log.py still reads the full record over SWD. */
int crash_log_pending(void) { return CRASH_LOG->magic == CRASH_LOG_MAGIC; }

#include "utils.h"

static char *crash_hex8(char *p, uint32_t v)
{
    *p++ = '0'; *p++ = 'x';
    hex_to_str(v, p, 8);
    return p + 8;
}
void crash_log_summary(char *buf, int n)
{
    if (n < 40) { if (n > 0) buf[0] = '\0'; return; }
    char *p = buf;
    const char *a = "Crash PC ", *b = " CFSR ";
    while (*a) *p++ = *a++;
    p = crash_hex8(p, CRASH_LOG->pc);
    while (*b) *p++ = *b++;
    p = crash_hex8(p, CRASH_LOG->cfsr);
    *p = '\0';
}
void crash_log_ack(void) { CRASH_LOG->magic = 0u; }

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
