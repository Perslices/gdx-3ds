/* port/3ds/gfx/n64/gdx_tri.cpp -- per-triangle path: reject-variant screen, cull, state diff
 * -> flush, combiner key, tile import + extents, sampler, shader, packed-VBO emission with the
 * aux record, texrect tris. Mirrors Interpreter::GfxSpTri1 (interpreter.cpp:3904-5605) on its
 * default 3DS configuration ([trifast] fast emission, [triloop] packed batches, [trectbatch]
 * atlas protocol) and Interpreter::Flush (:663). Plan §3.9/§3.10, invariants (19)-(26). */
#include "gdx_interp.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "gdx3ds_verify.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx_log.h"
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)

/* [native] ncensus tick source (gdx_rt_glue.cpp -> svcGetSystemTick; 0 in the host harness). */
extern "C" long long gdx_ninterp_now(void);

/* [nverify] source-identity fields for the tri side channel: FNV of the first 64 source bytes,
 * FNV of 64 TMEM bytes at the render tile, bridge address class. Harness-safe (seam + inline). */
static uint32_t VfyFnv64(const void* p, size_t n) { /* same word-then-byte FNV as gfx_citro3d VfyFnv */
    uint32_t h = 2166136261u;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
/* [nverify] whole-load-extent TMEM identity (see gfx_citro3d.cpp): a 64-byte prefix agrees on
 * any texture whose first rows are static and hides the divergence further in. The source hash
 * stays at 64 bytes -- size_bytes is the requested extent, not the readable one. */
static constexpr uint32_t kVfySrcHashCap = 4096u;
/* MUST equal gfx_citro3d.cpp's kVfyTmemHashCap: the LUS twin caps the TMEM hash at 256 bytes
 * and this side capped it at kVfySrcHashCap (4096), so for any load over 256 bytes the two
 * `tm=` fields hashed different lengths and could never agree. The field is receipt-only, but
 * it is the field a texture-identity mismatch is read against -- it said "the two TMEMs
 * differ" on every large texture, including ones that were identical. */
static constexpr uint32_t kVfyTmemHashCap = 4096u;
/* The TMEM bytes a load actually occupies. size_bytes counts SOURCE bytes; LOADTILE writes each
 * row at an 8-byte-aligned destination stride, so for a 74-byte row the footprint is 80 per row
 * and the last rows of the tile sit BEYOND size_bytes -- which is exactly where the decode reads
 * and where a hash over size_bytes could not see a divergence. */
static inline uint32_t VfyTmemFootprint(uint32_t sizeBytes, uint32_t lineBytes) {
    if (lineBytes == 0 || sizeBytes == 0) return sizeBytes;
    const uint32_t stride = (lineBytes + 7u) & ~7u;
    if (stride == lineBytes) return sizeBytes;
    const uint32_t rows = sizeBytes / lineBytes;
    if (rows == 0) return sizeBytes;
    return (rows - 1u) * stride + lineBytes;
}
static inline void VfyBytes8(unsigned* out, const uint8_t* p, uint32_t n) {
    out[0] = out[1] = 0u;
    if (p == nullptr) return;
    const uint32_t take = n < 8u ? n : 8u;
    uint8_t b[8] = {};
    memcpy(b, p, take);
    memcpy(out, b, 8);
}
static void VfyTriSource(unsigned long* addrOut, unsigned* clsOut, unsigned* srcOut, unsigned* tmemOut,
                         unsigned* srcB, unsigned* tmemB,
                         const uint8_t* addr, uint32_t sizeBytes, const uint8_t* tmem, uint32_t tmemBytes) {
    *addrOut = reinterpret_cast<unsigned long>(addr);
    *clsOut = addr ? gdx_gfx_classify_host_ptr(*addrOut) : 0u;
    *srcOut = addr ? VfyFnv64(addr, sizeBytes < 64u ? sizeBytes : 64u) : 0u;
    *tmemOut = tmem ? VfyFnv64(tmem, tmemBytes < kVfyTmemHashCap ? tmemBytes : kVfyTmemHashCap) : 0u;
    if (srcB != nullptr) {
        VfyBytes8(srcB, addr, sizeBytes);
        VfyBytes8(srcB + 2, addr && sizeBytes >= 8u ? addr + (sizeBytes - 8u) : nullptr, 8u);
    }
    if (tmemB != nullptr) {
        VfyBytes8(tmemB, tmem, tmemBytes);
        VfyBytes8(tmemB + 2, tmem && tmemBytes >= 8u ? tmem + (tmemBytes - 8u) : nullptr, 8u);
    }
}

namespace gdxn {

static constexpr float kMinClipW = 1.0e-5f;
static constexpr float kPrimDepthMax = 32767.0f;

static inline float FminExact(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

/* [native] ncensus: batch-size buckets 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+. */
static inline unsigned BatchBucket(size_t tris) {
    if (tris <= 2) return static_cast<unsigned>(tris) - 1u;
    unsigned b = 2;
    size_t hi = 4;
    while (b + 1u < kBatchHistBuckets && tris > hi) {
        b++;
        hi <<= 1;
    }
    return b;
}

void Interp::Flush(FlushReason why) {
    mStats.flush[why]++;
    if (mBufNumTris > 0) {
        mStats.batches++;
        /* Census timing wraps the whole drain (state application + submit): the ms a class is
         * charged is the submit cost of the batches it ended. Two tick reads per drained batch,
         * only with [native] ncensus=1. */
        const uint64_t t0 = mCensus ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        mBackend->SetCurrentPrimDepth(static_cast<float>(mRdp.prim_depth) / kPrimDepthMax);
        mBackend->SetCurrentAlphaCompareThreshold(static_cast<float>(mRdp.blend_color.a) / 255.0f);
        if (mPackBatch) {
            mBackend->DrawPackedBatch(mBufNumTris, &mPackAux);
        }
        if (mCensus) {
            mStats.flushDrain[why]++;
            mStats.flushTicks[why] += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
            mStats.batchHist[BatchBucket(mBufNumTris)]++;
            mStats.batchTris += static_cast<uint32_t>(mBufNumTris);
        }
        mPackBatch = false;
        mPackDst = nullptr;
        mPackCapTris = static_cast<size_t>(kMaxTriBuffer); /* GEOMLOSS: re-asked at the next begin */
        mBufNumTris = 0;
        mPackComb = nullptr;
        mPackFolded = false;
        mTriMemoValid = false; /* the memo described the batch just drawn */
    }
}

/* == LUS GdxHudRegFoldable ([native] fx_hud), over gdxn::Backend::BatchConstInputs. */
static inline bool MuxIsReg(uint8_t mux, bool isPrim, bool alphaChannel) {
    if (isPrim) {
        if (alphaChannel) {
            return mux == ACMUX_PRIMITIVE || mux == ACMUX_PRIM_LOD_FRAC;
        }
        return mux == CCMUX_PRIMITIVE || mux == CCMUX_PRIMITIVE_ALPHA || mux == CCMUX_PRIM_LOD_FRAC;
    }
    if (alphaChannel) {
        return mux == ACMUX_ENVIRONMENT;
    }
    return mux == CCMUX_ENVIRONMENT || mux == CCMUX_ENV_ALPHA;
}

bool Interp::RegFoldable(bool isPrim) const {
    if (!mPackBatch || mPackComb == nullptr) {
        return false;
    }
    uint32_t rgbMask = 0, alphaMask = 0;
    if (!mBackend->BatchConstInputs(&rgbMask, &alphaMask)) {
        return false;
    }
    for (int j = 0; j < 7; j++) {
        if (((rgbMask >> j) & 1u) && MuxIsReg(mPackComb->shader_input_mapping[0][j], isPrim, false)) {
            return false;
        }
        if (((alphaMask >> j) & 1u) && MuxIsReg(mPackComb->shader_input_mapping[1][j], isPrim, true)) {
            return false;
        }
    }
    /* The alpha channel's per-vertex slot only really rides per vertex when the batch wants a
     * vertex alpha at all and the fog factor has not claimed out[9]. */
    const int ai = mPackParams.vtxAlphaInput - 1;
    if (ai >= 0 && MuxIsReg(mPackComb->shader_input_mapping[1][ai], isPrim, true) &&
        (!mPackParams.wantVtxAlpha || mPackParams.fogFactorToAlpha)) {
        return false;
    }
    return true;
}

/* A prim/env VALUE change on an open batch: census it, then either absorb it ([native]
 * ownfold) or split as before. Behaviour with ownfold=0 is byte-for-byte the old path. */
void Interp::NoteRegChange(bool isPrim, FlushReason why) {
    if (mBufNumTris == 0) {
        mStats.flush[why]++; /* a no-op flush; keep the call count comparable */
        return;
    }
    const int reg = isPrim ? 0 : 1;
    const bool foldable = (mCensus || mOwnFold) ? RegFoldable(isPrim) : false;
    if (mCensus && foldable) {
        mStats.foldable[reg]++;
    }
    if (mOwnFold && foldable) {
        mStats.folded[reg]++;
        mPackFolded = true;
        return;
    }
    Flush(why);
}

void* Interp::LookupOrCreateShader(uint64_t id0, uint64_t id1) {
    void* prg = mBackend->LookupShader(id0, id1);
    if (prg == nullptr) {
        mBackend->UnloadShader(mRs.shader_program);
        prg = mBackend->CreateAndLoadNewShader(id0, id1);
        mRs.shader_program = prg;
    }
    return prg;
}

static inline LoadedVertex Interpolate(const LoadedVertex& a, const LoadedVertex& b, float t) {
    LoadedVertex r;
    memset(&r, 0, sizeof(r));
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    const auto cl = [](float v) { return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v)); };
    r.color.r = cl(a.color.r + (b.color.r - a.color.r) * t);
    r.color.g = cl(a.color.g + (b.color.g - a.color.g) * t);
    r.color.b = cl(a.color.b + (b.color.b - a.color.b) * t);
    r.color.a = cl(a.color.a + (b.color.a - a.color.a) * t);
    return r;
}

static inline void UpdateClipFlags(LoadedVertex& v) {
    uint8_t m = 0;
    if (v.w - kMinClipW < 0.0f) m |= 1u << 0;
    if (v.x + v.w < 0.0f) m |= 1u << 1;
    if (v.w - v.x < 0.0f) m |= 1u << 2;
    if (v.y + v.w < 0.0f) m |= 1u << 3;
    if (v.w - v.y < 0.0f) m |= 1u << 4;
    if (v.z + v.w < 0.0f) m |= 1u << 5;
    if (v.w - v.z < 0.0f) m |= 1u << 6;
    v.clip_out = m;
    v.clip_rej = 0;
    if (v.x < -v.w) v.clip_rej |= 1;
    if (v.x > v.w) v.clip_rej |= 2;
    if (v.y < -v.w) v.clip_rej |= 4;
    if (v.y > v.w) v.clip_rej |= 8;
    if (v.z < -v.w) v.clip_rej |= 16;
    if (v.z > v.w) v.clip_rej |= 32;
}

static inline float ClipDistance(const LoadedVertex& v, uint32_t plane) {
    switch (plane) {
        case 0: return v.w - kMinClipW;
        case 1: return v.x + v.w;
        case 2: return v.w - v.x;
        case 3: return v.y + v.w;
        case 4: return v.w - v.y;
        case 5: return v.z + v.w;
        default: return v.w - v.z;
    }
}

/* SpTri's state block: depth/decal/viewport/scissor diff, combiner key, texture import +
 * extents + sampler, shader program, blend. Binds the backend and banks the tri memo; no
 * emission. Mirrors GfxSpTri1 (interpreter.cpp) expression for expression; the machines
 * binder (seam BindMaterial) runs it against the run's registers. */
bool Interp::ResolveDrawState(bool is_rect, DrawState& out) {
    const bool zbuffer_enabled = (mRsp.geometry_mode & GM_ZBUFFER) == GM_ZBUFFER;
    const bool prim_depth_enabled = (mRdp.other_mode_l & ZS_PRIM) != 0;
    const bool depth_test = (zbuffer_enabled || prim_depth_enabled) && (mRdp.other_mode_l & RM_Z_CMP) == RM_Z_CMP;
    const bool depth_mask = (mRdp.other_mode_l & RM_Z_UPD) == RM_Z_UPD;
    const uint8_t depth_test_and_mask = (depth_test ? 1 : 0) | (depth_mask ? 2 : 0);
    if (depth_test_and_mask != mRs.depth_test_and_mask) {
        Flush(FLUSH_DEPTH);
        mBackend->SetDepthTestAndMask(depth_test, depth_mask);
        mRs.depth_test_and_mask = depth_test_and_mask;
    }
    const bool zmode_decal = (mRdp.other_mode_l & RM_ZMODE_DEC) == RM_ZMODE_DEC;
    if ((zmode_decal ? 1u : 0u) != mRs.decal_mode) {
        Flush(FLUSH_DECAL);
        mBackend->SetZmodeDecal(zmode_decal);
        mRs.decal_mode = zmode_decal ? 1 : 0;
    }
    if (mRdp.viewport_or_scissor_changed) {
        if (memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0) {
            Flush(FLUSH_VIEWPORT);
            mBackend->SetViewport(mRdp.viewport.x, mRdp.viewport.y, mRdp.viewport.width, mRdp.viewport.height);
            mRs.viewport = mRdp.viewport;
        }
        if (memcmp(&mRdp.scissor, &mRs.scissor, sizeof(Rect)) != 0) {
            Flush(FLUSH_SCISSOR);
            mBackend->SetScissor(mRdp.scissor.x, mRdp.scissor.y, mRdp.scissor.width, mRdp.scissor.height);
            mRs.scissor = mRdp.scissor;
        }
        mRdp.viewport_or_scissor_changed = false;
    }

    const uint64_t cc_id = mRdp.combine_mode;
    uint64_t cc_options = 0;
    bool use_alpha = ((mRdp.other_mode_l & (3 << 20)) == (BL_CLR_MEM << 20) &&
                      (mRdp.other_mode_l & (3 << 16)) == (BL_1MA << 16)) ||
                     ((mRdp.other_mode_l & (3 << 22)) == (BL_CLR_MEM << 22) &&
                      (mRdp.other_mode_l & (3 << 18)) == (BL_1MA << 18));
    const uint8_t blend_src = static_cast<uint8_t>(mRdp.other_mode_l >> 30);
    const bool use_blend_color = blend_src == BL_CLR_BL;
    const bool use_fog = blend_src == BL_CLR_FOG || use_blend_color;
    bool texture_edge = (mRdp.other_mode_l & RM_CVG_X_ALPHA) == RM_CVG_X_ALPHA;
    const bool use_noise = (mRdp.other_mode_l & (3u << MDSFT_ALPHACOMPARE)) == (AC_DITHER << MDSFT_ALPHACOMPARE);
    const bool use_2cyc = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_2CYCLE;
    bool alpha_threshold = (mRdp.other_mode_l & (3u << MDSFT_ALPHACOMPARE)) == (AC_THRESHOLD << MDSFT_ALPHACOMPARE);
    const bool invisible = (mRdp.other_mode_l & (3 << 24)) == (BL_0 << 24) &&
                           (mRdp.other_mode_l & (3 << 20)) == (BL_CLR_MEM << 20);
    const bool use_grayscale = mRdp.grayscale;
    const bool use_prim_depth = (mRdp.other_mode_l & ZS_PRIM) != 0;

    if (texture_edge) {
        if (use_alpha) {
            alpha_threshold = true;
            texture_edge = false;
        }
        use_alpha = true;
    }
    if (alpha_threshold || use_noise) use_alpha = true;
    if (use_alpha) cc_options |= SHOPT_ALPHA;
    if (use_fog) cc_options |= SHOPT_FOG;
    if (texture_edge) cc_options |= SHOPT_TEXTURE_EDGE;
    if (use_noise) cc_options |= SHOPT_NOISE;
    if (use_2cyc) cc_options |= SHOPT_2CYC;
    if (alpha_threshold) cc_options |= SHOPT_ALPHA_THRESHOLD;
    if (invisible) cc_options |= SHOPT_INVISIBLE;
    if (use_grayscale) cc_options |= SHOPT_GRAYSCALE;
    if (use_prim_depth) cc_options |= SHOPT_PRIM_DEPTH;
    cc_options |= static_cast<uint64_t>(static_cast<int64_t>(-1) << SHADER_ID_SHIFT); /* empty shader stack */

    const uint32_t texel0Tile = mRdp.first_tile_index;
    const uint32_t texel1Tile = (mRdp.first_tile_index + 1u) & 7u;
    /* masked/blended are never set on this port (no replacement packs). */

    CombinerKey key;
    key.combine_mode = cc_id;
    key.options = cc_options;
    bool created = false;
    Combiner* comb = mCcPool.Lookup(key, &created);
    if (comb == nullptr) {
        return false;
    }
    if (created) {
        Flush(FLUSH_COMBINER);
    }

    uint32_t tm = 0;
    uint32_t tex_width[2] = { 0, 0 }, tex_height[2] = { 0, 0 }, tex_width2[2] = { 0, 0 }, tex_height2[2] = { 0, 0 };
    uint32_t effective_tile[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        const uint32_t tile = (i == 1) ? texel1Tile : texel0Tile;
        effective_tile[i] = tile;
        if (!comb->usedTextures[i]) continue;
        const Tile& tt = mRdp.texture_tile[tile];
        if (mRdp.textures_changed[i]) {
            /* [trectbatch] atlas protocol, verbatim order (plan §3.10). */
            const int16_t prevNode = mRs.textures[i];
            const bool trectRect = mTrectBatchOn && is_rect;
            const bool trectDefer = trectRect && mBufNumTris > 0 && mPackBatch && !mPackFolded && prevNode >= 0 &&
                                    mTexCount + 1 < kTexCacheSize;
            const uint32_t prevId = trectDefer ? mTex[prevNode].texture_id : 0u;
            const int prevPage = trectDefer ? mBackend->TexPage(prevId) : -1;
            if (!trectDefer) {
                Flush(FLUSH_TEXTURE);
            }
            const bool trectArm = trectRect && i == 0 && mTrectRectAtlasOk;
            if (trectArm) mBackend->AtlasArm(true);
            ImportTexture(i, static_cast<int>(tile));
            if (trectArm) mBackend->AtlasArm(false);
            if (trectDefer) {
                const int16_t newNode = mRs.textures[i];
                const uint32_t newId = newNode >= 0 ? mTex[newNode].texture_id : 0u;
                const int newPage = newNode >= 0 ? mBackend->TexPage(newId) : -1;
                if (newNode == prevNode || (prevPage >= 0 && newPage == prevPage)) {
                    if (newNode != prevNode) {
                        mBackend->SetSamplerParameters(i, mRs.sampler_linear[i], mRs.sampler_cms[i], mRs.sampler_cmt[i]);
                        mBackend->VboPackRefresh(&mPackParams);
                    }
                } else {
                    mBackend->SelectTexture(i, prevId);
                    Flush(FLUSH_TEXTURE);
                    if (newNode >= 0) mBackend->SelectTexture(i, newId);
                }
            }
            mRdp.textures_changed[i] = false;
        }

        uint8_t cms = tt.cms;
        uint8_t cmt = tt.cmt;
        const LoadedTexture lt = LoadedTexAt(tt.tmem_index);
        const uint32_t loaded_line_size = lt.line_size_bytes;
        const uint32_t loaded_size = lt.size_bytes;
        const uint32_t loaded_full_line = lt.full_image_line_size_bytes;
        uint32_t tex_size_bytes;
        uint32_t line_size;
        if ((loaded_line_size != loaded_size || loaded_full_line != loaded_size) && loaded_line_size > 0) {
            line_size = loaded_line_size;
            tex_size_bytes = loaded_size;
        } else {
            line_size = tt.line_size_bytes;
            tex_size_bytes = lt.orig_size_bytes;
            if (tt.siz == IM_SIZ_32b) line_size *= 2;
        }
        if (line_size == 0) line_size = 1;
        tex_height[i] = tex_size_bytes / line_size;
        switch (tt.siz) {
            case IM_SIZ_4b: line_size <<= 1; break;
            case IM_SIZ_8b: break;
            case IM_SIZ_16b: line_size /= 2; break;
            case IM_SIZ_32b: line_size /= 4; break;
        }
        tex_width[i] = line_size;
        tex_width2[i] = static_cast<uint32_t>(static_cast<int32_t>((tt.lrs - tt.uls + 4) / 4));
        tex_height2[i] = static_cast<uint32_t>(static_cast<int32_t>((tt.lrt - tt.ult + 4) / 4));
        const uint32_t loadedPx = tex_width[i] * tex_height[i];
        const uint32_t renderedPx = tex_width2[i] * tex_height2[i];
        const bool pyrLike = renderedPx > 0 && loadedPx > renderedPx && loadedPx * 8 < renderedPx * 13;
        if ((pyrLike || (cms & TX_CLAMP)) && tex_width2[i] > 0 && tex_width2[i] < tex_width[i]) tex_width[i] = tex_width2[i];
        if ((pyrLike || (cmt & TX_CLAMP)) && tex_height2[i] > 0 && tex_height2[i] < tex_height[i]) tex_height[i] = tex_height2[i];
        /* ApplyTileMaskExtent(maskAuthoritative=true) */
        if (tt.masks != TX_NOMASK && tt.masks < 31) {
            if ((tt.cms & TX_CLAMP) == 0) tex_width[i] = 1u << tt.masks;
            else if (tex_width[i] > (1u << tt.masks)) tex_width[i] = 1u << tt.masks;
        }
        if (tt.maskt != TX_NOMASK && tt.maskt < 31) {
            if ((tt.cmt & TX_CLAMP) == 0) tex_height[i] = 1u << tt.maskt;
            else if (tex_height[i] > (1u << tt.maskt)) tex_height[i] = 1u << tt.maskt;
        }
        if (tex_width[i] == 0) tex_width[i] = (tt.masks != TX_NOMASK && tt.masks < 31) ? (1u << tt.masks) : 1u;
        if (tex_height[i] == 0) tex_height[i] = (tt.maskt != TX_NOMASK && tt.maskt < 31) ? (1u << tt.maskt) : 1u;

        const uint32_t tex_width1 = tex_width[i] << (cms & TX_MIRROR);
        const uint32_t tex_height1 = tex_height[i] << (cmt & TX_MIRROR);
        if ((cms & TX_CLAMP) && ((cms & TX_MIRROR) || tex_width1 != tex_width2[i])) {
            tm |= 1u << (2 * i);
            cms &= static_cast<uint8_t>(~TX_CLAMP);
        }
        if ((cmt & TX_CLAMP) && ((cmt & TX_MIRROR) || tex_height1 != tex_height2[i])) {
            tm |= 1u << (2 * i + 1);
            cmt &= static_cast<uint8_t>(~TX_CLAMP);
        }
        if (mRs.textures[i] < 0) continue;
        const bool linear_filter = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
        if (!mRs.sampler_valid[i] || linear_filter != mRs.sampler_linear[i] || cms != mRs.sampler_cms[i] ||
            cmt != mRs.sampler_cmt[i]) {
            Flush(FLUSH_SAMPLER);
            mBackend->SetSamplerParameters(i, linear_filter, cms, cmt);
            mRs.sampler_valid[i] = true;
            mRs.sampler_linear[i] = linear_filter;
            mRs.sampler_cms[i] = cms;
            mRs.sampler_cmt[i] = cmt;
        }
    }

    void* prg = comb->prg[tm];
    if (prg == nullptr) {
        comb->prg[tm] = prg = LookupOrCreateShader(comb->shader_id0, comb->shader_id1 | tm * SHOPT_TEXEL0_CLAMP_S);
    }
    if (prg != mRs.shader_program) {
        Flush(FLUSH_SHADER);
        mBackend->UnloadShader(mRs.shader_program);
        mBackend->LoadShader(prg);
        mRs.shader_program = prg;
    }
    if ((use_alpha ? 1u : 0u) != mRs.alpha_blend) {
        Flush(FLUSH_ALPHA);
        mBackend->SetUseAlpha(use_alpha);
        mRs.alpha_blend = use_alpha ? 1 : 0;
    }
    uint8_t numInputs;
    bool usedTextures[2];
    mBackend->ShaderGetInfo(prg, &numInputs, usedTextures);
    const ClipParams clip_parameters = mBackend->GetClipParameters();

    out.comb = comb;
    out.prg = prg;
    out.tm = tm;
    for (int i = 0; i < 2; i++) {
        out.tex_width[i] = tex_width[i];
        out.tex_height[i] = tex_height[i];
        out.tex_width2[i] = tex_width2[i];
        out.tex_height2[i] = tex_height2[i];
        out.effective_tile[i] = effective_tile[i];
        out.usedTextures[i] = usedTextures[i];
    }
    out.numInputs = numInputs;
    out.clip = clip_parameters;
    out.use_alpha = use_alpha;
    out.use_fog = use_fog;
    out.use_blend_color = use_blend_color;
    mTriMemo = out;
    mTriMemoValid = true;
    return true;
}

extern "C" int gdx_ninterp_cpuclip(void);

void Interp::SpTri(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    ProfScope prof(mBackend, 3);
    LoadedVertex* v1 = &mRsp.loaded_vertices[vtx1_idx];
    LoadedVertex* v2 = &mRsp.loaded_vertices[vtx2_idx];
    LoadedVertex* v3 = &mRsp.loaded_vertices[vtx3_idx];
    LoadedVertex* v_arr[3] = { v1, v2, v3 };
    if (!is_rect) mStats.tris++;

    /* Reject variants drop any triangle with a vertex outside the 2x box or behind the eye. */
    if (!is_rect && mClipFanDepth == 0 && (mVariant == 1 || mVariant == 2)) {
        for (const LoadedVertex* vertex : v_arr) {
            const float w2 = 2.0f * vertex->w;
            if (vertex->w <= 0.0f || vertex->x < -w2 || vertex->x > w2 || vertex->y < -w2 || vertex->y > w2 ||
                vertex->z < -vertex->w) {
                mStats.trisRejected++;
                /* COVERAGE: the reject-variant screen fired on this frame. */
                gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_REJECT));
                return;
            }
        }
    }

    /* CPU clipping: LUS default; the plan's no-clip mode is [native] cpuclip=0 (default 1 until
     * the M2 tunnel A/B proves the PICA path). */
    bool requiresClipping = false;
    if (!is_rect && mClipFanDepth == 0 && gdx_ninterp_cpuclip()) {
        requiresClipping = ((v1->clip_out | v2->clip_out | v3->clip_out) != 0);
    }
    if (requiresClipping) {
        /* COVERAGE: this frame ran the CPU clipper. `tunnel-silence-records` exists because
         * the clipper is where open failure #1 lives, and no SHOT schedule ever knew which
         * frames reach it. */
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_NEEDED));
        /* DROPSITE 3. Sutherland-Hodgman against 7 half-spaces, expression-for-expression the
         * LUS clipper (interpreter.cpp GfxSpTri1), 16-vertex cap included. Clipping a CONVEX
         * polygon by a half-space adds at most one vertex (two crossings cost at least one
         * vertex, and the result of each step is convex again), so from a triangle the output
         * is bounded by 3 + 7 = 10 and the cap has six slots of slack it can never use. The
         * cap is therefore counted, not raised: raising it would diverge from the reference
         * path this code exists to match, and clipPolyHw on the receipt is what proves the
         * bound empirically instead of by argument. */
        LoadedVertex polygonA[16] = { *v1, *v2, *v3 };
        LoadedVertex polygonB[16];
        memset(polygonB, 0, sizeof(polygonB));
        LoadedVertex* input = polygonA;
        LoadedVertex* output = polygonB;
        size_t inputCount = 3;
        /* [debug] clippolymax shrinks the cap below 16 so the truncation arm is reachable on
         * demand; the storage is unchanged, so nothing can overrun. 0 = shipped (16). */
        const size_t clipCap = mClipPolyMax;
        for (uint32_t plane = 0; plane < 7 && inputCount >= 3; ++plane) {
            size_t outputCount = 0;
            bool truncated = false;
            for (size_t i = 0; i < inputCount; ++i) {
                const LoadedVertex& current = input[i];
                const LoadedVertex& next = input[(i + 1) % inputCount];
                const float currentDistance = ClipDistance(current, plane);
                const float nextDistance = ClipDistance(next, plane);
                const bool currentInside = currentDistance >= 0.0f;
                const bool nextInside = nextDistance >= 0.0f;
                if (currentInside) {
                    if (outputCount < clipCap) {
                        output[outputCount++] = current;
                    } else {
                        truncated = true;
                    }
                }
                if (currentInside != nextInside && outputCount >= clipCap) truncated = true;
                if (currentInside != nextInside && outputCount < clipCap) {
                    const float denominator = currentDistance - nextDistance;
                    float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    output[outputCount++] = Interpolate(current, next, t);
                }
            }
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
            gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP, GDX3DS_COV_CLIP_ALLOUT));
            return;
        }
        /* COVERAGE: the fan's width is the feature — a 3-vertex fan is one triangle back, a
         * wider one is geometry the unclipped path would never have drawn. */
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_CLIP,
                                       GDX3DS_COV_CLIP_FAN | ((uint32_t)inputCount << 8)));
        LoadedVertex saved[3];
        for (size_t i = 0; i < 3; ++i) saved[i] = mRsp.loaded_vertices[kMaxVertices + i];
        mStats.tris--;
        ++mClipFanDepth;
        for (size_t i = 1; i + 1 < inputCount; ++i) {
            mRsp.loaded_vertices[kMaxVertices] = input[0];
            mRsp.loaded_vertices[kMaxVertices + 1] = input[i];
            mRsp.loaded_vertices[kMaxVertices + 2] = input[i + 1];
            for (size_t j = 0; j < 3; ++j) {
                LoadedVertex& cv = mRsp.loaded_vertices[kMaxVertices + j];
                cv.w = cv.w > kMinClipW ? cv.w : kMinClipW;
                UpdateClipFlags(cv);
            }
            SpTri(kMaxVertices, kMaxVertices + 1, kMaxVertices + 2, false);
        }
        --mClipFanDepth;
        for (size_t i = 0; i < 3; ++i) mRsp.loaded_vertices[kMaxVertices + i] = saved[i];
        return;
    }

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        if (!is_rect) mStats.trisRejected++;
        return;
    }

    if ((mRsp.geometry_mode & GM_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) cross = -cross;
        if ((mRsp.extra_geometry_mode & 0x00000001u) != 0) cross = -cross; /* G_EX_INVERT_CULLING */
        const uint32_t cull_type = mRsp.geometry_mode & GM_CULL_BOTH;
        if (cull_type == GM_CULL_FRONT) {
            if (cross <= 0) {
                if (!is_rect) mStats.trisCulled++;
                return;
            }
        } else if (cull_type == GM_CULL_BACK) {
            if (cross >= 0) {
                if (!is_rect) mStats.trisCulled++;
                return;
            }
        } else {
            if (!is_rect) mStats.trisCulled++;
            return;
        }
    }

    DrawState ds;
    if (!ResolveDrawState(is_rect, ds)) {
        return;
    }
    Combiner* comb = ds.comb;
    const uint32_t tm = ds.tm;
    const uint32_t(&tex_width)[2] = ds.tex_width;
    const uint32_t(&tex_height)[2] = ds.tex_height;
    const uint32_t(&tex_width2)[2] = ds.tex_width2;
    const uint32_t(&tex_height2)[2] = ds.tex_height2;
    const uint32_t(&effective_tile)[2] = ds.effective_tile;
    const bool(&usedTextures)[2] = ds.usedTextures;
    const uint8_t numInputs = ds.numInputs;
    const ClipParams clip_parameters = ds.clip;
    const bool use_alpha = ds.use_alpha;
    const bool use_fog = ds.use_fog;
    const bool use_blend_color = ds.use_blend_color;

    /* [nverify] diagnostic block: ~250-byte memset, ~35 field loads, two LoadedTexAt copies
     * and four FNV hashes per triangle. It was UNGATED on this path while the LUS twin has
     * always been guarded (interpreter.cpp:5134) — two agents found it independently.
     * The per-task latch means interp_verify=0 pays one branch (audit finding 31). */
    if (mVerifyActive) {
        Gdx3dsVerifyTri vt;
        memset(&vt, 0, sizeof(vt));
        const Tile& t0 = mRdp.texture_tile[effective_tile[0]];
        const LoadedTexture l0 = LoadedTexAt(t0.tmem_index);
        vt.tile0 = effective_tile[0]; vt.tile1 = effective_tile[1]; vt.firstTile = mRdp.first_tile_index; vt.isRect = is_rect;
        vt.uls0 = t0.uls; vt.ult0 = t0.ult; vt.lrs0 = t0.lrs; vt.lrt0 = t0.lrt;
        vt.texW0 = tex_width[0]; vt.texH0 = tex_height[0]; vt.texW20 = tex_width2[0]; vt.texH20 = tex_height2[0];
        vt.texW1 = tex_width[1]; vt.texH1 = tex_height[1]; vt.tm = tm; vt.vu = v1->u; vt.vv = v1->v;
        vt.shifts0 = t0.shifts; vt.shiftt0 = t0.shiftt; vt.line0 = l0.line_size_bytes; vt.size0 = l0.size_bytes;
        vt.full0 = l0.full_image_line_size_bytes; vt.orig0 = l0.orig_size_bytes;
        vt.scaleS = mRsp.texture_scaling_factor.s; vt.scaleT = mRsp.texture_scaling_factor.t;
        vt.omh = mRdp.other_mode_h; vt.oml = mRdp.other_mode_l; vt.geo = mRsp.geometry_mode; vt.variant = mVariant;
        vt.vpx = mRdp.viewport.x; vt.vpy = mRdp.viewport.y; vt.vpw = mRdp.viewport.width; vt.vph = mRdp.viewport.height;
        vt.lists = mStats.lists; vt.cmds = mStats.cmds; vt.branchZ = mStats.branchZTaken; vt.noop = mStats.noopLists;
        vt.blendA = mRdp.blend_color.a;
        {
            const uint32_t tmemOff = static_cast<uint32_t>(t0.tmem_index) * 8u;
            vt.used0 = comb->usedTextures[0] ? 1u : 0u; vt.used1 = comb->usedTextures[1] ? 1u : 0u;
            vt.tmem0 = t0.tmem_index;
            vt.rtLine0 = t0.line_size_bytes;
            if (t0.tmem_index < 512 && mRdp.tmem_span_base[t0.tmem_index] != 0) {
                vt.base0 = mRdp.tmem_span_base[t0.tmem_index] - 1u;
                const LoadedTexture& br = mRdp.loaded_texture[vt.base0];
                vt.baseAddr0 = reinterpret_cast<unsigned long>(br.addr); vt.baseSize0 = br.size_bytes; vt.baseOrig0 = br.orig_size_bytes;
            }
            /* fmt/siz/palette of the render tile + the live palette staging (see the LUS twin). */
            vt.tc1 = (static_cast<unsigned>(t0.fmt) << 8) | static_cast<unsigned>(t0.siz) |
                     (static_cast<unsigned>(t0.palette) << 16) | (l0.immutable ? 0x1000000u : 0u);
            /* CI only, and only the palette half the decode reads: this is per-triangle. */
            vt.tc0 = 0;
            if (t0.fmt == IM_FMT_CI && (t0.siz == IM_SIZ_4b || t0.siz == IM_SIZ_8b)) {
                uint32_t ph = 2166136261u;
                if (t0.siz == IM_SIZ_4b) {
                    const uint8_t* pal = mRdp.palette_staging[(t0.palette / 8) & 1] + (t0.palette % 8) * 32u;
                    for (unsigned bi = 0; bi < 32u; bi++) ph = (ph ^ pal[bi]) * 16777619u;
                } else {
                    for (unsigned hi = 0; hi < 2u; hi++) {
                        for (unsigned bi = 0; bi < 256u; bi++) ph = (ph ^ mRdp.palette_staging[hi][bi]) * 16777619u;
                    }
                }
                vt.tc0 = ph;
            }
            uint32_t tmemSpan0 = 0;
            if (tmemOff < sizeof(mRdp.tmem)) {
                const uint32_t remaining = static_cast<uint32_t>(sizeof(mRdp.tmem)) - tmemOff;
                tmemSpan0 = VfyTmemFootprint(l0.size_bytes, l0.line_size_bytes);
                if (tmemSpan0 < 8u) tmemSpan0 = 8u;
                if (tmemSpan0 > remaining) tmemSpan0 = remaining;
            }
            VfyTriSource(&vt.addr0, &vt.cls0, &vt.srcHash0, &vt.tmemHash0, vt.srcB0, vt.tmemB0,
                         vt.used0 ? l0.addr : nullptr, l0.size_bytes,
                         tmemSpan0 != 0 ? mRdp.tmem + tmemOff : nullptr, tmemSpan0);
            if (vt.used1) {
                const Tile& t1 = mRdp.texture_tile[effective_tile[1]];
                const LoadedTexture l1 = LoadedTexAt(t1.tmem_index);
                const uint32_t tmemOff1 = static_cast<uint32_t>(t1.tmem_index) * 8u;
                uint32_t tmemSpan1 = 0;
                if (tmemOff1 < sizeof(mRdp.tmem)) {
                    const uint32_t remaining1 = static_cast<uint32_t>(sizeof(mRdp.tmem)) - tmemOff1;
                    tmemSpan1 = l1.size_bytes > 8u ? l1.size_bytes : 8u;
                    if (tmemSpan1 > remaining1) tmemSpan1 = remaining1;
                }
                VfyTriSource(&vt.addr1, &vt.cls1, &vt.srcHash1, &vt.tmemHash1, nullptr, nullptr, l1.addr,
                             l1.size_bytes, tmemSpan1 != 0 ? mRdp.tmem + tmemOff1 : nullptr, tmemSpan1);
                vt.tmem1 = t1.tmem_index; vt.line1 = l1.line_size_bytes; vt.size1 = l1.size_bytes; vt.full1 = l1.full_image_line_size_bytes;
                vt.orig1 = l1.orig_size_bytes;
            }
        }
        mBackend->VerifyNoteTri(&vt);
    }

    /* ---- packed emission ([triloop] + [trifast]) ---- */
    if (mBufNumTris == 0) {
        /* GEOMLOSS: ask the backend how many triangles still fit rather than demanding room
         * for a whole kMaxTriBuffer batch. LUS answers a refusal by running its legacy
         * variable-stride loop, whose reservation is the size it actually needs — so the old
         * all-or-nothing begin made the owned path DROP triangles that interp=0 still draws,
         * for the last ~36 KiB of every frame's pool. Splitting the batch at the cap emits
         * exactly the same vertices in exactly the same order; only the draw count rises. */
        uint32_t capTris = 0;
        mPackDst = mBackend->VboPackBegin(&mPackParams, &capTris);
        mPackBatch = (mPackDst != nullptr);
        mPackComb = comb; /* the fold key for this batch (== LUS sGdxPackComb) */
        if (!mPackBatch) {
            /* Genuine exhaustion (or no shader / no frame / [debug] triloop=0): nothing can be
             * emitted here and the LUS path would drop it too. The backend logs the reason
             * once; packRefused on the [ninterp] line is the recurring signal. */
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

    struct PackInput {
        Rgba c;
        bool isShade;
    };
    const auto gdxInput = [&](int j, int k, int i) -> PackInput {
        Rgba tmp = { 0, 0, 0, 0 };
        bool isShade = false;
        switch (comb->shader_input_mapping[k][j]) {
            case CCMUX_PRIMITIVE: tmp = mRdp.prim_color; break;
            case CCMUX_SHADE: tmp = v_arr[i]->color; isShade = true; break;
            case CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = v_arr[i]->color.a; break;
            case CCMUX_ENVIRONMENT: tmp = mRdp.env_color; break;
            case CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = mRdp.prim_color.a; break;
            case CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = mRdp.env_color.a; break;
            case CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = mRdp.prim_lod_fraction; break;
            case CCMUX_LOD_FRACTION: {
                if (mRdp.other_mode_l & (1u << MDSFT_TEXTLOD)) {
                    float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                    if (distance_frac < 0.0f) distance_frac = 0.0f;
                    if (distance_frac > 1.0f) distance_frac = 1.0f;
                    tmp.r = tmp.g = tmp.b = tmp.a = static_cast<uint8_t>(distance_frac * 255.0f);
                } else {
                    tmp.r = tmp.g = tmp.b = tmp.a = 255;
                }
                break;
            }
            case CCMUX_KEY_CENTER: tmp = mRdp.key_center; break;
            case CCMUX_KEY_SCALE: tmp = mRdp.key_scale; break;
            case CCMUX_CONVERT_K4: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(mRdp.convert_k[4]); break;
            case CCMUX_CONVERT_K5: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(mRdp.convert_k[5]); break;
            default: break;
        }
        /* ACMUX_PRIM_LOD_FRAC (6) aliases CCMUX_1/CENTER in the alpha mapping: LUS's switch hits
           the G_ACMUX_PRIM_LOD_FRAC arm (tmp.a = prim_lod_fraction) because 6 has no CCMUX case
           listed before it. Reproduce: */
        if (k == 1 && comb->shader_input_mapping[k][j] == ACMUX_PRIM_LOD_FRAC) {
            tmp = Rgba{ 0, 0, 0, mRdp.prim_lod_fraction };
            isShade = false;
        }
        return PackInput{ tmp, isShade };
    };

    float clampU[2] = { 1e30f, 1e30f };
    float clampV[2] = { 1e30f, 1e30f };
    for (int t = 0; t < 2; t++) {
        if (!usedTextures[t]) continue;
        clampU[t] = mPackParams.wrapClampU[t];
        clampV[t] = mPackParams.wrapClampV[t];
        if (mPackParams.shaderClampGate) {
            if (tm & (1u << (2 * t))) clampU[t] = ((tex_width2[t] - 0.5f) / tex_width[t]) * mPackParams.uScale[t];
            if (tm & (1u << (2 * t + 1))) clampV[t] = ((tex_height2[t] - 0.5f) / tex_height[t]) * mPackParams.vScale[t];
        }
    }

    if (mBufNumTris == 0) {
        mPackAux.numInputs = static_cast<uint8_t>(numInputs < 7 ? numInputs : 7);
        for (int j = 0; j < mPackAux.numInputs; j++) {
            const PackInput cv = gdxInput(j, 0, 0);
            mPackAux.inputRgba[j][0] = cv.c.r / 255.0f;
            mPackAux.inputRgba[j][1] = cv.c.g / 255.0f;
            mPackAux.inputRgba[j][2] = cv.c.b / 255.0f;
            if (use_alpha) {
                const PackInput av = gdxInput(j, 1, 0);
                mPackAux.inputRgba[j][3] = (use_fog && !use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
            } else {
                mPackAux.inputRgba[j][3] = 1.0f;
            }
        }
        mPackAux.hasFog = use_fog ? 1 : 0;
        if (use_fog) {
            const Rgba& fogSrc = use_blend_color ? mRdp.blend_color : mRdp.fog_color;
            mPackAux.fogRgb[0] = fogSrc.r / 255.0f;
            mPackAux.fogRgb[1] = fogSrc.g / 255.0f;
            mPackAux.fogRgb[2] = fogSrc.b / 255.0f;
            mPackAux.fogFactor0 = use_blend_color ? mRdp.fog_color.a / 255.0f : v_arr[0]->color.a / 255.0f;
        } else {
            mPackAux.fogRgb[0] = mPackAux.fogRgb[1] = mPackAux.fogRgb[2] = 0.0f;
            mPackAux.fogFactor0 = 0.0f;
        }
    }

    float* gdxOut = mPackDst + mBufNumTris * (3 * GDX3DS_VBOPACK_STRIDE);
    const bool bilerp = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
    const bool halfTexel = bilerp && !is_rect; /* GDX_RECT_HALF_TEXEL default off */
    struct UvPrep {
        float uOff, vOff, uPre, vPre, uNormMul, vNormMul, uNormDiv, vNormDiv;
        bool uMul, vMul;
    } prep[2];
    for (int t = 0; t < 2; t++) {
        if (!usedTextures[t]) continue;
        const Tile& ut = mRdp.texture_tile[effective_tile[t]];
        const int shifts = ut.shifts, shiftt = ut.shiftt;
        prep[t].uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / static_cast<float>(1 << shifts) : static_cast<float>(1 << (16 - shifts)));
        prep[t].vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / static_cast<float>(1 << shiftt) : static_cast<float>(1 << (16 - shiftt)));
        prep[t].uOff = ut.uls / 4.0f;
        prep[t].vOff = ut.ult / 4.0f;
        const uint32_t tw = tex_width[t], th = tex_height[t];
        prep[t].uMul = (tw & (tw - 1u)) == 0u;
        prep[t].vMul = (th & (th - 1u)) == 0u;
        prep[t].uNormDiv = static_cast<float>(tw);
        prep[t].vNormDiv = static_cast<float>(th);
        prep[t].uNormMul = prep[t].uMul ? 1.0f / static_cast<float>(tw) : 1.0f;
        prep[t].vNormMul = prep[t].vMul ? 1.0f / static_cast<float>(th) : 1.0f;
    }
    const int ci = mPackParams.vtxColorInput - 1;
    const bool colorIsShade = ci >= 0 && comb->shader_input_mapping[0][ci] == CCMUX_SHADE;
    const int ai = mPackParams.vtxAlphaInput - 1;
    const bool alphaIsShade = ai >= 0 && comb->shader_input_mapping[1][ai] == CCMUX_SHADE;
    const bool fogFactorConst = mPackParams.fogFactorToAlpha && use_blend_color;
    const float fogFactorConstVal = fogFactorConst ? mByteToUnit[mRdp.fog_color.a] : 0.0f;
    const bool shadeAlphaIsOne = use_fog && !use_blend_color;
    for (int i = 0; i < 3; i++) {
        float* out = gdxOut + i * GDX3DS_VBOPACK_STRIDE;
        const LoadedVertex* vv = v_arr[i];
        float z = vv->z, w = vv->w;
        if (clip_parameters.zIsFrom0To1) z = (z + w) / 2.0f;
        out[0] = vv->x;
        out[1] = clip_parameters.invertY ? -vv->y : vv->y;
        out[2] = z;
        out[3] = w;
        for (int t = 0; t < 2; t++) {
            const int slot = (t == 0) ? 4 : 10;
            if (!usedTextures[t]) {
                out[slot] = 0.0f;
                out[slot + 1] = 0.0f;
                continue;
            }
            float u = vv->u / 32.0f;
            float v = vv->v / 32.0f;
            u *= prep[t].uPre;
            v *= prep[t].vPre;
            u -= prep[t].uOff;
            v -= prep[t].vOff;
            if (halfTexel) {
                u += 0.5f;
                v += 0.5f;
            }
            const float nu = prep[t].uMul ? u * prep[t].uNormMul : u / prep[t].uNormDiv;
            const float nv = prep[t].vMul ? v * prep[t].vNormMul : v / prep[t].vNormDiv;
            const float pu = FminExact(nu * mPackParams.uScale[t], clampU[t]);
            const float pv = FminExact(nv * mPackParams.vScale[t], clampV[t]);
            if (mPackParams.hasOff) {
                out[slot] = pu + mPackParams.uOff[t];
                out[slot + 1] = pv + mPackParams.vOff[t];
            } else {
                out[slot] = pu;
                out[slot + 1] = pv;
            }
        }
        if (ci >= 0) {
            if (colorIsShade) {
                out[6] = mByteToUnit[vv->color.r];
                out[7] = mByteToUnit[vv->color.g];
                out[8] = mByteToUnit[vv->color.b];
            } else {
                const PackInput cv = gdxInput(ci, 0, i);
                out[6] = mByteToUnit[cv.c.r];
                out[7] = mByteToUnit[cv.c.g];
                out[8] = mByteToUnit[cv.c.b];
            }
        } else {
            out[6] = out[7] = out[8] = 1.0f;
        }
        if (mPackParams.fogFactorToAlpha) {
            out[9] = fogFactorConst ? fogFactorConstVal : mByteToUnit[vv->color.a];
        } else if (mPackParams.wantVtxAlpha) {
            if (alphaIsShade) {
                out[9] = shadeAlphaIsOne ? 1.0f : mByteToUnit[vv->color.a];
            } else {
                const PackInput av = gdxInput(ai, 1, i);
                out[9] = (shadeAlphaIsOne && av.isShade) ? 1.0f : mByteToUnit[av.c.a];
            }
        } else {
            out[9] = 1.0f;
        }
    }
    if (use_fog) {
        if (use_blend_color) {
            mBackend->FogNoteParams(0, 0, 0);
        } else {
            mBackend->FogNoteParams(mRsp.fog_mul, mRsp.fog_offset, 1);
        }
    }
    if (!is_rect) mStats.trisEmitted++;
    /* GEOMLOSS: >= not ==, and against the pool-derived cap: under pool pressure the cap is
     * below kMaxTriBuffer and the batch is flushed early instead of overrunning the pool. */
    if (++mBufNumTris >= mPackCapTris) {
        Flush(FLUSH_FULL);
    }
}

} // namespace gdxn
