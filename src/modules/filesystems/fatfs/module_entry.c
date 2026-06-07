#include "storage/vfs.h"
#include "storage/sdcard.h"   /* SDCARD_SENTINEL_ADDR */
#include "system/module.h"
#include "ff.h"
#include "diskio.h"

MODULE_HEADER;

const host_api_t *g_host = NULL;
uint32_t fat_partition_base_addr = 0;
uint32_t fat_partition_size = 0;

static uint8_t g_cache_buf[4096] __attribute__((aligned(4)));
static uint32_t g_cache_addr = 0xFFFFFFFF;
static bool g_cache_dirty = false;

static void flush_cache(void) {
    if (g_cache_dirty && g_cache_addr != 0xFFFFFFFF && g_host) {
        g_host->flash_erase(g_cache_addr, 4096);
        for (uint32_t offset = 0; offset < 4096; offset += 256) {
            g_host->flash_write(g_cache_addr + offset, g_cache_buf + offset, 256);
        }
        g_cache_dirty = false;
    }
}

static void load_cache(uint32_t addr) {
    if (g_cache_addr != addr) {
        flush_cache();
        g_cache_addr = addr;
        if (g_host) {
            g_host->flash_read(g_cache_addr, g_cache_buf, 4096);
        }
        g_cache_dirty = false;
    }
}

DSTATUS disk_initialize(BYTE pdrv) { return 0; }
DSTATUS disk_status(BYTE pdrv) { return 0; }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_host) return RES_PARERR;
    /* SD-backed volume: sector commands through the in-core transport. */
    if (fat_partition_base_addr == SDCARD_SENTINEL_ADDR) {
        if (!g_host->sd_read) return RES_PARERR;
        return (g_host->sd_read((uint32_t)sector, buff, count) == 0) ? RES_OK : RES_ERROR;
    }
    uint32_t start_addr = fat_partition_base_addr + sector * 512;
    for (UINT i = 0; i < count; i++) {
        uint32_t addr = start_addr + i * 512;
        uint32_t block_addr = addr & ~4095;
        uint32_t block_offset = addr & 4095;
        if (g_cache_addr == block_addr) {
            g_host->memcpy(buff + i * 512, g_cache_buf + block_offset, 512);
        } else {
            g_host->flash_read(addr, buff + i * 512, 512);
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_host) return RES_PARERR;
    /* SD-backed volume: write sectors straight to the card (no flash erase/cache). */
    if (fat_partition_base_addr == SDCARD_SENTINEL_ADDR) {
        if (!g_host->sd_write) return RES_PARERR;
        return (g_host->sd_write((uint32_t)sector, buff, count) == 0) ? RES_OK : RES_ERROR;
    }
    uint32_t start_addr = fat_partition_base_addr + sector * 512;
    for (UINT i = 0; i < count; i++) {
        uint32_t addr = start_addr + i * 512;
        uint32_t block_addr = addr & ~4095;
        uint32_t block_offset = addr & 4095;
        load_cache(block_addr);
        g_host->memcpy(g_cache_buf + block_offset, buff + i * 512, 512);
        g_cache_dirty = true;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    if (cmd == CTRL_SYNC) {
        flush_cache();
        return RES_OK;
    }
    return RES_PARERR;
}

DWORD get_fattime(void) {
    if (g_host) return g_host->get_fattime();
    return 0;
}

static FATFS g_fat_fs;
static DIR g_fat_dirs[2];
static bool g_fat_dirs_used[2];
static FIL g_fat_files[2];
static bool g_fat_files_used[2];

static int fat_vfs_mount(uint32_t base_addr, uint32_t size) {
    fat_partition_base_addr = base_addr;
    fat_partition_size = size;
    g_cache_addr = 0xFFFFFFFF;
    g_cache_dirty = false;
    return f_mount(&g_fat_fs, "0:", 1) == FR_OK ? 0 : -1;
}

static int fat_vfs_unmount(void) {
    flush_cache();
    return f_mount(NULL, "0:", 1) == FR_OK ? 0 : -1;
}

static int fat_vfs_opendir(const char *path, void **dir_ctx) {
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!g_fat_dirs_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    if (f_opendir(&g_fat_dirs[idx], path) != FR_OK) return -1;
    g_fat_dirs_used[idx] = true;
    *dir_ctx = &g_fat_dirs[idx];
    return 0;
}

static int fat_vfs_readdir(void *dir_ctx, vfs_dirent_t *ent) {
    FILINFO fno;
    if (f_readdir((DIR *)dir_ctx, &fno) != FR_OK || fno.fname[0] == 0) return 0;
    g_host->strncpy(ent->name, fno.fname, sizeof(ent->name) - 1);
    ent->name[sizeof(ent->name) - 1] = '\0';
    ent->type = (fno.fattrib & AM_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = fno.fsize;
    return 1;
}

static int fat_vfs_closedir(void *dir_ctx) {
    DIR *dir = (DIR *)dir_ctx;
    f_closedir(dir);
    for (int i = 0; i < 2; i++) {
        if (&g_fat_dirs[i] == dir) {
            g_fat_dirs_used[i] = false;
            break;
        }
    }
    return 0;
}

static int fat_vfs_open(const char *path, int mode, void **file_ctx) {
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!g_fat_files_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    
    BYTE fat_mode = 0;
    if (mode & 1) fat_mode |= FA_READ;
    if (mode & 2) fat_mode |= FA_WRITE | FA_CREATE_ALWAYS;
    
    if (f_open(&g_fat_files[idx], path, fat_mode) != FR_OK) return -1;
    g_fat_files_used[idx] = true;
    *file_ctx = &g_fat_files[idx];
    return 0;
}

static int fat_vfs_close(void *file_ctx) {
    FIL *file = (FIL *)file_ctx;
    f_close(file);
    for (int i = 0; i < 2; i++) {
        if (&g_fat_files[i] == file) {
            g_fat_files_used[i] = false;
            break;
        }
    }
    return 0;
}

static int fat_vfs_read(void *file_ctx, void *buf, size_t len, size_t *read_len) {
    UINT r;
    if (f_read((FIL *)file_ctx, buf, len, &r) != FR_OK) return -1;
    *read_len = r;
    return 0;
}

static int fat_vfs_write(void *file_ctx, const void *buf, size_t len, size_t *written_len) {
    UINT w;
    if (f_write((FIL *)file_ctx, buf, len, &w) != FR_OK) return -1;
    *written_len = w;
    return 0;
}

static int fat_vfs_unlink(const char *path) {
    return f_unlink(path) == FR_OK ? 0 : -1;
}

static int fat_vfs_mkdir(const char *path) {
    return f_mkdir(path) == FR_OK ? 0 : -1;
}

static int fat_vfs_statfs(uint32_t *total_sectors, uint32_t *free_sectors) {
    FATFS *fs;
    DWORD free_clust;
    if (f_getfree("0:", &free_clust, &fs) != FR_OK) return -1;
    /* Report sectors (csize is sectors/cluster); multiplying by 512 here would
     * overflow uint32_t on a multi-GB card. */
    *total_sectors = (fs->n_fatent - 2) * fs->csize;
    *free_sectors  = free_clust * fs->csize;
    return 0;
}

__attribute__((section(".text.init_module"))) void init_module(vfs_driver_t *drv, const host_api_t *host) {
    g_host = host;
    
    drv->name = "FAT";
    drv->mount = fat_vfs_mount;
    drv->unmount = fat_vfs_unmount;
    drv->opendir = fat_vfs_opendir;
    drv->readdir = fat_vfs_readdir;
    drv->closedir = fat_vfs_closedir;
    drv->open = fat_vfs_open;
    drv->close = fat_vfs_close;
    drv->read = fat_vfs_read;
    drv->write = fat_vfs_write;
    drv->unlink = fat_vfs_unlink;
    drv->mkdir = fat_vfs_mkdir;
    drv->statfs = fat_vfs_statfs;
}
