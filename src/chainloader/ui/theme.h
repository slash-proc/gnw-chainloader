#ifndef THEME_H
#define THEME_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Theme model (deliberately minimal — see DESIGN/ACTIVE_WORK; expected to grow
 * if the project gains traction).
 *
 * Two themes live in the 40K core, COLOR-ONLY (no sprite/blitter code):
 *   slot 0  DEFAULT   the loaded OFW's signature colors (Zelda green+gold,
 *                     Mario red+gold, or neutral when no OFW is detected)
 *   slot 1  FALLBACK  a neutral palette, for when you don't want the OFW look
 *
 * Sprite themes (fairy/coin/Yoshi/custom) are loadable PIE theme MODULES that
 * register into slots 2.. via theme_register(); each brings its own colors and
 * optional sprite hooks. A selected module slot whose module isn't present
 * falls back to slot 0. Selection is persisted per-OFW (see boot_magic.h).
 *
 * Selector order shown to the user: DEFAULT -> <modules> -> FALLBACK.
 */

typedef struct { uint16_t bg, fg, accent, border, footer; } theme_colors_t;

typedef struct theme_driver {
    const char *name;          /* selector label (uppercase; GUI font is 0x20-0x5A) */
    theme_colors_t colors;
    /* Optional sprite hooks; a NULL hook uses the core's plain look for that
     * element (geometric cursor / bare footer / no animation). They blit from
     * the OFW tileset, so they run only when assets are loaded. */
    void (*draw_selector)(int x, int y, uint32_t tick);  /* menu cursor at row (x,y) */
    void (*draw_footer)(uint32_t tick);                  /* footer decoration over the solid bar */
    void (*draw_background)(uint32_t tick);              /* idle background animation */
} theme_driver_t;

/* A loaded theme module registers its theme(s) here (slots 2,3,...). */
void theme_register(theme_driver_t *t);

/* Which OFW is loaded (decoupled from board.h so a theme module needn't include
 * it). A module registers only themes matching this — the other OFW's sprite
 * tiles aren't in the tileset. */
typedef enum { THEME_OFW_NONE = 0, THEME_OFW_MARIO, THEME_OFW_ZELDA } theme_ofw_t;

/* Services the core gives a theme module so it can blit sprites without pulling
 * in core internals. The module reads framebuffer()/palette/tileset and writes
 * pixels itself (its own blitter — the core's was stripped to save flash). */
typedef struct {
    uint32_t (*get_tick)(void);
    void (*register_theme)(theme_driver_t *t);
    theme_ofw_t ofw;
    uint16_t *(*framebuffer)(void);   /* current 320x240 RGB565 back buffer (swaps each frame) */
    const uint16_t *palette;          /* dynamic_palette (OFW colors) */
    const uint8_t  *tileset;          /* dynamic_tileset (OFW sprite tiles) */
} theme_host_api_t;

/* Load the bundled theme module (if present) and re-apply the persisted slot so
 * a saved module theme restores on boot. Safe no-op if no module is found. */
void theme_modules_init(void);

/* Loader hook (implemented in system/loader.c alongside mod_load): load a PIE
 * theme module and call its init_module(const theme_host_api_t*). */
bool mod_load_theme(const char *path, const theme_host_api_t *host);

#define THEME_SLOT_DEFAULT     0
#define THEME_SLOT_FALLBACK    1
#define THEME_SLOT_MODULE_BASE 2

extern uint8_t ui_theme_slot;                 /* current slot for the active OFW */
void ui_set_theme_slot(uint8_t slot);         /* apply a slot (selector + boot restore) */
void ui_theme_cycle(int dir);                 /* step within the selector order (dir -1/+1) */
int  ui_theme_slot_count(void);               /* 2 + number of registered module themes */
const char *ui_theme_slot_name(uint8_t slot); /* selector label for a slot */

/* Render-site helpers that own the "active module sprite vs plain fallback"
 * decision (and the assets-loaded gate). draw_selector returns true if a module
 * drew the cursor; otherwise the caller draws the geometric one. footer/
 * background paint decoration over the core's already-drawn solid footer bar. */
bool theme_draw_selector(int x, int y, uint32_t tick);
void theme_draw_footer(uint32_t tick);
void theme_draw_background(uint32_t tick);

#endif // THEME_H
