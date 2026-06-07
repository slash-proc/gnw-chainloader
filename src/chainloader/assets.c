#include "assets.h"
#include "gui.h"
#include "ui.h"
#include "ui/gui_font.h"
#include "board.h"
#include "main.h"
#include "../common/memory_map.h"
#include "LzmaDec.h"
#include "../common/stub_services.h"
#include <string.h>

extern uint16_t *framebuffer;

#define LZMA_HEAP_RESET ((void*)1)

/* LZMA temporary workspace - uses back buffer as scratch space during boot */
extern uint16_t fb_b[320 * 240];
static uint16_t *get_scratch_buffer(void) {
    return fb_b;
}

/* LZMA Allocator */
void *lzma_alloc(ISzAllocPtr p, size_t size) {
    static uint8_t lzma_heap[49152] __attribute__((aligned(4)));
    static size_t used = 0;
    if (p == LZMA_HEAP_RESET) { used = 0; return NULL; }
    if (used + size > sizeof(lzma_heap)) return NULL;
    void *res = &lzma_heap[used];
    used += (size + 3) & ~3;
    return res;
}
void lzma_free(ISzAllocPtr p, void *address) { (void)p; (void)address; }
ISzAlloc lzma_allocator = { lzma_alloc, lzma_free };

/* Decompress via the stub's LZMA decoder (published at STUB_SERVICES_ADDR) so
 * the app doesn't link its own copy. If the table isn't present (layout drift),
 * fail gracefully — the caller treats non-SZ_OK as "assets unavailable". */
static SRes app_lzma_decode(Byte *dest, SizeT *destLen, const Byte *src, SizeT *srcLen,
        const Byte *propData, unsigned propSize, ELzmaFinishMode finishMode,
        ELzmaStatus *status, ISzAllocPtr alloc) {
    if (STUB_SERVICES->magic != STUB_SERVICES_MAGIC) return SZ_ERROR_FAIL;
    return STUB_SERVICES->lzma_decode(dest, destLen, src, srcLen, propData, propSize,
                                      finishMode, status, alloc);
}

static void gui_draw_asset_internal(int x, int y, const uint8_t *recipe, bool transparent, bool flip_h) {
    uint8_t dims = recipe[0];
    uint8_t flags = recipe[1];
    uint8_t w = dims >> 4;
    uint8_t h = dims & 0x0F;
    const uint8_t *tile_data = recipe + 3;

    if (flags & ASSET_FLAG_RAW_PIXELS) {
        const uint8_t *pixels = recipe + 4;
        if (w == 0 || h == 0) { // Special case: 8x8 font character (1bpp)
            for (int py = 0; py < 8; py++) {
                int fb_y = y + py;
                if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
                uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
                uint8_t row_byte = pixels[py];
                for (int px = 0; px < 8; px++) {
                    int fb_x = x + px;
                    if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;
                    int src_x = flip_h ? (7 - px) : px;
                    if ((row_byte >> src_x) & 1) {
                        fb_row[fb_x] = gui_fg_color;
                    } else if (!transparent) {
                        fb_row[fb_x] = gui_bg_color;
                    }
                }
            }
        } else { // Generic 8bpp pixels
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
                    fb_row[fb_x] = dynamic_palette[color_idx];
                }
            }
        }
    } else if (flags & ASSET_FLAG_NES_META) {
        uint32_t chr_offset = *(const uint32_t *)tile_data;
        const uint8_t *chr = (const uint8_t *)(EXTFLASH_BASE + chr_offset);
        const uint8_t *frame_tiles = tile_data + 4;
        
        for (int i = 0; i < 8; i++) {
            uint16_t t_id;
            if (flags & ASSET_FLAG_EXTENDED_TILE) t_id = ((const uint16_t*)frame_tiles)[i];
            else t_id = frame_tiles[i];

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
                    fb_row[fb_x] = dynamic_palette[80 + color_idx]; // Use NES sub-palette
                }
            }
        }
    } else {
        // Shared logic for Zelda (quadrants) and Mario (full tiles)
        for (int ty = 0; ty < h; ty++) {
            for (int tx = 0; tx < w; tx++) {
                int draw_tx = flip_h ? (w - 1 - tx) : tx;
                uint16_t tile_idx;
                if (flags & ASSET_FLAG_EXTENDED_TILE) tile_idx = ((const uint16_t*)tile_data)[ty * w + tx];
                else tile_idx = tile_data[ty * w + tx];
                
                if (tile_idx == 0xFFFF) continue;
                int base_id = (tile_idx & 0x0FFF);
                if (base_id >= 256) continue;   /* dynamic_tileset holds 256 tiles; guard OOB */
                const uint8_t *tile_ptr = dynamic_tileset + base_id * 256;

                if (flags & ASSET_FLAG_ZELDA_QUAD) {
                    int quad_map[4] = {0, 1, 2, 3};
                    if (flip_h) { quad_map[0] = 2; quad_map[2] = 0; quad_map[1] = 3; quad_map[3] = 1; }

                    int start_q = 0, end_q = 4;
                    if (flags & ASSET_FLAG_ZELDA_VERT) {
                        int sub_idx = (tile_idx >> 12);
                        if (sub_idx == 0) { start_q = 0; end_q = 2; }
                        else { start_q = 2; end_q = 4; }
                    }

                    for (int q = start_q; q < end_q; q++) {
                        int src_q = quad_map[q];
                        int dx = (flags & ASSET_FLAG_ZELDA_VERT) ? 0 : ((q / 2) * 8);
                        int dy = (q % 2) * 8;

                        // Inline the 8x8 sprite drawing for performance
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
                                fb_row[fb_x] = dynamic_palette[color_idx];
                            }
                        }
                    }
                } else {
                    // Mario 16x16 tile
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
                            fb_row[fb_x] = dynamic_palette[color_idx];
                        }
                    }
                }
            }
        }
    }
}

void gui_draw_asset(int x, int y, const uint8_t *recipe, bool transparent, bool flip_h) {
    if (!recipe) return;
    gui_draw_asset_internal(x, y, recipe, transparent, flip_h);
}

void gui_draw_asset_by_idx(int x, int y, int idx, bool transparent, bool flip_h) {
    if (idx < 0 || idx >= asset_list_count) return;
    gui_draw_asset_internal(x, y, asset_list[idx], transparent, flip_h);
}

const uint8_t *asset_get_by_idx(int idx) {
    if (idx < 0 || idx >= asset_list_count) return NULL;
    return asset_list[idx];
}

void asset_get_dims_by_idx(int idx, int *w, int *h) {
    if (idx < 0 || idx >= asset_list_count) { *w = 1; *h = 1; return; }
    const uint8_t *recipe = asset_list[idx];
    *w = recipe[0] >> 4;
    *h = recipe[0] & 0x0F;
}

static bool load_palette_from_compressed_memory(const uint8_t *source_fw,
                                                uint8_t *scratch,
                                                uint8_t *pal_raw) {
    const uint32_t RWDATA_TABLE   = 0x180A4;
    const uint32_t CM_DST         = 0x240F2124;
    const uint32_t PALETTE_OFFSET = 0xCD54;

    uint32_t cm_addr = 0, cm_len = 0;
    for (int n = 0; n < 5; n++) {
        uint32_t e = RWDATA_TABLE + n * 16;
        uint32_t inflate_ptr = *(const uint32_t *)(source_fw + e);
        if (inflate_ptr == 0x77777777 || inflate_ptr == 0) break;
        uint32_t rel_addr = *(const uint32_t *)(source_fw + e + 4);
        uint32_t comp_len = *(const uint32_t *)(source_fw + e + 8);
        uint32_t dst      = *(const uint32_t *)(source_fw + e + 12);
        if (dst == CM_DST) { cm_addr = e + 4 + rel_addr; cm_len = comp_len; }
    }
    if (cm_addr == 0 || cm_len == 0) return false;

    SizeT destLen = 0x10000;
    SizeT inSize = cm_len;
    ELzmaStatus status;
    static const uint8_t props[5] = {0x5D, 0x00, 0x40, 0x00, 0x00};
    if (app_lzma_decode(scratch, &destLen, source_fw + cm_addr, &inSize,
                   props, 5, LZMA_FINISH_ANY, &status, &lzma_allocator) != SZ_OK) {
        return false;
    }
    if (destLen < PALETTE_OFFSET + 320) return false;
    memcpy(pal_raw, scratch + PALETTE_OFFSET, 320);
    return true;
}

static void build_nes_palette(const uint8_t *nes_pal, const uint8_t *indices) {
    dynamic_palette[80] = 0; // Transparent
    for (int i = 0; i < 3; i++) {
        uint16_t r = nes_pal[indices[i] * 3 + 0], g = nes_pal[indices[i] * 3 + 1], b = nes_pal[indices[i] * 3 + 2];
        dynamic_palette[81 + i] = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3);
    }
}

void board_load_dynamic_assets(void) {
    memset(dynamic_palette, 0, sizeof(dynamic_palette));
    gui_bg_color = RGB565(0x12, 0x12, 0x12);
    gui_fg_color = RGB565(0xEE, 0xEE, 0xEE);
    gui_accent_color = RGB565(0x00, 0xD0, 0xD0);
    assets_loaded = false;

    if (!board_ospi_init()) return;

    const uint8_t *mario_backup = (const uint8_t *)(EXTFLASH_BASE + MARIO_SPI_OFFSET);
    const uint8_t *zelda_backup = (const uint8_t *)(EXTFLASH_BASE + ZELDA_SPI_OFFSET);
    const uint8_t *source_fw = NULL;
    int ofw_type = (int)board_console_type;

    if (ofw_type == 1) source_fw = mario_backup;
    else if (ofw_type == 2) source_fw = zelda_backup;
    else {
        if (board_is_valid_app((uint32_t)mario_backup)) { ofw_type = 1; source_fw = mario_backup; }
        else if (board_is_valid_app((uint32_t)zelda_backup)) { ofw_type = 2; source_fw = zelda_backup; }
    }

    if (source_fw == NULL) return;
    board_console_type = (board_console_type_t)ofw_type;

    if (ofw_type == 2) {
        const uint8_t *zelda_pal = (const uint8_t *)(EXTFLASH_BASE + 0x2E8B24);
        for (int i = 0; i < 77; i++) {
            uint16_t b = zelda_pal[i * 4 + 0], g = zelda_pal[i * 4 + 1], r = zelda_pal[i * 4 + 2];
            dynamic_palette[i] = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3);
        }
        const uint8_t *nes_pal = (const uint8_t *)(EXTFLASH_BASE + 0x2D8160);
        uint8_t link_colors[3] = {0x37, 0x28, 0x1A};
        build_nes_palette(nes_pal, link_colors);
        memcpy(dynamic_tileset, (const uint8_t *)(EXTFLASH_BASE + 0x20000), 65536);
        /* The Mario branch commits assets_loaded only on a successful decode; the
         * Zelda copy always "succeeds", so guard against an absent/erased external
         * flash (which reads back all-0xFF) the same way — otherwise it loads as a
         * garbage tileset and renders garbage sprites. A real tileset has data, so
         * any non-0xFF byte means the assets are present (color-only fallback
         * otherwise, like a failed Mario decode). */
        for (int i = 0; i < 65536; i++) {
            if (dynamic_tileset[i] != 0xFF) { assets_loaded = true; break; }
        }
    } else if (ofw_type == 1) {
        uint32_t compressed_ptr = *(const uint32_t *)(source_fw + 0x7350);
        if (compressed_ptr >= 0x08000000 && compressed_ptr < 0x08020000) {
            if ((uint32_t)source_fw == 0x08100000) compressed_ptr += 0x00100000;
            else if ((uint32_t)source_fw >= 0x90000000) compressed_ptr = (uint32_t)source_fw + (compressed_ptr - 0x08000000);
        }
        uint8_t pal_raw[320] = {0};
        load_palette_from_compressed_memory(source_fw, (uint8_t *)get_scratch_buffer(), pal_raw);
        lzma_alloc((void*)1, 0);
        SizeT destLen = 0x10000, inSize = 0x10000;
        ELzmaStatus status;
        static const uint8_t lzma_props[5] = {0x5D, 0x00, 0x40, 0x00, 0x00};
        if (app_lzma_decode(dynamic_tileset, &destLen, (const uint8_t *)compressed_ptr, &inSize, lzma_props, 5, LZMA_FINISH_ANY, &status, &lzma_allocator) == SZ_OK) {
            for (int i = 0; i < 80; i++) {
                uint16_t b = pal_raw[i * 4 + 0], g = pal_raw[i * 4 + 1], r = pal_raw[i * 4 + 2];
                dynamic_palette[i] = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3);
            }
            const uint8_t *nes_pal = (const uint8_t *)(EXTFLASH_BASE + 0x400000 + 0xA8B84);
            uint8_t mario_colors[3] = {0x16, 0x27, 0x18};
            build_nes_palette(nes_pal, mario_colors);
            assets_loaded = true;
        }
    }
    ui_update_theme();
    SCB_CleanDCache_by_Addr((uint32_t *)dynamic_palette, sizeof(dynamic_palette));
    SCB_CleanDCache_by_Addr((uint32_t *)dynamic_tileset, sizeof(dynamic_tileset));
}
