#include "utils.h"
#include <string.h>

char *int_to_str(int val, char *buf) {
    char temp[12];
    char *p = temp;
    if (val < 0) {
        *buf++ = '-';
        val = -val;
    }
    do {
        *p++ = '0' + (val % 10);
        val /= 10;
    } while (val);
    while (p > temp) {
        *buf++ = *--p;
    }
    *buf = '\0';
    return buf;
}

#ifdef ENABLE_EXTENDED_UTILS
void int_to_str_w(int val, char *buf, int width) {
    char temp[16];
    int i = 0;
    int is_neg = 0;
    if (val < 0) {
        is_neg = 1;
        val = -val;
    }
    do {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0 || i < width);
    if (is_neg) {
        temp[i++] = '-';
    }
    while (i > 0) {
        *buf++ = temp[--i];
    }
    *buf = '\0';
}
#endif

/* Bounded string ops — never write past dst[cap-1], always NUL-terminate. A source
 * too long for the buffer TRUNCATES instead of overflowing, so no language pack or
 * module string (pure data) can smash a fixed UI buffer and crash the device. This is
 * the safety net behind STABILITY IS LAW: bad/oversized data degrades to a clipped
 * label, never a freeze. */
void str_lcpy(char *dst, int cap, const char *src) {
    if (cap <= 0) return;
    while (--cap > 0 && *src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

void str_lcat(char *dst, int cap, const char *src) {
    if (cap <= 0) return;
    while (--cap > 0 && *dst) dst++;
    while (cap > 0 && *src) {
        *dst++ = *src++;
        cap--;
    }
    *dst = '\0';
}

static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

bool ext_list_match(const char *list, const char *ext) {
    if (!list || !ext) return false;
    while (*list) {
        const char *b = ext;
        while (*list && *list != ',') {
            if (to_lower(*list) != to_lower(*b)) break;
            list++;
            if (*b) b++;
        }
        if (*b == '\0' && (*list == '\0' || *list == ',')) return true;
        while (*list && *list != ',') list++;
        if (*list == ',') list++;
    }
    return false;
}

static void str_fmt_impl(char *dst, int cap, const char *tmpl, char fmt_char, const char *val_str) {
    if (cap <= 0) return;
    char *end = dst + cap - 1;
    bool done = false;
    while (*tmpl && dst < end) {
        if (!done && tmpl[0] == '%' && tmpl[1] == fmt_char) {
            tmpl += 2;
            while (*val_str && dst < end) {
                *dst++ = *val_str++;
            }
            done = true;
        } else {
            *dst++ = *tmpl++;
        }
    }
    *dst = '\0';
}

void str_fmt1_str(char *dst, int cap, const char *tmpl, const char *s) {
    str_fmt_impl(dst, cap, tmpl, 's', s);
}

void str_fmt1_int(char *dst, int cap, const char *tmpl, int n) {
    char num[16];
    int_to_str(n, num);
    str_fmt_impl(dst, cap, tmpl, 'd', num);
}

void hex_to_str(uint32_t val, char *buf, int width) {
    char temp[9];
    char *p = temp;
    while (width-- > 0 || val) {
        uint32_t digit = val & 0xF;
        *p++ = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
        val >>= 4;
    }
    while (p > temp) {
        *buf++ = *--p;
    }
    *buf = '\0';
}

static void format_unit(uint32_t bytes, uint32_t div, const char *unit, char *buf) {
    uint32_t whole = bytes / div;
    uint32_t rem = bytes % div;
    char *p = buf;
    if (rem == 0) {
        p = int_to_str(whole, p);
    } else {
        uint32_t frac = ((rem * 10) + div / 2) / div;
        if (frac >= 10) {
            whole++;
            frac = 0;
        }
        p = int_to_str(whole, p);
        *p++ = '.';
        p = int_to_str(frac, p);
    }
    while ((*p++ = *unit++));
}

void format_size(uint32_t bytes, char *buf) {
    if (bytes >= 1024 * 1024) {
        format_unit(bytes, 1024 * 1024, "MB", buf);
    } else if (bytes >= 1024) {
        format_unit(bytes, 1024, "KB", buf);
    } else {
        char *p = int_to_str(bytes, buf);
        *p++ = 'B';
        *p = '\0';
    }
}

void format_size_sectors(uint32_t sectors, char *buf) {
    uint32_t kb = sectors / 2u;
    if (kb >= 1024 * 1024) {
        format_unit(kb, 1024 * 1024, "GB", buf);
    } else if (kb >= 1024) {
        format_unit(kb, 1024, "MB", buf);
    } else {
        char *p = int_to_str(kb, buf);
        *p++ = 'K';
        *p++ = 'B';
        *p = '\0';
    }
}
