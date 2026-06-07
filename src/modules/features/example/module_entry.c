/*
 * Example feature module (PIE, transient) -- proves the feature-module framework.
 *
 * Its header manifest (set via -D in the Makefile) declares a Tools menu entry
 * "Example"; the core discovers it at boot (feature_discover) and lists it under Tools
 * after Partition Viewer, with zero core code naming this module. Selecting it
 * transiently loads this module and calls run(), which describes a simple list view and
 * hands it to the in-core UI service (host->run_view) -- so it gets the divider, themed
 * selector, scroll + wrap, and header/footer for free, with no hand-rolled drawing. The
 * service runs until B; then run() returns (the core reclaims the pool slot). This is the
 * template real feature modules (e.g. the MP3 player) follow, and a second proof that the
 * UI service seam is reusable.
 */
#include "system/module.h"
#include "system/feature.h"
#include "system/input.h"   /* INPUT_B */

MODULE_HEADER;

/* A tiny fixed menu, rendered by the shared service (UI_VIEW_MENU = list only). on_select is
 * NULL, so this is a back-only demo screen: B returns. */
static const char *const ROWS[] = {
    "Example feature module",
    "Rendered by the UI service",
    "Press B to exit",
};
static const char *example_label(int i) { return ROWS[i]; }

static void run(const feature_host_t *h, const char *path) {
    (void)path;   /* NULL for a menu launch; a file path for a file-type launch */
    ui_view_desc_t desc = {
        .kind            = UI_VIEW_MENU,
        .title           = "Example",
        .footer          = "B back",
        .header_color    = 0,
        .count           = (int)(sizeof(ROWS) / sizeof(ROWS[0])),
        .get_label       = example_label,
        .on_select       = 0,
        .draw_right_pane = 0,
    };
    h->run_view(&desc);
}

void init_module(const feature_host_t *host, feature_api_t *out) {
    (void)host;   /* the host is handed to run() per call */
    out->run = run;
}
