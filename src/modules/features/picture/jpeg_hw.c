/*
 * Hardware JPEG decoder for the Picture Viewer. See jpeg_hw.h.
 *
 * Bring-up (synthesised from the STM32H7 HAL JPEG driver + CMSIS defs, register map proven by
 * the fastcap encoder): clock (RCC.JPGDECEN) -> core (CR.JCEN) -> stop -> flush FIFOs -> clear
 * flags -> header processing (CONFR1.HDR) + DECODE (CONFR1.DE) -> start -> feed DIR until the
 * geometry registers populate (CONFR1[31:16] height != 0) -> read geometry + subsampling ->
 * stream/drain the decoded YCbCr MCUs from DOR.
 *
 * The FIFOs are 16 words deep with an 8-word threshold (SR.IFTF / SR.OFTF). We BURST: drain 8
 * output words per status poll (and feed 8 in) so the codec runs continuously instead of
 * stalling on a full output FIFO between single-word reads. Each completed MCU is nearest-
 * neighbour resampled straight into the framebuffer, converting only the displayed pixels.
 */
#include "stm32h7xx.h"
#include "jpeg_hw.h"

#define JPEG_CFR_CLEAR  0x30u   /* clear EOC + HPD (== fastcap's verified CFR_ALL) */

static void hw_start(void) {
    RCC->AHB3ENR |= RCC_AHB3ENR_JPGDECEN;
    (void)RCC->AHB3ENR;
    __DSB(); __ISB();
    for (volatile int d = 0; d < 64; d++) { }
    JPEG->CR = JPEG_CR_JCEN;
    JPEG->CONFR0 &= ~JPEG_CONFR0_START;
    JPEG->CR |= JPEG_CR_IFF | JPEG_CR_OFF;
    JPEG->CFR = JPEG_CFR_CLEAR;
    JPEG->CONFR1 = JPEG_CONFR1_HDR | JPEG_CONFR1_DE;
    JPEG->CONFR0 |= JPEG_CONFR0_START;
}
static void hw_stop(void) {
    JPEG->CONFR0 &= ~JPEG_CONFR0_START;
    JPEG->CFR = JPEG_CFR_CLEAR;
}

/* Feed one word of the JPEG stream to DIR. Returns 1 if a word was written, 0 if no data was
 * available (refilling the input staging buffer as needed). */
static int hw_feed(hwjpeg_read_fn rd, void *dev, uint8_t *in, int cap,
                   int *navail, int *npos, int *eof) {
    if (*npos + 4 > *navail && !*eof) {
        int rem = *navail - *npos;
        if (rem > 0 && *npos > 0) for (int i = 0; i < rem; i++) in[i] = in[*npos + i];
        *navail = rem; *npos = 0;
        int r = (int)rd(dev, in + *navail, (size_t)(cap - *navail));
        if (r <= 0) *eof = 1; else *navail += r;
    }
    if (*npos + 4 <= *navail) {
        uint32_t w = (uint32_t)in[*npos] | ((uint32_t)in[*npos + 1] << 8)
                   | ((uint32_t)in[*npos + 2] << 16) | ((uint32_t)in[*npos + 3] << 24);
        JPEG->DIR = w; *npos += 4; return 1;
    } else if (*eof && *npos < *navail) {
        uint32_t w = 0;
        for (int i = 0; *npos < *navail; i++, (*npos)++) w |= (uint32_t)in[*npos] << (8 * i);
        JPEG->DIR = w; return 1;
    }
    return 0;
}

/* ---- full pixel decode ---- */

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* NN-resample one decoded MCU (decoded rect [X,X+mcuW) x [Y,Y+mcuH)) onto the view, converting
 * only the screen pixels that map into it. buf: the codec's YCbCr 8x8 blocks -- ny luma, Cb, Cr.
 * fmt: 0=4:2:0 1=4:2:2 2=4:4:4 3=grayscale. */
static void blit_mcu(const hwjpeg_view_t *v, int X, int Y, int mcuW, int mcuH,
                     const uint8_t *buf, int fmt, int ny) {
    const uint8_t *Cb = buf + ny * 64;
    const uint8_t *Cr = buf + ny * 64 + 64;
    int right = X + mcuW - 1, bottom = Y + mcuH - 1;
    int i_lo = (int)((long)X * v->vw / v->dw) - v->svx - 1;
    int i_hi = (int)((long)(right + 1) * v->vw / v->dw) - v->svx + 1;
    int j_lo = (int)((long)Y * v->vh / v->dh) - v->svy - 1;
    int j_hi = (int)((long)(bottom + 1) * v->vh / v->dh) - v->svy + 1;
    if (i_lo < 0) i_lo = 0;
    if (i_hi > v->svw) i_hi = v->svw;
    if (j_lo < 0) j_lo = 0;
    if (j_hi > v->svh) j_hi = v->svh;

    for (int j = j_lo; j < j_hi; j++) {
        int dy = (int)((long)(v->svy + j) * v->dh / v->vh);
        if (dy < Y || dy > bottom) continue;
        int sy = v->ddy + j;
        if (sy < v->cy0 || sy >= v->cy1) continue;
        int py = dy - Y;
        uint16_t *frow = v->fb + (uint32_t)sy * v->stride;
        for (int i = i_lo; i < i_hi; i++) {
            int dx = (int)((long)(v->svx + i) * v->dw / v->vw);
            if (dx < X || dx > right) continue;
            int sx = v->ddx + i;
            if (sx < v->cx0 || sx >= v->cx1) continue;
            int px = dx - X;

            int yb, yi;
            if (fmt == 0)      { yb = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0); yi = (py & 7) * 8 + (px & 7); }
            else if (fmt == 1) { yb = (px >= 8 ? 1 : 0);                     yi = py * 8 + (px & 7); }
            else               { yb = 0;                                    yi = py * 8 + px; }
            int Yv = buf[yb * 64 + yi];

            int R, G, B;
            if (fmt == 3) { R = G = B = Yv; }
            else {
                int ci;
                if (fmt == 0)      ci = (py / 2) * 8 + (px / 2);
                else if (fmt == 1) ci = py * 8 + (px / 2);
                else               ci = py * 8 + px;
                int cb = (int)Cb[ci] - 128, cr = (int)Cr[ci] - 128;
                R = clamp8(Yv + ((91881 * cr) >> 16));
                G = clamp8(Yv - ((22554 * cb + 46802 * cr) >> 16));
                B = clamp8(Yv + ((116130 * cb) >> 16));
            }
            frow[sx] = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
        }
    }
}

/* MCU assembly state, shared by the burst-drain helper (static -> off the stack). */
static struct {
    uint8_t buf[6 * 64];
    int mbi, mcuW, mcuH, fmt, ny, mcx, mcy, mcus_x, done, mcu_bytes;
    const hwjpeg_view_t *view;
} g_mcu;

static inline void drain_word(uint32_t word) {
    g_mcu.buf[g_mcu.mbi++] = (uint8_t)word;
    g_mcu.buf[g_mcu.mbi++] = (uint8_t)(word >> 8);
    g_mcu.buf[g_mcu.mbi++] = (uint8_t)(word >> 16);
    g_mcu.buf[g_mcu.mbi++] = (uint8_t)(word >> 24);
    if (g_mcu.mbi >= g_mcu.mcu_bytes) {
        blit_mcu(g_mcu.view, g_mcu.mcx * g_mcu.mcuW, g_mcu.mcy * g_mcu.mcuH,
                 g_mcu.mcuW, g_mcu.mcuH, g_mcu.buf, g_mcu.fmt, g_mcu.ny);
        g_mcu.mbi = 0; g_mcu.done++;
        if (++g_mcu.mcx >= g_mcu.mcus_x) { g_mcu.mcx = 0; g_mcu.mcy++; }
    }
}

int hwjpeg_decode(hwjpeg_read_fn rd, void *dev, hwjpeg_info_fn on_info, void *ctx,
                  uint8_t *inbuf, int inbuf_cap) {
    if (!rd || !on_info || !inbuf || inbuf_cap < 8) return -1;
    hw_start();

    uint8_t *in = inbuf;
    int incap = inbuf_cap;
    int navail = 0, npos = 0, eof = 0, got = 0;
    long guard = 8000000;
    while (guard-- > 0) {                              /* feed until geometry populates */
        uint32_t sr = JPEG->SR;
        if (sr & JPEG_SR_IFNFF) (void)hw_feed(rd, dev, in, incap, &navail, &npos, &eof);
        if (((JPEG->CONFR1 >> 16) & 0xFFFFu) != 0u) { got = 1; break; }
        if (sr & JPEG_SR_EOCF) break;
        if (eof && npos >= navail) break;
    }
    if (!got) { hw_stop(); return -1; }

    int w = (int)((JPEG->CONFR3 >> 16) & 0xFFFFu);
    int h = (int)((JPEG->CONFR1 >> 16) & 0xFFFFu);
    uint32_t nf = JPEG->CONFR1 & 0x3u;
    int mcuW, mcuH, ny, nblocks, fmt, ncomp;
    if (nf == 0u) {
        fmt = 3; mcuW = 8; mcuH = 8; ny = 1; nblocks = 1; ncomp = 1;
    } else if (nf == JPEG_CONFR1_NF_1) {
        uint32_t yNB = (JPEG->CONFR4 & JPEG_CONFR4_NB) >> 4;
        ncomp = 3;
        if      (yNB == 3u) { fmt = 0; mcuW = 16; mcuH = 16; ny = 4; nblocks = 6; }
        else if (yNB == 1u) { fmt = 1; mcuW = 16; mcuH = 8;  ny = 2; nblocks = 4; }
        else if (yNB == 0u) { fmt = 2; mcuW = 8;  mcuH = 8;  ny = 1; nblocks = 3; }
        else { hw_stop(); return -1; }
    } else { hw_stop(); return -1; }   /* CMYK / unsupported -> software */

    hwjpeg_view_t view;
    on_info(ctx, w, h, ncomp, &view);

    int mcus_y = (h + mcuH - 1) / mcuH;
    g_mcu.mbi = 0; g_mcu.mcuW = mcuW; g_mcu.mcuH = mcuH; g_mcu.fmt = fmt; g_mcu.ny = ny;
    g_mcu.mcx = 0; g_mcu.mcy = 0; g_mcu.done = 0; g_mcu.mcu_bytes = nblocks * 64;
    g_mcu.mcus_x = (w + mcuW - 1) / mcuW; g_mcu.view = &view;
    int total = g_mcu.mcus_x * mcus_y;
    guard = 200000000;

    while (g_mcu.done < total && guard-- > 0) {
        uint32_t sr = JPEG->SR;
        if ((sr & JPEG_SR_IFTF) && !eof) {            /* burst-feed: room for >= 8 */
            for (int k = 0; k < 8; k++)
                if (!hw_feed(rd, dev, in, incap, &navail, &npos, &eof)) break;
        } else if ((sr & JPEG_SR_IFNFF) && !eof) {
            (void)hw_feed(rd, dev, in, incap, &navail, &npos, &eof);
        }
        if (sr & JPEG_SR_OFTF) {                       /* burst-drain: >= 8 words available */
            for (int k = 0; k < 8; k++) drain_word(JPEG->DOR);
        } else if (sr & JPEG_SR_OFNEF) {
            drain_word(JPEG->DOR);
        } else if (sr & JPEG_SR_EOCF) {
            break;
        }
    }

    hw_stop();
    return 0;
}
