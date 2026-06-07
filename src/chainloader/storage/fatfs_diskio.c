/*
 * fatfs_diskio.c — Minimal disk I/O interface for FatFs
 *
 * Currently stubbed out. This will eventually bridge to the SDMMC or SPI
 * hardware for the SD card mod. Since the chainloader only mounts FatFs
 * as read-only, write operations are not required.
 */

#include "ff.h"
#include "diskio.h"
#include "board.h"
#include <string.h>

uint32_t fat_partition_base_addr = 0;

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv == 0) {
        return 0;
    }
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv == 0) {
        return 0;
    }
    return STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    if (pdrv == 0) {
        memcpy(buff, (const void *)(fat_partition_base_addr + sector * 512), count * 512);
        return RES_OK;
    }
    return RES_PARERR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    return RES_WRPRT;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    return RES_PARERR;
}

DWORD get_fattime(void) {
    return board_rtc_get_fattime();
}
