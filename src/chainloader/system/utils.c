#include "utils.h"
#include <string.h>

void int_to_str(int val, char *buf) {
    char temp[16];
    int i = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    if (val < 0) {
        *buf++ = '-';
        val = -val;
    }
    while (val > 0) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = 0; j < i; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[i] = '\0';
}

#ifdef ENABLE_EXTENDED_UTILS
void int_to_str_w(int val, char *buf, int width) {
    char temp[16];
    int i = 0;
    int is_neg = 0;
    if (val == 0) {
        while (i < width) {
            temp[i++] = '0';
        }
    } else {
        if (val < 0) {
            is_neg = 1;
            val = -val;
        }
        while (val > 0 || i < width) {
            temp[i++] = '0' + (val % 10);
            val /= 10;
        }
        if (is_neg) {
            temp[i++] = '-';
        }
    }
    for (int j = 0; j < i; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[i] = '\0';
}
#endif

void hex_to_str(uint32_t val, char *buf, int width) {
    char temp[16];
    int i = 0;
    while (i < width || val > 0) {
        uint32_t digit = val & 0xF;
        temp[i++] = (digit < 10) ? ('0' + digit) : ('A' + (digit - 10));
        val >>= 4;
    }
    for (int j = 0; j < i; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[i] = '\0';
}

static void format_unit(uint32_t bytes, uint32_t div, const char *unit, char *buf) {
    uint32_t whole = bytes / div;
    uint32_t rem = bytes % div;
    if (rem == 0) {
        int_to_str(whole, buf);
    } else {
        uint32_t frac = ((rem * 10) + div / 2) / div;
        if (frac >= 10) {
            whole++;
            frac = 0;
        }
        char *p = buf;
        int_to_str(whole, p);
        p += strlen(p);
        *p++ = '.';
        int_to_str(frac, p);
    }
    strcat(buf, unit);
}

void format_size(uint32_t bytes, char *buf) {
    if (bytes >= 1024 * 1024) {
        format_unit(bytes, 1024 * 1024, "MB", buf);
    } else if (bytes >= 1024) {
        format_unit(bytes, 1024, "KB", buf);
    } else {
        int_to_str(bytes, buf);
        strcat(buf, "B");
    }
}
