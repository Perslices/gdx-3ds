/* gdx3ds_seam_lus.cpp — GdxInterpSeam over the vendored LUS Fast::Interpreter
 * (gdx3ds_interp_seam.h). The only TU of the native replay consumers that includes a LUS
 * header: it owns the C entries the vendored interpreter calls (the G_NOOP marker handler's
 * gdx3ds_track_marker / gdx3ds_fx_marker, gfx_dl_handler_common's gdx3ds_mesh_dl_hook) and
 * the two Interpreter members the vendored header declares for the port (GdxTrackMarker,
 * GdxFxMarker), and routes all of them to the interpreter-neutral replays.
 *
 * BindMaterial / LatchVtxState are GfxSpTri1's draw-state resolution and GfxSpVertex's RSP
 * latch, moved verbatim from gdx3ds_mesh.cpp (native/machines, verified bit-exact against the
 * interpreter's own draws). */
#include "fast/interpreter.h"

#include <cstring>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_track_interp.h"
#include "gdx3ds_vbopack.h"
#include "gdx3ds_vtx_math.h"

using namespace Fast;

namespace {

#ifdef N64_PRIM_DEPTH_MAX
constexpr float kPrimDepthMax = (float)N64_PRIM_DEPTH_MAX;
#else
constexpr float kPrimDepthMax = 32767.0f; /* interpreter.cpp: Flush() */
#endif

static_assert(sizeof(LoadedVertex) == sizeof(GdxSeamVertex), "LoadedVertex layout drifted from the seam");
static_assert(offsetof(LoadedVertex, color) == offsetof(GdxSeamVertex, color), "LoadedVertex.color offset");
static_assert(offsetof(LoadedVertex, clip_out) == offsetof(GdxSeamVertex, clip_out), "LoadedVertex.clip_out offset");
static_assert(sizeof(RGBA) == sizeof(GdxSeamRgba), "RGBA layout");
static_assert(MAX_VERTICES == GDXSEAM_MAX_VERTICES, "vertex cache size");

/* ApplyTileMaskExtent (interpreter.cpp, file-static) */
void ApplyTileMaskExtentMirror(const RDP* rdp, int tile, uint32_t& width, uint32_t& height, bool maskAuthoritative) {
    const uint8_t masks = rdp->texture_tile[tile].masks;
    const uint8_t maskt = rdp->texture_tile[tile].maskt;
    const uint8_t cms = rdp->texture_tile[tile].cms;
    const uint8_t cmt = rdp->texture_tile[tile].cmt;
    if (masks != G_TX_NOMASK && masks < 31) {
        if (maskAuthoritative && (cms & G_TX_CLAMP) == 0) {
            width = 1u << masks;
        } else {
            width = width < (1u << masks) ? width : (1u << masks);
        }
    }
    if (maskt != G_TX_NOMASK && maskt < 31) {
        if (maskAuthoritative && (cmt & G_TX_CLAMP) == 0) {
            height = 1u << maskt;
        } else {
            height = height < (1u << maskt) ? height : (1u << maskt);
        }
    }
}

class LusSeam final : public GdxInterpSeam {
  public:
    explicit LusSeam(Interpreter* g) : mG(g) {
        RSP* rsp = g->mRsp;
        RDP* rdp = g->mRdp;
        mR.vertices = reinterpret_cast<GdxSeamVertex*>(rsp->loaded_vertices);
        mR.geometry_mode = &rsp->geometry_mode;
        mR.extra_geometry_mode = &rsp->extra_geometry_mode;
        mR.fog_mul = &rsp->fog_mul;
        mR.fog_offset = &rsp->fog_offset;
        mR.tex_scale_s = &rsp->texture_scaling_factor.s;
        mR.tex_scale_t = &rsp->texture_scaling_factor.t;
        mR.prim_color = reinterpret_cast<GdxSeamRgba*>(&rdp->prim_color);
        mR.env_color = reinterpret_cast<GdxSeamRgba*>(&rdp->env_color);
        mR.fog_color = reinterpret_cast<GdxSeamRgba*>(&rdp->fog_color);
        mR.blend_color = reinterpret_cast<GdxSeamRgba*>(&rdp->blend_color);
        mR.key_center = reinterpret_cast<GdxSeamRgba*>(&rdp->key_center);
        mR.key_scale = reinterpret_cast<GdxSeamRgba*>(&rdp->key_scale);
        mR.convert_k = rdp->convert_k;
        mR.prim_lod_fraction = &rdp->prim_lod_fraction;
        mR.first_tile_index = &rdp->first_tile_index;
        mR.textures_changed = rdp->textures_changed;
        mR.other_mode_l = &rdp->other_mode_l;
        mR.other_mode_h = &rdp->other_mode_h;
        mR.combine_mode = &rdp->combine_mode;
        mR.prim_depth = &rdp->prim_depth;
        mR.viewport_or_scissor_changed = &rdp->viewport_or_scissor_changed;
    }
    Interpreter* Owner() const {
        return mG;
    }

    void SpVertex(size_t n, size_t dest, const void* vtx) override {
        mG->GfxSpVertex(n, dest, static_cast<const F3DVtx*>(vtx));
    }
    void SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val) override {
        mG->GfxSpModifyVertex(idx, where, val);
    }
    void SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) override {
        mG->GfxSpTexture(sc, tc, level, tile, on);
    }
    void SetCombineRaw(uint32_t w0, uint32_t w1) override {
        mG->GdxTrackSetCombine(w0, w1);
    }
    void SetPrimColor(uint8_t m, uint8_t l, GdxSeamRgba c) override {
        mG->GfxDpSetPrimColor(m, l, c.r, c.g, c.b, c.a);
    }
    void SetEnvColor(GdxSeamRgba c) override {
        mG->GfxDpSetEnvColor(c.r, c.g, c.b, c.a);
    }
    void Flush() override {
        mG->Flush();
    }
    size_t PendingTris() const override {
        return mG->mBufVboNumTris;
    }
    void SpTri(uint8_t a, uint8_t b, uint8_t c) override {
        mG->GfxSpTri1(a, b, c, false);
    }
    void PopAppended(size_t n0, size_t appended) override {
        mG->mBufVboNumTris = n0;
        mG->mBufVboLen -= appended * 3 * GDX3DS_VBOPACK_STRIDE;
    }
    void PackState(GdxTrackPackState* out) override {
        mG->GdxTrackPackState(out);
    }
    bool RejectVariant() const override {
        return mG->mF3dex2Variant == F3dex2Variant::Reject || mG->mF3dex2Variant == F3dex2Variant::FZeroFlxReject;
    }
    void TileUv(uint32_t tile, GdxSeamTileUv* out) const override {
        const auto& t = mG->mRdp->texture_tile[tile & 7u];
        out->shifts = t.shifts;
        out->shiftt = t.shiftt;
        out->uls = t.uls;
        out->ult = t.ult;
    }
    void ApplyDrawConstants() override {
        mG->mRapi->SetCurrentPrimDepth((float)mG->mRdp->prim_depth / kPrimDepthMax);
        mG->mRapi->SetCurrentAlphaCompareThreshold((float)mG->mRdp->blend_color.a / 255.0f);
    }
    bool BindMaterial(GdxSeamMaterial& mat) override;
    bool LatchVtxState(Gdx3dsVtxMathState& s) override;

  private:
    Interpreter* mG;
};

/* GfxSpTri1's draw-state resolution (interpreter.cpp), minus diagnostics and the s7 memo:
 * depth/decal/viewport, combiner key, texture import + sampler, shader program, blend.
 * Returns false for state shapes this path does not handle (caller replays via WalkRun). */
bool LusSeam::BindMaterial(GdxSeamMaterial& mat) {
    Interpreter* g = mG;
    RSP* rsp = g->mRsp;
    RDP* rdp = g->mRdp;
    GfxRenderingAPI* rapi = g->mRapi;
    RenderingState& rs = g->mRenderingState;

    const bool zbuffer_enabled = (rsp->geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    const bool prim_depth_enabled = (rdp->other_mode_l & G_ZS_PRIM) != 0;
    const bool depth_test = (zbuffer_enabled || prim_depth_enabled) && (rdp->other_mode_l & Z_CMP) == Z_CMP;
    const bool depth_mask = (rdp->other_mode_l & Z_UPD) == Z_UPD;
    const uint8_t depth_test_and_mask = (depth_test ? 1 : 0) | (depth_mask ? 2 : 0);
    if (depth_test_and_mask != rs.depth_test_and_mask) {
        g->Flush();
        rapi->SetDepthTestAndMask(depth_test, depth_mask);
        rs.depth_test_and_mask = depth_test_and_mask;
    }
    const bool zmode_decal = (rdp->other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rs.decal_mode) {
        g->Flush();
        rapi->SetZmodeDecal(zmode_decal);
        rs.decal_mode = zmode_decal;
    }
    if (rdp->viewport_or_scissor_changed) {
        if (memcmp(&rdp->viewport, &rs.viewport, sizeof(rdp->viewport)) != 0) {
            g->Flush();
            rapi->SetViewport(rdp->viewport.x, rdp->viewport.y, rdp->viewport.width, rdp->viewport.height);
            rs.viewport = rdp->viewport;
        }
        if (memcmp(&rdp->scissor, &rs.scissor, sizeof(rdp->scissor)) != 0) {
            g->Flush();
            rapi->SetScissor(rdp->scissor.x, rdp->scissor.y, rdp->scissor.width, rdp->scissor.height);
            rs.scissor = rdp->scissor;
        }
        rdp->viewport_or_scissor_changed = false;
    }

    const uint64_t cc_id = rdp->combine_mode;
    uint64_t cc_options = 0;
    bool use_alpha = ((rdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) &&
                      (rdp->other_mode_l & (3 << 16)) == (G_BL_1MA << 16)) ||
                     ((rdp->other_mode_l & (3 << 22)) == (G_BL_CLR_MEM << 22) &&
                      (rdp->other_mode_l & (3 << 18)) == (G_BL_1MA << 18));
    const uint8_t blend_src = rdp->other_mode_l >> 30;
    const bool use_blend_color = blend_src == G_BL_CLR_BL;
    const bool use_fog = blend_src == G_BL_CLR_FOG || use_blend_color;
    bool texture_edge = (rdp->other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = (rdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    bool alpha_threshold = (rdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    const bool invisible = (rdp->other_mode_l & (3 << 24)) == (G_BL_0 << 24) &&
                           (rdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    const bool use_grayscale = rdp->grayscale;
    const bool use_prim_depth = (rdp->other_mode_l & G_ZS_PRIM) != 0;
    if (texture_edge) {
        if (use_alpha) {
            alpha_threshold = true;
            texture_edge = false;
        }
        use_alpha = true;
    }
    if (alpha_threshold || use_noise) {
        use_alpha = true;
    }
    if (use_alpha) cc_options |= SHADER_OPT(ALPHA);
    if (use_fog) cc_options |= SHADER_OPT(FOG);
    if (texture_edge) cc_options |= SHADER_OPT(TEXTURE_EDGE);
    if (use_noise) cc_options |= SHADER_OPT(NOISE);
    if (use_2cyc) cc_options |= SHADER_OPT(_2CYC);
    if (alpha_threshold) cc_options |= SHADER_OPT(ALPHA_THRESHOLD);
    if (invisible) cc_options |= SHADER_OPT(INVISIBLE);
    if (use_grayscale) cc_options |= SHADER_OPT(GRAYSCALE);
    if (use_prim_depth) cc_options |= SHADER_OPT(PRIM_DEPTH);
    if (!g->mShaderStack.empty()) {
        cc_options |= (g->mShaderStack.top() << SHADER_ID_SHIFT);
    } else {
        cc_options |= -1 << SHADER_ID_SHIFT;
    }
    const uint32_t texel0Tile = rdp->first_tile_index;
    const uint32_t texel1Tile = (rdp->first_tile_index + 1u) & 7u;
    const auto& texel0Loaded = rdp->LoadedTextureAt(rdp->texture_tile[texel0Tile].tmem_index);
    const auto& texel1Loaded = rdp->LoadedTextureAt(rdp->texture_tile[texel1Tile].tmem_index);
    if (texel0Loaded.masked) cc_options |= SHADER_OPT(TEXEL0_MASK);
    if (texel1Loaded.masked) cc_options |= SHADER_OPT(TEXEL1_MASK);
    if (texel0Loaded.blended) cc_options |= SHADER_OPT(TEXEL0_BLEND);
    if (texel1Loaded.blended) cc_options |= SHADER_OPT(TEXEL1_BLEND);

    ColorCombinerKey key{};
    key.combine_mode = cc_id;
    key.options = cc_options;
    ColorCombiner* comb = g->LookupOrCreateColorCombiner(key);

    uint32_t tm = 0;
    for (int i = 0; i < 2; i++) {
        const uint32_t tile = (i == 1) ? texel1Tile : texel0Tile;
        mat.effective_tile[i] = tile;
        mat.tex_width[i] = mat.tex_height[i] = mat.tex_width2[i] = mat.tex_height2[i] = 0;
        if (!comb->usedTextures[i]) {
            continue;
        }
        const auto& activeLoaded = rdp->LoadedTextureAt(rdp->texture_tile[tile].tmem_index);
        if (rdp->textures_changed[i]) {
            g->Flush();
            g->ImportTexture(i, tile, false);
            if (activeLoaded.masked) {
                g->ImportTextureMask(SHADER_FIRST_MASK_TEXTURE + i, tile);
            }
            if (activeLoaded.blended) {
                g->ImportTexture(SHADER_FIRST_REPLACEMENT_TEXTURE + i, tile, true);
            }
            rdp->textures_changed[i] = false;
        }
        uint8_t cms = rdp->texture_tile[tile].cms;
        uint8_t cmt = rdp->texture_tile[tile].cmt;
        const uint32_t loaded_line_size = rdp->LoadedTextureAt(rdp->texture_tile[tile].tmem_index).line_size_bytes;
        const uint32_t loaded_size = rdp->LoadedTextureAt(rdp->texture_tile[tile].tmem_index).size_bytes;
        const uint32_t loaded_full_line =
            rdp->LoadedTextureAt(rdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
        uint32_t tex_size_bytes;
        uint32_t line_size;
        if ((loaded_line_size != loaded_size || loaded_full_line != loaded_size) && loaded_line_size > 0) {
            line_size = loaded_line_size;
            tex_size_bytes = loaded_size;
        } else {
            line_size = rdp->texture_tile[tile].line_size_bytes;
            tex_size_bytes = rdp->LoadedTextureAt(rdp->texture_tile[tile].tmem_index).orig_size_bytes;
            if (rdp->texture_tile[tile].siz == G_IM_SIZ_32b) {
                line_size *= 2;
            }
        }
        if (line_size == 0) {
            line_size = 1;
        }
        mat.tex_height[i] = tex_size_bytes / line_size;
        switch (rdp->texture_tile[tile].siz) {
            case G_IM_SIZ_4b:
                line_size <<= 1;
                break;
            case G_IM_SIZ_8b:
                break;
            case G_IM_SIZ_16b:
                line_size /= G_IM_SIZ_16b_LINE_BYTES;
                break;
            case G_IM_SIZ_32b:
                line_size /= 4;
                break;
        }
        mat.tex_width[i] = line_size;
        mat.tex_width2[i] = (uint32_t)(int32_t)((rdp->texture_tile[tile].lrs - rdp->texture_tile[tile].uls + 4) / 4);
        mat.tex_height2[i] = (uint32_t)(int32_t)((rdp->texture_tile[tile].lrt - rdp->texture_tile[tile].ult + 4) / 4);
        const uint32_t loadedPx = mat.tex_width[i] * mat.tex_height[i];
        const uint32_t renderedPx = mat.tex_width2[i] * mat.tex_height2[i];
        const bool pyrLike = renderedPx > 0 && loadedPx > renderedPx && loadedPx * 8 < renderedPx * 13;
        if ((pyrLike || (cms & G_TX_CLAMP)) && mat.tex_width2[i] > 0 && mat.tex_width2[i] < mat.tex_width[i]) {
            mat.tex_width[i] = mat.tex_width2[i];
        }
        if ((pyrLike || (cmt & G_TX_CLAMP)) && mat.tex_height2[i] > 0 && mat.tex_height2[i] < mat.tex_height[i]) {
            mat.tex_height[i] = mat.tex_height2[i];
        }
        ApplyTileMaskExtentMirror(rdp, tile, mat.tex_width[i], mat.tex_height[i], true);
        if (mat.tex_width[i] == 0) {
            const uint8_t masksFix = rdp->texture_tile[tile].masks;
            mat.tex_width[i] = (masksFix != G_TX_NOMASK && masksFix < 31) ? (1u << masksFix) : 1u;
        }
        if (mat.tex_height[i] == 0) {
            const uint8_t masktFix = rdp->texture_tile[tile].maskt;
            mat.tex_height[i] = (masktFix != G_TX_NOMASK && masktFix < 31) ? (1u << masktFix) : 1u;
        }
        const uint32_t tex_width1 = mat.tex_width[i] << (cms & G_TX_MIRROR);
        const uint32_t tex_height1 = mat.tex_height[i] << (cmt & G_TX_MIRROR);
        if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != mat.tex_width2[i])) {
            tm |= 1 << 2 * i;
            cms &= ~G_TX_CLAMP;
        }
        if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != mat.tex_height2[i])) {
            tm |= 1 << 2 * i + 1;
            cmt &= ~G_TX_CLAMP;
        }
        if (rs.mTextures[i] == nullptr) {
            continue;
        }
        const bool linear_filter = (rdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BILERP;
        if (!rs.sampler_valid[i] || linear_filter != rs.sampler_linear_filter[i] || cms != rs.sampler_cms[i] ||
            cmt != rs.sampler_cmt[i]) {
            g->Flush();
            if (activeLoaded.blended) {
                rapi->SetSamplerParameters(SHADER_FIRST_REPLACEMENT_TEXTURE + i, linear_filter, cms, cmt);
            }
            rapi->SetSamplerParameters(i, linear_filter, cms, cmt);
            rs.sampler_valid[i] = true;
            rs.sampler_linear_filter[i] = linear_filter;
            rs.sampler_cms[i] = cms;
            rs.sampler_cmt[i] = cmt;
        }
    }

    ShaderProgram* prg = comb->prg[tm];
    if (prg == nullptr) {
        comb->prg[tm] = prg =
            g->LookupOrCreateShaderProgram(comb->shader_id0, comb->shader_id1 | tm * SHADER_OPT(TEXEL0_CLAMP_S));
    }
    if (prg != rs.mShaderProgram) {
        g->Flush();
        rapi->UnloadShader(rs.mShaderProgram);
        rapi->LoadShader(prg);
        rs.mShaderProgram = prg;
    }
    if (use_alpha != rs.alpha_blend) {
        g->Flush();
        rapi->SetUseAlpha(use_alpha);
        rs.alpha_blend = use_alpha;
    }
    rapi->ShaderGetInfo(prg, &mat.numInputs, mat.usedTextures);
    const GfxClipParameters clip = rapi->GetClipParameters();
    mat.clip.zIsFrom0To1 = clip.z_is_from_0_to_1;
    mat.clip.invertY = clip.invertY;
    mat.inputMap = comb->shader_input_mapping;
    mat.combUsedTextures[0] = comb->usedTextures[0];
    mat.combUsedTextures[1] = comb->usedTextures[1];
    mat.prg = prg;
    mat.tm = tm;
    mat.use_alpha = use_alpha;
    mat.use_fog = use_fog;
    mat.use_blend_color = use_blend_color;
    return true;
}

/* Latch GfxSpVertex's RSP inputs. Returns false for shapes the header does not model. */
bool LusSeam::LatchVtxState(Gdx3dsVtxMathState& s) {
    Interpreter* g = mG;
    RSP* rsp = g->mRsp;
    memset(&s, 0, sizeof(s));
    memcpy(s.mp, rsp->MP_matrix, sizeof(s.mp));
    if (rsp->geometry_mode & G_LIGHTING_POSITIONAL) {
        return false;
    }
    /* AdjXForAspectRatio / G_EX_WIDESCREEN_STRETCH branch selection (GfxSpVertex). */
    const bool fixedAspectFramebuffer =
        g->mFbActive && g->mActiveFrameBuffer != g->mFrameBuffers.end() &&
        (!g->mActiveFrameBuffer->second.resize || g->mActiveFrameBuffer->second.forceFixedAspect);
    const float wf = (float)g->mCurDimensions.width;
    const float hf = (float)g->mCurDimensions.height;
    int adjMode = GDX3DS_VTX_X_NONE;
    if (!fixedAspectFramebuffer && g->mWidescreenEnabledCache && !g->mForceFixedAspectCache) {
        adjMode = GDX3DS_VTX_X_ADJ;
    }
    s.aspect = wf / hf;
    if ((rsp->extra_geometry_mode & G_EX_WIDESCREEN_STRETCH) != 0) {
        const bool widescreenFrameActive = !fixedAspectFramebuffer && (wf / hf) > (4.0f / 3.0f) &&
                                           g->mWidescreenEnabledCache && !g->mForceFixedAspectCache;
        s.xMode = widescreenFrameActive ? GDX3DS_VTX_X_STRETCH : adjMode;
    } else {
        s.xMode = adjMode;
    }
    s.lighting = (rsp->geometry_mode & G_LIGHTING) != 0;
    if (s.lighting) {
        if (rsp->lights_changed) {
            for (int i = 0; i < rsp->current_num_lights - 1; i++) {
                g->CalculateNormalDir(&rsp->current_lights[i].l, rsp->current_lights_coeffs[i]);
            }
            g->CalculateNormalDir(&rsp->lookat[0], rsp->current_lookat_coeffs[0]);
            g->CalculateNormalDir(&rsp->lookat[1], rsp->current_lookat_coeffs[1]);
            rsp->lights_changed = false;
        }
        s.numDir = rsp->current_num_lights - 1;
        if (s.numDir < 0 || s.numDir > GDX3DS_VTX_MAX_DIR_LIGHTS) {
            return false;
        }
        for (int i = 0; i < s.numDir; i++) {
            memcpy(s.coeffs[i], rsp->current_lights_coeffs[i], sizeof(s.coeffs[i]));
            memcpy(s.lightCol[i], rsp->current_lights[i].l.col, 3);
        }
        const int ambIdx = rsp->current_num_lights - 1;
        memcpy(s.ambient, rsp->current_lights[ambIdx].l.col, 3);
        s.textureGen = (rsp->geometry_mode & G_TEXTURE_GEN) != 0;
        s.textureGenLinear = (rsp->geometry_mode & G_TEXTURE_GEN_LINEAR) != 0;
        s.flxAlpha = g->mF3dex2Variant == F3dex2Variant::FZeroFlxReject && rsp->f3dflx_alpha_light_valid;
        if (s.flxAlpha) {
            gdx3ds_vtx_alpha_light(rsp->f3dflx_alpha_light,
                                   rsp->modelview_matrix_stack[rsp->modelview_matrix_stack_size - 1], s.alphaLight);
        }
        s.dmem = rsp->dmem;
        s.dmaIoDmem = (int)rsp->dma_io_dmem;
        memcpy(s.lookat, rsp->current_lookat_coeffs, sizeof(s.lookat));
    }
    s.fog = (rsp->geometry_mode & G_FOG) != 0;
    s.fogMul = rsp->fog_mul;
    s.fogOffset = rsp->fog_offset;
    return true;
}

LusSeam* sSeam = nullptr;

LusSeam& SeamFor(Interpreter* g) {
    if (sSeam == nullptr || sSeam->Owner() != g) {
        delete sSeam;
        sSeam = new LusSeam(g);
    }
    return *sSeam;
}

} // namespace

/* The two Interpreter members the vendored header declares for the port. */
void Interpreter::GdxTrackMarker(uint32_t w0, uintptr_t w1) {
    gdx3ds_track_marker_seam(SeamFor(this), w0, w1);
}
void Interpreter::GdxFxMarker(uint32_t w0, uintptr_t w1) {
    gdx3ds_fx_marker_seam(SeamFor(this), w0, w1);
}

/* The weak C entries the vendored G_NOOP handler calls; gfx_citro3d.cpp references them so
 * this object is always pulled out of libgdx3ds_gfx.a ahead of the LUS carve archive. */
extern "C" void gdx3ds_track_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1) {
    interp->GdxTrackMarker(w0, w1);
}
extern "C" void gdx3ds_fx_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1) {
    interp->GdxFxMarker(w0, w1);
}

/* The weak C entry gfx_dl_handler_common calls (gdx3ds_mesh.h). The bridge already baked
 * whatever it redirected, so no resolver is needed here. */
extern "C" int gdx3ds_mesh_dl_hook(void* interpreter, void* subGfx) {
    return gdx3ds_mesh_dl_hook_seam(SeamFor(static_cast<Interpreter*>(interpreter)), subGfx, nullptr, nullptr);
}
