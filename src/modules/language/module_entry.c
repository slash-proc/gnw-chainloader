/*
 * Language module (PIE) — the home of the former ui/i18n.c + ui/font_ext.c. It owns
 * everything beyond built-in ASCII English: discovery of /i18n/<code>.lang on the
 * filesystem, .lang pack + .fnt script-font loading, UTF-8 glyph rendering, language
 * switching, and re-discovery after an install (the SD->LittleFS install now lives in
 * the transient installer module, not here). Loaded at boot when present so the
 * core stays English-ASCII-lean; if absent, the core simply renders English.
 *
 * lang_mgr.c (the manager) and font_ext.c (the script-font reader) compile in here
 * UNCHANGED, calling the vfs, partition, strings_set_active and gui_set_ext_glyph
 * functions by name. Those resolve to the thin forwarders below, which route to the
 * host vtable
 * the core handed us (a PIE module can't link the core directly — no symbol
 * resolution). These forwarders are the module's entire dependency on the core.
 */
#include "system/module.h"
#include "system/language.h"
#include "storage/vfs.h"
#include "storage/partition.h"
#include "ui/strings.h"
#include "ui/i18n.h"
#include "gui.h"

MODULE_HEADER;

static const lang_host_t *g_host;

/* --- forwarders: lang_mgr.c / font_ext.c call these by name --------------- */
int vfs_read_file(const char *p, void *d, uint32_t m, uint32_t *o) { return g_host->read_file(p, d, m, o); }
int vfs_read_lang_lfs(const char *p, void *d, uint32_t m, uint32_t *o, uint16_t a) { return g_host->read_lang_lfs(p, d, m, o, a); }
uint32_t vfs_lfs_lang_version(const char *p, uint16_t a) { return g_host->lfs_lang_version(p, a); }
int vfs_lfs_free_kb(void) { return g_host->lfs_free_kb(); }
int vfs_lfs_write_file(const char *p, const void *d, uint32_t l) { return g_host->lfs_write_file(p, d, l); }
int vfs_lfs_has(const char *p) { return g_host->lfs_has(p); }
void vfs_lfs_enum_langs(uint16_t a, void (*cb)(const char *, const char *, const char *)) { g_host->lfs_enum_langs(a, cb); }
int vfs_sd_dir_exists(const char *d) { return g_host->sd_dir_exists(d); }
int vfs_sd_list_langs(char *n, int s, int m) { return g_host->sd_list_langs(n, s, m); }
bool vfs_is_fat_rw_loaded(void) { return g_host->is_fat_rw_loaded(); }
bool vfs_module_available(const char *p) { return g_host->module_available(p); }
int vfs_load_dynamic_driver(const char *n, const char *b) { return g_host->load_dynamic_driver(n, b); }
int partition_get_count(void) { return g_host->partition_count(); }
partition_info_t *partition_get_info(int i) { return g_host->partition_info(i); }
bool partition_is_sd(const partition_info_t *p) { return g_host->partition_is_sd(p); }
void strings_set_active(const char *const *t) { g_host->strings_set_active(t); }
void gui_set_ext_glyph(gui_ext_glyph_fn fn) { g_host->set_ext_glyph(fn); }

/* --- entry: wire the host, build the language list + apply persisted, export -- */
void init_module(const lang_host_t *host, lang_api_t *out) {
    g_host = host;
    i18n_init();   /* discovery + apply persisted language + register strings/glyph */
    out->current      = i18n_current;
    out->count        = i18n_count;
    out->endonym      = i18n_endonym;
    out->cycle        = i18n_cycle;
    out->set          = i18n_set;
    out->rediscover   = i18n_rediscover;
    out->persist      = i18n_persist_active;
    out->code         = i18n_code;
    out->rtl          = i18n_is_rtl;
}
