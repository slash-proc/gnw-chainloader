/*
 * Module Overview feature module (PIE, transient): a Tools-menu diagnostic listing every loaded
 * module -- where it loaded from (XIP in place from flash vs a full RAM copy), its pool footprint,
 * whether it is still resident, and its load model. Reads the loader's module registry through the
 * feature host (mod_count / mod_get), and renders through the in-core UI service (host->run_view):
 * it describes a dual-pane view (left module-name list + right detail pane) and the service draws
 * the divider, themed selector, scroll + wrap, and header/footer for free, matching the active
 * theme and mirroring for RTL. English-only literals (a diagnostic), so no strings/i18n. This is
 * the Module Overview rebuilt AS a module (it used to live in-core but was disabled to hold the
 * 40 KB stub ceiling) -- now it runs from the pool, on the shared UI service.
 */
#include "system/module.h"
#include "system/feature.h"
#include "system/input.h"
#include <string.h>

MODULE_HEADER;

static const char HEX[] = "0123456789ABCDEF";

/* "0x" + 8 hex digits, NUL-terminated, into out[11]. */
static void hex8(uint32_t v, char *out) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = HEX[(v >> ((7 - i) * 4)) & 0xFu];
    out[10] = '\0';
}

/* Unsigned decimal into out; returns the NUL position so a unit can be appended. */
static char *u2s(uint32_t v, char *out) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return out + n;
}

/* "<KB> KB" (rounded) for >= 1 KiB, else "<bytes> B". */
static void size_str(uint32_t b, char *out) {
    char *p = (b >= 1024u) ? u2s((b + 512u) / 1024u, out) : u2s(b, out);
    strcpy(p, (b >= 1024u) ? " KB" : " B");
}

/* The running host, stashed so the descriptor callbacks (which take no host arg) can reach the
 * loader registry. The module is single-instance + transient, so a file-scope pointer is fine. */
static const feature_host_t *g_host;

/* Left list: the nth module's basename. One static buffer is sufficient -- the list draws rows
 * sequentially, using each label before fetching the next. */
static const char *modview_label(int i) {
    static char name[44];
    mod_view_t m;
    if (!g_host->mod_get(i, &m)) return "";
    strcpy(name, m.name);
    return name;
}

/* Right pane: detail for the selected module, via the shared label/value pane rows. */
static void modview_pane(int sel, uint32_t tick) {
    (void)tick;
    mod_view_t m;
    if (!g_host->mod_get(sel, &m)) return;
    char buf[24];

    if (m.flash) hex8(m.flash, buf); else strcpy(buf, "RAM copy");
    g_host->pane_row(0, "SOURCE", buf, false, 0);

    size_str(m.ram, buf);
    g_host->pane_row(1, "POOL RAM", buf, false, 0);

    g_host->pane_row(2, "STATE", m.resident ? "resident" : "freed", false, 0);
    g_host->pane_row(3, "MODEL", (m.flags & 1u) ? "transient" : "resident", false, 0);
}

static void run(const feature_host_t *host, const char *path) {
    (void)path;
    g_host = host;
    ui_view_desc_t desc = {
        .kind            = UI_VIEW_DUALPANE,
        .title           = "Module Overview",
        .footer          = "UP/DOWN select   B back",
        .header_color    = 0,
        .count           = host->mod_count(),
        .get_label       = modview_label,
        .on_select       = NULL,                 /* a read-only diagnostic: A does nothing */
        .draw_right_pane = modview_pane,
    };
    host->run_view(&desc);
}

void init_module(const feature_host_t *host, feature_api_t *out) {
    (void)host;   /* host arrives per run() call */
    out->run = run;
}
