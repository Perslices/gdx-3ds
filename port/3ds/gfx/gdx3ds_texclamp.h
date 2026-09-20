/* gdx3ds_texclamp.h — the pow2-padding / atlas-view UV clamp contract, in one place.
 *
 * Every texture this backend binds is either
 *   (a) a STANDALONE C3D_Tex padded to PICA pow2/min-8 (uScale = extent / padded), or
 *   (b) a [trectbatch] atlas VIEW: a cell inside a 512x256 page, where
 *       uScale = cellExtent / pageExtent and uOff = cellOrigin / pageExtent.
 *
 * Both are the same expression with different "padded" and a non-zero offset, and all four
 * packers that emit PICA UVs -- gdx3ds_uv_pack (mesh/[trifast]), the interpreter's packed
 * append (vendor/lus/src/fast/interpreter.cpp), gdx_tri.cpp (owned interpreter) and
 * DrawTriangles' legacy repack -- apply them in this order:
 *
 *     uv_final = min(uv_normalised * uScale, clampLimit) + uOff
 *
 * THE MIN COMES FIRST. The clamp limit therefore lives in PRE-OFFSET space (an atlas view's
 * limit is measured from the CELL's own origin, not from the page's), which is why
 * AtlasFillTexParams must NOT add uOff into it: doing so would double the offset and walk
 * the sample out of the cell entirely. port/tests/gfx_atlas_uv_tests.cpp pins that.
 */
#ifndef GDX3DS_TEXCLAMP_H
#define GDX3DS_TEXCLAMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Content-edge clamp limit in padded, PRE-OFFSET UV space: the centre of the last content
 * texel, (extent - 0.5) / padded. Multiplying it back by `padded` gives extent - 0.5, so a
 * bilinear fetch at the limit puts weight 1 on the last content texel and weight 0 on the
 * first padding texel -- the N64 G_TX_CLAMP result. `padded` is the pow2 padded extent for a
 * standalone texture and the ATLAS PAGE extent for a view. */
static inline float gdx3ds_pad_clamp_limit(uint32_t extent, uint32_t padded) {
    return ((float)extent - 0.5f) / (float)padded;
}

/* ATLASUV -- an atlas VIEW's content-edge limit is whatever its STANDALONE twin's is.
 *
 * A coordinate clamp is not a tap clamp. min()ing a rect's FAR vertex rescales the WHOLE
 * quad's UV interpolation, not just the sliver past the edge. [trectbatch] is a pure
 * performance lever, so a view has to reproduce the standalone path's limit exactly --
 * including where that limit itself shears the quad -- or trectbatch=0 and =1 differ.
 *
 * What the standalone path does, per axis:
 *   - extent < pow2-padded extent: limit = (extent - 0.5) / padded. This DOES pull the far
 *     vertex in (a 3-tall HUD strip interpolates V over 0..2.5, not 0..3) and F-Zero X's
 *     race HUD gradient strips are all of this kind.
 *   - extent == padded, i.e. any POW2 axis >= 8 -- which is every HUD glyph (8x8, 16x32,
 *     32x32): NO limit at all. The far vertex keeps u = extent and the PICA's own
 *     GPU_CLAMP_TO_EDGE clamps the TAPS at the texture edge.
 *
 * For a view, `extent < padded` is trivially true (padded is the 512x256 PAGE), so the
 * pre-fix code clamped every view at extent - 0.5 -- including the pow2 glyphs whose
 * standalone twin is unclamped. A 16-texel glyph's far vertex went 16.0 -> 15.5, squeezing
 * the quad's texel span by 3% and sliding the sampled content right by up to half a texel:
 * the 782-px course-name residual (docs/research/native/atlasuv-progress.md).
 *
 * gdx3ds_view_clamp_limit therefore asks what the STANDALONE pad would have been:
 *   - padded axis  -> (extent - 0.5) / page, the same shear, bit-for-bit.
 *   - pow2 axis    -> extent / page, the cell's far EDGE. That is a no-op for every
 *     coordinate the standalone leaves alone, and past it the view's two taps are the last
 *     content texel and its own REPLICATED GUTTER copy, so the blend returns that texel --
 *     exactly GPU_CLAMP_TO_EDGE. Unlike no limit at all it also keeps a wild coordinate out
 *     of the NEXT cell on the page.
 * Pinned by port/tests/gfx_atlas_uv_tests.cpp.
 */
static inline uint32_t gdx3ds_standalone_padded(uint32_t extent) {
    uint32_t v = extent < 8u ? 8u : extent;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1u;
}

static inline float gdx3ds_view_clamp_limit(uint32_t extent, uint32_t page) {
    const float edge = (extent < gdx3ds_standalone_padded(extent)) ? ((float)extent - 0.5f)
                                                                   : (float)extent;
    return edge / (float)page;
}

/* Does a STANDALONE axis need a content-edge clamp? (A view always takes
 * gdx3ds_view_clamp_limit instead; see above.) */
static inline int gdx3ds_pad_clamp_needed(uint32_t extent, uint32_t padded) {
    return extent < padded;
}

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_TEXCLAMP_H */
