#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdbool.h>

char *int_to_str(int val, char *buf);
#ifdef ENABLE_EXTENDED_UTILS
void int_to_str_w(int val, char *buf, int width);
#endif
void hex_to_str(uint32_t val, char *buf, int width);

/* Bounded string ops: never write past dst[cap-1], always NUL-terminate; an oversized
 * source TRUNCATES rather than overflowing (so no translated/module string can smash a
 * fixed UI buffer and crash the device). Use these for any buffer fed by `tr()`/data. */
void str_lcpy(char *dst, int cap, const char *src);
void str_lcat(char *dst, int cap, const char *src);

/* True if file extension `ext` (no leading dot) is in `list` -- a comma-separated set of
 * lowercase extensions, e.g. ext_list_match("jpg,jpeg", "JPEG") -> true. Case-insensitive;
 * a list with no comma is the single-extension common case. Lets a feature module's header
 * declare several handled extensions in one field. */
bool ext_list_match(const char *list, const char *ext);

/* Single-placeholder template splicers (there is no printf), BOUNDED to `cap`. Copy
 * `tmpl` into `dst`, replacing the FIRST "%d" (resp. "%s") with the integer (resp.
 * string); a template with no placeholder copies through unchanged. Lets each language
 * place the value correctly (e.g. "Updated %s" / "%s OFW" / "File %d"). */
void str_fmt1_int(char *dst, int cap, const char *tmpl, int n);
void str_fmt1_str(char *dst, int cap, const char *tmpl, const char *s);

void format_size(uint32_t bytes, char *buf);
/* Like format_size but takes a count of 512-byte sectors, so capacities > 4 GB
 * (which overflow a uint32_t byte count) format correctly. */
void format_size_sectors(uint32_t sectors, char *buf);

#endif // UTILS_H
