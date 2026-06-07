/*
 * fatfs_ro.c — in-core read-only FatFs, exposed as a vfs_driver_t named "FAT".
 *
 * Mirrors the structure of lfs_wrapper.c (the in-core RO LittleFS): small pools
 * of DIR/FIL objects, a vfs_driver_t with read-side callbacks only (write/unlink
 * are NULL → the file browser shows MODE = RO and offers no paste/delete). The
 * backend (memory-mapped flash vs SD card) is selected by the partition base we
 * stash in fat_partition_base_addr before f_mount; the routing lives in
 * fatfs_diskio.c. The full RW + exFAT path is the fatfs_rw PIE module (Phase 5).
 *
 * Built with FF_FS_READONLY=1 / FF_FS_EXFAT=0 from the staged FatFs sources, so
 * f_getfree() is compiled out — statfs reports UNKNOWN (free needs a FAT scan).
 */

#include "ff.h"
#include "vfs.h"
#include "sdcard.h"
#include <string.h>

extern uint32_t fat_partition_base_addr;  /* defined in fatfs_diskio.c */

static FATFS s_fs;                 /* single mounted volume (FF_VOLUMES == 1) */
static DIR   s_dirs[2];
static bool  s_dirs_used[2];
static FIL   s_files[2];
static bool  s_files_used[2];

static int fat_vfs_mount(uint32_t base_addr, uint32_t size) {
    (void)size;
    fat_partition_base_addr = base_addr;          /* tells the diskio which backend */
    return (f_mount(&s_fs, "", 1) == FR_OK) ? 0 : -1;   /* opt=1: mount immediately */
}

static int fat_vfs_unmount(void) {
    f_mount(NULL, "", 0);
    return 0;
}

static int fat_vfs_opendir(const char *path, void **dir_ctx) {
    int idx = -1;
    for (int i = 0; i < 2; i++) if (!s_dirs_used[i]) { idx = i; break; }
    if (idx < 0) return -1;
    s_dirs_used[idx] = true;
    if (f_opendir(&s_dirs[idx], (path && path[0]) ? path : "/") != FR_OK) {
        s_dirs_used[idx] = false;
        return -1;
    }
    *dir_ctx = &s_dirs[idx];
    return 0;
}

static int fat_vfs_readdir(void *dir_ctx, vfs_dirent_t *ent) {
    FILINFO fno;
    if (f_readdir((DIR *)dir_ctx, &fno) != FR_OK) return -1;
    if (fno.fname[0] == 0) return 0;   /* end of directory */
    strncpy(ent->name, fno.fname, sizeof(ent->name) - 1);
    ent->name[sizeof(ent->name) - 1] = '\0';
    ent->type = (fno.fattrib & AM_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = (uint32_t)fno.fsize;
    return 1;
}

static int fat_vfs_closedir(void *dir_ctx) {
    DIR *d = (DIR *)dir_ctx;
    f_closedir(d);
    for (int i = 0; i < 2; i++) if (&s_dirs[i] == d) { s_dirs_used[i] = false; break; }
    return 0;
}

static int fat_vfs_open(const char *path, int mode, void **file_ctx) {
    if (mode & 2) return -1;   /* read-only filesystem */
    int idx = -1;
    for (int i = 0; i < 2; i++) if (!s_files_used[i]) { idx = i; break; }
    if (idx < 0) return -1;
    s_files_used[idx] = true;
    if (f_open(&s_files[idx], path, FA_READ) != FR_OK) {
        s_files_used[idx] = false;
        return -1;
    }
    *file_ctx = &s_files[idx];
    return 0;
}

static int fat_vfs_close(void *file_ctx) {
    FIL *f = (FIL *)file_ctx;
    f_close(f);
    for (int i = 0; i < 2; i++) if (&s_files[i] == f) { s_files_used[i] = false; break; }
    return 0;
}

static int fat_vfs_read(void *file_ctx, void *buf, size_t len, size_t *read_len) {
    UINT br = 0;
    if (f_read((FIL *)file_ctx, buf, (UINT)len, &br) != FR_OK) return -1;
    *read_len = br;
    return 0;
}

static int fat_vfs_statfs(uint32_t *total_bytes, uint32_t *free_bytes) {
    (void)total_bytes; (void)free_bytes;
    return -1;   /* RO build: f_getfree() compiled out → free space unknown */
}

static vfs_driver_t fat_vfs_driver = {
    .name     = "FAT",
    .mount    = fat_vfs_mount,
    .unmount  = fat_vfs_unmount,
    .opendir  = fat_vfs_opendir,
    .readdir  = fat_vfs_readdir,
    .closedir = fat_vfs_closedir,
    .open     = fat_vfs_open,
    .close    = fat_vfs_close,
    .read     = fat_vfs_read,
    .write    = NULL,   /* RO */
    .unlink   = NULL,   /* RO */
    .statfs   = fat_vfs_statfs,
};

void fat_vfs_init(void) {
    vfs_register_driver(&fat_vfs_driver);
}

/* Total data-area size of the currently-mounted volume, in 512-byte SECTORS.
 * RO-safe (does not call f_getfree, which is compiled out): data clusters =
 * n_fatent - 2, each csize sectors. Returned in sectors (not bytes) so it fits a
 * uint32_t for cards > 4 GB — an 8 GB volume is ~15.6M sectors but ~8e9 bytes,
 * which would overflow. Valid only while a volume is mounted. */
uint32_t fat_ro_total_sectors(void) {
    if (s_fs.fs_type == 0) return 0;   /* not mounted */
    return (uint32_t)(s_fs.n_fatent - 2) * s_fs.csize;
}
