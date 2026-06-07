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

static int host_flash_read(uint32_t addr, void *buf, size_t size) {
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
    uint32_t mem_addr = 0x90000000UL + addr;
    dcache_before_write(mem_addr, size);

    OSPI_DisableMemoryMappedMode();
    OSPI_Program(addr, buf, size);
    OSPI_EnableMemoryMappedMode();

    dcache_after_write(mem_addr, size);
    return 0;
}

static int host_flash_erase(uint32_t addr, uint32_t size) {
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
    if (dst->open(dp, 2 /* write/create */, &fd) != 0) {
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
int vfs_read_module(const char *path, void *dst, uint32_t max, uint32_t *out_size) {
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
    if (best < 0) return -1;

    return read_from_partition(partition_get_info(best), path, dst, max, out_size);
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
        vfs_load_dynamic_driver("LFS", "/modules/filesystems/lfs_rw.bin");
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

/* Stream-copy an SD-card file to the main LittleFS in 4 KiB chunks (no whole-file
 * buffer), creating the immediate parent dir. Loads the FAT (read) + lfs_rw (write)
 * modules if needed. Returns 0 on success. This is the module-facing seam over the
 * shared vfs_copy_open_file. */
int vfs_copy_sd_to_lfs(const char *sd_path, const char *lfs_path) {
    if (!vfs_is_lfs_rw_loaded())
        vfs_load_dynamic_driver("LFS", "/modules/filesystems/lfs_rw.bin");
    if (!vfs_is_fat_rw_loaded() && vfs_module_available("/modules/filesystems/fatfs.bin"))
        vfs_load_dynamic_driver("FAT", "/modules/filesystems/fatfs.bin");

    partition_info_t *sd = vfs_sd_partition();
    partition_info_t *lfs = vfs_main_lfs();
    if (!sd || !lfs) return -1;
    const char *sname = partition_driver_name(sd);
    vfs_driver_t *src = sname ? vfs_get_driver(sname) : NULL;
    vfs_driver_t *dst = vfs_get_driver("LFS");
    if (!src || !dst || !src->mount || !dst->mount) return -1;
    if (src->mount(sd->address, sd->size) != 0) return -1;
    if (dst->mount(lfs->address, lfs->size) != 0) { if (src->unmount) src->unmount(); return -1; }

    if (dst->mkdir) {                              /* one-level parent (/i18n, /drivers) */
        const char *slash = strrchr(lfs_path, '/');
        if (slash && slash != lfs_path) {
            char dir[128];
            uint32_t dn = (uint32_t)(slash - lfs_path);
            if (dn < sizeof(dir)) { memcpy(dir, lfs_path, dn); dir[dn] = '\0'; dst->mkdir(dir); }
        }
    }

    int r = vfs_copy_open_file(src, sd_path, dst, lfs_path, 0, NULL, NULL);
    if (dst->unmount) dst->unmount();
    if (src->unmount) src->unmount();
    return (r == VFS_COPY_OK) ? 0 : -1;
}

int vfs_read_lang_lfs(const char *path, void *dst, uint32_t max, uint32_t *out_size,
                      uint16_t want_abi) {
    /* The hot path: read the canonical pack from LittleFS only, so switching the
     * UI language never touches the slow SD card. Checks magic + ABI. */
    *out_size = 0;
    uint32_t got = 0;
    if (read_main_lfs(path, dst, max, &got) != 0 || got < 12) return -1;
    const uint8_t *h = (const uint8_t *)dst;
    (void)want_abi;   /* the core is authoritative: enforce its OWN strings ABI, not the caller's */
    if (!pack_abi_ok(h, LANG_PACK_MAGIC, STRINGS_ABI_VERSION)) return -1;
    *out_size = got;
    return 0;
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

void vfs_lfs_enum_langs(uint16_t want_abi,
                        void (*cb)(const char *code, const char *endonym, const char *script)) {
    /* Walk /i18n on the main LittleFS; for each valid pack hand the discovery its
     * self-described code + endonym + script (read straight from the 76-B header). */
    (void)want_abi;   /* core-authoritative ABI (see vfs_read_lang_lfs) */
    partition_info_t *p = vfs_main_lfs();
    if (!p) return;
    vfs_driver_t *drv = vfs_get_driver("LFS");
    if (!drv || !drv->mount || !drv->opendir || !drv->readdir || !drv->open || !drv->read) return;
    if (drv->mount(p->address, p->size) != 0) return;
    void *dctx = NULL;
    if (drv->opendir("/i18n", &dctx) == 0) {
        vfs_dirent_t ent;
        while (drv->readdir(dctx, &ent) == 1) {
            if (!is_lang_file(&ent)) continue;
            char path[280];
            int n = 0; const char *pre = "/i18n/";
            while (pre[n]) { path[n] = pre[n]; n++; }
            int m = 0; while (ent.name[m] && n < (int)sizeof(path) - 1) path[n++] = ent.name[m++];
            path[n] = '\0';
            uint8_t hdr[76];
            void *fctx = NULL; size_t rd = 0;
            if (drv->open(path, 1 /* read */, &fctx) != 0) continue;
            drv->read(fctx, hdr, sizeof(hdr), &rd);
            if (drv->close) drv->close(fctx);
            if (rd < sizeof(hdr)) continue;
            if (!pack_abi_ok(hdr, LANG_PACK_MAGIC, STRINGS_ABI_VERSION)) continue;
            cb((const char *)(hdr + 28), (const char *)(hdr + 44), (const char *)(hdr + 12));
        }
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
