/*
 * Host harness for the Picture Viewer's streaming BMP decoder (src/modules/features/picture/
 * bmp.c). Decodes a .bmp to a binary RGB565-expanded PPM so bmp_verify.py can diff it against a
 * reference decode -- this proves the device-bound decoder on the build host. Mirrors
 * png_decode_host.c.
 *
 *   gcc bmp_decode_host.c ../../src/modules/features/picture/bmp.c \
 *       -I../../src/modules/features/picture -o bmp_decode_host
 *   ./bmp_decode_host in.bmp out.ppm
 */
#include "bmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_in;
static size_t rdf(void *dev, uint8_t *buf, size_t len) { (void)dev; return fread(buf, 1, len, g_in); }

static uint16_t *g_img;
static int rowcb(void *u, int y, const uint16_t *row, int width) {
    bmp_dec_t *d = (bmp_dec_t *)u;
    if (!g_img) g_img = calloc((size_t)d->width * d->height, sizeof(uint16_t));
    if (g_img && y < d->height)
        memcpy(g_img + (size_t)y * d->width, row, (size_t)width * sizeof(uint16_t));
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.bmp out.ppm\n", argv[0]); return 2; }
    g_in = fopen(argv[1], "rb");
    if (!g_in) { perror("open"); return 2; }

    bmp_dec_t d;
    memset(&d, 0, sizeof d);
    d.read = rdf; d.dev = NULL;
    d.scratch = malloc(bmp_scratch_size());
    d.scratch_size = bmp_scratch_size();
    d.row_cb = rowcb; d.user = &d;

    bmp_result_t r = bmp_decode(&d);
    if (r != BMP_OK) { fprintf(stderr, "bmp_decode error %d\n", (int)r); return 1; }

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("out"); return 2; }
    fprintf(o, "P6\n%d %d\n255\n", d.width, d.height);
    for (size_t i = 0; i < (size_t)d.width * d.height; i++) {
        uint16_t p = g_img ? g_img[i] : 0;
        int r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
        fputc((r5 << 3) | (r5 >> 2), o);
        fputc((g6 << 2) | (g6 >> 4), o);
        fputc((b5 << 3) | (b5 >> 2), o);
    }
    fclose(o);
    fprintf(stderr, "ok %dx%d\n", d.width, d.height);
    return 0;
}
