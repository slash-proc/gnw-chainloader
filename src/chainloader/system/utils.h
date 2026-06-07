#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

void int_to_str(int val, char *buf);
#ifdef ENABLE_EXTENDED_UTILS
void int_to_str_w(int val, char *buf, int width);
#endif
void hex_to_str(uint32_t val, char *buf, int width);
void format_size(uint32_t bytes, char *buf);
/* Like format_size but takes a count of 512-byte sectors, so capacities > 4 GB
 * (which overflow a uint32_t byte count) format correctly. */
void format_size_sectors(uint32_t sectors, char *buf);

#endif // UTILS_H
