#ifndef UI_I18N_H
#define UI_I18N_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Language manager: selects the active UI language (a .lang string pack + its
 * .fnt script font, read from the device filesystem) and wires it into the string
 * table (tr()) and the glyph resolver (gui_glyph).
 *
 * Only English is baked into the firmware (index 0, no files). Every other
 * language is discovered from /i18n/<code>.lang on LittleFS at boot — each pack carries
 * its own code + endonym + script — so a language never compiled in still appears.
 * Everything degrades to English/ASCII if a pack or font is missing, so the menu
 * is always reachable. The active language is persisted by code (/i18n/.active).
 */

/* Activate language `idx` (loads its pack + font). Returns true on success;
 * on any failure it reverts to English (index 0) and returns false. */
bool i18n_set(uint8_t idx);

/* Build the runtime language list (English + filesystem-discovered packs, sorted
 * by code) and apply the persisted language. Call once after the filesystems are
 * up; i18n_rediscover() folds in any packs a later install adds. */
void i18n_init(void);

/* Re-scan LittleFS after the installer module copied in new packs, so they join the
 * runtime list (keeping the user's current language, tracked by code). Call once
 * after a successful install commit. */
void i18n_rediscover(void);

/* Persist the active language (by code) to /i18n/.active. Caller debounces. */
void i18n_persist_active(void);

uint8_t i18n_current(void);          /* active index (0 = English) */
int     i18n_count(void);            /* number of selectable languages */
const char *i18n_endonym(int idx);   /* selector label, in the language's script */
const char *i18n_code(int idx);      /* locale code, e.g. "de_DE" (shown as "(de_DE)") */
bool i18n_is_rtl(void);              /* active language is right-to-left (arabic script) */
/* Next/prev language index, wrapping (the list is sorted by code, English first). */
uint8_t i18n_cycle(uint8_t cur, int dir);

#endif /* UI_I18N_H */
