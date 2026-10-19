/* port/3ds/gfx/n64/gdx_rect.cpp -- viewport/scissor mapping and the rectangle family
 * (GfxDrawRectangle / GfxDpTextureRectangle / GfxDpFillRectangle / GfxDpSetScissor /
 * CalcAndSetViewport / AdjustVIewportOrScissor, interpreter.cpp 5696-5760, 5903-5930,
 * 7011-7408). No LUS framebuffer objects are ever active on this port (no OTR fb opcodes),
 * so the mFbActive branches collapse to the native-screen case. Plan §3.10. */
#include "gdx_interp.h"

#include <string.h>

namespace gdxn {

static inline uint32_t ColorComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xFu) | ((b & 0xFu) << 4) | ((c & 0x1Fu) << 8) | ((d & 7u) << 13);
}
static inline uint32_t AlphaComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7u) | ((b & 7u) << 3) | ((c & 7u) << 6) | ((d & 7u) << 9);
}

void Interp::AdjustViewportOrScissor(Rect* area) {
    const ClipParams cp = mBackend->GetClipParameters();
    if (cp.invertY) {
        area->y = static_cast<int16_t>(area->y - static_cast<int16_t>(area->height));
    } else {
        area->y = static_cast<int16_t>(mCfg.nativeH - area->y);
    }
    const float ratioX = static_cast<float>(mCfg.curW) / (2.0f * (mCfg.nativeW / 2));
    const float ratioY = static_cast<float>(mCfg.curH) / (2.0f * (mCfg.nativeH / 2));
    area->width = static_cast<uint32_t>(area->width * ratioX);
    area->height = static_cast<uint32_t>(area->height * ratioY);
    area->x = static_cast<int16_t>(area->x * ratioX);
    area->y = static_cast<int16_t>(area->y * ratioY);
    if (!mCfg.rendersToFb) {
        /* mGameWindowViewport = {0,0,screen} on 3DS; windowH - (0 + viewport.h) = 0. */
        area->x = static_cast<int16_t>(area->x + 0);
        area->y = static_cast<int16_t>(area->y + static_cast<int32_t>(mCfg.windowH) - static_cast<int32_t>(mCfg.curH));
    }
}

void Interp::CalcAndSetViewport(const int16_t* vp) {
    const float width = 2.0f * vp[0] / 4.0f;
    const float height = 2.0f * vp[1] / 4.0f;
    const float x = (vp[4] / 4.0f) - width / 2.0f;
    const float y = ((vp[5] / 4.0f) + height / 2.0f);
    mRdp.viewport.x = static_cast<int16_t>(x);
    mRdp.viewport.y = static_cast<int16_t>(y);
    mRdp.viewport.width = static_cast<uint32_t>(width);
    mRdp.viewport.height = static_cast<uint32_t>(height);
    mRsp.viewport_z_scale = vp[2];
    mRsp.viewport_z_trans = vp[6];
    AdjustViewportOrScissor(&mRdp.viewport);
    mRdp.viewport_or_scissor_changed = true;
}

void Interp::DpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    (void)mode;
    if (mCfg.removeBorders && ulx == (12 << 2) && lrx == (308 << 2) &&
        ((uly == (16 << 2) && lry == (224 << 2)) || (uly == (8 << 2) && lry == (232 << 2)))) {
        ulx = 0;
        uly = 0;
        lrx = 320 << 2;
        lry = 240 << 2;
    }
    const float x = ulx / 4.0f;
    const float y = lry / 4.0f;
    const float width = (lrx - ulx) / 4.0f;
    const float height = (lry - uly) / 4.0f;
    mRdp.scissor.x = static_cast<int16_t>(x);
    mRdp.scissor.y = static_cast<int16_t>(y);
    mRdp.scissor.width = static_cast<uint32_t>(width);
    mRdp.scissor.height = static_cast<uint32_t>(height);
    AdjustViewportOrScissor(&mRdp.scissor);
#ifdef EXPANSION_KIT
    ScissorPillarSafeX(x, width);
#endif
    mRdp.viewport_or_scissor_changed = true;
}

#ifdef EXPANSION_KIT
/* B11o -- the scissor and a 2D rectangle do NOT use the same x mapping on a 400x240 panel,
 * and a 48-pixel-wide drop-down is small enough for the disagreement to eat its text.
 *
 * AdjustViewportOrScissor scales x LINEARLY FROM ZERO by curW/nativeW = 400/320 = 1.25.
 * DrawRectangle instead takes x to NDC and runs it through AdjXForAspectRatio, a 0.8x
 * compression ABOUT SCREEN CENTRE, so a 2D rectangle lands at screen x = n64x + 40 -- the
 * hor+ pillarbox ekdraw-progress.md §2 derives (320 content columns, 40-px pillars, 1:1).
 * The two agree only at n64x = 160; the error is 0.25*x - 40 and grows outward.
 *
 * Full-screen scissors survive it (12..308 -> 15..385 still covers the whole 40..360 band),
 * which is why nothing noticed until an Expansion Kit build could open a drop-down.
 * expansion_kit/A3AE0.c:1033 scissors a drop-down to exactly its own 48-px item column,
 *     gDPSetScissor(..., widget->left, widget->top, widget->left + 48, ...)
 * On N64 those bounds are identically the drawn extent, so they clip NOTHING. On 3DS
 * gCreateWidget (left = 24) gets scissor 30..90 against content at 64..112 and loses the
 * right 22 columns of 48; gCourseEditEntryWidget (left = 216) loses its left 14.
 *
 * The fix is the UNION of the two mappings, because a scissor on this platform cannot know
 * whether what follows is 2D or 3D: keep the linear box (3D geometry and the viewport are
 * mapped that way and must stay clipped that way) and extend it to also cover where a 2D
 * rectangle with the same N64 bounds actually lands. It is strictly widening, so it can only
 * ever fail to clip; it never clips something the N64 kept.
 *
 * It is a NO-OP for every scissor whose edges sit at 0, 160 or 320, and for the 12/308 full
 * screen (15/385 by either mapping). It is scoped to EXPANSION_KIT anyway so that the
 * shipped .3dsx stays byte-identical; see docs/research/native/ekwidget-progress.md.
 *
 * [debug] ek_widget_scissor = 0 restores the linear-only box (i.e. restores the bug), which
 * is what makes the A/B a one-lever pair out of one binary. */
extern "C" int gdx_ninterp_ekwidgetscissor(void);

void Interp::ScissorPillarSafeX(float n64X, float n64W) {
    if (!gdx_ninterp_ekwidgetscissor()) {
        return;
    }
    const float halfW = static_cast<float>(mCfg.nativeW / 2);
    const float curW = static_cast<float>(mCfg.curW);
    const float curH = static_cast<float>(mCfg.curH);
    if (halfW <= 0.0f || curW <= 0.0f || curH <= 0.0f) {
        return;
    }
    /* Where DrawRectangle puts these same two N64 columns: NDC, AdjXForAspectRatio, viewport. */
    const float k = (4.0f / 3.0f) / (curW / curH);
    const float lo2d = (((n64X / halfW) - 1.0f) * k + 1.0f) * 0.5f * curW;
    const float hi2d = ((((n64X + n64W) / halfW) - 1.0f) * k + 1.0f) * 0.5f * curW;
    int32_t lo = mRdp.scissor.x;
    int32_t hi = lo + static_cast<int32_t>(mRdp.scissor.width);
    if (static_cast<int32_t>(lo2d) < lo) {
        lo = static_cast<int32_t>(lo2d);
    }
    if (static_cast<int32_t>(hi2d) > hi) {
        hi = static_cast<int32_t>(hi2d);
    }
    if (lo < 0) {
        lo = 0;
    }
    if (hi < lo) {
        hi = lo;
    }
    mRdp.scissor.x = static_cast<int16_t>(lo);
    mRdp.scissor.width = static_cast<uint32_t>(hi - lo);
}
#endif /* EXPANSION_KIT */

void Interp::DrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    const uint32_t saved_other_mode_h = mRdp.other_mode_h;
    const uint32_t cycle_type = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE));
    if (cycle_type == CYC_COPY) {
        mRdp.other_mode_h = (mRdp.other_mode_h & ~(3u << MDSFT_TEXTFILT)) | TF_POINT;
    }
    const float halfW = static_cast<float>(mCfg.nativeW / 2);
    const float halfH = static_cast<float>(mCfg.nativeH / 2);
    float ulxf = static_cast<float>(ulx);
    float ulyf = static_cast<float>(uly);
    float lrxf = static_cast<float>(lrx);
    float lryf = static_cast<float>(lry);
    ulxf = ulxf / (4.0f * halfW) - 1.0f;
    ulyf = -(ulyf / (4.0f * halfH)) + 1.0f;
    lrxf = lrxf / (4.0f * halfW) - 1.0f;
    lryf = -(lryf / (4.0f * halfH)) + 1.0f;
    {
        const float currentAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
        const float aspectScale = (4.0f / 3.0f) / currentAspect;
        const uint32_t widescreenMode = mRsp.extra_geometry_mode & (0x04u | 0x08u | 0x10u | 0x20u);
        const bool widescreenFrameActive = currentAspect > (4.0f / 3.0f) && mCfg.widescreen && !mCfg.forceFixedAspect;
        const bool stretchActive = widescreenFrameActive && (widescreenMode & 0x04u) != 0;
        const bool enhancedWidescreenUi = widescreenFrameActive && widescreenMode != 0 && mCfg.widescreenUi;
        if (stretchActive) {
            constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
            ulxf *= kSafeAreaScale;
            lrxf *= kSafeAreaScale;
        } else if (enhancedWidescreenUi && (widescreenMode & 0x20u) != 0) {
            const float center = (ulxf + lrxf) * 0.5f;
            const float halfWidth = (lrxf - ulxf) * 0.5f * aspectScale;
            ulxf = center - halfWidth;
            lrxf = center + halfWidth;
        } else {
            ulxf = AdjXForAspectRatio(ulxf);
            lrxf = AdjXForAspectRatio(lrxf);
            if (enhancedWidescreenUi) {
                const bool anchorLeft = (widescreenMode & 0x08u) != 0;
                const bool anchorRight = (widescreenMode & 0x10u) != 0;
                if (anchorLeft != anchorRight) {
                    const float uiAspect = currentAspect < mCfg.hudMaxAspect ? currentAspect : mCfg.hudMaxAspect;
                    float offset = (anchorRight ? 1.0f : -1.0f) * ((uiAspect - (4.0f / 3.0f)) / currentAspect);
                    if (mCfg.removeBorders) {
                        offset += (anchorRight ? 1.0f : -1.0f) * (0.1f / currentAspect);
                    }
                    ulxf += offset;
                    lrxf += offset;
                }
            }
        }
    }
    LoadedVertex* ul = &mRsp.loaded_vertices[kMaxVertices + 0];
    LoadedVertex* ll = &mRsp.loaded_vertices[kMaxVertices + 1];
    LoadedVertex* lr = &mRsp.loaded_vertices[kMaxVertices + 2];
    LoadedVertex* ur = &mRsp.loaded_vertices[kMaxVertices + 3];
    ul->x = ulxf; ul->y = ulyf; ul->z = -1.0f; ul->w = 1.0f;
    ll->x = ulxf; ll->y = lryf; ll->z = -1.0f; ll->w = 1.0f;
    lr->x = lrxf; lr->y = lryf; lr->z = -1.0f; lr->w = 1.0f;
    ur->x = lrxf; ur->y = ulyf; ur->z = -1.0f; ur->w = 1.0f;

    Rect default_viewport;
    default_viewport.x = 0;
    default_viewport.y = static_cast<int16_t>(mCfg.nativeH);
    default_viewport.width = mCfg.nativeW;
    default_viewport.height = mCfg.nativeH;
    AdjustViewportOrScissor(&default_viewport);

    const Rect viewport_saved = mRdp.viewport;
    const uint32_t geometry_mode_saved = mRsp.geometry_mode;
    mRdp.viewport = default_viewport;
    mRdp.viewport_or_scissor_changed = true;
    mRsp.geometry_mode = 0;
    SpTri(kMaxVertices + 0, kMaxVertices + 1, kMaxVertices + 3, true);
    SpTri(kMaxVertices + 1, kMaxVertices + 2, kMaxVertices + 3, true);
    mRsp.geometry_mode = geometry_mode_saved;
    mRdp.viewport = viewport_saved;
    mRdp.viewport_or_scissor_changed = true;
    if (cycle_type == CYC_COPY) {
        mRdp.other_mode_h = saved_other_mode_h;
    }
}

static bool TrectRectEligible(const Rdp& rdp, uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt) {
    const Tile& tt = rdp.texture_tile[tile];
    const bool clampS = ((tt.cms & TX_CLAMP) != 0 || tt.masks == 0) && !((tt.cms & TX_MIRROR) != 0 && tt.masks != 0);
    const bool clampT = ((tt.cmt & TX_CLAMP) != 0 || tt.maskt == 0) && !((tt.cmt & TX_MIRROR) != 0 && tt.maskt != 0);
    if (!clampS || !clampT) return false;
    if (tt.shifts != 0 || tt.shiftt != 0) return false;
    const float s0 = static_cast<float>(uls) / 32.0f, s1 = lrs / 32.0f;
    const float t0 = static_cast<float>(ult) / 32.0f, t1 = lrt / 32.0f;
    const float sMin = s0 < s1 ? s0 : s1, sMax = s0 < s1 ? s1 : s0;
    const float tMin = t0 < t1 ? t0 : t1, tMax = t0 < t1 ? t1 : t0;
    return sMin >= tt.uls / 4.0f - 0.01f && sMax <= tt.lrs / 4.0f + 1.01f && tMin >= tt.ult / 4.0f - 0.01f &&
           tMax <= tt.lrt / 4.0f + 1.01f;
}

void Interp::DpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    mStats.rects++;
    const uint64_t saved_combine_mode = mRdp.combine_mode;
    if ((mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_COPY) {
        if (dsdx > 0x0400 || dsdx < -0x0400) {
            dsdx = static_cast<int16_t>(dsdx >> 2);
        }
        DpSetCombineMode(ColorComb(0, 0, 0, CCMUX_TEXEL0), AlphaComb(0, 0, 0, ACMUX_TEXEL0), 0, 0);
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    if (flip) {
        dsdx = static_cast<int16_t>(-dsdx);
        dtdy = static_cast<int16_t>(-dtdy);
    }
    const int16_t width = static_cast<int16_t>(!flip ? lrx - ulx : lry - uly);
    const int16_t height = static_cast<int16_t>(!flip ? lry - uly : lrx - ulx);
    const float lrs = static_cast<float>(((uls << 7) + dsdx * width) >> 7);
    const float lrt = static_cast<float>(((ult << 7) + dtdy * height) >> 7);

    LoadedVertex* ul = &mRsp.loaded_vertices[kMaxVertices + 0];
    LoadedVertex* ll = &mRsp.loaded_vertices[kMaxVertices + 1];
    LoadedVertex* lr = &mRsp.loaded_vertices[kMaxVertices + 2];
    LoadedVertex* ur = &mRsp.loaded_vertices[kMaxVertices + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls; ll->v = lrt; ur->u = lrs; ur->v = ult;
    } else {
        ll->u = lrs; ll->v = ult; ur->u = uls; ur->v = lrt;
    }
    const uint8_t saved_tile = mRdp.first_tile_index;
    if (saved_tile != tile) {
        ArmTexturesChanged();
    }
    mRdp.first_tile_index = tile;
    mTrectRectAtlasOk = mTrectBatchOn && TrectRectEligible(mRdp, tile, uls, ult, lrs, lrt);
    DrawRectangle(ulx, uly, lrx, lry);
    mTrectRectAtlasOk = false;
    if (saved_tile != tile) {
        ArmTexturesChanged();
    }
    mRdp.first_tile_index = saved_tile;
    mRdp.combine_mode = saved_combine_mode;
}

void Interp::DpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    mStats.rects++;
    if (mRdp.color_image_address == mRdp.z_buf_address) {
        const bool isFullScreen = (ulx <= 0 && uly <= 0 && lrx >= static_cast<int32_t>(mCfg.nativeW - 1) * 4 &&
                                   lry >= static_cast<int32_t>(mCfg.nativeH - 1) * 4);
        if (isFullScreen) {
            return;
        }
        Flush(FLUSH_FBOP);
        const int32_t expanded_lrx = lrx + (1 << 2);
        const int32_t expanded_lry = lry + (1 << 2);
        const float x = ulx / 4.0f;
        const float y = expanded_lry / 4.0f;
        const float w = (expanded_lrx - ulx) / 4.0f;
        const float h = (expanded_lry - uly) / 4.0f;
        Rect area;
        area.x = static_cast<int16_t>(x);
        area.y = static_cast<int16_t>(y);
        area.width = static_cast<uint32_t>(w);
        area.height = static_cast<uint32_t>(h);
        AdjustViewportOrScissor(&area);
        mBackend->ClearDepthRegion(area.x, area.y, area.width, area.height);
        return;
    }
    const uint32_t mode = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE));
    if (ulx > lrx || uly > lry) {
        return;
    }
    if (ulx == 0 && uly == 0) {
        const bool isFullScreen = (lrx == (static_cast<int32_t>(mCfg.nativeW - 1) * 4) &&
                                   lry == (static_cast<int32_t>(mCfg.nativeH - 1) * 4));
        if (isFullScreen) {
            const float fsAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
            const float fsK = (4.0f / 3.0f) / (fsAspect > 4.0f / 3.0f ? fsAspect : 4.0f / 3.0f);
            int32_t ex = static_cast<int32_t>(160.0f * (1.0f / (fsK > 0.05f ? fsK : 0.05f) + 1.0f) * 4.0f) + 64;
            if (ex < 2048) ex = 2048;
            ulx = -ex;
            uly = -1024;
            lrx = ex;
            lry = 2048;
        }
    }
    if (mode == CYC_COPY || mode == CYC_FILL) {
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    for (int i = kMaxVertices; i < kMaxVertices + 4; i++) {
        mRsp.loaded_vertices[i].color = mRdp.fill_color;
    }
    const uint64_t saved_combine_mode = mRdp.combine_mode;
    if (mode == CYC_FILL) {
        DpSetCombineMode(ColorComb(0, 0, 0, CCMUX_SHADE), AlphaComb(0, 0, 0, ACMUX_SHADE), 0, 0);
    }
    DrawRectangle(ulx, uly, lrx, lry);
    mRdp.combine_mode = saved_combine_mode;
}

} // namespace gdxn
