#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdbool.h>
#include "ui/strings.h"   /* string_id_t for the translatable detail label */

typedef struct {
    uint32_t address;
    uint32_t size;
    const char* type;        /* stable English classification key (NOT translated in place) */
    string_id_t detail_id;   /* translatable detail label, resolved at draw time via tr() */
    uint32_t detail_num;     /* count for "%d" detail labels (Blocks/Entries); 0 otherwise */
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
 * partition_modules_ready — true once the boot scan has registered the module
 * sources (SD + the LittleFS at its known offset), so the theme module can load.
 * Set by the front-loaded probe well before the full sweep completes; also set at
 * STATE_COMPLETE as a fallback if the fast LittleFS probe missed.
 */
bool partition_modules_ready(void);

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
 * Returns true once Bank 2 holds the verified OFW; false if verification of the
 * source image/asset blob failed (Bank 2 left untouched) or a flash write failed.
 * Callers MUST NOT boot Bank 2 on a false return.
 */
bool partition_flash_ofw(const char *name, uint32_t spi_offset, uint32_t size);

int partition_get_count(void);
partition_info_t* partition_get_info(int index);

/* Re-probe only the SD card and reconcile its synthetic partition (the full
 * flash scan still happens at boot). Called on File Browser / Partition Viewer
 * entry so a card inserted/removed/swapped after boot is noticed. Returns true
 * if the partition set changed. */
bool partition_redetect_sd(void);

/* --- Shared partition classification (used by the loader + both UIs) --- */
/* True if this is the synthetic SD-card partition (base == SDCARD_SENTINEL_ADDR). */
bool partition_is_sd(const partition_info_t *p);
/* Short display code for the filesystem type: "LFS" / "FAT" / "FROG", or NULL. */
const char *partition_fs_code(const partition_info_t *p);
/* Registered vfs_driver name for the type: "LFS" / "FAT" / "FROGFS", or NULL. */
const char *partition_driver_name(const partition_info_t *p);
/* Scan a Retro-Go internal-flash image for its version banner ("Retro-Go SD v...").
 * Returns a pointer to the NUL-terminated version token ("v1.3.1-84-g905d6615")
 * inside the memory-mapped image, or NULL if absent. Version-proof identity marker:
 * the banner literal moves with every build, so we scan for it rather than test a
 * fixed offset (the old 0x400 "01 00 00 00" probe was generic and went stale). */
const char *partition_retrogo_version(uint32_t addr, uint32_t size);
void update_progress_ui(int pct, const char *title, const char *status);
extern uint32_t    total_ext_flash_size;
extern string_id_t partition_current_phase;  /* updated during scan; tr()'d on the progress screen */

#endif // PARTITION_H
