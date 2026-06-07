#ifndef CHAINLOADER_SYSTEM_CRASH_LOG_H
#define CHAINLOADER_SYSTEM_CRASH_LOG_H

#include <stdint.h>

/*
 * Persistent crash log in D3 SRD-SRAM (0x38000000). The chainloader's HardFault
 * handler records the faulting context here, then halts, so a crash can be read
 * back over SWD after the fact (and the record survives a warm reset while the D3
 * domain stays powered). Read it with scripts/debug/crash_log.py.
 *
 * The configurable faults (MemManage/BusFault/UsageFault) are left disabled, so
 * every fault escalates to HardFault — one handler captures all of them and CFSR
 * identifies the actual cause. D3 SRAM is otherwise unused (board.c already
 * clocks it), so the log has the bank to itself.
 */

#define CRASH_LOG_ADDR   0x38000000UL
#define CRASH_LOG_MAGIC  0xBADC0DE5UL    /* present in `magic` once a crash is recorded */

typedef struct {
    uint32_t magic;   /* CRASH_LOG_MAGIC when valid (written last) */
    uint32_t count;   /* total crashes recorded (accumulates across resets) */
    uint32_t hfsr;    /* SCB->HFSR  (HardFault status) */
    uint32_t cfsr;    /* SCB->CFSR  (UsageFault:31..16 | BusFault:15..8 | MemManage:7..0) */
    uint32_t mmfar;   /* SCB->MMFAR (faulting addr; valid iff CFSR.MMARVALID) */
    uint32_t bfar;    /* SCB->BFAR  (faulting addr; valid iff CFSR.BFARVALID) */
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;   /* stacked exception frame */
} crash_log_t;

/* Enable precise capture of the configurable faults (MemManage/Bus/Usage). Call once at
 * boot; without it every fault escalates to HardFault (still captured, less precise). */
void crash_log_init(void);

/* Surface a recorded crash to the user once at boot. A warm reset preserves the D3 record,
 * so crash_log_pending() is true on the next boot; format it with crash_log_summary() and
 * crash_log_ack() to clear (the full record stays readable over SWD until then). */
int  crash_log_pending(void);
void crash_log_summary(char *buf, int n);
void crash_log_ack(void);

#ifdef CRASH_TEST
/* Build-gated self-test: poll a DTCM cell and deliberately fault when the host
 * writes it, to verify the crash-log path. Compiled out unless -DCRASH_TEST. */
void crash_test_init(void);    /* clear the trigger cell (DTCM boot garbage) */
void crash_test_check(void);
#endif

#endif /* CHAINLOADER_SYSTEM_CRASH_LOG_H */
