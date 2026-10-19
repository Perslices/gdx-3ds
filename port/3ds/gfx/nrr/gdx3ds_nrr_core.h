/* port/3ds/gfx/nrr/gdx3ds_nrr_core.h -- C++ internals of the NRR render half, shared by the
 * citro3d path (gdx3ds_nrr.cpp), the direct emitter (gdx3ds_nrr_direct.cpp, [native] nrr_direct=1)
 * and its verify (gdx3ds_nrr_dxverify.cpp, nrr_direct=2). Categories include gdx3ds_nrr.h only.
 *
 * Function naming (tools/native/nrr_size.sh): NrrCoreHot* run in both paths, NrrCoreC3d* only on
 * the citro3d path (nrr_direct=0), NrrCoreDx* only on the direct path (nrr_direct=1). */
#ifndef GDX3DS_NRR_CORE_H
#define GDX3DS_NRR_CORE_H

#include "gdx3ds_nrr_internal.h"

#include <3ds.h>
#include <citro3d.h>

#include "../gdx3ds_stereo.h" /* n60stereo: gdx3ds_stereo_replay_frame, gdx3ds_stereo_rows */

/* The vendored citro3d's context (C3D_Context, C3DiF_*): the hand-off saves, restores or dirties
 * groups of it. Same layout contract as gdx3ds_overlap.c / gdx3ds_replay.cpp (citro3d 1.7.1). */
extern "C" {
#include "../../vendor/citro3d/source/internal.h"
}

namespace nrrcore {

constexpr int kXfVec4s = 28; /* gfx_citro3d.cpp kXfUniformVec4s (Gdx3dsXfDraw from mvp to off) */
constexpr int kStages = 6;   /* PICA TexEnv stages */

/* A state block's precomputed command words (direct path): the scissor (4 words), the effect
 * (C3Di_EffectBind's 14 packets, 34 words) and the six TexEnv stages (6 words each). */
constexpr uint32_t kDxStateWords = 4u + 34u + 6u * 6u;
/* Its groups (scissor, effect, stage 0..5): an apply re-sends only the groups whose interned
 * content id differs from what the hand-off last sent. */
constexpr int kDxGroups = 2 + kStages;

/* One texture unit of a resolved block: the backend id and the C3D_Tex param bits NRR's
 * filter / wrap set (param = (src.param & ~mask) | bits on a copy of the backend's struct). */
struct NrrTexUnit {
    uint32_t id; /* backend texture id, NRR_TEX_NONE = unit unused */
    uint32_t mask;
    uint32_t bits;
    const void* obj; /* [n60mach] category-owned C3D_Tex (wins over id) */
};

/* A TexEnv stage whose constant rides a combiner input (RefreshStageConstantsPacked's cases),
 * resolved cold: the 1-based inputs feeding rgb / alpha (0 = the fixed value). */
struct NrrConstSrc {
    uint8_t stage;
    uint8_t rgbIn;
    uint8_t aIn;
    uint8_t rgbOne; /* the fixed rgb when rgbIn is 0 or absent: 1 = 1.0, 0 = 0.0 */
    uint8_t aOne;
};

/* citro3d context groups the citro3d path saves lazily before it first changes them
 * (NrrCtx::saved), and the groups the direct path wrote to the GPU behind citro3d's back. */
enum : uint32_t {
    G_ATTR = 1u << 0,    /* attribute layout */
    G_BUF = 1u << 1,     /* vertex buffers */
    G_EFFECT = 1u << 2,  /* depth test/map, blend, alpha test, cull */
    G_VP = 1u << 3,      /* viewport (+ scissor on the citro3d path: C3D_SetViewport resets it) */
    G_TEXENV = 1u << 4,  /* the six TexEnv stages */
    G_ENVBUF = 1u << 5,  /* TexEnv buffer word: the fog mode */
    G_TEX = 1u << 6,     /* texture units */
    G_PROJ = 1u << 7,    /* the projection uniform (4 vec4) */
    G_XF = 1u << 8,      /* the gpuxf uniform block (28 vec4) */
    G_BOOL = 1u << 9,    /* the vertex shader's bool uniforms (xfon) */
    G_SCISSOR = 1u << 10, /* direct path only: the scissor alone */
    G_STK = 1u << 11      /* n60stereo: the stereo row uniform (stk) of the frame replay */
};

/* Everything the NRR draw path needs from the backend (latched once). */
extern NrrBkInfo gBk;
/* [native] nrr_direct: 0 = the citro3d path, 1 = direct words, 2 = verify (citro3d's words are
 * submitted; the direct words are built into a scratch buffer and compared). */
extern int gDirect;

/* n60draw: [native] nrr_draw, a bit mask (docs/research/native/n60draw-progress.md), default 0. */
enum : uint32_t {
    kDrawGuard = 1u,   /* the per-draw sync test reads citro3d's uniform-touch hint before scanning */
    kDrawXfRows = 2u,  /* NrrDrawXfRows: only the rows the caller may have changed are compared */
    kDrawCarry = 4u,   /* a hand-off that follows the previous one with no GPU word in between keeps
                          what the GPU holds: no citro3d re-emission, no full re-send */
    kDrawTrkPair = 8u, /* trk: pair-packed index append (gdx3ds_nrr_trk_draw.cpp) */
    kDrawAll = 15u
};
extern uint32_t gDraw;
constexpr uint32_t kXfAllRows = (1u << kXfVec4s) - 1u;

} // namespace nrrcore

/* The resolved block: citro3d's own context words, precomputed cold, so the hot apply is a few
 * compares and copies into citro3d's context (the same words the C3D_* setters would store) --
 * and, for the direct path, the finished command words of the scissor, effect and stages. */
struct NrrState {
    NrrStateKey key;
    uint32_t gen;
    NrrBkShader sh;       /* the backend's translation (inputs, fog stage, vertex inputs) */
    C3D_TexEnv env[nrrcore::kStages];
    C3D_Effect effect;    /* depth test + map, blend, alpha test, cull */
    u32 scissor[3];
    uint8_t numEnv;       /* stages that differ from passthrough (the rest are TexEnvInit) */
    uint8_t numConst;     /* entries of cs */
    uint8_t pad[2];
    nrrcore::NrrConstSrc cs[nrrcore::kStages];
    nrrcore::NrrTexUnit tex[2];
    uint32_t dxCount;     /* words of dx (kDxStateWords) */
    uint32_t dxConst[nrrcore::kStages]; /* the stages' constants in dx (what NrrSetInputs diffs against) */
    uint16_t dxGroup[nrrcore::kDxGroups]; /* interned content id per group of dx (never 0xFFFF) */
    uint32_t dx[nrrcore::kDxStateWords];
};

namespace nrrcore {

/* ---- the direct path (gdx3ds_nrr_direct.cpp) ---- */

/* Cold: the block's command words (s.dx) from its resolved env / effect / scissor. */
void NrrDxBuildState(NrrState& s);
/* n60intro: the scissor group of s.dx again (the rest is a copy of a built block's). */
void NrrDxRescissor(NrrState& s);
/* Cold, once per backend latch: the attribute layouts, buffer templates and viewport words. */
void NrrDxLatch();
/* NrrBegin (nrr_direct 1 and 2): forget what the last hand-off emitted, and emit what citro3d still
 * has pending (dirty context groups, uniforms) so the GPU holds exactly what citro3d's context says
 * before the first direct word; under 2 also the verify's baseline. */
void NrrCoreDxBegin();
void NrrCoreDxProjInvalidate(); /* nimach: the next 3D draw re-uploads the scene map (NrrProjInvalidate) */
void NrrCoreDxApply(NrrCtx* c, const NrrState* s);
void NrrCoreDxConst(int stage, uint32_t packed);
void NrrCoreDxDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts);
void NrrCoreDxDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                     const float* unif28x4, uint32_t rows);
/* NrrEnd: mark every group the direct words changed dirty in citro3d (it re-emits its own state). */
void NrrCoreDxEnd(NrrCtx* c);
/* n60draw: a render task ended (no carry across it); where a hand-off's words end (the verify's
 * NrrEnd, which does not run NrrCoreDxEnd); the [nrrdraw] window receipt. */
void NrrCoreDxTaskEnd();
void NrrCoreDxMarkEnd();
void NrrDxDrawWindow(unsigned long frame);

/* The dirty marks NrrCoreDxEnd would set: context flags, and which uniform groups (G_PROJ, G_XF,
 * G_BOOL) it re-dirties. */
uint32_t NrrDxEndFlags(uint32_t* unifGroups);
/* The registers the direct path's 2D / 3D projection and gpuxf block occupy (verify). */
int NrrDxProjLoc();
int NrrDxXfLoc();
int NrrDxStkLoc();

/* n60stereo: the stk row of a draw in a frame-replay frame (gdx3ds_stereo.h): a 2D draw whose first
 * vertex has clip w == 1 is screen-depth content (the per-draw loop's ortho heuristic), anything
 * else -- the 2D scene draws, every gpuxf draw -- is SCENE. NULL = not a replay frame (mono). */
inline const float* NrrStereoRow(NrrCtx* c, const float* verts2d) {
    if (!gdx3ds_stereo_replay_frame || gBk.stkLoc < 0) {
        return nullptr;
    }
    if (verts2d == nullptr || verts2d[3] != 1.0f) {
        return gdx3ds_stereo_rows[0];
    }
    if (c->anchorSet) {
        return c->anchorRow; /* NrrDraw2D clears it after the draw (both verify runs read it) */
    }
    return gdx3ds_stereo_rows[1];
}

/* [n60mach] keepVp gpuxf: the game's clip square (x, y in [-w, w]) on the main target under the 3D
 * map `m`, intersected with the scissor `cur` (citro3d's scissor words), as scissor words in `out`
 * (C3D_SetScissor's). 0 = nothing to cut (gdx3ds_nrr.cpp, cold, shared by both emitters). */
int NrrClipScissorWords(const C3D_Mtx* m, const u32 cur[3], u32 out[3]);

/* ---- the citro3d path (gdx3ds_nrr.cpp), callable by the verify ---- */

void NrrCoreC3dApply(NrrCtx* c, const NrrState* s);
void NrrCoreC3dConst(NrrCtx* c, int stage, uint32_t packed);
void NrrCoreC3dDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts);
void NrrCoreC3dDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                      const float* unif28x4);

/* ---- the verify (gdx3ds_nrr_dxverify.cpp, cold) ---- */

/* carried = 0: every model starts from citro3d's context. 1 (n60draw carry): the direct model keeps
 * the GPU it left at the previous end; the citro3d model is its previous state plus `words` (the
 * real begin flush, n words). */
void NrrDxVerifyBegin(int carried, const uint32_t* words, uint32_t n);
void NrrDxVerifyApply(NrrCtx* c, const NrrState* s);
void NrrDxVerifyConst(NrrCtx* c, int stage, uint32_t packed);
void NrrDxVerifyDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts);
void NrrDxVerifyDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                       const float* unif28x4, uint32_t rows);
/* Before the citro3d path's NrrEnd: the hand-back check (every register the direct words left
 * different from citro3d's context is in a group NrrCoreDxEnd marks dirty). */
void NrrDxVerifyEnd();

/* Receipt counters of the direct path (one window). */
struct NrrDxStats {
    uint32_t handoffs;
    uint32_t syncs;      /* hand-offs that found citro3d state pending at NrrBegin (emitted first) */
    uint32_t midSyncs;   /* citro3d state set INSIDE a hand-off (a category calling the backend), emitted */
    uint32_t draws;
    uint32_t verified;   /* verify: draws compared */
    uint32_t badDraw;    /* verify: draws whose GPU state at the trigger differs */
    uint32_t unknown;    /* verify: draws where one path wrote a register the baseline did not hold */
    uint32_t badEnd;     /* verify: hand-offs whose hand-back would leave a stale GPU register */
    uint32_t wordsC3d;   /* verify: citro3d's words for NRR draws */
    uint32_t wordsDx;    /* verify: the direct words for the same calls */
    uint32_t firstBadReg;
    uint32_t firstBadC3d;
    uint32_t firstBadDx;
    uint32_t firstBadKind; /* 1 register, 2 uniform, 3 trigger, 4 knownness, 5 end */
};
extern NrrDxStats gDxStats;

} // namespace nrrcore

#endif /* GDX3DS_NRR_CORE_H */
