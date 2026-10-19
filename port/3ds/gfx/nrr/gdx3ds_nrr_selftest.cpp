/* port/3ds/gfx/nrr/gdx3ds_nrr_selftest.cpp -- [native] nrr_selftest=1 (with nrr=1): DEBUG ONLY,
 * never in a card. Proves the NRR hand-off end to end in the emulator: it consumes the effects
 * section (c5tick EFX: explosions, debris, sparks -- side-effect free, so the race state is
 * untouched; the effects simply are not drawn while it is on), snapshots the tick and the view,
 * and at its 0x63 marker draws, in the HUD band at the bottom of the screen, through the whole core
 * path (state block, apply, per-draw inputs, ring, draw, restore):
 *   1. a solid 2D quad (shade passthrough), then the same state again with new ring vertices;
 *   2. a textured 2D quad (a category-owned 8x8 checker, texObj) whose combiner lerps two inputs by
 *      the texel -- one input rides a TexEnv constant (NrrSetInputs' constant path);
 *   3. two gpuxf quads (NrrDrawXf, raw 16-byte Vtx + indices in the ring, the 28-vec4 block), the
 *      second with one vec4 changed (a one-register uniform run).
 * Everything drawn after it in the frame (machines' boost, the HUD) must look stock: that is the
 * hand-off check. Both emitters ([native] nrr_direct 0/1/2) run the same calls.
 *
 * [native] nrr_selftest_clock=1: a tick pair around each core call; every 64 frames
 * `[nrrself] frames= ticks/call apply= inputs= draw2d= drawxf= (pair= subtracted)`, svc ticks per
 * call with the pair's own cost subtracted (1 emulator tick ~ 1 instruction: emu us x 268). */
#include "gdx3ds_nrr.h"

#include <3ds.h>
#include <citro3d.h>

#include <cstring>

#include "../gdx3ds_hotcold.h"
#include "../gdx3ds_native_cfg.h"
#include "../gdx3ds_vbopack.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

namespace {

struct SelfSnap {
    uint32_t tick;
    int32_t view;
};

/* gfx_cc_get_features nibbles: id0 >> (cycle*32 + channel*16 + k*4), (a - b) * c + d for k 0..3;
 * 0 = SHADER_0, 1..7 = SHADER_INPUT_n, 8 = SHADER_TEXEL0. */
constexpr uint64_t kCcShade = (1ull << 12) | (1ull << 28);  /* rgb = in1, a = in1 */
constexpr uint64_t kCcLerpTex = 0x2821ull | (1ull << 28);   /* rgb = (in1 - in2) * tex0 + in2, a = in1 */

enum { kStSolid = 0, kStTex = 1, kStXf = 2, kStates = 3 };
enum { kClkApply = 0, kClkInputs, kClkDraw2D, kClkDrawXf, kClkDraw2DSame, kClkDrawXfNext, kClkPair, kClocks };

struct SelfTest {
    const NrrState* st[kStates];
    uint32_t gen;
    C3D_Tex tex;
    bool texOk;
    bool logged;
    int clock;
    uint64_t ticks[kClocks];
    uint32_t calls[kClocks];
    uint32_t frames;
};

SelfTest sT;

/* XF raw vertex (shader.v.pica v0..v2): s16 ob + flag, s16 st, u8 cn. */
struct XfVtx {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
};

int SelftestSnap(NrrFrame* f, int cat, int view) {
    SelfSnap* s = static_cast<SelfSnap*>(NrrSnapAlloc(f, sizeof(SelfSnap)));
    if (s == nullptr) {
        return 0;
    }
    s->tick = NrrFrameTick(f);
    s->view = view;
    NrrSnapSet(f, cat, view, s);
    return 1;
}

/* ------------------------------------------------------------------ cold setup ---- */

/* An 8x8 RGBA8 checker in linear memory (the tiled texel order only permutes the cells). */
GDX_COLD bool MakeTexture() {
    if (!C3D_TexInit(&sT.tex, 8, 8, GPU_RGBA8)) {
        return false;
    }
    uint32_t* px = static_cast<uint32_t*>(sT.tex.data);
    for (int i = 0; i < 64; i++) {
        px[i] = ((i >> 2) & 1) ? 0xFFFFFFFFu : 0xFF404040u;
    }
    C3D_TexFlush(&sT.tex);
    C3D_TexSetFilter(&sT.tex, GPU_NEAREST, GPU_NEAREST);
    return true;
}

GDX_COLD const NrrState* BuildState(uint64_t cc, int program, bool textured) {
    NrrStateKey k;
    std::memset(&k, 0, sizeof(k));
    k.shaderId0 = cc;
    k.shaderId1 = 0;
    k.tex[0] = NRR_TEX_NONE;
    k.tex[1] = NRR_TEX_NONE;
    k.texObj[0] = textured && sT.texOk ? &sT.tex : nullptr;
    k.program = static_cast<uint8_t>(program);
    k.blend = NRR_BLEND_OFF;
    k.cull = NRR_CULL_NONE;
    return NrrStateGet(&k);
}

GDX_COLD void LogStates() {
    if (sT.logged || gdx_dbg_logf == nullptr) {
        return;
    }
    sT.logged = true;
    gdx_dbg_logf("[nrrself] states solid=%d tex=%d (texobj=%d vtxin=%d) xf=%d clock=%d\n", sT.st[kStSolid] != nullptr,
                 sT.st[kStTex] != nullptr, sT.texOk ? 1 : 0,
                 sT.st[kStTex] != nullptr ? NrrStateVtxColorInput(sT.st[kStTex]) : -1, sT.st[kStXf] != nullptr,
                 sT.clock);
}

GDX_COLD bool EnsureStates() {
    if (sT.st[kStSolid] != nullptr && sT.gen == NrrStateGeneration()) {
        return true;
    }
    sT.st[kStSolid] = BuildState(kCcShade, NRR_PROG_2D, false);
    sT.st[kStTex] = BuildState(kCcLerpTex, NRR_PROG_2D, true);
    sT.st[kStXf] = BuildState(kCcShade, NRR_PROG_XF, false);
    sT.gen = NrrStateGeneration();
    LogStates();
    return sT.st[kStSolid] != nullptr;
}

GDX_COLD void EmitClockWindow() {
    const uint64_t pair = sT.calls[kClkPair] ? sT.ticks[kClkPair] / sT.calls[kClkPair] : 0;
    unsigned per[kClkPair];
    for (int i = 0; i < kClkPair; i++) {
        const uint64_t avg = sT.calls[i] ? sT.ticks[i] / sT.calls[i] : 0;
        per[i] = static_cast<unsigned>(avg > pair ? avg - pair : 0);
    }
    if (gdx_dbg_logf != nullptr) {
        gdx_dbg_logf("[nrrself] frames=%u ticks/call apply=%u inputs=%u draw2d=%u drawxf=%u | steady draw2d=%u "
                     "drawxf=%u (pair=%u subtracted) calls apply=%u inputs=%u draw2d=%u drawxf=%u\n",
                     (unsigned)sT.frames, per[kClkApply], per[kClkInputs], per[kClkDraw2D], per[kClkDrawXf],
                     per[kClkDraw2DSame], per[kClkDrawXfNext], (unsigned)pair, (unsigned)sT.calls[kClkApply], (unsigned)sT.calls[kClkInputs],
                     (unsigned)sT.calls[kClkDraw2D], (unsigned)sT.calls[kClkDrawXf]);
    }
    std::memset(sT.ticks, 0, sizeof(sT.ticks));
    std::memset(sT.calls, 0, sizeof(sT.calls));
    sT.frames = 0;
}

/* --------------------------------------------------------------- the clocked calls ---- */

inline uint64_t ClockStart() {
    return sT.clock ? svcGetSystemTick() : 0;
}

inline void ClockStop(int which, uint64_t t0) {
    if (sT.clock) {
        sT.ticks[which] += svcGetSystemTick() - t0;
        sT.calls[which]++;
    }
}

void NrrSelftestHotApply(NrrCtx* c, const NrrState* st) {
    const uint64_t t0 = ClockStart();
    NrrStateApply(c, st);
    ClockStop(kClkApply, t0);
}

void NrrSelftestHotInputs(NrrCtx* c, const NrrState* st, const float (*in)[4], int n) {
    const uint64_t t0 = ClockStart();
    NrrSetInputs(c, st, in, n);
    ClockStop(kClkInputs, t0);
}

void NrrSelftestHotDraw2D(NrrCtx* c, const float* v, uint32_t n, int which) {
    const uint64_t t0 = ClockStart();
    NrrDraw2D(c, v, n);
    ClockStop(which, t0);
}

void NrrSelftestHotDrawXf(NrrCtx* c, const void* v, const uint16_t* idx, uint32_t n, const float* unif, int which) {
    const uint64_t t0 = ClockStart();
    NrrDrawXf(c, v, 16, idx, n, unif);
    ClockStop(which, t0);
}

void NrrSelftestHotPair() {
    const uint64_t t0 = ClockStart();
    ClockStop(kClkPair, t0);
}

/* ------------------------------------------------------------------- 2D quads ---- */

void NrrSelftestHotVertex(float* v, float x, float y, float u, float t, const float rgba[4]) {
    v[0] = x;
    v[1] = y;
    v[2] = 0.5f; /* z in [0, w] (GetClipParameters) */
    v[3] = 1.0f;
    v[4] = u;
    v[5] = t;
    v[6] = rgba[0];
    v[7] = rgba[1];
    v[8] = rgba[2];
    v[9] = rgba[3];
    v[10] = u;
    v[11] = t;
}

/* A quad over clip x [x0, x1], y [-0.92, -0.79] (the HUD band; clip x in [-1,1] maps the 4:3 area
 * as the interpreter does; the exact pixels do not matter for a self test). */
float* NrrSelftestHotQuad(NrrCtx* c, float x0, float x1, const float rgba[4]) {
    float* v = static_cast<float*>(NrrRingAlloc(c, 6u * GDX3DS_VBOPACK_STRIDE * sizeof(float)));
    if (v == nullptr) {
        return nullptr;
    }
    const float y0 = -0.92f, y1 = -0.79f;
    NrrSelftestHotVertex(v + 0 * GDX3DS_VBOPACK_STRIDE, x0, y0, 0.0f, 0.0f, rgba);
    NrrSelftestHotVertex(v + 1 * GDX3DS_VBOPACK_STRIDE, x1, y0, 1.0f, 0.0f, rgba);
    NrrSelftestHotVertex(v + 2 * GDX3DS_VBOPACK_STRIDE, x1, y1, 1.0f, 1.0f, rgba);
    NrrSelftestHotVertex(v + 3 * GDX3DS_VBOPACK_STRIDE, x0, y0, 0.0f, 0.0f, rgba);
    NrrSelftestHotVertex(v + 4 * GDX3DS_VBOPACK_STRIDE, x1, y1, 1.0f, 1.0f, rgba);
    NrrSelftestHotVertex(v + 5 * GDX3DS_VBOPACK_STRIDE, x0, y1, 0.0f, 1.0f, rgba);
    return v;
}

int NrrSelftestHotSolid(NrrCtx* c, float pulse) {
    const NrrState* st = sT.st[kStSolid];
    const float rgba[4] = { 1.0f, pulse, 1.0f - pulse, 1.0f };
    float* v = NrrSelftestHotQuad(c, -0.12f, -0.04f, rgba);
    float* w = NrrSelftestHotQuad(c, -0.03f, 0.03f, rgba);
    if (v == nullptr || w == nullptr) {
        return 3;
    }
    NrrSelftestHotApply(c, st);
    const float in[1][4] = { { rgba[0], rgba[1], rgba[2], rgba[3] } };
    NrrSelftestHotInputs(c, st, in, 1);
    NrrSelftestHotDraw2D(c, v, 6, kClkDraw2D);
    NrrSelftestHotDraw2D(c, w, 6, kClkDraw2DSame); /* same state, new ring vertices: the steady per-draw cost */
    return 0;
}

int NrrSelftestHotTextured(NrrCtx* c, float pulse) {
    const NrrState* st = sT.st[kStTex];
    if (st == nullptr) {
        return 4;
    }
    const float in[2][4] = { { 1.0f, 1.0f - pulse, 0.2f, 1.0f }, { 0.1f, 0.2f, pulse, 1.0f } };
    const int vin = NrrStateVtxColorInput(st);
    const float* rgba = vin >= 1 && vin <= 2 ? in[vin - 1] : in[0];
    float* v = NrrSelftestHotQuad(c, 0.04f, 0.12f, rgba);
    if (v == nullptr) {
        return 3;
    }
    NrrSelftestHotApply(c, st);
    NrrSelftestHotInputs(c, st, in, 2);
    NrrSelftestHotDraw2D(c, v, 6, kClkDraw2D);
    return 0;
}

/* ----------------------------------------------------------------- gpuxf quads ---- */

/* The 28-vec4 block (Gdx3dsXfDraw order, shader.v.pica): clip = (sx*ob.x + ox, sy*ob.y + oy, 0.5, 1),
 * conv = identity, no fog / light / texgen, colour = the constant (xsel 0, xseli 1). */
void NrrSelftestHotXfBlock(float* u, float ox, const float rgba[4]) {
    std::memset(u, 0, 28u * 4u * sizeof(float));
    u[0 * 4 + 0] = 0.0008f; /* xmvp[0] */
    u[0 * 4 + 3] = ox;
    u[1 * 4 + 1] = 0.0013f; /* xmvp[1] */
    u[1 * 4 + 3] = -0.92f;
    u[2 * 4 + 3] = 0.5f;    /* xmvp[2] */
    u[3 * 4 + 3] = 1.0f;    /* xmvp[3] */
    for (int i = 0; i < 4; i++) {
        u[4 * 4 + i] = 1.0f; /* xconv */
    }
    u[6 * 4 + 3] = 1.0f;     /* xfog: fog off */
    u[8 * 4 + 3] = 1.0f;     /* xunlit */
    u[17 * 4 + 3] = 1.0f;    /* xtg: texgen off */
    for (int i = 0; i < 4; i++) {
        u[23 * 4 + i] = rgba[i]; /* xcol */
    }
    u[25 * 4 + 0] = 1.0f;    /* xseli: colour and alpha from the constant */
    u[25 * 4 + 1] = 1.0f;
}

int NrrSelftestHotXf(NrrCtx* c, float pulse) {
    const NrrState* st = sT.st[kStXf];
    if (st == nullptr) {
        return 5;
    }
    XfVtx* v = static_cast<XfVtx*>(NrrRingAlloc(c, 4u * sizeof(XfVtx)));
    uint16_t* idx = static_cast<uint16_t*>(NrrRingAlloc(c, 6u * sizeof(uint16_t)));
    float* u = static_cast<float*>(NrrRingAlloc(c, 2u * 28u * 4u * sizeof(float)));
    if (v == nullptr || idx == nullptr || u == nullptr) {
        return 3;
    }
    static const int16_t kCorner[4][2] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    for (int i = 0; i < 4; i++) {
        std::memset(&v[i], 0, sizeof(XfVtx));
        v[i].ob[0] = kCorner[i][0];
        v[i].ob[1] = kCorner[i][1];
        v[i].cn[3] = 255;
    }
    static const uint16_t kIdx[6] = { 0, 1, 2, 0, 2, 3 };
    std::memcpy(idx, kIdx, sizeof(kIdx));
    const float rgba[4] = { pulse, 1.0f, 0.3f, 1.0f };
    NrrSelftestHotXfBlock(u, 0.16f, rgba);
    NrrSelftestHotXfBlock(u + 112, 0.28f, rgba); /* only xmvp[0] differs */
    NrrSelftestHotApply(c, st);
    NrrSelftestHotDrawXf(c, v, idx, 6, u, kClkDrawXf);
    NrrSelftestHotDrawXf(c, v, idx, 6, u + 112, kClkDrawXfNext); /* steady: one vec4 changed */
    return 0;
}

int NrrSelftestHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    const SelfSnap* snap = static_cast<const SelfSnap*>(NrrSnapGet(f, NRR_CAT_SELFTEST, view));
    if (snap == nullptr) {
        return 1;
    }
    if (!EnsureStates()) {
        return 2;
    }
    const float pulse = static_cast<float>(snap->tick & 63u) / 63.0f;
    int rc = NrrSelftestHotSolid(c, pulse);
    if (rc == 0) {
        rc = NrrSelftestHotTextured(c, pulse);
    }
    if (rc == 0) {
        rc = NrrSelftestHotXf(c, pulse);
    }
    if (sT.clock) {
        NrrSelftestHotPair();
        if (++sT.frames >= 64) {
            EmitClockWindow();
        }
    }
    return rc;
}

} // namespace

extern "C" void gdx3ds_nrr_cat_init_selftest(void) {
    std::memset(&sT, 0, sizeof(sT));
    sT.texOk = MakeTexture();
    sT.clock = gdx3ds_native_raw("nrr_selftest_clock", 0);
    gdx3ds_nrr_set_draw(NRR_CAT_SELFTEST, NrrSelftestHotDraw);
    (void)gdx3ds_nrr_register(NRR_CAT_SELFTEST, NRR_SEC(EFX), SelftestSnap);
}
