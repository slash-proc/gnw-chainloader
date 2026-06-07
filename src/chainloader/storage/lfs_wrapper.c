/*
 * lfs_wrapper.c — LittleFS block-device backend for the GNW chainloader.
 *
 * Uses the upstream littlefs submodule (deps/littlefs).
 *
 * Layout: the LittleFS partition is stored "inverted" — block 0 of the
 * filesystem maps to the *last* 4 KiB erase sector of the physical region,
 * and block N-1 maps to the *first* sector.  This matches the layout written
 * by Retro-Go / gnwmanager and is the same convention used in gw_littlefs.c.
 *
 * Addressing:
 *   physical address = (context_end) - (block + 1) * block_size + offset
 * where context_end = base + total_size  (one-past-the-end byte address).
 *
 * OSPI operations require temporarily leaving memory-mapped mode.
 * DCache is cleaned and invalidated around every write/erase to prevent
 * stale cache lines from being flushed over fresh data.
 */

#include "lfs.h"
#include "flash.h"        /* OSPI_DisableMemoryMappedMode / Enable, Program, EraseSync */
#include "board.h"        /* board_ospi_get_size, etc. */
#include <string.h>
#include <stdint.h>
#include "stm32h7xx.h"    /* SCB_CleanDCache_by_Addr, SCB_InvalidateDCache_by_Addr, SCB->CCR */

/* -------------------------------------------------------------------------
 * Buffer sizes
 * 256 bytes matches the OSPI NOR page-program granularity and avoids
 * alignment asserts inside the LittleFS write path.
 * ------------------------------------------------------------------------- */
#define LFS_CACHE_SIZE      256
#define LFS_LOOKAHEAD_SIZE   16

static uint8_t read_buffer    [LFS_CACHE_SIZE];
static uint8_t prog_buffer    [LFS_CACHE_SIZE];
static uint8_t lookahead_buffer[LFS_LOOKAHEAD_SIZE] __attribute__((aligned(4)));

/* -------------------------------------------------------------------------
 * DCache helpers
 * ------------------------------------------------------------------------- */
static inline int dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0;
}

static inline void dcache_before_write(uint32_t addr, uint32_t size) {
    if (dcache_enabled()) {
        SCB_CleanDCache_by_Addr((uint32_t *)addr, size);
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size);
        SCB_DisableDCache();
    }
}

static inline void dcache_after_write(uint32_t addr, uint32_t size) {
    if (dcache_enabled()) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size);
        SCB_EnableDCache();
    }
}

/* -------------------------------------------------------------------------
 * Block device callbacks
 * ------------------------------------------------------------------------- */

/*
 * Map an LFS (block, offset) pair to a physical OSPI memory-mapped address.
 * c->context points one byte past the end of the partition (= base + size).
 */
static inline uint32_t phys_addr(const struct lfs_config *c,
                                  lfs_block_t block, lfs_off_t off)
{
    return (uint32_t)c->context - ((block + 1) * c->block_size) + off;
}

static int lfs_flash_read(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, void *buffer, lfs_size_t size)
{
    memcpy(buffer, (const void *)phys_addr(c, block, off), size);
    return LFS_ERR_OK;
}

#ifndef LFS_READONLY
static int lfs_flash_prog(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t addr     = phys_addr(c, block, off);
    uint32_t ospi_off = addr - 0x90000000UL;   /* convert mapped → raw offset */

    dcache_before_write(addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_Program(ospi_off, buffer, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(addr, size);

    return LFS_ERR_OK;
}

static int lfs_flash_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t addr     = phys_addr(c, block, 0);
    uint32_t ospi_off = addr - 0x90000000UL;
    uint32_t sz       = c->block_size;

    dcache_before_write(addr, sz);

    OSPI_DisableMemoryMappedMode();
    OSPI_EraseSync(ospi_off, sz);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(addr, sz);

    return LFS_ERR_OK;
}
#endif

static int lfs_flash_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

/* -------------------------------------------------------------------------
 * Global LittleFS state
 * ------------------------------------------------------------------------- */
lfs_t lfs;

static struct lfs_config flash_cfg = {
    .read        = lfs_flash_read,
#ifndef LFS_READONLY
    .prog        = lfs_flash_prog,
    .erase       = lfs_flash_erase,
#else
    .prog        = NULL,
    .erase       = NULL,
#endif
    .sync        = lfs_flash_sync,

    .read_size      = LFS_CACHE_SIZE,
    .prog_size      = LFS_CACHE_SIZE,
    .block_size     = 4096,          /* overridden by lfs_mount_at() */
    .block_count    = 0,             /* overridden by lfs_mount_at() */
    .cache_size     = LFS_CACHE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE,
    .block_cycles   = 500,

    .read_buffer      = read_buffer,
    .prog_buffer      = prog_buffer,
    .lookahead_buffer = lookahead_buffer,

    /* context = one byte past the end of the partition.
     * Set by lfs_mount_at() at runtime. */
    .context = NULL,
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * lfs_mount_at — mount the LittleFS partition whose physical address range is
 * [base_addr, base_addr + block_count * 4096).
 *
 * block_count comes from the partition scanner (superblock metadata).
 * Returns 0 on success, negative LFS error code on failure.
 */
int lfs_mount_at(uint32_t base_addr, uint32_t block_count)
{
    uint32_t erase_size = OSPI_GetSmallestEraseSize();
    if (erase_size == 0) erase_size = 4096;  /* safe fallback */

    flash_cfg.block_size  = erase_size;
    flash_cfg.block_count = block_count;
    /* context = one-past-end of the physical region */
    flash_cfg.context     = (void *)(base_addr + block_count * erase_size);

    return lfs_mount(&lfs, &flash_cfg);
}

#include "vfs.h"

static lfs_dir_t lfs_vfs_dirs[2];
static bool lfs_vfs_dirs_used[2];

static lfs_file_t lfs_vfs_files[2];
static bool lfs_vfs_files_used[2];
static uint8_t lfs_vfs_file_buffers[2][256];
static struct lfs_file_config lfs_vfs_file_cfgs[2];

static int lfs_vfs_mount(uint32_t base_addr, uint32_t size) {
    return lfs_mount_at(base_addr, size / 4096);
}

static int lfs_vfs_unmount(void) {
    return lfs_unmount(&lfs);
}

static int lfs_vfs_opendir(const char *path, void **dir_ctx) {
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!lfs_vfs_dirs_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return LFS_ERR_NOMEM;
    
    lfs_vfs_dirs_used[idx] = true;
    int res = lfs_dir_open(&lfs, &lfs_vfs_dirs[idx], path);
    if (res < 0) {
        lfs_vfs_dirs_used[idx] = false;
        return res;
    }
    *dir_ctx = &lfs_vfs_dirs[idx];
    return 0;
}

static int lfs_vfs_readdir(void *dir_ctx, vfs_dirent_t *ent) {
    struct lfs_info info;
    int res = lfs_dir_read(&lfs, (lfs_dir_t *)dir_ctx, &info);
    if (res <= 0) return res;
    
    strncpy(ent->name, info.name, sizeof(ent->name) - 1);
    ent->name[sizeof(ent->name) - 1] = '\0';
    ent->type = (info.type == LFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = info.size;
    return 1;
}

static int lfs_vfs_closedir(void *dir_ctx) {
    lfs_dir_t *dir = (lfs_dir_t *)dir_ctx;
    int res = lfs_dir_close(&lfs, dir);
    for (int i = 0; i < 2; i++) {
        if (&lfs_vfs_dirs[i] == dir) {
            lfs_vfs_dirs_used[i] = false;
            break;
        }
    }
    return res;
}

static int lfs_vfs_open(const char *path, int mode, void **file_ctx) {
    int idx = -1;
    for (int i = 0; i < 2; i++) {
        if (!lfs_vfs_files_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return LFS_ERR_NOMEM;
    
    if (mode & 2) return LFS_ERR_INVAL; // Read-only file system
    
    memset(&lfs_vfs_file_cfgs[idx], 0, sizeof(struct lfs_file_config));
    lfs_vfs_file_cfgs[idx].buffer = lfs_vfs_file_buffers[idx];
    
    lfs_vfs_files_used[idx] = true;
    int res = lfs_file_opencfg(&lfs, &lfs_vfs_files[idx], path, LFS_O_RDONLY, &lfs_vfs_file_cfgs[idx]);
    if (res < 0) {
        lfs_vfs_files_used[idx] = false;
        return res;
    }
    *file_ctx = &lfs_vfs_files[idx];
    return 0;
}

static int lfs_vfs_close(void *file_ctx) {
    lfs_file_t *file = (lfs_file_t *)file_ctx;
    int res = lfs_file_close(&lfs, file);
    for (int i = 0; i < 2; i++) {
        if (&lfs_vfs_files[i] == file) {
            lfs_vfs_files_used[i] = false;
            break;
        }
    }
    return res;
}

static int lfs_vfs_read(void *file_ctx, void *buf, size_t len, size_t *read_len) {
    lfs_ssize_t res = lfs_file_read(&lfs, (lfs_file_t *)file_ctx, buf, len);
    if (res < 0) return res;
    *read_len = res;
    return 0;
}

static int lfs_vfs_write(void *file_ctx, const void *buf, size_t len, size_t *written_len) {
    return LFS_ERR_INVAL;
}

static int lfs_vfs_unlink(const char *path) {
    return LFS_ERR_INVAL;
}

static int lfs_vfs_statfs(uint32_t *total_sectors, uint32_t *free_sectors) {
    lfs_ssize_t used = lfs_fs_size(&lfs);
    if (used < 0) return -1;
    uint32_t spb = flash_cfg.block_size / 512;   /* 512-byte sectors per block */
    *total_sectors = (uint32_t)flash_cfg.block_count * spb;
    *free_sectors  = (uint32_t)(flash_cfg.block_count - used) * spb;
    return 0;
}

static vfs_driver_t lfs_vfs_driver = {
    .name = "LFS",
    .mount = lfs_vfs_mount,
    .unmount = lfs_vfs_unmount,
    .opendir = lfs_vfs_opendir,
    .readdir = lfs_vfs_readdir,
    .closedir = lfs_vfs_closedir,
    .open = lfs_vfs_open,
    .close = lfs_vfs_close,
    .read = lfs_vfs_read,
    .write = lfs_vfs_write,
    .unlink = lfs_vfs_unlink,
    .statfs = lfs_vfs_statfs
};

void lfs_vfs_init(void) {
    vfs_register_driver(&lfs_vfs_driver);
}
