/*
 * Installer module (PIE, transient).
 *
 * Copies new/updated artifacts from an SD card into LittleFS, gated by the running
 * firmware's ABI so something built for a different firmware is never installed (the
 * install gate mirrors the loader's load gate). Two classes share ONE generic path,
 * differing only by an artifact_kind_t descriptor:
 *   - language packs  /i18n/<code>.lang  (+ the script font /i18n/fonts/<s>.fnt)
 *   - PIE modules     the fixed /modules/*.bin list
 *
 * It never buffers a whole file: it peeks the header to compare versions, then
 * streams the copy through the core (host->copy_sd_to_lfs over vfs_copy_open_file),
 * so even large module images install with a 4 KiB buffer that lives in the core.
 * The module is stateless (host passed per call) and tiny -- it carries no transfer
 * buffer of its own.
 */
#include "system/module.h"
#include "system/installer.h"
#include <string.h>

MODULE_HEADER;

#define LANG_MAGIC    0x32474E4Cu    /* 'L','N','G','2' (MODULE_MAGIC = 'GMOD' from module.h) */
#define LANG_HDR_SIZE 76u            /* u32 magic; u16 abi; u16 str_count; u32 version;
                                      * char script[16]; char code[16]; char endonym[32] */
#define I18N_MAX      32

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Where an artifact class keeps its magic / ABI / version (+ optional count) in the
 * header. One descriptor per class is what makes the install path generic. */
typedef struct {
    uint32_t magic;
    uint8_t  abi_off;        /* 4 (pack) / 28 (module) */
    uint8_t  abi_u16;        /* pack abi is u16; module abi is u32 */
    uint8_t  ver_off;        /* 8 (pack) / 24 (module) */
    uint8_t  count_off;      /* 6 (pack); 0 = no count check (module) */
    uint32_t want_abi;       /* the running core's expected ABI for this class */
    uint16_t want_count;     /* STR_COUNT (pack); 0 (module) */
} artifact_kind_t;

/* Install SD `sd_path` to LittleFS `lfs_path` if it is newer than (or absent on)
 * LittleFS and passes the install gate (magic + ABI [+ count]). dry=true counts only
 * (writes nothing). Returns 1 if it installed / would install. Header-only reads
 * keep large images off this module; the copy itself streams in the core. */
static int artifact_install(const installer_host_t *h, const char *sd_path,
                            const char *lfs_path, const artifact_kind_t *k, bool dry) {
    uint8_t hdr[32];
    if (h->sd_read_header(sd_path, hdr, sizeof(hdr)) != 0) return 0;   /* SD must carry it */
    if (rd32(hdr) != k->magic) return 0;
    uint32_t sd_abi = k->abi_u16 ? rd16(hdr + k->abi_off) : rd32(hdr + k->abi_off);
    if (sd_abi != k->want_abi) return 0;                              /* install gate */
    if (k->count_off && rd16(hdr + k->count_off) != k->want_count) return 0;
    uint32_t sd_ver = rd32(hdr + k->ver_off);

    /* The LittleFS copy's version (0 = absent or invalid -> install). */
    uint8_t lhdr[32];
    uint32_t lfs_ver = 0;
    if (h->lfs_read_header(lfs_path, lhdr, sizeof(lhdr)) == 0 && rd32(lhdr) == k->magic) {
        uint32_t l_abi = k->abi_u16 ? rd16(lhdr + k->abi_off) : rd32(lhdr + k->abi_off);
        if (l_abi == k->want_abi) lfs_ver = rd32(lhdr + k->ver_off);
    }
    if (lfs_ver != 0 && sd_ver <= lfs_ver) return 0;                  /* already current */
    if (dry) return 1;
    return (h->copy_sd_to_lfs(sd_path, lfs_path) == 0) ? 1 : 0;       /* streaming copy */
}

/* The PIE modules the installer can update from an SD card (canonical LittleFS
 * paths -- the same list the build deploys). */
static const char *const MODULE_PATHS[] = {
    "/modules/filesystems/fatfs.bin",
    "/modules/filesystems/lfs_rw.bin",
    "/modules/theme/theme.bin",
    "/modules/language.bin",
    "/modules/installer.bin",
};
#define N_MODULES ((int)(sizeof(MODULE_PATHS) / sizeof(MODULE_PATHS[0])))

static char g_names[I18N_MAX][24];

/* A language pack plus its dependent script font (a font has no header/version, so
 * it is just copied when LittleFS lacks it). */
static int lang_one(const installer_host_t *h, const char *name,
                    const artifact_kind_t *k, bool dry) {
    char sd_pack[64], lfs_pack[64];
    strcpy(sd_pack, "/i18n/"); strcat(sd_pack, name);
    strcpy(lfs_pack, sd_pack);
    int did = artifact_install(h, sd_pack, lfs_pack, k, dry);
    if (!did || dry) return did;

    uint8_t hdr[LANG_HDR_SIZE];
    if (h->sd_read_header(sd_pack, hdr, sizeof(hdr)) == 0 && rd32(hdr) == LANG_MAGIC) {
        char font[80];
        strcpy(font, "/i18n/fonts/");
        strcat(font, (const char *)(hdr + 12));   /* script[16] @ offset 12 */
        strcat(font, ".fnt");
        if (!h->lfs_has(font)) h->copy_sd_to_lfs(font, font);
    }
    return did;
}

static int run(const installer_host_t *h, bool dry, int kind) {
    int n = 0;
    if (kind == INSTALLER_KIND_LANG) {
        const artifact_kind_t lang = { LANG_MAGIC, 4, 1, 8, 6, h->strings_abi, h->str_count };
        int count = h->sd_dir_exists("/i18n") ? h->sd_list_langs(&g_names[0][0], 24, I18N_MAX) : 0;
        for (int i = 0; i < count; i++) {
            if (h->progress && !dry) h->progress(count ? i * 100 / count : 0, "Installing");
            n += lang_one(h, g_names[i], &lang, dry);
        }
    } else {   /* INSTALLER_KIND_MODULE */
        const artifact_kind_t mod = { MODULE_MAGIC, 28, 0, 24, 0, h->module_abi, 0 };
        for (int i = 0; i < N_MODULES; i++) {
            if (h->progress && !dry) h->progress(i * 100 / N_MODULES, "Installing");
            n += artifact_install(h, MODULE_PATHS[i], MODULE_PATHS[i], &mod, dry);
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
