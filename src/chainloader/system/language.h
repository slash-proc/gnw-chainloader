#ifndef SYSTEM_LANGUAGE_H
#define SYSTEM_LANGUAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "storage/partition.h"   /* partition_info_t */
#include "gui.h"                  /* gui_ext_glyph_fn */

/*
 * Language module interface. Only English (ASCII) is baked into the core; every
 * non-English language — discovery, .lang pack + .fnt script font loading, UTF-8
 * rendering, switching, and re-discovery after an install — lives in the PIE module
 * /modules/language.bin (the new home of the former ui/i18n.c + ui/font_ext.c). It
 * is loaded at boot whenever present, so UTF-8 works even with English active
 * (e.g. accented filenames in the browser). Absent/failed -> the core stays
 * English-ASCII and the menu is always reachable.
 *
 * The core hands the module a host vtable (the FS reads + the two core render
 * seams the module must drive); the module fills a lang_api_t the core delegates
 * to. The module stays resident (the pool is never reset), so the string table
 * and glyph resolver it registers remain valid for the life of the session.
 */
typedef struct {
    /* Filesystem — the core owns the FS drivers + the partition table. */
    int      (*read_file)(const char *path, void *dst, uint32_t max, uint32_t *out_size);
    int      (*read_lang_lfs)(const char *path, void *dst, uint32_t max, uint32_t *out_size, uint16_t abi);
    uint32_t (*lfs_lang_version)(const char *path, uint16_t abi);
    int      (*lfs_free_kb)(void);
    int      (*lfs_write_file)(const char *path, const void *data, uint32_t len);
    int      (*lfs_has)(const char *path);
    void     (*lfs_enum_langs)(uint16_t abi, void (*cb)(const char *, const char *, const char *));
    int      (*sd_dir_exists)(const char *dir);
    int      (*sd_list_langs)(char *names, int stride, int max);
    bool     (*is_fat_rw_loaded)(void);
    bool     (*module_available)(const char *path);
    int      (*load_dynamic_driver)(const char *name, const char *bin_path);
    /* Partition table (for the SD install scan). */
    int               (*partition_count)(void);
    partition_info_t *(*partition_info)(int index);
    bool              (*partition_is_sd)(const partition_info_t *p);
    /* Core render/string seams the module drives. */
    void (*strings_set_active)(const char *const *table);
    void (*set_ext_glyph)(gui_ext_glyph_fn fn);
} lang_host_t;

/* Filled by the module's init_module; the core's thin ui/i18n.c delegates to it.
 * All-NULL (module absent) -> the core's English-only defaults. */
typedef struct {
    uint8_t     (*current)(void);
    int         (*count)(void);
    const char *(*endonym)(int idx);
    uint8_t     (*cycle)(uint8_t cur, int dir);
    bool        (*set)(uint8_t idx);
    void        (*rediscover)(void);   /* re-scan LittleFS after the installer added packs */
    void        (*persist)(void);
} lang_api_t;

/* Core-side loader (system/loader.c): load /modules/language.bin and run its
 * init_module(host, out_api). Returns false if the module is missing/invalid. */
bool mod_load_language(const char *path, const lang_host_t *host, lang_api_t *out_api);

#endif /* SYSTEM_LANGUAGE_H */
