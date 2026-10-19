/* port/tests/texswz_tests.c — N60TEX: gdx3ds_swz_rows_fast == gdx3ds_swz_rows_ref, bit for bit,
 * over every extent 1..64 x 1..64 and a few large ones, on random content, with the padding
 * pre-filled with a sentinel (a store outside the content region would show up). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_texswz.h"

static uint32_t sRng = 0x12345678u;

static uint32_t NextRandom(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static uint32_t NextPow2Min8(uint32_t v) {
    uint32_t p = 8;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static void FillRandom(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)NextRandom();
    }
}

static void FillSentinel(uint32_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = 0xDEADBEEFu ^ (uint32_t)i;
    }
}

/* One extent: 0 when both writers leave identical buffers. */
static int CheckExtent(uint32_t w, uint32_t h) {
    const uint32_t padW = NextPow2Min8(w);
    const uint32_t padH = NextPow2Min8(h);
    const size_t texels = (size_t)padW * padH;
    uint8_t* src = malloc((size_t)w * h * 4);
    uint32_t* a = malloc(texels * 4);
    uint32_t* b = malloc(texels * 4);
    int bad = 0;
    FillRandom(src, (size_t)w * h * 4);
    FillSentinel(a, texels);
    FillSentinel(b, texels);
    gdx3ds_swz_rows_ref(a, src, w, h, padW, padH);
    gdx3ds_swz_rows_fast(b, src, w, h, padW, padH);
    if (memcmp(a, b, texels * 4) != 0) {
        fprintf(stderr, "MISMATCH %ux%u (pad %ux%u)\n", w, h, padW, padH);
        bad = 1;
    }
    free(src);
    free(a);
    free(b);
    return bad;
}

/* One atlas cell: 0 when both cell writers leave identical pages. */
static int CheckCell(uint32_t w, uint32_t h, uint32_t px, uint32_t py) {
    const uint32_t pageW = 256, pageH = 256, gutter = 1;
    const size_t texels = (size_t)pageW * pageH;
    uint8_t* src = malloc((size_t)w * h * 4);
    uint32_t* a = malloc(texels * 4);
    uint32_t* b = malloc(texels * 4);
    int bad = 0;
    FillRandom(src, (size_t)w * h * 4);
    FillSentinel(a, texels);
    FillSentinel(b, texels);
    gdx3ds_swz_cell_ref(a, src, w, h, px, py, gutter, pageW, pageH);
    gdx3ds_swz_cell_fast(b, src, w, h, px, py, gutter, pageW, pageH);
    if (memcmp(a, b, texels * 4) != 0) {
        fprintf(stderr, "CELL MISMATCH %ux%u at %u,%u\n", w, h, px, py);
        bad = 1;
    }
    free(src);
    free(a);
    free(b);
    return bad;
}

int main(void) {
    static const uint32_t kLarge[][2] = { { 320, 240 }, { 1024, 8 }, { 8, 1024 }, { 200, 100 }, { 512, 512 } };
    unsigned checks = 0;
    unsigned failures = 0;
    for (uint32_t w = 1; w <= 64; w++) {
        for (uint32_t h = 1; h <= 64; h++) {
            failures += (unsigned)CheckExtent(w, h);
            checks++;
        }
    }
    for (size_t i = 0; i < sizeof(kLarge) / sizeof(kLarge[0]); i++) {
        failures += (unsigned)CheckExtent(kLarge[i][0], kLarge[i][1]);
        checks++;
    }
    for (uint32_t w = 1; w <= 40; w += 3) {
        for (uint32_t h = 1; h <= 40; h += 5) {
            for (uint32_t px = 0; px < 17; px += 4) {
                failures += (unsigned)CheckCell(w, h, px, (px * 7u) % 50u);
                checks++;
            }
        }
    }
    printf("texswz_tests: %u extents/cells, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
