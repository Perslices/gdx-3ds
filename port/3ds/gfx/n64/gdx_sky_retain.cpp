/* port/3ds/gfx/n64/gdx_sky_retain.cpp -- [native] sky_retain / skyclock: the owned
 * interpreter's side of the sky (c5sky-progress.md).
 *
 * SECTION MARKERS AND THE CENSUS ([native] skyclock=1). Background_Draw writes a p=0x65 G_NOOP at
 * each of its sections (gdx3ds_sky_capture.c). At every one the census charges the section that
 * was open with what the interpreter did since the last marker: wall ticks, commands walked,
 * TMEM loads and bytes, texture imports and decodes, vertices, triangles (and how many entered
 * the clipper), rectangles, batches, backend draws, and -- with gputrace -- the [prof] VTX/TRI/
 * IMP/DRW/MTX self time. One [skycnt] line per section per 64 tasks, per sky-drawing task.
 *
 * THE RETAINED REPLAY ([native] sky_retain=1). The capture moved Background_Draw's recordable
 * commands into a per-GfxPool record behind p=0x65 RUN markers. SkyReplayRun executes each run
 * at its marker through Exec -- the walker's own handlers, same order, same stream position --
 * and on top of that:
 *
 *  1. DEFERRED TEXTURE STATE. A run's texture-state commands (SETTIMG, SETTILE, SETTILESIZE,
 *     LOADBLOCK, LOADTILE, LOADTLUT) are queued as GROUPS (maximal consecutive runs of them)
 *     instead of executed. The queue is MATERIALIZED -- its groups executed, in order -- right
 *     before anything that reads texture state: a triangle that survives clipping (the SpTri
 *     hook, just before ResolveDrawState), any command other than a matrix, a vertex load or a
 *     triangle, and the end of the run. Matrices and vertex loads read only RSP state; a
 *     triangle the clipper empties never reaches ResolveDrawState.
 *     A group that is followed, with no materialization between, by a group with the IDENTICAL
 *     words except the SETTIMG operands is DEAD: everything it writes, the later group writes
 *     again with the same values (tiles, the TLUT staging range, the same TMEM span), so only
 *     the later one is executed. The background sprites are exactly this: every sprite queues
 *     [TLUT + 64x64 CI4 block] and ~5 of the 6 are behind the camera or off screen, so their
 *     quads clip to nothing and their loads were pure cost. The rule is restricted to groups
 *     holding a LOADTLUT and a LOADBLOCK (the TLUT re-arms textures_changed unconditionally)
 *     and is checked after the fact: an unresolved operand or a source shorter than its load
 *     executes the dead groups after all (`fallback`). Verify mode proves it per frame.
 *  2. RESOLVED ONCE. The texture key's source hash (FNV over the whole texture: the 4 KiB floor,
 *     the 2 KiB clouds, each 2 KiB CI4 sprite) and the CI palette fold are memoised per source,
 *     keyed on (address, length, the capture's sky identity generation, the source generation
 *     stamp for non-o2r sources, 8+8 sentinel bytes) and on the staged palette bytes. A course
 *     change, the ending, records.c or a new night palette set moves the generation.
 *  3. The repeated 28-vertex skybox load after the sprites (same matrix, same bytes) is served
 *     from the first load's transformed vertices when every T&L input is identical.
 * Draw order, blending, batching and every backend call are the stock path's. Level 2 (verify)
 * executes the stock path and checks every decision above against it (`vfy ... bad=`).
 *
 * Everything here runs on the drawing thread (the walker's noop hook). */
#include "gdx_interp.h"
#include "gdx3ds_replay.h" // [native] replay (C6): memoised vertex loads keep their tags

#include <new>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_sky_record.h"
#include "gdx3ds_xf_census.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx3ds_hotcold.h"

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

constexpr double kMsPerTick = 1.0 / 268111.856; /* svcGetSystemTick: the ARM11 clock */
constexpr int kCensusLines = GDXSKY_SEC_END; /* line 0 the header, 1..7 the sections */
constexpr int kReceiptLine = kCensusLines;      /* the [skyr] line */
constexpr int kOpsLine = kReceiptLine + 1;      /* the [skyop] line (skyclock=2) */
constexpr int kLastLine = kOpsLine + 1;         /* resets the window */
constexpr int kMaxGroups = 16;
constexpr int kMaxKilled = 32;
constexpr unsigned kHashSlots = 32; /* power of two; the sky hashes <= 10 distinct sources */
constexpr uint32_t kSentinel = 8;
constexpr unsigned kTmemSlots = 8; /* power of two; slot = source address (every sky load lands at TMEM 0) */
constexpr unsigned kVtxSlots = 4;  /* power of two; slot = source address */
/* Below these sizes the memo's own bookkeeping costs more than the work it saves (measured: the
 * 128-byte gradient hash, the sprites' 4-vertex loads). */
constexpr uint32_t kMinMemoBytes = 1024;
constexpr size_t kMinMemoVertices = 8;
constexpr int kProfVtx = 2, kProfTri = 3, kProfImp = 4, kProfDrw = 5, kProfMtx = 6; /* GDX3DS_PROF_* */
constexpr int kProfSections = Interp::kSkyProfSections;

const char* const kSectionName[GDXSKY_SEC_COUNT] = { "none",   "head",  "grad", "stars", "sprites",
                                                     "floor", "fwork", "cloud", "end" };

bool IsDeferredOp(uint8_t op) {
    return op == OP_SETTIMG || op == OP_SETTILE || op == OP_SETTILESIZE || op == OP_LOADBLOCK || op == OP_LOADTILE ||
           op == OP_LOADTLUT;
}

bool IsTriOp(uint8_t op) {
    return op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD;
}

bool IsRectOp(uint8_t op) {
    return op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_FILLRECT;
}

/* Colour registers: read at emission, never by ResolveDrawState. */
bool IsColourOp(uint8_t op) {
    return op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETFILLCOLOR;
}

/* ---- census ------------------------------------------------------------------------------- */

struct SkySnap {
    uint64_t ticks;
    uint32_t cmds, loads, copyBytes, imports, decodes, vtx, tris, clipIn, rects, batches, draws, mtx;
    uint64_t vtxTicks, triTicks, impTicks, drwTicks, mtxTicks;
};

struct SkyAccum {
    uint64_t entries;
    uint64_t ticks;
    uint64_t cmds, loads, copyBytes, imports, decodes, vtx, tris, clipIn, rects, batches, draws, mtx;
    uint64_t vtxTicks, triTicks, impTicks, drwTicks, mtxTicks;
};

/* The [prof] section accumulators (gdx3ds_gpu_prof.c) through the glue's reader: the host
 * harness links no profiler. */
SkySnap TakeSnap(const Counters& st, Interp::SkyProfFn prof) {
    SkySnap s;
    uint64_t ticks[kProfSections] = {};
    uint32_t calls[kProfSections] = {};
    if (prof != nullptr) {
        prof(ticks, calls);
    }
    s.ticks = static_cast<uint64_t>(gdx_ninterp_now());
    s.cmds = st.cmds;
    s.loads = st.tmemLoads;
    s.copyBytes = st.tmemCopyBytes;
    s.imports = st.texImports;
    s.decodes = st.texMisses;
    s.vtx = st.vtx;
    s.tris = st.triCalls;
    s.clipIn = st.triClipIn;
    s.rects = st.rects;
    s.batches = st.batches;
    s.draws = calls[kProfDrw];
    s.mtx = calls[kProfMtx];
    s.vtxTicks = ticks[kProfVtx];
    s.triTicks = ticks[kProfTri];
    s.impTicks = ticks[kProfImp];
    s.drwTicks = ticks[kProfDrw];
    s.mtxTicks = ticks[kProfMtx];
    return s;
}

void Charge(SkyAccum& a, const SkySnap& from, const SkySnap& to) {
    a.entries++;
    a.ticks += to.ticks - from.ticks;
    a.cmds += to.cmds - from.cmds;
    a.loads += to.loads - from.loads;
    a.copyBytes += to.copyBytes - from.copyBytes;
    a.imports += to.imports - from.imports;
    a.decodes += to.decodes - from.decodes;
    a.vtx += to.vtx - from.vtx;
    a.tris += to.tris - from.tris;
    a.clipIn += to.clipIn - from.clipIn;
    a.rects += to.rects - from.rects;
    a.batches += to.batches - from.batches;
    a.draws += to.draws - from.draws;
    a.mtx += to.mtx - from.mtx;
    a.vtxTicks += to.vtxTicks - from.vtxTicks;
    a.triTicks += to.triTicks - from.triTicks;
    a.impTicks += to.impTicks - from.impTicks;
    a.drwTicks += to.drwTicks - from.drwTicks;
    a.mtxTicks += to.mtxTicks - from.mtxTicks;
}

/* ---- memo keys ---------------------------------------------------------------------------- */

uint32_t HashSlot(const uint8_t* addr, uint32_t n) {
    return ((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)) * 2654435761u) ^ (n * 40503u)) >> 27 &
           (kHashSlots - 1u);
}

uint32_t TmemSlot(const uint8_t* addr) {
    return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)) * 2654435761u) >> 29 & (kTmemSlots - 1u);
}

void Sentinel(const uint8_t* addr, uint32_t n, uint8_t head[kSentinel], uint8_t tail[kSentinel]) {
    const uint32_t take = n < kSentinel ? n : kSentinel;
    memset(head, 0, kSentinel);
    memset(tail, 0, kSentinel);
    memcpy(head, addr, take);
    memcpy(tail, addr + (n - take), take);
}

/* Every input TransformVertices reads, for a vertex load without lighting or texgen. */
struct VtxKey {
    float mp[4][4];
    uint32_t geometryMode;
    uint32_t extraGeometryMode;
    int16_t fogMul, fogOffset;
    uint16_t scaleS, scaleT;
    uint32_t curW, curH;
    int32_t widescreen, forceFixedAspect;
    const uint8_t* src;
    uint32_t n, dest;
    uint32_t isBig;
};

bool SameVertex(const LoadedVertex& a, const LoadedVertex& b) {
    return memcmp(&a.x, &b.x, sizeof(float) * 6) == 0 && memcmp(&a.color, &b.color, sizeof(Rgba)) == 0 &&
           a.clip_rej == b.clip_rej && a.clip_out == b.clip_out;
}

} // namespace

/* The texture state a group writes: the whole Rdp prefix up to first_tile_index (palettes,
 * texture_to_load, the span store, TMEM, the tiles, textures_changed). Verify only. */
static constexpr size_t kTexStateBytes = offsetof(Rdp, first_tile_index);

struct Interp::SkyGroup {
    uint16_t start, end;           /* record words */
    uint16_t chainBegin, chainEnd; /* the dead groups it replaced (killed[]) */
    bool eliminable;
};

struct Interp::SkyState {
    /* ---- census ---- */
    int openSection; /* GDXSKY_SEC_*: the section the last marker opened (NONE = outside the sky) */
    SkySnap at;      /* the counters when it opened */
    SkyAccum acc[GDXSKY_SEC_COUNT];
    uint32_t skyTasks; /* tasks that drew a sky this window */
    uint32_t tasks;
    bool taskHadSky;

    /* ---- the run being replayed ---- */
    const GdxSkyRecord* rec;
    uint32_t generation;
    int level; /* 1 retain, 2 verify */
    SkyGroup live[kMaxGroups];
    int nLive;
    SkyGroup killed[kMaxKilled];
    int nKilled;
    int openStart; /* the group being collected, -1 = none */
    int openEnd;

    /* verify: the texture state before the first queued group, and the stock result */
    uint8_t* s0;
    uint8_t* s1;
    bool haveS0;

    /* ---- memos ---- */
    struct SrcHash {
        const uint8_t* addr;
        uint32_t n, hash, generation;
        uint64_t stamp[2];
        uint8_t head[kSentinel], tail[kSentinel];
        uint8_t immutable, cls, filled;
    };
    SrcHash src[kHashSlots];
    struct PalHash {
        uint8_t bytes[512];
        uint32_t n, hash;
        uint8_t filled;
    };
    PalHash pal;
    struct TmemHash {
        uint16_t base;
        uint32_t span, hash, generation;
        const uint8_t* addr;
        uint32_t size, line, full;
        uint64_t stamp[2];
        uint8_t head[kSentinel], tail[kSentinel];
        uint8_t native, immutable, cls, filled;
    };
    TmemHash tmem[kTmemSlots]; /* by source address */
    /* The source generations sampled before each LOADBLOCK a sky run executed, per TMEM base:
     * the stamp a TMEM-hash memo entry is valid from. */
    struct LoadStamp {
        uint16_t base;
        const uint8_t* addr;
        uint64_t stamp[2];
        uint32_t task;
        uint8_t valid;
    };
    LoadStamp loadStamp[kTmemSlots]; /* by TMEM base */
    struct VtxMemo {
        VtxKey key;
        uint8_t bytes[kMaxVertices * 16];
        LoadedVertex out[kMaxVertices];
        bool filled;
    };
    VtxMemo vtx[kVtxSlots];
    /* verify: the memo's answer for the hash being computed */
    uint32_t pendingHash;
    bool pending;

    /* ---- receipt window ---- */
    struct Window {
        uint32_t tasks, runs, words, badRec;
        uint32_t groups, dead, execGroups, fallback, forced;
        uint32_t srcHit, srcMiss, srcInval, palHit, palMiss, tmemHit, tmemMiss, tmemInval, vtxHit, vtxMiss;
        uint32_t tmemNoStore[3]; /* not a mirrored span / no pre-copy stamp this task / UNKNOWN class */
        uint32_t vfyTexOk, vfyTexBad, vfyHashOk, vfyHashBad, vfyVtxOk, vfyVtxBad, vfyStateOk, vfyStateBad;
        uint32_t fastDraws;
        uint32_t fastRefused[6]; /* memo invalid / no open batch / import owed / viewport / scissor */
        uint64_t runTicks;
    } win;
    uint32_t sesBad;
    uint32_t taskSeq; /* tasks since start: a load stamp is valid in its own task only */
    /* [native] skyclock=2: per-opcode ticks of the replayed words (children included) */
    uint64_t opTicks[256];
    uint32_t opCalls[256];
};

bool Interp::SkyEnsure() {
    if (mSky == nullptr) {
        mSky = new (std::nothrow) SkyState();
        if (mSky == nullptr) {
            return false;
        }
        memset(static_cast<void*>(mSky), 0, sizeof(*mSky));
        mSky->openStart = -1;
    }
    return true;
}

void Interp::SkySetLevels(int retain, int clock, SkyProfFn prof) {
    mSkyRetain = retain;
    mSkyClock = clock;
    mSkyProf = prof;
}

void Interp::SkyClear() {
    if (mSky != nullptr) {
        memset(mSky->src, 0, sizeof(mSky->src));
        memset(mSky->tmem, 0, sizeof(mSky->tmem));
        memset(mSky->loadStamp, 0, sizeof(mSky->loadStamp));
        mSky->pal.filled = 0;
        memset(mSky->vtx, 0, sizeof(mSky->vtx));
    }
}

/* ---- census ------------------------------------------------------------------------------- */

/* Close the open section (charge it) and open `section`. */
GDX_COLD void Interp::SkyCensusSwitch(int section) {
    SkyState& s = *mSky;
    const SkySnap now = TakeSnap(mStats, mSkyProf);
    if (s.openSection > GDXSKY_SEC_NONE && s.openSection < GDXSKY_SEC_COUNT) {
        Charge(s.acc[s.openSection], s.at, now);
    }
    s.at = now;
    s.openSection = section == GDXSKY_SEC_END ? GDXSKY_SEC_NONE : section;
    s.taskHadSky = true;
}

void Interp::SkyMarker(uint32_t w0, uint32_t w1) {
    if (!SkyEnsure()) {
        return;
    }
    const uint32_t kind = (w0 >> 12) & 0xFu;
    const uint32_t index = w0 & 0xFFFu;
    if (kind == GDXSKY_MK_SECTION) {
        if (GDX_UNLIKELY(mSkyClock != 0)) {
            SkyCensusSwitch(static_cast<int>(index));
        }
    } else if (kind == GDXSKY_MK_RUN) {
        SkyReplayRun(reinterpret_cast<const void*>(static_cast<uintptr_t>(w1)), index);
    }
}

void Interp::SkyTaskEnd() {
    if (mSky == nullptr) {
        return;
    }
    mSky->tasks++;
    mSky->win.tasks++;
    mSky->taskSeq++;
    if (mSky->taskHadSky) {
        mSky->skyTasks++;
    }
    mSky->taskHadSky = false;
    mSky->openSection = GDXSKY_SEC_NONE;
}

/* ---- deferred texture state --------------------------------------------------------------- */

/* A group the dead-group rule may drop: SETTIMG / SETTILE / SETTILESIZE / LOADBLOCK / LOADTLUT
 * only, with at least one LOADTLUT (which re-arms textures_changed unconditionally) and one
 * LOADBLOCK. LOADTILE's line walk reads other tiles, so it never qualifies. */
bool Interp::SkyGroupEliminable(uint16_t start, uint16_t end) const {
    const GdxSkyWord* w = mSky->rec->words;
    bool tlut = false;
    bool block = false;
    for (uint16_t i = start; i < end; i++) {
        const uint8_t op = Opcode(w[i].w0);
        if (op == OP_LOADTILE) {
            return false;
        }
        tlut = tlut || op == OP_LOADTLUT;
        block = block || op == OP_LOADBLOCK;
    }
    return tlut && block;
}

/* Identical words except the SETTIMG operands. */
bool Interp::SkyGroupsMatch(const SkyGroup& a, uint16_t start, uint16_t end) const {
    const GdxSkyWord* w = mSky->rec->words;
    if (a.end - a.start != end - start) {
        return false;
    }
    for (uint16_t k = 0; k < end - start; k++) {
        const GdxSkyWord& x = w[a.start + k];
        const GdxSkyWord& y = w[start + k];
        if (x.w0 != y.w0) {
            return false;
        }
        if (Opcode(x.w0) != OP_SETTIMG && (x.w1 != y.w1 || x.w1hi != y.w1hi)) {
            return false;
        }
    }
    return true;
}

void Interp::SkyGroupOpen(int wordIndex) {
    SkyState& s = *mSky;
    if (s.openStart < 0) {
        if (GDX_UNLIKELY(s.nLive == 0 && s.level == 2)) {
            SkyVerifySnapshotS0();
        }
        s.openStart = wordIndex;
    }
    s.openEnd = wordIndex + 1;
}

/* The group being collected is complete: queue it, and retire the previous group if this one
 * rewrites everything it wrote. */
void Interp::SkyGroupClose() {
    SkyState& s = *mSky;
    if (s.openStart < 0) {
        return;
    }
    const uint16_t start = static_cast<uint16_t>(s.openStart);
    const uint16_t end = static_cast<uint16_t>(s.openEnd);
    s.openStart = -1;
    s.win.groups++;
    if (s.nLive >= kMaxGroups) {
        SkyMaterialize(); /* never drop state for want of a slot */
        s.win.forced++;
    }
    SkyGroup g;
    g.start = start;
    g.end = end;
    g.eliminable = SkyGroupEliminable(start, end);
    g.chainBegin = static_cast<uint16_t>(s.nKilled);
    g.chainEnd = static_cast<uint16_t>(s.nKilled);
    if (s.nLive > 0) {
        SkyGroup& prev = s.live[s.nLive - 1];
        if (g.eliminable && prev.eliminable && prev.chainEnd == s.nKilled && s.nKilled < kMaxKilled &&
            SkyGroupsMatch(prev, start, end)) {
            g.chainBegin = prev.chainBegin;
            s.killed[s.nKilled] = prev;
            s.nKilled++;
            g.chainEnd = static_cast<uint16_t>(s.nKilled);
            s.nLive--;
            s.win.dead++;
        }
    }
    s.live[s.nLive++] = g;
    mSkyPending = true;
}

void Interp::SkyExecWords(uint16_t start, uint16_t end) {
    Frame f;
    f.list.base = reinterpret_cast<const uint8_t*>(&mSky->rec->words[start]);
    f.list.limit = static_cast<size_t>(end - start);
    f.list.stride = static_cast<uint8_t>(sizeof(GdxSkyWord));
    f.list.isBig = false;
    f.list.isF3d = false;
    f.idx = 0;
    while (f.idx < f.list.limit) {
        const Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        SkyExecTextureOp(c, f);
    }
}

/* One texture-state command; a LOADBLOCK also leaves the source stamp it was copied under. */
void Interp::SkyExecTextureOp(const Cmd& c, Frame& f) {
    if (Opcode(c.w0) != OP_LOADBLOCK) {
        (void)Exec(c, f, 0);
        return;
    }
    uint64_t stamp[2];
    gdx_gfx_tmem_source_stamp(stamp); /* before the copy reads the source */
    (void)Exec(c, f, 0);
    const uint16_t base = mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index;
    if (base >= 512u) {
        return;
    }
    SkyState::LoadStamp& e = mSky->loadStamp[base & (kTmemSlots - 1u)];
    e.base = base;
    e.addr = mRdp.loaded_texture[base].addr;
    e.stamp[0] = stamp[0];
    e.stamp[1] = stamp[1];
    e.task = mSky->taskSeq;
    e.valid = 1;
}

/* After a group that replaced dead ones executed: did it write everything they wrote? Its
 * operands resolved (DpLoadTlut / DpLoadBlock return early on an unresolved source), and every
 * LOADBLOCK's source was readable for the whole load (MirrorToTmem clamps the copy to it). */
bool Interp::SkyGroupCovered(const SkyGroup& g, uint32_t unresolvedBefore) const {
    if (mStats.timgUnresolved != unresolvedBefore) {
        return false;
    }
    const GdxSkyWord* w = mSky->rec->words;
    for (uint16_t i = g.start; i < g.end; i++) {
        if (Opcode(w[i].w0) != OP_LOADBLOCK) {
            continue;
        }
        const uint32_t tile = C1(w[i].w1, 24, 3);
        const uint16_t base = mRdp.texture_tile[tile].tmem_index;
        if (base >= 512u || mRdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) {
            return false;
        }
        const LoadedTexture& lt = mRdp.loaded_texture[base];
        const uint32_t room = static_cast<uint32_t>(sizeof(mRdp.tmem)) - static_cast<uint32_t>(base) * 8u;
        const uint32_t need = lt.size_bytes < room ? lt.size_bytes : room;
        if (lt.addr == nullptr || DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(lt.addr)) < need) {
            return false;
        }
    }
    return true;
}

void Interp::SkyExecGroup(const SkyGroup& g) {
    SkyState& s = *mSky;
    const uint32_t unresolved = mStats.timgUnresolved;
    SkyExecWords(g.start, g.end);
    s.win.execGroups++;
    if (g.chainEnd == g.chainBegin || SkyGroupCovered(g, unresolved)) {
        return;
    }
    /* Not provably covering: execute what it replaced, in order, then it again -- exactly the
     * stock sequence's final state (its second pass rewrites everything its first wrote). */
    s.win.fallback++;
    for (int k = g.chainBegin; k < g.chainEnd; k++) {
        SkyExecWords(s.killed[k].start, s.killed[k].end);
    }
    SkyExecWords(g.start, g.end);
}

/* Execute the queue: the SpTri hook (a triangle is about to resolve its state), any command that
 * is not a matrix, a vertex load or a triangle, and the end of every run. */
void Interp::SkyMaterialize() {
    if (!mSkyPending || mSky == nullptr) {
        return;
    }
    SkyState& s = *mSky;
    mSkyPending = false;
    if (s.openStart >= 0) {
        SkyGroupClose(); /* a group still being collected is part of the queue */
        mSkyPending = false;
    }
    if (GDX_UNLIKELY(s.level == 2)) {
        SkyVerifyMaterialize();
    } else {
        for (int i = 0; i < s.nLive; i++) {
            SkyExecGroup(s.live[i]);
        }
    }
    s.nLive = 0;
    s.nKilled = 0;
    s.haveS0 = false;
}

/* ---- verify: the retained texture state against the stock one ------------------------------ */

bool Interp::SkyVerifyEnsure() {
    SkyState& s = *mSky;
    if (s.s0 == nullptr) {
        s.s0 = new (std::nothrow) uint8_t[kTexStateBytes];
        s.s1 = new (std::nothrow) uint8_t[kTexStateBytes];
    }
    return s.s0 != nullptr && s.s1 != nullptr;
}

GDX_COLD void Interp::SkyVerifySnapshotS0() {
    SkyState& s = *mSky;
    if (!SkyVerifyEnsure()) {
        return;
    }
    memcpy(s.s0, &mRdp, kTexStateBytes);
    s.haveS0 = true;
}

/* The span list is a set; its order depends on the history of swap-removes. */
static bool SameSpanSet(const Rdp& a, const Rdp& b) {
    if (a.tmem_span_list_n != b.tmem_span_list_n) {
        return false;
    }
    if (a.tmem_span_list_n == 0xFFFFu) {
        return true; /* both invalid: the per-word walk rebuilds it from tmem_span_base */
    }
    for (uint32_t i = 0; i < a.tmem_span_list_n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < b.tmem_span_list_n && !found; j++) {
            found = a.tmem_span_list[i] == b.tmem_span_list[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool SameRange(const Rdp& a, const Rdp& b, size_t from, size_t to) {
    return memcmp(reinterpret_cast<const uint8_t*>(&a) + from, reinterpret_cast<const uint8_t*>(&b) + from,
                  to - from) == 0;
}

/* Everything in the prefix except tmem_generation (a counter nothing reads) and the span list's
 * order. */
static bool SameTexState(const Rdp& a, const Rdp& b) {
    const size_t listAt = offsetof(Rdp, tmem_span_list);
    const size_t listEnd = offsetof(Rdp, tmem_span_list_n) + sizeof(uint16_t);
    const size_t tmemAt = offsetof(Rdp, tmem);
    const size_t genAt = offsetof(Rdp, tmem_generation);
    const size_t tileAt = offsetof(Rdp, texture_tile);
    return SameRange(a, b, 0, listAt) && SameSpanSet(a, b) && SameRange(a, b, tmemAt, genAt) &&
           SameRange(a, b, tileAt, kTexStateBytes);
    (void)listEnd;
}

/* Stock has executed every group. When the model dropped any, rebuild what retain mode would
 * have left -- restore the state before the first group, run the live groups exactly as
 * retain mode does -- compare, and put the stock state back. */
GDX_COLD void Interp::SkyVerifyMaterialize() {
    SkyState& s = *mSky;
    if (s.nKilled == 0 || !s.haveS0 || !SkyVerifyEnsure()) {
        return;
    }
    memcpy(s.s1, &mRdp, kTexStateBytes);
    memcpy(&mRdp, s.s0, kTexStateBytes);
    for (int i = 0; i < s.nLive; i++) {
        SkyExecGroup(s.live[i]);
    }
    Rdp* stock = reinterpret_cast<Rdp*>(s.s1);
    const bool same = SameTexState(*stock, mRdp);
    memcpy(&mRdp, s.s1, kTexStateBytes);
    if (same) {
        s.win.vfyTexOk++;
    } else {
        s.win.vfyTexBad++;
        s.sesBad++;
    }
}

/* ---- the source-hash / palette memo (ImportTexture while a sky run replays) ---------------- */

bool Interp::SkySrcHashLookup(const uint8_t* addr, uint32_t n, bool immutable, uint32_t* out) {
    SkyState& s = *mSky;
    const SkyState::SrcHash& e = s.src[HashSlot(addr, n)];
    s.pending = false;
    if (n < kMinMemoBytes) {
        return false;
    }
    if (e.filled == 0 || e.addr != addr || e.n != n || e.immutable != (immutable ? 1 : 0)) {
        s.win.srcMiss++;
        return false;
    }
    bool fresh = immutable || (e.generation == s.generation &&
                               gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(addr), n, e.cls, e.stamp) != 0);
    if (fresh) {
        uint8_t head[kSentinel];
        uint8_t tail[kSentinel];
        Sentinel(addr, n, head, tail);
        fresh = memcmp(head, e.head, kSentinel) == 0 && memcmp(tail, e.tail, kSentinel) == 0;
    }
    if (!fresh) {
        s.win.srcInval++;
        return false;
    }
    if (s.level == 1) {
        *out = e.hash;
        s.win.srcHit++;
        return true;
    }
    s.pendingHash = e.hash; /* verify: compared in SkySrcHashStore with the stock hash */
    s.pending = true;
    return false;
}

void Interp::SkySrcHashStore(const uint8_t* addr, uint32_t n, bool immutable, uint32_t h) {
    SkyState& s = *mSky;
    if (n < kMinMemoBytes) {
        return;
    }
    if (s.pending) {
        s.pending = false;
        if (s.pendingHash == h) {
            s.win.vfyHashOk++;
        } else {
            s.win.vfyHashBad++;
            s.sesBad++;
        }
    }
    SkyState::SrcHash& e = s.src[HashSlot(addr, n)];
    e.addr = addr;
    e.n = n;
    e.hash = h;
    e.generation = s.generation;
    e.immutable = immutable ? 1 : 0;
    e.cls = static_cast<uint8_t>(gdx_gfx_tmem_source_class(reinterpret_cast<uintptr_t>(addr), n));
    /* the stamp is taken now, after the hash read the bytes: a write landing during the read
     * leaves the live generation ahead of it and the next lookup re-hashes (never stale) */
    gdx_gfx_tmem_source_stamp(e.stamp);
    Sentinel(addr, n, e.head, e.tail);
    e.filled = 1;
}

/* The palette fold is a pure function of the staged bytes it folds. */
bool Interp::SkyPalHashLookup(const uint8_t* bytes, uint32_t n, uint32_t* out) {
    SkyState& s = *mSky;
    s.pending = false;
    if (s.pal.filled == 0 || s.pal.n != n || memcmp(s.pal.bytes, bytes, n) != 0) {
        s.win.palMiss++;
        return false;
    }
    if (s.level == 1) {
        *out = s.pal.hash;
        s.win.palHit++;
        return true;
    }
    s.pendingHash = s.pal.hash;
    s.pending = true;
    return false;
}

void Interp::SkyPalHashStore(const uint8_t* bytes, uint32_t n, uint32_t h) {
    SkyState& s = *mSky;
    if (s.pending) {
        s.pending = false;
        if (s.pendingHash == h) {
            s.win.vfyHashOk++;
        } else {
            s.win.vfyHashBad++;
            s.sesBad++;
        }
    }
    if (n > sizeof(s.pal.bytes)) {
        s.pal.filled = 0;
        return;
    }
    memcpy(s.pal.bytes, bytes, n);
    s.pal.n = n;
    s.pal.hash = h;
    s.pal.filled = 1;
}

/* ---- the TMEM-hash memo (the RGBA16 / IA / I classes hash TMEM, not the source) -------------- */

/* TMEM [base*8, base*8+span) is exactly the mirror of the live span's source: the tile starts at
 * the base of a live LOADBLOCK span, and that load copied at least `span` bytes (a later load
 * over any of these words would have retired the span; a skipped one wrote nothing). */
bool Interp::SkyTmemSpanMirrorsSource(uint16_t base, uint32_t span) const {
    if (base >= 512u || mRdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u) ||
        mRdp.tmem_span_load_kind[base] != 1u) {
        return false;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    const uint32_t room = static_cast<uint32_t>(sizeof(mRdp.tmem)) - static_cast<uint32_t>(base) * 8u;
    if (lt.addr == nullptr || span == 0 || span > room || span > lt.size_bytes ||
        span > static_cast<uint32_t>(mRdp.tmem_span_words[base]) * 8u) {
        return false;
    }
    return DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(lt.addr)) >= span;
}

bool Interp::SkyTmemHashLookup(uint16_t base, uint32_t span, uint32_t* out) {
    SkyState& s = *mSky;
    s.pending = false;
    if (span < kMinMemoBytes) {
        return false;
    }
    if (!SkyTmemSpanMirrorsSource(base, span)) {
        s.win.tmemMiss++;
        return false;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    SkyState::TmemHash& e = s.tmem[TmemSlot(lt.addr)];
    if (e.filled == 0 || e.base != base || e.span != span || e.addr != lt.addr || e.size != lt.size_bytes ||
        e.line != lt.line_size_bytes || e.full != lt.full_image_line_size_bytes || e.native != lt.native_rgba16 ||
        e.immutable != lt.immutable) {
        s.win.tmemMiss++;
        return false;
    }
    const uint8_t* bytes = mRdp.tmem + static_cast<uint32_t>(base) * 8u;
    uint8_t head[kSentinel];
    uint8_t tail[kSentinel];
    Sentinel(bytes, span, head, tail);
    const bool fresh = e.generation == s.generation && memcmp(head, e.head, kSentinel) == 0 &&
                       memcmp(tail, e.tail, kSentinel) == 0 &&
                       gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(e.addr), span, e.cls, e.stamp) != 0;
    if (!fresh) {
        s.win.tmemInval++;
        e.filled = 0;
        return false;
    }
    /* Unchanged since the older stamp, so unchanged since this task's load too: keep the newer. */
    const SkyState::LoadStamp& ls = s.loadStamp[base & (kTmemSlots - 1u)];
    if (ls.valid != 0 && ls.base == base && ls.addr == e.addr && ls.task == s.taskSeq) {
        e.stamp[0] = ls.stamp[0];
        e.stamp[1] = ls.stamp[1];
    }
    if (s.level == 1) {
        *out = e.hash;
        s.win.tmemHit++;
        return true;
    }
    s.pendingHash = e.hash;
    s.pending = true;
    return false;
}

void Interp::SkyTmemHashStore(uint16_t base, uint32_t span, uint32_t h) {
    SkyState& s = *mSky;
    if (span < kMinMemoBytes) {
        return;
    }
    if (s.pending) {
        s.pending = false;
        if (s.pendingHash == h) {
            s.win.vfyHashOk++;
        } else {
            s.win.vfyHashBad++;
            s.sesBad++;
        }
    }
    /* Only a span a sky run loaded in this task, whose pre-copy stamp is known. */
    const SkyState::LoadStamp& ls = s.loadStamp[base & (kTmemSlots - 1u)];
    if (!SkyTmemSpanMirrorsSource(base, span)) {
        s.win.tmemNoStore[0]++;
        return;
    }
    if (ls.valid == 0 || ls.base != base || ls.task != s.taskSeq || ls.addr != mRdp.loaded_texture[base].addr) {
        s.win.tmemNoStore[1]++;
        return;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    const unsigned cls = gdx_gfx_tmem_source_class(reinterpret_cast<uintptr_t>(lt.addr), span);
    if (cls == GDX_TMEMSRC_UNKNOWN) {
        s.win.tmemNoStore[2]++;
        return;
    }
    SkyState::TmemHash& e = s.tmem[TmemSlot(lt.addr)];
    e.base = base;
    e.span = span;
    e.hash = h;
    e.generation = s.generation;
    e.addr = lt.addr;
    e.size = lt.size_bytes;
    e.line = lt.line_size_bytes;
    e.full = lt.full_image_line_size_bytes;
    e.native = lt.native_rgba16;
    e.immutable = lt.immutable;
    e.cls = static_cast<uint8_t>(cls);
    e.stamp[0] = ls.stamp[0];
    e.stamp[1] = ls.stamp[1];
    Sentinel(mRdp.tmem + static_cast<uint32_t>(base) * 8u, span, e.head, e.tail);
    e.filled = 1;
}

/* ---- the vertex-load memo ------------------------------------------------------------------ */

/* SpVertex for a sky run: the same window check, resolution and T&L, except that a load whose
 * every T&L input equals the last one's copies that load's transformed vertices. */
void Interp::SkyVertex(const Cmd& c, const ListRef& hint) {
    const size_t n = C0(c.w0, 12, 8);
    const size_t dest = C0(c.w0, 1, 7) - n;
    const bool memoable = n >= kMinMemoVertices && dest + n <= static_cast<size_t>(mVtxSoftCap) &&
                          (mRsp.geometry_mode & (GM_LIGHTING | GM_TEXTURE_GEN)) == 0 && gdx3ds_xf_census_on == 0;
    if (!memoable) {
        SpVertex(n, dest, c, hint);
        return;
    }
    const size_t need = n * 16u;
    const uintptr_t p = mSrc.ResolveData(c, need, /*preferPhysical=*/true, reinterpret_cast<uintptr_t>(hint.base));
    if (p == 0 || DlSrc::ReadableBytes(p) < need) {
        SpVertex(n, dest, c, hint); /* the stock fallback vertices */
        return;
    }
    VtxKey key;
    memset(&key, 0, sizeof(key));
    memcpy(key.mp, mRsp.MP_matrix, sizeof(key.mp));
    key.geometryMode = mRsp.geometry_mode;
    key.extraGeometryMode = mRsp.extra_geometry_mode;
    key.fogMul = mRsp.fog_mul;
    key.fogOffset = mRsp.fog_offset;
    key.scaleS = mRsp.texture_scaling_factor.s;
    key.scaleT = mRsp.texture_scaling_factor.t;
    key.curW = mCfg.curW;
    key.curH = mCfg.curH;
    key.widescreen = mCfg.widescreen;
    key.forceFixedAspect = mCfg.forceFixedAspect;
    key.src = reinterpret_cast<const uint8_t*>(p);
    key.n = static_cast<uint32_t>(n);
    key.dest = static_cast<uint32_t>(dest);
    key.isBig = hint.isBig ? 1u : 0u;
    SkyState& s = *mSky;
    SkyState::VtxMemo& m = s.vtx[(static_cast<uint32_t>(p) >> 4) & (kVtxSlots - 1u)];
    const bool hit = m.filled && memcmp(&key, &m.key, sizeof(key)) == 0 &&
                     memcmp(m.bytes, reinterpret_cast<const void*>(p), need) == 0;
    ProfScope prof(mBackend, 2);
    if (dest + n > mVtxHwEnd) {
        mVtxHwEnd = static_cast<uint32_t>(dest + n);
    }
    mStats.vtx += static_cast<uint32_t>(n);
    if (hit && s.level == 1) {
        memcpy(&mRsp.loaded_vertices[dest], m.out, n * sizeof(LoadedVertex));
        if (gdx3ds_replay_interp != nullptr) {
            /* C6: the memoised slots carry this load's tag and source, as TransformVertices'
             * would (left alone they kept the previous loads' tags: mixed-tag sky triangles) */
            ReplayNoteVertexLoad(reinterpret_cast<const uint8_t*>(p), n, dest, hint.isBig);
        }
        s.win.vtxHit++;
        return;
    }
    TransformVertices(reinterpret_cast<const uint8_t*>(p), n, dest, hint.isBig);
    if (hit) {
        /* hotcold: the verify compare, out of line */
        [this, &s, &m, dest, n]() GDX_COLD {
        bool same = true;
        for (size_t i = 0; i < n && same; i++) {
            same = SameVertex(mRsp.loaded_vertices[dest + i], m.out[i]);
        }
        if (same) {
            s.win.vfyVtxOk++;
        } else {
            s.win.vfyVtxBad++;
            s.sesBad++;
        }
        }();
    } else {
        s.win.vtxMiss++;
    }
    m.key = key;
    memcpy(m.bytes, reinterpret_cast<const void*>(p), need);
    memcpy(m.out, &mRsp.loaded_vertices[dest], n * sizeof(LoadedVertex));
    m.filled = true;
}

/* ---- the run --------------------------------------------------------------------------------- */

bool Interp::SkyRunValid(const void* record, uint32_t runIndex) const {
    const GdxSkyRecord* rec = static_cast<const GdxSkyRecord*>(record);
    if (rec == nullptr || rec->magic != GDXSKY_MAGIC || runIndex >= rec->nRuns) {
        return false;
    }
    const GdxSkyRun& run = rec->runs[runIndex];
    return run.end <= rec->nWords && run.start <= run.end && run.mode >= 1 && run.mode <= 3;
}

void Interp::SkyReplayRun(const void* record, uint32_t runIndex) {
    GdxSkyRecord* rec = static_cast<GdxSkyRecord*>(const_cast<void*>(record));
    SkyState& s = *mSky;
    if (!SkyRunValid(rec, runIndex)) {
        s.win.badRec++;
        if (rec != nullptr && rec->magic == GDXSKY_MAGIC) {
            rec->badRecord++;
        }
        return;
    }
    const GdxSkyRun& run = rec->runs[runIndex];
    const uint64_t t0 = mSkyClock != 0 ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    s.rec = rec;
    s.generation = rec->generation;
    s.level = run.mode == 3 ? 1 : run.mode; /* 3 = retain + the PICA clips the sky (not exact) */
    s.nLive = 0;
    s.nKilled = 0;
    s.openStart = -1;
    s.haveS0 = false;
    s.win.runs++;
    s.win.words += run.end - run.start;
    /* The matrices and vertex loads resolve with the walker's list as their hint, as they would
     * have where the capture found them. */
    const ListRef hint = mSp > 0 ? mStack[mSp - 1].list : ListRef();
    mSkyActive = true;
    mSkyNoClip = run.mode == 3;
    mHsxLean = mHudSkyEmit == 1; /* [native] hudsky_emit: the run's drains take the lean draw tail */
    mHsxSky = mHudSkyEmit == 1 && s.level == 1 && HsxSkyOk(); /* ... and its triangles the compact SpTri */
    mHsxSkyVfy = mHudSkyEmit == 2;
    SkyReplayWords(run.start, run.end, hint);
    SkyGroupClose();
    SkyMaterialize();
    mHsxSky = false;
    mHsxSkyVfy = false;
    mHsxLean = false;
    mSkyNoClip = false;
    mSkyActive = false;
    s.rec = nullptr;
    if (GDX_UNLIKELY(mSkyClock != 0)) {
        s.win.runTicks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
    }
}

void Interp::SkyReplayWords(uint16_t start, uint16_t end, const ListRef& hint) {
    SkyState& s = *mSky;
    Frame f;
    f.list.base = reinterpret_cast<const uint8_t*>(&s.rec->words[start]);
    f.list.limit = static_cast<size_t>(end - start);
    f.list.stride = static_cast<uint8_t>(sizeof(GdxSkyWord));
    f.list.isBig = false;
    f.list.isF3d = false;
    f.idx = 0;
    Frame hintFrame;
    hintFrame.list = hint;
    hintFrame.idx = 0;
    bool drawChain = false; /* only draws of one kind / colour writes since the last draw of this run */
    uint32_t lastDrawKey = 0;
    while (f.idx < f.list.limit) {
        const int at = static_cast<int>(start + f.idx);
        const Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        const uint8_t op = Opcode(c.w0);
        /* The same draw kind: a FILLRECT (FILL-cycle combine), a TEXRECT per tile and a triangle
         * (the RSP's geometry mode and viewport) each resolve their own state -- the HUD replay's
         * rule (gdx_hud_replay.cpp), which SkyRectState mirrors. */
        const bool drawOp = IsRectOp(op) || IsTriOp(op);
        const uint32_t drawKey = IsTriOp(op) ? 0x05u : ((static_cast<uint32_t>(op) << 8) | C1(c.w1, 24, 3));
        mSkyRectFast = drawChain && drawOp && drawKey == lastDrawKey;
        if (drawOp) {
            drawChain = true;
            lastDrawKey = drawKey;
        } else if (!IsColourOp(op)) {
            drawChain = false;
        }
        const uint64_t opT0 = mSkyClock >= 2 ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        SkyReplayWord(c, f, hintFrame, op, at);
        if (GDX_UNLIKELY(mSkyClock >= 2)) {
            s.opTicks[op] += static_cast<uint64_t>(gdx_ninterp_now()) - opT0;
            s.opCalls[op]++;
        }
    }
    mSkyRectFast = false;
}

void Interp::SkyReplayWord(const Cmd& c, Frame& f, Frame& hintFrame, uint8_t op, int at) {
    if (IsDeferredOp(op)) {
        SkyGroupOpen(at);
        if (GDX_UNLIKELY(mSky->level == 2)) {
            SkyExecTextureOp(c, f); /* verify: stock order, the queue is only modelled */
        }
        return;
    }
    SkyGroupClose();
    if (op == OP_VTX) {
        SkyVertex(c, hintFrame.list);
    } else if (op == OP_MTX) {
        (void)Exec(c, hintFrame, 0);
    } else if (IsTriOp(op)) {
        if (mHsxSky && (op == OP_TRI1 || op == OP_TRI2) && HsxSkyOk()) {
            /* [native] hudsky_emit: == Exec's TRI1 / TRI2 arms, each SpTri through the compact path */
            mHsxSkyCalls++;
            HsxSkyTri(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                      static_cast<uint8_t>(C0(c.w0, 0, 8) / 2));
            if (op == OP_TRI2) {
                HsxSkyTri(static_cast<uint8_t>(C1(c.w1, 16, 8) / 2), static_cast<uint8_t>(C1(c.w1, 8, 8) / 2),
                          static_cast<uint8_t>(C1(c.w1, 0, 8) / 2));
            }
            return;
        }
        if (mHsxSky) {
            mHsxSkyStock++;
        }
        (void)Exec(c, f, 0); /* SpTri materializes the queue if a triangle resolves state */
    } else {
        SkyMaterialize();
        (void)Exec(c, f, 0);
    }
}

/* A draw that follows a draw of its own kind, with only colour-register writes between, reuses
 * the open batch's DrawState instead of re-resolving it: every input ResolveDrawState reads is
 * what it was then, and nothing is owed (no armed import on a used unit, no viewport/scissor
 * change). The same test as gdx_hud_replay.cpp HudRectState; level 2 resolves fresh and
 * compares. */
bool Interp::SkyRectState(bool isRect, DrawState& out) {
    SkyState& s = *mSky;
    int why = 0;
    if (!mTriMemoValid || mTriMemo.comb == nullptr) {
        why = 1;
    } else if (!mPackBatch || mBufNumTris == 0) {
        why = 2;
    } else if ((mTriMemo.comb->usedTextures[0] && mRdp.textures_changed[0]) ||
               (mTriMemo.comb->usedTextures[1] && mRdp.textures_changed[1])) {
        why = 3;
    } else if (memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0) {
        why = 4;
    } else if (memcmp(&mRdp.scissor, &mRs.scissor, sizeof(Rect)) != 0) {
        why = 5;
    }
    if (why != 0) {
        s.win.fastRefused[why]++;
        return false;
    }
    if (s.level == 1) {
        out = mTriMemo;
        s.win.fastDraws++;
        return true;
    }
    /* hotcold: level 2 (verify) resolves fresh and compares, out of line */
    return [this, &s, isRect, &out]() GDX_COLD -> bool {
    const DrawState memo = mTriMemo;
    DrawState fresh;
    if (!ResolveDrawState(isRect, fresh)) {
        return false;
    }
    const bool same = fresh.comb == memo.comb && fresh.prg == memo.prg && fresh.tm == memo.tm &&
                      memcmp(fresh.tex_width, memo.tex_width, sizeof(memo.tex_width)) == 0 &&
                      memcmp(fresh.tex_height, memo.tex_height, sizeof(memo.tex_height)) == 0 &&
                      memcmp(fresh.tex_width2, memo.tex_width2, sizeof(memo.tex_width2)) == 0 &&
                      memcmp(fresh.tex_height2, memo.tex_height2, sizeof(memo.tex_height2)) == 0 &&
                      memcmp(fresh.effective_tile, memo.effective_tile, sizeof(memo.effective_tile)) == 0 &&
                      fresh.usedTextures[0] == memo.usedTextures[0] && fresh.usedTextures[1] == memo.usedTextures[1] &&
                      fresh.numInputs == memo.numInputs && fresh.use_alpha == memo.use_alpha &&
                      fresh.use_fog == memo.use_fog && fresh.use_blend_color == memo.use_blend_color &&
                      memcmp(&fresh.clip, &memo.clip, sizeof(memo.clip)) == 0 && mBufNumTris != 0;
    if (same) {
        s.win.vfyStateOk++;
    } else {
        s.win.vfyStateBad++;
        s.sesBad++;
    }
    out = fresh;
    return true;
    }();
}

/* ---- receipts --------------------------------------------------------------------------------- */

GDX_COLD static int FormatSection(char* buf, size_t cap, const char* name, const SkyAccum& a, double inv) {
    const double ms = kMsPerTick * inv;
    return snprintf(buf, cap,
                    "[skycnt] %s n=%.2f ms=%.3f c=%.1f ld=%.2f kb=%.2f im=%.2f dc=%.3f v=%.1f t=%.1f clip=%.1f "
                    "r=%.1f b=%.2f d=%.2f mtx=%.2f | vtx=%.3f tri=%.3f imp=%.3f drw=%.3f mtxms=%.3f",
                    name, a.entries * inv, a.ticks * ms, a.cmds * inv, a.loads * inv, a.copyBytes * inv / 1024.0,
                    a.imports * inv, a.decodes * inv, a.vtx * inv, a.tris * inv, a.clipIn * inv, a.rects * inv,
                    a.batches * inv, a.draws * inv, a.mtx * inv, a.vtxTicks * ms, a.triTicks * ms, a.impTicks * ms,
                    a.drwTicks * ms, a.mtxTicks * ms);
}

GDX_COLD int Interp::SkyFormatCensus(char* buf, size_t cap, int line) {
    SkyState& s = *mSky;
    if (mSkyClock == 0 || s.skyTasks == 0) {
        return 0;
    }
    const double inv = 1.0 / static_cast<double>(s.skyTasks);
    if (line == 0) {
        uint64_t total = 0;
        for (int i = 1; i < GDXSKY_SEC_COUNT; i++) {
            total += s.acc[i].ticks;
        }
        return snprintf(buf, cap, "[skycnt] tasks=%u sky=%u total_ms=%.3f (per sky task)", s.tasks, s.skyTasks,
                        total * kMsPerTick * inv);
    }
    if (s.acc[line].entries == 0) {
        return 0;
    }
    return FormatSection(buf, cap, kSectionName[line], s.acc[line], inv);
}

/* [native] skyclock=2: the five costliest opcodes of the replayed words, ms per sky task. */
GDX_COLD int Interp::SkyFormatOps(char* buf, size_t cap) {
    SkyState& s = *mSky;
    if (mSkyClock < 2 || s.skyTasks == 0) {
        return 0;
    }
    const double inv = 1.0 / static_cast<double>(s.skyTasks);
    int n = snprintf(buf, cap, "[skyop] sky=%u", s.skyTasks);
    bool taken[256] = {};
    for (int k = 0; k < 6 && n > 0 && static_cast<size_t>(n) < cap; k++) {
        int best = -1;
        for (int op = 0; op < 256; op++) {
            if (!taken[op] && s.opCalls[op] != 0 && (best < 0 || s.opTicks[op] > s.opTicks[best])) {
                best = op;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = true;
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " %02X=%.3fms/%.1f", best, s.opTicks[best] * kMsPerTick * inv,
                      s.opCalls[best] * inv);
    }
    memset(s.opTicks, 0, sizeof(s.opTicks));
    memset(s.opCalls, 0, sizeof(s.opCalls));
    return n;
}

GDX_COLD int Interp::SkyFormatReceipt(char* buf, size_t cap) {
    const SkyState::Window& w = mSky->win;
    if (mSkyRetain == 0 || w.runs == 0 || w.tasks == 0) {
        return 0;
    }
    const double inv = 1.0 / static_cast<double>(w.tasks);
    return snprintf(buf, cap,
                    "[skyr] tasks=%u level=%d runs=%.2f words=%.1f runms=%.3f | defer groups=%.2f dead=%.2f exec=%.2f "
                    "fallback=%u forced=%u | src hit=%.2f miss=%.2f inval=%u pal hit=%.2f miss=%.2f tmem hit=%.2f miss=%.2f "
                    "inval=%u nostore=%u/%u/%u | vtx hit=%.2f miss=%.2f | fast=%.1f refused=%u/%u/%u/%u/%u | vfy tex=%u/%u hash=%u/%u vtx=%u/%u "
                    "state=%u/%u ses_bad=%u badrec=%u",
                    w.tasks, mSkyRetain, w.runs * inv, w.words * inv, w.runTicks * kMsPerTick * inv, w.groups * inv,
                    w.dead * inv, w.execGroups * inv, w.fallback, w.forced, w.srcHit * inv, w.srcMiss * inv, w.srcInval,
                    w.palHit * inv, w.palMiss * inv, w.tmemHit * inv, w.tmemMiss * inv, w.tmemInval, w.tmemNoStore[0],
                    w.tmemNoStore[1], w.tmemNoStore[2], w.vtxHit * inv,
                    w.vtxMiss * inv, w.fastDraws * inv, w.fastRefused[1], w.fastRefused[2], w.fastRefused[3],
                    w.fastRefused[4], w.fastRefused[5], w.vfyTexOk, w.vfyTexBad, w.vfyHashOk, w.vfyHashBad,
                    w.vfyVtxOk, w.vfyVtxBad, w.vfyStateOk, w.vfyStateBad, mSky->sesBad, w.badRec);
}

/* Line 0: the census header; 1..7: one per section that ran; then the replay receipt; the last
 * line resets both windows. */
GDX_COLD int Interp::SkyFormat(char* buf, size_t cap, int line) {
    if (mSky == nullptr || line < 0 || line > kLastLine) {
        return 0;
    }
    SkyState& s = *mSky;
    int n = 0;
    if (line < kCensusLines) {
        n = SkyFormatCensus(buf, cap, line);
    } else if (line == kReceiptLine) {
        n = SkyFormatReceipt(buf, cap);
    } else if (line == kOpsLine) {
        n = SkyFormatOps(buf, cap);
    } else {
        memset(s.acc, 0, sizeof(s.acc));
        s.skyTasks = 0;
        s.tasks = 0;
        memset(&s.win, 0, sizeof(s.win));
    }
    if (n <= 0) {
        return 0;
    }
    return static_cast<size_t>(n) < cap ? n : static_cast<int>(cap) - 1;
}

} // namespace gdxn
