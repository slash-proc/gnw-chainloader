#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t address;
    uint32_t size;
    const char* type;
    char details[32];
} partition_info_t;

/*
 * Progress callback invoked once per probe address during the scan.
 *   done  — probe steps completed so far (1-based at the point of call).
 *   total — total probe steps pre-computed before scanning begins; always > 0.
 * Return true to continue scanning; return false to abort early.
 * May be NULL (no progress reporting, scan always runs to completion).
 */
typedef enum {
    PARTITION_SCAN_IDLE,
    PARTITION_SCAN_IN_PROGRESS,
    PARTITION_SCAN_COMPLETE,
    PARTITION_SCAN_ABORTED
} partition_scan_state_t;

/*
 * partition_scan_start — begin a new full scan.
 */
void partition_scan_start(void);

/*
 * partition_scan_update — perform one or more steps of the scan.
 * Intended to be called in the main loop or background tasks.
 */
void partition_scan_update(void);

/*
 * partition_scan_get_state — returns current state of the scanner.
 */
partition_scan_state_t partition_scan_get_state(void);

/*
 * partition_scan_get_progress — returns current progress (done/total).
 */
void partition_scan_get_progress(int *done, int *total);

/* --- Partition Operations --- */

/**
 * Erases a range of internal or external flash.
 * addr: absolute address (0x08... or 0x90...)
 * size: bytes to erase (will be rounded up to sector boundaries)
 */
void partition_erase(uint32_t addr, uint32_t size);

/**
 * Flashes a firmware from external flash to internal bank 2.
 * name: display name (e.g. "Mario OFW")
 * spi_offset: source offset in external flash
 * size: bytes to write (e.g. 128KB)
 */
void partition_flash_ofw(const char *name, uint32_t spi_offset, uint32_t size);

int partition_get_count(void);
partition_info_t* partition_get_info(int index);
void update_progress_ui(int pct, const char *title, const char *status);
extern uint32_t    total_ext_flash_size;
extern const char *partition_current_phase;  /* updated during scan; display on progress screen */

#endif // PARTITION_H
