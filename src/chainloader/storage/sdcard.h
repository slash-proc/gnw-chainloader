#ifndef SDCARD_H
#define SDCARD_H

/*
 * In-core SD-card block device.
 *
 * Faithful port of the proven, tested-on-this-device SD support in
 * `example projects/game-and-watch-bootloader/` (Core/Src/gw_sdcard.c +
 * porting/lib/FatFs/user_diskio_{spi,softspi}.c + softspi.c). That reference
 * supports two physical SD mods and auto-detects between them; we replicate
 * both:
 *
 *   - SDCARD_HW_SPI1  : "Tim" mod, dedicated pins — PB3 SCK / PD7 MOSI /
 *                       PB4 MISO (SPI1 AF5) + PB9 CS + PA15 VCC-enable.
 *                       No conflict with the external OSPI flash.
 *   - SDCARD_HW_OSPI1 : "Yota9" mod, bit-banged SoftSPI over the SHARED OSPI
 *                       flash pins — PB2 CLK / PB1 MOSI / PD12 MISO / PE11 CS.
 *                       Mutually exclusive with memory-mapped flash, so every
 *                       op brackets board_ospi_suspend()/board_ospi_resume().
 *
 * This is the foundation for SD-card support (Goal 13): the in-core RO FAT/LFS
 * readers and the RW PIE module all reach the card through these sector calls
 * (the RW module via the host_api sd_read/sd_write callbacks).
 *
 * STABILITY IS LAW: detection deinits OSPI and toggles power, so it must run
 * OFF the boot-critical path (after the launcher is reachable) and always
 * restores OSPI memory-mapped mode before returning — the menu/theme read the
 * external flash and must keep working whether or not a card is present.
 */

#include <stdint.h>
#include <stdbool.h>

/* Synthetic partition base used to mark an SD-backed volume. The SD card is not
 * memory-mapped, so the in-core FatFs diskio treats a partition at this address
 * as "route sector I/O to sdcard_read/write()" instead of a memory read. Chosen
 * outside every real region (internal flash 0x08…, external flash 0x90…). */
#define SDCARD_SENTINEL_ADDR 0xC0000000UL

typedef enum {
    SDCARD_HW_UNDETECTED = 0, /* no probe done yet */
    SDCARD_HW_NONE,           /* probed; no card responded on either bus */
    SDCARD_HW_SPI1,           /* dedicated-pin SPI1 (Tim) */
    SDCARD_HW_OSPI1,          /* soft-SPI over the OSPI flash pins (Yota9) */
} sdcard_hw_t;

/*
 * Probe SPI1 first, then SoftSPI-over-OSPI, running the SD init handshake
 * (CMD0 / CMD8 / ACMD41 / CMD58-CCS) on each. Records which bus a card answered
 * on. Idempotent-ish: re-probes each call. Always leaves OSPI memory-mapped
 * mode restored. Returns true if a card initialized.
 */
bool sdcard_detect(void);

sdcard_hw_t sdcard_hw(void);  /* which bus the card is on (or NONE/UNDETECTED) */
bool sdcard_present(void);    /* true once a card has initialized */

/*
 * 512-byte logical-sector I/O. Dispatches on the detected bus; the SoftSPI bus
 * is bracketed internally (acquire pins -> transfer -> restore OSPI). `count`
 * is the number of consecutive sectors. Returns 0 on success, non-zero on error
 * (including no card / wrong bus). Block vs byte addressing (SDSC/SDHC) handled
 * internally.
 */
int sdcard_read(uint32_t sector, uint8_t *buf, uint32_t count);
int sdcard_write(uint32_t sector, const uint8_t *buf, uint32_t count);

#endif /* SDCARD_H */
