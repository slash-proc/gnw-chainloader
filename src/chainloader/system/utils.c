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

/* Bounded string ops — never write past dst[cap-1], always NUL-terminate. A source
 * too long for the buffer TRUNCATES instead of overflowing, so no language pack or
 * module string (pure data) can smash a fixed UI buffer and crash the device. This is
 * the safety net behind STABILITY IS LAW: bad/oversized data degrades to a clipped
 * label, never a freeze. */
void str_lcpy(char *dst, int cap, const char *src) {
    int n = 0;
    if (cap <= 0) return;
    while (n < cap - 1 && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

void str_lcat(char *dst, int cap, const char *src) {
    int n = 0;
    if (cap <= 0) return;
    while (n < cap - 1 && dst[n]) n++;          /* bounded walk to current end */
    while (n < cap - 1 && *src) dst[n++] = *src++;
    dst[n] = '\0';
}

void str_fmt1_int(char *dst, int cap, const char *tmpl, int n) {
    char num[16];
    int_to_str(n, num);
    int i = 0, done = 0;
    while (*tmpl && i < cap - 1) {
        if (!done && tmpl[0] == '%' && tmpl[1] == 'd') {
            for (const char *p = num; *p && i < cap - 1; p++) dst[i++] = *p;
            tmpl += 2;
            done = 1;
        } else {
            dst[i++] = *tmpl++;
        }
    }
    if (cap > 0) dst[i] = '\0';
}

void str_fmt1_str(char *dst, int cap, const char *tmpl, const char *s) {
    int i = 0, done = 0;
    while (*tmpl && i < cap - 1) {
        if (!done && tmpl[0] == '%' && tmpl[1] == 's') {
            for (const char *p = s; *p && i < cap - 1; p++) dst[i++] = *p;
            tmpl += 2;
            done = 1;
        } else {
            dst[i++] = *tmpl++;
        }
    }
    if (cap > 0) dst[i] = '\0';
}

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

void format_size_sectors(uint32_t sectors, char *buf) {
    /* 2 sectors == 1 KiB. Work in KiB so the intermediate fits a uint32_t even
     * for multi-GB cards (an 8 GB card is ~15.6M sectors -> ~7.8M KiB). */
    uint32_t kb = sectors / 2u;
    if (kb >= 1024 * 1024) {
        format_unit(kb, 1024 * 1024, "GB", buf);
    } else if (kb >= 1024) {
        format_unit(kb, 1024, "MB", buf);
    } else {
        int_to_str(kb, buf);
        strcat(buf, "KB");
    }
}
