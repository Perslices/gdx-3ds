/* gfx_citro3d_nrrmach.inc -- n60 NRR milestone 2: the machines' backend half
 * (nrr/gdx3ds_nrr_mach.h, docs/research/native/n60mach-progress.md). Included at the end of
 * gfx_citro3d.cpp, after the renderer singleton.
 *
 * Capture (cold): right after a legacy gpuxf machine run was drawn, read back the state it drew
 * with -- the shader's ids, the C3D_Tex bound per used unit with its sampler, depth test/mask,
 * decal, blend, the alpha-compare reference, the scissor and whether the fog unit was off -- so the
 * recorder can build the run's core state block (gdx3ds_nrr.h NrrStateKey). The hot draw is the
 * core's (NrrStateApply / NrrSetInputs / NrrSetFog / NrrDrawXf); nothing here runs per frame. */

namespace Fast {

struct NrrBackend {
    static int Capture(GfxRenderingAPIC3D& r, GdxNrrBackendRun* o) {
        const ShaderProgramC3D* prg = r.mCurrentShader;
        if (prg == nullptr) {
            return 0;
        }
        o->id0 = prg->id0;
        o->id1 = prg->id1;
        for (int t = 0; t < 2; t++) {
            o->tex[t] = nullptr;
            if (!prg->cc.usedTextures[t]) {
                continue;
            }
            const uint32_t id = r.mBoundTextureIds[t];
            if (id >= r.mTextures.size() || !r.mTextures[id].inited || r.mTextures[id].page >= 0) {
                return 0; /* not a standalone texture (an atlas view, or nothing uploaded) */
            }
            const auto& tx = r.mTextures[id];
            o->tex[t] = &tx.tex;
            o->texLinear[t] = tx.linearFilter ? 1 : 0;
            o->texCms[t] = (uint8_t)tx.cms;
            o->texCmt[t] = (uint8_t)tx.cmt;
        }
        o->depthTest = r.mDepthTest ? 1 : 0;
        o->depthMask = r.mDepthMask ? 1 : 0;
        o->decal = r.mZmodeDecal ? 1 : 0;
        o->useAlpha = r.mUseAlpha ? 1 : 0;
        o->alphaRef = (uint8_t)(int)(r.mCurrentAlphaCompareThreshold * 255.0f); /* ApplyAlphaTest's ref */
        o->fogOk = (GdxNoFogFile() || !(prg->cc.opt_fog && prg->fogOffset >= 0 && !prg->fogBlendStage)) ? 1 : 0;
        if (r.mScValid) {
            o->scissor[0] = (int16_t)r.mScX;
            o->scissor[1] = (int16_t)r.mScY;
            o->scissor[2] = (int16_t)r.mScW;
            o->scissor[3] = (int16_t)r.mScH;
        }
        return 1;
    }
};

} // namespace Fast

extern "C" int gdx3ds_nrr_backend_capture(GdxNrrBackendRun* out) {
    return Fast::NrrBackend::Capture(GdxCitro3dRendererSingleton(), out);
}
