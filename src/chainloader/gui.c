#include "gui.h"
#include "board.h"
#include <string.h>

/* Dual RGB565 framebuffers in AXI SRAM, aligned to 32-byte cache line size */
uint16_t fb_a[SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(32)));
uint16_t fb_b[SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(32)));

/* Public pointer that the drawing APIs mutate */
uint16_t *framebuffer = fb_a;
static uint16_t *back_buffer = fb_b;
static bool gui_initialized = false;

static volatile uint32_t frame_counter = 0;
static volatile uint16_t *ltdc_pending_fb = NULL;

SPI_HandleTypeDef hspi2;
LTDC_HandleTypeDef hltdc;

/* Hardware Interrupt Handlers */
void LTDC_IRQHandler(void) {
    HAL_LTDC_IRQHandler(&hltdc);
}

void HAL_LTDC_LineEventCallback(LTDC_HandleTypeDef *hltdc_inst) {
    if (hltdc_inst->Instance == LTDC) {
        frame_counter++;
        HAL_LTDC_ProgramLineEvent(hltdc_inst, SCREEN_HEIGHT - 1);
    }
}

/* Called by HAL when VBR fires — we are in the blanking period, safe to apply IMR */
void HAL_LTDC_ReloadEventCallback(LTDC_HandleTypeDef *hltdc_inst) {
    if (ltdc_pending_fb != NULL) {
        HAL_LTDC_SetAddress(hltdc_inst, (uint32_t)ltdc_pending_fb, 0);
        ltdc_pending_fb = NULL;
    }
}

#include "assets.h"
/* actual storage definitions for dynamic assets */
bool assets_loaded = false;
uint16_t dynamic_palette[128] __attribute__((aligned(32)));
uint8_t dynamic_tileset[65536] __attribute__((aligned(32)));

uint16_t gui_bg_color = RGB565(0x20, 0x24, 0x28);
uint16_t gui_fg_color = RGB565(0xD0, 0xD4, 0xD8);
uint16_t gui_accent_color = RGB565(0x00, 0xA0, 0xA0);
uint16_t gui_border_color = RGB565(0xD0, 0xD4, 0xD8);

static void gui_spi2_init(void) {
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    __HAL_RCC_SPI2_FORCE_RESET();
    __HAL_RCC_SPI2_RELEASE_RESET();

    SPI2->CFG1 = (7 << SPI_CFG1_DSIZE_Pos) | (4 << SPI_CFG1_MBR_Pos);
    SPI2->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_SSOE;
    SPI2->CR1 = SPI_CR1_SSI;
    SPI2->CR2 = 2;
    SPI2->CR1 |= SPI_CR1_SPE;
}

static void gui_ltdc_init(void) {
    hltdc.Instance = LTDC;
    hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
    hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
    hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
    hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IIPC;
    hltdc.Init.HorizontalSync = 9;
    hltdc.Init.VerticalSync = 1;
    hltdc.Init.AccumulatedHBP = 60;
    hltdc.Init.AccumulatedVBP = 7;
    hltdc.Init.AccumulatedActiveW = 380;
    hltdc.Init.AccumulatedActiveH = 247;
    hltdc.Init.TotalWidth = 392;
    hltdc.Init.TotalHeigh = 255;
    hltdc.Init.Backcolor.Blue = 0;
    hltdc.Init.Backcolor.Green = 0;
    hltdc.Init.Backcolor.Red = 0;
    if (HAL_LTDC_Init(&hltdc) != HAL_OK) {
        Error_Handler();
    }

    LTDC_LayerCfgTypeDef pLayerCfg = {0};
    pLayerCfg.WindowX0 = 0;
    pLayerCfg.WindowX1 = SCREEN_WIDTH;
    pLayerCfg.WindowY0 = 0;
    pLayerCfg.WindowY1 = SCREEN_HEIGHT;
    pLayerCfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
    pLayerCfg.Alpha = 255;
    pLayerCfg.Alpha0 = 255;
    pLayerCfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
    pLayerCfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
    pLayerCfg.FBStartAdress = (uint32_t)framebuffer;
    pLayerCfg.ImageWidth = SCREEN_WIDTH;
    pLayerCfg.ImageHeight = SCREEN_HEIGHT;
    pLayerCfg.Backcolor.Blue = 0;
    pLayerCfg.Backcolor.Green = 255;
    pLayerCfg.Backcolor.Red = 0;
    if (HAL_LTDC_ConfigLayer(&hltdc, &pLayerCfg, 0) != HAL_OK) {
        Error_Handler();
    }
}

static void gw_lcd_set_chipselect(uint32_t p) {
    GPIOB->BSRR = p == 0 ? GPIO_PIN_12 : (uint32_t)GPIO_PIN_12 << 16;
}

static void gw_lcd_set_reset(uint32_t p) {
    GPIOD->BSRR = p == 0 ? GPIO_PIN_8 : (uint32_t)GPIO_PIN_8 << 16;
}

static void gw_lcd_spi_tx(SPI_HandleTypeDef *spi, uint8_t *pData) {
    (void)spi;
    gw_lcd_set_chipselect(1);
    HAL_Delay(2);

    SPI2->CR1 &= ~SPI_CR1_SPE;
    SPI2->CR2 = 2;
    SPI2->CR1 |= SPI_CR1_SPE;
    SPI2->CR1 |= SPI_CR1_CSTART;

    for (int i = 0; i < 2; i++) {
        while (!(SPI2->SR & SPI_SR_TXP));
        *((volatile uint8_t *)&SPI2->TXDR) = pData[i];
    }

    while (!(SPI2->SR & SPI_SR_EOT));
    SPI2->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;

    HAL_Delay(2);
    wdog_refresh();
    gw_lcd_set_chipselect(0);
    HAL_Delay(2);
}

void gui_init(void) {
    if (gui_initialized) return;
    gui_initialized = true;

    board_lcd_gpios_init();
    gui_spi2_init();

    /* Pre-fill both buffers with the background color before LTDC starts scanning */
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        fb_a[i] = COLOR_BG;
        fb_b[i] = COLOR_BG;
    }
    SCB_CleanDCache_by_Addr((uint32_t *)fb_a, sizeof(fb_a));
    SCB_CleanDCache_by_Addr((uint32_t *)fb_b, sizeof(fb_b));

    /* Init LTDC before powering on the LCD panel. The panel starts displaying as
     * soon as the SPI init sequence completes, so LTDC must already be outputting
     * valid background data at that point. */
    gui_ltdc_init();

    HAL_LTDC_SetAddress(&hltdc, (uint32_t)framebuffer, 0);

    __HAL_LTDC_ENABLE_IT(&hltdc, LTDC_IT_LI | LTDC_IT_RR);
    HAL_LTDC_ProgramLineEvent(&hltdc, SCREEN_HEIGHT - 1);
    HAL_NVIC_SetPriority(LTDC_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(LTDC_IRQn);

    /* Now power on and SPI-init the LCD panel */
    gw_lcd_set_chipselect(0);
    gw_lcd_set_reset(0);

    GPIOD->BSRR = GPIO_PIN_4;   // 3V3 on
    HAL_Delay(2);
    GPIOD->BSRR = (uint32_t)GPIO_PIN_1 << 16; // 1V8 on
    HAL_Delay(50);
    wdog_refresh();

    /* Lets go, bootup sequence. */
    /* reset sequence */
    gw_lcd_set_reset(0); // HIGH
    HAL_Delay(1);
    gw_lcd_set_reset(1); // LOW (assert)
    HAL_Delay(20);
    gw_lcd_set_reset(0); // HIGH (release)
    HAL_Delay(50);
    wdog_refresh();

    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x08\x80");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x6E\x80");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x80\x80");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x68\x00");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\xd0\x00");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x1b\x00");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\xe0\x00");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x6a\x80");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x80\x00");
    gw_lcd_spi_tx(&hspi2, (uint8_t *)"\x14\x80");

    /* Wait for panel to finish initializing before turning on backlight */
    HAL_Delay(50);
    wdog_refresh();

    gui_backlight_on(true);
}

void gui_deinit(void) {
    gui_initialized = false;
    gui_backlight_on(false);

    /* Power down and reset the LCD panel */
    GPIOD->BSRR = ((uint32_t)GPIO_PIN_8 << 16) | GPIO_PIN_1 | ((uint32_t)GPIO_PIN_4 << 16);

    /* Shut down LTDC */
    HAL_NVIC_DisableIRQ(LTDC_IRQn);
    HAL_LTDC_DeInit(&hltdc);
    __HAL_RCC_LTDC_CLK_DISABLE();

    /* Shut down SPI2 */
    SPI2->CR1 &= ~SPI_CR1_SPE;
    __HAL_RCC_SPI2_CLK_DISABLE();
}

void gui_fill(uint16_t color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        framebuffer[i] = color;
    }
}

void gui_draw_char(int x, int y, uint32_t cp, uint16_t color) {
    gui_draw_char_clipped(x, y, 0, SCREEN_WIDTH, cp, color);
}

void gui_draw_selector(int x, int y, uint16_t color) {
    gui_draw_char(x, y, '>', color);
}

/*
 * Draws an uncompressed sprite to the active framebuffer.
 * Optimised: Hoists vertical bounds check and pre-computes row address offset.
 */
void gui_draw_sprite(int x, int y, int w, int h, const uint8_t *pixel_data, bool transparent, bool flip_h, int pitch) {
    if (pitch == 0) pitch = w;
    for (int ty = 0; ty < h; ty++) {
        int fb_y = y + ty;
        if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;

        uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
        const uint8_t *src_row = &pixel_data[ty * pitch];

        for (int tx = 0; tx < w; tx++) {
            int fb_x = x + tx;
            if (fb_x < 0 || fb_x >= SCREEN_WIDTH) continue;

            int src_x = flip_h ? (w - 1 - tx) : tx;
            uint8_t color_idx = src_row[src_x];

            // Limit palette access to <= 79. Color indices >= 80 are padding/transparent.
            if (transparent && (color_idx == 0 || color_idx >= 80)) continue;
            fb_row[fb_x] = dynamic_palette[color_idx];
        }
    }
}

void gui_draw_text(int x, int y, const char *str, uint16_t color) {
    gui_draw_text_marquee(x, y, SCREEN_WIDTH, str, color, false, 0);
}

#include "assets.h"
#include "ui/gui_font.h"
void gui_draw_char_clipped(int x, int y, int clip_x, int clip_w, uint32_t cp, uint16_t color) {
    if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 'A';
    
    if (cp < 0x20 || cp > 0x5A) return; // unsupported char
    const uint8_t *bitmap = gui_font_ascii[cp - 0x20];
    
    for (int row = 0; row < 8; row++) {
        int fb_y = y + row;
        if (fb_y < 0 || fb_y >= SCREEN_HEIGHT) continue;
        uint16_t *fb_row = &framebuffer[fb_y * SCREEN_WIDTH];
        uint8_t line = bitmap[row];
        for (int col = 0; col < 8; col++) {
            int fb_x = x + col;
            if (fb_x < 0 || fb_x >= SCREEN_WIDTH || fb_x < clip_x || fb_x >= clip_x + clip_w) continue;
            if ((line >> (7 - col)) & 1) fb_row[fb_x] = color;
        }
    }
}

void gui_draw_text_marquee(int x, int y, int max_w, const char *str, uint16_t color, bool is_active, uint32_t tick_offset) {
    int text_len = strlen(str);
    int text_w = text_len * 8;
    int offset_x = 0;

    if (text_w > max_w && is_active) {
        uint32_t ticks = HAL_GetTick() - tick_offset;
        int wait_start = 2000; // 2s
        int wait_end = 2000;   // 2s
        int scroll_speed = 30; // pixels per second
        int scroll_dist = text_w - max_w;
        int scroll_time = (scroll_dist * 1000) / scroll_speed;
        int cycle_time = wait_start + scroll_time + wait_end;
        
        int t = ticks % cycle_time;
        if (t < wait_start) {
            offset_x = 0;
        } else if (t < wait_start + scroll_time) {
            offset_x = ((t - wait_start) * scroll_speed) / 1000;
        } else {
            offset_x = scroll_dist;
        }
    }

    int cursor_x = x - offset_x;
    const uint8_t *u = (const uint8_t *)str;
    while (*u) {
        uint32_t cp = *u++;
        if (cursor_x + 8 > x && cursor_x < x + max_w) {
            gui_draw_char_clipped(cursor_x, y, x, max_w, cp, color);
        }
        cursor_x += 8;
    }
}

void gui_draw_rect(int x, int y, int w, int h, uint16_t color) {
    // Horizontal lines
    for (int i = 0; i < w; i++) {
        int fb_x = x + i;
        if (fb_x >= 0 && fb_x < SCREEN_WIDTH) {
            if (y >= 0 && y < SCREEN_HEIGHT) framebuffer[y * SCREEN_WIDTH + fb_x] = color;
            if (y + h - 1 >= 0 && y + h - 1 < SCREEN_HEIGHT) framebuffer[(y + h - 1) * SCREEN_WIDTH + fb_x] = color;
        }
    }
    // Vertical lines
    for (int i = 0; i < h; i++) {
        int fb_y = y + i;
        if (fb_y >= 0 && fb_y < SCREEN_HEIGHT) {
            if (x >= 0 && x < SCREEN_WIDTH) framebuffer[fb_y * SCREEN_WIDTH + x] = color;
            if (x + w - 1 >= 0 && x + w - 1 < SCREEN_WIDTH) framebuffer[fb_y * SCREEN_WIDTH + (x + w - 1)] = color;
        }
    }
}

/* Clip a rectangle to the screen, mutating x/y/w/h. Returns false if nothing is left to draw. */
static bool gui_clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > SCREEN_WIDTH)  { *w = SCREEN_WIDTH - *x; }
    if (*y + *h > SCREEN_HEIGHT) { *h = SCREEN_HEIGHT - *y; }
    return (*w > 0 && *h > 0);
}

/*
 * Draws a solid filled rectangle of a specified color.
 * Optimised: Clips boundaries upfront to eliminate bounds checking from inner loop.
 */
void gui_draw_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!gui_clip_rect(&x, &y, &w, &h)) return;

    for (int j = 0; j < h; j++) {
        uint16_t *dest = &framebuffer[(y + j) * SCREEN_WIDTH + x];
        for (int i = 0; i < w; i++) {
            dest[i] = color;
        }
    }
}

/*
 * Blends a rectangle with the existing framebuffer contents.
 * frost=false: 25% darken (selection dim). frost=true: 50% lighten toward white.
 */
void gui_draw_blend_rect(int x, int y, int w, int h, bool frost) {
    if (!gui_clip_rect(&x, &y, &w, &h)) return;
    (void)frost;

    for (int j = 0; j < h; j++) {
        uint16_t *dest = &framebuffer[(y + j) * SCREEN_WIDTH + x];
        for (int i = 0; i < w; i++) {
            uint16_t pixel = dest[i];
            dest[i] = ((pixel & 0xE79C) >> 2) + ((gui_bg_color & 0xE79C) >> 2) + ((gui_bg_color & 0xF7DE) >> 1);
        }
    }
}

void gui_draw_progress_bar(int x, int y, int w, int h, int percent, uint16_t border_color, uint16_t fill_color) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gui_draw_rect(x, y, w, h, border_color);
    int fill_w = (percent * (w - 4)) / 100;
    if (fill_w > 0) {
        gui_draw_fill_rect(x + 2, y + 2, fill_w, h - 4, fill_color);
    }
    int empty_w = (w - 4) - fill_w;
    if (empty_w > 0) {
        gui_draw_fill_rect(x + 2 + fill_w, y + 2, empty_w, h - 4, COLOR_BG);
    }
}

void gui_backlight_on(bool on) {
    if (on) {
        GPIOA->BSRR = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    } else {
        GPIOA->BSRR = (GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6) << 16;
    }
}

void gui_refresh(void) {
    SCB_CleanDCache_by_Addr((uint32_t *)framebuffer, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

    /* Queue the new buffer — callback applies it via IMR during blanking */
    ltdc_pending_fb = framebuffer;
    HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);

    /* Wait for both VBR and the callback's IMR to clear (matches retro-go) */
    while (hltdc.Instance->SRCR & (LTDC_SRCR_VBR | LTDC_SRCR_IMR)) {
#ifndef DEBUG
        __WFI();
#endif
    }

    uint16_t *temp = framebuffer;
    framebuffer = back_buffer;
    back_buffer = temp;
}
