/*
 * Bundled theme module (PIE). Provides the sprite themes that used to live in
 * the core: ZELDA "FAIRY" (animated fairy cursor) and, for Mario, "COIN" and
 * "YOSHI" (the original brick-floor + walking-Yoshi look). The core keeps only
 * color-only Default/Fallback themes; this module restores the sprites without
 * costing the 40K core any flash.
 *
 * It carries its own blitter (the core's gui_draw_asset was stripped) and reads
 * the framebuffer + OFW tileset/palette through the theme_host_api_t. Loaded by
 * the chainloader's PIE loader (mod_load_theme); init_module registers the
 * OFW-appropriate theme(s).
 */
#include "system/module.h"
#include "ui/theme.h"
#include "assets.h"          /* ASSET_FLAG_* + ASSET_* recipes (via assets_gen.h) */
#include <stdint.h>
#include <stdbool.h>

MODULE_HEADER;

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define EXTFLASH_BASE 0x90000000UL
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const theme_host_api_t *g_host;

/* Self-contained sprite blitter — mirrors the core's former gui_draw_asset_internal,
 * reading the host's current framebuffer + OFW palette/tileset. Omits the 1bpp
 * font path (theme sprites are tile graphics, never glyphs). */
static void blit(int x, int y, const uint8_t *recipe, bool transparent, bool flip_h) {
    if (!recipe) return;
    uint16_t *framebuffer = g_host->framebuffer();
    const uint16_t *palette = g_host->palette;
    const uint8_t  *tileset = g_host->tileset;

    uint8_t dims = recipe[0];
    uint8_t flags = recipe[1];
    uint8_t w = dims >> 4;
    uint8_t h = dims & 0x0F;
    const uint8_t *tile_data = recipe + 3;

    if (flags & ASSET_FLAG_RAW_PIXELS) {
        if (w == 0 || h == 0) return;   /* 1bpp font — unused here */
        const uint8_t *pixels = recipe + 4;
        for (int py = 0; py < h * 16; py++) {
            int fb_y = y + py;
            if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
            uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
            for (int px = 0; px < w * 16; px++) {
                int fb_x = x + px;
                if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;
                int src_x = flip_h ? (w * 16 - 1 - px) : px;
                uint8_t color_idx = pixels[py * w * 16 + src_x];
                if (transparent && color_idx == 0) continue;
                fb_row[fb_x] = palette[color_idx];
            }
        }
    } else if (flags & ASSET_FLAG_NES_META) {
        uint32_t chr_offset = *(const uint32_t *)tile_data;
        const uint8_t *chr = (const uint8_t *)(EXTFLASH_BASE + chr_offset);
        const uint8_t *frame_tiles = tile_data + 4;
        for (int i = 0; i < 8; i++) {
            uint16_t t_id = (flags & ASSET_FLAG_EXTENDED_TILE)
                          ? ((const uint16_t *)frame_tiles)[i] : frame_tiles[i];
            if (t_id == 0xFC) continue;
            int draw_tx = flip_h ? (1 - (i % 2)) : (i % 2);
            int dx = draw_tx * 8;
            int dy = (i / 2) * 8;
            const uint8_t *tile_bytes = chr + (t_id & 0xFF) * 16;
            for (int py = 0; py < 8; py++) {
                int fb_y = y + dy + py;
                if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
                uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
                uint8_t low_b = tile_bytes[py];
                uint8_t high_b = tile_bytes[py + 8];
                for (int px = 0; px < 8; px++) {
                    int fb_x = x + dx + px;
                    if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;
                    int src_x = flip_h ? (7 - px) : px;
                    uint8_t bit0 = (low_b >> (7 - src_x)) & 1;
                    uint8_t bit1 = (high_b >> (7 - src_x)) & 1;
                    uint8_t color_idx = (bit1 << 1) | bit0;
                    if (transparent && color_idx == 0) continue;
                    fb_row[fb_x] = palette[80 + color_idx];   /* NES sub-palette */
                }
            }
        }
    } else {
        /* Zelda quadrants / Mario 16x16 tiles, indexed into the tileset. */
        for (int ty = 0; ty < h; ty++) {
            for (int tx = 0; tx < w; tx++) {
                int draw_tx = flip_h ? (w - 1 - tx) : tx;
                uint16_t tile_idx = (flags & ASSET_FLAG_EXTENDED_TILE)
                                  ? ((const uint16_t *)tile_data)[ty * w + tx]
                                  : tile_data[ty * w + tx];
                if (tile_idx == 0xFFFF) continue;
                if ((tile_idx & 0x0FFF) >= 256) continue;   /* tileset holds 256 tiles; guard OOB */
                const uint8_t *tile_ptr = tileset + (tile_idx & 0x0FFF) * 256;

                if (flags & ASSET_FLAG_ZELDA_QUAD) {
                    int quad_map[4] = {0, 1, 2, 3};
                    if (flip_h) { quad_map[0]=2; quad_map[2]=0; quad_map[1]=3; quad_map[3]=1; }
                    int start_q = 0, end_q = 4;
                    if (flags & ASSET_FLAG_ZELDA_VERT) {
                        if ((tile_idx >> 12) == 0) { start_q = 0; end_q = 2; }
                        else                       { start_q = 2; end_q = 4; }
                    }
                    for (int q = start_q; q < end_q; q++) {
                        int src_q = quad_map[q];
                        int dx = (flags & ASSET_FLAG_ZELDA_VERT) ? 0 : ((q / 2) * 8);
                        int dy = (q % 2) * 8;
                        const uint8_t *q_pixels = tile_ptr + src_q * 64;
                        for (int py = 0; py < 8; py++) {
                            int fb_y = y + ty * 16 + dy + py;
                            if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
                            uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
                            for (int px = 0; px < 8; px++) {
                                int fb_x = x + draw_tx * 16 + dx + px;
                                if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;
                                int src_x = flip_h ? (7 - px) : px;
                                uint8_t color_idx = q_pixels[py * 8 + src_x];
                                if (transparent && (color_idx == 0 || color_idx >= 80)) continue;
                                fb_row[fb_x] = palette[color_idx];
                            }
                        }
                    }
                } else {
                    for (int py = 0; py < 16; py++) {
                        int fb_y = y + ty * 16 + py;
                        if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
                        uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
                        for (int px = 0; px < 16; px++) {
                            int fb_x = x + draw_tx * 16 + px;
                            if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;
                            int src_x = flip_h ? (15 - px) : px;
                            uint8_t color_idx = tile_ptr[py * 16 + src_x];
                            if (transparent && (color_idx == 0 || color_idx >= 80)) continue;
                            fb_row[fb_x] = palette[color_idx];
                        }
                    }
                }
            }
        }
    }
}

/* ----- ZELDA: green/gold + animated fairy cursor ----- */
static void zelda_selector(int x, int y, uint32_t tick) {
    static const uint8_t *const fairy[] = {
        ASSET_ZELDA_ENTITIES_FAIRY_LEFT, ASSET_ZELDA_ENTITIES_FAIRY_RIGHT };
    blit(x + 6, y - 4, fairy[(tick / 150) % 2], true, false);
}
static theme_driver_t zelda_theme = {
    .name = "FAIRY",
    .colors = { RGB565(0x1E,0x60,0x30), RGB565(0xFF,0xFF,0xFF),
                RGB565(0xE5,0xB8,0x3B), RGB565(0xE5,0xB8,0x3B), RGB565(0x0F,0x30,0x18) },
    .draw_selector = zelda_selector,
};

/* ----- MARIO: red/gold + spinning coin cursor ----- */
static void coin_selector(int x, int y, uint32_t tick) {
    static const uint8_t *const coin[] = {
        ASSET_MARIO_COIN_FRAME_1, ASSET_MARIO_COIN_FRAME_2,
        ASSET_MARIO_COIN_FRAME_3, ASSET_MARIO_COIN_FRAME_4 };
    blit(x + 2, y - 4, coin[(tick / 100) % 4], true, false);
}
static theme_driver_t mario_theme = {
    .name = "COIN",
    .colors = { RGB565(0xA0,0x20,0x18), RGB565(0xFF,0xFF,0xFF),
                RGB565(0xE5,0xB8,0x3B), RGB565(0xE5,0xB8,0x3B), RGB565(0x40,0x0C,0x08) },
    .draw_selector = coin_selector,
};

/* ----- YOSHI: the original Mario look (blue, brick floor + walking Yoshi + clouds) ----- */
static void yoshi_footer(uint32_t tick) {
    for (int x = 0; x < SCREEN_WIDTH; x += 16) {
        blit(x, 219, ASSET_MARIO_BRICK_FLOOR, false, false);
        blit(x, 235, ASSET_MARIO_BRICK_FLOOR, false, false);
    }
    int range = SCREEN_WIDTH - 32;
    int t = (tick / 50) % (2 * range);
    int x = (t < range) ? t : (2 * range - t);
    static const uint8_t *const yoshi[] = {
        ASSET_MARIO_YOSHI_GREEN_WALKING_1, ASSET_MARIO_YOSHI_GREEN_WALKING_2,
        ASSET_MARIO_YOSHI_GREEN_WALKING_3 };
    blit(x, 200, yoshi[(tick / 150) % 3], true, (t >= range));
}
static void yoshi_background(uint32_t tick) {
    static const int8_t st[] = { 0, 1, 2, 2, 3, 2, 2, 1, 0, -1, -2, -2, -3, -2, -2, -1 };
    static const struct { uint16_t x, y, d, o; } c[] = {
        { 88, 40, 120, 0 }, { 268, 60, 180, 4 }, { 178, 25, 150, 8 }, { 358, 48, 200, 12 } };
    for (int i = 0; i < 4; i++) {
        int cx = (c[i].x + 368 - ((tick / c[i].d) % 368)) % 368 - 48;
        blit(cx, c[i].y + st[((tick / 250) + c[i].o) % 16], ASSET_MARIO_CLOUDS_LARGE, true, false);
    }
}
static theme_driver_t yoshi_theme = {
    .name = "YOSHI",
    .colors = { RGB565(0x5F,0x73,0xFF), RGB565(0x00,0x00,0x00),
                RGB565(0x00,0x50,0xA0), RGB565(0x00,0x00,0x00), RGB565(0x00,0x1A,0x50) },
    .draw_selector   = coin_selector,
    .draw_footer     = yoshi_footer,
    .draw_background = yoshi_background,
};

void init_module(const theme_host_api_t *host) {
    g_host = host;
    if (host->ofw == THEME_OFW_ZELDA) {
        host->register_theme(&zelda_theme);
    } else if (host->ofw == THEME_OFW_MARIO) {
        host->register_theme(&mario_theme);
        host->register_theme(&yoshi_theme);
    }
}
