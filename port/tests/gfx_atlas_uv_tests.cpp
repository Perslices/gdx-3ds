// gfx_atlas_uv_tests.cpp -- the pow2-padding / [trectbatch] atlas-view UV contract
// (RESIDUALS-2 items 2 and 3).
//
// Standalone console exe: no citro3d, no libultraship. It drives the REAL production code
// -- gdx3ds_pad_clamp_limit (port/3ds/gfx/gdx3ds_texclamp.h, what AtlasFillTexParams and
// DrawTriangles' legacy repack call) and gdx3ds_uv_prep / gdx3ds_uv_pack
// (port/3ds/gfx/gdx3ds_vtx_math.h, the packer the mesh path runs and the interpreter's
// packed append / gdx_tri.cpp transcribe line for line) -- and pins:
//
//   1. STANDALONE clamp: a UV past the content edge lands exactly on the last content
//      texel's CENTRE, so bilinear puts weight 0 on the first padding texel.
//   2. GUTTER WIDTH: an atlas-eligible rect (its texel window inside the tile window)
//      can reach at most texel `extent` -- one past the content -- so the 1-texel
//      replicated gutter the atlas writes, and the one UploadTexture now writes into the
//      pow2 padding, is exactly wide enough. This is the [padgutter] fix's premise.
//   3. ATLAS VIEW: the clamp limit is measured in PRE-OFFSET space, because every packer
//      does min() first and adds uOff after. A view's sample therefore stays inside its
//      own cell.
//   4. NEGATIVE CONTROL for the "AtlasFillTexParams forgets to add uOff" report: adding
//      uOff into the limit walks the sample far outside the cell. This check is what
//      would have caught that change had it been made.
//   5. UNDERSHOOT: an eligible rect's lowest UV stays inside the cell's 1-texel gutter,
//      which is why no lower clamp is needed for views.
//   6. PARITY: standalone and atlas view of the same content resolve to the same
//      content-relative texel coordinate -- the invariant behind "trectbatch=0 and
//      trectbatch=1 must render the same pixels".
//
// Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "gdx3ds_texclamp.h"
#include "gdx3ds_vtx_math.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(const char* name, bool ok, double got, double want) {
    ++g_checks;
    if (ok) {
        printf("[ OK ] %-62s got=%.6f\n", name, got);
    } else {
        printf("[FAIL] %-62s got=%.6f want=%.6f\n", name, got, want);
        ++g_failures;
    }
}

static void check_near(const char* name, float got, float want, float tol) {
    check(name, std::fabs(got - want) <= tol, got, want);
}

static void check_true(const char* name, bool ok, double got) {
    check(name, ok, got, 0.0);
}

static const float kNoClamp = 1e30f;

// One packed UV, exactly as the shipping packers derive it. `padded` is the pow2 padded
// extent for a standalone texture and the ATLAS PAGE extent for a view.
struct UvCase {
    uint32_t texW, texH;     // decoded content extent
    uint32_t padW, padH;     // padded (standalone) or page (view) extent
    uint32_t axTexels;       // cell origin in page texels (0 for standalone)
    bool view;
    int bilerp;
};

// Returns the final UV in PADDED/PAGE texel units (uv * padded), which is what the PICA
// sampler indexes with.
static float pack_u_texels(const UvCase& c, float sTexels, float clampLimit) {
    Gdx3dsUvPrep prep;
    gdx3ds_uv_prep(&prep, 0, 0, 0, 0, c.texW, c.texH, c.bilerp);
    const float uOff = c.view ? (float)c.axTexels / (float)c.padW : 0.0f;
    float outU = 0.0f, outV = 0.0f;
    gdx3ds_uv_pack(&prep, sTexels * 32.0f, 0.0f, (float)c.texW / (float)c.padW,
                   (float)c.texH / (float)c.padH, clampLimit, kNoClamp, c.view ? 1 : 0, uOff, 0.0f,
                   &outU, &outV);
    return outU * (float)c.padW;
}

// Bilinear weight the PICA puts on texel index `idx` when sampling at texel coordinate `c`.
static float bilinear_weight(float coord, int idx) {
    const float f = coord - 0.5f;
    const int i0 = (int)std::floor(f);
    const float frac = f - (float)i0;
    if (idx == i0) {
        return 1.0f - frac;
    }
    if (idx == i0 + 1) {
        return frac;
    }
    return 0.0f;
}

int main(void) {
    printf("gdx_gfx_atlas_uv_tests -- pow2-pad / atlas-view UV clamp contract\n\n");

    // ---- 1. standalone clamp lands on the last content texel's centre -----------------
    {
        const UvCase sa = { 12, 5, 16, 8, 0, false, 1 };
        const float limU = gdx3ds_pad_clamp_limit(sa.texW, sa.padW);
        check_near("standalone limit * padded == extent - 0.5", limU * (float)sa.padW, 11.5f, 1e-5f);
        // s = 12 is the rect's right edge: u = 12, +0.5 half-texel = 12.5 -> texel 12.5,
        // which is INSIDE the padding. The clamp must pull it back to 11.5.
        const float unclamped = pack_u_texels(sa, 12.0f, kNoClamp);
        const float clamped = pack_u_texels(sa, 12.0f, limU);
        check_true("standalone: unclamped right edge reaches the padding", unclamped > 11.5f, unclamped);
        check_near("standalone: clamped right edge == last content texel centre", clamped, 11.5f, 1e-5f);
        check_near("standalone: bilinear weight on first pad texel at the limit", bilinear_weight(clamped, 12),
                   0.0f, 1e-6f);
        check_near("standalone: bilinear weight on last content texel at the limit", bilinear_weight(clamped, 11),
                   1.0f, 1e-6f);
    }

    // ---- 2. one gutter texel is enough for an atlas-eligible rect ---------------------
    // GdxTrectRectEligible admits a rect only if its texel window sits inside
    // [uls, lrs + 1]; with uls = 0 that is s <= texW, so the packed coordinate is at most
    // texW + 0.5 under bilerp -- texel index texW, and no further.
    {
        const UvCase sa = { 12, 5, 16, 8, 0, false, 1 };
        const float worst = pack_u_texels(sa, (float)sa.texW, kNoClamp);
        check_near("eligible rect's worst unclamped coord == extent + 0.5", worst, 12.5f, 1e-5f);
        check_near("  -> weight on gutter texel [extent]", bilinear_weight(worst, 12), 1.0f, 1e-6f);
        check_near("  -> weight on the SECOND pad texel is zero", bilinear_weight(worst, 13), 0.0f, 1e-6f);
    }

    // ---- 3. atlas view: the clamp limit is pre-offset, the sample stays in the cell ----
    {
        const UvCase view = { 12, 5, 512, 256, 33, true, 1 };
        const float limU = gdx3ds_pad_clamp_limit(view.texW, view.padW); // (12 - 0.5) / 512
        check_near("view limit * page == cell extent - 0.5", limU * (float)view.padW, 11.5f, 1e-5f);
        const float atEdge = pack_u_texels(view, 12.0f, limU);
        check_near("view: right edge == ax + cell extent - 0.5", atEdge, 33.0f + 11.5f, 1e-4f);
        const float wayOver = pack_u_texels(view, 100.0f, limU);
        check_near("view: a wildly over-range UV still lands on the cell's last texel", wayOver, 44.5f, 1e-4f);
        check_true("view: sample stays inside the cell [ax, ax + w)", wayOver < 33.0f + 12.0f, wayOver);

        // ---- 4. NEGATIVE CONTROL -----------------------------------------------------
        // "AtlasFillTexParams computes wrapClampU from the page extent without adding
        // uOff" reads like a bug, but the packers add uOff AFTER the min. Folding uOff
        // into the limit double-counts it: prove the sample then escapes the cell.
        const float bogusLim = limU + (float)view.axTexels / (float)view.padW;
        const float escaped = pack_u_texels(view, 100.0f, bogusLim);
        check_true("negative control: limit + uOff escapes the cell AND its gutter",
                   escaped > 33.0f + 12.0f + 1.0f, escaped);
        check_true("negative control: it escapes by roughly ax texels",
                   std::fabs(escaped - (44.5f + 33.0f)) < 1e-3f, escaped);
    }

    // ---- 5. undershoot stays inside the 1-texel gutter --------------------------------
    // Eligibility allows the rect's window to start 0.01 texels before uls, and POINT
    // filtering adds no half-texel bias, so the lowest reachable coordinate is ax - 0.01:
    // inside the replicated gutter column at ax - 1. No lower clamp is needed.
    {
        const UvCase viewPoint = { 12, 5, 512, 256, 33, true, 0 };
        const float limU = gdx3ds_pad_clamp_limit(viewPoint.texW, viewPoint.padW);
        const float low = pack_u_texels(viewPoint, -0.01f, limU);
        check_true("view: eligible undershoot stays within the 1-texel gutter",
                   low >= 32.0f && low <= 33.0f, low);
    }

    // ---- 6. standalone and atlas view resolve to the same content-relative texel -------
    {
        const UvCase sa = { 12, 5, 16, 8, 0, false, 1 };
        const UvCase view = { 12, 5, 512, 256, 33, true, 1 };
        const float limSa = gdx3ds_pad_clamp_limit(sa.texW, sa.padW);
        const float limVw = gdx3ds_pad_clamp_limit(view.texW, view.padW);
        static const float kS[] = { 0.0f, 1.0f, 5.5f, 11.0f, 12.0f, 100.0f };
        bool allEqual = true;
        float worstDelta = 0.0f;
        for (unsigned i = 0; i < sizeof(kS) / sizeof(kS[0]); i++) {
            const float a = pack_u_texels(sa, kS[i], limSa);
            const float b = pack_u_texels(view, kS[i], limVw) - (float)view.axTexels;
            const float d = std::fabs(a - b);
            if (d > worstDelta) {
                worstDelta = d;
            }
            if (d > 1e-4f) {
                allEqual = false;
            }
        }
        check_true("standalone vs view: same content-relative texel for every sample", allEqual, worstDelta);
    }

    // ---- 7. a pow2 axis has no padding and therefore no clamp -------------------------
    {
        const UvCase pow2 = { 16, 8, 16, 8, 0, false, 1 };
        check_near("pow2 axis: uScale is the identity", (float)pow2.texW / (float)pow2.padW, 1.0f, 0.0f);
        // AtlasFillTexParams gates its limit on width < padWidth, so nothing is clamped
        // here; the mirror seam sits at the logical edge (shadow2-mirror-period-audit.md).
        const float v = pack_u_texels(pow2, 8.0f, kNoClamp);
        check_near("pow2 axis: UV passes through unscaled", v, 8.5f, 1e-5f);
    }

    // ---- 8. ATLASUV: a view must not take the content-edge COORDINATE clamp ----------
    // The 782-px course-name residual. Every HUD glyph is pow2 on both axes (8x8, 16x32,
    // 32x32), so its STANDALONE texture has no padding and `extent < padded` is false: no
    // limit is applied and the rect's far vertex keeps u = extent, with the PICA's
    // GPU_CLAMP_TO_EDGE clamping the TAPS. For the same content as an atlas VIEW `extent <
    // padded` is trivially true (padded is the 512-wide PAGE), so the old gate clamped the
    // far vertex to extent - 0.5 -- and a min() on a VERTEX rescales the whole quad's UV
    // interpolation, not just the sliver past the edge.
    {
        // bilerp=0: texture RECTANGLES take no half-texel offset (sGdxRectHalfTexelDisabled
        // is default-on), and rects are the only thing that ever samples a view. Measured on
        // hardware-path receipts as cs=0..16.0 standalone vs cs=0..15.5 as a view.
        const UvCase saPow2 = { 16, 32, 16, 32, 0, false, 0 };
        const UvCase vwPow2 = { 16, 32, 512, 256, 53, true, 0 };
        check_true("gate: pow2 standalone axis needs no content clamp",
                   gdx3ds_pad_clamp_needed(saPow2.texW, saPow2.padW) == 0, 0.0);
        check_true("gate: padded standalone axis still needs one",
                   gdx3ds_pad_clamp_needed(12u, 16u) == 1, 0.0);
        check_near("view limit is the cell's far EDGE, not the last texel centre",
                   gdx3ds_view_clamp_limit(vwPow2.texW, vwPow2.padW) * (float)vwPow2.padW,
                   16.0f, 1e-4f);

        // The measured defect, reproduced: the far vertex of a 16-texel glyph.
        const float saFar = pack_u_texels(saPow2, 16.0f, kNoClamp);
        const float vwFarOld = pack_u_texels(vwPow2, 16.0f, gdx3ds_pad_clamp_limit(16u, 512u)) -
                               (float)vwPow2.axTexels;
        const float vwFarNew = pack_u_texels(vwPow2, 16.0f,
                                              gdx3ds_view_clamp_limit(vwPow2.texW, vwPow2.padW)) -
                               (float)vwPow2.axTexels;
        check_near("regression: standalone glyph far vertex == 16.0", saFar, 16.0f, 1e-4f);
        check_near("regression: OLD view far vertex was 15.5 (the shear)", vwFarOld, 15.5f, 1e-4f);
        check_near("fixed: view far vertex == standalone far vertex", vwFarNew, saFar, 1e-4f);

        // Whole-rect parity: an eligible rect's UVs interpolate between its two vertices, so
        // every sample across the quad must agree, not just the endpoints.
        bool allEqual = true;
        float worstDelta = 0.0f;
        for (int k = 0; k <= 64; k++) {
            const float sT = 16.0f * (float)k / 64.0f;
            const float a = pack_u_texels(saPow2, sT, kNoClamp);
            const float b = pack_u_texels(vwPow2, sT,
                                          gdx3ds_view_clamp_limit(vwPow2.texW, vwPow2.padW)) -
                            (float)vwPow2.axTexels;
            const float d = std::fabs(a - b);
            if (d > worstDelta) {
                worstDelta = d;
            }
            if (d > 1e-4f) {
                allEqual = false;
            }
        }
        check_true("fixed: pow2 glyph - view matches standalone across the WHOLE rect",
                   allEqual, worstDelta);

        // And the old behaviour really did shear it (negative control for the fix).
        float worstOld = 0.0f;
        for (int k = 0; k <= 64; k++) {
            const float sT = 16.0f * (float)k / 64.0f;
            const float a = pack_u_texels(saPow2, sT, kNoClamp);
            const float b = pack_u_texels(vwPow2, sT, gdx3ds_pad_clamp_limit(16u, 512u)) -
                            (float)vwPow2.axTexels;
            worstOld = std::fabs(a - b) > worstOld ? std::fabs(a - b) : worstOld;
        }
        check_true("negative control: the OLD view clamp sheared samples by half a texel",
                   worstOld > 0.49f && worstOld < 0.51f, worstOld);
    }

    // ---- 9. a PADDED (non-pow2) axis: edge-clamped view == centre-clamped standalone -----
    // Moving the view's limit out to the cell edge must not regress the case where the
    // standalone DOES clamp. It does not: over the eligibility window the two either land on
    // the same coordinate, or both resolve to full weight on the last content texel (the view
    // because its gutter texel is a copy of that texel, the standalone because its limit is
    // that texel's centre).
    {
        const UvCase sa = { 12, 5, 16, 8, 0, false, 1 };
        const UvCase vw = { 12, 5, 512, 256, 33, true, 1 };
        const UvCase vwPow2b = { 16, 32, 512, 256, 53, true, 0 };
        const float limSa = gdx3ds_pad_clamp_limit(sa.texW, sa.padW);
        const float limVw = gdx3ds_view_clamp_limit(vw.texW, vw.padW);
        bool sameResult = true;
        float worst = 0.0f;
        for (int k = 0; k <= 48; k++) {
            const float sT = 12.0f * (float)k / 48.0f; // eligibility: s in [0, texW]
            const float a = pack_u_texels(sa, sT, limSa);
            const float b = pack_u_texels(vw, sT, limVw) - (float)vw.axTexels;
            const float d = std::fabs(a - b);
            if (d <= 1e-4f) {
                continue; // identical coordinate
            }
            worst = d > worst ? d : worst;
            // Otherwise both must resolve to the last content texel (index texW - 1) alone:
            // the view's two taps are that texel and its gutter copy.
            const float wSa = bilinear_weight(a, (int)sa.texW - 1);
            const float wVwContent = bilinear_weight(b, (int)sa.texW - 1);
            const float wVwGutter = bilinear_weight(b, (int)sa.texW); // == a copy of texW - 1
            if (std::fabs(wSa - 1.0f) > 1e-5f || std::fabs(wVwContent + wVwGutter - 1.0f) > 1e-5f) {
                sameResult = false;
            }
        }
        check_true("padded axis: edge-clamped view resolves to the same texel as the "
                   "centre-clamped standalone", sameResult, worst);
        // The worst reachable coordinate still cannot leave the cell's 1-texel gutter.
        // On a PADDED axis the view must take the standalone's own (extent - 0.5) limit,
        // shear and all: F-Zero X's race HUD gradient strips (304x3, 160x24) are padded on
        // both axes and interpolate V over 0..h-0.5 on the standalone path. A view that
        // clamped at the cell EDGE instead diverged on probe3 race frames 1500-1701 even
        // though it fixed the glyphs.
        const float farWild = pack_u_texels(vw, 400.0f, limVw) - (float)vw.axTexels;
        check_near("padded axis: view limit == the standalone limit, not the cell edge",
                   farWild, (float)vw.texW - 0.5f, 1e-3f);
        check_near("padded axis: standalone clamps to the same coordinate",
                   pack_u_texels(sa, 400.0f, limSa), farWild, 1e-3f);
        check_true("padded axis: it therefore stays inside the cell + its 1-texel gutter",
                   farWild <= (float)vw.texW + 0.5f, farWild);
        check_near("  -> and resolves to the last content texel alone",
                   bilinear_weight(farWild, (int)vw.texW - 1) + bilinear_weight(farWild, (int)vw.texW),
                   1.0f, 1e-5f);
        // The pow2 glyph gets the same bound.
        const float glyphWild =
            pack_u_texels(vwPow2b, 400.0f, gdx3ds_view_clamp_limit(vwPow2b.texW, vwPow2b.padW)) -
            (float)vwPow2b.axTexels;
        check_near("pow2 view: a wild coord clamps to the cell's far edge", glyphWild, 16.0f, 1e-3f);
    }

    printf("\n");
    if (g_failures == 0) {
        printf("=== ALL PASS (%d checks) ===\n", g_checks);
        return 0;
    }
    printf("=== %d FAILURE(S) of %d checks ===\n", g_failures, g_checks);
    return 1;
}
