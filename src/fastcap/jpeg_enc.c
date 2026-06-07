/*
 * Hardware JPEG encoder for fastcap Gear 2.  See jpeg_enc.h.
 *
 * Direct-register driver for the STM32H7B0 JPEG codec, modelled on the
 * STM32H7xx HAL JPEG driver (the table-building routines are ported verbatim
 * from it — that is where the H7-specific Huffman/quant memory formats live).
 * No HAL, no DMA: a single polled feed/drain loop.  Fixed configuration:
 * YCbCr, 4:2:0 subsampling.  Quality (1..100) is passed to jpeg_enc_init at
 * reinit by the host (FASTCAP_QUALITY cell) and scales the standard Annex K
 * tables in set_quant; the per-call geometry and the HDR flag (emit a complete
 * JPEG vs. an entropy scan only) are programmed in jpeg_encode_region.
 */
#include "jpeg_enc.h"

/* ---- JPEG peripheral register map (JPGDEC_BASE = 0x52003000) ---- */
typedef struct {
    volatile uint32_t CONFR0;
    volatile uint32_t CONFR1;
    volatile uint32_t CONFR2;
    volatile uint32_t CONFR3;
    volatile uint32_t CONFR4;
    volatile uint32_t CONFR5;
    volatile uint32_t CONFR6;
    volatile uint32_t CONFR7;
    uint32_t          _r20[4];
    volatile uint32_t CR;
    volatile uint32_t SR;
    volatile uint32_t CFR;
    uint32_t          _r3c;
    volatile uint32_t DIR;
    volatile uint32_t DOR;
    uint32_t          _r48[2];
    volatile uint32_t QMEM0[16];
    volatile uint32_t QMEM1[16];
    volatile uint32_t QMEM2[16];
    volatile uint32_t QMEM3[16];
    volatile uint32_t HUFFMIN[16];
    volatile uint32_t HUFFBASE[32];
    volatile uint32_t HUFFSYMB[84];
    volatile uint32_t DHTMEM[103];
    uint32_t          _r4FC;
    volatile uint32_t HUFFENC_AC0[88];
    volatile uint32_t HUFFENC_AC1[88];
    volatile uint32_t HUFFENC_DC0[8];
    volatile uint32_t HUFFENC_DC1[8];
} JPEG_Regs;

#define JPG          ((JPEG_Regs *)0x52003000UL)
#define RCC_AHB3ENR  ((volatile uint32_t *)0x580244D4UL)
#define RCC_JPGDECEN (1u << 5)

/* CONFR0 */
#define CONFR0_START   0x1u
/* CONFR1 */
#define CONFR1_DE      0x008u
#define CONFR1_CS_0    0x010u            /* COLORSPACE = 1 → YCbCr, 2 quant tables */
#define CONFR1_NF_1    0x002u            /* NF field = 2 → 3 components */
#define CONFR1_NS_1    0x080u            /* NS field = 2 → 3 scan components */
#define CONFR1_HDR     0x100u            /* emit JPEG header markers in the output */
/* CONFR4 (Y): Hs=2,Vs=2,4 blocks */
#define CONFR4_Y       0x2230u           /* HSF_1|VSF_1|NB(3<<4) */
/* CONFR5/6 (Cb/Cr): Hs=1,Vs=1,1 block,quant table 1,Huff table 1 */
#define CONFR_CHROMA   0x1107u           /* HSF_0|VSF_0|QT_0|HA|HD */
/* CR */
#define CR_JCEN        0x1u
#define CR_IFF         0x2000u
#define CR_OFF         0x4000u
/* SR */
#define SR_IFNFF       0x004u
#define SR_OFTF        0x008u
#define SR_OFNEF       0x010u
#define SR_EOCF        0x020u
/* CFR */
#define CFR_ALL        0x30u             /* clear EOC + HPD flags */

/* DWT cycle counter (for the feed/drain safety timeout) */
#define DWT_CYCCNT  ((volatile uint32_t *)0xE0001004UL)
#define CPU_HZ      280000000UL
#define ENCODE_TIMEOUT_CYCS (CPU_HZ / 20UL)   /* ~50 ms */

/* RGB565→YCbCr 4:2:0 MCU-ordered input staging buffer.  In D2 AHB-SRAM1, right
   after the fastcap tile-hash table (0x30000000); 8 KB reserved.  The tiled
   fastcap path only ever encodes one 32x16 tile at a time = 2 MCU x 384 B = 768 B;
   only the (currently unused) jpeg_encode_full whole-frame path would need more.
   Fed to the JPEG core by the CPU, so the codec peripheral never touches D2. */
#define YCC_BUF  ((uint8_t *)0x30000400UL)

#define SCREEN_W  320
#define SCREEN_H  240
/* 4:2:0 MCU is 16×16; per MCU the YCC stream is 6 blocks × 64 bytes = 384 B.
   Image geometry (width/height/MCU count) is programmed per encode call. */
#define MCU_PX    16u
#define MCU_BYTES (6u * 64u)

/* Table sizes */
#define DC_HUFF_SIZE  12
#define AC_HUFF_SIZE  162
#define QUANT_SIZE    64
#define QUALITY_MIN   1
#define QUALITY_MAX   100

/* ---- Standard tables (ISO/IEC 10918-1 Annex K, copied from STM32 HAL) ---- */

static const uint8_t LUM_Q[QUANT_SIZE] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};
static const uint8_t CHROM_Q[QUANT_SIZE] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

static const uint8_t ZIGZAG[QUANT_SIZE] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct { uint8_t Bits[16]; uint8_t HuffVal[DC_HUFF_SIZE]; } DCHuff;
typedef struct { uint8_t Bits[16]; uint8_t HuffVal[AC_HUFF_SIZE]; } ACHuff;

static const DCHuff DC_LUM = {
    { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb }
};
static const DCHuff DC_CHROM = {
    { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb }
};
static const ACHuff AC_LUM = {
    { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d },
    {
        0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
        0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
        0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
        0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
        0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
        0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
        0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
        0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
        0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
        0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
        0xf9,0xfa
    }
};
static const ACHuff AC_CHROM = {
    { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },
    {
        0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
        0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
        0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
        0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
        0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
        0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
        0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
        0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
        0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
        0xf9,0xfa
    }
};

/* ---- Huffman bits/vals → sizes/codes (HAL Figures C.1–C.3) ---- */

static int bits_to_size_codes(const uint8_t *bits, uint8_t *huffsize,
                              uint32_t *huffcode, uint32_t *lastK)
{
    uint32_t i, p = 0, l, code, si;
    for (l = 0; l < 16u; l++) {
        i = bits[l];
        if ((p + i) > 256u) return -1;
        while (i != 0u) { huffsize[p++] = (uint8_t)(l + 1u); i--; }
    }
    huffsize[p] = 0;
    *lastK = p;

    code = 0; si = huffsize[0]; p = 0;
    while (huffsize[p] != 0u) {
        while ((uint32_t)huffsize[p] == si) { huffcode[p++] = code; code++; }
        if (si > 31u) return -1;
        if (code >= (1u << si)) return -1;
        code <<= 1; si++;
    }
    return 0;
}

/* Program one DC encoder Huffman table (base = HUFFENC_DC0 or HUFFENC_DC1). */
/* Scratch shared by the (one-shot, non-reentrant) table builders.  Kept static
   rather than on-stack: the hook runs on the chainloader's gui_refresh stack,
   which does not have ~1.3 KB of headroom for these. */
static uint8_t  s_huffsize[257];
static uint32_t s_huffcode[257];

static int set_huff_dc(const DCHuff *t, volatile uint32_t *base)
{
    uint8_t  codeLen[DC_HUFF_SIZE] = {0};
    uint32_t code[DC_HUFF_SIZE]    = {0};
    uint32_t k, lastK, l, lsb, msb, i;
    volatile uint32_t *addr;

    if (bits_to_size_codes(t->Bits, s_huffsize, s_huffcode, &lastK) != 0) return -1;
    for (k = 0; k < lastK; k++) {
        l = t->HuffVal[k];
        if (l >= DC_HUFF_SIZE) return -1;
        code[l]    = s_huffcode[k];
        codeLen[l] = (uint8_t)(s_huffsize[k] - 1u);
    }

    addr = base + (DC_HUFF_SIZE / 2);   /* +6 */
    addr[0] = 0x0FFF0FFF;
    addr[1] = 0x0FFF0FFF;

    i = DC_HUFF_SIZE;
    while (i > 1u) {
        i--; addr--;
        msb = (((uint32_t)codeLen[i] & 0xFu) << 8) | (code[i] & 0xFFu);
        i--;
        lsb = (((uint32_t)codeLen[i] & 0xFu) << 8) | (code[i] & 0xFFu);
        *addr = lsb | (msb << 16);
    }
    return 0;
}

/* Program one AC encoder Huffman table (base = HUFFENC_AC0 or HUFFENC_AC1). */
static int set_huff_ac(const ACHuff *t, volatile uint32_t *base)
{
    static uint8_t  codeLen[AC_HUFF_SIZE];
    static uint32_t code[AC_HUFF_SIZE];
    uint32_t k, lastK, l, lsb, msb, i, msbVal;
    volatile uint32_t *addr;

    for (i = 0; i < AC_HUFF_SIZE; i++) { codeLen[i] = 0; code[i] = 0; }

    if (bits_to_size_codes(t->Bits, s_huffsize, s_huffcode, &lastK) != 0) return -1;
    for (k = 0; k < lastK; k++) {
        l = t->HuffVal[k];
        if (l == 0u)            l = 160;          /* EOB */
        else if (l == 0xF0u)    l = 161;          /* ZRL */
        else { msbVal = (l & 0xF0u) >> 4; l = (msbVal * 10u) + (l & 0x0Fu) - 1u; }
        if (l >= AC_HUFF_SIZE) return -1;
        code[l]    = s_huffcode[k];
        codeLen[l] = (uint8_t)(s_huffsize[k] - 1u);
    }

    addr = base + (AC_HUFF_SIZE / 2);   /* +81 */
    addr[0] = 0x0FFF0FFF; addr[1] = 0x0FFF0FFF; addr[2] = 0x0FFF0FFF;
    addr[3] = 0x0FD10FD0; addr[4] = 0x0FD30FD2;
    addr[5] = 0x0FD50FD4; addr[6] = 0x0FD70FD6;

    i = AC_HUFF_SIZE;
    while (i > 1u) {
        i--; addr--;
        msb = (((uint32_t)codeLen[i] & 0xFu) << 8) | (code[i] & 0xFFu);
        i--;
        lsb = (((uint32_t)codeLen[i] & 0xFu) << 8) | (code[i] & 0xFFu);
        *addr = lsb | (msb << 16);
    }
    return 0;
}

/* Program the DHTMEM region so the core emits the DHT marker in the header.
   Ported verbatim from the HAL JPEG_Set_Huff_DHTMem (byte-packing layout).
   The -Warray-bounds suppression covers GCC false positives: `a` walks a
   fixed-address register array whose bounds the optimizer cannot track. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static void set_huff_dht(void)
{
    const ACHuff *ac0 = &AC_LUM,  *ac1 = &AC_CHROM;
    const DCHuff *dc0 = &DC_LUM,  *dc1 = &DC_CHROM;
    volatile uint32_t *m = JPG->DHTMEM;
    volatile uint32_t *a;
    uint32_t v, idx;

    /* DC0 BITS (DHTMEM[0..3]) */
    a = m + 3; idx = 16;
    while (idx > 3u) {
        *a = ((uint32_t)dc0->Bits[idx-1] << 24) | ((uint32_t)dc0->Bits[idx-2] << 16) |
             ((uint32_t)dc0->Bits[idx-3] << 8)  |  (uint32_t)dc0->Bits[idx-4];
        a--; idx -= 4u;
    }
    /* DC0 VALS (DHTMEM[4..6]) */
    a = m + 6; idx = 12;
    while (idx > 3u) {
        *a = ((uint32_t)dc0->HuffVal[idx-1] << 24) | ((uint32_t)dc0->HuffVal[idx-2] << 16) |
             ((uint32_t)dc0->HuffVal[idx-3] << 8)  |  (uint32_t)dc0->HuffVal[idx-4];
        a--; idx -= 4u;
    }
    /* AC0 BITS (DHTMEM[7..10]) */
    a = m + 10; idx = 16;
    while (idx > 3u) {
        *a = ((uint32_t)ac0->Bits[idx-1] << 24) | ((uint32_t)ac0->Bits[idx-2] << 16) |
             ((uint32_t)ac0->Bits[idx-3] << 8)  |  (uint32_t)ac0->Bits[idx-4];
        a--; idx -= 4u;
    }
    /* AC0 VALS (DHTMEM[11..51], last word shared) */
    a = m + 51;
    v = *a & 0xFFFF0000u;
    v |= ((uint32_t)ac0->HuffVal[161] << 8) | (uint32_t)ac0->HuffVal[160];
    *a = v;
    a = m + 50; idx = 160;
    while (idx > 3u) {
        *a = ((uint32_t)ac0->HuffVal[idx-1] << 24) | ((uint32_t)ac0->HuffVal[idx-2] << 16) |
             ((uint32_t)ac0->HuffVal[idx-3] << 8)  |  (uint32_t)ac0->HuffVal[idx-4];
        a--; idx -= 4u;
    }
    /* DC1 BITS (DHTMEM[51..55], shared words) */
    a = m + 51;
    v = *a & 0x0000FFFFu;
    v |= ((uint32_t)dc1->Bits[1] << 24) | ((uint32_t)dc1->Bits[0] << 16);
    *a = v;
    a = m + 55;
    v = *a & 0xFFFF0000u;
    v |= ((uint32_t)dc1->Bits[15] << 8) | (uint32_t)dc1->Bits[14];
    *a = v;
    a = m + 54; idx = 12;
    while (idx > 3u) {
        *a = ((uint32_t)dc1->Bits[idx+1] << 24) | ((uint32_t)dc1->Bits[idx] << 16) |
             ((uint32_t)dc1->Bits[idx-1] << 8)  |  (uint32_t)dc1->Bits[idx-2];
        a--; idx -= 4u;
    }
    /* DC1 VALS (DHTMEM[55..58], shared words) */
    a = m + 55;
    v = *a & 0x0000FFFFu;
    v |= ((uint32_t)dc1->HuffVal[1] << 24) | ((uint32_t)dc1->HuffVal[0] << 16);
    *a = v;
    a = m + 58;
    v = *a & 0xFFFF0000u;
    v |= ((uint32_t)dc1->HuffVal[11] << 8) | (uint32_t)dc1->HuffVal[10];
    *a = v;
    a = m + 57; idx = 8;
    while (idx > 3u) {
        *a = ((uint32_t)dc1->HuffVal[idx+1] << 24) | ((uint32_t)dc1->HuffVal[idx] << 16) |
             ((uint32_t)dc1->HuffVal[idx-1] << 8)  |  (uint32_t)dc1->HuffVal[idx-2];
        a--; idx -= 4u;
    }
    /* AC1 BITS (DHTMEM[58..62], shared words) */
    a = m + 58;
    v = *a & 0x0000FFFFu;
    v |= ((uint32_t)ac1->Bits[1] << 24) | ((uint32_t)ac1->Bits[0] << 16);
    *a = v;
    a = m + 62;
    v = *a & 0xFFFF0000u;
    v |= ((uint32_t)ac1->Bits[15] << 8) | (uint32_t)ac1->Bits[14];
    *a = v;
    a = m + 61; idx = 12;
    while (idx > 3u) {
        *a = ((uint32_t)ac1->Bits[idx+1] << 24) | ((uint32_t)ac1->Bits[idx] << 16) |
             ((uint32_t)ac1->Bits[idx-1] << 8)  |  (uint32_t)ac1->Bits[idx-2];
        a--; idx -= 4u;
    }
    /* AC1 VALS (DHTMEM[62..102], first word shared) */
    a = m + 62;
    v = *a & 0x0000FFFFu;
    v |= ((uint32_t)ac1->HuffVal[1] << 24) | ((uint32_t)ac1->HuffVal[0] << 16);
    *a = v;
    a = m + 102; idx = 160;
    while (idx > 3u) {
        *a = ((uint32_t)ac1->HuffVal[idx+1] << 24) | ((uint32_t)ac1->HuffVal[idx] << 16) |
             ((uint32_t)ac1->HuffVal[idx-1] << 8)  |  (uint32_t)ac1->HuffVal[idx-2];
        a--; idx -= 4u;
    }
}
#pragma GCC diagnostic pop

/* Program a quantization table (zigzag-ordered, scaled by quality 1..100). */
static void set_quant(const uint8_t *q, volatile uint32_t *dst, uint32_t quality)
{
    uint32_t scale, i = 0, j, row, val;

    if (quality >= 50u) scale = 200u - (quality * 2u);
    else                scale = 5000u / quality;

    while (i < (QUANT_SIZE - 3u)) {
        row = 0;
        for (j = 0; j < 4u; j++) {
            val = (((uint32_t)q[ZIGZAG[i + j]] * scale) + 50u) / 100u;
            if (val == 0u) val = 1u; else if (val > 255u) val = 255u;
            row |= (val & 0xFFu) << (8u * j);
        }
        i += 4u;
        *dst++ = row;
    }
}

/* ---- Color conversion ---- */

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static inline void rgb565_unpack(uint16_t px, int *r, int *g, int *b)
{
    int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* Build the 4:2:0 MCU-ordered YCbCr byte stream for the mcu_x×mcu_y grid of MCUs
   whose top-left pixel is (x0,y0) in fb (stride SCREEN_W):
   per MCU → 4 Y blocks (TL,TR,BL,BR), 1 Cb block, 1 Cr block; each 8×8 raster. */
static void build_ycc_region(const uint16_t *fb, uint32_t x0, uint32_t y0,
                             uint32_t mcu_x, uint32_t mcu_y)
{
    static const int yox[4] = {0, 8, 0, 8};
    static const int yoy[4] = {0, 0, 8, 8};
    uint8_t *p = YCC_BUF;
    uint32_t mx, my;
    int blk, yy, xx, cy, cx;

    for (my = 0; my < mcu_y; my++) {
        for (mx = 0; mx < mcu_x; mx++) {
            int px0 = (int)(x0 + mx * 16u), py0 = (int)(y0 + my * 16u);

            /* 4 luma blocks */
            for (blk = 0; blk < 4; blk++) {
                for (yy = 0; yy < 8; yy++) {
                    const uint16_t *row = fb + (py0 + yoy[blk] + yy) * SCREEN_W + px0 + yox[blk];
                    for (xx = 0; xx < 8; xx++) {
                        int r, g, b;
                        rgb565_unpack(row[xx], &r, &g, &b);
                        *p++ = clamp8((77 * r + 150 * g + 29 * b) >> 8);
                    }
                }
            }
            /* Cb block (2×2 averaged) */
            for (cy = 0; cy < 8; cy++) {
                for (cx = 0; cx < 8; cx++) {
                    int sr = 0, sg = 0, sb = 0, r, g, b, dx, dy;
                    for (dy = 0; dy < 2; dy++)
                        for (dx = 0; dx < 2; dx++) {
                            rgb565_unpack(fb[(py0 + 2*cy + dy) * SCREEN_W + px0 + 2*cx + dx], &r, &g, &b);
                            sr += r; sg += g; sb += b;
                        }
                    sr >>= 2; sg >>= 2; sb >>= 2;
                    *p++ = clamp8(((-43 * sr - 85 * sg + 128 * sb) >> 8) + 128);
                }
            }
            /* Cr block (2×2 averaged) */
            for (cy = 0; cy < 8; cy++) {
                for (cx = 0; cx < 8; cx++) {
                    int sr = 0, sg = 0, sb = 0, r, g, b, dx, dy;
                    for (dy = 0; dy < 2; dy++)
                        for (dx = 0; dx < 2; dx++) {
                            rgb565_unpack(fb[(py0 + 2*cy + dy) * SCREEN_W + px0 + 2*cx + dx], &r, &g, &b);
                            sr += r; sg += g; sb += b;
                        }
                    sr >>= 2; sg >>= 2; sb >>= 2;
                    *p++ = clamp8(((128 * sr - 107 * sg - 21 * sb) >> 8) + 128);
                }
            }
        }
    }
}

/* ---- Public API ---- */

void jpeg_enc_init(uint32_t quality)
{
    if (quality < QUALITY_MIN) quality = QUALITY_MIN;
    if (quality > QUALITY_MAX) quality = QUALITY_MAX;

    *RCC_AHB3ENR |= RCC_JPGDECEN;
    (void)*RCC_AHB3ENR;                /* readback: ensure the clock-enable took effect */
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
    for (volatile int d = 0; d < 64; d++) { } /* brief settle before register access */

    JPG->CR = CR_JCEN;                 /* enable the codec core */

    set_huff_dht();
    set_huff_ac(&AC_LUM,   JPG->HUFFENC_AC0);
    set_huff_ac(&AC_CHROM, JPG->HUFFENC_AC1);
    set_huff_dc(&DC_LUM,   JPG->HUFFENC_DC0);
    set_huff_dc(&DC_CHROM, JPG->HUFFENC_DC1);

    set_quant(LUM_Q,   JPG->QMEM0, quality);
    set_quant(CHROM_Q, JPG->QMEM1, quality);

    /* YCbCr 4:2:0 component config (Y: 2×2, Cb/Cr: 1×1).  Image geometry
       (CONFR1 height / CONFR3 width / CONFR2 MCU count) is set per encode. */
    JPG->CONFR4 = CONFR4_Y;
    JPG->CONFR5 = CONFR_CHROMA;
    JPG->CONFR6 = CONFR_CHROMA;
}

uint32_t jpeg_encode_region(const uint16_t *fb, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, uint8_t *out, uint32_t out_cap,
                            int with_header)
{
    const uint32_t mcu_x = w / MCU_PX;
    const uint32_t mcu_y = h / MCU_PX;
    const uint32_t num_mcu = mcu_x * mcu_y;
    const uint32_t ycc_words = (num_mcu * MCU_BYTES) / 4u;
    const uint32_t *in = (const uint32_t *)YCC_BUF;
    uint32_t in_idx = 0, out_n = 0, t0;

    if (mcu_x == 0u || mcu_y == 0u) return 0;

    build_ycc_region(fb, x, y, mcu_x, mcu_y);

    /* Program this region's geometry; emit the JPEG header markers only when asked. */
    JPG->CONFR1 = (h << 16) | (with_header ? CONFR1_HDR : 0u)
                | CONFR1_CS_0 | CONFR1_NF_1 | CONFR1_NS_1;
    JPG->CONFR3 = (w << 16);
    JPG->CONFR2 = (num_mcu - 1u);

    /* Init the encode pass: stop, flush FIFOs, clear flags, start. */
    JPG->CONFR0 &= ~CONFR0_START;
    JPG->CR |= CR_IFF | CR_OFF;
    JPG->CFR  = CFR_ALL;
    JPG->CONFR0 |= CONFR0_START;

    t0 = *DWT_CYCCNT;
    for (;;) {
        uint32_t sr = JPG->SR;

        /* End of conversion: EOC set and output FIFO fully drained. */
        if ((sr & (SR_EOCF | SR_OFTF | SR_OFNEF)) == SR_EOCF) break;

        if (sr & SR_OFNEF) {
            uint32_t w = JPG->DOR;
            if (out_n + 4u > out_cap) { JPG->CONFR0 &= ~CONFR0_START; return 0; }
            out[out_n]     = (uint8_t)(w);
            out[out_n + 1] = (uint8_t)(w >> 8);
            out[out_n + 2] = (uint8_t)(w >> 16);
            out[out_n + 3] = (uint8_t)(w >> 24);
            out_n += 4u;
            t0 = *DWT_CYCCNT;
        } else if ((sr & SR_IFNFF) && in_idx < ycc_words) {
            JPG->DIR = in[in_idx++];
            t0 = *DWT_CYCCNT;
        }

        if ((*DWT_CYCCNT - t0) >= ENCODE_TIMEOUT_CYCS) {
            JPG->CONFR0 &= ~CONFR0_START;
            return 0;
        }
    }

    JPG->CONFR0 &= ~CONFR0_START;
    return out_n;
}

uint32_t jpeg_encode_full(const uint16_t *fb, uint8_t *out, uint32_t out_cap)
{
    return jpeg_encode_region(fb, 0u, 0u, SCREEN_W, SCREEN_H, out, out_cap, 1);
}
