/*
 * Installer module (PIE, transient).
 *
 * Installs/updates artifacts gated by the running firmware's ABI (the install gate mirrors the
 * loader's load gate, so an artifact built for a different firmware is never copied on). Two classes:
 *   - language packs  /i18n/<code>.lang (+ the script font /i18n/fonts/<s>.fnt): SD -> LittleFS.
 *   - PIE modules     the fixed /modules/*.bin list: from ANY source partition (SD / LittleFS / FAT)
 *     to the FAT store if the module is FAT-XIP-capable (MOD_FLAG_R9_PIC) else to LittleFS. The FAT
 *     destination is laid out contiguous (the core's f_expand) so it can be XIP'd in place. A
 *     successful copy deletes the source iff it is writable -- so an LFS module MIGRATES to FAT, and
 *     an SD delivery drops once installed, while a read-only source is left intact.
 *
 * It never buffers a whole file: it peeks headers to compare versions and to read the flags/size,
 * then streams the copy through the core's shared 4 KiB loop. Stateless (host passed per call).
 */
#include "system/module.h"
#include "system/installer.h"
#include <string.h>

MODULE_HEADER;

#define LANG_MAGIC    0x32474E4Cu    /* 'L','N','G','2' */
#define LANG_HDR_SIZE 76u
#define I18N_MAX      32

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Where an artifact class keeps its magic / ABI / version (+ optional count) in the header. */
typedef struct {
    uint32_t magic;
    uint8_t  abi_off;        /* 4 (pack) / 28 (module) */
    uint8_t  abi_u16;        /* pack abi is u16; module abi is u32 */
    uint8_t  ver_off;        /* 8 (pack) / 24 (module) */
    uint8_t  count_off;      /* 6 (pack); 0 = no count check (module) */
    uint32_t want_abi;
    uint16_t want_count;
} artifact_kind_t;

/* The partition matching a class: 'S' = SD, 'L' = LittleFS, 'F' = the FAT store (the non-SD FAT). */
static partition_info_t *find_part(const installer_host_t *h, char want) {
    int n = h->part_count();
    for (int i = 0; i < n; i++) {
        partition_info_t *p = h->part_info(i);
        if (!p) continue;
        bool sd = h->part_is_sd(p);
        const char *fs = h->part_fs(p);
        if (want == 'S' && sd) return p;
        if (want == 'L' && fs && fs[0] == 'L') return p;            /* "LFS" */
        if (want == 'F' && !sd && fs && fs[0] == 'F') return p;     /* non-SD "FAT" = EXT-FAT store */
    }
    return NULL;
}

/* Header version of `path` on `part` for class `k`, or 0 if absent / wrong magic / wrong ABI. */
static uint32_t artifact_ver(const installer_host_t *h, partition_info_t *part, const char *path,
                             const artifact_kind_t *k) {
    if (!part) return 0;
    uint8_t hdr[32];
    if (h->read_header(part, path, hdr, sizeof(hdr)) != 0 || rd32(hdr) != k->magic) return 0;
    uint32_t abi = k->abi_u16 ? rd16(hdr + k->abi_off) : rd32(hdr + k->abi_off);
    if (abi != k->want_abi) return 0;
    if (k->count_off && rd16(hdr + k->count_off) != k->want_count) return 0;
    return rd32(hdr + k->ver_off);
}

/* Install `sp` on `src` to `dp` on `dst` if it is newer than (or absent on) the destination and
 * passes the gate. dry=true counts only. `size` is the FAT-contiguous f_expand size (0 = plain). A
 * successful real copy deletes the source (no-op if the source is read-only). Returns 1 if installed. */
static int artifact_install(const installer_host_t *h, partition_info_t *src, const char *sp,
                            partition_info_t *dst, const char *dp, const artifact_kind_t *k,
                            uint32_t size, bool dry) {
    if (!src || !dst || src == dst) return 0;             /* nothing to do / already in place */
    uint32_t s_ver = artifact_ver(h, src, sp, k);
    if (s_ver == 0) return 0;                             /* source absent or gate-failed */
    uint32_t d_ver = artifact_ver(h, dst, dp, k);
    if (d_ver != 0 && s_ver <= d_ver) return 0;           /* destination already current */
    if (dry) return 1;
    if (h->copy(src, sp, dst, dp, size) != 0) return 0;
    h->unlink(src, sp);                                   /* delete iff writable (no-op on RO) */
    return 1;
}

static const char *const MODULE_PATHS[] = {
    "/modules/filesystems/fatfs.bin",
    "/modules/filesystems/lfs_rw.bin",
    "/modules/theme/theme.bin",
    "/modules/language.bin",
    "/modules/installer.bin",
    "/modules/fileops.bin",
};
#define N_MODULES ((int)(sizeof(MODULE_PATHS) / sizeof(MODULE_PATHS[0])))

static char g_names[I18N_MAX][24];

/* A module install: pick the highest-version source across all partitions, choose the FAT store (if
 * FAT-XIP-capable) or LittleFS as the destination, and copy/migrate via artifact_install. */
static int module_one(const installer_host_t *h, const char *path, partition_info_t *fat,
                      partition_info_t *lfs, bool dry) {
    partition_info_t *src = NULL;
    uint32_t best = 0, flags = 0, size = 0;
    int np = h->part_count();
    for (int j = 0; j < np; j++) {
        partition_info_t *p = h->part_info(j);
        uint8_t hdr[44];
        if (!p || h->read_header(p, path, hdr, sizeof(hdr)) != 0) continue;
        if (rd32(hdr) != MODULE_MAGIC || rd32(hdr + 28) != h->module_abi) continue;   /* gate */
        uint32_t ver = rd32(hdr + 24);
        if (!src || ver > best) {
            src = p; best = ver;
            flags = rd32(hdr + 36);                          /* flags @ 36 */
            size = rd32(hdr + 8) + rd32(hdr + 12) * 8;       /* reloc_offset + reloc_count*8 = file size */
        }
    }
    if (!src) return 0;
    partition_info_t *dst = (flags & MOD_FLAG_R9_PIC) ? fat : lfs;
    if (!dst) return 0;
    const artifact_kind_t mod = { MODULE_MAGIC, 28, 0, 24, 0, h->module_abi, 0 };
    return artifact_install(h, src, path, dst, path, &mod, (dst == fat) ? size : 0, dry);
}

/* A language pack plus its dependent script font (a font has no header/version, so it is copied
 * when LittleFS lacks it, and the SD copy is dropped only if WE just wrote it). */
static int lang_one(const installer_host_t *h, const char *name, partition_info_t *sd,
                    partition_info_t *lfs, const artifact_kind_t *k, bool dry) {
    char pack[64];
    strcpy(pack, "/i18n/"); strcat(pack, name);

    char font[80]; font[0] = '\0';
    if (!dry) {
        uint8_t hdr[LANG_HDR_SIZE];
        if (h->read_header(sd, pack, hdr, sizeof(hdr)) == 0 && rd32(hdr) == LANG_MAGIC) {
            strcpy(font, "/i18n/fonts/");
            strcat(font, (const char *)(hdr + 12));          /* script[16] @ 12 */
            strcat(font, ".fnt");
        }
    }

    int did = artifact_install(h, sd, pack, lfs, pack, k, 0, dry);
    if (!did || dry) return did;

    if (font[0]) {
        uint8_t probe[4];
        if (h->read_header(lfs, font, probe, sizeof(probe)) != 0 &&   /* LittleFS lacks it */
            h->copy(sd, font, lfs, font, 0) == 0)
            h->unlink(sd, font);
    }
    return did;
}

static int run(const installer_host_t *h, bool dry, int kind) {
    int n = 0;
    if (kind == INSTALLER_KIND_LANG) {
        partition_info_t *sd = find_part(h, 'S');
        partition_info_t *lfs = find_part(h, 'L');
        const artifact_kind_t lang = { LANG_MAGIC, 4, 1, 8, 6, h->strings_abi, h->str_count };
        int count = (sd && h->sd_dir_exists("/i18n")) ? h->sd_list_langs(&g_names[0][0], 24, I18N_MAX) : 0;
        for (int i = 0; i < count; i++) {
            if (h->progress && !dry) h->progress(count ? i * 100 / count : 0, "Installing");
            n += lang_one(h, g_names[i], sd, lfs, &lang, dry);
        }
    } else {   /* INSTALLER_KIND_MODULE */
        partition_info_t *fat = find_part(h, 'F');
        partition_info_t *lfs = find_part(h, 'L');
        for (int i = 0; i < N_MODULES; i++) {
            if (h->progress && !dry) h->progress(i * 100 / N_MODULES, "Installing");
            n += module_one(h, MODULE_PATHS[i], fat, lfs, dry);
        }
    }
    if (h->progress && !dry) h->progress(100, "Installing");
    return n;
}

static int scan_fn(const installer_host_t *h, int kind)   { return run(h, true, kind); }
static int commit_fn(const installer_host_t *h, int kind) { return run(h, false, kind); }

void init_module(const installer_host_t *host, installer_api_t *out) {
    (void)host;   /* stateless: host is passed to scan()/commit() per call */
    out->scan   = scan_fn;
    out->commit = commit_fn;
}
