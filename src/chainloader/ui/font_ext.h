#ifndef UI_FONT_EXT_H
#define UI_FONT_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include "gui.h"   /* gui_glyph_info_t */

/*
 * Active external script font — a per-script .fnt blob (cooked by
 * scripts/build/cook_font.py blob) read whole into RAM and indexed by codepoint.
 * Supplies the non-ASCII glyphs (accented Latin, CJK, ...) that the in-core ASCII
 * font can't; gui_glyph() consults it between the ASCII fast-path and the '?' box.
 *
 * Only one script font is active at a time (the active language's). Everything
 * degrades gracefully: a missing/oversized/corrupt blob just leaves no active
 * font, so non-ASCII renders as '?' and the menu stays reachable.
 */

/* Load the blob at `path` (vfs, SD-first) as the active SCRIPT font (the current
 * language's script). False on any failure (absent / too big / bad magic). */
bool font_ext_open(const char *path);

/* Load the blob at `path` as the always-on BASE font (the shared Latin set),
 * tried after the script font so accented Latin renders in any language and even
 * before one is chosen. Loaded once; survives language switches. */
bool font_ext_open_base(const char *path);

/* Activate a blob already resident in `buf` (sz bytes). The buffer must outlive
 * the active font (font_ext_open uses the module's own static buffer). Pure —
 * no I/O — so it is unit-testable on the host. */
bool font_ext_parse(const uint8_t *buf, uint32_t sz);

/* Drop the active font (back to in-core ASCII / '?'). */
void font_ext_close(void);

/* Resolve a codepoint from the active font. False if no active font or the glyph
 * is absent — callers then fall back to '?'. */
bool font_ext_glyph(uint32_t cp, gui_glyph_info_t *gi);

#endif /* UI_FONT_EXT_H */
