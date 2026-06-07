#ifndef SYSTEM_INSTALLER_H
#define SYSTEM_INSTALLER_H

#include <stdint.h>
#include <stdbool.h>
#include "storage/partition.h"   /* partition_info_t for the generic install primitives */

/*
 * Transient installer module interface.
 *
 * SD->LittleFS install is not needed for basic recovery, so it lives entirely in
 * its own PIE module (/modules/installer.bin, MOD_FLAG_TRANSIENT): the core loads
 * it on demand, runs it, and reclaims its pool slot (mod_pool_reset).
 *
 * It installs TWO artifact classes with one generic, gated path: language packs
 * (/i18n/<code>.lang + their /i18n/fonts/<script>.fnt) and PIE modules (the fixed
 * /modules/*.bin list). The install gate mirrors the loader's load gate -- magic +
 * the running core's ABI for that class -- so an artifact built for a different
 * firmware is never copied onto the device. The core passes its OWN expected ABI
 * values in, so the module enforces the running firmware's contract.
 *
 * The module never buffers a whole file: it peeks only the header (sd/lfs_read_
 * header) to compare versions, then streams the copy through the shared in-core
 * vfs_copy_open_file (copy_sd_to_lfs), so even large module images install with a
 * 4 KiB transfer buffer that lives in the core, not here.
 *
 * Flow (core-orchestrated): scan() counts new artifacts; the core shows one confirm;
 * on accept commit() installs them. The module is loaded for the scan, reclaimed,
 * then loaded again for the commit -- so a declined prompt leaves nothing resident.
 */
typedef struct {
    /* Header peek on ANY partition (version compare without buffering the file). 0 ok. */
    int  (*read_header)(partition_info_t *part, const char *path, void *dst, uint32_t n);
    /* Stream-copy a file between partitions; a FAT dest lands CONTIGUOUS (size -> f_expand) for XIP. 0 ok. */
    int  (*copy)(partition_info_t *src, const char *sp, partition_info_t *dst, const char *dp, uint32_t size);
    /* Delete a file from a partition; <0 on a read-only partition (no RW driver / write-locked card)
     * -- which is exactly "delete the source only if it is writable". */
    int  (*unlink)(partition_info_t *part, const char *path);
    /* Partition enumeration: scan every source, classify a destination (FAT store vs LFS vs SD). */
    int  (*part_count)(void);
    partition_info_t *(*part_info)(int i);
    bool (*part_is_sd)(const partition_info_t *part);
    const char *(*part_fs)(const partition_info_t *part);   /* "FAT" / "LFS" / ... */
    /* Language-pack SD scan helpers (the lang class still lists /i18n on the SD). */
    int  (*sd_dir_exists)(const char *dir);
    int  (*sd_list_langs)(char *names, int stride, int max);
    /* Optional UI seam: install progress (pct 0..100, a short label). May be NULL. */
    void (*progress)(int pct, const char *what);
    /* The running core's expected ABI values -- the install gate enforces THESE. */
    uint16_t strings_abi;     /* == STRINGS_ABI_VERSION */
    uint16_t str_count;       /* == (uint16_t)STR_COUNT  */
    uint32_t module_abi;      /* == MODULE_ABI_VERSION   */
} installer_host_t;

/* Artifact classes -- scan/commit operate on ONE class so the core can prompt per
 * class (a module-only SD shows "Install N module(s)?", not a language prompt). */
#define INSTALLER_KIND_LANG   0   /* /i18n/<code>.lang packs (+ their script fonts) */
#define INSTALLER_KIND_MODULE 1   /* the /modules/*.bin PIE-module list */

typedef struct {
    int (*scan)(const installer_host_t *host, int kind);     /* count new `kind` artifacts; no writes */
    int (*commit)(const installer_host_t *host, int kind);   /* install them; returns count done */
} installer_api_t;

/* loader.c: load /modules/installer.bin transiently and run its
 * init_module(host, out_api). Returns false if missing/invalid. The CALLER owns the
 * pool slot (mod_pool_mark before, mod_pool_reset after). */
bool mod_load_installer(const char *path, const installer_host_t *host, installer_api_t *out_api);

#endif /* SYSTEM_INSTALLER_H */
