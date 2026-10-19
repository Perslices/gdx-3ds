/* gfx_citro3d_nrr.inc -- the NRR core's backend bridge (n60core; contract
 * port/3ds/gfx/nrr/gdx3ds_nrr_internal.h). Included by gfx_citro3d.cpp inside namespace Fast, next
 * to gfx_citro3d_hsx.inc, so it shares the file's statics (the projection dedupe, the shader pool,
 * the elision trackers). Nothing here runs unless a [native] nrr category draws. The hand-back
 * itself is not here: gdx3ds_nrr.cpp restores the citro3d context groups it changed, which leaves
 * every cache of this backend (and the interpreter's render-state cache) true by construction. */

void GfxRenderingAPIC3D::NrrDepthMap(bool decal) {
    const bool saved = mZmodeDecal;
    SetZmodeDecal(decal); /* the one place the decal bias (and its gdx-decalbias.txt override) lives */
    mZmodeDecal = saved;
}

/* Cold: the backend's own combiner translation, without making the shader current (the
 * interpreter's mRs.shader_program must stay what the backend's mCurrentShader is). */
bool GfxRenderingAPIC3D::NrrTranslate(uint64_t id0, uint64_t id1, NrrBkShader* out) {
    auto it = mShaderPool.find(std::make_pair(id0, id1));
    ShaderProgramC3D* prg = nullptr;
    if (it != mShaderPool.end()) {
        prg = &it->second;
    } else {
        ShaderProgramC3D& p = mShaderPool[std::make_pair(id0, id1)];
        p.id0 = id0;
        p.id1 = id1;
        gfx_cc_get_features(id0, id1, &p.cc);
        ComputeVertexLayout(p);
        MapCombiner(p);
        prg = &p;
    }
    std::memset(out, 0, sizeof(*out));
    out->numStages = (uint8_t)(prg->numStages < NRR_BK_STAGES ? prg->numStages : NRR_BK_STAGES);
    out->fogStage = (int8_t)(prg->fogBlendStage ? prg->fogBlendStageIndex : -1);
    out->vtxColorInput = (int8_t)prg->vtxColorInput;
    out->vtxAlphaInput = (int8_t)prg->vtxAlphaInput;
    out->inputSize = (uint8_t)prg->inputSize;
    for (int u = 0; u < 2; u++) {
        out->usedTex[u] = prg->cc.usedTextures[u] ? 1 : 0;
        out->clamp[u][0] = prg->cc.clamp[u][0] ? 1 : 0;
        out->clamp[u][1] = prg->cc.clamp[u][1] ? 1 : 0;
    }
    out->texEdge = prg->cc.opt_texture_edge ? 1 : 0;
    out->alphaThreshold = prg->cc.opt_alpha_threshold ? 1 : 0;
    for (int s = 0; s < out->numStages; s++) {
        const TexEnvStageC3D& st = prg->stages[s];
        NrrBkStage& o = out->stages[s];
        o.rgbFunc = (uint8_t)st.rgbFunc;
        o.alphaFunc = (uint8_t)st.alphaFunc;
        for (int k = 0; k < 3; k++) {
            o.rgbSrc[k] = (uint8_t)st.rgbSrc[k];
            o.alphaSrc[k] = (uint8_t)st.alphaSrc[k];
            o.rgbOp[k] = (uint8_t)st.rgbOp[k];
            o.alphaOp[k] = (uint8_t)st.alphaOp[k];
        }
        o.constRgbInput = (int8_t)st.constRgbInput;
        o.constAlphaInput = (int8_t)st.constAlphaInput;
        o.constRgbFixed = (int8_t)st.constRgbFixed;
        o.constAlphaFixed = (int8_t)st.constAlphaFixed;
        o.constColor = st.constColor;
    }
    return prg->mapped || prg->numStages > 0;
}

bool GfxRenderingAPIC3D::NrrTex(uint32_t texId, NrrBkTex* out) const {
    if (texId != NRR_TEX_NONE && (texId & NRR_TEX_PAGE) != 0) {
        return NrrTexPage(texId & ~NRR_TEX_PAGE, out);
    }
    if (texId >= mTextures.size()) {
        return false;
    }
    const TextureC3D& t = mTextures[texId];
    if (!TexReady(t)) {
        return false;
    }
    if (t.page >= 0) {
        out->tex = const_cast<C3D_Tex*>(&mAtlasPages[(size_t)t.page].tex);
        sAtlasPageLastUsed[(size_t)t.page] = (unsigned long)mFrameIndex; /* as DrawPackedLean */
        out->uOff = t.uOff;
        out->vOff = t.vOff;
    } else {
        out->tex = const_cast<C3D_Tex*>(&t.tex);
        out->uOff = 0.0f;
        out->vOff = 0.0f;
    }
    out->uScale = t.uScale;
    out->vScale = t.vScale;
    return true;
}

/* An atlas page as a texture (NRR_TEX_PAGE): what every view on it binds, with no view offset. */
bool GfxRenderingAPIC3D::NrrTexPage(uint32_t page, NrrBkTex* out) const {
    if (page >= mAtlasPages.size() || !mAtlasPages[page].inited) {
        return false;
    }
    out->tex = const_cast<C3D_Tex*>(&mAtlasPages[page].tex);
    sAtlasPageLastUsed[page] = (unsigned long)mFrameIndex; /* as DrawPackedLean */
    out->uOff = 0.0f;
    out->vOff = 0.0f;
    out->uScale = 1.0f;
    out->vScale = 1.0f;
    return true;
}

/* ---- the C bridge (gdx3ds_nrr_internal.h) ---- */

extern "C" int gdx3ds_nrr_bk_info(NrrBkInfo* out) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr || !r->mInitialized) {
        return 0;
    }
    out->fixupMtx = &r->mFixupMatrix;
    out->attrLegacy = &r->mAttrLegacy;
    out->attrXf = &r->mAttrXf;
    out->attrXfF = &r->mAttrXfF;
    out->projLoc = r->mProjectionUniformLoc;
    out->xfUnifLoc = r->mXfUnifLoc;
    out->xfBoolLoc = r->mXfBoolLoc;
    out->stkLoc = Gdx3dsStereo::ReplayStkLoc();
    return 1;
}

/* NRR draws only into an open frame on the main (screen) target: mono, or -- with the 3D slider
 * up -- a frame in stereo frame-replay mode (n60stereo: the first NRR hand-off of the frame enters
 * it; every NRR draw is then emitted once and replayed for the right eye at EndFrame). */
extern "C" int gdx3ds_nrr_bk_frame_active(void) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr || !r->mInitialized || !r->mFrameActive || r->mCurrentFramebuffer != 0) {
        return 0;
    }
    if (r->mStereoEnabled && Gdx3dsStereo::Active()) {
        return Gdx3dsStereo::ReplayEnter() ? 1 : 0;
    }
    return 1;
}

extern "C" int gdx3ds_nrr_bk_translate(uint64_t id0, uint64_t id1, NrrBkShader* out) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    return (r != nullptr && r->NrrTranslate(id0, id1, out)) ? 1 : 0;
}

extern "C" int gdx3ds_nrr_bk_tex(uint32_t texId, NrrBkTex* out) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    return (r != nullptr && r->NrrTex(texId, out)) ? 1 : 0;
}

/* The frame's linear packed pool (NativeXfVertices' bump): FlushPendingVbo flushes it once
 * before submission (EndFrame / the [overlap] kick's prepare), NRR data included. */
extern "C" void* gdx3ds_nrr_bk_alloc(uint32_t bytes) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr || !r->mFrameActive || bytes == 0) {
        return nullptr;
    }
    const size_t floats = (((size_t)bytes + 15u) & ~(size_t)15u) / sizeof(float);
    const size_t start = (r->mVboOffsetFloats + 3u) & ~(size_t)3u; /* 16-byte aligned */
    if (start + floats > r->mVboPoolFloats) {
        return nullptr;
    }
    float* p = r->mVboPool + start;
    r->mVboOffsetFloats = start + floats;
    if (r->mVboOffsetFloats > r->mVboHighWaterFloats) {
        r->mVboHighWaterFloats = r->mVboOffsetFloats;
    }
    return p;
}

extern "C" void gdx3ds_nrr_bk_scissor(int x, int y, int w, int h) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r != nullptr) {
        r->ApplyScissorRect(x, y, w, h); /* translation only: mSc* (the backend's own) untouched */
    }
}

extern "C" void gdx3ds_nrr_bk_depth_map(int decal) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r != nullptr) {
        r->NrrDepthMap(decal != 0);
    }
}

/* nrrdiet: mVpAffineActive is SetViewport's sub-viewport fold ("a sub-viewport is menu 3D
 * anchored to 2D artwork", gfx_citro3d.cpp ~3391) -- it is only ever set for a non-full-window
 * viewport, which a race/sky/trk/mach/course frame never requests (its viewport is always the
 * full 400x240 window); only a menu/machmenu machine preview's sub-viewport sets it. Out of
 * line and cold so core's per-marker proj3d call stays the dispBase pick only. */
GDX_COLD const C3D_Mtx* Bk3dProj3dAffineFold(const GfxRenderingAPIC3D* r, const C3D_Mtx* dispBase) {
    static C3D_Mtx sFold;
    if (DispModeLatched() > 0) {
        dispBase = &sDispProjUi;
    }
    sFold = *dispBase;
    for (int i = 0; i < 4; i++) {
        const float mx = dispBase->r[i].x, my = dispBase->r[i].y;
        sFold.r[i].x = mx * r->mVpSx;
        sFold.r[i].y = my * r->mVpSy;
        sFold.r[i].w = dispBase->r[i].w + mx * r->mVpTx + my * r->mVpTy;
    }
    return &sFold;
}

/* [n60misc] The projection a packed 2D (NrrDraw2D) draw of the main target uploads: DrawTriangles'
 * dispBase for a screen-space draw (clip w == 1 classifies UI with no bridge override, and an
 * [anchor] prim-depth rect takes the UI map too). Mode 0 = the fixup; modes 1-3 = the border
 * mode's UI matrix (mode 1's is the fixup times 1.0, bit-identical; modes 2/3 zoom the HUD). */
extern "C" const void* gdx3ds_nrr_bk_proj2d(void) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr) {
        return nullptr;
    }
    if (DispModeLatched() > 0) {
        return &sDispProjUi;
    }
    return &r->mFixupMatrix;
}

/* [n60mach] The projection a mono 3D (gpuxf) draw of the main target uploads: DrawTriangles'
 * dispBase for a non-anchored draw (clip w != 1), with a folded viewport ([vpfix]: a split-screen
 * view) folded in exactly as DrawTriangles folds it (the result lives in a static: one per
 * hand-off, the caller copies it into the uniform at once). */
extern "C" const void* gdx3ds_nrr_bk_proj3d(void) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr) {
        return nullptr;
    }
    const C3D_Mtx* dispBase = &r->mFixupMatrix;
    if (DispModeLatched() > 0) {
        dispBase = Gdx3dsStereo::ClassifyDraw(false) == GDX3DS_STEREO_UI_ZERO_PARALLAX ? &sDispProjUi
                                                                                         : &sDispProjScene;
    }
    if (GDX_LIKELY(!r->mVpAffineActive)) {
        return dispBase;
    }
    return Bk3dProj3dAffineFold(r, dispBase);
}

/* n60stereo: an NRR prim-depth rectangle's stereo row -- DrawTriangles' [anchor] rule (the killswitch,
 * the depth, the receipt counters) for a draw the backend never sees. */
extern "C" void gdx3ds_nrr_bk_anchor_row(float d, float row[4]) {
    if (GdxStereoAnchorOn() == 0) {
        Gdx3dsStereo::ClassRow(GDX3DS_STEREO_UI_ZERO_PARALLAX, row);
        return;
    }
    Gdx3dsStereo::SetAnchorDepth(d);
    Gdx3dsStereo::ClassRow(GDX3DS_STEREO_ANCHORED, row);
    sAnchorDraws++;
    if (d < sAnchorDMin) {
        sAnchorDMin = d;
    }
    if (d > sAnchorDMax) {
        sAnchorDMax = d;
    }
}

/* ---- n60 nrr_trk (n60trk): a category whose state the interpreter resolves ---------------------
 *
 * The track's materials are resolved once (cold) by the interpreter's own bind
 * (BindMaterialRec), which leaves the backend holding exactly the draw state; this reads it back
 * as an NrrStateKey (shader ids, the bound textures and their sampler bits, depth/decal/blend,
 * scissor) so the hot loop applies a core state block instead of re-binding. Cull is the caller's
 * (a gpuxf draw takes it per draw). */
bool GfxRenderingAPIC3D::NrrCurrentKey(NrrStateKey* k) const {
    if (mCurrentShader == nullptr) {
        return false;
    }
    std::memset(k, 0, sizeof(*k));
    k->shaderId0 = mCurrentShader->id0;
    k->shaderId1 = mCurrentShader->id1;
    for (int u = 0; u < 2; u++) {
        const uint32_t id = mBoundTextureIds[u];
        if (!mCurrentShader->cc.usedTextures[u] || id >= mTextures.size()) {
            k->tex[u] = NRR_TEX_NONE;
            continue;
        }
        const TextureC3D& t = mTextures[id];
        k->tex[u] = id;
        k->texLinear[u] = t.linearFilter ? 1 : 0;
        k->texCms[u] = (uint8_t)t.cms;
        k->texCmt[u] = (uint8_t)t.cmt;
    }
    k->program = NRR_PROG_XF;
    k->depthTest = mDepthTest ? 1 : 0;
    k->depthWrite = mDepthMask ? 1 : 0;
    k->decal = mZmodeDecal ? 1 : 0;
    k->blend = mUseAlpha ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    k->alphaTest = NRR_AT_OFF; /* the core derives edge / threshold from the shader's own options */
    k->alphaRef = (uint8_t)(mCurrentAlphaCompareThreshold * 255.0f);
    NrrScissorKey(k->scissor);
    return true;
}

/* The scissor field of a key for the backend's scissor now (all 0 = the full window). */
void GfxRenderingAPIC3D::NrrScissorKey(int16_t out[4]) const {
    if (mScX == 0 && mScY == 0 && mScW == 400 && mScH == 240) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = (int16_t)mScX;
    out[1] = (int16_t)mScY;
    out[2] = (int16_t)mScW;
    out[3] = (int16_t)mScH;
}

extern "C" int gdx3ds_nrr_bk_current_key(NrrStateKey* k) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    return (r != nullptr && r->NrrCurrentKey(k)) ? 1 : 0;
}

extern "C" void gdx3ds_nrr_bk_scissor_key(int16_t out[4]) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    r->NrrScissorKey(out);
}

extern "C" uint32_t gdx3ds_nrr_bk_tex_serial(uint32_t texId) {
    GfxRenderingAPIC3D* r = sGdxOracleApi;
    if (r == nullptr || texId >= r->mTextures.size()) {
        return 0;
    }
    return r->mTextures[texId].serial;
}
