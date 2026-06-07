#ifndef SYS_BENCH_H
#define SYS_BENCH_H

/*
 * Boot-timing instrumentation. Opt-in only: built with `make BOOT_BENCH=1 ...`,
 * which adds -DBOOT_BENCH. The default (golden) build defines neither the arrays
 * nor the marks, so it carries zero bytes — same pattern as REMOTE_INPUT.
 *
 * Two arrays, both read back over SWD after boot (resolve their 0x240xxxxx
 * addresses with `nm`, dump with scripts/debug/boot_bench.py):
 *
 *   g_boot_bench[]  — coarse boot milestones. BENCH_MARK(i) stamps g_boot_bench[i]
 *                     with the current ms tick. Index map in
 *                     docs/startup-module-probe.md: 0=launcher commit, 1=menu
 *                     loop, 2=first frame, 3=modules ready, 4=theme applied,
 *                     5=first themed frame; init sub-phases: 6=board_init done,
 *                     7=ospi_init done, 8=gui_init done; SD-probe sub-phases:
 *                     9=probe start, 10=after detect, 11=after mount,
 *                     12=after statfs, 13=after unmount.
 *
 *   g_scan_bench[]  — per partition-scan-stage entry ticks, one slot per
 *                     scan_state_internal_t value (partition.c). BENCH_SCAN_ENTER
 *                     stamps the FIRST tick each state is observed, so consecutive
 *                     non-zero slots give per-stage durations. Slot order MUST
 *                     match the enum in partition.c:
 *                       0 IDLE  1 PROBE_SD  2 PROBE_LFS  3 SCAN_INT1  4 GAP_INT1
 *                       5 SCAN_INT2  6 GAP_INT2  7 SCAN_EXT  8 GAP_EXT  9 COMPLETE
 */

#ifdef BOOT_BENCH

#include <stdint.h>

uint32_t HAL_GetTick(void);   /* avoid pulling the full HAL header in here */

#define BOOT_BENCH_N 16
#define SCAN_BENCH_N 12       /* IDLE..COMPLETE (+headroom), keep in sync with partition.c */

extern volatile uint32_t g_boot_bench[BOOT_BENCH_N];
extern volatile uint32_t g_scan_bench[SCAN_BENCH_N];

#define BENCH_MARK(i) \
    do { if ((unsigned)(i) < BOOT_BENCH_N) g_boot_bench[(i)] = HAL_GetTick(); } while (0)

/* Stamp the first tick a scan state is seen (later re-entries leave it). */
#define BENCH_SCAN_ENTER(state) \
    do { unsigned s_ = (unsigned)(state); \
         if (s_ < SCAN_BENCH_N && g_scan_bench[s_] == 0) g_scan_bench[s_] = HAL_GetTick(); \
    } while (0)

#else

#define BENCH_MARK(i)           ((void)0)
#define BENCH_SCAN_ENTER(state) ((void)0)

#endif /* BOOT_BENCH */

#endif /* SYS_BENCH_H */
