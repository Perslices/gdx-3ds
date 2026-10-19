/* port/3ds/gfx/gdx3ds_texswz.h — N60TEX: the RGBA32 -> PICA tiled (Morton, bottom-up) texel
 * writers of GfxRenderingAPIC3D::UploadTexture, as two interchangeable functions.
 *
 * gdx3ds_swz_rows_ref   the stock per-texel loop: four byte loads, a shift/or, a Morton lookup
 *                       and a tile multiply per texel.
 * gdx3ds_swz_rows_fast  the same stores, same values, same addresses ([native] tex_fast=1): one
 *                       word load + byte reverse per texel, and the tile base / Morton row term
 *                       hoisted per row and per 8-texel tile, the eight x terms unrolled.
 *
 * Exactness: for every (x, y) both write dst[((dy>>3)*tpr + (x>>3))*64 | xlut[x&7] | ylut[dy&7]]
 * = R<<24|G<<16|B<<8|A with dy = padH-1-y; the x and y Morton terms use disjoint bits, so the
 * fast form's `base + xlut[k]` equals the reference's `|`. port/tests/texswz_tests.c compares
 * the two on random content over every extent up to 64x64 plus large ones (CI host test).
 *
 * Header-only and libctru-free so the host test compiles it unchanged. */
#ifndef GDX3DS_TEXSWZ_H
#define GDX3DS_TEXSWZ_H

#include <stdint.h>
#include <string.h>

static const uint32_t kGdxSwzXlut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static const uint32_t kGdxSwzYlut[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };

static inline uint32_t gdx3ds_swz_texel_bytes(const uint8_t* px) {
    return ((uint32_t)px[0] << 24) | ((uint32_t)px[1] << 16) | ((uint32_t)px[2] << 8) | px[3];
}

/* R,G,B,A bytes in memory -> R<<24|G<<16|B<<8|A: a little-endian word load, byte-reversed. */
static inline uint32_t gdx3ds_swz_texel_word(const uint8_t* px) {
    uint32_t w;
    memcpy(&w, px, 4);
    return __builtin_bswap32(w);
}

static inline void gdx3ds_swz_rows_ref(uint32_t* dst, const uint8_t* rgba32, uint32_t width, uint32_t height,
                                       uint32_t padW, uint32_t padH) {
    const uint32_t tilesPerRow = padW >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* srcRow = rgba32 + (size_t)y * width * 4;
        const uint32_t dy = padH - 1 - y;
        const uint32_t tileY = dy >> 3;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t offset = ((tileY * tilesPerRow + (x >> 3)) << 6) | kGdxSwzXlut[x & 7] | kGdxSwzYlut[dy & 7];
            dst[offset] = gdx3ds_swz_texel_bytes(srcRow + (size_t)x * 4);
        }
    }
}

/* Eight texels of one row into one tile row: `rowTile` = the tile's base | the row's y term. */
static inline void gdx3ds_swz_tile_row8(uint32_t* rowTile, const uint8_t* src) {
    rowTile[0x00] = gdx3ds_swz_texel_word(src + 0);
    rowTile[0x01] = gdx3ds_swz_texel_word(src + 4);
    rowTile[0x04] = gdx3ds_swz_texel_word(src + 8);
    rowTile[0x05] = gdx3ds_swz_texel_word(src + 12);
    rowTile[0x10] = gdx3ds_swz_texel_word(src + 16);
    rowTile[0x11] = gdx3ds_swz_texel_word(src + 20);
    rowTile[0x14] = gdx3ds_swz_texel_word(src + 24);
    rowTile[0x15] = gdx3ds_swz_texel_word(src + 28);
}

static inline void gdx3ds_swz_row_fast(uint32_t* dst, const uint8_t* srcRow, uint32_t width, uint32_t rowBase) {
    uint32_t x = 0;
    for (; x + 8u <= width; x += 8u) {
        gdx3ds_swz_tile_row8(dst + rowBase + (x << 3), srcRow + (size_t)x * 4);
    }
    for (; x < width; x++) {
        dst[rowBase + ((x >> 3) << 6) + kGdxSwzXlut[x & 7]] = gdx3ds_swz_texel_word(srcRow + (size_t)x * 4);
    }
}

static inline void gdx3ds_swz_rows_fast(uint32_t* dst, const uint8_t* rgba32, uint32_t width, uint32_t height,
                                        uint32_t padW, uint32_t padH) {
    const uint32_t tilesPerRow = padW >> 3;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t dy = padH - 1 - y;
        const uint32_t rowBase = (((dy >> 3) * tilesPerRow) << 6) + kGdxSwzYlut[dy & 7];
        gdx3ds_swz_row_fast(dst, rgba32 + (size_t)y * width * 4, width, rowBase);
    }
}


/* ---- the [trectbatch] atlas cell: content + a replicated gutter at an arbitrary page position ----
 * Cell texel (gx, gy), gx in [-g, w+g), gy in [-g, h+g), samples the content at the clamped
 * (sx, sy) and lands at page column px+g+gx, page row py+g+gy (stored bottom-up). */

static inline void gdx3ds_swz_cell_ref(uint32_t* dst, const uint8_t* rgba32, uint32_t w, uint32_t h, uint32_t px,
                                       uint32_t py, uint32_t gutter, uint32_t pageW, uint32_t pageH) {
    const uint32_t tilesPerRow = pageW >> 3;
    for (int gy = -(int)gutter; gy < (int)(h + gutter); gy++) {
        const uint32_t sy = (uint32_t)(gy < 0 ? 0 : (gy >= (int)h ? (int)h - 1 : gy));
        const uint8_t* srcRow = rgba32 + (size_t)sy * w * 4;
        const uint32_t dy = pageH - 1 - (py + gutter + (uint32_t)gy);
        const uint32_t tileY = dy >> 3;
        for (int gx = -(int)gutter; gx < (int)(w + gutter); gx++) {
            const uint32_t sx = (uint32_t)(gx < 0 ? 0 : (gx >= (int)w ? (int)w - 1 : gx));
            const uint32_t x = px + gutter + (uint32_t)gx;
            const uint32_t offset = ((tileY * tilesPerRow + (x >> 3)) << 6) | kGdxSwzXlut[x & 7] | kGdxSwzYlut[dy & 7];
            dst[offset] = gdx3ds_swz_texel_bytes(srcRow + (size_t)sx * 4);
        }
    }
}

static inline uint32_t gdx3ds_swz_col_offset(uint32_t rowBase, uint32_t x) {
    return rowBase + ((x >> 3) << 6) + kGdxSwzXlut[x & 7];
}

/* `n` source texels of one row to page columns x0..x0+n-1: single texels up to a tile edge,
 * whole tile rows, single texels after. */
static inline void gdx3ds_swz_span_fast(uint32_t* dst, const uint8_t* src, uint32_t x0, uint32_t n, uint32_t rowBase) {
    uint32_t x = x0;
    const uint32_t end = x0 + n;
    for (; x < end && (x & 7u) != 0; x++, src += 4) {
        dst[gdx3ds_swz_col_offset(rowBase, x)] = gdx3ds_swz_texel_word(src);
    }
    for (; x + 8u <= end; x += 8u, src += 32) {
        gdx3ds_swz_tile_row8(dst + rowBase + (x << 3), src);
    }
    for (; x < end; x++, src += 4) {
        dst[gdx3ds_swz_col_offset(rowBase, x)] = gdx3ds_swz_texel_word(src);
    }
}

static inline void gdx3ds_swz_cell_fast(uint32_t* dst, const uint8_t* rgba32, uint32_t w, uint32_t h, uint32_t px,
                                        uint32_t py, uint32_t gutter, uint32_t pageW, uint32_t pageH) {
    const uint32_t tilesPerRow = pageW >> 3;
    const uint32_t x0 = px + gutter;
    for (int gy = -(int)gutter; gy < (int)(h + gutter); gy++) {
        const uint32_t sy = (uint32_t)(gy < 0 ? 0 : (gy >= (int)h ? (int)h - 1 : gy));
        const uint8_t* srcRow = rgba32 + (size_t)sy * w * 4;
        const uint32_t dy = pageH - 1 - (py + gutter + (uint32_t)gy);
        const uint32_t rowBase = (((dy >> 3) * tilesPerRow) << 6) + kGdxSwzYlut[dy & 7];
        const uint32_t leftTexel = gdx3ds_swz_texel_word(srcRow);
        const uint32_t rightTexel = gdx3ds_swz_texel_word(srcRow + (size_t)(w - 1) * 4);
        for (uint32_t g = 1; g <= gutter; g++) {
            dst[gdx3ds_swz_col_offset(rowBase, x0 - g)] = leftTexel;
            dst[gdx3ds_swz_col_offset(rowBase, x0 + w - 1 + g)] = rightTexel;
        }
        gdx3ds_swz_span_fast(dst, srcRow, x0, w, rowBase);
    }
}

#endif /* GDX3DS_TEXSWZ_H */
