/* gfx_citro3d_hsx.inc -- [native] hudsky_emit: the compact emitter's backend half (hudsky,
 * docs/research/native/hudsky-progress.md). Included by gfx_citro3d.cpp inside namespace Fast,
 * so it shares the file's statics (the projection dedupe, the draw counters, the diagnostic
 * latches) with DrawTriangles.
 *
 * WHY. On the Old 3DS the HUD and the sky are instruction-fetch bound (hwprof §10.1: 48% of
 * their cycles stalled on I-fetch, 56-62 I-misses per 1k instructions; the track's resident
 * loop sits at 36). A retained HUD run replayed ~10 backend calls through two virtual layers,
 * and every batch it drained walked DrawTriangles: 11.5 KB of code whose hot packed path is
 * scattered between the uvdiag, mesh-capture, sky-wedge, livery, prim, verify, stereo and
 * gpuxf blocks. HsxRun executes a whole retained run (setters, begins, float slices, draws)
 * in one loop, and DrawPackedLean is the packed draw with only what a steady-state,
 * non-diagnostic, mono, non-gpuxf packed draw executes -- the same calls, in the same order,
 * with the same arguments, as DrawTriangles makes for that draw.
 *
 * EXACTNESS. HsxLeanOk() is the precondition under which DrawTriangles' packed path reduces to
 * DrawPackedLean statement for statement: no diagnostic armed (uvdiag, mesh capture, diag_sky,
 * livery, prim, nverify, the nativebatch stock hash), no gpuxf draw or indexed draw threading
 * through, no stereo pass. When it does not hold, HsxRun refuses (the interpreter replays the
 * recorded calls the stock way) and DrawPackedLean routes to DrawPackedBatch. Per draw, every
 * early-out DrawTriangles has (no shader, an exhausted pool) is routed to DrawPackedBatch too,
 * so the drop/log behaviour stays the stock one. The parity gate (fh, every SHOT BMP, gstate,
 * and the [hsx] gpu hash of every draw's command words) checks it on the corpus. */

bool GfxRenderingAPIC3D::HsxLeanOk() const {
    bool ok = mInitialized && mFrameActive && mXf == nullptr && !mNativeActive && !sNbStockArm &&
              sMeshCaptureFn == nullptr && !sLiveryArmed && !sPrimDiagArmed && gdx3ds_verify_mode == 0 &&
              !UvDiagOn() && !(mStereoEnabled && Gdx3dsStereo::Active());
#ifdef __3DS__
    ok = ok && !GdxDiagSkyOn();
#endif
    return ok;
}

/* DrawTriangles for a packed batch (mPackedAux != nullptr, mXf == nullptr, !mNativeActive)
 * under HsxLeanOk(): the statements that run there, in their order. */
void GfxRenderingAPIC3D::DrawPackedLean(size_t numTris, const Gdx3dsVboPackAux* aux) {
    const size_t numVerts = numTris * 3;
    const size_t vtxPoolFloats = numVerts * kOutStrideFloats;
    if (numTris == 0 || mCurrentShader == nullptr || !HsxLeanOk() ||
        mVboOffsetFloats + vtxPoolFloats > mVboPoolFloats) {
        DrawPackedBatch(numTris, aux); /* the stock early-outs, logs and counters */
        return;
    }
    const bool profOn = gdx3ds_prof_active != 0; /* [prof] DRW, as DrawTriangles' scope */
    uint64_t profT0 = 0, profSnap = 0;
    if (profOn) {
        profT0 = gdx3ds_prof_enter(&profSnap);
    }
    /* == DrawTriangles' C6 head (the untagged-draw note names this tail as its caller) */
    const uint16_t replayTag = gdx3ds_replay_rec ? gdx3ds_replay_take_draw_tag() : 0;
    if (replayTag == GDX_REPLAY_TAG_UNKNOWN && gdx3ds_replay_rec) {
        gdx3ds_replay_note_untagged(__builtin_return_address(0), sReplayOuterCaller, (unsigned)numTris);
    }
    sReplayOuterCaller = nullptr;
    const ShaderProgramC3D& prg = *mCurrentShader;
    float* const dst = mVboPool + mVboOffsetFloats;
    mPackedAux = aux;
    for (int tile = 0; tile < 2; tile++) {
        if (!prg.cc.usedTextures[tile]) {
            continue;
        }
        const uint32_t texId = mBoundTextureIds[tile];
        if (texId < mTextures.size() && TexReady(mTextures[texId])) {
            TextureC3D& t = mTextures[texId];
            const bool view = t.page >= 0;
            const GPU_TEXTURE_WRAP_PARAM wrapS = prg.cc.clamp[tile][0] ? GPU_CLAMP_TO_EDGE : WrapFromN64(t.cms);
            const GPU_TEXTURE_WRAP_PARAM wrapT = prg.cc.clamp[tile][1] ? GPU_CLAMP_TO_EDGE : WrapFromN64(t.cmt);
            C3D_Tex* bindTex = view ? &mAtlasPages[(size_t)t.page].tex : &t.tex;
            if (view) {
                sAtlasPageLastUsed[(size_t)t.page] = (unsigned long)mFrameIndex;
                const GPU_TEXTURE_FILTER_PARAM vf = t.linearFilter ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(bindTex, vf, vf);
            }
            C3D_TexSetWrap(bindTex, wrapS, wrapT);
            BindTexUnit(tile, bindTex);
            gdx3ds_gpuprof_note_tex_bind(texId);
        } else {
            mFrameBindMisses++;
            if (mMissingTexInited) {
                BindTexUnit(tile, &mMissingTex);
                sTexMissingBinds++;
            }
            GFX_C3D_LOG("DROPSITE: shader wants texel%d but texture %u is not uploaded (bound the "
                        "white fallback: %d)\n",
                        tile, mBoundTextureIds[tile], mMissingTexInited ? 1 : 0);
        }
    }
    (void)RefreshStageConstantsPacked(prg, *aux);
    ApplyAlphaTest();
    if (!GdxNoFogFile() && prg.cc.opt_fog && prg.fogOffset >= 0 && !prg.fogBlendStage) {
        UpdateFogState(prg, dst, numVerts, kOutStrideFloats, aux);
    } else {
        DisableFog();
    }
    const bool texBacked = mCurrentFramebuffer >= 0 && mCurrentFramebuffer < (int)mFramebuffers.size() &&
                           mFramebuffers[mCurrentFramebuffer].texBacked;
    if (mXfStateOn) {
        XfRestore();
    }
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, dst, kOutStrideFloats * sizeof(float), 4, 0x3210);
    mBufLastVerts = nullptr;
    const float clsW = dst[3];
    const bool anchoredDraw = GdxStereoAnchorOn() != 0 && !texBacked && prg.cc.opt_prim_depth && clsW == 1.0f;
    const C3D_Mtx* dispBase = &mFixupMatrix;
    if (!texBacked && DispModeLatched() > 0) {
        const int dispCls = Gdx3dsStereo::ClassifyDraw(clsW == 1.0f);
        dispBase = (dispCls == GDX3DS_STEREO_UI_ZERO_PARALLAX || anchoredDraw) ? &sDispProjUi : &sDispProjScene;
    }
    C3D_Mtx vpMtx;
    if (mVpAffineActive && !texBacked) {
        if (!texBacked && DispModeLatched() > 0) {
            dispBase = &sDispProjUi;
        }
        vpMtx = *dispBase;
        for (int i = 0; i < 4; i++) {
            const float mx = dispBase->r[i].x, my = dispBase->r[i].y;
            vpMtx.r[i].x = mx * mVpSx;
            vpMtx.r[i].y = my * mVpSy;
            vpMtx.r[i].w = dispBase->r[i].w + mx * mVpTx + my * mVpTy;
        }
        dispBase = &vpMtx;
        sDispProjLast = nullptr;
    }
    if (gdx3ds_replay_rec && gdx3ds_replay_legacy_needs_proj(replayTag)) {
        sDispProjLast = nullptr;
    }
    if (dispBase != sDispProjLast) {
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mProjectionUniformLoc, dispBase);
        sDispProjLast = (dispBase == &vpMtx) ? nullptr : dispBase;
    }
    if (gdx3ds_replay_rec) {
        gdx3ds_replay_note_vtx0(dst);
        gdx3ds_replay_note_range(dst, (unsigned)vtxPoolFloats);
        u32* const w0 = gpuCmdBuf + gpuCmdBufOffset;
        {
            GdxHwsScope hws(GDX3DS_HWS_DRAW);
            const u32 hsxW0 = gpuCmdBufOffset;
            C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
            if (HsxGpuHashOn()) {
                HsxGpuHashFold(hsxW0, dst, vtxPoolFloats);
            }
        }
        gdx3ds_replay_draw_post(replayTag, 0, w0, gpuCmdBuf + gpuCmdBufOffset, nullptr);
    } else {
        GdxHwsScope hws(GDX3DS_HWS_DRAW);
        const u32 hsxW0 = gpuCmdBufOffset;
        C3D_DrawArrays(GPU_TRIANGLES, 0, (int)numVerts);
        if (HsxGpuHashOn()) {
            HsxGpuHashFold(hsxW0, dst, vtxPoolFloats);
        }
    }
    mVboOffsetFloats += vtxPoolFloats;
    if (mVboOffsetFloats > mVboHighWaterFloats) {
        mVboHighWaterFloats = mVboOffsetFloats;
    }
    mFrameDrawCalls++;
    if (gdx3ds_cov_on) {
        CovNoteDraw(prg, texBacked, anchoredDraw, numVerts, numTris);
    }
    sPackedDrawCalls++;
    mFrameTris += (uint32_t)numTris;
    gdx3ds_gpuprof_note_verts((unsigned)numVerts);
    if (texBacked) {
        mFrameDrawsTexFb++;
    } else {
        mFrameDrawsScreenFb++;
    }
    mPackedAux = nullptr;
    sHsxLeanDraws++;
    if (profOn) {
        gdx3ds_prof_exit(GDX3DS_PROF_DRW, profT0, profSnap);
    }
}

/* VboPackBegin's normal case (a whole batch fits, no forced cap, triloop and vbosplit on, a
 * shader bound, a frame open): the cursor at the pool's free tail. The params VboPackBegin
 * computes are not needed (a retained run's batch params ride its record), and that case has
 * no other effect. Anything else takes VboPackBegin itself. */
float* GfxRenderingAPIC3D::HsxBegin() {
    const size_t freeFloats = mVboPoolFloats > mVboOffsetFloats ? mVboPoolFloats - mVboOffsetFloats : 0;
    if (GdxTriloopEnabled() && mInitialized && mFrameActive && mCurrentShader != nullptr && GdxVboSplitEnabled() &&
        GdxVboPackCap() == 0 && freeFloats / (3 * kOutStrideFloats) >= 256) {
        return mVboPool + mVboOffsetFloats;
    }
    Gdx3dsVboPackParams scratch;
    uint32_t cap = 0;
    return VboPackBegin(&scratch, &cap);
}

/* [native] nrr_hud: HsxBegin's normal case, reporting the cap (a whole 256-triangle batch). */
float* GfxRenderingAPIC3D::NrrPackCursor(uint32_t* capTris) {
    const size_t freeFloats = mVboPoolFloats > mVboOffsetFloats ? mVboPoolFloats - mVboOffsetFloats : 0;
    if (GdxTriloopEnabled() && mInitialized && mFrameActive && mCurrentShader != nullptr && GdxVboSplitEnabled() &&
        GdxVboPackCap() == 0 && freeFloats / (3 * kOutStrideFloats) >= 256) {
        *capTris = 256;
        return mVboPool + mVboOffsetFloats;
    }
    Gdx3dsVboPackParams scratch;
    return VboPackBegin(&scratch, capTris);
}

bool GfxRenderingAPIC3D::HsxRun(Gdx3dsHsxProgram* pr) {
    if (!HsxLeanOk()) {
        return false;
    }
    float* cursor = pr->cursor;
    constexpr size_t kTriFloats = 3u * GDX3DS_VBOPACK_STRIDE;
    for (uint32_t k = 0; k < pr->nOps; k++) {
        const Gdx3dsHsxOp& o = pr->ops[k];
        switch (o.op) {
            case GDX3DS_HSX_UNLOAD: UnloadShader((ShaderProgram*)o.p); break;
            case GDX3DS_HSX_LOAD: LoadShader((ShaderProgram*)o.p); break;
            case GDX3DS_HSX_SELTEX: SelectTexture(o.i[0], (uint32_t)o.i[1]); break;
            case GDX3DS_HSX_SAMPLER:
                SetSamplerParameters(o.i[0], o.a != 0, (uint32_t)o.i[1], (uint32_t)o.i[2]);
                break;
            case GDX3DS_HSX_DEPTH: SetDepthTestAndMask(o.a != 0, o.b != 0); break;
            case GDX3DS_HSX_DECAL: SetZmodeDecal(o.a != 0); break;
            case GDX3DS_HSX_VIEWPORT: SetViewport(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_SCISSOR: SetScissor(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_ALPHA: SetUseAlpha(o.a != 0); break;
            case GDX3DS_HSX_PRIMDEPTH: SetCurrentPrimDepth(o.f); break;
            case GDX3DS_HSX_ALPHATHR: SetCurrentAlphaCompareThreshold(o.f); break;
            case GDX3DS_HSX_FOGNOTE: gdx3ds_fog_note_params(o.i[0], o.i[1], o.i[2]); break;
            case GDX3DS_HSX_CLEARDEPTH: ClearDepthRegion(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_ATLASARM: AtlasArm(o.a != 0); break;
            case GDX3DS_HSX_BEGIN:
                cursor = HsxBegin();
                pr->begins++;
                break;
            case GDX3DS_HSX_FLOATS:
                if (cursor != nullptr) {
                    memcpy(cursor + (size_t)o.i[0] * kTriFloats, pr->floats + o.i[2],
                           (size_t)o.i[1] * kTriFloats * sizeof(float));
                }
                break;
            case GDX3DS_HSX_DRAW:
                if (gdx3ds_replay_interp != nullptr) {
                    /* == HudRetainReplay's C6 tag: a batch this run began is screen space, a batch
                     * open before the run holds this frame's triangles and takes the live tag */
                    gdx3ds_replay_interp->setDrawTag(o.screen != 0 ? (uint16_t)GDX_REPLAY_TAG_SCREEN : pr->liveTag);
                }
                DrawPackedLean((size_t)o.i[0], &pr->auxs[o.i[1]]);
                pr->draws++;
                cursor = nullptr;
                break;
            default: break;
        }
    }
    pr->cursor = cursor;
    sHsxRuns++;
    return true;
}
