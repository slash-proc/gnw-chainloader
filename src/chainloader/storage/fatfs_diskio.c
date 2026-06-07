/*
 * fatfs_diskio.c — in-core FatFs disk I/O (read-only).
 *
 * One diskio backs the in-core RO FatFs for BOTH backends, routed by the
 * mounted partition's base address (set by fat_vfs_mount before f_mount):
 *   - SDCARD_SENTINEL_ADDR -> the SD card, via sdcard_read() (sector commands)
 *   - any other address    -> memory-mapped flash, via memcpy from base+LBA*512
 *
 * The chainloader mounts FatFs read-only, so disk_write is compiled out under
 * FF_FS_READONLY (the RW path is the fatfs_rw PIE module, Phase 5).
 */

#include "ff.h"
#include "diskio.h"
#include "board.h"
#include "sdcard.h"
#include <string.h>

uint32_t fat_partition_base_addr = 0;

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (fat_partition_base_addr == SDCARD_SENTINEL_ADDR)
        return sdcard_present() ? 0 : STA_NOINIT;
    return 0; /* memory-mapped flash is always ready */
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (fat_partition_base_addr == SDCARD_SENTINEL_ADDR)
        return sdcard_present() ? 0 : STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv != 0) return RES_PARERR;
    if (fat_partition_base_addr == SDCARD_SENTINEL_ADDR)
        return (sdcard_read((uint32_t)sector, buff, count) == 0) ? RES_OK : RES_ERROR;
    memcpy(buff, (const void *)(fat_partition_base_addr + (uint32_t)sector * 512u),
           (uint32_t)count * 512u);
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    (void)pdrv; (void)buff; (void)sector; (void)count;
    return RES_WRPRT;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    (void)pdrv; (void)buff;
    if (cmd == CTRL_SYNC) return RES_OK;   /* nothing to flush in RO */
    return RES_PARERR;
}

DWORD get_fattime(void) {
    return board_rtc_get_fattime();
}
