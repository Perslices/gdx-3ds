/* port/3ds/gfx/gdx3ds_replay.cpp -- C6 pose-replay, "60 Hz motion" (contract in
 * gdx3ds_replay.h; design, gates and hardware notes in docs/research/native/c6replay-progress.md).
 *
 * One frame table, rebuilt by every rendered frame A and consumed by at most one replay B
 * before the next rendered frame starts (the 1:1 cap), so nothing is double-buffered:
 *   ops      every matrix op of A's walk (G_MTX: pool slot or constant; POPMTX; task RESET)
 *   slots    the GfxPool offsets A's matrices came from, with A's raw words, both pools' words
 *            at the start of the skipped tick and B's words at its end
 *   entries  one per (matrix state, x scale) a draw used: MP_A and the modelview's 3x3, and at
 *            replay MP_B / 3x3_B from re-running the ops on B's slots
 *   sites    one per draw that emitted a pose uniform: which words, A's values, which tag
 *   queue    A's GX queue entries, copied right after C3D_FrameEnd
 * Everything runs on the thread that owns the C3D frame (main; the lever refuses the render
 * thread). */
#include "gdx3ds_replay.h"
#include "gdx3ds_hwprof.h" /* hwprof [hwsite] rplrec: the recording tax of replay=1 ([debug] hwsite) */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_replay_math.h"

using namespace GdxReplayMath;

extern "C" {
int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
int gdx3ds_config_loaded(void) __attribute__((weak));
void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
void gdx3ds_replay_game_capture(GdxReplayGame* out) __attribute__((weak));
uint32_t gdx_interp_cut_epoch(void) __attribute__((weak));
int gdx3ds_c5tick_hollow_pending(void) __attribute__((weak));
int gdx3ds_frameskip_replay_plan(int* skipRun) __attribute__((weak));
/* gfx_citro3d.cpp: the main render target's colour buffer (RGBA8, tiled, 240x400 portrait). */
void* gdx3ds_replay_backend_colorbuf(void);
/* citro3d internals (1.7.1): the context object and the uniform re-dirty the probe also links. */
extern char __C3D_Context[];
void C3Di_DirtyUniforms(GPU_SHADER_TYPE type);
}

int gdx3ds_replay_on = 0;
int gdx3ds_replay_rec = 0;

namespace {

/* ---- sizes (one frame; ~200 KB, allocated once when the lever is on) ---- */
constexpr int kMaxOps = 1024;
constexpr int kMaxConst = 192;
constexpr int kMaxSlots = 160;
constexpr int kMaxEntries = 512;
constexpr int kMaxSites = 768;
constexpr int kMaxEmits = 3072;
constexpr int kMaxVals = 12288;
constexpr int kMaxResets = 4;
constexpr int kMaxQueue = 32;
constexpr int kMtxStack = 11;
constexpr unsigned kWindow = 256;

/* The interpreter's G_MTX parameter bits (gdx_gbi.h; push already XORed back). */
constexpr unsigned kMtxPush = 0x01;
constexpr unsigned kMtxLoad = 0x02;
constexpr unsigned kMtxProjection = 0x04;

/* The gpuxf pose vec4s, relative to xmvp (gfx_citro3d.cpp kXfUniforms): xmvp 0-3, xldir 9-11,
 * xlook 15-16, xflx 26, xoff 27. */
constexpr int kXfPoseCount = 11;
constexpr uint8_t kXfPoseVec[kXfPoseCount] = { 0, 1, 2, 3, 9, 10, 11, 15, 16, 26, 27 };
constexpr int kXfUniformVec4s = 28;

enum OpKind : uint8_t { OP_MTX = 1, OP_POP = 2, OP_RESET = 3 };

struct Op {
    uint8_t kind;
    uint8_t params;
    int16_t slot; /* OP_MTX from a pool: slot index; else -1 */
    int16_t cnst; /* OP_MTX constant: index into sConst */
    uint16_t arg; /* OP_POP count, OP_RESET index */
};

struct Slot {
    uint32_t off;
    int32_t a[16];        /* A's words, host order (what SpMatrix converted) */
    int32_t start[2][16]; /* both pools at the start of the skipped tick */
    int32_t b[16];        /* B's pool at the end of the skipped tick */
    uint8_t fresh;
};

struct ResetState {
    float p[4][4];
    float mv[kMtxStack][4][4];
    float mp[4][4]; /* the interpreter's MP as the task starts (only ops recompute it) */
    int size;
};

struct Entry {
    uint32_t opCount;
    float xscale;
    float mpA[4][4];
    float mv3A[3][3];
    float mpB[4][4];
    float mv3B[3][3];
};

enum SiteKind : uint8_t { SITE_LEGACY = 1, SITE_XF = 2 };

struct Site {
    uint16_t tag;
    uint8_t kind;
    uint8_t nEmit;
    uint16_t emit0;
    uint16_t cat; /* the c5cat scene category the draw was recorded in (diagnostics) */
    uint32_t val0; /* legacy: 16 floats (rows); xf: kXfPoseCount * 4 floats (x y z w each) */
};

struct Emit {
    uint32_t* ptr;  /* word c[0] of the vec4 */
    uint8_t vec;    /* legacy: row 0-3; xf: pose index 0..kXfPoseCount-1 */
    uint8_t split;  /* 1: c[1..3] start at ptr + 2 (the vec4 began at a packet's param0) */
};

struct QEntry {
    uint32_t d[8];
};

/* Why a rendered frame cannot be a replay source. */
enum Bad : uint8_t {
    BAD_NONE = 0,
    BAD_UNTAGGED,
    BAD_OVERFLOW,
    BAD_MPDRIFT,
    BAD_PARTIAL,
    BAD_XFMP,
    BAD_STEREO,
    BAD_READBACK,
    BAD_GXCMD,
    BAD_NODRAWS,
    BAD_SLOTDIFF,
    BAD_SCAN,
    BAD_TEXBACKED,
    BAD_NOTASK,
    BAD_COUNT
};
const char* const kBadName[BAD_COUNT] = { "none", "untagged", "overflow", "mpdrift", "partial", "xfmp",   "stereo",
                                          "readback", "gxcmd", "nodraws", "slotdiff", "scan", "texbacked", "notask" };

/* Why a skipped tick was not replayed. */
enum Cut : uint8_t {
    CUT_NONE = 0,
    CUT_ABAD,
    CUT_NOB,
    CUT_CADENCE,
    CUT_FENCE,
    CUT_PLAYERS,
    CUT_PAUSE,
    CUT_TRANS,
    CUT_MODE,
    CUT_MENU,
    CUT_START,
    CUT_HOOK,
    CUT_APT,
    CUT_RACERS,
    CUT_RACEEND,
    CUT_LOD,
    CUT_VIS,
    CUT_CAMERA,
    CUT_POOL,
    CUT_MATH,
    CUT_UNARMED,
    CUT_COUNT
};
const char* const kCutName[CUT_COUNT] = { "none",  "abad",   "nob",  "cadence", "fence",   "players", "pause",
                                          "trans", "mode",   "menu", "start",   "hook",    "apt",     "racers",
                                          "raceend", "lod", "vis",  "camera",  "pool",    "math",
                                          "unarmed" };

/* ---- config ---- */
int sLatched = 0;
int sVerify = 0;
int sLodCut = 1;
int sVisCut = 1;
float sCutDist = 1500.0f;
float sCutCos = 0.866f; /* 30 degrees */
int sMarkersOn = 0;
int sDiag = 0;              /* [debug] replay_diag=N: one line per site for N replays */
int sNoPatch = 0;           /* [debug] replay_verify_nopatch: replay A unpatched (the no-replay baseline) */
int sDumpLeft = 0;          /* [debug] replay_verify_dump: raw image pairs still to write */
unsigned long sDumpFrom = 0; /* [debug] replay_verify_dump_from: first host frame to dump */

/* ---- the frame table ---- */
struct Table {
    Op ops[kMaxOps];
    float cnst[kMaxConst][4][4];
    const void* cnstSrc[kMaxConst];  /* where a non-pool matrix was read from */
    int32_t cnstRaw[kMaxConst][16];  /* its raw bytes then */
    Slot slots[kMaxSlots];
    ResetState resets[kMaxResets];
    Entry entries[kMaxEntries];
    Site sites[kMaxSites];
    Emit emits[kMaxEmits];
    float vals[kMaxVals];
    QEntry queue[kMaxQueue];
};
Table* sT = nullptr;
int sNOps = 0, sNConst = 0, sNSlots = 0, sNResets = 0, sNEntries = 0, sNSites = 0, sNEmits = 0, sNVals = 0;
int sNQueue = 0;
uint8_t sBad = BAD_NONE;      /* first reason the frame being built cannot be replayed */
int sTableValid = 0;          /* the table describes the last rendered frame, complete */
int sTableTasks = 0;          /* tasks walked in the frame being built */
int sFrameOpen = 0;           /* between frame_begin and frame_end_post */
int sNeedReset = 0;           /* reset at the first task (a verified replay reads the table first) */
int sAPool = -1;              /* the pool A's matrices came from */
unsigned sAliasHits = 0, sDirectHits = 0; /* [diag] how PoolOf resolved A's matrices */
void* sBackBufA = nullptr;    /* the top back buffer A's transfer targeted */
const float* sInterpMp = nullptr;

/* shadow matrix state (the interpreter's, recomputed from the op log) */
float sP[4][4];
float sMv[kMtxStack][4][4];
int sMvSize = 1;
float sMp[4][4];

/* per-draw state */
uint16_t sDrawTag = GDX_REPLAY_TAG_UNKNOWN;
uint16_t sProjTag = 0xFFFF;
uint16_t sXfTag = 0xFFFF;
float sMeshXscale = 1.0f;
float sMeshMp[4][4];
int sInSky = 0;
unsigned sScene = 0;     /* the last c5cat scene marker's category */
/* [diag] per-draw governing site in A, and the verified replay's effective pose per draw */
constexpr int kMaxDraws = 1024;
struct DrawGov {
    int16_t site;
    uint8_t xf;
    uint8_t cat;
    uint16_t tag;
    float p[4]; /* the first packed vertex */
    const uint32_t* rp; /* the GPU vertex (+index) range and its checksum at record */
    uint32_t rn, rsum;
};
DrawGov sDrawGov[kMaxDraws];
float sVtx0[4];
const uint32_t* sRangeP = nullptr;
uint32_t sRangeN = 0;

uint32_t RangeSum(const uint32_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}
/* [diag] G_VTX sources in A: pool offset + count + category */
struct VSrc {
    uint32_t off;
    uint16_t n;
    uint8_t pool;
    uint8_t cat;
};
/* Re-uploaded vertices: every packed vertex A wrote from a GfxPool-sourced slot of a category in
 * [native] replay_revtx. The game rebuilds those Vtx every tick (sky tiles, course strips, effect
 * quads), so no matrix correction can move them; at replay B's Vtx at the same pool offset is
 * T&L'd with A's MP and packing, and the site's K then lands it exactly where B's own frame does. */
struct Revtx {
    float* out;       /* the packed position in A's VBO */
    uint32_t src;     /* pool offset | pool << 30 | big << 31 */
    uint16_t tag;
    uint16_t pad;
    uint8_t before[16]; /* the other pool at this offset as A's walk met it (two ticks back) */
};
constexpr int kMaxRevtx = 12288;
Revtx* sRevtx = nullptr;
int sNRevtx = 0;
int sHash = 0;            /* [debug] replay_hash: one digest of every replay's patched words + re-uploaded vertices */
unsigned long sFrameA = 0; /* the host frame A's iteration ended on */
float sLastAng = 0.0f, sLastDist = 0.0f; /* the last GameCut's camera rotation (deg) and eye move */
unsigned sRevtxMask = 0; /* [native] replay_revtx: c5cat categories re-uploaded (bit = GDX3DS_C5CAT_*) */
constexpr int kMaxVSrc = 2048;
VSrc sVSrc[kMaxVSrc];
int sNVSrc = 0;
int sNDraws = 0;
int sLastProjSite = -1, sLastXfSite = -1;
struct VfyDraw {
    uint8_t xf, cat, valid;
    uint16_t tag;
    float m[16];
    float pred[4]; /* legacy: A's first vertex through the draw's correction */
    float alt[4][4]; /* the same with x scale 1, 0.8, 160/148, 1.25 */
    float xs;
    uint8_t hasPred;
};
VfyDraw* sVfyDraws = nullptr;
int sVfyDrawN = 0;
int sFDraw = 0;             /* F's draw index */
float sFProj[16], sFXf[16]; /* F's effective uniforms */
unsigned sSkipCat = 0;   /* [debug] replay_skip_cat: categories (bit = GDX3DS_C5CAT_*) left unpatched */
int sDeep = 0;           /* [debug] replay_deep: the per-draw / per-G_VTX census (costs CPU) */
unsigned sSkipTagHi = 0; /* [debug] replay_skip_tag_hi: skip the range skip_tag..skip_tag_hi */
unsigned sSkipTag = 0;   /* [debug] replay_skip_tag: one tag left unpatched */
unsigned sSkipCatXf = 0; /* [debug] replay_skip_cat_xf: the same, gpuxf sites only */
int sProjLoc = -1;
int sXfLoc = -1;

/* ---- game capture / cadence ---- */
GdxReplayGame sGameWalk; /* at A's first task (pool geometry, A's pool) */
GdxReplayGame sGameA;    /* end of A's iteration */
GdxReplayGame sGameWalkA; /* sGameWalk as A's iteration ended (the tick A drew) */
GdxReplayGame sGameB;    /* end of the skipped tick */
/* The tick a replay presents is the one built two iterations before the next render (the last
 * list the slot dropped): A's own iteration at D = 2, the first skip at D = 3. Its end-of-
 * iteration capture and whether its build kept what the replay reads (c5tick's hollow). */
struct IterEnd {
    GdxReplayGame game;
    int wanted;
};
IterEnd sEnd[2];      /* [0] the last iteration's end, [1] the one before */
GdxReplayGame sGameBTick; /* the presented tick's state (sEnd[1] as the last skip ended) */
int sBWanted = 0;     /* its build kept the replay's sections */
int sHaveA = 0;
int sArmIter = 0;     /* this iteration's frame may be a replay source (gdx3ds_replay_iteration_begin) */
int sWantIter = 0;    /* this iteration builds the tick a replay will present */
int sArmedA = 0;      /* the last rendered frame was recorded (its table is A) */
uint8_t sArmCutIter = CUT_UNARMED; /* why this iteration's frame is not recorded */
uint8_t sArmedCut = CUT_UNARMED;   /* ... and the last rendered frame's (its slot's cut reason) */
int sMaxSkips = 2;    /* [native] replay_dmax - 1: the most skipped ticks a slot may replay across */
int sBSamePool = 0;   /* the presented tick wrote A's own pool (an even skip count) */
int sHaveB = 0;
int sSkipsSinceA = 0;
uint32_t sEpochA = 0, sEpochB = 0;
unsigned sAptCount = 0, sAptA = 0, sAptB = 0;
int sBPool = -1;

/* ---- swap fence ---- */
void (*sOrigCb)(gxCmdQueue_s*) = nullptr;
int sCbInstalled = 0;
volatile int sReplaySwapPending = 0;
volatile int sFramePresentPending = 0;
volatile u32 sLastSwapCounter = 0;
volatile int sLastSwapWasReplay = 0;
volatile int sSwapLatchKnown = 0;

/* ---- verify lane ---- */
int sVerifyPending = 0; /* the governor's SKIP was turned into this render: replay it first */
int sVerifyDone = 0;    /* the verified replay ran for the frame being rendered */
uint32_t* sVfyR = nullptr; /* replay image (linear RGBA8 240x400) */
uint32_t* sVfyF = nullptr; /* full render */
uint32_t* sVfyA = nullptr; /* the source frame A (dump only) */
struct VfyEntry {
    uint32_t opCount;
    float xscale;
    float mp[4][4];
};
VfyEntry* sVfy = nullptr;
int sVfyN = 0;
/* the slot words the verified replay used (B's, or A's for a stale slot), by pool offset */
struct VfySlot {
    uint32_t off;
    int32_t w[16];
    uint8_t fresh;
};
VfySlot* sVfySlots = nullptr;
int sVfySlotN = 0;

/* ---- receipts ---- */
struct Win {
    unsigned iters, renders, skips, slots, replays, recorded;
    unsigned cuts[CUT_COUNT];
    unsigned bad[BAD_COUNT];
    unsigned fenceWaits;
    uint64_t pTicks, pMax, patchTicks, submitTicks;
    unsigned sites, entries, ops, stale, slotsN, tableFrames;
    float dEyeMax, angMax;
    /* verify */
    unsigned vFrames, vAligned, vMisaligned, vEntries, vExact;
    unsigned vSlotSame, vSlotDiff, vSlotStaleDiff, vSlotNew;
    unsigned constChanged, constTotal;
    unsigned meshMpN, meshMpMis;
    unsigned govN, govMis;
    unsigned dN, dMis;
    unsigned lN, lMis;
    unsigned vsPool[16], vsDiff[16], vsOther[16];
    unsigned rgN[16], rgBad[16];
    unsigned rvN, rvUp, rvStale, rvOver;
    unsigned vfyHollow;
    unsigned angH[6]; /* camera rotation per decided slot: <0.5 <1 <2 <3 <5 >=5 deg */
    float lMax;
    float dMax;
    float vMaxRel;
    uint64_t vPx, vPxDiff, vPxOver;
    unsigned vMaxDelta;
    uint64_t vSumDelta;
};
Win sWin;
uint64_t sWinT0 = 0;
unsigned long sFrameNum = 0;

void Log(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(buf, len);
    }
}

extern "C" void gdx3ds_replay_arm_interp(int on);

int ConfigInt(const char* section, const char* key, int fallback) {
    if (&gdx3ds_config_get_int == nullptr) {
        return fallback;
    }
    return gdx3ds_config_get_int(section, key, fallback);
}

gxCmdQueue_s* FrameQueue() {
    return reinterpret_cast<gxCmdQueue_s*>(__C3D_Context);
}

/* citro3d 1.7.1 C3D_Context prefix (source/internal.h): the GX queue, the command buffer, and
 * the dirty flags. The layout is checked before the lever arms. */
struct C3dPrefix {
    gxCmdQueue_s gxQueue;
    u32* cmdBuf;
    size_t cmdBufSize;
    float cmdBufUsage;
    u32 flags;
};
C3dPrefix* Ctx() {
    return reinterpret_cast<C3dPrefix*>(__C3D_Context);
}

/* C3DiF_* (citro3d internal.h): everything a frame's first draw must re-emit so the frame's
 * command list is self-contained (AttrInfo, BufInfo, Effect, FrameBuf, Viewport, Scissor,
 * Program, TexEnvBuf, TexStatus, FogLut, TexAll, TexEnvAll). */
constexpr u32 kDirtyAll = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
                          (1u << 9) | (1u << 14) | (1u << 17) | (7u << 23) | (0x3Fu << 26);

void MarkBad(uint8_t why) {
    if (sBad == BAD_NONE) {
        sBad = why;
    }
}

void ResetTable() {
    sNOps = sNConst = sNSlots = sNResets = sNEntries = sNSites = sNEmits = sNVals = 0;
    sNQueue = 0;
    sBad = BAD_NONE;
    sTableValid = 0;
    sTableTasks = 0;
    sProjTag = 0xFFFF;
    sXfTag = 0xFFFF;
    sDrawTag = GDX_REPLAY_TAG_UNKNOWN;
    sInSky = 0;
    sScene = 0;
    sNDraws = 0;
    sNVSrc = 0;
    sNRevtx = 0;
    for (int i = 0; i < GDX_REPLAY_VTX_SLOTS; i++) {
        gdx3ds_replay_vtx_src[i] = GDX_REPLAY_SRC_NONE;
    }
    sLastProjSite = sLastXfSite = -1;
    sFDraw = 0;
    sAPool = -1;
    for (int i = 0; i < GDX_REPLAY_VTX_SLOTS; i++) {
        gdx3ds_replay_vtx_tag[i] = GDX_REPLAY_TAG_UNKNOWN;
    }
}

/* ---- matrix state (SpMatrix / SpPopMatrix, gdx_rsp.cpp, float for float) ---- */

void ApplyMtx(float p[4][4], float mv[kMtxStack][4][4], int* size, float mp[4][4], unsigned params,
              const float m[4][4]) {
    if (params & kMtxProjection) {
        if (params & kMtxLoad) {
            memcpy(p, m, sizeof(float) * 16);
        } else {
            MulF(p, m, p);
        }
    } else {
        if ((params & kMtxPush) && *size < kMtxStack) {
            ++*size;
            memcpy(mv[*size - 1], mv[*size - 2], sizeof(float) * 16);
        }
        if (params & kMtxLoad) {
            if (*size == 0) {
                ++*size;
            }
            memcpy(mv[*size - 1], m, sizeof(float) * 16);
        } else {
            MulF(mv[*size - 1], m, mv[*size - 1]);
        }
    }
    MulF(mp, mv[*size - 1], p);
}

void ApplyPop(float p[4][4], float mv[kMtxStack][4][4], int* size, float mp[4][4], unsigned count) {
    while (count--) {
        if (*size > 0) {
            --*size;
            if (*size > 0) {
                MulF(mp, mv[*size - 1], p);
            }
        }
    }
}

void Mv3Of(const float mv[kMtxStack][4][4], int size, float out[3][3]) {
    const int top = size > 0 ? size - 1 : 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = mv[top][i][j];
        }
    }
}

/* Pool offset of a matrix source, or -1 for a constant. */
int PoolOf(const void* src, uint32_t* off) {
    const unsigned char* s = static_cast<const unsigned char*>(src);
    const GdxReplayGame& g = sGameWalk;
    if (g.poolBase == nullptr || g.poolSize == 0) {
        return -1;
    }
    if (s >= g.poolBase && s + 64 <= g.poolBase + 2u * g.poolSize) {
        sDirectHits++;
        const uint32_t rel = (uint32_t)(s - g.poolBase);
        *off = rel % g.poolSize;
        return (int)(rel / g.poolSize);
    }
    if (g.aliasBase != nullptr && s >= g.aliasBase && s + 64 <= g.aliasBase + g.poolSize) {
        sAliasHits++;
        *off = (uint32_t)(s - g.aliasBase);
        return g.curPool;
    }
    return -1;
}

int FindOrAddSlot(uint32_t off, const int32_t* words) {
    for (int i = 0; i < sNSlots; i++) {
        if (sT->slots[i].off == off) {
            if (memcmp(sT->slots[i].a, words, 64) != 0) {
                MarkBad(BAD_SLOTDIFF);
            }
            return i;
        }
    }
    if (sNSlots >= kMaxSlots) {
        MarkBad(BAD_OVERFLOW);
        return -1;
    }
    Slot& s = sT->slots[sNSlots];
    s.off = off;
    memcpy(s.a, words, 64);
    s.fresh = 0;
    /* Both pools as A's walk meets the slot. The walk runs BEFORE its iteration's logic tick
     * (it draws the previous tick's list), so the other pool still holds the value from two
     * ticks back: B's tick rewrote the slot iff B's pool differs from this at replay time. */
    const GdxReplayGame& g = sGameWalk;
    for (int p = 0; p < 2; p++) {
        if (g.poolBase != nullptr && off + 64u <= g.poolSize) {
            memcpy(s.start[p], g.poolBase + (size_t)p * g.poolSize + off, 64);
        } else {
            memcpy(s.start[p], words, 64);
        }
    }
    return sNSlots++;
}

bool Recording() {
    return gdx3ds_replay_rec && sT != nullptr && sFrameOpen && !sVerifyPending && sBad == BAD_NONE;
}

/* ---- sites ---- */

struct Found {
    uint32_t* w[4];
    uint8_t have; /* bit c = word c seen */
};

void FoundAdd(Found* f, const UnifWord& u) {
    f->w[u.comp] = u.ptr;
    f->have |= (uint8_t)(1u << u.comp);
}

bool AddEmit(const Found& f, uint8_t vec) {
    if (sNEmits >= kMaxEmits) {
        MarkBad(BAD_OVERFLOW);
        return false;
    }
    Emit& e = sT->emits[sNEmits++];
    e.ptr = f.w[0];
    e.vec = vec;
    e.split = (f.w[1] != f.w[0] + 1) ? 1 : 0;
    if (e.split && (f.w[1] != f.w[0] + 2 || f.w[2] != f.w[0] + 3 || f.w[3] != f.w[0] + 4)) {
        MarkBad(BAD_SCAN);
        return false;
    }
    if (!e.split && (f.w[2] != f.w[0] + 2 || f.w[3] != f.w[0] + 3)) {
        MarkBad(BAD_SCAN);
        return false;
    }
    return true;
}

Site* NewSite(uint16_t tag, uint8_t kind, int nVals) {
    if (sNSites >= kMaxSites || sNVals + nVals > kMaxVals) {
        MarkBad(BAD_OVERFLOW);
        return nullptr;
    }
    Site& s = sT->sites[sNSites++];
    s.tag = tag;
    s.kind = kind;
    s.cat = (uint16_t)sScene;
    s.nEmit = 0;
    s.emit0 = (uint16_t)sNEmits;
    s.val0 = (uint32_t)sNVals;
    sNVals += nVals;
    return &s;
}

void RecordLegacy(uint16_t tag, const Found proj[4]) {
    for (int r = 0; r < 4; r++) {
        if (proj[r].have != 0xF) {
            MarkBad(BAD_PARTIAL);
            return;
        }
    }
    Site* s = NewSite(tag, SITE_LEGACY, 16);
    if (s == nullptr) {
        return;
    }
    float* v = &sT->vals[s->val0];
    for (int r = 0; r < 4; r++) {
        for (int j = 0; j < 4; j++) {
            v[r * 4 + j] = BitsFloat(*proj[r].w[3 - j]); /* c[0..3] = w z y x */
        }
        if (!AddEmit(proj[r], (uint8_t)r)) {
            return;
        }
        s->nEmit++;
    }
}

/* The xf draw's G rows 0..2 must be the tag's MP rows 0..2 (machines: G = MP; the track's
 * rebased G = T(E) * MP): proves the tag names the matrix the uniform was built from. */
bool XfMatchesEntry(uint16_t tag, const float* shadow) {
    if (tag < GDX_REPLAY_TAG_FIRST || (int)(tag - GDX_REPLAY_TAG_FIRST) >= sNEntries) {
        return false;
    }
    const Entry& e = sT->entries[tag - GDX_REPLAY_TAG_FIRST];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            if (shadow[i * 4 + j] != e.mpA[j][i]) {
                return false;
            }
        }
    }
    return true;
}

void RecordXf(uint16_t tag, const Found pose[kXfPoseCount], const float* shadow) {
    int any = 0;
    for (int k = 0; k < kXfPoseCount; k++) {
        if (pose[k].have != 0 && pose[k].have != 0xF) {
            MarkBad(BAD_PARTIAL);
            return;
        }
        any |= pose[k].have;
    }
    if (!any) {
        return;
    }
    if (tag == GDX_REPLAY_TAG_SCREEN || tag == GDX_REPLAY_TAG_SKY_SCREEN || !XfMatchesEntry(tag, shadow)) {
        MarkBad(BAD_XFMP);
        return;
    }
    Site* s = NewSite(tag, SITE_XF, kXfPoseCount * 4);
    if (s == nullptr) {
        return;
    }
    float* v = &sT->vals[s->val0];
    for (int k = 0; k < kXfPoseCount; k++) {
        memcpy(v + k * 4, shadow + kXfPoseVec[k] * 4, 4 * sizeof(float));
        if (pose[k].have == 0xF) {
            if (!AddEmit(pose[k], (uint8_t)k)) {
                return;
            }
            s->nEmit++;
        }
    }
}

int XfPoseIndex(int rel) {
    for (int k = 0; k < kXfPoseCount; k++) {
        if (kXfPoseVec[k] == rel) {
            return k;
        }
    }
    return -1;
}

/* ---- the swap callback ---- */

void QueueFinishWrap(gxCmdQueue_s* q) {
    if (sReplaySwapPending) {
        sReplaySwapPending = 0;
        gfxScreenSwapBuffers(GFX_TOP, false);
        sLastSwapCounter = C3D_FrameCounter(0);
        sLastSwapWasReplay = 1;
        sSwapLatchKnown = 1;
    }
    if (sOrigCb != nullptr) {
        sOrigCb(q);
    }
    if (sFramePresentPending) {
        /* citro3d's onQueueFinish just swapped the frame's top buffer (needSwapTop). */
        sFramePresentPending = 0;
        sLastSwapCounter = C3D_FrameCounter(0);
        sLastSwapWasReplay = 0;
        sSwapLatchKnown = 1;
    }
}

void InstallCallback() {
    if (sCbInstalled) {
        return;
    }
    gxCmdQueue_s* q = FrameQueue();
    sOrigCb = q->callback;
    q->callback = QueueFinishWrap;
    sCbInstalled = 1;
}

/* 1 when a vblank has passed since the latest top swap request (it is on screen). */
bool LastSwapLatched() {
    if (!sSwapLatchKnown) {
        return false;
    }
    return (s32)(C3D_FrameCounter(0) - sLastSwapCounter) > 0;
}

/* ---- the replay itself ---- */

const int32_t* SlotWordsForB(const Slot& s) {
    return s.fresh ? s.b : s.a;
}

/* Re-run A's ops with B's slot words: every entry's MP_B / 3x3_B. */
bool ComputeB() {
    float p[4][4], mp[4][4];
    float mv[kMtxStack][4][4];
    int size = 1;
    memset(p, 0, sizeof(p));
    memset(mv, 0, sizeof(mv));
    memset(mp, 0, sizeof(mp));
    int e = 0;
    for (int k = 0; k <= sNOps; k++) {
        while (e < sNEntries && sT->entries[e].opCount == (uint32_t)k) {
            memcpy(sT->entries[e].mpB, mp, sizeof(mp));
            Mv3Of(mv, size, sT->entries[e].mv3B);
            e++;
        }
        if (k == sNOps) {
            break;
        }
        const Op& op = sT->ops[k];
        if (op.kind == OP_RESET) {
            const ResetState& r = sT->resets[op.arg];
            memcpy(p, r.p, sizeof(p));
            memcpy(mv, r.mv, sizeof(mv));
            size = r.size;
            memcpy(mp, r.mp, sizeof(mp));
        } else if (op.kind == OP_POP) {
            ApplyPop(p, mv, &size, mp, op.arg);
        } else if (op.slot >= 0) {
            float m[4][4];
            MtxFromWords(SlotWordsForB(sT->slots[op.slot]), m);
            ApplyMtx(p, mv, &size, mp, op.params, m);
        } else {
            ApplyMtx(p, mv, &size, mp, op.params, sT->cnst[op.cnst]);
        }
    }
    return e == sNEntries;
}

/* The star/sprite scroll patch for screen-space draws inside the sky: points at infinity map
 * A -> B through the rotation homography of the camera (rows 0-2 of MP act on directions). */
bool SkyKp(const Entry& e, double kp[4][4]) {
    double n[3][3];
    const int cols[3] = { 0, 1, 3 };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            n[r][c] = e.mpA[r][cols[c]];
        }
    }
    const double det = n[0][0] * (n[1][1] * n[2][2] - n[1][2] * n[2][1]) - n[0][1] * (n[1][0] * n[2][2] - n[1][2] * n[2][0]) +
                       n[0][2] * (n[1][0] * n[2][1] - n[1][1] * n[2][0]);
    if (fabs(det) < 1e-30) {
        return false;
    }
    double q[3][3];
    q[0][0] = (n[1][1] * n[2][2] - n[1][2] * n[2][1]) / det;
    q[0][1] = (n[0][2] * n[2][1] - n[0][1] * n[2][2]) / det;
    q[0][2] = (n[0][1] * n[1][2] - n[0][2] * n[1][1]) / det;
    q[1][0] = (n[1][2] * n[2][0] - n[1][0] * n[2][2]) / det;
    q[1][1] = (n[0][0] * n[2][2] - n[0][2] * n[2][0]) / det;
    q[1][2] = (n[0][2] * n[1][0] - n[0][0] * n[1][2]) / det;
    q[2][0] = (n[1][0] * n[2][1] - n[1][1] * n[2][0]) / det;
    q[2][1] = (n[0][1] * n[2][0] - n[0][0] * n[2][1]) / det;
    q[2][2] = (n[0][0] * n[1][1] - n[0][1] * n[1][0]) / det;
    double c[3][4]; /* rows x, y, w of the input -> clip_B */
    for (int r = 0; r < 3; r++) {
        for (int j = 0; j < 4; j++) {
            c[r][j] = q[r][0] * e.mpB[0][j] + q[r][1] * e.mpB[1][j] + q[r][2] * e.mpB[2][j];
        }
    }
    const double s = e.xscale != 0.0f ? e.xscale : 1.0f;
    memset(kp, 0, sizeof(double) * 16);
    kp[0][0] = c[0][0];
    kp[0][1] = c[0][1] / s;
    kp[0][3] = c[0][3] / s;
    kp[1][0] = s * c[1][0];
    kp[1][1] = c[1][1];
    kp[1][3] = c[1][3];
    kp[2][2] = 1.0;
    kp[3][0] = s * c[2][0];
    kp[3][1] = c[2][1];
    kp[3][3] = c[2][3];
    return true;
}

/* Legacy projection for the sky rect: Proj_B = Proj_A * Kp^T. */
void ApplyKpLegacy(const float projA[16], const double kp[4][4], float projB[16]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double acc = 0.0;
            for (int k = 0; k < 4; k++) {
                acc += (double)projA[i * 4 + k] * kp[j][k];
            }
            projB[i * 4 + j] = (float)acc;
        }
    }
}

void WriteVec(const Emit& e, const float xyzw[4]) {
    e.ptr[0] = FloatBits(xyzw[3]);
    uint32_t* rest = e.ptr + (e.split ? 2 : 1);
    rest[0] = FloatBits(xyzw[2]);
    rest[1] = FloatBits(xyzw[1]);
    rest[2] = FloatBits(xyzw[0]);
}

struct PatchStats {
    uint32_t* lo;
    uint32_t* hi;
    unsigned sites;
    float dEye;
};

void NoteRange(PatchStats* ps, const Emit& e) {
    uint32_t* end = e.ptr + (e.split ? 5 : 4);
    if (ps->lo == nullptr || e.ptr < ps->lo) {
        ps->lo = e.ptr;
    }
    if (ps->hi == nullptr || end > ps->hi) {
        ps->hi = end;
    }
}

bool PatchLegacySite(const Site& s, PatchStats* ps) {
    const bool sky = (s.tag & 0x8000u) != 0;
    const uint16_t tag = (uint16_t)(s.tag & 0x7FFFu);
    if (tag < GDX_REPLAY_TAG_FIRST) {
        return true; /* screen space: A's value is B's */
    }
    const Entry& e = sT->entries[tag - GDX_REPLAY_TAG_FIRST];
    const float* a = &sT->vals[s.val0];
    float b[16];
    if (sky) {
        double kp[4][4];
        if (!SkyKp(e, kp)) {
            return false;
        }
        ApplyKpLegacy(a, kp, b);
    } else {
        double k[4][4];
        if (!Correction(e.mpA, e.mpB, k)) {
            return false;
        }
        float pa[4][4], pb[4][4];
        memcpy(pa, a, sizeof(pa));
        if (!PatchLegacy(pa, k, e.xscale, pb)) {
            return false;
        }
        memcpy(b, pb, sizeof(b));
    }
    for (int i = 0; i < s.nEmit; i++) {
        const Emit& em = sT->emits[s.emit0 + i];
        WriteVec(em, b + em.vec * 4);
        NoteRange(ps, em);
    }
    return true;
}

bool PatchXfSite(const Site& s, PatchStats* ps) {
    const Entry& e = sT->entries[s.tag - GDX_REPLAY_TAG_FIRST];
    const float* a = &sT->vals[s.val0];
    double k[4][4];
    if (!Correction(e.mpA, e.mpB, k)) {
        return false;
    }
    float ua[4][4], ub[4][4];
    memcpy(ua, a, sizeof(ua));
    PatchXfMvp(ua, k, ub);
    float b[kXfPoseCount * 4];
    memcpy(b, a, sizeof(b));
    memcpy(b, ub, sizeof(ub));
    /* xoff (pose index 10) stays A's. A resident track draw's G = T(E_A) * MP_A pairs with
     * xoff = -E_A, and G_A * K = T(E_A) * MP_B is already B's draw for the same xoff: the
     * vertex lands where B's own frame puts it, only rebased about A's integer eye instead of
     * B's (a few units: nothing at float24). Re-basing to B's eye instead (U' = U T(E_B - E_A),
     * xoff' = -E_B) couples a site's xmvp to its xoff, and xmvp and xoff are separate GPU
     * registers that later draws inherit independently: a retained stream draw (xoff = 0) that
     * re-emits only xoff after a resident draw kept the resident draw's re-based xmvp and moved
     * by E_B - E_A -- the wall/road shift track_resident showed under verify (c6replay-progress
     * section 10). Without the re-base every site's xmvp is independent of xoff. */
    /* model-space light / lookat / FLX directions (pose indices 4-9) */
    double r[3][3];
    if (LightRotation(e.mv3A, e.mv3B, r)) {
        for (int kidx = 4; kidx <= 9; kidx++) {
            RotateDir(r, a + kidx * 4, b + kidx * 4);
        }
    }
    for (int i = 0; i < s.nEmit; i++) {
        const Emit& em = sT->emits[s.emit0 + i];
        WriteVec(em, b + em.vec * 4);
        NoteRange(ps, em);
    }
    return true;
}

void DiagSites() {
    for (int i = 0; i < sNSites; i++) {
        const Site& st = sT->sites[i];
        const uint16_t tag = (uint16_t)(st.tag & 0x7FFFu);
        double kd = -1.0;
        unsigned opc = 0;
        if (tag >= GDX_REPLAY_TAG_FIRST) {
            const Entry& e = sT->entries[tag - GDX_REPLAY_TAG_FIRST];
            opc = e.opCount;
            double k[4][4];
            if (Correction(e.mpA, e.mpB, k)) {
                kd = 0.0;
                for (int a = 0; a < 4; a++) {
                    for (int b = 0; b < 4; b++) {
                        const double d = fabs(k[a][b] - (a == b ? 1.0 : 0.0));
                        kd = d > kd ? d : kd;
                    }
                }
            }
        }
        double xdev = -1.0;
        if (st.kind == SITE_XF && tag >= GDX_REPLAY_TAG_FIRST) {
            const Entry& e = sT->entries[tag - GDX_REPLAY_TAG_FIRST];
            double k[4][4];
            if (Correction(e.mpA, e.mpB, k)) {
                float ua[4][4], ub[4][4];
                memcpy(ua, &sT->vals[st.val0], sizeof(ua));
                PatchXfMvp(ua, k, ub);
                xdev = 0.0;
                for (int a = 0; a < 4; a++) {
                    for (int b = 0; b < 3; b++) {
                        const double d = fabs((double)ub[a][b] - (double)e.mpB[b][a]);
                        xdev = d > xdev ? d : xdev;
                    }
                }
            }
        }
        float oa[2] = { 0, 0 }, ob[2] = { 0, 0 };
        if (tag >= GDX_REPLAY_TAG_FIRST) {
            const Entry& e = sT->entries[tag - GDX_REPLAY_TAG_FIRST];
            if (e.mpA[3][3] != 0.0f && e.mpB[3][3] != 0.0f) {
                oa[0] = e.mpA[3][0] / e.mpA[3][3] * 200.0f;
                oa[1] = e.mpA[3][1] / e.mpA[3][3] * 120.0f;
                ob[0] = e.mpB[3][0] / e.mpB[3][3] * 200.0f;
                ob[1] = e.mpB[3][1] / e.mpB[3][3] * 120.0f;
            }
        }
        Log("[replay] diag site=%d tag=0x%04x kind=%u cat=%u emit=%u op=%u kdev=%.4g xdev=%.4g oA=%.0f,%.0f oB=%.0f,%.0f wA=%.0f wB=%.0f",
            i, st.tag, st.kind, st.cat, st.nEmit, opc, kd, xdev, (double)oa[0], (double)oa[1], (double)ob[0], (double)ob[1],
            tag >= GDX_REPLAY_TAG_FIRST ? (double)sT->entries[tag - GDX_REPLAY_TAG_FIRST].mpA[3][3] : 0.0,
            tag >= GDX_REPLAY_TAG_FIRST ? (double)sT->entries[tag - GDX_REPLAY_TAG_FIRST].mpB[3][3] : 0.0);
    }
    for (int i = 0; i < sNOps; i++) {
        const Op& op = sT->ops[i];
        Log("[replay] diag op=%d kind=%u params=%u slot=%d off=0x%x fresh=%u", i, op.kind, op.params, op.slot,
            op.slot >= 0 ? sT->slots[op.slot].off : 0u, op.slot >= 0 ? sT->slots[op.slot].fresh : 0u);
    }
}

bool PatchAll(PatchStats* ps) {
    memset(ps, 0, sizeof(*ps));
    if (sDiag > 0 && sFrameNum >= sDumpFrom) {
        sDiag--;
        DiagSites();
    }
    if (sNoPatch) {
        return true;
    }
    for (int i = 0; i < sNSites; i++) {
        const Site& s = sT->sites[i];
        if ((sSkipTag != 0 && (s.tag & 0x7FFFu) >= sSkipTag && (s.tag & 0x7FFFu) <= (sSkipTagHi > sSkipTag ? sSkipTagHi : sSkipTag)) || ((sSkipCat >> s.cat) & 1u) != 0 || (s.kind == SITE_XF && ((sSkipCatXf >> s.cat) & 1u) != 0)) {
            continue;
        }
        const bool ok = s.kind == SITE_LEGACY ? PatchLegacySite(s, ps) : PatchXfSite(s, ps);
        if (!ok) {
            return false;
        }
        ps->sites++;
    }
    return true;
}

/* After a replay the GPU's uniform registers hold B's values while citro3d's shadow holds A's:
 * make the next frame re-emit every uniform the replay touched. */
void RedirtyPatchedUniforms() {
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
}

void FlushPatched(const PatchStats& ps) {
    if (ps.lo != nullptr && ps.hi > ps.lo) {
        GSPGPU_FlushDataCache(ps.lo, (u32)((ps.hi - ps.lo) * sizeof(u32)));
    }
}

bool IsScreenTransfer(const QEntry& q) {
    return (q.d[0] & 0xFFu) == 0x03u && (void*)(uintptr_t)q.d[2] == sBackBufA;
}

/* Re-add A's queue entries (the frame queue must be stopped and empty). `forVerify`: skip the
 * screen transfer and the mirror copies (the verified replay is compared, never shown). */
void AddQueue(bool forVerify, void* backNow) {
    gxCmdQueue_s* q = FrameQueue();
    for (int i = 0; i < sNQueue; i++) {
        gxCmdEntry_s e;
        memcpy(&e, sT->queue[i].d, sizeof(e));
        const uint32_t id = sT->queue[i].d[0] & 0xFFu;
        if (IsScreenTransfer(sT->queue[i])) {
            if (forVerify) {
                continue;
            }
            e.data[2] = (u32)(uintptr_t)backNow;
        } else if (forVerify && id == 0x04u) {
            continue;
        }
        gxCmdQueueAdd(q, &e);
    }
}

/* ---- cut decision ---- */

bool IsRaceMode(int mode) {
    const int m = mode & 0x3FFF; /* strip the F3D microcode bits */
    return m == 1 || m == 2 || m == 14 || m == 21 || (m >= 3 && m <= 5);
}

uint8_t GameCut(const GdxReplayGame& a, const GdxReplayGame& b) {
    if (a.numPlayers != 1 || b.numPlayers != 1) {
        return CUT_PLAYERS;
    }
    if (a.paused || b.paused) {
        return CUT_PAUSE;
    }
    if (a.modeChange != 0 || b.modeChange != 0 || a.transition != 0 || b.transition != 0) {
        return CUT_TRANS;
    }
    if (a.gameMode != b.gameMode) {
        return CUT_MODE;
    }
    if (!IsRaceMode(b.gameMode)) {
        return CUT_MENU;
    }
    if (a.clearTimer != 0 || b.clearTimer != 0) {
        return CUT_START;
    }
    if (a.totalRacers != b.totalRacers) {
        return CUT_RACERS;
    }
    if ((a.flags[0] | b.flags[0]) & 7) {
        /* the player finished, crashed or retired: the finish sequence, or an explosion whose
         * fx (A's, at the render rate) fill the screen -- 10-33 % of pixels off under verify */
        return CUT_RACEEND;
    }
    uint8_t lod = CUT_NONE;
    for (int i = 0; i < GDX_REPLAY_GAME_RACERS; i++) {
        if (a.flags[i] != b.flags[i]) {
            return CUT_RACERS;
        }
        if (a.lod[i] != b.lod[i]) {
            const bool vis = (a.lod[i] == 0) != (b.lod[i] == 0);
            if (vis && sVisCut) {
                lod = CUT_VIS;
            } else if (!vis && sLodCut && lod == CUT_NONE) {
                lod = CUT_LOD;
            }
        }
    }
    if (lod != CUT_NONE) {
        return lod;
    }
    const float dx = b.eye[0] - a.eye[0], dy = b.eye[1] - a.eye[1], dz = b.eye[2] - a.eye[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    const float la = sqrtf(a.fwd[0] * a.fwd[0] + a.fwd[1] * a.fwd[1] + a.fwd[2] * a.fwd[2]);
    const float lb = sqrtf(b.fwd[0] * b.fwd[0] + b.fwd[1] * b.fwd[1] + b.fwd[2] * b.fwd[2]);
    const float cosAng = (la > 0.0f && lb > 0.0f)
                             ? (a.fwd[0] * b.fwd[0] + a.fwd[1] * b.fwd[1] + a.fwd[2] * b.fwd[2]) / (la * lb)
                             : 1.0f;
    if (dist > sWin.dEyeMax) {
        sWin.dEyeMax = dist;
    }
    const float ang = (float)(acos(cosAng > 1.0f ? 1.0 : (cosAng < -1.0f ? -1.0 : (double)cosAng)) * 57.29577951308232);
    if (ang > sWin.angMax) {
        sWin.angMax = ang;
    }
    sLastAng = ang;
    sLastDist = dist;
    {
        static const float kEdges[5] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f };
        int bin = 0;
        while (bin < 5 && ang >= kEdges[bin]) {
            bin++;
        }
        sWin.angH[bin]++;
    }
    if (dist > sCutDist || cosAng < sCutCos) {
        static int sCamLogs = 0;
        if (sCamLogs < 8) {
            sCamLogs++;
            Log("[replay] camera cut lim=%.1f/%.4f dist=%.1f cos=%.4f eyeA=%.0f,%.0f,%.0f eyeB=%.0f,%.0f,%.0f", (double)sCutDist, (double)sCutCos, (double)dist,
                (double)cosAng, (double)a.eye[0], (double)a.eye[1], (double)a.eye[2], (double)b.eye[0],
                (double)b.eye[1], (double)b.eye[2]);
        }
        return CUT_CAMERA;
    }
    return CUT_NONE;
}

/* The part of GameCut that holds for a whole stretch of play (players, pause, transitions,
 * menus, the start countdown, the finish), read on one state: a render whose last tick is cut
 * this way would be recorded for nothing. */
uint8_t StaticCut(const GdxReplayGame& g) {
    if (g.numPlayers != 1) {
        return CUT_PLAYERS;
    }
    if (g.paused) {
        return CUT_PAUSE;
    }
    if (g.modeChange != 0 || g.transition != 0) {
        return CUT_TRANS;
    }
    if (!IsRaceMode(g.gameMode)) {
        return CUT_MENU;
    }
    if (g.clearTimer != 0) {
        return CUT_START;
    }
    if (g.flags[0] & 7) {
        return CUT_RACEEND;
    }
    return CUT_NONE;
}

uint8_t DecideCut(bool verify) {
    if (!verify && !sArmedA) {
        return sArmedCut; /* A was not recorded: its slot could not replay (why, decided then) */
    }
    if (!sTableValid) {
        return CUT_ABAD;
    }
    if (!sHaveA || !sHaveB) {
        return CUT_NOB;
    }
    if (!verify && (sSkipsSinceA < 1 || sSkipsSinceA > sMaxSkips)) {
        return CUT_CADENCE;
    }
    if (!verify && !sBWanted) {
        return CUT_UNARMED; /* the presented tick's build was not told to keep (c5tick hollow) */
    }
    if (sEpochA != sEpochB) {
        return CUT_HOOK;
    }
    if (sAptA != sAptB || sAptB != sAptCount) {
        return CUT_APT;
    }
    /* Each side is the tick its image shows. verify: both captured at their walk's start
     * (sGameWalk is still A's here). The lever: A drew the tick before its iteration's (the walk
     * runs first), and B is the slot's last dropped tick, captured as its iteration ended
     * (A's own iteration at D = 2, the first skip at D = 3). */
    const uint8_t g = verify ? GameCut(sGameWalk, sGameB) : GameCut(sGameWalkA, sGameBTick);
    if (g != CUT_NONE) {
        return g;
    }
    /* GfxPools alternate per built tick: an odd skip count presents a tick in the other pool,
     * an even one a tick that rewrote A's own (the walk-time copies keep A's words). */
    const bool wantSame = !verify && (sSkipsSinceA & 1) == 0;
    if (sBPool < 0 || (sBPool == sAPool) != wantSame) {
        static int sPoolLogs = 0;
        if (sPoolLogs < 6) {
            sPoolLogs++;
            Log("[replay] pool cut frame=%lu A=%d B=%d skips=%d gameA.cur=%d walk.cur=%d alias=%u direct=%u", sFrameNum, sAPool,
                sBPool, sSkipsSinceA, sGameA.curPool, sGameWalk.curPool, sAliasHits, sDirectHits);
        }
        return CUT_POOL;
    }
    return CUT_NONE;
}

/* Freshness per slot from the skipped tick: a slot B's tick rewrote carries B's pose; one it did
 * not rewrite holds the value from two ticks before B, which is A's only if nothing moved. */
unsigned ClassifySlots(const unsigned char* poolB) {
    unsigned stale = 0;
    for (int i = 0; i < sNSlots; i++) {
        Slot& s = sT->slots[i];
        memcpy(s.b, poolB + s.off, 64);
        const bool rewrote = memcmp(s.b, s.start[sBPool], 64) != 0;
        const bool same = memcmp(s.b, s.a, 64) == 0;
        s.fresh = (rewrote || same) ? 1 : 0;
        if (!s.fresh) {
            stale++;
        }
    }
    return stale;
}

void CountCut(uint8_t c) {
    sWin.cuts[c]++;
    if (c == CUT_ABAD) {
        sWin.bad[sBad]++;
    }
}

/* Capture B's matrices (B's pool is the current one), classify, compute, patch, flush. */
/* A matrix that is not in a GfxPool is taken as a constant of the frame; count the ones whose
 * source bytes moved by the time of the replay (diagnostic: they would be stale in B). */
void CountConstChanges() {
    static int sLogs = 0;
    for (int i = 0; i < sNConst; i++) {
        sWin.constTotal++;
        if (memcmp(sT->cnstSrc[i], sT->cnstRaw[i], 64) != 0) {
            sWin.constChanged++;
            if (sLogs < 12) {
                sLogs++;
                Log("[replay] const moved src=%p idx=%d", sT->cnstSrc[i], i);
            }
        }
    }
}

/* B's Vtx for every recorded vertex, T&L'd with A's MP (the vertex path's own arithmetic). */
void Revertex(const unsigned char* poolB) {
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < sNRevtx; i++) {
        const Revtx& r = sRevtx[i];
        sWin.rvN++;
        const unsigned char* v = poolB + (r.src & 0x3FFFFFFFu);
        if ((int)((r.src >> 30) & 1u) == sBPool) {
            if (!sBSamePool) {
                continue; /* B's tick wrote A's pool on an odd skip count: not the double-buffer */
            }
            /* B rewrote A's own pool (D = 3): bytes it left alone are A's, and their T&L below
             * reproduces A's vertex, so every record is re-uploaded (no two-ticks-back test). */
        } else if (memcmp(v, r.before, 16) == 0) {
            sWin.rvStale++; /* not rewritten by B's tick: keep A's */
            continue;
        }
        int16_t ob[3];
        memcpy(ob, v, 6);
        if ((r.src & 0x80000000u) != 0) {
            for (int k = 0; k < 3; k++) {
                ob[k] = (int16_t)(((uint16_t)ob[k] >> 8) | ((uint16_t)ob[k] << 8));
            }
        }
        const Entry& e = sT->entries[r.tag - GDX_REPLAY_TAG_FIRST];
        const float (*mp)[4] = e.mpA;
        float x = ob[0] * mp[0][0] + ob[1] * mp[1][0] + ob[2] * mp[2][0] + mp[3][0];
        const float y = ob[0] * mp[0][1] + ob[1] * mp[1][1] + ob[2] * mp[2][1] + mp[3][1];
        const float z = ob[0] * mp[0][2] + ob[1] * mp[1][2] + ob[2] * mp[2][2] + mp[3][2];
        const float w = ob[0] * mp[0][3] + ob[1] * mp[1][3] + ob[2] * mp[2][3] + mp[3][3];
        x = x * e.xscale;
        r.out[0] = x;
        r.out[1] = y;
        r.out[2] = (z + w) / 2.0f; /* GetClipParameters: z in [0, w], no y flip */
        r.out[3] = w;
        sWin.rvUp++;
        const uintptr_t a = (uintptr_t)r.out;
        lo = a < lo ? a : lo;
        hi = a + 16 > hi ? a + 16 : hi;
    }
    if (hi > lo) {
        GSPGPU_FlushDataCache((const void*)lo, (u32)(hi - lo));
    }
}

bool PrepareB(PatchStats* ps, uint64_t* patchTicks) {
    const unsigned char* poolB = sGameB.poolBase + (size_t)sBPool * sGameB.poolSize;
    if (sDeep) {
        CountConstChanges();
    }
    for (int i = 0; sDeep && i < sNDraws; i++) {
        const DrawGov& d = sDrawGov[i];
        if (d.rp == nullptr) {
            continue;
        }
        sWin.rgN[d.cat & 15u]++;
        if (RangeSum(d.rp, d.rn) != d.rsum) {
            sWin.rgBad[d.cat & 15u]++;
            static int sRgLogs = 0;
            if (sRgLogs < 24) {
                sRgLogs++;
                Log("[replay] vbo changed frame=%lu j=%d cat=%u tag=0x%04x floats=%u at=%p", sFrameNum, i, d.cat, d.tag, d.rn,
                    (const void*)d.rp);
            }
        }
    }
    for (int i = 0; sDeep && i < sNVSrc; i++) {
        const VSrc& v = sVSrc[i];
        const unsigned char* pa = sGameB.poolBase + (size_t)v.pool * sGameB.poolSize + v.off;
        const unsigned c = v.cat & 15u;
        sWin.vsPool[c]++;
        if (v.pool != (uint8_t)sBPool && v.off + v.n * 16u <= sGameB.poolSize && memcmp(pa, poolB + v.off, v.n * 16u) != 0) {
            sWin.vsDiff[c]++;
        }
    }
    sWin.stale += ClassifySlots(poolB);
    const uint64_t t0 = svcGetSystemTick();
    if (!ComputeB() || !PatchAll(ps)) {
        return false;
    }
    FlushPatched(*ps);
    if (!sNoPatch) {
        Revertex(poolB);
    }
    if (sHash) {
        /* What the GPU will read that the replay wrote: every site's pose words and every
         * re-uploaded vertex -- a function of B's tick alone once A's frame is fixed. */
        uint32_t h = 2166136261u;
        for (int i = 0; i < sNSites; i++) {
            const Site& st = sT->sites[i];
            for (int k = 0; k < st.nEmit; k++) {
                const Emit& em = sT->emits[st.emit0 + k];
                const uint32_t* rest = em.ptr + (em.split ? 2 : 1);
                const uint32_t w[4] = { em.ptr[0], rest[0], rest[1], rest[2] };
                for (int c = 0; c < 4; c++) {
                    h = (h ^ w[c]) * 16777619u;
                }
            }
        }
        unsigned up = 0;
        for (int i = 0; i < sNRevtx; i++) {
            const Revtx& r = sRevtx[i];
            if (memcmp(poolB + (r.src & 0x3FFFFFFFu), r.before, 16) == 0) {
                continue;
            }
            up++;
            h = (h ^ (uint32_t)i) * 16777619u;
            for (int c = 0; c < 4; c++) {
                uint32_t u;
                memcpy(&u, &r.out[c], 4);
                h = (h ^ u) * 16777619u;
            }
        }
        Log("[replay] hash frameA=%lu sites=%d revtx=%u/%d h=%08lx", sFrameA, sNSites, up, sNRevtx, (unsigned long)h);
    }
    *patchTicks = svcGetSystemTick() - t0;
    if (ps->dEye > sWin.dEyeMax) {
        sWin.dEyeMax = ps->dEye;
    }
    return true;
}

/* ---- verify lane: readback + compare ---- */

constexpr u32 kFbW = 240;
constexpr u32 kFbH = 400;

void ReadbackTarget(uint32_t* dst) {
    void* color = gdx3ds_replay_backend_colorbuf();
    if (color == nullptr || dst == nullptr) {
        return;
    }
    gxCmdQueue_s* q = FrameQueue();
    gxCmdQueueWait(q, -1);
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    gxCmdEntry_s e;
    memset(&e, 0, sizeof(e));
    e.data[0] = 0x01000100u | 0x03u;
    e.data[1] = (u32)(uintptr_t)color;
    e.data[2] = (u32)(uintptr_t)dst;
    e.data[3] = GX_BUFFER_DIM(kFbW, kFbH);
    e.data[4] = GX_BUFFER_DIM(kFbW, kFbH);
    e.data[5] = flags;
    GSPGPU_InvalidateDataCache(dst, kFbW * kFbH * 4);
    gxCmdQueueAdd(q, &e);
    gxCmdQueueRun(q);
    gxCmdQueueWait(q, -1);
    GSPGPU_InvalidateDataCache(dst, kFbW * kFbH * 4);
}

void ComparePixels() {
    if (sVfyR == nullptr || sVfyF == nullptr) {
        return;
    }
    const u32 n = kFbW * kFbH;
    for (u32 i = 0; i < n; i++) {
        const uint32_t a = sVfyR[i];
        const uint32_t b = sVfyF[i];
        sWin.vPx++;
        if (a == b) {
            continue;
        }
        unsigned mx = 0;
        for (int c = 8; c < 32; c += 8) { /* RGBA8: skip the alpha byte (bits 0-7) */
            const int da = (int)((a >> c) & 0xFFu) - (int)((b >> c) & 0xFFu);
            const unsigned d = (unsigned)(da < 0 ? -da : da);
            mx = d > mx ? d : mx;
        }
        if (mx == 0) {
            continue;
        }
        sWin.vPxDiff++;
        sWin.vSumDelta += mx;
        if (mx > 8) {
            sWin.vPxOver++;
        }
        if (mx > sWin.vMaxDelta) {
            sWin.vMaxDelta = mx;
        }
    }
}

/* [debug] replay_verify_dump=N: the first N verified pairs (from replay_verify_dump_from) as raw
 * RGBA8 240x400 portrait files on the SD, for tools-side inspection. */
void DumpPair() {
    if (sDumpLeft <= 0 || sFrameNum < sDumpFrom) {
        return;
    }
    sDumpLeft--;
    Log("[replay] dump frame=%lu ang=%.2f dist=%.1f", sFrameNum, (double)sLastAng, (double)sLastDist);
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/3ds/gdiffuser/c6vfy-%06lu.raw", sFrameNum);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    fwrite(sVfyR, 4, kFbW * kFbH, f);
    fwrite(sVfyF, 4, kFbW * kFbH, f);
    if (sVfyA != nullptr) {
        fwrite(sVfyA, 4, kFbW * kFbH, f);
    }
    fclose(f);
}

/* A site's current 4x4 (legacy: the projection rows; xf: xmvp rows), read from its words. */
bool ReadSiteMtx(const Site& st, float m[16]) {
    int seen = 0;
    for (int i = 0; i < st.nEmit; i++) {
        const Emit& em = sT->emits[st.emit0 + i];
        if (em.vec >= 4) {
            continue;
        }
        const uint32_t* rest = em.ptr + (em.split ? 2 : 1);
        float* row = m + em.vec * 4;
        row[3] = BitsFloat(em.ptr[0]);
        row[2] = BitsFloat(rest[0]);
        row[1] = BitsFloat(rest[1]);
        row[0] = BitsFloat(rest[2]);
        seen |= 1 << em.vec;
    }
    return seen == 0xF;
}

void KeepVerifyDraws() {
    sVfyDrawN = sNDraws;
    for (int i = 0; i < sNDraws; i++) {
        VfyDraw& v = sVfyDraws[i];
        const DrawGov& d = sDrawGov[i];
        v.xf = d.xf;
        v.cat = d.cat;
        v.tag = d.tag;
        v.valid = (d.site >= 0 && ReadSiteMtx(sT->sites[d.site], v.m)) ? 1 : 0;
        v.hasPred = 0;
        const uint16_t t = (uint16_t)(d.tag & 0x7FFFu);
        if (!d.xf && t >= GDX_REPLAY_TAG_FIRST && (d.tag & 0x8000u) == 0) {
            const Entry& e = sT->entries[t - GDX_REPLAY_TAG_FIRST];
            double k[4][4], f[4][4], fi[4][4], tt[4][4], kp[4][4];
            if (Correction(e.mpA, e.mpB, k)) {
                PackMatrix(e.xscale, f);
                if (Invert4(f, fi)) {
                    MulD(tt, fi, k);
                    MulD(kp, tt, f);
                    for (int c = 0; c < 4; c++) {
                        v.pred[c] = (float)(d.p[0] * kp[0][c] + d.p[1] * kp[1][c] + d.p[2] * kp[2][c] + d.p[3] * kp[3][c]);
                    }
                    v.hasPred = 1;
                    v.xs = e.xscale;
                    static const float kAlt[4] = { 1.0f, 0.8f, 160.0f / 148.0f, 1.25f };
                    for (int a = 0; a < 4; a++) {
                        double fa[4][4], fai[4][4], ta[4][4], ka[4][4];
                        PackMatrix(kAlt[a], fa);
                        Invert4(fa, fai);
                        MulD(ta, fai, k);
                        MulD(ka, ta, fa);
                        for (int c = 0; c < 4; c++) {
                            v.alt[a][c] = (float)(d.p[0] * ka[0][c] + d.p[1] * ka[1][c] + d.p[2] * ka[2][c] + d.p[3] * ka[3][c]);
                        }
                    }
                }
            }
        }
    }
}

void ReadFound4(const Found* rows, float m[16]) {
    for (int r = 0; r < 4; r++) {
        for (int j = 0; j < 4; j++) {
            m[r * 4 + j] = BitsFloat(*rows[r].w[3 - j]);
        }
    }
}

/* F's draw j against the replay's draw j: the pose uniform each really ran with. */
void CompareDraw(uint16_t tag, int isXf, bool sawProj, const Found* proj, bool sawPose, const Found* pose) {
    if (sawProj && proj[0].have == 0xF && proj[1].have == 0xF && proj[2].have == 0xF && proj[3].have == 0xF) {
        ReadFound4(proj, sFProj);
    }
    if (sawPose && pose[0].have == 0xF && pose[1].have == 0xF && pose[2].have == 0xF && pose[3].have == 0xF) {
        ReadFound4(pose, sFXf);
    }
    const int j = sFDraw++;
    if (j >= sVfyDrawN) {
        return;
    }
    const VfyDraw& v = sVfyDraws[j];
    if (!isXf && v.xf == 0 && v.hasPred && v.tag == tag && sVtx0[3] > 0.0f && v.pred[3] > 0.0f) {
        /* legacy: the pose is in the vertices; A's first vertex corrected vs F's, in pixels */
        const float dx = (v.pred[0] / v.pred[3] - sVtx0[0] / sVtx0[3]) * 200.0f;
        const float dy = (v.pred[1] / v.pred[3] - sVtx0[1] / sVtx0[3]) * 120.0f;
        const float px = sqrtf(dx * dx + dy * dy);
        sWin.lN++;
        if (px > sWin.lMax) {
            sWin.lMax = px;
        }
        const bool focus = v.cat == 5 && sDumpFrom != 0 && sFrameNum >= sDumpFrom && sFrameNum < sDumpFrom + 4;
        if (px > 1.0f || focus) {
            sWin.lMis++;
            static int sLLogs = 0;
            if ((sLLogs < 60 && v.cat != 3) || focus) {
                sLLogs++;
                float ap[4];
                for (int a = 0; a < 4; a++) {
                    const float ax = (v.alt[a][0] / v.alt[a][3] - sVtx0[0] / sVtx0[3]) * 200.0f;
                    const float ay = (v.alt[a][1] / v.alt[a][3] - sVtx0[1] / sVtx0[3]) * 120.0f;
                    ap[a] = sqrtf(ax * ax + ay * ay);
                }
                Log("[replay] legacy alt xs=%.4f px@1=%.2f @0.8=%.2f @1.081=%.2f @1.25=%.2f", (double)v.xs, (double)ap[0],
                    (double)ap[1], (double)ap[2], (double)ap[3]);
                Log("[replay] legacy diff frame=%lu j=%d cat=%u tag=0x%04x px=%.2f dx=%.2f dy=%.2f wA=%.1f wF=%.1f", sFrameNum, j,
                    v.cat, tag, (double)px, (double)dx, (double)dy, (double)v.pred[3], (double)sVtx0[3]);
            }
        }
        return;
    }
    if (!v.valid || v.xf != (isXf ? 1 : 0) || !isXf) {
        return;
    }
    const float* f = isXf ? sFXf : sFProj;
    float scale = 0.0f, diff = 0.0f;
    for (int i = 0; i < 16; i++) {
        const float a = fabsf(f[i]);
        scale = a > scale ? a : scale;
        const float d = fabsf(f[i] - v.m[i]);
        diff = d > diff ? d : diff;
    }
    const float rel = scale > 0.0f ? diff / scale : diff;
    sWin.dN++;
    if (rel > sWin.dMax) {
        sWin.dMax = rel;
    }
    if (rel > 1e-3f) {
        sWin.dMis++;
        static int sLogs = 0;
        if (sLogs < 40) {
            sLogs++;
            Log("[replay] draw diff frame=%lu j=%d xf=%d cat=%u tagR=0x%04x tagF=0x%04x rel=%.4g", sFrameNum, j, isXf,
                v.cat, v.tag, tag, (double)rel);
        }
    }
}

/* The replayed frame's MP per entry, kept for the pose diff against the full render. */
void KeepVerifyPoses() {
    if (sVfy == nullptr) {
        return;
    }
    sVfySlotN = sNSlots;
    for (int i = 0; i < sNSlots; i++) {
        sVfySlots[i].off = sT->slots[i].off;
        memcpy(sVfySlots[i].w, SlotWordsForB(sT->slots[i]), 64);
        sVfySlots[i].fresh = sT->slots[i].fresh;
    }
    if (sVfyDraws != nullptr) {
        KeepVerifyDraws();
    }
    sVfyN = sNEntries;
    for (int i = 0; i < sNEntries; i++) {
        sVfy[i].opCount = sT->entries[i].opCount;
        sVfy[i].xscale = sT->entries[i].xscale;
        memcpy(sVfy[i].mp, sT->entries[i].mpB, sizeof(sVfy[i].mp));
    }
}

/* B's full render loads a pool matrix: is it the one the replay used for that offset? */
void VerifySlot(uint32_t off, const int32_t* words) {
    if (!sVerifyDone || sVfySlots == nullptr) {
        return;
    }
    for (int i = 0; i < sVfySlotN; i++) {
        if (sVfySlots[i].off == off) {
            if (memcmp(sVfySlots[i].w, words, 64) == 0) {
                sWin.vSlotSame++;
            } else if (sVfySlots[i].fresh) {
                sWin.vSlotDiff++;
            } else {
                sWin.vSlotStaleDiff++;
            }
            return;
        }
    }
    sWin.vSlotNew++;
}

/* B's full render creates entry `idx`: compare it with the replay's MP_B for the same entry. */
void VerifyEntry(int idx, const Entry& e) {
    if (!sVerifyDone || sVfy == nullptr) {
        return;
    }
    if (idx >= sVfyN || sVfy[idx].opCount != e.opCount || sVfy[idx].xscale != e.xscale) {
        sWin.vMisaligned++;
        sVerifyDone = 2; /* the rest of this frame no longer lines up */
        return;
    }
    if (sVerifyDone != 1) {
        return;
    }
    sWin.vEntries++;
    float scale = 0.0f, diff = 0.0f;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            const float m = fabsf(e.mpA[i][j]);
            scale = m > scale ? m : scale;
            const float d = fabsf(e.mpA[i][j] - sVfy[idx].mp[i][j]);
            diff = d > diff ? d : diff;
        }
    }
    if (diff == 0.0f) {
        sWin.vExact++;
    }
    const float rel = scale > 0.0f ? diff / scale : diff;
    if (rel > sWin.vMaxRel) {
        sWin.vMaxRel = rel;
    }
}

/* ---- receipts ---- */

float TicksToUs(uint64_t t) {
    return (float)((double)t * 1000.0 / (double)CPU_TICKS_PER_MSEC);
}

void EmitWindow() {
    const uint64_t now = svcGetSystemTick();
    const double dtS = sWinT0 != 0 ? (double)(now - sWinT0) / (double)SYSCLOCK_ARM11 : 0.0;
    const double rps = dtS > 0.0 ? sWin.replays / dtS : 0.0;
    const double rend = dtS > 0.0 ? sWin.renders / dtS : 0.0;
    const unsigned tf = sWin.tableFrames != 0 ? sWin.tableFrames : 1;
    const unsigned rp = sWin.replays != 0 ? sWin.replays : 1;
    unsigned cutTotal = 0;
    for (int i = 1; i < CUT_COUNT; i++) {
        cutTotal += sWin.cuts[i];
    }
    Log("[replay] frame=%lu n=%u rendered/s=%.1f replays/s=%.1f images/s=%.1f slots=%u replays=%u cuts=%u recorded=%u "
        "P_us=%.0f max=%.0f patch_us=%.0f submit_us=%.0f sites/f=%.1f tags/f=%.1f ops/f=%.1f slots/f=%.1f "
        "stale/replay=%.2f dEyeMax=%.0f angMax=%.1f fencewait=%u const=%u/%u meshmp=%u/%u gov=%u/%u draws=%u/%u dmax=%.3g legacy=%u/%u lmaxpx=%.1f",
        sFrameNum, sWin.iters, rend, rps, rend + rps, sWin.slots, sWin.replays, cutTotal, sWin.recorded,
        TicksToUs(sWin.pTicks) / rp, TicksToUs(sWin.pMax), TicksToUs(sWin.patchTicks) / rp,
        TicksToUs(sWin.submitTicks) / rp, (double)sWin.sites / tf, (double)sWin.entries / tf,
        (double)sWin.ops / tf, (double)sWin.slotsN / tf, (double)sWin.stale / rp, (double)sWin.dEyeMax,
        (double)sWin.angMax, sWin.fenceWaits, sWin.constChanged, sWin.constTotal, sWin.meshMpMis, sWin.meshMpN, sWin.govMis, sWin.govN, sWin.dMis, sWin.dN, (double)sWin.dMax, sWin.lMis, sWin.lN, (double)sWin.lMax);
    Log("[replay] revtx vfyhollow=%u recs=%u up=%u stale=%u over=%u | ang<0.5=%u <1=%u <2=%u <3=%u <5=%u >=5=%u", sWin.vfyHollow, sWin.rvN, sWin.rvUp,
        sWin.rvStale, sWin.rvOver, sWin.angH[0], sWin.angH[1], sWin.angH[2], sWin.angH[3], sWin.angH[4], sWin.angH[5]);
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "[replay] cuts");
    for (int i = 1; i < CUT_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        if (sWin.cuts[i] != 0) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%u", kCutName[i], sWin.cuts[i]);
        }
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " | abad");
    }
    for (int i = 1; i < BAD_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        if (sWin.bad[i] != 0) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%u", kBadName[i], sWin.bad[i]);
        }
    }
    Log("%s", buf);
    for (unsigned c = 0; c < 16; c++) {
        if (sWin.vsPool[c] != 0 || sWin.vsOther[c] != 0) {
            Log("[replay] vtxsrc cat=%u pool=%u diff=%u other=%u | vbo draws=%u changed=%u", c, sWin.vsPool[c], sWin.vsDiff[c],
                sWin.vsOther[c], sWin.rgN[c], sWin.rgBad[c]);
        }
    }
    if (sVerify) {
        const double px = sWin.vPx != 0 ? (double)sWin.vPx : 1.0;
        Log("[replay] verify frames=%u pose aligned=%u misaligned=%u entries=%u exact=%u maxrel=%.3g | slots same=%u "
            "diff=%u stalediff=%u new=%u | px diff=%.3f%% over1q=%.3f%% maxd=%u meand=%.2f",
            sWin.vFrames, sWin.vAligned, sWin.vMisaligned, sWin.vEntries, sWin.vExact, (double)sWin.vMaxRel,
            sWin.vSlotSame, sWin.vSlotDiff, sWin.vSlotStaleDiff, sWin.vSlotNew,
            100.0 * (double)sWin.vPxDiff / px, 100.0 * (double)sWin.vPxOver / px, sWin.vMaxDelta,
            sWin.vPxDiff != 0 ? (double)sWin.vSumDelta / (double)sWin.vPxDiff : 0.0);
    }
    memset(&sWin, 0, sizeof(sWin));
    sWinT0 = now;
}

bool AllocTables() {
    if (sT != nullptr) {
        return true;
    }
    sT = static_cast<Table*>(malloc(sizeof(Table)));
    if (sT == nullptr) {
        return false;
    }
    memset(sT, 0, sizeof(Table));
    if (sRevtxMask != 0) {
        sRevtx = static_cast<Revtx*>(malloc(sizeof(Revtx) * kMaxRevtx));
        if (sRevtx == nullptr) {
            sRevtxMask = 0;
        }
    }
    if (sVerify) {
        sVfyR = static_cast<uint32_t*>(linearAlloc(kFbW * kFbH * 4));
        sVfyF = static_cast<uint32_t*>(linearAlloc(kFbW * kFbH * 4));
        sVfyA = static_cast<uint32_t*>(linearAlloc(kFbW * kFbH * 4));
        sVfy = static_cast<VfyEntry*>(malloc(sizeof(VfyEntry) * kMaxEntries));
        sVfySlots = static_cast<VfySlot*>(malloc(sizeof(VfySlot) * kMaxSlots));
        sVfyDraws = static_cast<VfyDraw*>(malloc(sizeof(VfyDraw) * kMaxDraws));
        if (sVfyR == nullptr || sVfyF == nullptr || sVfy == nullptr || sVfySlots == nullptr) {
            sVerify = 0;
        }
    }
    return true;
}

bool LayoutOk() {
    const C3dPrefix* c = Ctx();
    return c->gxQueue.entries != nullptr && c->gxQueue.maxEntries == kMaxQueue && c->cmdBuf != nullptr &&
           c->cmdBufSize >= 0x10000u;
}

void LatchConfig(int rtMode) {
    sLatched = 1;
    const int want = ConfigInt("native", "replay", 0) != 0;
    const int wantVerify = ConfigInt("debug", "replay_verify", 0) != 0;
    const int interpOwned = ConfigInt("native", "interp", 0) != 0;
    const int haveGame = &gdx3ds_replay_game_capture != nullptr;
    sLodCut = ConfigInt("native", "replay_lodcut", 1) != 0;
    sVisCut = ConfigInt("native", "replay_viscut", 1) != 0;
    sCutDist = (float)ConfigInt("native", "replay_cutdist", 1500);
    const int deg = ConfigInt("native", "replay_cutdeg", 30); /* 3 measured worse: at a snap, A again is further from B than the replay */
    const int cdeg = ConfigInt("native", "replay_cutcdeg", 0); /* centidegrees; > 0 overrides replay_cutdeg */
    sCutCos = (float)cos((cdeg > 0 ? (double)cdeg / 100.0 : (double)deg) * 0.017453292519943295); /* double: cosf returned inf on this libm */
    const char* why = "on";
    if (!want && !wantVerify) {
        why = "off";
    } else if (rtMode != 0) {
        why = "renderthread";
    } else if (!interpOwned) {
        why = "needs [native] interp=1";
    } else if (!haveGame) {
        why = "no game capture";
    } else if (!LayoutOk()) {
        why = "c3d-layout";
    }
    const bool ok = (want || wantVerify) && rtMode == 0 && interpOwned && haveGame;
    sVerify = ok && wantVerify;
    sDumpLeft = ConfigInt("debug", "replay_verify_dump", 0);
    sNoPatch = sVerify && ConfigInt("debug", "replay_verify_nopatch", 0) != 0;
    sDiag = ConfigInt("debug", "replay_diag", 0);
    sSkipCat = (unsigned)ConfigInt("debug", "replay_skip_cat", 0);
    sSkipCatXf = (unsigned)ConfigInt("debug", "replay_skip_cat_xf", 0);
    sSkipTag = (unsigned)ConfigInt("debug", "replay_skip_tag", 0);
    sDeep = ConfigInt("debug", "replay_deep", 0) != 0;
    sHash = ConfigInt("debug", "replay_hash", 0) != 0;
    sRevtxMask = (unsigned)ConfigInt("native", "replay_revtx", (1 << 2) | (1 << 8)); /* sky + fx */
    {
        /* D = 2: one replay per render; D = 3: one replay, held for two vblanks. D = 4 would
         * present a tick in the pool the revtx staleness test cannot read, so 3 is the cap. */
        const int dmax = ConfigInt("native", "replay_dmax", 3);
        sMaxSkips = (dmax < 2 ? 2 : (dmax > 3 ? 3 : dmax)) - 1;
    }
    sSkipTagHi = (unsigned)ConfigInt("debug", "replay_skip_tag_hi", 0);
    sDumpFrom = (unsigned long)ConfigInt("debug", "replay_verify_dump_from", 0);
    if (ok && !AllocTables()) {
        why = "alloc";
    }
    gdx3ds_replay_on = (ok && sT != nullptr) ? 1 : 0;
    sMarkersOn = gdx3ds_replay_on;
    gdx3ds_replay_arm_interp(0); /* armed per frame (gdx3ds_replay_frame_begin) */
    Log("[replay] init replay=%d verify=%d lodcut=%d viscut=%d cutdist=%.0f cutdeg=%d dmax=%d table=%uKB (%s)",
        gdx3ds_replay_on, sVerify, sLodCut, sVisCut, (double)sCutDist, deg, sMaxSkips + 1,
        (unsigned)(sizeof(Table) / 1024), why);
    sWinT0 = svcGetSystemTick();
}

} // namespace

/* ======================================================================================= */

extern "C" {

void gdx3ds_replay_init(int rtMode) {
    if (!sLatched) {
        LatchConfig(rtMode);
    }
}

int gdx3ds_replay_markers_armed(void) {
    return sMarkersOn;
}

void gdx3ds_replay_note_apt(void) {
    sAptCount++;
}

void gdx3ds_replay_backend_bind(int projLoc, int xfLoc) {
    sProjLoc = projLoc;
    sXfLoc = xfLoc;
}

void gdx3ds_replay_set_draw_tag(uint16_t tag) {
    sDrawTag = tag;
}

uint16_t gdx3ds_replay_take_draw_tag(void) {
    const uint16_t t = sDrawTag;
    sDrawTag = GDX_REPLAY_TAG_UNKNOWN;
    return t;
}

void gdx3ds_replay_set_mesh_xscale(float xscale) {
    sMeshXscale = xscale;
}

float gdx3ds_replay_mesh_xscale(void) {
    return sMeshXscale;
}

void gdx3ds_replay_out_vtx(float* out, int slot) {
    if (sRevtx == nullptr || slot < 0 || slot >= GDX_REPLAY_VTX_SLOTS || ((sRevtxMask >> (sScene & 31u)) & 1u) == 0 ||
        !Recording()) {
        return;
    }
    const uint32_t src = gdx3ds_replay_vtx_src[slot];
    const uint16_t tag = gdx3ds_replay_vtx_tag[slot];
    if (src == GDX_REPLAY_SRC_NONE || tag < GDX_REPLAY_TAG_FIRST || (tag & 0x8000u) != 0) {
        return;
    }
    if (sNRevtx >= kMaxRevtx) {
        sWin.rvOver++;
        return;
    }
    Revtx& r = sRevtx[sNRevtx++];
    r.out = out;
    r.src = src;
    r.tag = tag;
    const uint32_t off = src & 0x3FFFFFFFu;
    const int other = 1 - (int)((src >> 30) & 1u);
    memcpy(r.before, sGameWalk.poolBase + (size_t)other * sGameWalk.poolSize + off, 16);
}

void gdx3ds_replay_note_range(const float* p, unsigned nFloats) {
    sRangeP = reinterpret_cast<const uint32_t*>(p);
    sRangeN = nFloats;
}

void gdx3ds_replay_note_vtx0(const float* xyzw) {
    memcpy(sVtx0, xyzw, sizeof(sVtx0));
}

void gdx3ds_replay_note_mesh_mp(const float* mp) {
    memcpy(sMeshMp, mp, sizeof(sMeshMp));
}

/* The CPU mesh draw about to be issued: were its vertices T&L'd with its tag's MP_A? */
void gdx3ds_replay_check_mesh_mp(void) {
    if (!sDeep || !Recording() || sDrawTag < GDX_REPLAY_TAG_FIRST || (sDrawTag & 0x8000u) != 0) {
        return;
    }
    const int idx = sDrawTag - GDX_REPLAY_TAG_FIRST;
    if (idx >= sNEntries) {
        return;
    }
    const Entry& e = sT->entries[idx];
    sWin.meshMpN++;
    float diff = 0.0f;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            const float d = fabsf(e.mpA[i][j] - sMeshMp[i][j]);
            diff = d > diff ? d : diff;
        }
    }
    if (diff != 0.0f) {
        sWin.meshMpMis++;
        static int sLogs = 0;
        if (sLogs < 16) {
            sLogs++;
            Log("[replay] mesh mp mismatch frame=%lu tag=0x%04x cat=%u ops=%u/%d diff=%.4g", sFrameNum, sDrawTag, sScene,
                e.opCount, sNOps, (double)diff);
        }
    }
}

void gdx3ds_replay_scene_marker(unsigned category) {
    sInSky = category == 2u; /* GDX3DS_C5CAT_SKY */
    sScene = category;
}

static int ReplayInSky(void) {
    return sInSky;
}

/* ---- frame lifecycle ---- */

void gdx3ds_replay_frame_begin(void) {
    if (!gdx3ds_replay_on) {
        return;
    }
    /* Record only a frame whose slot can replay (or one verify needs): an unarmed frame runs the
     * lever-off path -- no hooks, no tags, no batch splits, no re-dirty. */
    const int arm = sArmIter || sVerifyPending;
    sArmedA = arm;
    sArmedCut = arm ? CUT_NONE : sArmCutIter;
    gdx3ds_replay_rec = arm;
    gdx3ds_replay_arm_interp(arm);
    if (!arm) {
        sTableValid = 0;
        return;
    }
    sWin.recorded++;
    /* Self-contained command list: re-emit every piece of citro3d state at the first draw. */
    Ctx()->flags |= kDirtyAll;
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    sFrameOpen = 1;
    if (sVerifyPending) {
        sNeedReset = 1; /* the verified replay reads A's table at the first task first */
    } else {
        ResetTable();
        sNeedReset = 0;
    }
}

static void ReplayTaskBegin(const float* p16, const float* mvStack16x11, int mvSize, const float* mp16);

uint16_t gdx3ds_replay_cur_tag(float xscale) {
    if (!Recording() || sInterpMp == nullptr) {
        return GDX_REPLAY_TAG_UNKNOWN;
    }
    GdxHwsScope hws(GDX3DS_HWS_RPLREC);
    if (sNEntries > 0) {
        const Entry& last = sT->entries[sNEntries - 1];
        if (last.opCount == (uint32_t)sNOps && last.xscale == xscale) {
            return (uint16_t)(GDX_REPLAY_TAG_FIRST + sNEntries - 1);
        }
    }
    if (sNEntries >= kMaxEntries) {
        MarkBad(BAD_OVERFLOW);
        return GDX_REPLAY_TAG_UNKNOWN;
    }
    if (memcmp(sMp, sInterpMp, sizeof(sMp)) != 0) {
        MarkBad(BAD_MPDRIFT);
        return GDX_REPLAY_TAG_UNKNOWN;
    }
    Entry& e = sT->entries[sNEntries];
    e.opCount = (uint32_t)sNOps;
    e.xscale = xscale;
    memcpy(e.mpA, sInterpMp, sizeof(e.mpA));
    Mv3Of(sMv, sMvSize, e.mv3A);
    VerifyEntry(sNEntries, e);
    sNEntries++;
    return (uint16_t)(GDX_REPLAY_TAG_FIRST + sNEntries - 1);
}

static void ReplayOpMtx(unsigned params, const void* src, const int32_t* words) {
    if (!Recording()) {
        return;
    }
    GdxHwsScope hws(GDX3DS_HWS_RPLREC);
    if (sNOps >= kMaxOps) {
        MarkBad(BAD_OVERFLOW);
        return;
    }
    Op& op = sT->ops[sNOps];
    op.kind = OP_MTX;
    op.params = (uint8_t)params;
    op.slot = -1;
    op.cnst = -1;
    op.arg = 0;
    float m[4][4];
    MtxFromWords(words, m);
    uint32_t off = 0;
    const int pool = PoolOf(src, &off);
    if (pool >= 0) {
        if (sAPool < 0) {
            sAPool = pool;
        } else if (sAPool != pool) {
            MarkBad(BAD_SLOTDIFF);
        }
        VerifySlot(off, words);
        op.slot = (int16_t)FindOrAddSlot(off, words);
        if (op.slot < 0) {
            return;
        }
    } else {
        if (sNConst >= kMaxConst) {
            MarkBad(BAD_OVERFLOW);
            return;
        }
        memcpy(sT->cnst[sNConst], m, sizeof(m));
        sT->cnstSrc[sNConst] = src;
        memcpy(sT->cnstRaw[sNConst], src, 64);
        op.cnst = (int16_t)sNConst++;
    }
    ApplyMtx(sP, sMv, &sMvSize, sMp, params, m);
    sNOps++;
}

static void ReplayOpPop(unsigned count) {
    if (!Recording()) {
        return;
    }
    GdxHwsScope hws(GDX3DS_HWS_RPLREC);
    if (sNOps >= kMaxOps) {
        MarkBad(BAD_OVERFLOW);
        return;
    }
    Op& op = sT->ops[sNOps++];
    op.kind = OP_POP;
    op.params = 0;
    op.slot = -1;
    op.cnst = -1;
    op.arg = (uint16_t)count;
    ApplyPop(sP, sMv, &sMvSize, sMp, count);
}

/* ---- backend draws ---- */

int gdx3ds_replay_legacy_needs_proj(uint16_t tag) {
    return Recording() && tag != sProjTag;
}

int gdx3ds_replay_xf_needs_proj(void) {
    return Recording() && sProjTag != 0xFFFF && (sProjTag & 0x7FFFu) >= GDX_REPLAY_TAG_FIRST;
}
int gdx3ds_replay_xf_needs_pose(uint16_t tag) {
    return Recording() && tag != sXfTag;
}

void gdx3ds_replay_note_untagged(void* inner, void* outer, unsigned tris) {
    if (!Recording()) {
        return;
    }
    /* one line per distinct path (its callers), with the frame it was first seen in */
    constexpr int kPaths = 32;
    static void* sSeen[kPaths][2];
    static int sNSeen = 0;
    for (int i = 0; i < sNSeen; i++) {
        if (sSeen[i][0] == inner && sSeen[i][1] == outer) {
            return;
        }
    }
    if (sNSeen >= kPaths) {
        return;
    }
    sSeen[sNSeen][0] = inner;
    sSeen[sNSeen][1] = outer;
    sNSeen++;
    Log("[replay] untagged frame=%lu cat=%u tris=%u inner=%p outer=%p", sFrameNum, sScene, tris, inner, outer);
}

void gdx3ds_replay_frame_bad(int reason) {
    if (!gdx3ds_replay_on || !sFrameOpen) {
        return;
    }
    MarkBad(reason == GDX_REPLAY_BAD_STEREO ? BAD_STEREO : BAD_TEXBACKED);
}

void gdx3ds_replay_draw_post(uint16_t tag, int isXf, uint32_t* w0, uint32_t* w1, const float* xfShadow) {
    if (!Recording()) {
        return;
    }
    GdxHwsScope hws(GDX3DS_HWS_RPLREC);
    if (tag == GDX_REPLAY_TAG_UNKNOWN) {
        MarkBad(BAD_UNTAGGED);
        return;
    }
    Found proj[4];
    Found pose[kXfPoseCount];
    memset(proj, 0, sizeof(proj));
    memset(pose, 0, sizeof(pose));
    bool sawProj = false, sawPose = false;
    const int projLoc = sProjLoc;
    const int xfLoc = sXfLoc;
    const bool ok = ScanUniformWords(w0, w1, [&](const UnifWord& u) {
        const int idx = u.index;
        if (projLoc >= 0 && idx >= projLoc && idx < projLoc + 4) {
            FoundAdd(&proj[idx - projLoc], u);
            sawProj = true;
        } else if (xfLoc >= 0 && idx >= xfLoc && idx < xfLoc + kXfUniformVec4s) {
            const int k = XfPoseIndex(idx - xfLoc);
            if (k >= 0) {
                FoundAdd(&pose[k], u);
                sawPose = true;
            }
        }
    });
    if (!ok) {
        MarkBad(BAD_SCAN);
        return;
    }
    {
        /* [diag] the uniform this draw really runs with: its own emission, else the last one */
        const uint16_t gov = isXf ? (sawPose ? tag : sXfTag) : (sawProj ? tag : sProjTag);
        sWin.govN++;
        if (gov != tag) {
            sWin.govMis++;
            static int sGovLogs = 0;
            if (sGovLogs < 16) {
                sGovLogs++;
                Log("[replay] gov mismatch frame=%lu tag=0x%04x gov=0x%04x xf=%d cat=%u sawProj=%d sawPose=%d", sFrameNum,
                    tag, gov, isXf, sScene, sawProj ? 1 : 0, sawPose ? 1 : 0);
            }
        }
    }
    if (sVerifyDone != 0 && sVfyDraws != nullptr && sDumpFrom != 0 && sFrameNum >= sDumpFrom && sFrameNum < sDumpFrom + 2) {
        const int j = sFDraw;
        const VfyDraw* r = j < sVfyDrawN ? &sVfyDraws[j] : nullptr;
        Log("[replay] seq frame=%lu vd=%d j=%d F tag=0x%04x cat=%u xf=%d ops=%d | R tag=0x%04x cat=%u xf=%u", sFrameNum,
            sVerifyDone, j, tag, sScene, isXf, sNOps, r ? r->tag : 0xFFFF, r ? r->cat : 99u, r ? r->xf : 9u);
    }
    if (sVerifyDone == 1 && sVfyDraws != nullptr) {
        CompareDraw(tag, isXf, sawProj, proj, sawPose, pose);
    } else if (sVerifyDone != 0) {
        sFDraw++;
    }
    if (sawProj) {
        if (isXf) {
            sProjTag = 0xFFFF; /* not this draw's: the next legacy draw re-emits it */
        } else {
            const int n0 = sNSites;
            RecordLegacy(tag, proj);
            if (sNSites > n0) {
                sLastProjSite = sNSites - 1;
            }
            sProjTag = tag;
        }
    }
    if (sawPose) {
        if (!isXf) {
            sXfTag = 0xFFFF;
        } else {
            const int n0 = sNSites;
            RecordXf(tag, pose, xfShadow);
            if (sNSites > n0) {
                sLastXfSite = sNSites - 1;
            }
            sXfTag = tag;
        }
    }
    if ((sDeep || sVerify) && sNDraws < kMaxDraws) {
        DrawGov& d = sDrawGov[sNDraws++];
        d.site = (int16_t)(isXf ? sLastXfSite : sLastProjSite);
        d.xf = (uint8_t)(isXf ? 1 : 0);
        d.cat = (uint8_t)sScene;
        d.tag = tag;
        memcpy(d.p, sVtx0, sizeof(d.p));
        d.rp = isXf ? nullptr : sRangeP;
        d.rn = isXf ? 0 : sRangeN;
        d.rsum = (sDeep && d.rp != nullptr) ? RangeSum(d.rp, d.rn) : 0;
    }
}

void gdx3ds_replay_frame_end_pre(void) {
    if (!gdx3ds_replay_on) {
        return;
    }
    sBackBufA = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    InstallCallback();
    sFramePresentPending = 1;
}

void gdx3ds_replay_frame_end_post(unsigned drawCalls) {
    if (!gdx3ds_replay_on || !sFrameOpen) {
        return;
    }
    sFrameOpen = 0;
    gdx3ds_replay_rec = 0; /* re-armed per frame */
    gdx3ds_replay_arm_interp(0);
    if (sVerifyPending) {
        /* the verified tick never walked a task: nothing was replayed */
        sVerifyPending = 0;
        CountCut(CUT_NOB);
        ResetTable();
        sNeedReset = 0;
        return;
    }
    if (sVerifyDone) {
        /* The verified tick's full render: read it back and compare with the replay image. */
        ReadbackTarget(sVfyF);
        ComparePixels();
        DumpPair();
        sWin.vFrames++;
        if (sVerifyDone == 1) {
            sWin.vAligned++;
        }
        sVerifyDone = 0;
        sTableValid = 0; /* a verified frame is never a replay source */
        return;
    }
    if (sNeedReset) {
        MarkBad(BAD_NOTASK);
    }
    if (drawCalls == 0 || sTableTasks == 0) {
        MarkBad(sTableTasks == 0 ? BAD_NOTASK : BAD_NODRAWS);
    }
    /* A's GX queue, before anything else can clear it. */
    gxCmdQueue_s* q = FrameQueue();
    const int n = q->numEntries;
    int screen = 0;
    const u32* cmdLo = Ctx()->cmdBuf;
    const u32* cmdHi = cmdLo + Ctx()->cmdBufSize / sizeof(u32);
    for (int i = 0; i < n && i < kMaxQueue; i++) {
        memcpy(sT->queue[i].d, &q->entries[i], sizeof(sT->queue[i].d));
        const uint32_t id = sT->queue[i].d[0] & 0xFFu;
        if (id == 0x01u) {
            const u32* list = (const u32*)(uintptr_t)sT->queue[i].d[1];
            if (list < cmdLo || list >= cmdHi) {
                MarkBad(BAD_GXCMD);
            }
        } else if (id == 0x03u) {
            if (IsScreenTransfer(sT->queue[i])) {
                screen++;
            } else {
                MarkBad(BAD_READBACK);
            }
        } else if (id != 0x02u && id != 0x04u) {
            MarkBad(BAD_GXCMD);
        }
    }
    sNQueue = n;
    if (screen != 1) {
        MarkBad(BAD_READBACK);
    }
    sWin.tableFrames++;
    sWin.sites += (unsigned)sNSites;
    sWin.entries += (unsigned)sNEntries;
    sWin.ops += (unsigned)sNOps;
    sWin.slotsN += (unsigned)sNSlots;
    sTableValid = sBad == BAD_NONE;
    if (!sTableValid) {
        sWin.bad[sBad]++;
    }
    if (sVerify && sDumpLeft > 0 && sTableValid && sVfyA != nullptr && sFrameNum + 2 >= sDumpFrom) {
        ReadbackTarget(sVfyA); /* dump only: A as rendered */
    }
}

/* ---- host loop ---- */

void gdx3ds_replay_iteration_begin(unsigned long frame, int skip) {
    if (!gdx3ds_replay_on) {
        return;
    }
    (void)frame;
    /* A rendered frame is worth recording only when the slot after it can replay: a paced
     * steady slot that skips 1..dmax-1 ticks (D = 2, or D = 3: one replay held for two
     * vblanks), outside the stretches every slot is cut in (menus, pause, transitions, the
     * countdown, the finish, 2+ players). Verify records every frame. Decided after the
     * frameskip decision, before the walk and the build, so the frame
     * (gdx3ds_replay_frame_begin) and c5tick's hollow keeps (gdx3ds_replay_wants_tick) read
     * the same answer; an unrecorded frame costs nothing (hooks NULL, gdx3ds_replay_rec 0). */
    int skipRun = 0;
    const int k = (&gdx3ds_frameskip_replay_plan != nullptr) ? gdx3ds_frameskip_replay_plan(&skipRun) : 0;
    /* the last iteration's end state is the tick this iteration's walk draws */
    const uint8_t pre = (k >= 1 && k <= sMaxSkips) ? StaticCut(sEnd[0].game) : CUT_UNARMED;
    sArmCutIter = pre;
    sArmIter = !skip && (sVerify || pre == CUT_NONE);
    /* the tick built now is presented iff the next iteration is the slot's last skip */
    sWantIter = sVerify || (pre == CUT_NONE && skipRun + 1 == k);
}

int gdx3ds_replay_wants_tick(void) {
    return gdx3ds_replay_on && sWantIter;
}

void gdx3ds_replay_iteration_end(unsigned long frame, int skip) {
    if (!gdx3ds_replay_on) {
        return;
    }
    sFrameNum = frame;
    sWin.iters++;
    const uint32_t epoch = (&gdx_interp_cut_epoch != nullptr) ? gdx_interp_cut_epoch() : 0;
    sEnd[1] = sEnd[0];
    sEnd[0].wanted = sWantIter;
    if (skip) {
        sWin.skips++;
        sSkipsSinceA++;
        gdx3ds_replay_game_capture(&sGameB);
        sEnd[0].game = sGameB;
        /* The walk runs before its iteration's tick, so each iteration's walk draws the tick
         * the iteration before it built. The tick a replay presents is the last one the slot
         * dropped: built two iterations before the next render -- A's own iteration at D = 2
         * (its pool is the other one, intact: this skip wrote A's), the first skip at D = 3
         * (it rewrote A's pool; the second skip wrote the other). Its matrices are that pool at
         * replay time, its game state that iteration's end capture. Epoch and APT are this
         * skip's (nothing may change from A to the slot). */
        sGameBTick = sEnd[1].game;
        sBWanted = sEnd[1].wanted;
        sBPool = sGameBTick.curPool;
        sBSamePool = (sSkipsSinceA & 1) == 0;
        sEpochB = epoch;
        sAptB = sAptCount;
        sHaveB = 1;
    } else {
        sWin.renders++;
        gdx3ds_replay_game_capture(&sGameA);
        sEnd[0].game = sGameA;
        sGameWalkA = sGameWalk; /* the state A's walk drew (the tick before) */
        sFrameA = frame;
        sEpochA = epoch;
        sAptA = sAptCount;
        sHaveA = 1;
        sHaveB = 0;
        sSkipsSinceA = 0;
    }
    if (sWin.iters >= kWindow) {
        EmitWindow();
    }
}

/* ---- steady: present the skipped tick at the slot's first vblank ---- */

void gdx3ds_replay_slot_begin(int d, int beginMissed, int catchup) {
    if (!gdx3ds_replay_on || sVerify) {
        return;
    }
    (void)d;
    if (!sHaveB) {
        return; /* no skipped tick since the last render (D = 1): nothing to replay */
    }
    sWin.slots++;
    const uint64_t t0 = svcGetSystemTick();
    uint8_t cut = DecideCut(false);
    if (cut == CUT_NONE && (beginMissed || catchup != 0)) {
        cut = CUT_CADENCE;
    }
    if (cut == CUT_NONE && !LastSwapLatched()) {
        cut = CUT_FENCE; /* A's swap has not latched: the back buffer is still on screen */
    }
    PatchStats ps;
    uint64_t patchTicks = 0;
    if (cut == CUT_NONE && !PrepareB(&ps, &patchTicks)) {
        cut = CUT_MATH;
    }
    sHaveB = 0;
    sTableValid = 0; /* 1:1 -- one replay per rendered frame */
    if (cut != CUT_NONE) {
        CountCut(cut);
        return;
    }
    const uint64_t ts = svcGetSystemTick();
    gxCmdQueue_s* q = FrameQueue();
    gxCmdQueueWait(q, -1);
    gxCmdQueueStop(q);
    gxCmdQueueClear(q);
    AddQueue(false, gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL));
    sReplaySwapPending = 1;
    gxCmdQueueRun(q);
    RedirtyPatchedUniforms();
    const uint64_t t1 = svcGetSystemTick();
    sWin.replays++;
    sWin.patchTicks += patchTicks;
    sWin.submitTicks += t1 - ts;
    sWin.pTicks += t1 - t0;
    if (t1 - t0 > sWin.pMax) {
        sWin.pMax = t1 - t0;
    }
}

void gdx3ds_replay_before_present(void) {
    if (!gdx3ds_replay_on || !sLastSwapWasReplay) {
        return;
    }
    /* A replay swapped after the vblank this present was held to: wait for it to latch, or this
     * frame's transfer would write the buffer still being scanned out. */
    if (sReplaySwapPending) {
        gxCmdQueueWait(FrameQueue(), -1);
    }
    if (!LastSwapLatched()) {
        sWin.fenceWaits++;
        C3D_FrameSync();
    }
}

/* ---- verify lane ---- */

int gdx3ds_replay_verify_take(void) {
    if (!gdx3ds_replay_on || !sVerify || !sTableValid) {
        return 0;
    }
    if (&gdx3ds_c5tick_hollow_pending != nullptr && gdx3ds_c5tick_hollow_pending()) {
        /* c5tick built this tick hollow for a skip: it has no full render to compare against
         * (the governor re-skips it). Hollow ticks are proven by [debug] replay_hash instead. */
        sWin.vfyHollow++;
        return 0;
    }
    sVerifyPending = 1;
    return 1;
}

static void ReplayTaskBegin(const float* p16, const float* mvStack16x11, int mvSize, const float* mp16) {
    sInterpMp = mp16;
    if (!gdx3ds_replay_on || !sFrameOpen) {
        return;
    }
    if (sVerifyPending) {
        /* B's display list is built and its frame is open, nothing drawn yet: A's command
         * list, VBOs and queue copy are intact. Replay A with B's poses, read it back. */
        sVerifyPending = 0;
        gdx3ds_replay_game_capture(&sGameB);
        sBPool = sGameB.curPool;
        sEpochB = (&gdx_interp_cut_epoch != nullptr) ? gdx_interp_cut_epoch() : 0;
        sAptB = sAptCount;
        sHaveB = 1;
        sWin.slots++;
        uint8_t cut = DecideCut(true);
        PatchStats ps;
        uint64_t patchTicks = 0;
        if (cut == CUT_NONE && !PrepareB(&ps, &patchTicks)) {
            cut = CUT_MATH;
        }
        if (cut == CUT_NONE) {
            gxCmdQueue_s* q = FrameQueue();
            AddQueue(true, nullptr);
            gxCmdQueueRun(q);
            gxCmdQueueWait(q, -1);
            ReadbackTarget(sVfyR);
            gxCmdQueueStop(q);
            gxCmdQueueClear(q);
            RedirtyPatchedUniforms();
            Ctx()->flags |= kDirtyAll;
            KeepVerifyPoses();
            sVerifyDone = 1;
            sWin.replays++;
            sWin.patchTicks += patchTicks;
        } else {
            CountCut(cut);
        }
        ResetTable();
        sNeedReset = 0;
    } else if (sNeedReset) {
        ResetTable();
        sNeedReset = 0;
    }
    if (sTableTasks == 0) {
        gdx3ds_replay_game_capture(&sGameWalk);
    }
    sTableTasks++;
    if (sBad != BAD_NONE) {
        return;
    }
    /* The task's starting matrix state is a constant of the frame (RESET op). */
    if (sNResets >= kMaxResets || sNOps >= kMaxOps || sInterpMp == nullptr) {
        MarkBad(BAD_OVERFLOW);
        return;
    }
    ResetState& r = sT->resets[sNResets];
    memcpy(r.p, p16, sizeof(r.p));
    memcpy(r.mv, mvStack16x11, sizeof(r.mv));
    r.size = mvSize;
    memcpy(r.mp, sInterpMp, sizeof(r.mp));
    Op& op = sT->ops[sNOps++];
    op.kind = OP_RESET;
    op.params = 0;
    op.slot = -1;
    op.cnst = -1;
    op.arg = (uint16_t)sNResets++;
    memcpy(sP, r.p, sizeof(sP));
    memcpy(sMv, r.mv, sizeof(sMv));
    sMvSize = r.size;
    memcpy(sMp, r.mp, sizeof(sMp));
    sInSky = 0;
    sScene = 0;
    for (int i = 0; i < GDX_REPLAY_VTX_SLOTS; i++) {
        gdx3ds_replay_vtx_tag[i] = GDX_REPLAY_TAG_UNKNOWN;
    }
}

static void ReplayVtxSrc(const void* src, unsigned n, unsigned dest, int isBig) {
    if (!Recording()) {
        return;
    }
    uint32_t off = 0;
    const int pool = PoolOf(src, &off);
    for (unsigned i = 0; i < n && dest + i < GDX_REPLAY_VTX_SLOTS; i++) {
        const uint32_t o = off + i * 16u;
        gdx3ds_replay_vtx_src[dest + i] = (pool >= 0 && o + 16u <= sGameWalk.poolSize)
                                              ? (o | ((uint32_t)pool << 30) | (isBig ? 0x80000000u : 0u))
                                              : GDX_REPLAY_SRC_NONE;
    }
    if (!sDeep) {
        return;
    }
    if (pool < 0) {
        sWin.vsOther[sScene & 15u]++;
        return;
    }
    if (sNVSrc < kMaxVSrc) {
        VSrc& v = sVSrc[sNVSrc++];
        v.off = off;
        v.n = (uint16_t)n;
        v.pool = (uint8_t)pool;
        v.cat = (uint8_t)sScene;
    }
}

static const GdxReplayInterpHooks kInterpHooks = {
    ReplayTaskBegin, ReplayOpMtx, ReplayOpPop, gdx3ds_replay_cur_tag, gdx3ds_replay_set_draw_tag, ReplayInSky, ReplayVtxSrc, gdx3ds_replay_out_vtx,
};

void gdx3ds_replay_arm_interp(int on) {
    gdx3ds_replay_interp = on ? &kInterpHooks : 0;
}

} // extern "C"
