/*
 * Language manager — see i18n.h.
 *
 * Only English is baked into the firmware (the in-core ASCII font + string
 * table). Every other language is DATA discovered on the device filesystem: at
 * boot the manager scans /i18n/<code>.lang on LittleFS and reads each pack's
 * self-described code + endonym + script straight from its header to build the
 * runtime language list. So a language never compiled into the firmware appears
 * automatically once its pack is present. The active language is persisted by
 * code (/i18n/.active), so the list can grow or shrink without disturbing it.
 *
 * .lang layout (little-endian, magic 'LNG2', from scripts/build/cook_lang.py):
 *   u32 magic; u16 abi; u16 str_count; u32 version;
 *   char script[16]; char code[16]; char endonym[32];   (76-byte header)
 *   u32 offsets[str_count]  (blob offset, or 0xFFFFFFFF = English fallback)
 *   u8  blob[]              (NUL-terminated UTF-8 strings)
 */
#include "ui/i18n.h"
#include "ui/strings.h"
#include "ui/font_ext.h"
#include "storage/vfs.h"
#include "storage/partition.h"
#include <string.h>

#define LANG_MAGIC      0x32474E4Cu    /* 'L','N','G','2' */
#define LANG_FALLBACK   0xFFFFFFFFu
#define LANG_MAX_BYTES  (8u * 1024u)
#define LANG_HDR_SIZE   76u
#define I18N_MAX        32             /* English + up to 31 discovered languages */

/* Shared Latin font — the always-on BASE glyph source, so accented Latin renders
 * in every language (and Latin filenames render in English). Loaded once. */
#define I18N_LATIN_FONT "/i18n/fonts/latin.fnt"

typedef struct {
    char code[16];      /* locale code, e.g. "de_DE" (reserved "en" = in-core English) */
    char endonym[32];   /* selector label in the language's own script */
    char script[16];    /* font script -> /i18n/fonts/<script>.fnt ("latin" = base) */
} i18n_entry_t;

static uint8_t      g_lang_buf[LANG_MAX_BYTES] __attribute__((aligned(4)));
static const char  *g_table[STR_COUNT];   /* pointers into g_lang_buf, NULL = English */
static i18n_entry_t g_langs[I18N_MAX];     /* [0] = English; rest discovered, sorted by code */
static int          g_lang_count;
static uint8_t      g_current;            /* index into g_langs */
static uint32_t     g_lang_sz;
static bool         g_base_ok;            /* the always-on base latin.fnt is loaded */
static bool         g_rtl;                /* active language is right-to-left (arabic script) */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Validate a pack resident in `buf` and install it as the active string table.
 * Rejects a magic / ABI / count mismatch so a stale pack can never desync the UI. */
static bool lang_parse(const uint8_t *buf, uint32_t sz) {
    if (sz < LANG_HDR_SIZE || rd32(buf) != LANG_MAGIC) return false;
    if (rd16(buf + 4) != STRINGS_ABI_VERSION) return false;
    if (rd16(buf + 6) != (uint16_t)STR_COUNT) return false;

    const uint8_t *offs = buf + LANG_HDR_SIZE;
    uint32_t blob_off = LANG_HDR_SIZE + 4u * (uint32_t)STR_COUNT;
    if (blob_off > sz) return false;

    for (uint32_t i = 0; i < (uint32_t)STR_COUNT; i++) {
        uint32_t o = rd32(offs + 4u * i);
        g_table[i] = (o != LANG_FALLBACK && blob_off + o < sz)
                   ? (const char *)(buf + blob_off + o)
                   : 0;   /* fall back to English for this id */
    }
    strings_set_active(g_table);
    return true;
}

static void to_english(void) {
    strings_set_active(0);
    font_ext_close();
}

/* Build "/i18n/<code>.lang" and "/i18n/fonts/<script>.fnt" into caller buffers. */
static void lang_paths(const i18n_entry_t *e, char *lp, char *fp) {
    strcpy(lp, "/i18n/");       strcat(lp, e->code);   strcat(lp, ".lang");
    strcpy(fp, "/i18n/fonts/");  strcat(fp, e->script); strcat(fp, ".fnt");
}

bool i18n_set(uint8_t idx) {
    if (idx >= (uint8_t)g_lang_count) idx = 0;
    const i18n_entry_t *e = &g_langs[idx];

    if (idx == 0) {                /* English (index 0): in-core, no files needed */
        to_english();
        g_current = 0;
        g_lang_sz = 0;
        g_rtl = false;
        return true;
    }

    char lp[64], fp[64];
    lang_paths(e, lp, fp);
    uint32_t sz = 0;
    bool ok = (vfs_read_lang_lfs(lp, g_lang_buf, sizeof(g_lang_buf), &sz, STRINGS_ABI_VERSION) == 0 &&
               lang_parse(g_lang_buf, sz));
    if (ok) {
        /* A language with no usable font would render as '?' everywhere — don't
         * load it; fall back to clean English. Latin scripts ride the always-on
         * base font (no per-switch reload — that was the lag); other scripts need
         * their own .fnt to be present. */
        if (strcmp(e->script, "latin") == 0) { font_ext_close(); ok = g_base_ok; }
        else                                   ok = font_ext_open(fp);
    }
    if (ok) {
        g_current = idx;
        g_lang_sz = sz;
        g_rtl = (strcmp(e->script, "arabic") == 0);   /* RTL scripts mirror the UI */
        return true;
    }

    to_english();                  /* pack/font missing or corrupt -> graceful English */
    g_current = 0;
    g_lang_sz = 0;
    g_rtl = false;
    return false;
}

/* Discovery: append a filesystem-discovered pack to the runtime list. */
static void discover_cb(const char *code, const char *endonym, const char *script) {
    if (g_lang_count >= I18N_MAX) return;
    if (strcmp(code, "en") == 0) return;                /* reserved in-core sentinel (slot 0) */
    for (int i = 0; i < g_lang_count; i++)
        if (strcmp(g_langs[i].code, code) == 0) return;  /* dedup by code */
    i18n_entry_t *e = &g_langs[g_lang_count++];
    strncpy(e->code, code, sizeof(e->code) - 1);
    strncpy(e->endonym, endonym, sizeof(e->endonym) - 1);
    strncpy(e->script, script, sizeof(e->script) - 1);
}

/* Sort discovered entries (g_langs[1..]) by locale code; English stays first. */
static void sort_by_code(void) {
    for (int i = 2; i < g_lang_count; i++) {
        i18n_entry_t tmp = g_langs[i];
        int j = i - 1;
        while (j >= 1 && strcmp(g_langs[j].code, tmp.code) > 0) {
            g_langs[j + 1] = g_langs[j];
            j--;
        }
        g_langs[j + 1] = tmp;
    }
}

static void set_english_entry(void) {
    memset(g_langs, 0, sizeof(g_langs));
    strcpy(g_langs[0].code, "en");      /* reserved sentinel code (never a real pack) */
    strcpy(g_langs[0].endonym, "English");
    strcpy(g_langs[0].script, "latin");
    g_lang_count = 1;
}

static int find_code(const char *code) {
    for (int i = 0; i < g_lang_count; i++)
        if (strcmp(g_langs[i].code, code) == 0) return i;
    return 0;   /* English */
}

/* A real English pack (en_US / en_UK / ...) was discovered. When one is present the
 * in-core "en" sentinel is HIDDEN from the live selector (it stays the silent tr()
 * fallback): the user picks a proper mixed-case English, not the all-caps baked one.
 * With no English pack the sentinel is the only English and stays visible. */
static bool english_pack_present(void) {
    for (int i = 1; i < g_lang_count; i++)
        if (strncmp(g_langs[i].code, "en_", 3) == 0) return true;
    return false;
}

/* (Re)build the runtime list: English + every valid pack on LittleFS, by code. */
static void discover_into_list(void) {
    set_english_entry();
    vfs_lfs_enum_langs(STRINGS_ABI_VERSION, discover_cb);
    sort_by_code();
}

/* Apply the persisted language (/i18n/.active). With none persisted (fresh device),
 * default to en_US, then en_UK, then the in-core English sentinel. An explicit choice
 * is persisted by code and restored verbatim. */
static void restore_active(void) {
    int idx = 0;
    bool have_persisted = false;
    char code[16];
    uint32_t sz = 0;
    if (vfs_read_file("/i18n/.active", code, sizeof(code) - 1, &sz) == 0 && sz > 0) {
        if (sz >= sizeof(code)) sz = sizeof(code) - 1;
        code[sz] = '\0';
        for (uint32_t k = 0; k < sz; k++)
            if (code[k] == '\n' || code[k] == '\r' || code[k] == ' ') { code[k] = '\0'; break; }
        idx = find_code(code);
        have_persisted = (idx != 0);   /* a real persisted language was found */
    }
    if (!have_persisted) {             /* absent/empty/missing -> en_US, then en_UK */
        idx = find_code("en_US");
        if (idx == 0) idx = find_code("en_UK");
    }
    (void)i18n_set((uint8_t)idx);
}

void i18n_init(void) {
    gui_set_ext_glyph(font_ext_glyph);   /* core resolver for now; the language module takes this over */
    g_base_ok = font_ext_open_base(I18N_LATIN_FONT);
    discover_into_list();
    restore_active();
}

/* Re-scan after the installer module added packs: keep the user's current language
 * if they're on one (its index may have shifted, so track by code), else apply the
 * persisted choice that may have just become available. */
void i18n_rediscover(void) {
    char cur[16];
    strncpy(cur, g_langs[g_current].code, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';
    bool was_english = (g_current == 0);
    g_base_ok = font_ext_open_base(I18N_LATIN_FONT);   /* the install may have just pulled it */
    discover_into_list();
    if (was_english) restore_active();
    else             (void)i18n_set((uint8_t)find_code(cur));
}

/* Persist the active language by code (/i18n/.active). The caller debounces this
 * (it loads the RW LittleFS module) so rapid cycling stays snappy. */
void i18n_persist_active(void) {
    const char *code = g_langs[g_current].code;
    vfs_lfs_write_file("/i18n/.active", code, (uint32_t)strlen(code));
}

uint8_t i18n_current(void) { return g_current; }
bool    i18n_is_rtl(void)  { return g_rtl; }

/* Selectable count: the hidden in-core sentinel doesn't count when a real English
 * pack exists (it's never reachable through the selector then). */
int i18n_count(void) {
    return english_pack_present() ? g_lang_count - 1 : g_lang_count;
}

const char *i18n_endonym(int idx) {
    if (idx < 0 || idx >= g_lang_count) return "";
    return g_langs[idx].endonym;
}

const char *i18n_code(int idx) {
    if (idx < 0 || idx >= g_lang_count) return "";
    return g_langs[idx].code;
}

/* Next/prev selectable language, wrapping. Skips index 0 (the in-core sentinel) when
 * it's hidden, so the all-caps baked English never appears once a real English pack
 * is installed; the user lands only on proper packs. */
uint8_t i18n_cycle(uint8_t cur, int dir) {
    int n = g_lang_count;
    if (n <= 0) return 0;
    int step = (dir < 0) ? -1 : 1;
    int pos = (int)cur;
    for (int i = 0; i < n; i++) {
        pos = (pos + step) % n;
        if (pos < 0) pos += n;
        if (pos == 0 && english_pack_present()) continue;   /* hidden sentinel */
        return (uint8_t)pos;
    }
    return cur;
}
