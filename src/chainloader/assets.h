#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>
#include <stdbool.h>
#include "assets_gen.h"

/* --- Asset Pack Flags --- */
#define ASSET_FLAG_ZELDA_QUAD    0x01
#define ASSET_FLAG_ZELDA_VERT    0x02
#define ASSET_FLAG_NES_META      0x04
#define ASSET_FLAG_EXTENDED_TILE 0x08
#define ASSET_FLAG_RAW_PIXELS    0x10

extern bool assets_loaded;
/* Dynamic asset buffers residing in RAM */
extern uint16_t dynamic_palette[128] __attribute__((aligned(32)));
extern uint8_t dynamic_tileset[65536] __attribute__((aligned(32)));

/**
 * Draws a sprite by its direct recipe pointer.
 */
void gui_draw_asset(int x, int y, const uint8_t *recipe, bool transparent, bool flip_h);

/**
 * Draws a sprite by its index in the asset list.
 */
void gui_draw_asset_by_idx(int x, int y, int idx, bool transparent, bool flip_h);

const uint8_t *asset_get_by_idx(int idx);
void asset_get_dims_by_idx(int idx, int *w, int *h);

#endif // ASSETS_H
