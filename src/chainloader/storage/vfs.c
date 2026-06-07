#include "vfs.h"
#include "flash.h"
#include "board.h"
#include "lfs.h"
#include "partition.h"
#include "sdcard.h"
#include "system/loader.h"
#include "system/module.h"
#include "ui/strings.h"      /* STRINGS_ABI_VERSION: the core is authoritative on pack ABI */
#include "../../common/abi.h"
#include <string.h>
#include "stm32h7xx.h"

static vfs_driver_t *g_drivers[8];
static int g_driver_count = 0;
static host_api_t g_host_api;

/* A module flash address may be a flash OFFSET (the documented host-API convention) or an absolute
 * memory-mapped address (the loaded FAT driver hands us the partition's absolute base). Normalize
 * to an offset so the 0x90000000 base is never double-added (which faulted at 0x20540000). */
static inline uint32_t flash_norm_off(uint32_t addr) {
    return (addr >= 0x90000000UL) ? (addr - 0x90000000UL) : addr;
}

static int host_flash_read(uint32_t addr, void *buf, size_t size) {
    addr = flash_norm_off(addr);
    memcpy(buf, (const void *)(0x90000000UL + addr), size);
    return 0;
}

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

static int host_flash_write(uint32_t addr, const void *buf, size_t size) {
    addr = flash_norm_off(addr);
    uint32_t mem_addr = 0x90000000UL + addr;
    dcache_before_write(mem_addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_Program(addr, buf, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(mem_addr, size);
    return 0;
}

static int host_flash_erase(uint32_t addr, uint32_t size) {
    addr = flash_norm_off(addr);
    uint32_t mem_addr = 0x90000000UL + addr;
    dcache_before_write(mem_addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_EraseSync(addr, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(mem_addr, size);
    return 0;
}

/* SD sector transport exposed to modules. The in-core sdcard layer dispatches by
 * detected bus and (for the SoftSPI/OSPI-tap mod) brackets each op with the OSPI
 * suspend/resume itself, so the module just sees plain sector I/O. */
static int host_sd_read(uint32_t sector, void *buf, uint32_t count) {
    return sdcard_read(sector, (uint8_t *)buf, count);
}

static int host_sd_write(uint32_t sector, const void *buf, uint32_t count) {
    return sdcard_write(sector, (const uint8_t *)buf, count);
}

extern void lfs_vfs_init(void);
extern void fat_vfs_init(void);

void vfs_init(void) {
    g_driver_count = 0;
    memset(g_drivers, 0, sizeof(g_drivers));

    lfs_vfs_init();
    fat_vfs_init();
    
    g_host_api.get_tick = HAL_GetTick;
    g_host_api.flash_read = host_flash_read;
    g_host_api.flash_write = host_flash_write;
    g_host_api.flash_erase = host_flash_erase;
    g_host_api.sd_read = host_sd_read;
    g_host_api.sd_write = host_sd_write;
    g_host_api.memcpy = memcpy;
    g_host_api.memset = memset;
    g_host_api.memcmp = memcmp;
    g_host_api.strlen = strlen;
    g_host_api.strcpy = strcpy;
    g_host_api.strncpy = strncpy;
    g_host_api.strcmp = strcmp;
    g_host_api.strcat = strcat;
    g_host_api.get_fattime = board_rtc_get_fattime;
}

static bool g_lfs_rw_loaded = false;
static bool g_fat_rw_loaded = false;

bool vfs_is_lfs_rw_loaded(void) {
    return g_lfs_rw_loaded;
}

bool vfs_is_fat_rw_loaded(void) {
    return g_fat_rw_loaded;
}

int vfs_register_driver(vfs_driver_t *driver) {
    if (g_driver_count >= 8) return -1;
    g_drivers[g_driver_count++] = driver;
    return 0;
}

vfs_driver_t* vfs_get_driver(const char *name) {
    for (int i = g_driver_count - 1; i >= 0; i--) {
        if (strcmp(g_drivers[i]->name, name) == 0) {
            return g_drivers[i];
        }
    }
    return NULL;
}

/*
 * Read up to `max` bytes of `path` from ONE partition into dst. Returns 0 on
 * success (sets *out_size), -1 if absent/unreadable. Uses the generic
 * mount/open/read drivers (LittleFS, FAT on flash or SD).
 */
static int read_from_partition(partition_info_t *part, const char *path,
                               void *dst, uint32_t max, uint32_t *out_size) {
    if (!part) return -1;

    const char *dname = partition_driver_name(part);
    vfs_driver_t *drv = dname ? vfs_get_driver(dname) : NULL;
    if (!drv || !drv->mount || !drv->open || !drv->read) return -1;
    if (drv->mount(part->address, part->size) != 0) return -1;

    void *fctx = NULL;
    int ok = -1;
    if (drv->open(path, 1 /* read */, &fctx) == 0) {
        uint32_t total = 0;
        int rc = 0;
        while (total < max) {
            size_t rd = 0;
            rc = drv->read(fctx, (uint8_t *)dst + total, max - total, &rd);
            if (rc != 0 || rd == 0) break;
            total += (uint32_t)rd;
        }
        if (drv->close) drv->close(fctx);
        if (total > 0) { *out_size = total; ok = 0; }
    }
    if (drv->unmount) drv->unmount();
    return ok;
}

/* Shared 4 KiB transfer buffer for the streaming file copy (vfs_copy_open_file).
 * One in-core copy loop, reused by the file browser and by modules (the installer)
 * through the host seam below, so a large file is never buffered whole. */
static uint8_t g_copy_buf[4096];

int vfs_copy_open_file(vfs_driver_t *src, const char *sp,
                       vfs_driver_t *dst, const char *dp,
                       uint32_t size_hint, vfs_copy_cb cb, void *user) {
    if (!src || !dst || !src->open || !src->read || !dst->open || !dst->write)
        return VFS_COPY_OPEN;
    void *fs = NULL, *fd = NULL;
    if (src->open(sp, 1 /* read */, &fs) != 0) return VFS_COPY_OPEN;
    /* A sized copy to a driver that offers it lands CONTIGUOUS (f_expand) for XIP; else plain create. */
    int dopen = (size_hint && dst->open_expand) ? dst->open_expand(dp, size_hint, &fd)
                                                 : dst->open(dp, 2 /* write/create */, &fd);
    if (dopen != 0) {
        if (src->close) src->close(fs);
        return VFS_COPY_OPEN;
    }
    uint32_t copied = 0;
    int res = VFS_COPY_OK;
    for (;;) {
        if (cb && cb(size_hint ? (int)(copied * 100u / size_hint) : 0, user)) {
            res = VFS_COPY_CANCEL; break;
        }
        size_t rd = 0;
        if (src->read(fs, g_copy_buf, sizeof(g_copy_buf), &rd) != 0) { res = VFS_COPY_READ; break; }
        if (rd == 0) break;                          /* EOF */
        size_t wr = 0;
        int w = dst->write(fd, g_copy_buf, rd, &wr);
        if (w != 0 || wr != rd) { res = (w == 0 && wr < rd) ? VFS_COPY_FULL : VFS_COPY_WRITE; break; }
        copied += (uint32_t)rd;
    }
    if (src->close) src->close(fs);
    if (dst->close) dst->close(fd);
    if (res != VFS_COPY_OK && dst->unlink) dst->unlink(dp);   /* drop the partial file */
    return res;
}

/*
 * Fetch a PIE module .bin by `path` for the loader (system/loader.c), choosing
 * by VERSION across all read-capable filesystems. Two-phase:
 *   1. Scan every filesystem (SD card first), peek each copy's module header,
 *      validate the GMOD magic, and remember the HIGHEST version seen.
 *   2. Read that winner's whole image into `dst`.
 * So a newer module on the SD card overrides an older one baked into the
 * internal LittleFS; equal versions resolve to the SD-first copy. Returns 0 on
 * success. (sort_partitions orders by address, so the SD-first preference is
 * made explicit with a two-pass scan.)
 */
/* Scan every read-capable partition (SD first, two-pass) for a valid PIE module at
 * `path` and return the index of the HIGHEST-version copy, or -1 if none. The SD-first
 * pass + strict '>' make the SD copy win on a version tie. Shared by vfs_read_module
 * (full RAM read) and vfs_map_module (XIP map) so the precedence logic lives once. */
static int find_best_module(const char *path) {
    int pcount = partition_get_count();
    int best = -1;
    uint32_t best_ver = 0;
    for (int pass = 0; pass < 2; pass++) {        /* pass 0: SD only; pass 1: the rest */
        for (int i = 0; i < pcount; i++) {
            partition_info_t *part = partition_get_info(i);
            if (!part) continue;
            if ((pass == 0) != partition_is_sd(part)) continue;

            module_header_t hdr;                  /* aligned: uint32_t fields */
            uint32_t got = 0;
            if (read_from_partition(part, path, &hdr, sizeof(hdr), &got) != 0) continue;
            /* reject a module built for a different firmware (magic + framework ABI) */
            if (got < sizeof(hdr) ||
                !module_abi_ok((const uint8_t *)&hdr, MODULE_MAGIC, MODULE_ABI_VERSION)) continue;

            /* strict '>' so the first copy at the top version wins (SD-first). */
            if (best < 0 || hdr.version > best_ver) { best = i; best_ver = hdr.version; }
        }
    }
    return best;
}

int vfs_read_module(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
    int best = find_best_module(path);
    if (best < 0) return -1;
    return read_from_partition(partition_get_info(best), path, dst, max, out_size);
}

/* defined in fatfs_ro.c: map a contiguous FAT file to its mapped-flash address (self-mounting). */
extern int fat_ro_map(uint32_t base, const char *path, uint32_t *out_addr, uint32_t *out_size);

/* For execute-in-place: find the highest-version copy of the module (same precedence as
 * vfs_read_module) and, IF that copy lives on a memory-mapped FAT partition as a contiguous
 * file, return its mapped-flash address + size so the loader can run .text in place. Returns
 * -1 when the winning copy is not XIP-able (SD, LittleFS, non-FAT, fragmented, or absent), in
 * which case the caller falls back to vfs_read_module (a full RAM copy). */
int vfs_map_module(const char *path, uint32_t *out_addr, uint32_t *out_size) {
    int best = find_best_module(path);
    if (best < 0) return -1;
    partition_info_t *bp = partition_get_info(best);
    if (partition_is_sd(bp)) return -1;                                     /* SD: not mappable */
    if (!bp->type || bp->type[0] != 'F' || bp->type[1] != 'A') return -1;   /* FAT volume only */
    return fat_ro_map(bp->address, path, out_addr, out_size);
}

/* Map an arbitrary file (e.g. a .fnt) to its contiguous mapped-flash address for read-in-place.
 * No version scan or header check (unlike vfs_map_module): just find the file on a memory-mapped
 * FAT partition and return its address + size (fat_ro_map verifies contiguity). Returns -1 if the
 * file is not a contiguous file on a mappable FAT partition; the caller falls back to a RAM read. */
int vfs_map_file(const char *path, uint32_t *out_addr, uint32_t *out_size) {
    int pcount = partition_get_count();
    for (int i = 0; i < pcount; i++) {
        partition_info_t *part = partition_get_info(i);
        if (!part || partition_is_sd(part)) continue;
        if (!part->type || part->type[0] != 'F' || part->type[1] != 'A') continue;  /* FAT only */
        if (fat_ro_map(part->address, path, out_addr, out_size) == 0) return 0;
    }
    return -1;
}

int vfs_read_file(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
    int pcount = partition_get_count();
    *out_size = 0;
    /* SD first (pass 0), then the rest — same precedence as module loads, so a
     * file on the SD card shadows one baked into the LittleFS image. No magic
     * check: this reads an arbitrary file (e.g. an i18n .fnt / .lang pack). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pcount; i++) {
            partition_info_t *part = partition_get_info(i);
            if (!part) continue;
            if ((pass == 0) != partition_is_sd(part)) continue;
            if (read_from_partition(part, path, dst, max, out_size) == 0 && *out_size > 0)
                return 0;
        }
    }
    return -1;
}

/* 'L','N','G','2' (self-describing pack format) — first 12 bytes of a .lang pack
 * are {u32 magic, u16 abi, u16 str_count, u32 version}. */
#define LANG_PACK_MAGIC 0x32474E4Cu

static uint32_t vfs_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The chainloader's main LittleFS partition (on flash, not the SD) — where the
 * /i18n assets live. NULL if none is visible. */
static partition_info_t *vfs_main_lfs(void) {
    int pcount = partition_get_count();
    for (int i = 0; i < pcount; i++) {
        partition_info_t *part = partition_get_info(i);
        if (part && !partition_is_sd(part) && part->type && part->type[0] == 'L')
            return part;
    }
    return NULL;
}

int vfs_lfs_free_kb(void) {
    partition_info_t *p = vfs_main_lfs();
    if (!p) return -1;
    vfs_driver_t *drv = vfs_get_driver("LFS");
    if (!drv || !drv->mount || !drv->statfs) return -1;
    if (drv->mount(p->address, p->size) != 0) return -1;
    uint32_t total = 0, freesec = 0;
    int r = drv->statfs(&total, &freesec);
    if (drv->unmount) drv->unmount();
    return (r == 0) ? (int)(freesec / 2u) : -1;   /* 512-byte sectors -> KiB */
}

int vfs_lfs_write_file(const char *path, const void *data, uint32_t len) {
    if (!vfs_is_lfs_rw_loaded())
        vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    partition_info_t *p = vfs_main_lfs();
    if (!p) return -1;
    vfs_driver_t *drv = vfs_get_driver("LFS");
    if (!drv || !drv->mount || !drv->open || !drv->write) return -1;   /* needs RW */
    if (drv->mount(p->address, p->size) != 0) return -1;

    /* Ensure the parent directory exists (LittleFS open won't auto-create it).
     * Pack paths are one level deep (/i18n/x or /lang/x), so a single mkdir of
     * the immediate parent suffices; an existing dir just errors harmlessly. */
    if (drv->mkdir) {
        const char *slash = strrchr(path, '/');
        if (slash && slash != path) {
            char dir[128];
            uint32_t n = (uint32_t)(slash - path);
            if (n < sizeof(dir)) {
                memcpy(dir, path, n);
                dir[n] = '\0';
                drv->mkdir(dir);
            }
        }
    }

    void *fctx = NULL;
    int rc = -1;
    if (drv->open(path, 2 /* write/create */, &fctx) == 0) {
        uint32_t total = 0;
        int w = 0;
        while (total < len) {
            size_t wr = 0;
            w = drv->write(fctx, (const uint8_t *)data + total, len - total, &wr);
            if (w != 0 || wr == 0) break;
            total += (uint32_t)wr;
        }
        if (drv->close) drv->close(fctx);
        if (total == len) rc = 0;
    }
    if (drv->unmount) drv->unmount();
    return rc;
}

/* Read a file from the main on-flash LittleFS only (never the SD card). 0 ok. */
static int read_main_lfs(const char *path, void *dst, uint32_t max, uint32_t *got) {
    partition_info_t *p = vfs_main_lfs();
    if (!p) return -1;
    return read_from_partition(p, path, dst, max, got);
}

/* The SD-card partition, or NULL if no card is present. */
static partition_info_t *vfs_sd_partition(void) {
    int pcount = partition_get_count();
    for (int i = 0; i < pcount; i++) {
        partition_info_t *part = partition_get_info(i);
        if (part && partition_is_sd(part)) return part;
    }
    return NULL;
}

/* Read the first `n` bytes of an SD-card / LittleFS file (header only -- never the
 * whole file, so an artifact's version can be peeked without buffering it). 0 ok. */
int vfs_sd_read_header(const char *path, void *dst, uint32_t n) {
    partition_info_t *sd = vfs_sd_partition();
    uint32_t got = 0;
    return (sd && read_from_partition(sd, path, dst, n, &got) == 0 && got > 0) ? 0 : -1;
}
int vfs_lfs_read_header(const char *path, void *dst, uint32_t n) {
    uint32_t got = 0;
    return (read_main_lfs(path, dst, n, &got) == 0 && got > 0) ? 0 : -1;
}
/* Header peek on ANY partition (SD / LittleFS / FAT store), so the installer can scan every source. */
int vfs_read_header(partition_info_t *part, const char *path, void *dst, uint32_t n) {
    uint32_t got = 0;
    return (part && read_from_partition(part, path, dst, n, &got) == 0 && got > 0) ? 0 : -1;
}

/* Stream-copy `sp` on `sp_part` to `dp` on `dp_part` in 4 KiB chunks (no whole-file buffer),
 * creating the immediate parent dir. A FAT-store dest is laid out CONTIGUOUS (size -> f_expand)
 * for XIP; size 0 is a plain copy. Loads the LFS/FAT RW drivers as needed. The generic install
 * copy: any source (SD/LFS/FAT) -> FAT or LFS. Returns 0 on success. */
int vfs_install_copy(partition_info_t *sp_part, const char *sp,
                     partition_info_t *dp_part, const char *dp, uint32_t size) {
    if (!sp_part || !dp_part) return -1;
    if (!vfs_is_lfs_rw_loaded())
        vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    if (!vfs_is_fat_rw_loaded() && vfs_module_available("/fs/fat.bin"))
        vfs_load_dynamic_driver("FAT", "/fs/fat.bin");

    vfs_driver_t *src = vfs_get_driver(partition_driver_name(sp_part));
    vfs_driver_t *dst = vfs_get_driver(partition_driver_name(dp_part));
    if (!src || !dst || !src->mount || !dst->mount) return -1;
    if (src->mount(sp_part->address, sp_part->size) != 0) return -1;
    if (dst->mount(dp_part->address, dp_part->size) != 0) { if (src->unmount) src->unmount(); return -1; }

    if (dst->mkdir) {                              /* one-level parent (/i18n, /modules) */
        const char *slash = strrchr(dp, '/');
        if (slash && slash != dp) {
            char dir[128];
            uint32_t dn = (uint32_t)(slash - dp);
            if (dn < sizeof(dir)) { memcpy(dir, dp, dn); dir[dn] = '\0'; dst->mkdir(dir); }
        }
    }

    int r = vfs_copy_open_file(src, sp, dst, dp, size, NULL, NULL);
    if (dst->unmount) dst->unmount();
    if (src->unmount) src->unmount();
    return (r == VFS_COPY_OK) ? 0 : -1;
}

/* SD -> main LittleFS shim over vfs_install_copy (the language/font install path). */
int vfs_copy_sd_to_lfs(const char *sd_path, const char *lfs_path) {
    return vfs_install_copy(vfs_sd_partition(), sd_path, vfs_main_lfs(), lfs_path, 0);
}

/* Delete `path` from `part` (loads FAT/LFS RW as needed). 0 on success; <0 if the file or a RW
 * driver is absent. That <0 is exactly "leave the source unless it is writable" -- a read-only
 * partition has no RW driver (unlink NULL) and a write-locked card fails the unlink. */
int vfs_install_unlink(partition_info_t *part, const char *path) {
    if (!part) return -1;
    if (!vfs_is_lfs_rw_loaded())
        vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    if (!vfs_is_fat_rw_loaded() && vfs_module_available("/fs/fat.bin"))
        vfs_load_dynamic_driver("FAT", "/fs/fat.bin");
    vfs_driver_t *drv = vfs_get_driver(partition_driver_name(part));
    if (!drv || !drv->mount || !drv->unlink) return -1;
    if (drv->mount(part->address, part->size) != 0) return -1;
    int r = drv->unlink(path);
    if (drv->unmount) drv->unmount();
    return (r == 0) ? 0 : -1;
}

/* SD-source delete shim (the SD is delivery-only). */
int vfs_sd_unlink(const char *path) {
    return vfs_install_unlink(vfs_sd_partition(), path);
}

/* --- Streaming reads (handle-based) for on-demand glyph paging ---------------------- */
int vfs_stream_open_drv(vfs_stream_t *s, vfs_driver_t *drv, uint32_t addr, uint32_t size,
                        const char *path) {
    if (!s) return -1;
    s->drv = NULL; s->ctx = NULL;
    if (!drv || !drv->mount || !drv->open || !drv->read || !drv->seek || !path) return -1;
    if (drv->mount(addr, size) != 0) return -1;
    void *ctx = NULL;
    if (drv->open(path, 1 /* read */, &ctx) != 0) return -1;
    s->drv = drv; s->ctx = ctx;
    return 0;
}

int vfs_open_stream(vfs_stream_t *s, const char *path) {
    if (!s || !path) return -1;
    s->drv = NULL; s->ctx = NULL;
    int pcount = partition_get_count();
    for (int i = 0; i < pcount; i++) {                 /* LittleFS / flash only -- never the SD */
        partition_info_t *part = partition_get_info(i);
        if (!part || partition_is_sd(part)) continue;
        const char *dname = partition_driver_name(part);
        if (!dname) continue;
        vfs_ensure_rw(dname);                          /* load the seek-capable RW driver */
        vfs_driver_t *drv = vfs_get_driver(dname);
        if (drv && vfs_stream_open_drv(s, drv, part->address, part->size, path) == 0) return 0;
    }
    return -1;
}

int vfs_stream_read(vfs_stream_t *s, void *buf, uint32_t n) {
    if (!s || !s->drv || !s->drv->read) return -1;
    size_t rd = 0;
    if (s->drv->read(s->ctx, buf, n, &rd) != 0) return -1;
    return (int)rd;
}
int vfs_stream_seek(vfs_stream_t *s, uint32_t off) {
    if (!s || !s->drv || !s->drv->seek) return -1;
    return s->drv->seek(s->ctx, off);
}
void vfs_stream_close(vfs_stream_t *s) {
    if (s && s->drv && s->drv->close) s->drv->close(s->ctx);
    if (s) { s->drv = NULL; s->ctx = NULL; }
}

int vfs_lfs_read(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
    /* Generic LFS-only read (no SD scan): the hot path for language packs, which must
     * never touch the slow SD card. The caller validates the file format (the language
     * module owns the .lang magic/ABI/header knowledge). */
    *out_size = 0;
    return read_main_lfs(path, dst, max, out_size);
}

uint32_t vfs_lfs_lang_version(const char *path, uint16_t want_abi) {
    uint8_t hdr[12];
    uint32_t got = 0;
    (void)want_abi;   /* core-authoritative ABI (see vfs_read_lang_lfs) */
    if (read_main_lfs(path, hdr, sizeof(hdr), &got) != 0 || got < sizeof(hdr)) return 0;
    if (!pack_abi_ok(hdr, LANG_PACK_MAGIC, STRINGS_ABI_VERSION)) return 0;
    return vfs_rd32(hdr + 8);
}

int vfs_lfs_has(const char *path) {
    uint8_t b;
    uint32_t got = 0;
    return (read_main_lfs(path, &b, 1, &got) == 0) ? 1 : 0;
}

/* True if `ent` is a "*.lang" file entry. */
static int is_lang_file(const vfs_dirent_t *ent) {
    if (ent->type != VFS_TYPE_FILE) return 0;
    int L = 0;
    while (ent->name[L]) L++;
    return L > 5 && ent->name[L-5] == '.' && ent->name[L-4] == 'l' &&
           ent->name[L-3] == 'a' && ent->name[L-2] == 'n' && ent->name[L-1] == 'g';
}

/* Enumerate the FILE entries of `dir` on the main on-flash LittleFS, invoking cb with
 * each name (a pointer into a transient buffer -- copy in the callback). Mounts +
 * unmounts the LFS itself; the feature-module discovery uses this to scan
 * /modules/features at boot (no active mount to disturb at that point). */
void vfs_lfs_enum_dir(const char *dir, void (*cb)(const char *name)) {
    /* The main LFS partition's driver name IS "LFS", so this is the generic enum
     * scoped to that one partition (no separate mount/scan body needed). */
    vfs_enum_dir(vfs_main_lfs(), dir, cb);
}

/* Enumerate `dir` on a SPECIFIC partition (any driver: the in-core FAT, LFS, ...), so feature
 * discovery can scan the EXT-FAT module store (the XIP source), not just the LFS. Same
 * mount/opendir/readdir dance as vfs_lfs_enum_dir, but the partition + its driver are caller-chosen. */
void vfs_enum_dir(partition_info_t *p, const char *dir, void (*cb)(const char *name)) {
    if (!p) return;
    vfs_driver_t *drv = vfs_get_driver(partition_driver_name(p));
    if (!drv || !drv->mount || !drv->opendir || !drv->readdir) return;
    if (drv->mount(p->address, p->size) != 0) return;
    void *dctx = NULL;
    if (drv->opendir(dir, &dctx) == 0) {
        vfs_dirent_t ent;
        while (drv->readdir(dctx, &ent) == 1)
            if (ent.type == VFS_TYPE_FILE) cb(ent.name);
        if (drv->closedir) drv->closedir(dctx);
    }
    if (drv->unmount) drv->unmount();
}

int vfs_sd_dir_exists(const char *dir) {
    /* Cheap probe with whatever FAT driver is loaded (the in-core 8.3 one is fine
     * for a directory name) so we don't pay to load the LFN FAT-RW module on a
     * card that has no /i18n folder. */
    int pcount = partition_get_count();
    for (int i = 0; i < pcount; i++) {
        partition_info_t *part = partition_get_info(i);
        if (!part || !partition_is_sd(part)) continue;
        vfs_driver_t *drv = vfs_get_driver(partition_driver_name(part));
        if (!drv || !drv->mount || !drv->opendir) continue;
        if (drv->mount(part->address, part->size) != 0) continue;
        void *dctx = NULL;
        int ok = (drv->opendir(dir, &dctx) == 0);
        if (ok && drv->closedir) drv->closedir(dctx);
        if (drv->unmount) drv->unmount();
        if (ok) return 1;
    }
    return 0;
}

int vfs_sd_list_langs(char *names, int stride, int max) {
    /* Collect the basenames of /i18n/<code>.lang on the SD card (needs the LFN FAT-RW
     * driver loaded, since ".lang" is not an 8.3 extension). Returns the count. */
    int count = 0;
    int pcount = partition_get_count();
    for (int i = 0; i < pcount && count < max; i++) {
        partition_info_t *part = partition_get_info(i);
        if (!part || !partition_is_sd(part)) continue;
        vfs_driver_t *drv = vfs_get_driver(partition_driver_name(part));
        if (!drv || !drv->mount || !drv->opendir || !drv->readdir) continue;
        if (drv->mount(part->address, part->size) != 0) continue;
        void *dctx = NULL;
        if (drv->opendir("/i18n", &dctx) == 0) {
            vfs_dirent_t ent;
            while (count < max && drv->readdir(dctx, &ent) == 1) {
                if (!is_lang_file(&ent)) continue;
                int L = 0; while (ent.name[L]) L++;
                if (L >= stride) continue;
                char *slot = names + (long)count * stride;
                int m = 0; while (ent.name[m]) { slot[m] = ent.name[m]; m++; }
                slot[m] = '\0';
                count++;
            }
            if (drv->closedir) drv->closedir(dctx);
        }
        if (drv->unmount) drv->unmount();
    }
    return count;
}

/*
 * Cheap existence probe: does `path` exist on any read-capable filesystem?
 * Same SD-first, two-pass iteration as vfs_read_whole, but only opens and closes
 * the file (no data transfer). Used to decide whether a FAT/LFS partition can be
 * made RW (its RW module is reachable) without paying for a full read.
 */
bool vfs_module_available(const char *path) {
    int pcount = partition_get_count();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < pcount; i++) {
            partition_info_t *part = partition_get_info(i);
            if (!part) continue;
            bool sd = partition_is_sd(part);
            if ((pass == 0) != sd) continue;

            const char *dname = partition_driver_name(part);
            vfs_driver_t *drv = dname ? vfs_get_driver(dname) : NULL;
            if (!drv || !drv->mount || !drv->open) continue;
            if (drv->mount(part->address, part->size) != 0) continue;

            void *fctx = NULL;
            bool found = (drv->open(path, 1 /* read */, &fctx) == 0);
            if (found && drv->close) drv->close(fctx);
            if (drv->unmount) drv->unmount();
            if (found) return true;
        }
    }
    return false;
}

/* Load the RW driver for a filesystem (by driver name) on demand. Shared by the file
 * browser + the feature host so the load logic + module paths live in one place.
 * vfs_load_dynamic_driver already no-ops if the RW driver is loaded, so no guard needed. */
void vfs_ensure_rw(const char *name) {
    if (!name) return;
    if (name[0] == 'L')
        vfs_load_dynamic_driver("LFS", "/fs/lfs.bin");
    else if (name[0] == 'F' && name[1] == 'A')
        vfs_load_dynamic_driver("FAT", "/fs/fat.bin");
}

int vfs_load_dynamic_driver(const char *name, const char *bin_path) {
    /* LFS and FAT each have an in-core RO driver already registered under the
     * same name, so vfs_get_driver(name) is non-NULL before the RW module loads.
     * Gate those on a dedicated "RW loaded" flag instead; the RW module registers
     * over the RO one (vfs_get_driver searches newest-first). */
    if (strcmp(name, "LFS") == 0) {
        if (g_lfs_rw_loaded) return 0;
    } else if (strcmp(name, "FAT") == 0) {
        if (g_fat_rw_loaded) return 0;
    } else if (vfs_get_driver(name) != NULL) {
        return 0;
    }

    /* Caller-owned vtables the module's init_module fills (one per FS kind). */
    static vfs_driver_t dyn_drv_lfs;
    static vfs_driver_t dyn_drv_fat;
    vfs_driver_t *dyn = (strcmp(name, "LFS") == 0) ? &dyn_drv_lfs : &dyn_drv_fat;
    memset(dyn, 0, sizeof(*dyn));

    /* Generic loader: bump-allocate RAM, relocate the PIE image, call init. */
    if (mod_load(bin_path, dyn, &g_host_api) && vfs_register_driver(dyn) == 0) {
        if (strcmp(name, "LFS") == 0) g_lfs_rw_loaded = true;
        else if (strcmp(name, "FAT") == 0) g_fat_rw_loaded = true;
        return 0;
    }
    return -1;
}
