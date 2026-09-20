/* port/3ds/gfx/n64/gdx_seam_owned.cpp -- GdxInterpSeam over gdxn::Interp (gdx_seam_owned.h).
 *
 * Every entry maps onto the owned interpreter's own handler: SpVertex is TransformVertices
 * (the G_VTX body from a resolved host-endian source), SpTri is the full triangle path,
 * SetCombineRaw the OP_SETCOMBINE decode, the prim/env setters the value-change-flush
 * handlers, Flush the batch drain. PackState exposes the [triloop] latches and the tri memo
 * ResolveDrawState banks; BindMaterial IS ResolveDrawState (SpTri's state block against the
 * current registers, no emission); LatchVtxState mirrors SpVertex's RSP reads the way the LUS
 * adapter mirrors GfxSpVertex's. Layout equivalence with the seam's value shapes is asserted. */
#include "gdx_seam_owned.h"

#include <stddef.h>
#include <string.h>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_track_interp.h"
#include "gdx3ds_vtx_math.h"
#include "gdx_interp.h"

using namespace gdxn;

static_assert(sizeof(LoadedVertex) == sizeof(GdxSeamVertex), "gdxn::LoadedVertex layout drifted from the seam");
static_assert(offsetof(LoadedVertex, color) == offsetof(GdxSeamVertex, color), "LoadedVertex.color offset");
static_assert(offsetof(LoadedVertex, clip_out) == offsetof(GdxSeamVertex, clip_out), "LoadedVertex.clip_out offset");
static_assert(sizeof(Rgba) == sizeof(GdxSeamRgba), "Rgba layout");
static_assert(kMaxVertices == GDXSEAM_MAX_VERTICES, "vertex cache size");

class GdxOwnedSeam final : public GdxInterpSeam {
  public:
    explicit GdxOwnedSeam(Interp* i) : mI(i) {
        Rsp& rsp = i->mRsp;
        Rdp& rdp = i->mRdp;
        mR.vertices = reinterpret_cast<GdxSeamVertex*>(rsp.loaded_vertices);
        mR.geometry_mode = &rsp.geometry_mode;
        mR.extra_geometry_mode = &rsp.extra_geometry_mode;
        mR.fog_mul = &rsp.fog_mul;
        mR.fog_offset = &rsp.fog_offset;
        mR.tex_scale_s = &rsp.texture_scaling_factor.s;
        mR.tex_scale_t = &rsp.texture_scaling_factor.t;
        mR.prim_color = reinterpret_cast<GdxSeamRgba*>(&rdp.prim_color);
        mR.env_color = reinterpret_cast<GdxSeamRgba*>(&rdp.env_color);
        mR.fog_color = reinterpret_cast<GdxSeamRgba*>(&rdp.fog_color);
        mR.blend_color = reinterpret_cast<GdxSeamRgba*>(&rdp.blend_color);
        mR.key_center = reinterpret_cast<GdxSeamRgba*>(&rdp.key_center);
        mR.key_scale = reinterpret_cast<GdxSeamRgba*>(&rdp.key_scale);
        mR.convert_k = rdp.convert_k;
        mR.prim_lod_fraction = &rdp.prim_lod_fraction;
        mR.first_tile_index = &rdp.first_tile_index;
        mR.textures_changed = rdp.textures_changed;
        mR.other_mode_l = &rdp.other_mode_l;
        mR.other_mode_h = &rdp.other_mode_h;
        mR.combine_mode = &rdp.combine_mode;
        mR.prim_depth = &rdp.prim_depth;
        mR.viewport_or_scissor_changed = &rdp.viewport_or_scissor_changed;
    }

    void SpVertex(size_t n, size_t dest, const void* vtx) override {
        ProfScope prof(mI->mBackend, 2);
        mI->mStats.vtx += static_cast<uint32_t>(n);
        mI->TransformVertices(static_cast<const uint8_t*>(vtx), n, dest, false);
    }
    void SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val) override {
        mI->SpModifyVertex(idx, where, val);
    }
    void SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) override {
        mI->SpTexture(sc, tc, level, tile, on);
    }
    void SetCombineRaw(uint32_t w0, uint32_t w1) override {
        mI->SetCombineWords(w0, w1);
    }
    void SetPrimColor(uint8_t m, uint8_t l, GdxSeamRgba c) override {
        mI->DpSetPrimColor(m, l, Rgba{ c.r, c.g, c.b, c.a });
    }
    void SetEnvColor(GdxSeamRgba c) override {
        mI->DpSetEnvColor(Rgba{ c.r, c.g, c.b, c.a });
    }
    void Flush() override {
        mI->Flush(FLUSH_HOOK);
    }
    size_t PendingTris() const override {
        return mI->mBufNumTris;
    }
    void SpTri(uint8_t a, uint8_t b, uint8_t c) override {
        mI->SpTri(a, b, c, false);
    }
    void PopAppended(size_t n0, size_t appended) override {
        (void)appended; /* the packed cursor is the pool's; only the count is ours */
        mI->mBufNumTris = n0;
    }
    void PackState(GdxTrackPackState* out) override {
        const DrawState& m = mI->mTriMemo;
        out->packBatch = mI->mPackBatch;
        out->packDst = mI->mPackDst;
        out->params = &mI->mPackParams;
        out->aux = &mI->mPackAux;
        out->memoValid = mI->mTriMemoValid;
        out->memoDirty = false; /* the memo is dropped at Flush(), never left stale (gdx_tri.cpp) */
        out->inputMap = (mI->mTriMemoValid && m.comb != nullptr) ? m.comb->shader_input_mapping : nullptr;
        out->prg = m.prg;
        out->tm = m.tm;
        for (int i = 0; i < 2; i++) {
            out->texWidth[i] = m.tex_width[i];
            out->texHeight[i] = m.tex_height[i];
            out->texWidth2[i] = m.tex_width2[i];
            out->texHeight2[i] = m.tex_height2[i];
            out->effectiveTile[i] = m.effective_tile[i];
            out->usedTextures[i] = m.usedTextures[i];
        }
        out->clip.zIsFrom0To1 = m.clip.zIsFrom0To1;
        out->clip.invertY = m.clip.invertY;
        out->cullBoth = GM_CULL_BOTH;
        out->cullFront = GM_CULL_FRONT;
        out->cullBack = GM_CULL_BACK;
        out->triFast = true; /* the packed trifast emission is the owned path's only path */
        out->rejectVariant = RejectVariant();
        out->minClipW = 1.0e-5f; /* gdx_tri.cpp kMinClipW == LUS kGdxMinClipW */
    }
    bool RejectVariant() const override {
        return mI->mVariant == 1 || mI->mVariant == 2;
    }
    void TileUv(uint32_t tile, GdxSeamTileUv* out) const override {
        const Tile& t = mI->mRdp.texture_tile[tile & 7u];
        out->shifts = t.shifts;
        out->shiftt = t.shiftt;
        out->uls = t.uls;
        out->ult = t.ult;
    }
    bool BindMaterial(GdxSeamMaterial& mat) override {
        DrawState ds;
        if (!mI->ResolveDrawState(false, ds)) {
            return false;
        }
        mat.inputMap = ds.comb->shader_input_mapping;
        mat.combUsedTextures[0] = ds.comb->usedTextures[0];
        mat.combUsedTextures[1] = ds.comb->usedTextures[1];
        mat.prg = ds.prg;
        mat.tm = ds.tm;
        for (int i = 0; i < 2; i++) {
            mat.tex_width[i] = ds.tex_width[i];
            mat.tex_height[i] = ds.tex_height[i];
            mat.tex_width2[i] = ds.tex_width2[i];
            mat.tex_height2[i] = ds.tex_height2[i];
            mat.effective_tile[i] = ds.effective_tile[i];
            mat.usedTextures[i] = ds.usedTextures[i];
        }
        mat.numInputs = ds.numInputs;
        mat.clip.zIsFrom0To1 = ds.clip.zIsFrom0To1;
        mat.clip.invertY = ds.clip.invertY;
        mat.use_alpha = ds.use_alpha;
        mat.use_fog = ds.use_fog;
        mat.use_blend_color = ds.use_blend_color;
        return true;
    }
    bool LatchVtxState(Gdx3dsVtxMathState& s) override;
    void ApplyDrawConstants() override {
        mI->mBackend->SetCurrentPrimDepth(static_cast<float>(mI->mRdp.prim_depth) / 32767.0f);
        mI->mBackend->SetCurrentAlphaCompareThreshold(static_cast<float>(mI->mRdp.blend_color.a) / 255.0f);
    }

    const void* ResolveVtx(uint64_t w1word, size_t bytes) const {
        Cmd c;
        c.w0 = static_cast<uint32_t>(OP_VTX) << 24;
        c.w1 = static_cast<uint32_t>(w1word);
        c.w1hi = static_cast<uint32_t>(w1word >> 32);
        const uintptr_t p = mI->mSrc.ResolveData(c, bytes, /*preferPhysical=*/true, reinterpret_cast<uintptr_t>(mListHint));
        if (p == 0 || DlSrc::ReadableBytes(p) < bytes) {
            return nullptr;
        }
        return reinterpret_cast<const void*>(p);
    }
    void SetListHint(const void* p) {
        mListHint = p;
    }

  private:
    Interp* mI;
    const void* mListHint = nullptr;
};

/* Latch SpVertex's RSP inputs (gdx_rsp.cpp), the shapes gdx3ds_vtx_math.h models. */
bool GdxOwnedSeam::LatchVtxState(Gdx3dsVtxMathState& s) {
    Rsp& rsp = mI->mRsp;
    memset(&s, 0, sizeof(s));
    memcpy(s.mp, rsp.MP_matrix, sizeof(s.mp));
    if (rsp.geometry_mode & GM_LIGHTING_POSITIONAL) {
        return false;
    }
    /* AdjXForAspectRatio / G_EX_WIDESCREEN_STRETCH branch selection (SpVertex). */
    const GdxFrameConfig& cfg = mI->mCfg;
    const float wf = static_cast<float>(cfg.curW);
    const float hf = static_cast<float>(cfg.curH);
    int adjMode = GDX3DS_VTX_X_NONE;
    if (cfg.widescreen && !cfg.forceFixedAspect) {
        adjMode = GDX3DS_VTX_X_ADJ;
    }
    s.aspect = wf / hf;
    if ((rsp.extra_geometry_mode & GDXSEAM_G_EX_WIDESCREEN_STRETCH) != 0) {
        const bool widescreenFrameActive = (wf / hf) > (4.0f / 3.0f) && cfg.widescreen && !cfg.forceFixedAspect;
        s.xMode = widescreenFrameActive ? GDX3DS_VTX_X_STRETCH : adjMode;
    } else {
        s.xMode = adjMode;
    }
    s.lighting = (rsp.geometry_mode & GM_LIGHTING) != 0;
    if (s.lighting) {
        if (rsp.lights_changed) {
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                mI->CalculateNormalDir(&rsp.current_lights[i].l, rsp.current_lights_coeffs[i]);
            }
            mI->CalculateNormalDir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
            mI->CalculateNormalDir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
            rsp.lights_changed = false;
        }
        s.numDir = rsp.current_num_lights - 1;
        if (s.numDir < 0 || s.numDir > GDX3DS_VTX_MAX_DIR_LIGHTS) {
            return false;
        }
        for (int i = 0; i < s.numDir; i++) {
            memcpy(s.coeffs[i], rsp.current_lights_coeffs[i], sizeof(s.coeffs[i]));
            memcpy(s.lightCol[i], rsp.current_lights[i].l.col, 3);
        }
        const int ambIdx = rsp.current_num_lights - 1;
        memcpy(s.ambient, rsp.current_lights[ambIdx].l.col, 3);
        s.textureGen = (rsp.geometry_mode & GM_TEXTURE_GEN) != 0;
        s.textureGenLinear = (rsp.geometry_mode & GM_TEXTURE_GEN_LINEAR) != 0;
        s.flxAlpha = mI->mVariant == 2 && rsp.f3dflx_alpha_light_valid;
        if (s.flxAlpha) {
            gdx3ds_vtx_alpha_light(rsp.f3dflx_alpha_light, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                                   s.alphaLight);
        }
        s.dmem = rsp.dmem;
        s.dmaIoDmem = static_cast<int>(rsp.dma_io_dmem);
        memcpy(s.lookat, rsp.current_lookat_coeffs, sizeof(s.lookat));
    }
    s.fog = (rsp.geometry_mode & GM_FOG) != 0;
    s.fogMul = rsp.fog_mul;
    s.fogOffset = rsp.fog_offset;
    return true;
}

GdxInterpSeam* GdxOwnedSeamCreate(Interp* interp) {
    return new GdxOwnedSeam(interp);
}

void GdxOwnedSeamSetListHint(GdxInterpSeam* seam, const void* listPtr) {
    static_cast<GdxOwnedSeam*>(seam)->SetListHint(listPtr);
}

const void* GdxOwnedSeamResolveVtx(uint64_t w1word, size_t bytes, void* user) {
    return static_cast<const GdxOwnedSeam*>(user)->ResolveVtx(w1word, bytes);
}
