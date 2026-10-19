/* port/3ds/gfx/n64/gdx_nrr_sky.cpp -- [native] nrr_sky: the sky/background as an NRR category
 * (docs/research/native/n60-design.md §4.4, n60sky-progress.md). Render side.
 *
 * The game side (port/3ds/game/gdx3ds_nrr_sky_game.c) consumes Race_Draw's SKY section in the 1P
 * race and writes, in place of Background_Draw's ~170 commands:
 *
 *   LEARN regions (G_NOOP p=0x64, cold: a venue's first frame, or a block gone stale). The stock
 *     sky's STATE commands in stock order with a dummy triangle at each draw point. The interpreter
 *     resolves that triangle exactly as it resolves the stock one (combiner, shader ids, texture
 *     cache import, sampler, blend, packed-batch parameters, vertex-0 record); LEARN_END banks
 *     what it bound -- as an NRR state key (core NrrStateGet), the vertex packing as a linear map
 *     probed from the interpreter's own packer, the combiner inputs -- and discards the batch.
 *
 *   NRR markers (core p=0x63, cat SKY): view slot 0 = part A (gradient, day sprites) or the whole
 *     sky, slot 1 = part B (floor, clouds) when stock commands (stars, night sprites) sit between.
 *     NrrSkyHotDraw: transform the GfxPool's own vertices (Background_Update wrote them in logic)
 *     with the MP the stock head just left in the walker's RSP (the stock T&L's matrix and
 *     expressions), drop triangles outside one frustum plane, pack them, one core state block +
 *     ring + NrrDraw2D per block. The PICA clips (performance mode). Nothing walks display-list
 *     words, touches TMEM or hashes a texture key on this path.
 *
 * The one non-linear input: the floor's combiner reads shade as a vertex-0 constant in the packed
 * layout, and the stock batch's vertex 0 is the first vertex the CPU clipper emits; a colour-only
 * replica of the 7-plane clip of the first triangle picks it -- on the game side, in the snapshot
 * (NrrSkySnapFloorShade, gdx3ds_nrr_sky_game.c), with the sprite matrices.
 *
 * Exactness: none claimed (n60-design §7.3 tolerance). */
#include <stdio.h>
#include <string.h>

#include <new>

#include "gdx_interp.h"
#include "gdx_log.h"
#include "gdx3ds_hotcold.h"
#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_sky.h"
#include "nrr/gdx3ds_nrr_internal.h" /* n60retry: gdx3ds_nrr_bk_tex_serial */
#include "nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" long long gdx_ninterp_now(void);


/* Render side -> game side (gfx/nrr/gdx3ds_nrr_sky.h). Defined here: this TU links into every
 * build that walks the markers (and the host interpreter test). */
extern "C" {
volatile int gdx3ds_nrr_sky_relearn = 0;
volatile int gdx3ds_nrr_sky_broken = 0;
volatile float gdx3ds_nrr_sky_xs = 1.0f;
}

namespace gdxn {

namespace nrrsky {

constexpr size_t kStride = GDX3DS_VBOPACK_STRIDE;

/* Background_Draw's triangles, in its order (gSP2Triangles pairs). */
const uint8_t kGradTris[2][3] = { { 8, 11, 9 }, { 8, 10, 11 } };
const uint8_t kFloorTris[4][3] = { { 0, 3, 1 }, { 0, 2, 3 }, { 4, 7, 5 }, { 4, 6, 7 } };
const uint8_t kCloudTris[4][3] = { { 12, 15, 13 }, { 12, 14, 15 }, { 16, 19, 17 }, { 16, 18, 19 } };
const uint8_t kSpriteTris[2][3] = { { 0, 3, 1 }, { 0, 2, 3 } };

/* One draw's vertex packing, linear in the stock inputs (probed from HudPackVerts, the packer
 * SpTri's emission shares): uv = min(U * mul + add, clamp) + off per axis with
 * U = (short)(tc * scale >> 16); colour channels either constants or the vertex's own bytes. */
struct SkyPack {
    float uMul, uAdd, vMul, vAdd;
    float uClamp, vClamp, uOff, vOff;
    float rgb[3], a;
    uint16_t scaleS, scaleT;
    uint8_t rgbVtx, aVtx;
    uint8_t invertY, z01;
};

/* One learned state block. */
struct SkyBlock {
    uint32_t gen; /* learn serial (0 = empty); valid only for frames carrying it */
    uint8_t usable;
    uint8_t shadeRgb, shadeA; /* combiner inputs taken from vertex 0's shade (bits) */
    uint8_t nInputs;
    int16_t slot; /* the interpreter's texture-cache entry, its id and source */
    uint32_t texId;
    const uint8_t* texAddr;
    uint32_t texHash;   /* n60retry: the entry's content hash at the learn (an in-place re-import moves it) */
    uint32_t texSerial; /* n60retry: the backend id's content serial at the learn (a re-upload moves it) */
    NrrStateKey key;
    const NrrState* st;
    uint32_t stGen;
    float inputs[7][4];
    SkyPack pack;
};

/* Render-side state (one walker thread: NRR refuses the render thread). */
struct SkyRender {
    SkyBlock blocks[GDX_NRRSKY_K_COUNT];
    /* n60retry: each block's own core-state memory (NrrStateBytes, on first need) for when the
     * core's shared table is full (the post-race HUD screens fill it): a block without a state is
     * a frame without that part of the sky (the draw has no stock fallback). */
    void* stMem[GDX_NRRSKY_K_COUNT] = {};
    Interp* interp = nullptr; /* the walker that learned the blocks */
    int learnKind = -1;
    float mp[4][4]; /* the head's MP of the frame being drawn */
    float xs = 1.0f;
    int16_t scissor[4] = { 0, 0, 0, 0 };
    /* receipt window */
    uint32_t wFrames = 0, wDraws = 0, wTris = 0, wRej = 0, wLearn = 0, wLearnFail = 0, wStale = 0, wFallback = 0;
    uint32_t learnLogs = 0;
    /* stars: the learn rectangle A's six packed vertices, the screen -> clip slope per 1/4 px, and
     * whether the prim colour rides the vertex colour (else: one draw per star, prim as input) */
    float starTpl[6][kStride];
    float starAx = 0.0f, starAy = 0.0f;
    uint8_t starFoldRgb = 0, starFoldA = 0, starPrimRgb = 0, starPrimA = 0;
    /* [native] nrr_sky_clock=1 (diagnostic, inflates): svc ticks of the core calls per draw --
     * [0] NrrStateApply, [1] NrrSetInputs, [2] NrrDraw2D, [3] an empty clock pair (the read cost) */
    int clock = 0;
    uint64_t wCore[4] = { 0, 0, 0, 0 };
};

SkyRender* sSky = nullptr;
float sByteToUnit[256];

GDX_COLD bool SkyEnsure() {
    if (sSky == nullptr) {
        sSky = new (std::nothrow) SkyRender();
        NRR_CRUMB_FIRST(NRR_CRUMB_SKYLEARN, 16, "sky alloc SkyRender=%p bytes=%lu", (void*)sSky,
                        (unsigned long)sizeof(SkyRender));
        if (sSky != nullptr) {
            memset(sSky->blocks, 0, sizeof(sSky->blocks));
            if (&gdx3ds_config_get_int != nullptr) {
                sSky->clock = gdx3ds_config_get_int("native", "nrr_sky_clock", 0);
            }
        }
    }
    return sSky != nullptr;
}

/* The block's own memory, built for its key (the shared table is full). The hand-off forgets its
 * last applied block: a rebuild in place may sit at the address it last applied. */
GDX_COLD const NrrState* SkyOwnState(NrrCtx* c, SkyBlock& b) {
    const size_t kind = static_cast<size_t>(&b - sSky->blocks);
    void*& mem = sSky->stMem[kind];
    if (mem == nullptr) {
        mem = ::operator new(NrrStateBytes(), std::nothrow);
    }
    c->last = nullptr;
    return mem != nullptr ? NrrStateBuildInto(&b.key, mem) : nullptr;
}

/* The block's core state for the frame's scissor (a new scissor or a core flush: cold rebuild). */
GDX_COLD const NrrState* SkyStateFor(NrrCtx* c, SkyBlock& b, const int16_t scissor[4]) {
    memcpy(b.key.scissor, scissor, sizeof(b.key.scissor));
    b.st = NrrStateGet(&b.key);
    if (b.st == nullptr) {
        b.st = SkyOwnState(c, b);
    }
    b.stGen = NrrStateGeneration();
    return b.st;
}

} // namespace nrrsky

using namespace nrrsky;

/* ---- learning (cold): Interp members, they read the resolve's results --------------------- */

void Interp::NrrSkyMarker(uint32_t w0, uint32_t w1) {
    if (!SkyEnsure()) {
        return;
    }
    const uint32_t op = (w0 >> 8) & 0xFFu;
    if (op == GDX_NRRSKY_OP_LEARN_BEGIN) {
        NrrSkyLearnBegin(w1 & 0xFFu);
    } else if (op == GDX_NRRSKY_OP_LEARN_END) {
        NrrSkyLearnEnd(w1 & 0xFFu, w1 >> 8);
    }
}

GDX_COLD void Interp::NrrSkyLearnBegin(uint32_t kind) {
    NRR_CRUMB_FIRST(NRR_CRUMB_SKYLEARN, 16, "sky learn begin kind=%lu", (unsigned long)kind);
    Flush(FLUSH_HOOK); /* whatever is open is real geometry: draw it; the dummy opens its own batch */
    sSky->learnKind = kind < GDX_NRRSKY_K_COUNT ? static_cast<int>(kind) : -1;
}

/* Probe the packer with synthetic vertices: the UV map's slope and offset per axis, the colour
 * channels' sources; then the core state key. False for a state the hot path does not model. */
GDX_COLD bool Interp::NrrSkyProbe(void* blockPtr) {
    SkyBlock& b = *static_cast<SkyBlock*>(blockPtr);
    const DrawState& ds = mTriMemo;
    if (ds.usedTextures[1] || !ds.usedTextures[0] || ds.use_fog || mPackParams.fogFactorToAlpha) {
        return false;
    }
    if ((mRsp.geometry_mode & (GM_CULL_BOTH | GM_FOG | GM_LIGHTING | GM_TEXTURE_GEN)) != 0) {
        return false;
    }
    LoadedVertex p[3];
    memset(p, 0, sizeof(p));
    for (int i = 0; i < 3; i++) {
        p[i].w = 1.0f;
    }
    p[1].u = 1024.0f;
    p[1].v = 1024.0f;
    p[1].color = Rgba{ 255, 255, 255, 255 };
    const LoadedVertex* const pv[3] = { &p[0], &p[1], &p[2] };
    float o[3 * kStride];
    const bool bilerp = (mRdp.other_mode_h & (3u << MDSFT_TEXTFILT)) == TF_BILERP;
    HudPackVerts(ds, pv, 3, &p[0], o, bilerp);
    SkyPack& k = b.pack;
    k.uClamp = mPackParams.wrapClampU[0]; /* HudPackVerts' clamp expressions for unit 0 */
    k.vClamp = mPackParams.wrapClampV[0];
    if (mPackParams.shaderClampGate) {
        if (ds.tm & 1u) {
            k.uClamp = ((ds.tex_width2[0] - 0.5f) / ds.tex_width[0]) * mPackParams.uScale[0];
        }
        if (ds.tm & 2u) {
            k.vClamp = ((ds.tex_height2[0] - 0.5f) / ds.tex_height[0]) * mPackParams.vScale[0];
        }
    }
    k.uOff = mPackParams.hasOff ? mPackParams.uOff[0] : 0.0f;
    k.vOff = mPackParams.hasOff ? mPackParams.vOff[0] : 0.0f;
    k.uAdd = o[4] - k.uOff;
    k.vAdd = o[5] - k.vOff;
    k.uMul = (o[kStride + 4] - o[4]) / 1024.0f;
    k.vMul = (o[kStride + 5] - o[5]) / 1024.0f;
    k.rgbVtx = o[kStride + 6] != o[6] ? 1 : 0;
    k.aVtx = o[kStride + 9] != o[9] ? 1 : 0;
    k.rgb[0] = o[6];
    k.rgb[1] = o[7];
    k.rgb[2] = o[8];
    k.a = o[9];
    k.scaleS = mRsp.texture_scaling_factor.s;
    k.scaleT = mRsp.texture_scaling_factor.t;
    k.invertY = ds.clip.invertY ? 1 : 0;
    k.z01 = ds.clip.zIsFrom0To1 ? 1 : 0;
    /* The combiner inputs (the vertex-0 record) and the ones that are shade. */
    b.nInputs = mPackAux.numInputs < 7 ? mPackAux.numInputs : 7;
    memcpy(b.inputs, mPackAux.inputRgba, sizeof(b.inputs));
    b.shadeRgb = 0;
    b.shadeA = 0;
    for (int j = 0; j < b.nInputs; j++) {
        if (ds.comb->shader_input_mapping[0][j] == CCMUX_SHADE && j + 1 != mPackParams.vtxColorInput) {
            b.shadeRgb |= static_cast<uint8_t>(1u << j);
        }
        if (ds.use_alpha && ds.comb->shader_input_mapping[1][j] == ACMUX_SHADE && j + 1 != mPackParams.vtxAlphaInput) {
            b.shadeA |= static_cast<uint8_t>(1u << j);
        }
    }
    /* The core key: what ResolveDrawState bound, in the core's terms. */
    NrrStateKey& key = b.key;
    memset(&key, 0, sizeof(key));
    key.shaderId0 = ds.comb->shader_id0;
    key.shaderId1 = ds.comb->shader_id1 | static_cast<uint64_t>(ds.tm) * SHOPT_TEXEL0_CLAMP_S;
    key.tex[0] = b.texId;
    key.tex[1] = NRR_TEX_NONE;
    key.texLinear[0] = mRs.sampler_linear[0] ? 1 : 0;
    key.texCms[0] = mRs.sampler_cms[0];
    key.texCmt[0] = mRs.sampler_cmt[0];
    key.program = NRR_PROG_2D;
    key.depthTest = (mRs.depth_test_and_mask & 1u) != 0 ? 1 : 0;
    key.depthWrite = (mRs.depth_test_and_mask & 2u) != 0 ? 1 : 0;
    key.decal = mRs.decal_mode == 1 ? 1 : 0;
    key.blend = mRs.alpha_blend == 1 ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    key.cull = NRR_CULL_NONE;
    key.alphaTest = NRR_AT_OFF; /* the translated shader's texEdge / alphaThreshold decide it */
    key.alphaRef = mRdp.blend_color.a;
    return true;
}

/* The two learn rectangles (A at the learn corner, B offset by DX, DY quarter pixels) just packed:
 * A's vertices are the template, B - A the slope; the prim colour's route from the combiner. */
GDX_COLD bool Interp::NrrSkyLearnStars() {
    SkyRender& S = *sSky;
    const float* a = mPackDst;
    const float* bv = mPackDst + 6 * kStride;
    memcpy(S.starTpl, a, sizeof(S.starTpl));
    S.starAx = (bv[0] - a[0]) / static_cast<float>(GDX_NRRSKY_STAR_LEARN_DX);
    S.starAy = (bv[1] - a[1]) / static_cast<float>(GDX_NRRSKY_STAR_LEARN_DY);
    const Combiner* comb = mTriMemo.comb;
    const int vci = mPackParams.vtxColorInput;
    const int vai = mPackParams.vtxAlphaInput;
    S.starFoldRgb = (vci > 0 && comb->shader_input_mapping[0][vci - 1] == CCMUX_PRIMITIVE) ? 1 : 0;
    S.starFoldA = (vai > 0 && comb->shader_input_mapping[1][vai - 1] == ACMUX_PRIMITIVE) ? 1 : 0;
    S.starPrimRgb = 0;
    S.starPrimA = 0;
    for (int j = 0; j < 7; j++) {
        if (comb->shader_input_mapping[0][j] == CCMUX_PRIMITIVE && j + 1 != vci) {
            S.starPrimRgb |= static_cast<uint8_t>(1u << j);
        }
        if (comb->shader_input_mapping[1][j] == ACMUX_PRIMITIVE && j + 1 != vai) {
            S.starPrimA |= static_cast<uint8_t>(1u << j);
        }
    }
    GDXN_LOG("[nrrsky] learn stars ax=%.6g ay=%.6g fold rgb=%u a=%u primIn=%x/%x tpl0=%.4f,%.4f,%.4f,%.4f uv=%.4f,%.4f",
             S.starAx, S.starAy, S.starFoldRgb, S.starFoldA, S.starPrimRgb, S.starPrimA, a[0], a[1], a[2], a[3], a[4],
             a[5]);
    return S.starAx != 0.0f && S.starAy != 0.0f;
}

GDX_COLD void Interp::NrrSkyLearnEnd(uint32_t kind, uint32_t gen) {
    SkyRender& S = *sSky;
    const bool open = S.learnKind == static_cast<int>(kind) && kind < GDX_NRRSKY_K_COUNT;
    S.learnKind = -1;
    if (!open) {
        return;
    }
    S.interp = this;
    SkyBlock& b = S.blocks[kind];
    memset(&b, 0, sizeof(b));
    const bool resolved = mBufNumTris > 0 && mPackBatch && mTriMemoValid && mRs.textures[0] >= 0;
    if (resolved) {
        b.slot = mRs.textures[0];
        b.texId = mTex[b.slot].texture_id;
        b.texAddr = mTex[b.slot].key.addr;
        b.texHash = mTex[b.slot].key.content_hash;
        b.texSerial = gdx3ds_nrr_bk_tex_serial(b.texId);
        b.usable = NrrSkyProbe(&b) ? 1 : 0;
        if (kind == GDX_NRRSKY_K_STAR) {
            b.usable = (b.usable && mBufNumTris == 4 && NrrSkyLearnStars()) ? 1 : 0;
        }
        b.gen = gen;
    }
    S.wLearn++;
    NRR_CRUMB_FIRST(NRR_CRUMB_SKYLEARN, 16, "sky learn end kind=%lu gen=%lu resolved=%d usable=%d tex=%d/%lu addr=%p",
                    (unsigned long)kind, (unsigned long)gen, resolved ? 1 : 0, (int)b.usable, (int)b.slot,
                    (unsigned long)b.texId, (const void*)b.texAddr);
    if (!resolved || !b.usable) {
        S.wLearnFail++;
        if (!resolved) {
            gdx3ds_nrr_sky_relearn = 1;
        } else {
            gdx3ds_nrr_sky_broken = 1; /* a state the hot path does not model: the stub goes stock */
        }
    }
    if (S.learnLogs < 48) {
        S.learnLogs++;
        const DrawState& ds = mTriMemo;
        GDXN_LOG("[nrrsky] learn kind=%u gen=%u ok=%d usable=%d cc=%08x%08x oml=%08x omh=%08x geo=%08x "
                 "tex=%d/%u %ux%u tm=%u alpha=%d dz=%u | uv %.6g*U+%.6g clamp %.6g off %.6g | %.6g*V+%.6g "
                 "clamp %.6g off %.6g | rgb %s %.3f,%.3f,%.3f a %s %.3f | vci=%d vai=%d shade=%x/%x in=%u "
                 "scale=%04x/%04x",
                 kind, gen, resolved ? 1 : 0, b.usable, static_cast<unsigned>(mRdp.combine_mode >> 32),
                 static_cast<unsigned>(mRdp.combine_mode), mRdp.other_mode_l, mRdp.other_mode_h, mRsp.geometry_mode,
                 b.slot, b.texId, ds.tex_width[0], ds.tex_height[0], ds.tm, ds.use_alpha ? 1 : 0,
                 mRs.depth_test_and_mask, b.pack.uMul, b.pack.uAdd, b.pack.uClamp, b.pack.uOff, b.pack.vMul,
                 b.pack.vAdd, b.pack.vClamp, b.pack.vOff, b.pack.rgbVtx ? "vtx" : "const", b.pack.rgb[0],
                 b.pack.rgb[1], b.pack.rgb[2], b.pack.aVtx ? "vtx" : "const", b.pack.a, mPackParams.vtxColorInput,
                 mPackParams.vtxAlphaInput, b.shadeRgb, b.shadeA, b.nInputs, b.pack.scaleS, b.pack.scaleT);
    }
    /* Discard the dummy batch: Flush's bookkeeping without the draw (VboPackBegin commits nothing). */
    mPackBatch = false;
    mPackDst = nullptr;
    mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
    mBufNumTris = 0;
    mPackComb = nullptr;
    mPackFolded = false;
    mTriMemoValid = false;
}

/* ---- hot: Interp members the draw reads through ----------------------------------------- */

/* The block's texture-cache entry still holds what was learned (the id is recycled on retire);
 * refreshed in the LRU like a cache hit. */
bool Interp::NrrSkyHotTex(int16_t slot, uint32_t id, const uint8_t* addr, uint32_t contentHash) {
    const TexEntry& e = mTex[slot];
    if (!e.used || !e.uploaded || e.texture_id != id || e.key.addr != addr || e.key.content_hash != contentHash) {
        return false;
    }
    TexCacheTouch(slot);
    return true;
}

/* The head's MP (Background_Draw's head was walked right before the marker), TransformVertices'
 * aspect branch as one factor (x * (4/3) / aspect there: within an ulp), and the scissor the
 * stock draw would bind. */
void Interp::NrrSkyHotHead(float mp[4][4], float* xs, int16_t scissor[4]) const {
    memcpy(mp, mRsp.MP_matrix, sizeof(mRsp.MP_matrix));
    float s = 1.0f;
    const float aspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
    if ((mRsp.extra_geometry_mode & 0x00000004u) != 0 && aspect > (4.0f / 3.0f) && mCfg.widescreen &&
        !mCfg.forceFixedAspect) {
        s = 160.0f / (160.0f - 12.0f);
    } else if (mCfg.widescreen && !mCfg.forceFixedAspect) {
        s = (4.0f / 3.0f) / aspect;
    }
    *xs = s;
    scissor[0] = static_cast<int16_t>(mRdp.scissor.x);
    scissor[1] = static_cast<int16_t>(mRdp.scissor.y);
    scissor[2] = static_cast<int16_t>(mRdp.scissor.width);
    scissor[3] = static_cast<int16_t>(mRdp.scissor.height);
}

void Interp::NrrSkyHotProj(float out[4][4]) const {
    memcpy(out, mRsp.P_matrix, sizeof(mRsp.P_matrix));
}

namespace nrrsky {

/* Transform the referenced vertices (TransformVertices' expressions; x times the aspect factor)
 * and their frustum outcodes. */
__attribute__((noinline)) void NrrSkyHotXform(const float (*mp)[4], float xs, const Vtx* v, uint32_t lo,
                                              uint32_t hi, float (*clip)[4], uint32_t* code) {
    for (uint32_t i = lo; i <= hi; i++) {
        const Vtx& s = v[i];
        float* q = clip[i - lo];
        for (int k = 0; k < 4; k++) {
            q[k] = s.ob[0] * mp[0][k] + s.ob[1] * mp[1][k] + s.ob[2] * mp[2][k] + mp[3][k];
        }
        q[0] *= xs;
        code[i - lo] = (q[0] < -q[3] ? 1u : 0u) | (q[0] > q[3] ? 2u : 0u) | (q[1] < -q[3] ? 4u : 0u) |
                       (q[1] > q[3] ? 8u : 0u) | (q[3] <= 0.0f ? 16u : 0u);
    }
}

/* One packed vertex (HudPackVerts' layout) through the block's linear map. */
__attribute__((noinline)) void NrrSkyHotPack(const SkyPack& k, const float* c, const Vtx& s, float* o) {
    o[0] = c[0];
    o[1] = k.invertY ? -c[1] : c[1];
    o[2] = k.z01 ? (c[2] + c[3]) / 2.0f : c[2];
    o[3] = c[3];
    const short U = static_cast<short>(s.tc[0] * k.scaleS >> 16);
    const short V = static_cast<short>(s.tc[1] * k.scaleT >> 16);
    const float u = U * k.uMul + k.uAdd;
    const float w = V * k.vMul + k.vAdd;
    o[4] = (u < k.uClamp ? u : k.uClamp) + k.uOff; /* FminExact: no NaN reaches here (finite inputs) */
    o[5] = (w < k.vClamp ? w : k.vClamp) + k.vOff;
    for (int i = 0; i < 3; i++) {
        o[6 + i] = k.rgbVtx ? sByteToUnit[s.cn[i]] : k.rgb[i];
    }
    o[9] = k.aVtx ? sByteToUnit[s.cn[3]] : k.a;
    o[10] = 0.0f;
    o[11] = 0.0f;
}

/* n60retry receipt: the first stale textures (which check moved). */
GDX_COLD void NrrSkyNoteStaleTex(const SkyBlock& b) {
    static unsigned sLogs = 0;
    if (sLogs >= 16) {
        return;
    }
    sLogs++;
    GDXN_LOG("[nrrsky] stale tex kind=%d slot=%d id=%lu serial=%lu/%lu", static_cast<int>(&b - sSky->blocks),
             static_cast<int>(b.slot), static_cast<unsigned long>(b.texId), static_cast<unsigned long>(b.texSerial),
             static_cast<unsigned long>(gdx3ds_nrr_bk_tex_serial(b.texId)));
}

/* The block's core state for this frame: its texture-cache entry still valid (else relearn), the
 * core block current for the frame's scissor. Null = not drawable this frame (counted). */
const NrrState* NrrSkyHotState(NrrCtx* c, SkyBlock& b) {
    SkyRender& S = *sSky;
    if (!S.interp->NrrSkyHotTex(b.slot, b.texId, b.texAddr, b.texHash) ||
        gdx3ds_nrr_bk_tex_serial(b.texId) != b.texSerial) {
        NrrSkyNoteStaleTex(b);
        S.wStale++;
        gdx3ds_nrr_sky_relearn = 1;
        return nullptr;
    }
    if (b.st == nullptr || b.stGen != NrrStateGeneration() || memcmp(b.key.scissor, S.scissor, sizeof(S.scissor)) != 0) {
        if (SkyStateFor(c, b, S.scissor) == nullptr) {
            S.wFallback++;
        }
    }
    return b.st;
}

/* [native] nrr_sky_clock: the same three core calls, each between two clock reads. */
GDX_COLD void NrrSkyClockedCore(NrrCtx* c, const NrrState* st, const SkyBlock& b, const float (*in)[4],
                                const float* dst, uint32_t nv) {
    uint64_t* w = sSky->wCore;
    uint64_t t0 = static_cast<uint64_t>(gdx_ninterp_now());
    NrrStateApply(c, st);
    uint64_t t1 = static_cast<uint64_t>(gdx_ninterp_now());
    w[0] += t1 - t0;
    t0 = static_cast<uint64_t>(gdx_ninterp_now());
    if (b.nInputs != 0) {
        NrrSetInputs(c, st, in, b.nInputs);
    }
    t1 = static_cast<uint64_t>(gdx_ninterp_now());
    w[1] += t1 - t0;
    t0 = static_cast<uint64_t>(gdx_ninterp_now());
    NrrDraw2DScene(c, dst, nv);
    t1 = static_cast<uint64_t>(gdx_ninterp_now());
    w[2] += t1 - t0;
    t0 = static_cast<uint64_t>(gdx_ninterp_now());
    t1 = static_cast<uint64_t>(gdx_ninterp_now());
    w[3] += t1 - t0;
}

/* n triangles over `v` under `mp` with block `kind`: transform, screen, pack, one draw. 0 = drawn
 * (or nothing visible), nonzero = the block is not usable this frame. */
int NrrSkyHotQuads(NrrCtx* c, uint32_t kind, uint32_t gen, const float (*mp)[4], const Vtx* v,
                   const uint8_t (*tris)[3], int n, const uint8_t* shade) {
    SkyRender& S = *sSky;
    SkyBlock& b = S.blocks[kind];
    if (b.gen != gen || !b.usable) {
        S.wStale++;
        gdx3ds_nrr_sky_relearn = 1;
        return 1;
    }
    const uint32_t lo = tris[0][0] < tris[0][1] ? tris[0][0] : tris[0][1]; /* every list starts at its lowest */
    const uint32_t hi = lo + (n == 2 ? 3u : 7u);                        /* one quad, or two */
    float clip[8][4];
    uint32_t code[8];
    NrrSkyHotXform(mp, S.xs, v, lo, hi, clip, code);
    int live = 0;
    uint8_t keep[4];
    for (int t = 0; t < n; t++) {
        if ((code[tris[t][0] - lo] & code[tris[t][1] - lo] & code[tris[t][2] - lo]) == 0) {
            keep[live++] = static_cast<uint8_t>(t);
        }
    }
    S.wRej += static_cast<uint32_t>(n - live);
    if (live == 0) {
        return 0;
    }
    const NrrState* st = NrrSkyHotState(c, b);
    if (st == nullptr) {
        return 2;
    }
    float* dst = static_cast<float*>(NrrRingAlloc(c, static_cast<uint32_t>(live) * 3u * kStride * sizeof(float)));
    if (dst == nullptr) {
        S.wFallback++;
        return 4;
    }
    for (int t = 0; t < live; t++) {
        for (int j = 0; j < 3; j++) {
            const uint32_t i = tris[keep[t]][j];
            NrrSkyHotPack(b.pack, clip[i - lo], v[i], dst + (t * 3 + j) * kStride);
        }
    }
    float in[7][4];
    if (b.nInputs != 0) {
        memcpy(in, b.inputs, sizeof(in));
        if ((b.shadeRgb | b.shadeA) != 0 && shade != nullptr) {
            for (int j = 0; j < 7; j++) {
                for (int k = 0; k < 4; k++) {
                    if ((((k < 3) ? b.shadeRgb : b.shadeA) >> j) & 1u) {
                        in[j][k] = sByteToUnit[shade[k]];
                    }
                }
            }
        }
    }
    if (S.clock != 0) {
        NrrSkyClockedCore(c, st, b, in, dst, static_cast<uint32_t>(live) * 3u);
    } else {
        NrrStateApply(c, st);
        if (b.nInputs != 0) {
            NrrSetInputs(c, st, in, b.nInputs);
        }
        NrrDraw2DScene(c, dst, static_cast<uint32_t>(live) * 3u); /* perspective: the border mode's scene map */
    }
    S.wDraws++;
    S.wTris += static_cast<uint32_t>(live);
    return 0;
}

/* A shader that keeps prim a stage constant: one draw per star with its colour as the input (not
 * met in the corpus: the star combiner folds prim into the vertex colour). */
GDX_COLD void NrrSkyStarsUnfolded(NrrCtx* c, const NrrState* st, const SkyBlock& b, const GdxNrrSkyStar* stars,
                                  uint32_t n, const float* dst) {
    SkyRender& S = *sSky;
    for (uint32_t i = 0; i < n; i++) {
        float in[7][4];
        memcpy(in, b.inputs, sizeof(in));
        for (int j = 0; j < 7; j++) {
            for (int k = 0; k < 4; k++) {
                if (((k < 3 ? S.starPrimRgb : S.starPrimA) >> j) & 1u) {
                    in[j][k] = sByteToUnit[stars[i].rgba[k]];
                }
            }
        }
        NrrSetInputs(c, st, in, b.nInputs);
        NrrDraw2D(c, dst + i * 6u * kStride, 6u);
        S.wDraws++;
    }
}

/* The stars: the learned rectangle translated to each star's corner, its prim colour in the vertex
 * colour (one draw) -- or, when the shader keeps prim a stage constant, one draw per star. */
int NrrSkyHotStars(NrrCtx* c, uint32_t gen, const GdxNrrSkyStar* stars, uint32_t n) {
    SkyRender& S = *sSky;
    SkyBlock& b = S.blocks[GDX_NRRSKY_K_STAR];
    if (b.gen != gen || !b.usable) {
        S.wStale++;
        gdx3ds_nrr_sky_relearn = 1;
        return 1;
    }
    const NrrState* st = NrrSkyHotState(c, b);
    float* dst = st != nullptr ? static_cast<float*>(NrrRingAlloc(c, n * 6u * kStride * sizeof(float))) : nullptr;
    if (dst == nullptr) {
        return 2;
    }
    const bool fold = (S.starPrimRgb | S.starPrimA) == 0; /* no stage constant reads prim */
    for (uint32_t i = 0; i < n; i++) {
        const float dx = static_cast<float>(stars[i].xl - GDX_NRRSKY_STAR_LEARN_XL) * S.starAx;
        const float dy = static_cast<float>(stars[i].yl - GDX_NRRSKY_STAR_LEARN_YL) * S.starAy;
        for (int k = 0; k < 6; k++) {
            float* o = dst + (i * 6u + static_cast<uint32_t>(k)) * kStride;
            memcpy(o, S.starTpl[k], kStride * sizeof(float));
            o[0] += dx;
            o[1] += dy;
            if (S.starFoldRgb) {
                o[6] = sByteToUnit[stars[i].rgba[0]];
                o[7] = sByteToUnit[stars[i].rgba[1]];
                o[8] = sByteToUnit[stars[i].rgba[2]];
            }
            if (S.starFoldA) {
                o[9] = sByteToUnit[stars[i].rgba[3]];
            }
        }
    }
    NrrStateApply(c, st);
    if (!fold) {
        NrrSkyStarsUnfolded(c, st, b, stars, n, dst);
    } else {
        if (b.nInputs != 0) {
            NrrSetInputs(c, st, b.inputs, b.nInputs);
        }
        NrrDraw2D(c, dst, n * 6u);
        S.wDraws++;
    }
    S.wTris += n * 2u;
    return 0;
}

/* n60retry: the head's Camera_Draw viewport reaches the backend only at the next interpreter
 * triangle, and the sky's projection map folds the backend's: a viewport transition (the records
 * screen, the retry intro) drew the sky under the previous frame's. Applied with the hand-off
 * closed (the backend changes citro3d). */
void NrrSkySyncViewport(NrrCtx* c, Interp& I) {
    if (I.ViewportPending()) {
        NrrEnd(c);
        I.ApplyViewportScissor();
        NrrBegin(c);
    }
}

/* The category's draw (core contract): slot 0 = part A (or the whole sky), slot 1 = part B. */
int NrrSkyHotDraw(NrrCtx* c, int slot, const NrrFrame* f) {
    const GdxNrrSkyFrame* snap = static_cast<const GdxNrrSkyFrame*>(NrrSnapGet(f, NRR_CAT_SKY, slot));
    if (snap == nullptr || sSky == nullptr || sSky->interp == nullptr) {
        gdx3ds_nrr_sky_relearn = 1;
        return 1;
    }
    SkyRender& S = *sSky;
    const bool whole = (snap->flags & GDX_NRRSKY_F_VIEW) != 0; /* niview: view `slot`'s whole sky */
    const bool a = slot == 0 || whole;
    const bool b = slot != 0 || (snap->flags & GDX_NRRSKY_F_SPLIT) == 0;
    if (whole) {
        c->keepVp = 1; /* the view's viewport/scissor, cut to its clip square (as the machines) */
        gdx3ds_nrr_note_view(slot);
    }
    const Vtx* v = static_cast<const Vtx*>(snap->vtx);
    int rc = 0;
    if (a) {
        NrrSkySyncViewport(c, *S.interp);
        S.interp->NrrSkyHotHead(S.mp, &S.xs, S.scissor);
        gdx3ds_nrr_sky_xs = S.xs;
        S.wFrames++;
        rc |= NrrSkyHotQuads(c, GDX_NRRSKY_K_GRAD, snap->identity, S.mp, v, kGradTris, 2, nullptr);
        if (snap->starCount != 0) {
            rc |= NrrSkyHotStars(c, snap->identity, snap->stars, snap->starCount);
        }
        for (uint32_t i = 0; i < snap->spriteCount; i++) {
            rc |= NrrSkyHotQuads(c, snap->sprites[i].kind, snap->identity, snap->sprites[i].mp,
                                 static_cast<const Vtx*>(snap->spriteVtx), kSpriteTris, 2, nullptr);
        }
    }
    if (b) {
        rc |= NrrSkyHotQuads(c, GDX_NRRSKY_K_FLOOR, snap->identity, S.mp, v, kFloorTris, 4,
                             snap->floorShadeOk ? snap->floorShade : nullptr);
        if ((snap->flags & GDX_NRRSKY_F_CLOUDS) != 0) {
            rc |= NrrSkyHotQuads(c, GDX_NRRSKY_K_CLOUD, snap->identity, S.mp, v, kCloudTris, 4, nullptr);
        }
    }
    return rc;
}

} // namespace nrrsky

/* ---- receipt ------------------------------------------------------------------------------ */

int Interp::NrrSkyFormat(char* buf, size_t cap, uint32_t tasks) {
    if (sSky == nullptr) {
        return 0;
    }
    SkyRender& S = *sSky;
    if (S.wFrames == 0 && S.wLearn == 0 && S.wStale == 0) {
        return 0;
    }
    const int n = snprintf(buf, cap,
                           "[nrrsky] tasks=%u frames=%u draws=%u tris=%u rej=%u learn=%u lfail=%u stale=%u "
                           "fallback=%u broken=%d",
                           tasks, S.wFrames, S.wDraws, S.wTris, S.wRej, S.wLearn, S.wLearnFail, S.wStale, S.wFallback,
                           gdx3ds_nrr_sky_broken);
    if (S.clock != 0 && S.wDraws != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        /* per draw, emulator instructions (1 tick = 1 instruction), the clock pair subtracted */
        const double d = static_cast<double>(S.wDraws);
        const double z = static_cast<double>(S.wCore[3]) / d;
        snprintf(buf + n, cap - static_cast<size_t>(n), " | core instr/draw apply=%.0f inputs=%.0f draw2d=%.0f (pair=%.0f)",
                 S.wCore[0] / d - z, S.wCore[1] / d - z, S.wCore[2] / d - z, z);
    }
    memset(S.wCore, 0, sizeof(S.wCore));
    S.wFrames = S.wDraws = S.wTris = S.wRej = 0;
    S.wLearn = S.wLearnFail = S.wStale = S.wFallback = 0;
    return n;
}

} // namespace gdxn

/* Core contract (gdx3ds_nrr.h "how to add a category"): the render half of the category init; the
 * game half (port/3ds/game/gdx3ds_nrr_sky_game.c, gdx3ds_nrr_cat_init_sky) calls it. */
extern "C" void gdx3ds_nrr_sky_render_init(void) {
    for (int i = 0; i < 256; i++) {
        gdxn::nrrsky::sByteToUnit[i] = static_cast<float>(i) / 255.0f;
    }
    (void)gdxn::nrrsky::SkyEnsure();
    gdx3ds_nrr_set_draw(NRR_CAT_SKY, gdxn::nrrsky::NrrSkyHotDraw);
}
