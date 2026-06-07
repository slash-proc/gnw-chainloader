#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

void int_to_str(int val, char *buf);
#ifdef ENABLE_EXTENDED_UTILS
void int_to_str_w(int val, char *buf, int width);
#endif
void hex_to_str(uint32_t val, char *buf, int width);
void format_size(uint32_t bytes, char *buf);

#endif // UTILS_H
