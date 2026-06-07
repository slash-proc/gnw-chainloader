/*
 * Core language shim. Only English (ASCII) is baked into the firmware; everything
 * else lives in the PIE language module (/modules/language.bin — the former
 * ui/i18n.c + ui/font_ext.c). i18n_init() loads it when present: the module then
 * discovers languages, applies the persisted choice, and registers the active
 * string table + the non-ASCII glyph resolver. Absent or failed -> these stubs
 * return the English-only defaults, so the menu always renders in ASCII English
 * and is always reachable (languages are a nice-to-have). Every other entry point
 * just delegates to the loaded module, which stays resident (the pool is never
 * reset), so the string table + glyph resolver it registered remain valid.
 */
#include "ui/i18n.h"
#include "system/language.h"
#include "storage/vfs.h"
#include "storage/partition.h"
#include "ui/strings.h"
#include "gui.h"
#include <stddef.h>

static lang_api_t g_lang;   /* all-NULL until the module loads -> English defaults */

/* The core functions the module drives through: FS reads + the two render seams. */
static const lang_host_t g_host = {
    .read_file           = vfs_read_file,
    .read_lang_lfs       = vfs_read_lang_lfs,
    .lfs_lang_version    = vfs_lfs_lang_version,
    .lfs_free_kb         = vfs_lfs_free_kb,
    .lfs_write_file      = vfs_lfs_write_file,
    .lfs_has             = vfs_lfs_has,
    .lfs_enum_langs      = vfs_lfs_enum_langs,
    .sd_dir_exists       = vfs_sd_dir_exists,
    .sd_list_langs       = vfs_sd_list_langs,
    .is_fat_rw_loaded    = vfs_is_fat_rw_loaded,
    .module_available    = vfs_module_available,
    .load_dynamic_driver = vfs_load_dynamic_driver,
    .partition_count     = partition_get_count,
    .partition_info      = partition_get_info,
    .partition_is_sd     = partition_is_sd,
    .strings_set_active  = strings_set_active,
    .set_ext_glyph       = gui_set_ext_glyph,
};

void i18n_init(void) {
    lang_api_t api = {0};
    if (mod_load_language("/modules/language.bin", &g_host, &api))
        g_lang = api;     /* module owns discovery + rendering from here on */
    /* else: g_lang stays all-NULL -> English ASCII via the defaults below */
}

uint8_t     i18n_current(void) { return g_lang.current ? g_lang.current() : 0; }
int         i18n_count(void)   { return g_lang.count   ? g_lang.count()   : 1; }
const char *i18n_endonym(int idx) {
    return g_lang.endonym ? g_lang.endonym(idx) : (idx == 0 ? "English" : "");
}
uint8_t i18n_cycle(uint8_t cur, int dir) { return g_lang.cycle ? g_lang.cycle(cur, dir) : 0; }
bool    i18n_set(uint8_t idx)            { return g_lang.set   ? g_lang.set(idx)   : (idx == 0); }
void    i18n_rediscover(void)            { if (g_lang.rediscover) g_lang.rediscover(); }
void    i18n_persist_active(void)        { if (g_lang.persist) g_lang.persist(); }
