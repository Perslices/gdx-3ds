/* port/3ds/gfx/n64/gdx_hudsky_tri.inc -- [native] hudsky_emit: the sky's compact triangle path
 * (hudsky, docs/research/native/hudsky-progress.md). Included at the end of gdx_tri.cpp: it uses
 * that file's clipper (ClipPrepass / ClipPlaneFast / UpdateClipFlags, the o3dsfast forms) and
 * TrackMatOps::Culled (SpTri's backface rule, verbatim).
 *
 * WHY. The sky is geometry: ~22 triangles a frame, every one through the CPU clipper (the
 * screen-spanning gradient / floor / cloud quads straddle the frustum, the sprites behind the
 * camera clip to nothing), and each surviving fan triangle through SpTri. SpTri is 13 KB of
 * code -- the reject-variant screen, the verify side channel, the phase clock, the clipper
 * written twice, the gdxInput lambda, the emission -- and the fan re-enters it per triangle.
 * HsxSkyTri is SpTri for exactly the case the sky runs (a top-level, non-rectangle triangle
 * under the o3dsfast clipper, no verify, no trace clock, no reject variant): the same
 * statements in the same order, with the fan's triangles and the emission going through one
 * compact tail whose packing is the HUD direct path's (HudPackVerts / HudPackAux, now with the
 * non-rectangle half-texel term) -- the same code the HUD's direct rectangles emit through.
 *
 * EXACTNESS. Statement for statement SpTri on that case: the counters, the coverage marks, the
 * C6 replay tags, the clipper (same functions, same cap, same prepass), the fan's scratch slots,
 * the trivial reject, the backface rule, the deferred texture state and the state block
 * (SkyRectState / ResolveDrawState, unchanged), the batch begin and its GEOMLOSS cap, the
 * vertex-0 aux, the per-vertex emission, the fog note, the FULL flush. HsxSkyOk() is the case;
 * anything else is SpTri itself. Level 2 runs SpTri and checks, for every sky triangle SpTri
 * emits, that HudPackVerts produces its floats bit for bit (HsxSkyVerifyEmit). */

/* The case HsxSkyTri reproduces (latched per sky run, re-tested per triangle where it can move). */
bool Interp::HsxSkyOk() const {
    return !mVerifyActive && !mDxOn && mVariant == 0 && mO3dsFast != 0 && !mSkyNoClip && mClipFanDepth == 0 &&
           gdx_ninterp_cpuclip() != 0 && !mBackend->ProfActive();
}

/* SpTri's tail for a triangle that passed the clipper stage: the trivial reject, the backface
 * rule, the state block, the batch bookkeeping and the emission. */
void Interp::HsxSkyTail(LoadedVertex* v1, LoadedVertex* v2, LoadedVertex* v3, uint16_t replayTag) {
    const LoadedVertex* v_arr[3] = { v1, v2, v3 };
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        mStats.trisRejected++;
        mStats.triTrivialRej++;
        return;
    }
    if ((mRsp.geometry_mode & GM_CULL_BOTH) != 0 && TrackMatOps::Culled(*this, v1, v2, v3)) {
        mStats.trisCulled++;
        mStats.triCull++;
        return;
    }
    DrawState ds;
    if (mSkyPending) {
        SkyMaterialize();
    }
    if (!(mHudRectFast && HudRectState(false, ds)) && !(mSkyRectFast && SkyRectState(false, ds)) &&
        !ResolveDrawState(false, ds)) {
        return;
    }
    if (gdx3ds_replay_interp != nullptr) {
        ReplayBatchTag(replayTag);
    }
    if (mBufNumTris == 0) {
        uint32_t capTris = 0;
        mStats.triBegin++;
        mPackDst = mBackend->VboPackBegin(&mPackParams, &capTris);
        mPackBatch = (mPackDst != nullptr);
        mPackComb = ds.comb;
        if (!mPackBatch) {
            mStats.packRefused++;
            mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
        } else {
            mPackCapTris = capTris < static_cast<uint32_t>(kMaxTriBuffer) ? static_cast<size_t>(capTris)
                                                                         : static_cast<size_t>(kMaxTriBuffer);
            if (mPackCapTris < static_cast<size_t>(kMaxTriBuffer)) {
                mStats.packSplit++;
            }
        }
    }
    if (!mPackBatch) {
        return;
    }
    if (mBufNumTris == 0) {
        HudPackAux(ds, v1, &mPackAux);
    }
    float* gdxOut = mPackDst + mBufNumTris * (3 * GDX3DS_VBOPACK_STRIDE);
    if (gdx3ds_replay_interp != nullptr) {
        for (int i = 0; i < 3; i++) {
            const ptrdiff_t slot = v_arr[i] - &mRsp.loaded_vertices[0];
            gdx3ds_replay_interp->outVtx(gdxOut + i * GDX3DS_VBOPACK_STRIDE,
                                         (slot >= 0 && slot < kMaxVertices) ? static_cast<int>(slot) : -1);
        }
    }
    const bool bilerp = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
    HudPackVerts(ds, v_arr, 3, v1, gdxOut, bilerp);
    if (ds.use_fog) {
        if (ds.use_blend_color) {
            mBackend->FogNoteParams(0, 0, 0);
        } else {
            mBackend->FogNoteParams(mRsp.fog_mul, mRsp.fog_offset, 1);
        }
    }
    mStats.trisEmitted++;
    mStats.triEmitAll++;
    mHsxSkyTris++;
    if (++mBufNumTris >= mPackCapTris) {
        Flush(FLUSH_FULL);
    }
}

/* SpTri(a, b, c, false) at depth 0 under HsxSkyOk(). */
void Interp::HsxSkyTri(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    mStats.triCalls++;
    mStats.triTopCalls++;
    LoadedVertex* v1 = &mRsp.loaded_vertices[vtx1_idx];
    LoadedVertex* v2 = &mRsp.loaded_vertices[vtx2_idx];
    LoadedVertex* v3 = &mRsp.loaded_vertices[vtx3_idx];
    mStats.tris++;
    const uint16_t replayTag =
        gdx3ds_replay_interp != nullptr ? ReplayTriTag(vtx1_idx, vtx2_idx, vtx3_idx, false) : 0;
    if ((v1->clip_out | v2->clip_out | v3->clip_out) == 0) {
        HsxSkyTail(v1, v2, v3, replayTag);
        return;
    }
    mStats.triClipIn++;
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_NEEDED));
    LoadedVertex polygonA[16];
    polygonA[0] = *v1;
    polygonA[1] = *v2;
    polygonA[2] = *v3;
    LoadedVertex polygonB[16];
    LoadedVertex* input = polygonA;
    LoadedVertex* output = polygonB;
    size_t inputCount = 3;
    const size_t clipCap = mClipPolyMax;
    bool passed = false;
    const uint32_t firstPlane = ClipPrepass(*v1, *v2, *v3, &inputCount, &passed);
    if (passed && mClipPolyHw < 3u) {
        mClipPolyHw = 3u;
    }
    for (uint32_t plane = firstPlane; plane < 7 && inputCount >= 3; ++plane) {
        size_t outputCount = 0;
        bool truncated = false;
        ClipPlaneFast(input, inputCount, output, clipCap, plane, &outputCount, &truncated);
        if (outputCount > mClipPolyHw) {
            mClipPolyHw = static_cast<uint32_t>(outputCount);
        }
        if (truncated) {
            mStats.clipTrunc++;
            gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_TRUNC));
            if (mDropsiteClipLogged == 0) {
                mDropsiteClipLogged = 1;
                GDXN_LOG("DROPSITE: clip polygon TRUNCATED at %u vertices on plane %u (input "
                         "%u) -- the fan below draws a malformed polygon. A convex input "
                         "cannot exceed 3+7=10 against the shipped cap of 16, so at 16 this "
                         "would mean the polygon lost convexity",
                         static_cast<unsigned>(clipCap), static_cast<unsigned>(plane),
                         static_cast<unsigned>(inputCount));
            }
        }
        inputCount = outputCount;
        LoadedVertex* tmp = input;
        input = output;
        output = tmp;
    }
    if (inputCount < 3) {
        mStats.trisRejected++;
        mStats.triClipAllOut++;
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_ALLOUT));
        return;
    }
    mStats.triClipFan += static_cast<uint32_t>(inputCount - 2u);
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_FAN | ((uint32_t)inputCount << 8)));
    LoadedVertex saved[3];
    for (size_t i = 0; i < 3; ++i) {
        saved[i] = mRsp.loaded_vertices[kMaxVertices + i];
    }
    mStats.tris--;
    mReplayClipTag = replayTag;
    ++mClipFanDepth;
    LoadedVertex* f = &mRsp.loaded_vertices[kMaxVertices];
    for (size_t i = 1; i + 1 < inputCount; ++i) {
        f[0] = input[0];
        f[1] = input[i];
        f[2] = input[i + 1];
        for (size_t j = 0; j < 3; ++j) {
            LoadedVertex& cv = f[j];
            cv.w = cv.w > kMinClipW ? cv.w : kMinClipW;
            UpdateClipFlags(cv);
        }
        /* == SpTri(kMaxVertices, kMaxVertices + 1, kMaxVertices + 2, false) at depth 1: no reject
         * screen, no clipper; its replay tag is the clipped triangle's */
        mStats.triCalls++;
        mStats.tris++;
        HsxSkyTail(&f[0], &f[1], &f[2], gdx3ds_replay_interp != nullptr ? mReplayClipTag : 0);
    }
    --mClipFanDepth;
    for (size_t i = 0; i < 3; ++i) {
        mRsp.loaded_vertices[kMaxVertices + i] = saved[i];
    }
}

/* Level 2: SpTri just emitted a sky triangle at `out` from `v` under `ds`; HudPackVerts with the
 * same inputs must give the same floats, bit for bit. */
GDX_COLD void Interp::HsxSkyVerifyEmit(const DrawState& ds, const LoadedVertex* const* v, const float* out, bool halfTexel) {
    float pred[3 * GDX3DS_VBOPACK_STRIDE];
    HudPackVerts(ds, v, 3, v[0], pred, halfTexel);
    if (memcmp(pred, out, sizeof(pred)) == 0) {
        mHsxSkyVfyOk++;
    } else {
        mHsxSkyVfyBad++;
        mHsxSkyVfyBadSes++;
    }
}
