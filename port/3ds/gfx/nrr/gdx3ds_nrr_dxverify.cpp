/* port/3ds/gfx/nrr/gdx3ds_nrr_dxverify.cpp -- [native] nrr_direct=2: per-draw proof that the direct
 * emitter (gdx3ds_nrr_direct.cpp) puts the PICA in the same state as the citro3d path. DEBUG, cold.
 *
 * Every NRR call runs twice from the same state: the direct emitter first, into a scratch command
 * buffer (gpuCmdBuf swapped), then the citro3d path for real (its words are what the GPU gets).
 * The two streams differ by design (the direct path sends one offset write where citro3d re-sends
 * the 40-word buffer config, one TEXENVi_COLOR write where citro3d re-sends the stage, uniforms as
 * one covering run, state at apply time instead of at the draw), so the compare is SEMANTIC, on a
 * model of the PICA register file:
 *   - both models start from the same BASELINE: the words citro3d's context stands for, synthesised
 *     by citro3d's own binds at NrrBegin (after the hand-off sync, nothing is pending, so the GPU
 *     holds exactly that: attributes, buffers, effect, viewport, scissor, bound texture units,
 *     TexEnv buffer, the six stages, the bool and every VS float uniform);
 *   - masked writes update bytes; the float-uniform CONFIG/DATA port is modelled as the PICA
 *     does (index + component counter, f32 / f24);
 *   - at each draw trigger (DRAWARRAYS / DRAWELEMENTS) the register file, the uniform file and
 *     "a texture-cache clear happened since the last draw" must be equal in both; a register
 *     that only one path ever wrote is counted `unv=` (the baseline missed it).
 * At NrrEnd the HAND-BACK is checked: every register (and uniform) the direct words left different
 * from the baseline must belong to a group NrrCoreDxEnd marks dirty, so citro3d's next draw puts its
 * own state back (`endbad=`). Receipt: the [nrrdx] line (gdx3ds_nrr.cpp). */
#include "gdx3ds_nrr_core.h"

#include <cstring>
#include <new>

#include "../gdx3ds_hotcold.h"
#include "../../vendor/citro3d/gdx/c3d_gdx.h"
#include "gdx3ds_nrr_crumb.h" /* n60hwdiag */

using namespace nrrcore;

namespace {

constexpr int kRegs = 0x400;
constexpr int kUnifs = C3D_FVUNIF_COUNT;
constexpr uint32_t kScratchWords = 8192;

struct RegFile {
    uint32_t reg[kRegs];
    uint8_t known[kRegs];       /* byte mask of the register's known bytes */
    uint32_t unif[kUnifs][4];
    uint8_t unifKnown[kUnifs];
    uint32_t cacheClears;       /* TEXUNIT_CONFIG bit-16 writes (texture cache clears) */
};

struct PicaModel {
    RegFile now;
    RegFile atDraw;
    bool sawDraw;
    uint32_t unifIndex;
    uint32_t unifComponent;
    bool unifF32;
};

struct Verify {
    PicaModel base;
    PicaModel a; /* the citro3d path */
    PicaModel b; /* the direct path */
    uint32_t scratch[kScratchWords];
    int mismatchLogs;
};

Verify* sV = nullptr;
const NrrCtx* sCur = nullptr;   /* the call being verified (diagnostics) */
uint32_t sDrawInHandoff = 0;

GDX_COLD bool Ready() {
    if (sV == nullptr) {
        sV = new (std::nothrow) Verify();
        if (sV != nullptr) {
            std::memset(sV, 0, sizeof(*sV));
        }
        NRR_CRUMB_FIRST(NRR_CRUMB_DXALLOC, 4, "verify alloc Verify=%p bytes=%lu scratch words=%lu", (void*)sV,
                        (unsigned long)sizeof(Verify), (unsigned long)kScratchWords);
    }
    return sV != nullptr;
}

/* ----------------------------------------------------------------- the model ---- */

GDX_COLD bool IsTriggerOrPort(uint32_t r) {
    switch (r) {
        case GPUREG_DRAWARRAYS:
        case GPUREG_DRAWELEMENTS:
        case GPUREG_VTX_FUNC:
        case GPUREG_RESTART_PRIMITIVE:
        case GPUREG_FRAMEBUFFER_INVALIDATE:
        case GPUREG_FRAMEBUFFER_FLUSH:
        case GPUREG_EARLYDEPTH_CLEAR:
            return true;
        default:
            break;
    }
    if (r >= GPUREG_FIXEDATTRIB_INDEX && r <= GPUREG_FIXEDATTRIB_DATA2) {
        return true;
    }
    if (r >= GPUREG_VSH_FLOATUNIFORM_CONFIG && r <= GPUREG_VSH_FLOATUNIFORM_DATA + 7) {
        return true;
    }
    return r >= GPUREG_GSH_FLOATUNIFORM_CONFIG && r <= GPUREG_GSH_FLOATUNIFORM_DATA + 7;
}

GDX_COLD void UniformWrite(PicaModel& m, uint32_t r, uint32_t value) {
    if (r == GPUREG_VSH_FLOATUNIFORM_CONFIG) {
        m.unifF32 = (value & 0x80000000u) != 0;
        m.unifIndex = value & 0xFFu;
        m.unifComponent = 0;
        return;
    }
    if (m.unifIndex >= static_cast<uint32_t>(kUnifs)) {
        return; /* out of range: the PICA ignores it, so does the model */
    }
    m.now.unif[m.unifIndex][m.unifComponent++] = value;
    if (m.unifComponent == (m.unifF32 ? 4u : 3u)) {
        if (!m.unifF32) {
            m.now.unif[m.unifIndex][3] = 0xF24F24F2u; /* an f24 triple: tagged, never equal to f32 words */
        }
        m.now.unifKnown[m.unifIndex] = 1;
        m.unifIndex++;
        m.unifComponent = 0;
    }
}

GDX_COLD void RegWrite(PicaModel& m, uint32_t r, uint32_t mask, uint32_t value) {
    if (r >= GPUREG_VSH_FLOATUNIFORM_CONFIG && r <= GPUREG_VSH_FLOATUNIFORM_DATA + 7) {
        UniformWrite(m, r, value);
        return;
    }
    if ((r == GPUREG_DRAWARRAYS || r == GPUREG_DRAWELEMENTS) && !m.sawDraw) {
        m.atDraw = m.now;
        m.sawDraw = true;
    }
    if (r == GPUREG_TEXUNIT_CONFIG && (mask & 0x4u) != 0 && (value & BIT(16)) != 0) {
        m.now.cacheClears++;
    }
    for (uint32_t b = 0; b < 4; b++) {
        if ((mask & (1u << b)) == 0) {
            continue;
        }
        const uint32_t lane = 0xFFu << (8u * b);
        m.now.reg[r] = (m.now.reg[r] & ~lane) | (value & lane);
        m.now.known[r] |= static_cast<uint8_t>(1u << b);
    }
}

/* One stream in GPUCMD_AddInternal's layout. Returns false when it is malformed. */
GDX_COLD bool Feed(PicaModel& m, const uint32_t* w, uint32_t n) {
    uint32_t p = 0;
    while (p + 1 < n) {
        const uint32_t header = w[p + 1];
        const uint32_t reg = header & 0x3FFu;
        const uint32_t mask = (header >> 16) & 0xFu;
        const uint32_t count = ((header >> 20) & 0xFFu) + 1u;
        const bool inc = (header >> 31) != 0;
        const uint32_t words = count + 1u + ((count - 1u) & 1u);
        if (p + words > n) {
            return false;
        }
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t value = k == 0 ? w[p] : w[p + 1 + k];
            RegWrite(m, inc ? (reg + k) & 0x3FFu : reg, mask, value);
        }
        p += words;
    }
    return p == n;
}

/* ------------------------------------------------------- the two command streams ---- */

struct BufSave {
    u32* buf;
    u32 size;
    u32 off;
};

void* sC3dState = nullptr; /* citro3d's state around a shadow run (C3Dgdx_StateSave) */
/* Context flags the citro3d path's own apply / constant set since its last draw (it emits only at
 * the draw): the direct shadow must not see them as state "set by someone else" (nrr_direct=1 never
 * writes the context, so there they do not exist). Uniform dirt: the citro3d path sets it only
 * inside its draws, which flush it. */
u32 sC3dOwnFlags = 0;

GDX_COLD void ScratchBegin(BufSave& s) {
    GPUCMD_GetBuffer(&s.buf, &s.size, &s.off);
    GPUCMD_SetBuffer(sV->scratch, kScratchWords, 0);
}

GDX_COLD uint32_t ScratchEnd(const BufSave& s) {
    const uint32_t n = gpuCmdBufOffset;
    GPUCMD_SetBuffer(s.buf, s.size, s.off);
    return n;
}

/* The direct path's words for one call: into the scratch buffer, then into model b. */
template <typename Fn> GDX_COLD void RunDirect(Fn fn) {
    if (sC3dState == nullptr) {
        sC3dState = ::operator new(C3Dgdx_StateBytes(), std::nothrow);
        NRR_CRUMB_FIRST(NRR_CRUMB_DXALLOC, 4, "verify alloc citro3d state save=%p bytes=%lu", sC3dState,
                        (unsigned long)C3Dgdx_StateBytes());
    }
    BufSave s;
    ScratchBegin(s);
    if (sC3dState != nullptr) {
        C3Dgdx_StateSave(sC3dState); /* a mid-hand-off sync must not take citro3d's pending state */
        C3Di_GetContext()->flags &= ~sC3dOwnFlags;
    }
    fn();
    if (sC3dState != nullptr) {
        C3Dgdx_StateLoad(sC3dState);
    }
    const uint32_t n = ScratchEnd(s);
    gDxStats.wordsDx += n;
    if (!Feed(sV->b, sV->scratch, n)) {
        gDxStats.unknown++;
    }
}

/* The citro3d path's words for one call: what it appended to the real buffer, into model a. */
template <typename Fn> GDX_COLD void RunCitro3d(Fn fn, bool draw) {
    u32* buf = gpuCmdBuf;
    const u32 off = gpuCmdBufOffset;
    const u32 before = C3Di_GetContext()->flags;
    const u32 touch = C3Dgdx_unifTouch; /* n60draw: the citro3d path's own uniform writes are not foreign */
    fn();
    C3Dgdx_unifTouch = touch;
    sC3dOwnFlags = draw ? 0u : (sC3dOwnFlags | (C3Di_GetContext()->flags & ~before));
    if (gpuCmdBuf != buf || gpuCmdBufOffset < off) {
        gDxStats.unknown++; /* the buffer moved under the call: not comparable */
        return;
    }
    const uint32_t n = gpuCmdBufOffset - off;
    gDxStats.wordsC3d += n;
    if (!Feed(sV->a, buf + off, n)) {
        gDxStats.unknown++;
    }
}

/* -------------------------------------------------------------- the baseline ---- */

/* The words citro3d's context stands for, through citro3d's own binds (nothing is pending after
 * the hand-off sync, so the GPU holds exactly this). */
GDX_COLD void SynthBaseline() {
    C3D_Context* ctx = C3Di_GetContext();
    C3Di_AttrInfoBind(&ctx->attrInfo);
    C3Di_BufInfoBind(&ctx->bufInfo);
    C3Di_EffectBind(&ctx->effect);
    GPUCMD_AddIncrementalWrites(GPUREG_VIEWPORT_WIDTH, ctx->viewport, 4);
    GPUCMD_AddWrite(GPUREG_VIEWPORT_XY, ctx->viewport[4]);
    GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, ctx->scissor, 3);
    for (int i = 0; i < 3; i++) {
        if (ctx->tex[i] != nullptr) {
            C3Di_SetTex(i, ctx->tex[i]);
        }
    }
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
    GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, ctx->texShadow);
    GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, ctx->texEnvBuf);
    GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, ctx->texEnvBufClr);
    GPUCMD_AddWrite(GPUREG_FOG_COLOR, ctx->fogClr);
    for (int i = 0; i < kStages; i++) {
        C3Di_TexEnvBind(i, &ctx->texEnv[i]);
    }
    GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000u | C3D_BoolUnifs[GPU_VERTEX_SHADER]);
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u);
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, reinterpret_cast<u32*>(C3D_FVUnif[GPU_VERTEX_SHADER]),
                     kUnifs * 4);
}

/* -------------------------------------------------------------- the compare ---- */

GDX_COLD void NoteMismatch(uint32_t kind, uint32_t reg, uint32_t c3d, uint32_t dx) {
    if (gDxStats.firstBadKind == 0) {
        gDxStats.firstBadKind = kind;
        gDxStats.firstBadReg = reg;
        gDxStats.firstBadC3d = c3d;
        gDxStats.firstBadDx = dx;
    }
    if (sV->mismatchLogs < 40) {
        sV->mismatchLogs++;
        gdx3ds_nrr_log("[nrrdx] MISMATCH kind=%u (1 reg 2 unif 3 trigger 4 known 5 end) reg=0x%03lx c3d=0x%08lx "
                       "dx=0x%08lx draw=%lu | cat=%d keepVp=%d scene2d=%d drawInHandoff=%lu sc c3d=%08lx/%08lx "
                       "dx=%08lx/%08lx",
                       (unsigned)kind, (unsigned long)reg, (unsigned long)c3d, (unsigned long)dx,
                       (unsigned long)gDxStats.verified, sCur ? sCur->cat : -1, sCur ? sCur->keepVp : -1,
                       sCur ? sCur->scene2d : -1, (unsigned long)sDrawInHandoff,
                       (unsigned long)sV->a.atDraw.reg[GPUREG_SCISSORTEST_POS],
                       (unsigned long)sV->a.atDraw.reg[GPUREG_SCISSORTEST_DIM],
                       (unsigned long)sV->b.atDraw.reg[GPUREG_SCISSORTEST_POS],
                       (unsigned long)sV->b.atDraw.reg[GPUREG_SCISSORTEST_DIM]);
    }
}

/* n60draw: the gpuxf rows are read only by the shader's gpuxf procedure (shader.v.pica: `ifu xfon`),
 * so a draw whose bool register has xfon off in both models cannot see them: they are compared at
 * gpuxf draws only. (A carried hand-off leaves NRR's last block there where the citro3d path's begin
 * flush put citro3d's back; a 2D draw before the next gpuxf draw reads neither.) */
GDX_COLD bool XfRowsUnread(const RegFile& a, const RegFile& b) {
    const int loc = NrrDxXfLoc();
    if (loc < 0 || gBk.xfBoolLoc < 0) {
        return false;
    }
    const uint32_t r = GPUREG_VSH_BOOLUNIFORM;
    const uint32_t bit = 1u << (gBk.xfBoolLoc - 0x68);
    return (a.known[r] & 1u) != 0 && (b.known[r] & 1u) != 0 && (a.reg[r] & bit) == 0 && (b.reg[r] & bit) == 0;
}

/* 0 = equal; else the first difference's kind (and it is logged). */
GDX_COLD uint32_t CompareFiles(const RegFile& a, const RegFile& b) {
    for (uint32_t r = 0; r < static_cast<uint32_t>(kRegs); r++) {
        if (IsTriggerOrPort(r)) {
            continue;
        }
        if (a.known[r] != b.known[r]) {
            NoteMismatch(4, r, a.reg[r], b.reg[r]);
            return 4;
        }
        uint32_t lanes = 0;
        for (uint32_t k = 0; k < 4; k++) {
            lanes |= (a.known[r] & (1u << k)) ? (0xFFu << (8u * k)) : 0u;
        }
        if (((a.reg[r] ^ b.reg[r]) & lanes) != 0) {
            NoteMismatch(1, r, a.reg[r], b.reg[r]);
            return 1;
        }
    }
    const bool xfUnread = XfRowsUnread(a, b);
    const int xf = NrrDxXfLoc();
    for (int u = 0; u < kUnifs; u++) {
        if (xfUnread && u >= xf && u < xf + kXfVec4s) {
            continue;
        }
        if (a.unifKnown[u] != b.unifKnown[u]) {
            NoteMismatch(4, 0x1000u + static_cast<uint32_t>(u), 0, 0);
            return 4;
        }
        if (a.unifKnown[u] && std::memcmp(a.unif[u], b.unif[u], sizeof(a.unif[u])) != 0) {
            NoteMismatch(2, 0x1000u + static_cast<uint32_t>(u), a.unif[u][0], b.unif[u][0]);
            return 2;
        }
    }
    /* n60draw: a clear the direct words make where the citro3d path makes none is not a difference a
     * pixel can show (the cache only refetches what memory holds); one the direct words MISS is. A
     * carried hand-off re-emits citro3d's texture state (with its clear) whenever citro3d still has it
     * pending from an earlier direct end, where the citro3d path's own end left nothing pending. */
    if (a.cacheClears != 0 && b.cacheClears == 0) {
        NoteMismatch(3, GPUREG_TEXUNIT_CONFIG, a.cacheClears, b.cacheClears);
        return 3;
    }
    return 0;
}

GDX_COLD void CompareDraw() {
    PicaModel& a = sV->a;
    PicaModel& b = sV->b;
    if (!a.sawDraw && !b.sawDraw) {
        return;
    }
    gDxStats.verified++;
    uint32_t kind = 0;
    if (a.sawDraw != b.sawDraw) {
        NoteMismatch(3, GPUREG_DRAWARRAYS, a.sawDraw ? 1u : 0u, b.sawDraw ? 1u : 0u);
        kind = 3;
    } else {
        kind = CompareFiles(a.atDraw, b.atDraw);
    }
    if (kind == 4) {
        gDxStats.unknown++;
    } else if (kind != 0) {
        gDxStats.badDraw++;
    }
    a.sawDraw = false;
    b.sawDraw = false;
    a.now.cacheClears = 0;
    b.now.cacheClears = 0;
}

/* ------------------------------------------------------------ the hand-back ---- */

/* The citro3d dirty flag whose re-emission rewrites register r; 0 = rewritten by every draw's own
 * packets (nothing to restore); 0xFFFFFFFF = no group owns it (the direct path must not leave it
 * changed). */
GDX_COLD uint32_t OwnerFlag(uint32_t r) {
    for (int i = 0; i < kStages; i++) {
        const uint32_t base = GPUREG_TEXENV0_SOURCE + 8u * static_cast<uint32_t>(i < 4 ? i : i + 2);
        if (r >= base && r < base + 5u) {
            return C3DiF_TexEnv(i);
        }
    }
    if ((r >= GPUREG_VIEWPORT_WIDTH && r <= GPUREG_VIEWPORT_INVH) || r == GPUREG_VIEWPORT_XY) {
        return C3DiF_Viewport;
    }
    if (r >= GPUREG_SCISSORTEST_MODE && r <= GPUREG_SCISSORTEST_DIM) {
        return C3DiF_Scissor;
    }
    if (r == GPUREG_ATTRIBBUFFERS_FORMAT_LOW || r == GPUREG_ATTRIBBUFFERS_FORMAT_HIGH ||
        r == GPUREG_VSH_INPUTBUFFER_CONFIG || r == GPUREG_VSH_NUM_ATTR ||
        r == GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW || r == GPUREG_VSH_ATTRIBUTES_PERMUTATION_HIGH) {
        return C3DiF_AttrInfo;
    }
    if (r == GPUREG_ATTRIBBUFFERS_LOC || (r >= GPUREG_ATTRIBBUFFER0_OFFSET && r <= GPUREG_ATTRIBBUFFERB_CONFIG2)) {
        return C3DiF_BufInfo;
    }
    switch (r) {
        case GPUREG_DEPTHMAP_ENABLE:
        case GPUREG_FACECULLING_CONFIG:
        case GPUREG_DEPTHMAP_SCALE:
        case GPUREG_DEPTHMAP_OFFSET:
        case GPUREG_FRAGOP_ALPHA_TEST:
        case GPUREG_FRAGOP_ALPHA_TEST + 1: /* stencil test */
        case GPUREG_FRAGOP_ALPHA_TEST + 2: /* stencil op */
        case GPUREG_FRAGOP_ALPHA_TEST + 3: /* depth / colour mask */
        case GPUREG_GAS_DELTAZ_DEPTH:
        case GPUREG_BLEND_COLOR:
        case GPUREG_BLEND_FUNC:
        case GPUREG_LOGIC_OP:
        case GPUREG_COLOR_OPERATION:
        case GPUREG_FRAGOP_SHADOW:
        case GPUREG_EARLYDEPTH_TEST1:
        case GPUREG_EARLYDEPTH_TEST2:
        case GPUREG_EARLYDEPTH_FUNC:
        case GPUREG_EARLYDEPTH_DATA:
            return C3DiF_Effect;
        case GPUREG_TEXUNIT_CONFIG:
        case GPUREG_TEXUNIT0_SHADOW:
            return C3DiF_TexAll;
        case GPUREG_TEXENV_UPDATE_BUFFER:
        case GPUREG_TEXENV_BUFFER_COLOR:
        case GPUREG_FOG_COLOR:
            return C3DiF_TexEnvBuf;
        case GPUREG_PRIMITIVE_CONFIG:
        case GPUREG_INDEXBUFFER_CONFIG:
        case GPUREG_NUMVERTICES:
        case GPUREG_GEOSTAGE_CONFIG:
        case GPUREG_VERTEX_OFFSET:
        case GPUREG_GEOSTAGE_CONFIG2:
        case GPUREG_START_DRAW_FUNC0:
            return 0;
        default:
            break;
    }
    if ((r >= GPUREG_TEXUNIT0_BORDER_COLOR && r <= GPUREG_TEXUNIT0_TYPE) ||
        (r >= GPUREG_TEXUNIT1_BORDER_COLOR && r <= GPUREG_TEXUNIT1_TYPE) ||
        (r >= GPUREG_TEXUNIT2_BORDER_COLOR && r <= GPUREG_TEXUNIT2_TYPE)) {
        return C3DiF_TexAll;
    }
    return 0xFFFFFFFFu;
}

GDX_COLD bool UniformRestored(int u, uint32_t groups) {
    const int proj = NrrDxProjLoc();
    const int xf = NrrDxXfLoc();
    if ((groups & G_PROJ) != 0 && u >= proj && u < proj + 4) {
        return true;
    }
    if ((groups & G_STK) != 0 && u == NrrDxStkLoc()) {
        return true;
    }
    return (groups & G_XF) != 0 && xf >= 0 && u >= xf && u < xf + kXfVec4s;
}

/* Every register / uniform the direct words left different from the baseline is re-emitted by
 * citro3d after NrrCoreDxEnd's marks. */
GDX_COLD bool HandBackOk(const RegFile& dx, const RegFile& base, uint32_t flags, uint32_t unif) {
    for (uint32_t r = 0; r < static_cast<uint32_t>(kRegs); r++) {
        if (IsTriggerOrPort(r) || dx.known[r] == 0) {
            continue;
        }
        const bool same = dx.known[r] == base.known[r] && dx.reg[r] == base.reg[r];
        if (same) {
            continue;
        }
        if (r == GPUREG_VSH_BOOLUNIFORM) {
            if ((unif & G_BOOL) == 0) {
                NoteMismatch(5, r, base.reg[r], dx.reg[r]);
                return false;
            }
            continue;
        }
        const uint32_t owner = OwnerFlag(r);
        if (owner != 0 && (owner == 0xFFFFFFFFu || (owner & flags) == 0)) {
            NoteMismatch(5, r, base.reg[r], dx.reg[r]);
            return false;
        }
    }
    for (int u = 0; u < kUnifs; u++) {
        if (!dx.unifKnown[u] || std::memcmp(dx.unif[u], base.unif[u], sizeof(dx.unif[u])) == 0) {
            continue;
        }
        if (!UniformRestored(u, unif)) {
            NoteMismatch(5, 0x1000u + static_cast<uint32_t>(u), base.unif[u][0], dx.unif[u][0]);
            return false;
        }
    }
    return true;
}

/* n60hwdiag: the first direct-word emission (into the scratch) and the first compares per category. */
GDX_COLD void CrumbDxEmit(const NrrCtx* c, const char* what) {
    NRR_CRUMB_FIRST(NRR_CRUMB_DXEMIT + c->cat, 2, "dx emit > %s cat=%s (direct words into the verify scratch %p) "
                    "real cmdbuf=%p off=%lu/%lu", what, gdx3ds_nrr_cat_name(c->cat), (void*)sV->scratch,
                    (void*)gpuCmdBuf, (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize);
}

GDX_COLD void CrumbDxCompare(const NrrCtx* c) {
    NRR_CRUMB_FIRST(NRR_CRUMB_DXCMP + c->cat, 2, "dx cmp < cat=%s verified=%lu bad=%lu unv=%lu words c3d=%lu dx=%lu "
                    "first=%lu reg=0x%03lx cmdbuf=%p off=%lu", gdx3ds_nrr_cat_name(c->cat),
                    (unsigned long)gDxStats.verified, (unsigned long)gDxStats.badDraw, (unsigned long)gDxStats.unknown,
                    (unsigned long)gDxStats.wordsC3d, (unsigned long)gDxStats.wordsDx,
                    (unsigned long)gDxStats.firstBadKind, (unsigned long)gDxStats.firstBadReg, (void*)gpuCmdBuf,
                    (unsigned long)gpuCmdBufOffset);
}

} // namespace

/* ------------------------------------------------------------------ entries ---- */

GDX_COLD void nrrcore::NrrDxVerifyBegin(int carried, const uint32_t* words, uint32_t n) {
    if (!Ready()) {
        return;
    }
    std::memset(&sV->base, 0, sizeof(sV->base));
    BufSave s;
    ScratchBegin(s);
    SynthBaseline();
    const uint32_t nb = ScratchEnd(s);
    Feed(sV->base, sV->scratch, nb);
    sV->base.sawDraw = false;
    sV->base.now.cacheClears = 0;
    sDrawInHandoff = 0;
    sC3dOwnFlags = 0;
    if (!carried) {
        sV->a = sV->base;
        sV->b = sV->base;
        return;
    }
    /* n60draw carry: no word reached the GPU since the previous end, so the citro3d model is its
     * previous state plus the real begin flush, and the direct model is its previous state (its
     * lazy words follow in the shadow runs). A cache clear in the flush counts for the first draw. */
    sV->a.sawDraw = false;
    sV->a.now.cacheClears = 0;
    if (words != nullptr && n != 0 && !Feed(sV->a, words, n)) {
        gDxStats.unknown++;
    }
    sV->b.sawDraw = false;
    sV->b.now.cacheClears = 0;
}

GDX_COLD void nrrcore::NrrDxVerifyApply(NrrCtx* c, const NrrState* s) {
    if (!Ready()) {
        NrrCoreC3dApply(c, s);
        return;
    }
    RunDirect([&] { NrrCoreDxApply(c, s); });
    RunCitro3d([&] { NrrCoreC3dApply(c, s); }, false);
}

GDX_COLD void nrrcore::NrrDxVerifyConst(NrrCtx* c, int stage, uint32_t packed) {
    if (!Ready()) {
        NrrCoreC3dConst(c, stage, packed);
        return;
    }
    RunDirect([&] { NrrCoreDxConst(stage, packed); });
    RunCitro3d([&] { NrrCoreC3dConst(c, stage, packed); }, false);
}

GDX_COLD void nrrcore::NrrDxVerifyDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts) {
    if (!Ready()) {
        NrrCoreC3dDraw2D(c, verts, nVerts);
        return;
    }
    sCur = c;
    sDrawInHandoff++;
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDxEmit(c, "Draw2D");
    }
    RunDirect([&] { NrrCoreDxDraw2D(c, verts, nVerts); });
    RunCitro3d([&] { NrrCoreC3dDraw2D(c, verts, nVerts); }, true);
    CompareDraw();
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDxCompare(c);
    }
}

GDX_COLD void nrrcore::NrrDxVerifyDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx,
                                         uint32_t nIdx, const float* unif28x4, uint32_t rows) {
    if (!Ready()) {
        NrrCoreC3dDrawXf(c, verts, stride, idx, nIdx, unif28x4);
        return;
    }
    sCur = c;
    sDrawInHandoff++;
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDxEmit(c, "DrawXf");
    }
    RunDirect([&] { NrrCoreDxDrawXf(c, verts, stride, idx, nIdx, unif28x4, rows); });
    RunCitro3d([&] { NrrCoreC3dDrawXf(c, verts, stride, idx, nIdx, unif28x4); }, true);
    CompareDraw();
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDxCompare(c);
    }
}

GDX_COLD void nrrcore::NrrDxVerifyEnd() {
    if (!Ready()) {
        return;
    }
    uint32_t unif = 0;
    const uint32_t flags = NrrDxEndFlags(&unif);
    if (!HandBackOk(sV->b.now, sV->base.now, flags, unif)) {
        gDxStats.badEnd++;
    }
    NrrCoreDxMarkEnd(); /* n60draw carry: the verify's NrrEnd does not run NrrCoreDxEnd */
}
