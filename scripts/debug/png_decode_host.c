/*
 * Host harness for the Picture Viewer's streaming PNG decoder (src/modules/features/picture/
 * png.c). Decodes a .png to a binary RGB565 PPM-ish file so png_verify.py can diff it against
 * a reference (PIL) decode -- this lets the device-bound decoder be proven on the build host.
 *
 *   gcc png_decode_host.c ../../src/modules/features/picture/png.c \
 *       ../../src/modules/features/picture/miniz.c -I../../src/modules/features/picture \
 *       -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_APIS \
 *       -DMINIZ_NO_MALLOC -o png_decode_host
 *   ./png_decode_host in.png out.ppm
 */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_in;
static size_t rdf(void *dev, uint8_t *buf, size_t len) { (void)dev; return fread(buf, 1, len, g_in); }

static uint16_t *g_img;
static int rowcb(void *u, int y, const uint16_t *row, int width) {
    png_dec_t *d = (png_dec_t *)u;
    if (!g_img) g_img = calloc((size_t)d->width * d->height, sizeof(uint16_t));
    if (g_img && y < d->height)
        memcpy(g_img + (size_t)y * d->width, row, (size_t)width * sizeof(uint16_t));
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.png out.ppm\n", argv[0]); return 2; }
    g_in = fopen(argv[1], "rb");
    if (!g_in) { perror("open"); return 2; }

    png_dec_t d;
    memset(&d, 0, sizeof d);
    d.read = rdf; d.dev = NULL;
    d.scratch = malloc(png_scratch_size());
    d.scratch_size = png_scratch_size();
    d.row_cb = rowcb; d.user = &d;

    png_result_t r = png_decode(&d);
    if (r != PNG_OK) { fprintf(stderr, "png_decode error %d\n", (int)r); return 1; }

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
