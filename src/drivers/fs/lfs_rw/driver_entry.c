#include "storage/vfs.h"
#include "lfs.h"
#include <stdint.h>

const host_api_t *g_host = NULL;

#define LFS_CACHE_SIZE      256
#define LFS_LOOKAHEAD_SIZE   16

static uint8_t read_buffer    [LFS_CACHE_SIZE];
static uint8_t prog_buffer    [LFS_CACHE_SIZE];
static uint8_t lookahead_buffer[LFS_LOOKAHEAD_SIZE] __attribute__((aligned(4)));

static inline uint32_t phys_addr(const struct lfs_config *c,
                                  lfs_block_t block, lfs_off_t off)
{
    return (uint32_t)c->context - ((block + 1) * c->block_size) + off;
}

static int lfs_flash_read(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, void *buffer, lfs_size_t size)
{
    if (!g_host) return LFS_ERR_IO;
    g_host->memcpy(buffer, (const void *)phys_addr(c, block, off), size);
    return LFS_ERR_OK;
}

static int lfs_flash_prog(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, const void *buffer, lfs_size_t size)
{
    if (!g_host) return LFS_ERR_IO;
    uint32_t addr     = phys_addr(c, block, off);
    uint32_t ospi_off = addr - 0x90000000UL;
    g_host->flash_write(ospi_off, buffer, size);
    return LFS_ERR_OK;
}

static int lfs_flash_erase(const struct lfs_config *c, lfs_block_t block)
{
    if (!g_host) return LFS_ERR_IO;
    uint32_t addr     = phys_addr(c, block, 0);
    uint32_t ospi_off = addr - 0x90000000UL;
    g_host->flash_erase(ospi_off, c->block_size);
    return LFS_ERR_OK;
}

static int lfs_flash_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

lfs_t lfs;
static struct lfs_config flash_cfg;

static lfs_dir_t lfs_vfs_dirs[2];
static bool lfs_vfs_dirs_used[2];

static lfs_file_t lfs_vfs_files[2];
static bool lfs_vfs_files_used[2];
static uint8_t lfs_vfs_file_buffers[2][256];
static struct lfs_file_config lfs_vfs_file_cfgs[2];

static int lfs_vfs_mount(uint32_t base_addr, uint32_t size) {
    flash_cfg.read        = lfs_flash_read;
    flash_cfg.prog        = lfs_flash_prog;
    flash_cfg.erase       = lfs_flash_erase;
    flash_cfg.sync        = lfs_flash_sync;

    flash_cfg.read_size      = LFS_CACHE_SIZE;
    flash_cfg.prog_size      = LFS_CACHE_SIZE;
    flash_cfg.block_size     = 4096;
    flash_cfg.block_count    = size / 4096;
    flash_cfg.cache_size     = LFS_CACHE_SIZE;
    flash_cfg.lookahead_size = LFS_LOOKAHEAD_SIZE;
    flash_cfg.block_cycles   = 500;

    flash_cfg.read_buffer      = read_buffer;
    flash_cfg.prog_buffer      = prog_buffer;
    flash_cfg.lookahead_buffer = lookahead_buffer;
    flash_cfg.context     = (void *)(base_addr + size);

    return lfs_mount(&lfs, &flash_cfg);
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
    
    g_host->strncpy(ent->name, info.name, sizeof(ent->name) - 1);
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
    
    int lfs_mode = 0;
    if (mode & 1) lfs_mode |= LFS_O_RDONLY;
    if (mode & 2) lfs_mode |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
    
    g_host->memset(&lfs_vfs_file_cfgs[idx], 0, sizeof(struct lfs_file_config));
    lfs_vfs_file_cfgs[idx].buffer = lfs_vfs_file_buffers[idx];
    
    lfs_vfs_files_used[idx] = true;
    int res = lfs_file_opencfg(&lfs, &lfs_vfs_files[idx], path, lfs_mode, &lfs_vfs_file_cfgs[idx]);
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
    lfs_ssize_t res = lfs_file_write(&lfs, (lfs_file_t *)file_ctx, buf, len);
    if (res < 0) return res;
    *written_len = res;
    return 0;
}

static int lfs_vfs_unlink(const char *path) {
    return lfs_remove(&lfs, path);
}

static int lfs_vfs_statfs(uint32_t *total_bytes, uint32_t *free_bytes) {
    lfs_ssize_t used = lfs_fs_size(&lfs);
    if (used < 0) return -1;
    *total_bytes = flash_cfg.block_count * flash_cfg.block_size;
    *free_bytes = (flash_cfg.block_count - used) * flash_cfg.block_size;
    return 0;
}

__attribute__((section(".text.init_driver"))) void init_driver(vfs_driver_t *drv, const host_api_t *host) {
    g_host = host;
    
    drv->name = "LFS";
    drv->mount = lfs_vfs_mount;
    drv->unmount = lfs_vfs_unmount;
    drv->opendir = lfs_vfs_opendir;
    drv->readdir = lfs_vfs_readdir;
    drv->closedir = lfs_vfs_closedir;
    drv->open = lfs_vfs_open;
    drv->close = lfs_vfs_close;
    drv->read = lfs_vfs_read;
    drv->write = lfs_vfs_write;
    drv->unlink = lfs_vfs_unlink;
    drv->statfs = lfs_vfs_statfs;
}
