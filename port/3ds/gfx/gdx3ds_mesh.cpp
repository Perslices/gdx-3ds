/* gdx3ds_mesh.cpp — native retained machine meshes (see gdx3ds_mesh.h and
 * docs/research/native/07-machines-plan.md / machines-progress.md).
 *
 * Registry: the 30 machine slots x 5 LODs of racer.c D_800CDDB0[] (LOD 6 is the empty
 * segment-7 list and stays on the interpreter). Each entry owns an 8-byte stub F3DGfx
 * list (G_ENDDL) the bridge emits in place of the converted list; the interpreter's G_DL
 * hook recognises the stub by address and draws the entry's mesh.
 *
 * Bake: lazy, at bridge lookup, from the built list through gdx3ds_mesh_walk.c; vertex
 * chunks reference the loaded asset-segment image in place (host-endian Vtx), indices
 * and material runs live on the regular heap (~100 B + 6 B/tri per mesh).
 *
 * Draw (native): latch the RSP state GfxSpVertex reads, T&L every mesh vertex once
 * (gdx3ds_vtx_math.h), then per material run: apply the run's RDP/RSP state through the
 * interpreter's own setters, resolve the draw state exactly as GfxSpTri1 does (same
 * ColorCombiner key, ImportTexture, sampler, shader program, blend/depth), reject /
 * clip / cull every triangle with the interpreter's rules, pack survivors in the PICA
 * layout and issue ONE indexed draw through the backend. Prim/env INHERIT runs read the
 * interpreter's current colours. State left behind equals what the real walk leaves.
 *
 * walk_only ([native] machines_walk_only=1) replays the mesh through the interpreter's own
 * vertex / triangle path instead — the M2 stepping stone that separates walk from T&L
 * regressions; it is also the per-run fallback whenever the native binder refuses a state
 * shape.
 *
 * Interpreter access goes through GdxInterpSeam (gdx3ds_interp_seam.h): the draw-time code
 * below is interpreter-neutral; GfxSpTri1's state resolution and GfxSpVertex's RSP latch are
 * the seam's BindMaterial / LatchVtxState (gdx3ds_seam_lus.cpp for the vendored LUS
 * interpreter, n64/gdx_seam_owned.cpp for the owned walker). Under the owned walker there is
 * no bridge conversion, so the G_DL hook meets the game's registered list itself and bakes it
 * lazily with the walker's own vertex resolver (gdx3ds_mesh_dl_hook_seam). */
#include "gdx3ds_mesh.h"
#include "gdx3ds_xfprof.h"
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_native_cfg.h"
#include "gdx3ds_replay.h" // C6 [replay]: the CPU mesh runs' x scale for the pose patch

#include <3ds.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_mesh_walk.h"
#include "gdx3ds_vbopack.h"
#include "gdx3ds_vtx_math.h"
#include "gdx3ds_xf_census.h"
#include "gdx3ds_stereo.h" /* n60finish: the stereo class row (the partitioned draw's overlap margin) */
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD slow paths off the per-mesh hot code

/* [native] cpuclip, owned by the interpreter glue (port/3ds/gfx/n64/gdx_rt_glue.cpp). Weak:
 * the DL-test harness links this library without the glue, and absent the lever the historical
 * behaviour (always clip) is the right default. */
extern "C" int gdx_ninterp_cpuclip(void) __attribute__((weak));

/* The ONE read of the lever on this path. [native] cpuclip is a whole-frame semantic, not an
 * interpreter-local one: with the emit loop clipping unconditionally, cpuclip=0 used to mean
 * "the owned interpreter hands PICA the unclipped triangles while the retained-mesh path still
 * Sutherland-Hodgman clips its own", i.e. two clip semantics inside one frame, which is not a
 * state the N64 can be in and not something an A/B can attribute. Read once per mesh draw in
 * NativeDraw and handed to BOTH the RunHasSurvivor screen and the emit loop: the two ran the
 * same arithmetic off two different reads of this lever after the merge, and the screen's read
 * was missing, so cpuclip=0 machines=1 dropped whole runs the emit loop would have drawn. */
static inline bool CpuClipEnabled() {
    return (&gdx_ninterp_cpuclip == nullptr) || gdx_ninterp_cpuclip() != 0;
}

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" const char* gdx3ds_config_get_string(const char* section, const char* key, const char* fallback)
    __attribute__((weak));
extern "C" int gdx3ds_config_loaded(void) __attribute__((weak));
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
/* C5 STAGE 0 [c5cat] (port/3ds/gdx3ds_mainprof.h): the native draw's own timing bucket. Weak:
 * the DL harness and the host machines harness link this file without the clock. */
extern "C" int gdx3ds_c5cat_enter(int cat) __attribute__((weak));
extern "C" void gdx3ds_c5cat_leave(int prev) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact) __attribute__((weak));
/* Render-thread identity (port/3ds/gdx3ds_renderthread.cpp). Weak: the DL-test harness and
 * the desktop unit builds link gdx3ds_gfx without it, and then main IS the drawing thread. */
extern "C" int gdx3ds_rt_mode(void) __attribute__((weak));
extern "C" int gdx3ds_rt_on_render_thread(void) __attribute__((weak));
extern "C" int gdx3ds_rt_task_rendering(void) __attribute__((weak));
/* racer.c: Gfx* D_800CDDB0[30 * 6] — machine model lists (5 LODs + the empty LOD 6). Weak:
 * the DL-test harness links gdx3ds_gfx without the game, and the seam entries now pull this
 * object in unconditionally (gdx3ds_seam_lus.cpp -> gdx3ds_mesh_dl_hook_seam). */
extern "C" void* D_800CDDB0[] __attribute__((weak));
/* n60 NRR machines (nrr/gdx3ds_nrr_mach.cpp, n64 object library): the cold record hooks. Weak:
 * the DL harness links this library without the NRR. */
extern "C" int gdx3ds_nrr_mach_recording(int entryIdx) __attribute__((weak));
extern "C" int gdx3ds_nrr_mach_rec_begin(int entryIdx, unsigned nRuns, const void* verts) __attribute__((weak));
extern "C" void gdx3ds_nrr_mach_rec_run(const Gdx3dsXfDraw* xf, const Gdx3dsVboPackAux* aux, const uint16_t* idx,
                                        uint32_t nIdx, uint8_t envRgbMask, uint8_t colEnv, uint8_t envA,
                                        int blendColor) __attribute__((weak));
extern "C" void gdx3ds_nrr_mach_rec_end(int need) __attribute__((weak));
extern "C" void gdx3ds_nrr_mach_frame_begin(void) __attribute__((weak));
extern "C" int gdx3ds_nrr_level(int cat) __attribute__((weak));
/* racer.c: Gfx* D_800CDD38[30] — each slot's texture-set list ([native] machines_texskip). */
extern "C" void* D_800CDD38[] __attribute__((weak));

namespace {

constexpr int kSlots = 30;
constexpr int kLodsPerSlot = 6;
constexpr int kNativeLods = 5;
constexpr int kEntries = kSlots * kLodsPerSlot;
/* [native] dress_retained (C5 Stage 3b): decoration body lists registered on first sight inside
 * the decoration scope. They live in sEntries AFTER the machine entries (so EnsureResident, the
 * retire queues and the material records treat them exactly like machines) but never in the
 * machine hash: Find(), the texskip probes and the bridge stubs only ever see machine lists. */
constexpr int kDressEntries = 112; /* nirace: + the span programs' geometry segments (ni_race_mesh) */
constexpr int kAllEntries = kEntries + kDressEntries;
constexpr uint8_t kDressSlot = 0xFF; /* Entry::slot of a dress entry (never a mask bit) */
constexpr int kMaxIdxScratch = GDX3DS_MESH_MAX_TRIS * 3 * 8; /* fan worst case 8 tris per clipped tri */
#ifdef N64_PRIM_DEPTH_MAX
constexpr float GDX3DS_N64_PRIM_DEPTH_MAX = (float)N64_PRIM_DEPTH_MAX;
#else
constexpr float GDX3DS_N64_PRIM_DEPTH_MAX = 32767.0f; /* interpreter.cpp: Flush() */
#endif

struct Mesh {
    Gdx3dsMeshRun* runs;
    Gdx3dsMeshChunk* chunks;
    uint16_t* tris;   /* nTris * 3 mesh-global vertex indices */
    uint8_t* vtxChunk; /* mesh-global vertex -> chunk index (walk_only replay) */
    /* [native] gpuxf: per run, the texture_scaling_factor its vertices were loaded with
     * (texS, texT pairs), and whether every non-empty run is uniform in it -- the shader
     * carries ONE scale per draw, so a run mixing chunk scales stays on the CPU. */
    uint16_t* xfRunTex;
    uint8_t xfScaleOk;
    /* [native] machines_retained: the entry's GPU-resident copy this mesh draws from
     * (BORROWED -- the Entry owns the linear buffer; see the retained block below).
     * resState: 0 unchecked, 1 resident, 2 refused (no linear memory). */
    const uint8_t* res;
    uint8_t resState;
    /* [native] machines_mattab: recorded material resolves, GDXSEAM_MATREC_WAYS per run (owned,
     * allocated on the first draw with the lever on; drawing thread only). */
    GdxSeamMatRec* matRecs;
    uint8_t matState; /* 0 unallocated, 1 allocated, 2 refused (no heap) */
    uint8_t matWays;  /* records per run (GDXSEAM_MATREC_WAYS; the far speck keeps more) */
    uint16_t nRuns, nChunks, nTris, nVerts;
    uint32_t magic;    /* kMeshMagic while live, poisoned at free (stale-draw detector) */
    Mesh* retireNext;  /* retire queue link; only the retire lock touches it */
};

constexpr uint32_t kMeshMagic = 0x4D534831u;  /* "MSH1" */
constexpr uint32_t kMeshPoison = 0xDEADBEEFu;

/* The 8-byte command the bridge emits in place of the converted list: a G_ENDDL packet in
 * the interpreter's own word layout (two uintptr_t words, as Fast::F3DGfx on this port). */
struct alignas(8) Stub {
    uintptr_t w0; /* G_ENDDL */
    uintptr_t w1;
    uint32_t entry;  /* index into sEntries */
    uint32_t magic;
};

struct Entry {
    const void* src; /* game's Gfx list (host-built, 16-byte packets) */
    Mesh* mesh;
    uint32_t gen;    /* generation the mesh was baked at */
    uint8_t failed;  /* bake failed at `gen` */
    uint8_t slot, lod;
    /* [native] machines_retained: the slot x LOD's GPU-resident vertex + index buffer (linear
     * memory, raw 16-byte Vtx then u16 indices). Owned by the entry, touched ONLY on the
     * drawing thread (DrawMesh, RetireDrain), replaced only when a rebake changes its bytes. */
    uint8_t* res;
    uint32_t resBytes;
};

constexpr uint32_t kStubMagic = 0x4D455348u; /* "MESH" */
constexpr uint32_t kEndDl = 0xDFu;

Stub sStubs[kEntries];
Entry sEntries[kAllEntries];
bool sRegistered = false;
int sRegisteredCount = 0;

/* ---- mesh lifetime: the free/draw ownership invariant ---------------------------------
 *
 * INVARIANT: a Mesh* that a render task may still dereference is never free()d, and a
 * baked Mesh* is only ever REPLACED in its Entry, never cleared while a valid mesh exists.
 *
 * Two threads reach this registry, and until 2026-09-23 nothing fenced them:
 *
 *   bake/free  With `[debug] bridgemain=1` (the hardware default) the bridge's G_DL
 *              conversion -- and with it gdx3ds_mesh_bridge_lookup -> LookupForDraw ->
 *              Bake -- runs on the SUBMITTING (game/main) thread, inside
 *              gdx3ds_rt_submit_task and deliberately BEFORE that call's backpressure
 *              wait, "so the bridge of N+1 overlaps the tail of render(N)"
 *              (gdx3ds_renderthread.cpp). Under `[native] interp=1` the owned walker's
 *              G_DL hook bakes on the render thread as well, so BOTH threads can bake.
 *   draw       DrawMesh runs on the render thread, replaying task N.
 *
 * gdx3ds_mesh_lists_rebuilt() (racer.c: machine select, the records screen, a custom
 * machine, func_8008D97C's all-30 rebuild) bumps sGeneration, so the next prepare rebakes
 * every entry -- freeing, in place, exactly the Mesh* the live render is walking.
 *
 * The fix has the same shape as the texture cache's render-thread ownership (M7,
 * gdx_texcache_request_clear / GdxDrainTextureCacheRequests, renderthread-progress.md):
 * the baking thread only RETIRES the superseded mesh onto a queue; the DRAWING thread
 * frees it, from RetireDrain() at a frame boundary, one drain later.
 *
 * Why one extra drain is enough. RetireDrain runs only on the drawing thread (the render
 * thread when it exists, else main), so every draw it has ever issued has returned by the
 * time it runs -- program order is the fence, exactly as it is for the texture cache. A
 * mesh is pushed onto `pending` only AFTER its Entry has been repointed, so no draw can
 * newly reach it afterwards. Draining in two steps (free what the previous drain stole,
 * then steal `pending`) leaves a full inter-frame gap between "no longer reachable" and
 * "freed", which also covers a mesh retired by the render thread itself mid-task.
 *
 * Bake() additionally holds sBakeLock: sWalkScratch is a single file-scope buffer and two
 * threads reading the same stale Entry would otherwise retire the same Mesh* twice (a
 * double free). Uncontended it is one atomic; it is only ever contended during a
 * list rebuild, which happens on screen transitions and never in steady state.
 *
 * `[native] machines_defer_free = 0` restores the pre-fix in-place free, so the race can
 * be A/B'd on one artifact. Receipt: `[native] meshlife ...` (overlap= is the measured
 * race window; stale= and unsafe= must stay 0).
 */
std::atomic<uint32_t> sGeneration{1};

struct MeshLock {
    LightLock l;
    MeshLock() { LightLock_Init(&l); }  /* static init: single-threaded, no lazy-init race */
};
MeshLock sRetireLock;
MeshLock sBakeLock;
struct RetireGuard {
    RetireGuard() { LightLock_Lock(&sRetireLock.l); }
    ~RetireGuard() { LightLock_Unlock(&sRetireLock.l); }
};
struct BakeGuard {
    BakeGuard() { LightLock_Lock(&sBakeLock.l); }
    ~BakeGuard() { LightLock_Unlock(&sBakeLock.l); }
};

Mesh* sRetirePending = nullptr; /* retired, not yet stolen  (retire lock) */
Mesh* sRetireStolen = nullptr;  /* stolen last drain, freed at the next (retire lock) */
int sDeferFree = 1;             /* [native] machines_defer_free */

/* Race receipt. DrawMesh publishes the mesh it is inside so a retire/free on the other
 * thread can see -- and count -- the overlap it would otherwise have corrupted. */
std::atomic<uintptr_t> sDrawingMesh{0};
std::atomic<uint32_t> sDrawingTid{0};
/* Atomic because both threads bump them and a lost update could hide a violation; relaxed
 * because they are counters, not a synchronisation mechanism. */
struct Life {
    std::atomic<unsigned> retired, freed, held, overlap, overlapSame, unsafeFree, staleDraw, bakeWait, inflight, lookupOvl, bakeOvl;
} sLife;
inline unsigned Bump(std::atomic<unsigned>& c) { return c.fetch_add(1u, std::memory_order_relaxed) + 1u; }
inline unsigned Get(const std::atomic<unsigned>& c) { return c.load(std::memory_order_relaxed); }
int sLifeLogs = 0;

/* GEOMLOSS: one-shot loudness for a truncated run. A renderer that silently stops drawing
 * under pool pressure is invisible on a console we can only observe through a log, so the
 * first truncation of the session names itself; poolTrunc on the [native] machines receipt
 * (pool=refused/trunc) stays the recurring signal. */
int sTruncLogged = 0;

/* pointer -> entry hash (open addressing, 512 slots for 150 entries) */
constexpr int kHashSize = 512;
int16_t sHash[kHashSize];

/* config (latched once the INI is parsed; -1 = not yet) */
int sEnabled = -1;
int sWalkOnly = 0;
int sVerify = 0;
uint32_t sSlotMask = 0xFFFFFFFFu;
uint32_t sLodMask = 0x1Fu;
int sVerbose = 0;
/* GEOMLOSS killswitch: [debug] meshtruncfix=0 restores the pre-fix mid-triangle break, which
 * leaves nIdx misaligned and draws the rest of the run from mismatched vertices. Test-only —
 * it exists so the corruption and the fix can be A/B'd from one binary. Never ship it at 0. */
int sTruncFix = 1;
/* GEOMLOSS test knob: [debug] meshcapverts=N caps the per-run vertex budget directly, so the
 * truncation path can be reached deterministically instead of by sweeping pool sizes until a
 * clipped machine happens to straddle the cap. 0 = off = whatever the pool actually offers. */
int sCapVerts = 0;
/* [native] gpuxf (default 0 even under all=1): GPU vertex transform of the retained meshes
 * (o3dsgpuxf-progress.md). gpuxf_maxerr is the float24 error-bound gate in 1/1000 px. */
int sGpuXf = 0;
int sGpuXfMaxErr = 125;
/* [native] machines_retained (default 0, needs gpuxf=1; C5 Stage 1, c5s1-progress.md): draw the
 * gpuxf meshes from a GPU-resident per-(slot, LOD) buffer with the FLX alpha in the shader, so
 * nothing is copied per frame. machines_retained_clip (default 0 = PARITY mode: keep gpuxf's
 * containment gate and survivor screen, exact against the CPU path; 1 = PERFORMANCE mode: the
 * PICA clips meshes that straddle the screen edge and every run is bound without the survivor
 * screen -- an intentional, tolerance-gated divergence at the screen edges). */
int sRetained = 0;
int sRetainedClip = 0;
/* [native] machines_mattab (default 0, needs gpuxf=1; C5 Stage 1b, c5s1b-progress.md): serve each
 * gpuxf run's material resolve from a per-(slot, LOD, run) record -- the resolved combiner,
 * shader, sampler and depth/blend targets and the texture-cache slot its import hit -- instead
 * of recomputing it (and re-hashing the texture's source bytes) every frame. 1 = use, 2 = verify
 * (the full path always runs and is compared with the record). */
int sMatTab = 0;
int sMatLive = 0; /* [native] machines_mattab_live (c5mach2) */
int sSpeck = 0;   /* [native] machines_speck (c5mach2) */
extern int sWalkFast; /* [native] machines_walkfast (c5mach2), defined with the texskip queries */
} // namespace
extern "C" {
int gdx3ds_xfprof_on = 0; /* [debug] xfprof (c5mach2): section profile of the gpuxf run path */
int gdx3ds_xfprof_inrun = 0;
uint64_t gdx3ds_xfprof_ticks[XFP_COUNT];
uint64_t gdx3ds_xfprof_last;
}
namespace {
/* hotcold: the [debug] xfprof bodies are out of line (cold); the guard stays inline. */
GDX_COLD void XfpStart() {
    gdx3ds_xfprof_last = svcGetSystemTick();
}
GDX_COLD void XfpMark(int section) {
    gdx3ds_xfprof_mark(section, svcGetSystemTick());
}
#define XFP_START()                                        \
    do {                                                   \
        if (GDX_UNLIKELY(gdx3ds_xfprof_on)) {              \
            XfpStart();                                    \
        }                                                  \
    } while (0)
#define XFP(section)                                                 \
    do {                                                             \
        if (GDX_UNLIKELY(gdx3ds_xfprof_on)) {                        \
            XfpMark((section));                                      \
        }                                                            \
    } while (0)
unsigned long sMatBytes = 0; /* live material-record bytes (receipt only) */

/* receipts */
unsigned long sFrame = 0;
struct Stats {
    unsigned draws, walkDraws, runs, verts, tris, rejected, allOut, culled, clipped, fanTris, bindFallbacks, poolRefused,
        poolTrunc, emptyRuns, idxMisaligned;
    unsigned bakes, bakeFails, rebakes;
    unsigned vfyDraws, vfyTris, vfyTriMismatch, vfyCountMismatch, vfyMatMismatch, vfyAuxDiff;
    uint64_t ticks, tnlTicks, bindTicks, emitTicks, drawTicks;
    /* [native] gpuxf: meshes drawn by the GPU transform / whole-mesh invisible (no T&L) /
     * refused back to the CPU path by reason; runs/tris/verts it drew; runs it handed to
     * the interpreter walk; vertices the survivor screen transformed; worst error bound. */
    unsigned xfMeshes, xfInvisible, xfRuns, xfTris, xfVerts, xfWalk, xfScreened, xfGateMiss;
    unsigned xfFbFlx, xfFbTgLin, xfFbLights, xfFbTexs, xfFbClip, xfFbErr, xfFbPool, xfFbParams;
    unsigned xfMaxErrMpx;
    uint64_t xfTicks;
    /* [native] machines_retained: meshes / runs drawn from resident buffers, FLX meshes whose
     * alpha the shader computed, perf-mode straddlers, runs bound without the survivor screen,
     * refusals (LUT not stock -> upload path; no linear memory), buffers built / reused on a
     * rebake whose bytes did not change / retired. */
    unsigned rrMeshes, rrRuns, rrFlx, rrStraddle, rrNoScreen, rrFbLut, rrFbAlloc;
    unsigned rrBuilds, rrReused, rrRetired;
    /* [native] machines_mattab: bind outcomes by GDXSEAM_MM_* (hits + miss reasons / verify
     * fulls), records (re)banked, misses of a record that had been valid (invalidations),
     * backend state calls the hits issued, verify compares / mismatches, runs refused a record
     * (no heap), and the material path's time (ApplyRunState + screen + bind) of bound runs. */
    unsigned mtOut[GDXSEAM_MM_RESULT_COUNT];
    unsigned mtBanked, mtInval, mtBinds, mtChecked, mtMismatch, mtNoHeap;
    unsigned mtLive; /* units imported live by a hit (machines_mattab_live) */
    unsigned mtKeyBits[10]; /* key misses by differing part (GdxSeamMatOutcome::keyBits bit order) */
    uint64_t mtBindTicks, mtScrTicks;
    /* ... and what reordering the bound runs of a mesh could save (analysis only; parity mode
     * draws in stock order): shader (texenv) / texture changes between consecutive bound runs
     * in stock order, the distinct-count floor if the opaque runs were free to move, and the
     * runs pinned by blending or decal depth mode. */
    unsigned ordRuns, ordPrgSw, ordTexSw, ordPrgMin, ordTexMin, ordPinned;
} sWin = {}, sTot = {};
int sBakeLogs = 0;
int sFailLogs = 0;
/* [native] dress_retained: set around a dress draw when fogged draws must stay on the CPU path. */
bool sDressCpuIfFog = false;

/* per-draw scratch (render thread only) */
Gdx3dsLoadedVtx sLoaded[GDX3DS_MESH_MAX_VERTS];
uint16_t sRemap[GDX3DS_MESH_MAX_VERTS];
uint16_t sIdx[kMaxIdxScratch];
Gdx3dsMeshWalkOut sWalkScratch;

GDX_COLD void Log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
        svcOutputDebugString(buf, len);
        if (&gdx3ds_filelog_write != nullptr) {
            gdx3ds_filelog_write(buf, len);
        }
    }
}

GDX_COLD void TruncLoud(uint32_t maxVerts, uint32_t nOut, uint32_t need, unsigned triCount, unsigned tri) {
    if (sTruncLogged) {
        return;
    }
    sTruncLogged = 1;
    Log("GEOMLOSS: machines run TRUNCATED — VBO pool cap %u verts reached at %u (+%u needed) on "
        "triangle %u of %u; the run now ends on a triangle boundary and the rest of it is NOT drawn",
        maxVerts, nOut, need, tri, triCount);
}

uint32_t ParseHex(const char* s, uint32_t fallback) {
    if (s == nullptr || *s == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    return (end != nullptr && end != s) ? (uint32_t)v : fallback;
}

GDX_COLD int EnabledLatch();

/* Returns the enabled flag, latching the INI keys once the loader says they are ready. */
int Enabled() {
    if (GDX_LIKELY(sEnabled >= 0)) {
        return sEnabled;
    }
    return EnabledLatch();
}

/* hotcold: the latch (once, at config-ready) off the per-lookup path. */
GDX_COLD int EnabledLatch() {
    if (&gdx3ds_config_get_int == nullptr) {
        sEnabled = 0;
        return 0;
    }
    if (&gdx3ds_config_loaded != nullptr && !gdx3ds_config_loaded()) {
        return 0; /* not latched yet: frames before config-ready run the legacy path */
    }
    sEnabled = gdx3ds_native_lever("machines", 0) != 0 ? 1 : 0; /* [native] all=1 turns it on */
    sWalkOnly = gdx3ds_config_get_int("native", "machines_walk_only", 0) != 0 ? 1 : 0;
    sVerify = gdx3ds_config_get_int("native", "machines_verify", 0) != 0 ? 1 : 0;
    /* Killswitch for the deferred-retire fix: 0 frees a superseded mesh in place, which is
     * the pre-2026-09-23 behaviour and races the render thread (meshrace-progress.md). */
    sDeferFree = gdx3ds_config_get_int("native", "machines_defer_free", 1) != 0 ? 1 : 0;
    sVerbose = gdx3ds_config_get_int("debug", "verbose", 0) != 0 ? 1 : 0;
    sTruncFix = gdx3ds_config_get_int("debug", "meshtruncfix", 1) != 0 ? 1 : 0;
    sCapVerts = gdx3ds_config_get_int("debug", "meshcapverts", 0);
    if (sCapVerts < 0) {
        sCapVerts = 0;
    }
    sGpuXf = gdx3ds_config_get_int("native", "gpuxf", 0) != 0 ? 1 : 0;
    if (sVerify || sWalkOnly || !gdx3ds_mesh_backend_xf_ready()) {
        sGpuXf = 0; /* both are CPU-path instruments; a shader without the layout cannot run it */
    }
    sGpuXfMaxErr = gdx3ds_config_get_int("native", "gpuxf_maxerr", 125);
    Log("[gpuxf] init gpuxf=%d maxerr=%dmpx shader=%d (ini latched; forced off under verify/walk_only)", sGpuXf,
        sGpuXfMaxErr, gdx3ds_mesh_backend_xf_ready());
    sRetained = (sGpuXf && gdx3ds_config_get_int("native", "machines_retained", 0) != 0) ? 1 : 0;
    sRetainedClip = (sRetained && gdx3ds_config_get_int("native", "machines_retained_clip", 0) != 0) ? 1 : 0;
    Log("[rr] init machines_retained=%d clip=%d (%s mode; needs gpuxf=1)", sRetained, sRetainedClip,
        sRetainedClip ? "performance" : "parity");
    sMatTab = sGpuXf ? gdx3ds_config_get_int("native", "machines_mattab", 0) : 0;
    if (sMatTab < 0 || sMatTab > 2) {
        sMatTab = 0;
    }
    Log("[mattab] init machines_mattab=%d (%s; needs gpuxf=1) rec=%u bytes/run", sMatTab,
        sMatTab == 2 ? "verify" : (sMatTab == 1 ? "use" : "off"), (unsigned)sizeof(GdxSeamMatRec));
    /* c5mach2 (b): a unit the record holds no import hit for (the race setup's non-immutable
     * tile 0) imports live -- the stock ImportTexture -- instead of missing the whole record. */
    sMatLive = (sMatTab != 0 && gdx3ds_config_get_int("native", "machines_mattab_live", 0) != 0) ? 1 : 0;
    Log("[mattab] init machines_mattab_live=%d (a non-immutable unit imports live; needs machines_mattab)",
        sMatLive);
    /* c5mach2 (a): the LOD-6 far speck (one list every slot shares) baked and drawn like a body
     * instead of walked; the same native path, so the same parity argument. */
    sSpeck = gdx3ds_config_get_int("native", "machines_speck", 0) != 0 ? 1 : 0;
    /* [native] nrr_mach draws the far speck from its record: the speck's first legacy draws must be
     * mesh draws (the cold record pass), as under machines_speck. */
    if (&gdx3ds_nrr_level != nullptr && gdx3ds_nrr_level(2 /* NRR_CAT_MACH */) != 0) {
        sSpeck = 1;
    }
    Log("[speck] init machines_speck=%d (the far speck drawn as a native mesh)", sSpeck);
    sWalkFast = gdx3ds_config_get_int("native", "machines_walkfast", 0) != 0 ? 1 : 0;
    Log("[walkfast] init machines_walkfast=%d (texskip's set-list lookup bounds-filtered)", sWalkFast);
    gdx3ds_xfprof_on = gdx3ds_config_get_int("debug", "xfprof", 0) != 0 ? 1 : 0;
    if (&gdx3ds_config_get_string != nullptr) {
        sSlotMask = ParseHex(gdx3ds_config_get_string("native", "machines_mask", "ffffffff"), 0xFFFFFFFFu);
        sLodMask = ParseHex(gdx3ds_config_get_string("native", "machines_lod_mask", "1f"), 0x1Fu);
    }
    Log("[native] machines=%d walk_only=%d verify=%d defer_free=%d truncfix=%d capverts=%d "
        "mask=%08lx lod_mask=%02lx (ini latched)",
        sEnabled, sWalkOnly, sVerify, sDeferFree, sTruncFix, sCapVerts,
        (unsigned long)sSlotMask, (unsigned long)sLodMask);
    return sEnabled;
}

inline uint32_t HashPtr(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return (uint32_t)((v >> 4) ^ (v >> 13) ^ (v >> 22)) & (kHashSize - 1);
}

GDX_COLD void EnsureRegistryBuild();

void EnsureRegistry() {
    if (GDX_LIKELY(sRegistered)) {
        return;
    }
    EnsureRegistryBuild();
}

/* hotcold: the one-time registry build off the per-lookup path. */
GDX_COLD void EnsureRegistryBuild() {
    sRegistered = true;
    memset(sHash, -1, sizeof(sHash));
    if (D_800CDDB0 == nullptr) {
        Log("[native] machines registry: no game lists linked (harness build)");
        return;
    }
    for (int slot = 0; slot < kSlots; slot++) {
        for (int lod = 0; lod < kNativeLods; lod++) {
            const int idx = slot * kLodsPerSlot + lod;
            const void* src = D_800CDDB0[idx];
            Entry& e = sEntries[idx];
            e.src = src;
            e.mesh = nullptr;
            e.gen = 0;
            e.failed = 0;
            e.slot = (uint8_t)slot;
            e.lod = (uint8_t)lod;
            e.res = nullptr;
            e.resBytes = 0;
            sStubs[idx].w0 = (uintptr_t)(kEndDl << 24);
            sStubs[idx].w1 = 0;
            sStubs[idx].entry = (uint32_t)idx;
            sStubs[idx].magic = kStubMagic;
            if (src == nullptr) {
                continue;
            }
            uint32_t h = HashPtr(src);
            while (sHash[h] >= 0) {
                h = (h + 1) & (kHashSize - 1);
            }
            sHash[h] = (int16_t)idx;
            sRegisteredCount++;
        }
    }
    Log("[native] machines registry: %d lists (%d slots x %d LODs)", sRegisteredCount, kSlots, kNativeLods);
}

Entry* Find(const void* src) {
    uint32_t h = HashPtr(src);
    for (int probe = 0; probe < kHashSize; probe++) {
        const int16_t idx = sHash[h];
        if (idx < 0) {
            return nullptr;
        }
        if (sEntries[idx].src == src) {
            return &sEntries[idx];
        }
        h = (h + 1) & (kHashSize - 1);
    }
    return nullptr;
}

void FreeMesh(Mesh* m) {
    if (m == nullptr) {
        return;
    }
    free(m->runs);
    free(m->chunks);
    free(m->tris);
    free(m->vtxChunk);
    free(m->xfRunTex);
    if (m->matRecs != nullptr) {
        sMatBytes -= (unsigned long)((m->nRuns > 0 ? m->nRuns : 1u) * m->matWays * sizeof(GdxSeamMatRec));
        free(m->matRecs);
    }
    m->magic = kMeshPoison; /* a draw that still holds this pointer reports itself */
    free(m);
}

inline uint32_t SelfTid() {
    u32 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    return (uint32_t)tid;
}

/* The one thread that runs DrawMesh: the render thread when there is one, else whatever
 * thread runs the frame. Only it may free a retired mesh; every other thread enqueues. */
bool OnDrawingThread() {
    if (&gdx3ds_rt_mode == nullptr || gdx3ds_rt_mode() == 0) {
        return true; /* sequential path: one thread bakes, draws and frees */
    }
    return &gdx3ds_rt_on_render_thread != nullptr && gdx3ds_rt_on_render_thread() != 0;
}

/* "This call is touching the registry from a thread that never draws, while a render TASK
 * is queued or executing" -- the bridgemain overlap, measured at the point of use. Every
 * such call is one the replay of that task could be racing through the same Entry. */
bool OffThreadWhileRendering() {
    return !OnDrawingThread() && &gdx3ds_rt_task_rendering != nullptr && gdx3ds_rt_task_rendering() != 0;
}

/* Hand a superseded mesh to the drawing thread. PRECONDITION: its Entry no longer points
 * at it, so no draw can newly reach it (see the ownership note above). */
GDX_COLD void RetireMesh(Mesh* m) {
    if (m == nullptr) {
        return;
    }
    /* STRUCTURAL window: this retire comes from a thread that never draws, while a render
     * TASK is queued or executing. Every one of these is a mesh the replay of that task can
     * still reach through its Entry -- i.e. exactly the free the pre-fix code performed in
     * place. It is the measurement that does not depend on catching the two threads inside
     * DrawMesh at the same instant. */
    if (OffThreadWhileRendering()) {
        const unsigned n = Bump(sLife.inflight);
        if (n <= 4) {
            Log("[native] meshlife INFLIGHT tick=%llu retire tid=%lu mesh=%p while a render task is live "
                "(defer=%d)",
                (unsigned long long)svcGetSystemTick(), (unsigned long)SelfTid(), (void*)m, sDeferFree);
        }
    }
    const uintptr_t drawing = sDrawingMesh.load(std::memory_order_acquire);
    if (drawing != 0) {
        /* Measured proof that the free/draw window is real: another thread is inside
         * DrawMesh right now. Harmless with the deferral on; a use-after-free with it off. */
        Bump(sLife.overlap);
        if (drawing == (uintptr_t)m) {
            Bump(sLife.overlapSame);
        }
        if (sLifeLogs < 8) {
            sLifeLogs++;
            Log("[native] meshlife OVERLAP tick=%llu retire tid=%lu mesh=%p | drawing tid=%lu mesh=%p same=%d "
                "defer=%d",
                (unsigned long long)svcGetSystemTick(), (unsigned long)SelfTid(), (void*)m,
                (unsigned long)sDrawingTid.load(std::memory_order_relaxed), (void*)drawing,
                drawing == (uintptr_t)m ? 1 : 0, sDeferFree);
        }
    }
    if (!sDeferFree) {
        if (drawing != 0) {
            Bump(sLife.unsafeFree); /* the pre-fix behaviour, kept only for the A/B */
        }
        FreeMesh(m);
        Bump(sLife.freed);
        return;
    }
    RetireGuard g;
    m->retireNext = sRetirePending;
    sRetirePending = m;
    Bump(sLife.retired);
    Bump(sLife.held);
}

/* Drawing thread, at a frame boundary: free what the previous drain stole, then steal the
 * current pending list. The two steps are what put a whole frame between "unreachable"
 * and "freed"; both run on the thread that issues every DrawMesh, so program order -- not
 * a guess about task overlap -- is what makes the free safe. */
void ResRetireDrain();

void RetireDrain() {
    if (!OnDrawingThread()) {
        return;
    }
    ResRetireDrain(); /* [native] machines_retained: the resident buffers' own two-step queue */
    Mesh* freeNow = nullptr;
    {
        RetireGuard g;
        freeNow = sRetireStolen;
        sRetireStolen = sRetirePending;
        sRetirePending = nullptr;
    }
    while (freeNow != nullptr) {
        Mesh* const next = freeNow->retireNext;
        if (sDrawingMesh.load(std::memory_order_acquire) != 0) {
            Bump(sLife.unsafeFree); /* must stay 0: a draw is live on some thread at free time */
        }
        FreeMesh(freeNow);
        Bump(sLife.freed);
        if (Get(sLife.held) != 0) {
            sLife.held.fetch_sub(1u, std::memory_order_relaxed);
        }
        freeNow = next;
    }
}

template <typename T> T* DupArray(const T* src, size_t n) {
    T* p = (T*)malloc(n * sizeof(T) + 1);
    if (p != nullptr && n != 0) {
        memcpy(p, src, n * sizeof(T));
    }
    return p;
}

/* Retire the mesh this bake supersedes. The Entry is repointed FIRST (here, or at the
 * successful publish below) and only then is the old pointer queued, so a concurrent draw
 * always loads a live allocation -- old or new, never null and never freed. */
inline void RetireOld(Entry& e, Mesh* old) {
    if (old != nullptr) {
        e.mesh = nullptr;
        RetireMesh(old);
    }
}

/* [native] gpuxf: record each run's texture_scaling_factor (the chunk scale its vertices were
 * loaded with) and whether every non-empty run is uniform in it. Static structure, so once per
 * bake. */
uint8_t XfComputeRunScales(Mesh* m) {
    uint8_t allOk = 1;
    for (unsigned r = 0; r < m->nRuns; r++) {
        const Gdx3dsMeshRun& run = m->runs[r];
        uint16_t s = 0, t = 0;
        bool have = false;
        for (unsigned k = 0; k < (unsigned)run.triCount * 3u; k++) {
            const Gdx3dsMeshChunk& ch = m->chunks[m->vtxChunk[m->tris[(size_t)run.firstTri * 3 + k]]];
            if (!have) {
                s = ch.texS;
                t = ch.texT;
                have = true;
            } else if (ch.texS != s || ch.texT != t) {
                allOk = 0;
            }
        }
        m->xfRunTex[r * 2 + 0] = s;
        m->xfRunTex[r * 2 + 1] = t;
    }
    return allOk;
}

/* [native] machines_texskip: which texel inputs a raw G_SETCOMBINE can read. Cycle 1's TEXEL0 /
 * TEXEL1 are the run's tile / tile+1; any texel input in cycle 2 is taken to need both (the
 * cycle-2 texel swap is the combiner's business, and a superset is always safe). */
struct TexelUse {
    bool c1t0, c1t1, c2any;
};
inline void NoteTexelInput(uint32_t v, bool isRgbC, bool& t0, bool& t1) {
    if (v == 1u || (isRgbC && v == 8u)) {
        t0 = true;
    }
    if (v == 2u || (isRgbC && v == 9u)) {
        t1 = true;
    }
}
TexelUse DecodeTexelUse(uint32_t w0, uint32_t w1) {
    const auto f0 = [w0](unsigned pos, unsigned width) { return (w0 >> pos) & ((1u << width) - 1u); };
    const auto f1 = [w1](unsigned pos, unsigned width) { return (w1 >> pos) & ((1u << width) - 1u); };
    TexelUse u = { false, false, false };
    /* cycle 1: rgb a b c d, alpha a b c d */
    NoteTexelInput(f0(20, 4), false, u.c1t0, u.c1t1);
    NoteTexelInput(f1(28, 4), false, u.c1t0, u.c1t1);
    NoteTexelInput(f0(15, 5), true, u.c1t0, u.c1t1);
    NoteTexelInput(f1(15, 3), false, u.c1t0, u.c1t1);
    NoteTexelInput(f0(12, 3), false, u.c1t0, u.c1t1);
    NoteTexelInput(f1(12, 3), false, u.c1t0, u.c1t1);
    NoteTexelInput(f0(9, 3), false, u.c1t0, u.c1t1);
    NoteTexelInput(f1(9, 3), false, u.c1t0, u.c1t1);
    /* cycle 2 */
    bool a = false, b = false;
    NoteTexelInput(f0(5, 4), false, a, b);
    NoteTexelInput(f1(24, 4), false, a, b);
    NoteTexelInput(f0(0, 5), true, a, b);
    NoteTexelInput(f1(6, 3), false, a, b);
    NoteTexelInput(f1(21, 3), false, a, b);
    NoteTexelInput(f1(3, 3), false, a, b);
    NoteTexelInput(f1(18, 3), false, a, b);
    NoteTexelInput(f1(0, 3), false, a, b);
    u.c2any = a || b;
    return u;
}

/* The tiles the mesh's runs can sample, following each run's tile and combiner from the state
 * the body inherits (the caller's live tile and combiner texel use) through every G_TEXTURE /
 * G_SETCOMBINE of the list. */
uint8_t ComputeReqTiles(const Mesh* m, int inhTile, TexelUse inhUse) {
    int tile = inhTile & 7;
    bool haveComb = true;
    TexelUse use = inhUse;
    uint8_t mask = 0;
    for (unsigned r = 0; r < m->nRuns; r++) {
        const Gdx3dsMeshRun& run = m->runs[r];
        if (run.texSet) {
            tile = run.tile & 7;
        }
        if (run.combineSet) {
            use = DecodeTexelUse(run.combineW0, run.combineW1);
            haveComb = true;
        }
        if (run.triCount == 0) {
            continue;
        }
        if (!haveComb || tile < 0) {
            return 0xFF;
        }
        const uint8_t t0 = (uint8_t)(1u << tile);
        const uint8_t t1 = (uint8_t)(1u << ((tile + 1) & 7));
        if (use.c1t0) {
            mask |= t0;
        }
        if (use.c1t1) {
            mask |= t1;
        }
        if (use.c2any) {
            mask |= (uint8_t)(t0 | t1);
        }
    }
    return mask;
}

GDX_COLD bool Bake(Entry& e, Gdx3dsMeshResolveFn resolve, void* user, const void* walkSrc = nullptr) {
    const uint64_t t0 = svcGetSystemTick();
    /* One baker at a time: sWalkScratch is a single file-scope buffer, and two threads
     * that both read the same stale Entry would retire the same Mesh* twice. */
    BakeGuard bakeGuard;
    const uint32_t gen = sGeneration.load(std::memory_order_acquire);
    if (e.mesh != nullptr && e.gen == gen && !e.failed) {
        Bump(sLife.bakeWait); /* the other thread baked this entry while we waited on the lock */
        return true;
    }
    /* Keep the old mesh installed for the whole walk: a draw racing this bake keeps
     * drawing last generation's machine instead of losing the model for a frame. */
    Mesh* const old = e.mesh;
    if (old != nullptr) {
        sWin.rebakes++;
        sTot.rebakes++;
    }
    e.gen = gen;
    e.failed = 0;
    Gdx3dsMeshWalkOut& w = sWalkScratch;
    /* walkSrc: the host-built packet stream to walk (a dress entry's widened copy); else the
     * entry's own list. */
    gdx3ds_mesh_walk(walkSrc != nullptr ? walkSrc : e.src, GDX3DS_MESH_MAX_CMDS, resolve, user, &w);
    if (w.status != GDX3DS_MESH_WALK_OK || w.nTris == 0 || w.nVerts == 0) {
        e.failed = 1;
        RetireOld(e, old);
        sWin.bakeFails++;
        sTot.bakeFails++;
        if (sFailLogs < 64 && e.slot != kDressSlot) {
            sFailLogs++;
            Log("[native] bake FAIL slot=%d lod=%d: %s at cmd %u op %02X (tris=%u verts=%u)", e.slot, e.lod + 1,
                gdx3ds_mesh_walk_strerror(w.status), w.errCmd, w.errOpcode, w.nTris, w.nVerts);
        }
        return false;
    }
    Mesh* m = (Mesh*)calloc(1, sizeof(Mesh));
    if (m == nullptr) {
        e.failed = 1;
        RetireOld(e, old);
        return false;
    }
    m->magic = kMeshMagic;
    m->nRuns = w.nRuns;
    m->nChunks = w.nChunks;
    m->nTris = w.nTris;
    m->nVerts = w.nVerts;
    m->runs = DupArray(w.runs, w.nRuns);
    m->chunks = DupArray(w.chunks, w.nChunks);
    m->tris = DupArray(w.tris, (size_t)w.nTris * 3);
    m->vtxChunk = (uint8_t*)malloc(w.nVerts);
    if (m->runs == nullptr || m->chunks == nullptr || m->tris == nullptr || m->vtxChunk == nullptr) {
        FreeMesh(m); /* never published: no other thread can hold it */
        e.failed = 1;
        RetireOld(e, old);
        sWin.bakeFails++;
        sTot.bakeFails++;
        return false;
    }
    for (unsigned c = 0; c < w.nChunks; c++) {
        for (unsigned k = 0; k < w.chunks[c].count; k++) {
            m->vtxChunk[w.chunks[c].baseVertex + k] = (uint8_t)c;
        }
    }
    m->xfRunTex = (uint16_t*)malloc(sizeof(uint16_t) * 2u * (w.nRuns > 0 ? w.nRuns : 1u));
    m->xfScaleOk = m->xfRunTex != nullptr ? XfComputeRunScales(m) : 0;
    e.mesh = m; /* the single publishing store; only now is `old` unreachable */
    RetireMesh(old);
    sWin.bakes++;
    sTot.bakes++;
    gdx3ds_hitch_add(GDX_HITCH_EV_MESH_BAKE, 1u); /* HITCH */
    const uint64_t dt = svcGetSystemTick() - t0;
    if ((sBakeLogs < 160 || sVerbose) && e.slot != kDressSlot) {
        sBakeLogs++;
        Log("[native] bake slot=%d lod=%d cmds=%u runs=%u chunks=%u verts=%u tris=%u us=%lu gen=%lu", e.slot,
            e.lod + 1, w.nCmds, w.nRuns, w.nChunks, w.nVerts, w.nTris,
            (unsigned long)(dt * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC), (unsigned long)e.gen);
    }
    return true;
}

/* ---- draw-time helpers ------------------------------------------------------------- */

inline void ApplyRunState(GdxInterpSeam& g, const Gdx3dsMeshRun& run) {
    if (run.texSet) {
        g.SpTexture(run.texS, run.texT, 0, run.tile, run.texOn);
    }
    if (run.combineSet) {
        /* The raw words, unpacked by the interpreter's own G_SETCOMBINE dispatch. */
        g.SetCombineRaw(run.combineW0, run.combineW1);
    }
    if (run.primKind == GDX3DS_MESH_COLOR_CONST) {
        const uint32_t c = run.primRgba;
        const GdxSeamRgba rgba = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        g.SetPrimColor(run.primMin, run.primLod, rgba);
    }
    if (run.envKind == GDX3DS_MESH_COLOR_CONST) {
        const uint32_t c = run.envRgba;
        const GdxSeamRgba rgba = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        g.SetEnvColor(rgba);
    }
}

/* walk_only replay of one run through the interpreter's own vertex/tri path. */
void WalkRun(GdxInterpSeam& g, const Mesh* m, const Gdx3dsMeshRun& run) {
    const GdxSeamRegs& R = g.R();
    const uint16_t saveS = *R.tex_scale_s;
    const uint16_t saveT = *R.tex_scale_t;
    for (unsigned t = 0; t < run.triCount; t++) {
        const uint16_t* tri = &m->tris[(run.firstTri + t) * 3];
        for (int k = 0; k < 3; k++) {
            const Gdx3dsMeshChunk& ch = m->chunks[m->vtxChunk[tri[k]]];
            const void* v = (const uint8_t*)ch.src + (size_t)(tri[k] - ch.baseVertex) * 16u;
            *R.tex_scale_s = ch.texS;
            *R.tex_scale_t = ch.texT;
            g.SpVertex(1, (size_t)k, v);
        }
        g.SpTri(0, 1, 2);
    }
    *R.tex_scale_s = saveS;
    *R.tex_scale_t = saveT;
    sWin.tris += run.triCount;
    sTot.tris += run.triCount;
}

/* GfxSpTri1's gdxInput lambda: combiner input j of channel k (0 rgb, 1 alpha) for a vertex. */
struct PackInput {
    GdxSeamRgba c;
    bool isShade;
};
inline PackInput InputFor(const GdxSeamRegs& R, const GdxSeamMaterial& mat, int j, int k, const Gdx3dsLoadedVtx* v) {
    PackInput r;
    memset(&r, 0, sizeof(r));
    switch (mat.inputMap[k][j]) {
        case GDXSEAM_CCMUX_PRIMITIVE:
            r.c = *R.prim_color;
            break;
        case GDXSEAM_CCMUX_SHADE:
            r.c.r = v->r;
            r.c.g = v->g;
            r.c.b = v->b;
            r.c.a = v->a;
            r.isShade = true;
            break;
        case GDXSEAM_CCMUX_SHADE_ALPHA:
            r.c.r = r.c.g = r.c.b = r.c.a = v->a;
            break;
        case GDXSEAM_CCMUX_ENVIRONMENT:
            r.c = *R.env_color;
            break;
        case GDXSEAM_CCMUX_PRIMITIVE_ALPHA:
            r.c.r = r.c.g = r.c.b = R.prim_color->a;
            break;
        case GDXSEAM_CCMUX_ENV_ALPHA:
            r.c.r = r.c.g = r.c.b = R.env_color->a;
            break;
        case GDXSEAM_CCMUX_PRIM_LOD_FRAC:
            r.c.r = r.c.g = r.c.b = *R.prim_lod_fraction;
            break;
        case GDXSEAM_CCMUX_KEY_CENTER:
            r.c = *R.key_center;
            break;
        case GDXSEAM_CCMUX_KEY_SCALE:
            r.c = *R.key_scale;
            break;
        case GDXSEAM_CCMUX_CONVERT_K4:
            r.c.r = r.c.g = r.c.b = R.convert_k[4];
            break;
        case GDXSEAM_CCMUX_CONVERT_K5:
            r.c.r = r.c.g = r.c.b = R.convert_k[5];
            break;
        case GDXSEAM_ACMUX_PRIM_LOD_FRAC:
            r.c.a = *R.prim_lod_fraction;
            break;
        default:
            break;
    }
    return r;
}

/* Per-run packing context (everything the [trifast] emission derives per tri, here once). */
struct PackCtx {
    Gdx3dsUvPrep uv[2];
    float clampU[2], clampV[2];
    int ci, ai;
    bool colorIsShade, alphaIsShade, fogFactorConst, shadeAlphaIsOne;
    float fogFactorConstVal;
    GdxSeamRgba constColor; /* non-shade colour input (per-run constant) */
    GdxSeamRgba constAlpha;
    bool constAlphaIsShade;
    const Gdx3dsVboPackParams* params;
    const GdxSeamMaterial* mat;
};

/* Per-run packing context: everything the [trifast] emission derives per tri, here once
 * (shared by the CPU emit and the gpuxf uniforms). false = a combiner shape the packed layout
 * does not model (per-vertex rgb from shade alpha); the caller replays the run. `v0` only
 * feeds non-SHADE inputs, which do not read it. */
bool PreparePackCtx(GdxInterpSeam& g, const GdxSeamMaterial& mat, const Gdx3dsVboPackParams& params,
                    const Gdx3dsLoadedVtx* v0, PackCtx& c) {
    const GdxSeamRegs& R = g.R();
    memset(&c, 0, sizeof(c));
    c.params = &params;
    c.mat = &mat;
    const bool bilerp = (*R.other_mode_h & (3U << GDXSEAM_G_MDSFT_TEXTFILT)) == GDXSEAM_G_TF_BILERP;
    for (int t = 0; t < 2; t++) {
        if (!mat.usedTextures[t]) {
            continue;
        }
        const uint32_t uv_tile = mat.effective_tile[t];
        GdxSeamTileUv tile;
        g.TileUv(uv_tile, &tile);
        gdx3ds_uv_prep(&c.uv[t], tile.shifts, tile.shiftt, tile.uls, tile.ult, mat.tex_width[t], mat.tex_height[t],
                       bilerp);
        c.clampU[t] = params.wrapClampU[t];
        c.clampV[t] = params.wrapClampV[t];
        if (params.shaderClampGate) {
            if (mat.tm & (1 << 2 * t)) {
                c.clampU[t] = ((mat.tex_width2[t] - 0.5f) / mat.tex_width[t]) * params.uScale[t];
            }
            if (mat.tm & (1 << 2 * t + 1)) {
                c.clampV[t] = ((mat.tex_height2[t] - 0.5f) / mat.tex_height[t]) * params.vScale[t];
            }
        }
    }
    c.ci = params.vtxColorInput - 1;
    c.colorIsShade = c.ci >= 0 && mat.inputMap[0][c.ci] == GDXSEAM_CCMUX_SHADE;
    c.ai = params.vtxAlphaInput - 1;
    c.alphaIsShade = c.ai >= 0 && mat.inputMap[1][c.ai] == GDXSEAM_CCMUX_SHADE;
    c.fogFactorConst = params.fogFactorToAlpha && mat.use_blend_color;
    c.fogFactorConstVal = c.fogFactorConst ? gdx3ds_byte_to_unit(R.fog_color->a) : 0.0f;
    c.shadeAlphaIsOne = mat.use_fog && !mat.use_blend_color;
    if (c.ci >= 0 && !c.colorIsShade) {
        const PackInput cv = InputFor(R, mat, c.ci, 0, v0);
        c.constColor = cv.c;
        if (mat.inputMap[0][c.ci] == GDXSEAM_CCMUX_SHADE_ALPHA) {
            return false;
        }
    }
    if (c.ai >= 0 && !c.alphaIsShade) {
        const uint8_t mux = mat.inputMap[1][c.ai];
        c.constAlphaIsShade = (mux == GDXSEAM_CCMUX_SHADE_ALPHA);
        const PackInput av = InputFor(R, mat, c.ai, 1, v0);
        c.constAlpha = av.c;
    }
    return true;
}

/* Batch aux record from vertex 0 of the first surviving triangle (GfxSpTri1 fills it on the
 * batch's first append, after reject/cull), plus the fog-line note Flush() would make. */
void BuildAux(const GdxSeamRegs& R, const GdxSeamMaterial& mat, const Gdx3dsLoadedVtx& first, Gdx3dsVboPackAux& aux) {
    memset(&aux, 0, sizeof(aux));
    aux.numInputs = (uint8_t)(mat.numInputs < 7 ? mat.numInputs : 7);
    for (int j = 0; j < aux.numInputs; j++) {
        const PackInput cv = InputFor(R, mat, j, 0, &first);
        aux.inputRgba[j][0] = cv.c.r / 255.0f;
        aux.inputRgba[j][1] = cv.c.g / 255.0f;
        aux.inputRgba[j][2] = cv.c.b / 255.0f;
        if (mat.use_alpha) {
            const PackInput av = InputFor(R, mat, j, 1, &first);
            aux.inputRgba[j][3] = (mat.use_fog && !mat.use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
        } else {
            aux.inputRgba[j][3] = 1.0f;
        }
    }
    aux.hasFog = mat.use_fog ? 1 : 0;
    if (mat.use_fog) {
        const GdxSeamRgba& fogSrc = mat.use_blend_color ? *R.blend_color : *R.fog_color;
        aux.fogRgb[0] = fogSrc.r / 255.0f;
        aux.fogRgb[1] = fogSrc.g / 255.0f;
        aux.fogRgb[2] = fogSrc.b / 255.0f;
        aux.fogFactor0 = mat.use_blend_color ? R.fog_color->a / 255.0f : first.a / 255.0f;
        if (&gdx3ds_fog_note_params != nullptr) {
            if (mat.use_blend_color) {
                gdx3ds_fog_note_params(0, 0, 0);
            } else {
                gdx3ds_fog_note_params(*R.fog_mul, *R.fog_offset, 1);
            }
        }
    }
}

inline void PackVertex(const PackCtx& c, const Gdx3dsLoadedVtx* vv, float* out) {
    float z = vv->z, w = vv->w;
    if (c.mat->clip.zIsFrom0To1) {
        z = (z + w) / 2.0f;
    }
    out[0] = vv->x;
    out[1] = c.mat->clip.invertY ? -vv->y : vv->y;
    out[2] = z;
    out[3] = w;
    for (int t = 0; t < 2; t++) {
        const int slot = (t == 0) ? 4 : 10;
        if (!c.mat->usedTextures[t]) {
            out[slot] = 0.0f;
            out[slot + 1] = 0.0f;
            continue;
        }
        gdx3ds_uv_pack(&c.uv[t], vv->u, vv->v, c.params->uScale[t], c.params->vScale[t], c.clampU[t], c.clampV[t],
                       c.params->hasOff, c.params->uOff[t], c.params->vOff[t], &out[slot], &out[slot + 1]);
    }
    if (c.ci >= 0) {
        if (c.colorIsShade) {
            out[6] = gdx3ds_byte_to_unit(vv->r);
            out[7] = gdx3ds_byte_to_unit(vv->g);
            out[8] = gdx3ds_byte_to_unit(vv->b);
        } else {
            out[6] = gdx3ds_byte_to_unit(c.constColor.r);
            out[7] = gdx3ds_byte_to_unit(c.constColor.g);
            out[8] = gdx3ds_byte_to_unit(c.constColor.b);
        }
    } else {
        out[6] = out[7] = out[8] = 1.0f;
    }
    if (c.params->fogFactorToAlpha) {
        out[9] = c.fogFactorConst ? c.fogFactorConstVal : gdx3ds_byte_to_unit(vv->a);
    } else if (c.params->wantVtxAlpha) {
        if (c.alphaIsShade) {
            out[9] = c.shadeAlphaIsOne ? 1.0f : gdx3ds_byte_to_unit(vv->a);
        } else {
            /* SHADE_ALPHA is the only per-vertex non-SHADE alpha source (isShade false in the
             * interpreter's lambda, so no fog substitution); the rest are run constants. */
            out[9] = gdx3ds_byte_to_unit(c.constAlphaIsShade ? vv->a : c.constAlpha.a);
        }
    } else {
        out[9] = 1.0f;
    }
}

/* F3DEX2 cull bits (fast/f3dex2.h needs the interpreter's OPCODE macro, so mirrored here). */
constexpr uint32_t kF3dex2CullFront = 0x00000200;
constexpr uint32_t kF3dex2CullBack = 0x00000400;
constexpr uint32_t kF3dex2CullBoth = 0x00000600;
int CullModeOf(uint32_t geometry_mode) {
    const uint32_t bits = geometry_mode & kF3dex2CullBoth;
    if (bits == kF3dex2CullFront) return GDX3DS_CULL_FRONT;
    if (bits == kF3dex2CullBack) return GDX3DS_CULL_BACK;
    if (bits == kF3dex2CullBoth) return GDX3DS_CULL_BOTH;
    return GDX3DS_CULL_NONE;
}

/* Does any triangle of this run survive reject/clip/cull?
 *
 * GfxSpTri1 screens EVERY triangle (reject box, clip-out fan, all-out, backface cull) and
 * returns BEFORE its draw-state resolution, so a run whose triangles are all rejected never
 * resolves a material: no ImportTexture, no shader load, no sampler/viewport write, and
 * crucially `mRdp->textures_changed[]` is left ARMED. The native path resolves per run, so
 * without this screen it imported for invisible runs and disarmed textures_changed — a later
 * run re-asserting the same tile then kept a binding the texture cache had since recycled.
 * The screen is the same arithmetic as the emit loop below -- INCLUDING the [native] cpuclip
 * lever, which both take from one read in NativeDraw (see CpuClipEnabled) so they cannot drift
 * apart again -- and returns at the first survivor, so a visible run pays ~one triangle. */
bool RunHasSurvivor(const Mesh* m, const Gdx3dsMeshRun& run, bool rejVariant, int cullMode, int invert,
                    bool cpuClipOn) {
    const uint16_t* tris = &m->tris[(size_t)run.firstTri * 3];
    for (unsigned t = 0; t < run.triCount; t++) {
        const Gdx3dsLoadedVtx* v1 = &sLoaded[tris[t * 3 + 0]];
        const Gdx3dsLoadedVtx* v2 = &sLoaded[tris[t * 3 + 1]];
        const Gdx3dsLoadedVtx* v3 = &sLoaded[tris[t * 3 + 2]];
        if (rejVariant && gdx3ds_tri_rej_reject(v1, v2, v3)) {
            continue;
        }
        if (cpuClipOn && (v1->clip_out | v2->clip_out | v3->clip_out) != 0) {
            Gdx3dsLoadedVtx poly[GDX3DS_CLIP_POLY_MAX];
            const int n = gdx3ds_tri_clip(v1, v2, v3, poly);
            if (n < 3) {
                continue;
            }
            for (int i = 1; i + 1 < n; i++) {
                Gdx3dsLoadedVtx fan[3];
                fan[0] = poly[0];
                fan[1] = poly[i];
                fan[2] = poly[i + 1];
                for (int j = 0; j < 3; j++) {
                    if (fan[j].w < GDX3DS_VTX_MIN_CLIP_W) {
                        fan[j].w = GDX3DS_VTX_MIN_CLIP_W;
                    }
                    gdx3ds_vtx_update_clip_flags(&fan[j]);
                }
                if (gdx3ds_tri_all_out(&fan[0], &fan[1], &fan[2])) {
                    continue;
                }
                if (gdx3ds_tri_cull(&fan[0], &fan[1], &fan[2], cullMode, invert)) {
                    continue;
                }
                return true;
            }
            continue;
        }
        if (gdx3ds_tri_all_out(v1, v2, v3)) {
            continue;
        }
        if (gdx3ds_tri_cull(v1, v2, v3, cullMode, invert)) {
            continue;
        }
        return true;
    }
    return false;
}

/* hotcold: NativeRun's GEOMLOSS invariant trip (counted + logged once), out of line. */
GDX_COLD void NoteIdxMisaligned(uint32_t nIdx) {
    sWin.idxMisaligned++;
    sTot.idxMisaligned++;
    if (!sTruncLogged) {
        sTruncLogged = 1;
        Log("GEOMLOSS: machines index buffer MISALIGNED (nIdx=%u, not a multiple of 3) — trimmed to "
            "%u; a cap broke mid-triangle, which would have drawn the rest of the run from "
            "mismatched vertices",
            nIdx, nIdx - (nIdx % 3u));
    }
}

/* Native draw of one run. Returns false if the backend refused (caller replays).
 * hotcold: noinline keeps the CPU path out of NativeDraw's gpuxf body (as the base inliner did). */
__attribute__((noinline)) bool NativeRun(GdxInterpSeam& g, const Mesh* m, const Gdx3dsMeshRun& run, const GdxSeamMaterial& mat,
               bool rejVariant, bool cpuClipOn) {
    const GdxSeamRegs& R = g.R();
    const uint64_t te0 = svcGetSystemTick();
    struct EmitTimer {
        uint64_t t0;
        ~EmitTimer() {
            sWin.emitTicks += svcGetSystemTick() - t0;
        }
    } emitTimer = { te0 };
    /* G_CCMUX_LOD_FRACTION is the one combiner input GfxSpTri1's gdxInput lambda evaluates
     * PER TRIANGLE (from v1->w under G_TL_LOD), which neither the per-run constant below nor
     * the batch aux record can represent. InputFor() has no case for it and would silently
     * emit black, so refuse the native path and let the caller replay the run through the
     * interpreter (counted as `fb` on the receipt). No built machine list uses it today. */
    for (int j = 0; j < mat.numInputs && j < 7; j++) {
        if (mat.inputMap[0][j] == GDXSEAM_CCMUX_LOD_FRACTION || mat.inputMap[1][j] == GDXSEAM_CCMUX_LOD_FRACTION) {
            return false;
        }
    }
    Gdx3dsVboPackParams params;
    memset(&params, 0, sizeof(params));
    /* Minimum: every mesh vertex once; fan vertices (rare) take whatever the pool still has
     * (capVerts); emission stops at the cap and counts poolTrunc. */
    const uint32_t minVerts = (uint32_t)m->nVerts + 8u;
    const uint32_t maxIdx = (uint32_t)run.triCount * 3u * 8u;
    uint32_t maxVerts = 0;
    float* dst = gdx3ds_mesh_backend_begin(&params, minVerts, maxIdx, &maxVerts);
    if (sCapVerts > 0 && maxVerts > (uint32_t)sCapVerts) {
        maxVerts = (uint32_t)sCapVerts; /* GEOMLOSS test knob: force the truncation path */
    }
    if (dst == nullptr) {
        sWin.poolRefused++;
        sTot.poolRefused++;
        return false;
    }
    PackCtx c;
    if (!PreparePackCtx(g, mat, params, &sLoaded[0], c)) {
        return false; /* per-vertex rgb from shade alpha: not modelled, replay */
    }

    const int cullMode = CullModeOf(*R.geometry_mode);
    const int invert = (*R.extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0;
    memset(sRemap, 0xFF, sizeof(uint16_t) * m->nVerts);
    uint32_t nOut = 0, nIdx = 0;
    bool runTrunc = false; /* GEOMLOSS: set when a cap ends the run on a triangle boundary */
    Gdx3dsLoadedVtx first;
    bool haveFirst = false;
    memset(&first, 0, sizeof(first));
    const uint16_t* tris = &m->tris[(size_t)run.firstTri * 3];
    for (unsigned t = 0; t < run.triCount; t++) {
        const Gdx3dsLoadedVtx* v1 = &sLoaded[tris[t * 3 + 0]];
        const Gdx3dsLoadedVtx* v2 = &sLoaded[tris[t * 3 + 1]];
        const Gdx3dsLoadedVtx* v3 = &sLoaded[tris[t * 3 + 2]];
        if (rejVariant && gdx3ds_tri_rej_reject(v1, v2, v3)) {
            sWin.rejected++;
            continue;
        }
        if (cpuClipOn && (v1->clip_out | v2->clip_out | v3->clip_out) != 0) {
            Gdx3dsLoadedVtx poly[GDX3DS_CLIP_POLY_MAX];
            const int n = gdx3ds_tri_clip(v1, v2, v3, poly);
            sWin.clipped++;
            if (n < 3) {
                continue;
            }
            for (int i = 1; i + 1 < n; i++) {
                Gdx3dsLoadedVtx fan[3];
                fan[0] = poly[0];
                fan[1] = poly[i];
                fan[2] = poly[i + 1];
                for (int j = 0; j < 3; j++) {
                    if (fan[j].w < GDX3DS_VTX_MIN_CLIP_W) {
                        fan[j].w = GDX3DS_VTX_MIN_CLIP_W;
                    }
                    gdx3ds_vtx_update_clip_flags(&fan[j]);
                }
                if (gdx3ds_tri_all_out(&fan[0], &fan[1], &fan[2])) {
                    sWin.allOut++;
                    continue;
                }
                if (gdx3ds_tri_cull(&fan[0], &fan[1], &fan[2], cullMode, invert)) {
                    sWin.culled++;
                    continue;
                }
                if (nOut + 3 > maxVerts || nIdx + 3 > kMaxIdxScratch) {
                    /* GEOMLOSS: whole-triple pre-check (already correct); stop the RUN, not
                     * just this fan, so a truncated run is a clean prefix of the geometry
                     * instead of a hole with later triangles still drawn over it. */
                    sWin.poolTrunc++;
                    sTot.poolTrunc++;
                    TruncLoud(maxVerts, nOut, 3, run.triCount, t);
                    runTrunc = true;
                    break;
                }
                if (!haveFirst) {
                    first = fan[0];
                    haveFirst = true;
                }
                for (int j = 0; j < 3; j++) {
                    PackVertex(c, &fan[j], dst + (size_t)nOut * GDX3DS_VBOPACK_STRIDE);
                    sIdx[nIdx++] = (uint16_t)nOut;
                    nOut++;
                }
                sWin.fanTris++;
            }
            if (runTrunc && sTruncFix) {
                break;
            }
            continue;
        }
        if (gdx3ds_tri_all_out(v1, v2, v3)) {
            sWin.allOut++;
            continue;
        }
        if (gdx3ds_tri_cull(v1, v2, v3, cullMode, invert)) {
            sWin.culled++;
            continue;
        }
        if (nIdx + 3 > kMaxIdxScratch) {
            /* GEOMLOSS: index scratch is the other cap; it was already a whole-triple
             * pre-check but it left no trace at all. Count it with the pool truncations. */
            sWin.poolTrunc++;
            sTot.poolTrunc++;
            TruncLoud(maxVerts, nOut, 3, run.triCount, t);
            runTrunc = true;
            break;
        }
        const Gdx3dsLoadedVtx* vv[3] = { v1, v2, v3 };
        /* GEOMLOSS: the vertex-cap check must clear the WHOLE triple before any index is
         * pushed. It used to sit inside the j loop, so hitting the cap at j==1 or j==2 left
         * one or two indices already appended and broke only the inner loop — nIdx stayed
         * permanently off by 1 or 2 and every remaining triangle of the run was assembled
         * from mismatched vertices. Count the distinct vertices this triangle still has to
         * pack (duplicated corners of a degenerate triangle are packed once), and stop the
         * run on a triangle boundary when they do not fit. */
        const uint16_t g0 = tris[t * 3 + 0];
        const uint16_t g1 = tris[t * 3 + 1];
        const uint16_t g2 = tris[t * 3 + 2];
        uint32_t need = 0;
        if (sRemap[g0] == 0xFFFF) {
            need++;
        }
        if (sRemap[g1] == 0xFFFF && g1 != g0) {
            need++;
        }
        if (sRemap[g2] == 0xFFFF && g2 != g0 && g2 != g1) {
            need++;
        }
        if (sTruncFix && nOut + need > maxVerts) {
            sWin.poolTrunc++;
            sTot.poolTrunc++;
            TruncLoud(maxVerts, nOut, need, run.triCount, t);
            runTrunc = true;
            break;
        }
        for (int j = 0; j < 3; j++) {
            const uint16_t gi = tris[t * 3 + j];
            if (sRemap[gi] == 0xFFFF) {
                if (!sTruncFix && nOut >= maxVerts) {
                    /* pre-fix arm ([debug] meshtruncfix=0): breaks the INNER loop with
                     * indices for this triangle already pushed — nIdx misaligns here. */
                    sWin.poolTrunc++;
                    sTot.poolTrunc++;
                    break;
                }
                sRemap[gi] = (uint16_t)nOut;
                PackVertex(c, vv[j], dst + (size_t)nOut * GDX3DS_VBOPACK_STRIDE);
                nOut++;
            }
            if (!haveFirst) {
                first = *vv[j];
                haveFirst = true;
            }
            sIdx[nIdx++] = sRemap[gi];
        }
    }
    sWin.tris += run.triCount;
    sTot.tris += run.triCount;
    /* GEOMLOSS invariant guard: an index buffer must end on a triangle boundary. Every cap
     * above is a whole-triple pre-check, so this can no longer trip — but a misaligned tail
     * draws every following triangle from mismatched vertices, which is the worst failure
     * this path has, so it is checked rather than assumed. Trimming is the safe answer. */
    if (GDX_UNLIKELY(nIdx % 3u != 0u)) {
        NoteIdxMisaligned(nIdx);
        nIdx -= nIdx % 3u;
    }
    if (nIdx == 0) {
        gdx3ds_mesh_backend_draw(0, sIdx, 0, nullptr); /* releases the reservation */
        return true;
    }
    /* Batch aux record: vertex 0 of the first surviving triangle (GfxSpTri1 fills it on the
     * batch's first append, after reject/cull). */
    Gdx3dsVboPackAux aux;
    BuildAux(R, mat, first, aux);
    /* Flush() sets these on the backend before every DrawTriangles; mirror it. */
    g.ApplyDrawConstants();
    const uint64_t td0 = svcGetSystemTick();
    gdx3ds_mesh_backend_draw(nOut, sIdx, nIdx, &aux);
    sWin.drawTicks += svcGetSystemTick() - td0;
    sWin.verts += nOut;
    sTot.verts += nOut;
    sWin.runs++;
    sTot.runs++;
    return true;
}

/* ---- [native] machines_verify: capture both passes and compare ------------------------ */
constexpr uint32_t kCapMaxTris = GDX3DS_MESH_MAX_TRIS * 8;
struct CapTri {
    float v[3 * GDX3DS_VBOPACK_STRIDE];
    uint64_t id0, id1;
    uint32_t tex0, tex1;
    float aux[7 * 4 + 4];
};
CapTri* sCap[2] = { nullptr, nullptr };
uint32_t sCapCount[2] = { 0, 0 };
int sCapWhich = 0;

GDX_COLD void CaptureCb(const Gdx3dsMeshCapture* cap, void* user) {
    (void)user;
    CapTri* out = sCap[sCapWhich];
    uint32_t& n = sCapCount[sCapWhich];
    if (out == nullptr) {
        return;
    }
    const uint32_t tris = cap->idx != nullptr ? cap->nIdx / 3u : cap->numTris;
    for (uint32_t t = 0; t < tris && n < kCapMaxTris; t++, n++) {
        CapTri& ct = out[n];
        for (int k = 0; k < 3; k++) {
            const uint32_t vi = cap->idx != nullptr ? cap->idx[t * 3 + k] : t * 3 + k;
            memcpy(&ct.v[k * GDX3DS_VBOPACK_STRIDE], cap->verts + (size_t)vi * GDX3DS_VBOPACK_STRIDE,
                   GDX3DS_VBOPACK_STRIDE * sizeof(float));
        }
        ct.id0 = cap->shaderId0;
        ct.id1 = cap->shaderId1;
        ct.tex0 = cap->texId[0];
        ct.tex1 = cap->texId[1];
        memset(ct.aux, 0, sizeof(ct.aux));
        if (cap->aux != nullptr) {
            memcpy(ct.aux, cap->aux->inputRgba, sizeof(cap->aux->inputRgba));
            /* Entries past numInputs are never written by either producer (the interpreter's
             * static record keeps stale values there): compare only the live ones. */
            for (int j = cap->aux->numInputs < 7 ? cap->aux->numInputs : 7; j < 7; j++) {
                ct.aux[j * 4 + 0] = ct.aux[j * 4 + 1] = ct.aux[j * 4 + 2] = ct.aux[j * 4 + 3] = 0.0f;
            }
            /* The inputs riding the vertex attribute are per-vertex (their vertex-0 record is
             * unused by the backend and legitimately differs across batch splits): mask them so
             * every remaining aux entry is a real stage constant that MUST match. */
            if (cap->vtxColorInput > 0 && cap->vtxColorInput <= 7) {
                ct.aux[(cap->vtxColorInput - 1) * 4 + 0] = ct.aux[(cap->vtxColorInput - 1) * 4 + 1] =
                    ct.aux[(cap->vtxColorInput - 1) * 4 + 2] = 0.0f;
            }
            if (cap->vtxAlphaInput > 0 && cap->vtxAlphaInput <= 7) {
                ct.aux[(cap->vtxAlphaInput - 1) * 4 + 3] = 0.0f;
            }
            ct.aux[28] = cap->aux->fogRgb[0];
            ct.aux[29] = cap->aux->fogRgb[1];
            ct.aux[30] = cap->aux->fogRgb[2];
            ct.aux[31] = (float)cap->aux->hasFog;
        }
    }
}

struct ListStateSnap {
    GdxSeamRgba prim, env;
    uint8_t primLod;
    uint64_t combine;
    uint32_t firstTile;
    bool texChanged[2];
    uint16_t texS, texT;
    bool vpChanged;
};
void SnapListState(const GdxSeamRegs& R, ListStateSnap& sn) {
    sn.prim = *R.prim_color;
    sn.env = *R.env_color;
    sn.primLod = *R.prim_lod_fraction;
    sn.combine = *R.combine_mode;
    sn.firstTile = *R.first_tile_index;
    sn.texChanged[0] = R.textures_changed[0];
    sn.texChanged[1] = R.textures_changed[1];
    sn.texS = *R.tex_scale_s;
    sn.texT = *R.tex_scale_t;
    sn.vpChanged = *R.viewport_or_scissor_changed;
}
void RestoreListState(const GdxSeamRegs& R, const ListStateSnap& sn) {
    *R.prim_color = sn.prim;
    *R.env_color = sn.env;
    *R.prim_lod_fraction = sn.primLod;
    *R.combine_mode = sn.combine;
    *R.first_tile_index = (uint8_t)sn.firstTile;
    R.textures_changed[0] = sn.texChanged[0];
    R.textures_changed[1] = sn.texChanged[1];
    *R.tex_scale_s = sn.texS;
    *R.tex_scale_t = sn.texT;
    *R.viewport_or_scissor_changed = sn.vpChanged;
}

void NativeDraw(GdxInterpSeam& g, Mesh* m, bool allowWalkFallback, int entryIdx);

int sVerifyLogs = 0;
GDX_COLD void VerifyDraw(GdxInterpSeam& g, Mesh* m, int slot, int lod) {
    if (sCap[0] == nullptr) {
        sCap[0] = (CapTri*)malloc(sizeof(CapTri) * kCapMaxTris);
        sCap[1] = (CapTri*)malloc(sizeof(CapTri) * kCapMaxTris);
        if (sCap[0] == nullptr || sCap[1] == nullptr) {
            Log("[native] verify: capture buffers unavailable, drawing natively without verify");
            sVerify = 0;
            NativeDraw(g, m, true, -1);
            return;
        }
    }
    const GdxSeamRegs& R = g.R();
    ListStateSnap snap;
    SnapListState(R, snap);
    /* pass 1: the interpreter's own path, drawn and captured */
    sCapCount[0] = sCapCount[1] = 0;
    sCapWhich = 0;
    gdx3ds_mesh_backend_set_capture(CaptureCb, nullptr, 0);
    for (unsigned r = 0; r < m->nRuns; r++) {
        ApplyRunState(g, m->runs[r]);
        if (m->runs[r].triCount != 0) {
            WalkRun(g, m, m->runs[r]);
            g.Flush();
        }
    }
    /* pass 2: the native path, captured only (no GPU submit) */
    RestoreListState(R, snap);
    R.textures_changed[0] = true; /* pass 1 bound the last run's textures: re-import */
    R.textures_changed[1] = true;
    sCapWhich = 1;
    gdx3ds_mesh_backend_set_capture(CaptureCb, nullptr, 1);
    NativeDraw(g, m, false, -1);
    gdx3ds_mesh_backend_set_capture(nullptr, nullptr, 0);
    /* compare */
    const uint32_t nW = sCapCount[0], nN = sCapCount[1];
    const uint32_t n = nW < nN ? nW : nN;
    unsigned triMis = 0, matMis = 0, auxDiff = 0;
    int firstTri = -1, firstSlot = -1, firstMat = -1;
    for (uint32_t t = 0; t < n; t++) {
        const CapTri& a = sCap[0][t];
        const CapTri& b = sCap[1][t];
        if (memcmp(a.v, b.v, sizeof(a.v)) != 0) {
            triMis++;
            if (firstTri < 0) {
                firstTri = (int)t;
                for (int k = 0; k < 3 * GDX3DS_VBOPACK_STRIDE; k++) {
                    if (memcmp(&a.v[k], &b.v[k], sizeof(float)) != 0) {
                        firstSlot = k;
                        break;
                    }
                }
            }
        }
        if (a.id0 != b.id0 || a.id1 != b.id1 || a.tex0 != b.tex0 || a.tex1 != b.tex1) {
            matMis++;
            if (firstMat < 0) {
                firstMat = (int)t;
            }
        }
        if (memcmp(a.aux, b.aux, sizeof(a.aux)) != 0) {
            auxDiff++;
        }
    }
    sWin.vfyDraws++;
    sWin.vfyTris += n;
    sWin.vfyTriMismatch += triMis;
    sWin.vfyMatMismatch += matMis;
    sWin.vfyAuxDiff += auxDiff;
    if (nW != nN) {
        sWin.vfyCountMismatch++;
    }
    if ((triMis != 0 || matMis != 0 || nW != nN) && sVerifyLogs < 40) {
        sVerifyLogs++;
        if (firstTri >= 0) {
            const CapTri& a = sCap[0][firstTri];
            const CapTri& b = sCap[1][firstTri];
            uint32_t ua, ub;
            memcpy(&ua, &a.v[firstSlot], 4);
            memcpy(&ub, &b.v[firstSlot], 4);
            Log("[native] verify MISMATCH slot=%d lod=%d tris walk=%lu native=%lu triMis=%u matMis=%u first tri=%d "
                "float=%d walk=%08lx(%g) native=%08lx(%g)",
                slot, lod + 1, (unsigned long)nW, (unsigned long)nN, triMis, matMis, firstTri, firstSlot,
                (unsigned long)ua, (double)a.v[firstSlot], (unsigned long)ub, (double)b.v[firstSlot]);
        } else if (firstMat >= 0) {
            /* Name the disagreeing field: a material-only mismatch means BindMaterial resolved a
             * different shader program or bound a different texture than GfxSpTri1 did for the
             * same draw state — the drift class the replica is exposed to. */
            const CapTri& a = sCap[0][firstMat];
            const CapTri& b = sCap[1][firstMat];
            Log("[native] verify MISMATCH slot=%d lod=%d tris walk=%lu native=%lu matMis=%u first tri=%d "
                "id0 w=%08lx%08lx n=%08lx%08lx id1 w=%08lx%08lx n=%08lx%08lx tex0 w=%lu n=%lu tex1 w=%lu n=%lu",
                slot, lod + 1, (unsigned long)nW, (unsigned long)nN, matMis, firstMat,
                (unsigned long)(a.id0 >> 32), (unsigned long)(uint32_t)a.id0, (unsigned long)(b.id0 >> 32),
                (unsigned long)(uint32_t)b.id0, (unsigned long)(a.id1 >> 32), (unsigned long)(uint32_t)a.id1,
                (unsigned long)(b.id1 >> 32), (unsigned long)(uint32_t)b.id1, (unsigned long)a.tex0,
                (unsigned long)b.tex0, (unsigned long)a.tex1, (unsigned long)b.tex1);
        } else {
            Log("[native] verify MISMATCH slot=%d lod=%d tris walk=%lu native=%lu matMis=%u (count)", slot,
                lod + 1, (unsigned long)nW, (unsigned long)nN, matMis);
        }
    }
    sWin.draws++;
    sTot.draws++;
}

/* [debug] xf_census: the CPU-transformed chunk, bucket mesh. */
GDX_COLD void XfCensusChunk(const Gdx3dsVtxMathState& vs, const Gdx3dsMeshChunk& ch) {
    float xs = 1.0f;
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        xs = 160.0f / (160.0f - 12.0f);
    } else if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        xs = (4.0f / 3.0f) / vs.aspect;
    }
    gdx3ds_xf_census_path = GDX3DS_XFC_MESH;
    const uint8_t* src = (const uint8_t*)ch.src;
    for (unsigned k = 0; k < ch.count; k++) {
        const Gdx3dsLoadedVtx& d = sLoaded[ch.baseVertex + k];
        gdx3ds_xf_census_note(((const Gdx3dsVtxIn*)(src + (size_t)k * 16u))->ob, vs.mp, xs, d.x, d.y, d.w);
    }
    gdx3ds_xf_census_path = GDX3DS_XFC_DL;
}

/* ---- [native] gpuxf: GPU vertex transform of a whole mesh -------------------------------
 *
 * The CPU path above T&Ls every mesh vertex (gdx3ds_vtx_transform) and then screens, clips,
 * culls and packs every triangle. This path hands the PICA the RAW 16-byte Vtx instead and
 * runs the transform in shader.v.pica's gpuxf proc. It is taken only when the result is
 * PROVABLY the same picture (o3dsgpuxf-progress.md §2):
 *
 *  1. Containment. The mesh's model-space bounding box is transformed on the CPU (8 corners,
 *     the exact fp32 expressions). Clip coordinates are affine in the model coordinates, so
 *     every vertex is a convex combination of the corners: all corners strictly inside the
 *     7 clip planes (margin kXfMargin) => no vertex is clipped, rejected (F3DEX2.Rej's 2w box
 *     contains the w box) or all-out, on the CPU OR the PICA. The only per-triangle CPU stage
 *     left is the backface cull, which the PICA does in its place. All corners strictly
 *     outside ONE of the planes all_out tests => every triangle is dropped on the CPU path
 *     whatever the lever state, and the mesh is skipped with no T&L at all.
 *  2. Precision. The PICA computes in float24 (16-bit mantissa). The venue-floor regression
 *     of feat/3ds-gputransform was catastrophic cancellation: world-space vertices (|ob| to
 *     32000) against a view matrix whose products cancel to a clip coordinate thousands of
 *     times smaller. The bound here is the dp4's worst case, 2^-14 * sum|m_k * ob_k|, divided
 *     by the smallest corner w and scaled to screen pixels; a draw over gpuxf_maxerr (default
 *     1/8 px) stays on the CPU. Machine meshes are model-space (|ob| ~ 10^2): their bound is
 *     three orders of magnitude inside the gate; the floor's is two orders outside it.
 *  3. State. Every material decision the CPU path makes per run is made here the same way,
 *     from the same CPU-transformed vertex: RunHasSurvivor's screen (texture import stays
 *     armed for invisible runs), the aux record and fog note (vertex 0 of the first
 *     surviving triangle), and the stereo/DISP class (its packed w).
 * FLX alpha (a DMEM LUT the shader cannot index) is computed on the CPU into the uploaded
 * copy's alpha byte (XfPatchFlxAlpha). What the shader cannot express stays on the CPU, per
 * mesh: linear texgen (acos), more than 3 directional lights, runs mixing chunk scales. */
constexpr float kXfMargin = 1.0f / 1024.0f; /* containment margin, relative to w */
constexpr float kXfMinW = 0.01f;            /* > the fog guard's 0.001 and the clip plane's 1e-5 */

uint32_t sXfStamp[GDX3DS_MESH_MAX_VERTS];
uint32_t sXfEpoch = 0;

enum XfCover { XF_INSIDE, XF_OUTSIDE, XF_STRADDLE };

struct XfBox {
    float lo[3], hi[3];
};

bool XfMeshBox(const Mesh* m, XfBox& b) {
    int lo[3] = { 32767, 32767, 32767 };
    int hi[3] = { -32768, -32768, -32768 };
    for (unsigned ci = 0; ci < m->nChunks; ci++) {
        const Gdx3dsMeshChunk& ch = m->chunks[ci];
        const uint8_t* src = (const uint8_t*)ch.src;
        for (unsigned k = 0; k < ch.count; k++) {
            const Gdx3dsVtxIn* v = (const Gdx3dsVtxIn*)(src + (size_t)k * 16u);
            for (int a = 0; a < 3; a++) {
                lo[a] = v->ob[a] < lo[a] ? v->ob[a] : lo[a];
                hi[a] = v->ob[a] > hi[a] ? v->ob[a] : hi[a];
            }
        }
    }
    for (int a = 0; a < 3; a++) {
        b.lo[a] = (float)lo[a];
        b.hi[a] = (float)hi[a];
    }
    return lo[0] <= hi[0];
}

/* The corner transform: gdx3ds_vtx_transform's position expressions, including the x scale. */
inline void XfClipOf(const Gdx3dsVtxMathState& s, float ox, float oy, float oz, float c[4]) {
    for (int i = 0; i < 4; i++) {
        c[i] = ox * s.mp[0][i] + oy * s.mp[1][i] + oz * s.mp[2][i] + s.mp[3][i];
    }
    if (s.xMode == GDX3DS_VTX_X_STRETCH) {
        c[0] = c[0] * (160.0f / (160.0f - 12.0f));
    } else if (s.xMode == GDX3DS_VTX_X_ADJ) {
        c[0] = c[0] * (4.0f / 3.0f) / s.aspect;
    }
}

/* Containment of the 8 box corners (see the block comment, point 1) and the smallest w. */
XfCover XfClassify(const Gdx3dsVtxMathState& s, const XfBox& b, float* wMin) {
    uint32_t allOut = 0x1Fu; /* x<-w x>w y<-w y>w z>w: the clip_rej planes all_out tests */
    bool inside = true;
    *wMin = 1e30f;
    for (int k = 0; k < 8; k++) {
        float c[4];
        XfClipOf(s, (k & 1) ? b.hi[0] : b.lo[0], (k & 2) ? b.hi[1] : b.lo[1], (k & 4) ? b.hi[2] : b.lo[2], c);
        const float w = c[3];
        const float in = w * (1.0f - kXfMargin);
        const float out = w * (1.0f + kXfMargin);
        *wMin = w < *wMin ? w : *wMin;
        if (!(w > kXfMinW && c[0] > -in && c[0] < in && c[1] > -in && c[1] < in && c[2] > -in && c[2] < in)) {
            inside = false;
        }
        uint32_t outMask = 0;
        outMask |= (c[0] < -out) ? 1u : 0u;
        outMask |= (c[0] > out) ? 2u : 0u;
        outMask |= (c[1] < -out) ? 4u : 0u;
        outMask |= (c[1] > out) ? 8u : 0u;
        outMask |= (c[2] > out) ? 16u : 0u;
        allOut &= (w > 0.0f) ? outMask : 0u;
    }
    if (inside) {
        return XF_INSIDE;
    }
    return allOut != 0 ? XF_OUTSIDE : XF_STRADDLE;
}

/* Float24 error bound of the shader's dp4, in 1/1000 screen pixels (point 2). The PICA keeps
 * a 16-bit mantissa; each of the four products and three sums rounds, so 2^-14 * sum|terms|
 * bounds a row's absolute error. An NDC coordinate x/w then errs by at most (e_x + e_w)/w
 * (|x| <= w inside), times 200 px (the 400-px axis) to reach the screen. */
unsigned XfErrorBoundMpx(const Gdx3dsVtxMathState& s, const XfBox& b, float wMin) {
    float amax[3];
    for (int a = 0; a < 3; a++) {
        amax[a] = fabsf(b.lo[a]) > fabsf(b.hi[a]) ? fabsf(b.lo[a]) : fabsf(b.hi[a]);
    }
    float e[4];
    for (int i = 0; i < 4; i++) {
        e[i] = fabsf(s.mp[3][i]);
        for (int a = 0; a < 3; a++) {
            e[i] += fabsf(s.mp[a][i]) * amax[a];
        }
        e[i] *= 1.0f / 16384.0f;
    }
    const float xs = (s.xMode == GDX3DS_VTX_X_NONE) ? 1.0f : 1.2f;
    float ex = e[0] * xs + e[3];
    const float ey = e[1] + e[3];
    ex = ex > ey ? ex : ey;
    const float px = ex / wMin * 200.0f * 1000.0f;
    return px > 4.0e9f ? 4000000000u : (unsigned)px;
}

/* The survivor screen's vertex: gdx3ds_vtx_transform's position and clip flags ONLY, with the
 * same expressions in the same order (VFPv2 has no fused multiply-add and the build has no
 * fast-math, so the floats are the CPU path's bit for bit) -- the screen reads nothing else,
 * and the lighting/texgen/fog it skips is most of a T&L. sScreen holds these; sLoaded holds
 * full T&L results (XfVertex) for the aux record. */
Gdx3dsLoadedVtx sScreen[GDX3DS_MESH_MAX_VERTS];
uint32_t sXfPosStamp[GDX3DS_MESH_MAX_VERTS];

const Gdx3dsLoadedVtx* XfScreenVertex(const Gdx3dsVtxMathState& vs, const Mesh* m, uint16_t gi) {
    if (sXfPosStamp[gi] != sXfEpoch) {
        const Gdx3dsMeshChunk& ch = m->chunks[m->vtxChunk[gi]];
        const Gdx3dsVtxIn* v = (const Gdx3dsVtxIn*)((const uint8_t*)ch.src + (size_t)(gi - ch.baseVertex) * 16u);
        float c[4];
        XfClipOf(vs, (float)v->ob[0], (float)v->ob[1], (float)v->ob[2], c);
        Gdx3dsLoadedVtx* d = &sScreen[gi];
        d->x = c[0];
        d->y = c[1];
        d->z = c[2];
        d->w = c[3];
        d->clip_rej = 0;
        if (d->x < -d->w) d->clip_rej |= 1;
        if (d->x > d->w) d->clip_rej |= 2;
        if (d->y < -d->w) d->clip_rej |= 4;
        if (d->y > d->w) d->clip_rej |= 8;
        if (d->z > d->w) d->clip_rej |= 32;
        d->clip_out = gdx3ds_vtx_clip_out_mask(d);
        sXfPosStamp[gi] = sXfEpoch;
        sWin.xfScreened++;
    }
    return &sScreen[gi];
}

/* The lazily T&L'd mesh vertex (exactly the CPU path's value): vertex 0 of a run's first
 * surviving triangle, for the aux record, fog note and stereo class. */
const Gdx3dsLoadedVtx* XfVertex(Gdx3dsVtxMathState& vs, const Mesh* m, uint16_t gi) {
    if (sXfStamp[gi] != sXfEpoch) {
        const Gdx3dsMeshChunk& ch = m->chunks[m->vtxChunk[gi]];
        vs.texScaleS = ch.texS;
        vs.texScaleT = ch.texT;
        const uint8_t* src = (const uint8_t*)ch.src + (size_t)(gi - ch.baseVertex) * 16u;
        gdx3ds_vtx_transform(&vs, (const Gdx3dsVtxIn*)src, &sLoaded[gi]);
        sXfStamp[gi] = sXfEpoch;
    }
    return &sLoaded[gi];
}

/* RunHasSurvivor on lazily transformed vertices, returning the first surviving triangle
 * (-1 = none). The clip branch cannot be reached under XF_INSIDE; if rounding ever reached it
 * the CPU rule is still applied (xfGateMiss counts it). */
int XfFirstSurvivor(Gdx3dsVtxMathState& vs, const Mesh* m, const Gdx3dsMeshRun& run, bool rejVariant, int cullMode,
                    int invert, bool cpuClipOn) {
    const uint16_t* tris = &m->tris[(size_t)run.firstTri * 3];
    for (unsigned t = 0; t < run.triCount; t++) {
        const Gdx3dsLoadedVtx* v1 = XfScreenVertex(vs, m, tris[t * 3 + 0]);
        const Gdx3dsLoadedVtx* v2 = XfScreenVertex(vs, m, tris[t * 3 + 1]);
        const Gdx3dsLoadedVtx* v3 = XfScreenVertex(vs, m, tris[t * 3 + 2]);
        if (rejVariant && gdx3ds_tri_rej_reject(v1, v2, v3)) {
            continue;
        }
        if (cpuClipOn && (v1->clip_out | v2->clip_out | v3->clip_out) != 0) {
            sWin.xfGateMiss++;
            Gdx3dsLoadedVtx poly[GDX3DS_CLIP_POLY_MAX];
            const int n = gdx3ds_tri_clip(v1, v2, v3, poly);
            for (int i = 1; i + 1 < n; i++) {
                Gdx3dsLoadedVtx fan[3] = { poly[0], poly[i], poly[i + 1] };
                for (int j = 0; j < 3; j++) {
                    if (fan[j].w < GDX3DS_VTX_MIN_CLIP_W) {
                        fan[j].w = GDX3DS_VTX_MIN_CLIP_W;
                    }
                    gdx3ds_vtx_update_clip_flags(&fan[j]);
                }
                if (!gdx3ds_tri_all_out(&fan[0], &fan[1], &fan[2]) &&
                    !gdx3ds_tri_cull(&fan[0], &fan[1], &fan[2], cullMode, invert)) {
                    return (int)t;
                }
            }
            continue;
        }
        if (gdx3ds_tri_all_out(v1, v2, v3)) {
            continue;
        }
        if (gdx3ds_tri_cull(v1, v2, v3, cullMode, invert)) {
            continue;
        }
        return (int)t;
    }
    return -1;
}

/* Mesh-level uniforms: the MP rows, fog, lighting, texgen (everything LatchVtxState owns). */
void XfFillMesh(const Gdx3dsVtxMathState& vs, Gdx3dsXfDraw& xf) {
    memset(&xf, 0, sizeof(xf));
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            xf.mvp[i][k] = vs.mp[k][i];
        }
    }
    const float fogOn = vs.fog ? 1.0f : 0.0f;
    xf.fog[0] = (float)vs.fogMul;
    xf.fog[1] = (float)vs.fogOffset;
    xf.fog[2] = fogOn;
    xf.fog[3] = 1.0f - fogOn;
    const float lit = vs.lighting ? 1.0f : 0.0f;
    xf.amb[3] = lit;
    xf.unlit[3] = 1.0f - lit;
    if (vs.lighting) {
        for (int c = 0; c < 3; c++) {
            xf.amb[c] = (float)vs.ambient[c];
        }
        for (int i = 0; i < vs.numDir && i < 3; i++) {
            for (int c = 0; c < 3; c++) {
                xf.ldir[i][c] = vs.coeffs[i][c] / 127.0f;
                xf.lcol[i][c] = (float)vs.lightCol[i][c];
            }
        }
        for (int k = 0; k < 2; k++) {
            for (int c = 0; c < 3; c++) {
                xf.look[k][c] = vs.lookat[k][c] / 127.0f;
            }
        }
    }
    /* FLX alpha replaces texgen (GfxSpVertex: the LUT branch computes no UVs). */
    const float tg = (vs.lighting && vs.textureGen && !vs.flxAlpha) ? 1.0f : 0.0f;
    xf.tg[0] = tg;
    xf.tg[3] = 1.0f - tg;
}

/* Run-level uniforms: texture scale, the packed clip convention, the UV affine of PackVertex
 * (gdx3ds_uv_pack: u = min(U * a + b, clamp) + off) and the combiner input selects. */
void XfFillRun(const PackCtx& c, const GdxSeamMaterial& mat, uint16_t texS, uint16_t texT, const Gdx3dsVtxMathState& vs,
               Gdx3dsXfDraw& xf) {
    const Gdx3dsVboPackParams& p = *c.params;
    xf.tg[1] = (float)texS;
    xf.tg[2] = (float)texT;
    xf.tcs[0] = (float)texS / 65536.0f;
    xf.tcs[1] = (float)texT / 65536.0f;
    float xs = 1.0f;
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        xs = 160.0f / (160.0f - 12.0f);
    } else if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        xs = (4.0f / 3.0f) / vs.aspect;
    }
    xf.conv[0] = xs;
    xf.conv[1] = mat.clip.invertY ? -1.0f : 1.0f;
    xf.conv[2] = mat.clip.zIsFrom0To1 ? 0.5f : 1.0f;
    xf.conv[3] = 1.0f;
    xf.convw[2] = mat.clip.zIsFrom0To1 ? 0.5f : 0.0f;
    for (int t = 0; t < 2; t++) {
        xf.uvc[t][0] = 1.0e18f;
        xf.uvc[t][1] = 1.0e18f;
        if (!mat.usedTextures[t]) {
            continue;
        }
        const Gdx3dsUvPrep& u = c.uv[t];
        const float nu = u.uMul ? u.uNormMul : 1.0f / u.uNormDiv;
        const float nv = u.vMul ? u.vNormMul : 1.0f / u.vNormDiv;
        const float half = u.halfTexel ? 0.5f : 0.0f;
        xf.uva[t][0] = u.uPre / 32.0f * nu * p.uScale[t];
        xf.uva[t][1] = u.vPre / 32.0f * nv * p.vScale[t];
        xf.uva[t][2] = (half - u.uOff) * nu * p.uScale[t];
        xf.uva[t][3] = (half - u.vOff) * nv * p.vScale[t];
        xf.uvc[t][0] = c.clampU[t] < 1.0e18f ? c.clampU[t] : 1.0e18f;
        xf.uvc[t][1] = c.clampV[t] < 1.0e18f ? c.clampV[t] : 1.0e18f;
        xf.uvc[t][2] = p.hasOff ? p.uOff[t] : 0.0f;
        xf.uvc[t][3] = p.hasOff ? p.vOff[t] : 0.0f;
    }
    /* colour: out[6..8] of PackVertex */
    float selC = 0.0f;
    if (c.ci >= 0 && c.colorIsShade) {
        selC = 1.0f;
    } else if (c.ci >= 0) {
        xf.col[0] = gdx3ds_byte_to_unit(c.constColor.r);
        xf.col[1] = gdx3ds_byte_to_unit(c.constColor.g);
        xf.col[2] = gdx3ds_byte_to_unit(c.constColor.b);
    } else {
        xf.col[0] = xf.col[1] = xf.col[2] = 1.0f;
    }
    /* alpha: out[9] of PackVertex (the vertex alpha is the fog factor under G_FOG) */
    float selA = 0.0f;
    xf.col[3] = 1.0f;
    if (p.fogFactorToAlpha) {
        if (c.fogFactorConst) {
            xf.col[3] = c.fogFactorConstVal;
        } else {
            selA = 1.0f;
        }
    } else if (p.wantVtxAlpha) {
        if (c.alphaIsShade) {
            selA = c.shadeAlphaIsOne ? 0.0f : 1.0f;
        } else if (c.constAlphaIsShade) {
            selA = 1.0f;
        } else {
            xf.col[3] = gdx3ds_byte_to_unit(c.constAlpha.a);
        }
    }
    xf.sel[0] = selC;
    xf.sel[1] = selA;
    xf.seli[0] = 1.0f - selC;
    xf.seli[1] = 1.0f - selA;
}

/* ---- [native] machines_retained: GPU-resident per-(slot, LOD) buffers (C5 Stage 1) --------
 *
 * The gpuxf path above copies every mesh's raw Vtx into the frame pool per draw and patches
 * the FLX alpha byte into the copy on the CPU. The retained path draws from a buffer built
 * ONCE per (slot, LOD) instead: the same raw Vtx (a valid PICA vertex as-is), then the mesh's
 * u16 indices, in one linearAlloc. The FLX alpha moves into the shader (the stock LUT is three
 * analytic bands, shader.v.pica), guarded per draw by FlxLutIsStock -- any other DMEM table
 * takes the upload path, exactly as before.
 *
 * Ownership. The Entry owns the buffer; a Mesh only borrows it (Mesh::res). Buffers are
 * created, compared and retired ONLY on the drawing thread (DrawMesh), and freed from
 * RetireDrain one full frame after they became unreachable -- the same two-step deferral the
 * meshes use. C3D_FrameBegin(SYNCDRAW) waits for the previous frame's GPU queue before
 * gdx3ds_mesh_frame_begin runs, so a buffer freed two drains after its last draw is never
 * read by the PICA again. A rebake (sGeneration bump: race load, machine select, a custom
 * machine edit) rebuilds the buffer only when the new mesh's bytes differ from the resident
 * copy (rebuilds= vs reused= on the [rr] receipt). */
constexpr int kResRetireCap = 2 * kAllEntries;
uint8_t* sResPending[kResRetireCap];
uint8_t* sResStolen[kResRetireCap];
int sResPendingN = 0;
int sResStolenN = 0;
unsigned long sResBytes = 0; /* live resident bytes (all entries) */
int sResCount = 0;           /* live resident buffers */

void ResRetire(uint8_t* p, uint32_t bytes) {
    if (p == nullptr) {
        return;
    }
    sResBytes -= bytes;
    sResCount--;
    sWin.rrRetired++;
    if (sResPendingN < kResRetireCap) {
        sResPending[sResPendingN++] = p;
        return;
    }
    /* Cannot happen (at most one retire per entry per drain): leak rather than free early. */
}

/* Drawing thread, frame boundary (RetireDrain's cadence): free what the previous drain stole. */
void ResRetireDrain() {
    for (int i = 0; i < sResStolenN; i++) {
        linearFree(sResStolen[i]);
    }
    for (int i = 0; i < sResPendingN; i++) {
        sResStolen[i] = sResPending[i];
    }
    sResStolenN = sResPendingN;
    sResPendingN = 0;
}

inline uint32_t ResIdxOffset(const Mesh* m) {
    return (uint32_t)m->nVerts * 16u;
}

inline uint32_t ResBytesFor(const Mesh* m) {
    return ResIdxOffset(m) + (uint32_t)m->nTris * 3u * (uint32_t)sizeof(uint16_t);
}

bool ResMatches(const Entry& e, const Mesh* m) {
    if (e.res == nullptr || e.resBytes != ResBytesFor(m)) {
        return false;
    }
    for (unsigned ci = 0; ci < m->nChunks; ci++) {
        const Gdx3dsMeshChunk& ch = m->chunks[ci];
        if (memcmp(e.res + (size_t)ch.baseVertex * 16u, ch.src, (size_t)ch.count * 16u) != 0) {
            return false;
        }
    }
    return memcmp(e.res + ResIdxOffset(m), m->tris, (size_t)m->nTris * 3u * sizeof(uint16_t)) == 0;
}

uint8_t* ResBuild(const Mesh* m) {
    const uint32_t bytes = ResBytesFor(m);
    uint8_t* p = (uint8_t*)linearAlloc(bytes);
    if (p == nullptr) {
        return nullptr;
    }
    for (unsigned ci = 0; ci < m->nChunks; ci++) {
        const Gdx3dsMeshChunk& ch = m->chunks[ci];
        memcpy(p + (size_t)ch.baseVertex * 16u, ch.src, (size_t)ch.count * 16u);
    }
    memcpy(p + ResIdxOffset(m), m->tris, (size_t)m->nTris * 3u * sizeof(uint16_t));
    gdx3ds_mesh_backend_flush_resident(p, bytes);
    return p;
}

/* The mesh's resident buffer, built or re-validated on its first retained draw. NULL = the
 * upload path draws it (no linear memory). Drawing thread only. */
GDX_COLD const uint8_t* EnsureResidentSlow(Mesh* m, int entryIdx);

const uint8_t* EnsureResident(Mesh* m, int entryIdx) {
    if (GDX_LIKELY(m->resState == 1)) {
        return m->res;
    }
    if (m->resState == 2) {
        return nullptr;
    }
    return EnsureResidentSlow(m, entryIdx);
}

/* hotcold: the first retained draw of a mesh (re-validate or build its resident buffer). */
GDX_COLD const uint8_t* EnsureResidentSlow(Mesh* m, int entryIdx) {
    Entry& e = sEntries[entryIdx];
    if (ResMatches(e, m)) {
        sWin.rrReused++;
        sTot.rrReused++;
    } else {
        uint8_t* p = ResBuild(m);
        if (p == nullptr) {
            m->resState = 2;
            sWin.rrFbAlloc++;
            return nullptr;
        }
        ResRetire(e.res, e.resBytes);
        e.res = p;
        e.resBytes = ResBytesFor(m);
        sResBytes += e.resBytes;
        sResCount++;
        sWin.rrBuilds++;
        sTot.rrBuilds++;
        gdx3ds_hitch_add(GDX_HITCH_EV_RR_BUILD, 1u); /* HITCH */
    }
    m->res = e.res;
    m->resState = 1;
    return m->res;
}

/* racer.c D_800CE3D0 in its analytic form (shader.v.pica xlut0..2): the alpha for LUT index i. */
constexpr int FlxLutAlpha(int i) {
    const int a1 = (10 * i - 230) < (450 - 10 * i) ? (10 * i - 230) : (450 - 10 * i);
    const int b1 = a1 < 60 ? a1 : 60;
    const int d2 = i - 100 < 0 ? 100 - i : i - 100;
    const int d3 = i - 117 < 0 ? 117 - i : i - 117;
    return (b1 > 0 ? b1 : 0) + (110 - 20 * d2 > 0 ? 110 - 20 * d2 : 0) + (230 - 30 * d3 > 0 ? 230 - 30 * d3 : 0);
}
/* Spot checks against the table in racer.c (every non-zero run and its neighbours). */
static_assert(FlxLutAlpha(23) == 0 && FlxLutAlpha(24) == 10 && FlxLutAlpha(29) == 60 && FlxLutAlpha(39) == 60 &&
                  FlxLutAlpha(40) == 50 && FlxLutAlpha(44) == 10 && FlxLutAlpha(45) == 0,
              "FLX LUT band 1");
static_assert(FlxLutAlpha(94) == 0 && FlxLutAlpha(95) == 10 && FlxLutAlpha(100) == 110 && FlxLutAlpha(105) == 10 &&
                  FlxLutAlpha(106) == 0,
              "FLX LUT band 2");
static_assert(FlxLutAlpha(109) == 0 && FlxLutAlpha(110) == 20 && FlxLutAlpha(117) == 230 &&
                  FlxLutAlpha(124) == 20 && FlxLutAlpha(125) == 0 && FlxLutAlpha(127) == 0 && FlxLutAlpha(-128) == 0,
              "FLX LUT band 3");

uint8_t sFlxLut[256];
bool sFlxLutInit = false;

/* The shader computes the analytic LUT, so a draw may use it only when DMEM holds exactly that
 * table where GfxSpVertex would index it (256 bytes from dma_io_dmem). */
bool FlxLutIsStock(const Gdx3dsVtxMathState& vs) {
    if (!sFlxLutInit) {
        for (int k = 0; k < 256; k++) {
            sFlxLut[k] = (uint8_t)FlxLutAlpha(k - 128);
        }
        sFlxLutInit = true;
    }
    if (vs.dmem == nullptr || vs.dmaIoDmem < 0 || vs.dmaIoDmem + 256 > GDX3DS_VTX_DMEM_SIZE) {
        return false;
    }
    return memcmp(vs.dmem + vs.dmaIoDmem, sFlxLut, sizeof(sFlxLut)) == 0;
}

inline bool MeshUsesFlx(const Gdx3dsVtxMathState& vs) {
    return vs.lighting && vs.textureGen && vs.flxAlpha;
}

/* The retained vertex source for this draw, or NULL = the gpuxf upload path. */
const uint8_t* RetainedSource(Mesh* m, int entryIdx, const Gdx3dsVtxMathState& vs) {
    if (!sRetained || entryIdx < 0) {
        return nullptr;
    }
    if (MeshUsesFlx(vs) && !FlxLutIsStock(vs)) {
        sWin.rrFbLut++;
        return nullptr;
    }
    return EnsureResident(m, entryIdx);
}

void XfFillFlx(const Gdx3dsVtxMathState& vs, Gdx3dsXfDraw& xf) {
    if (!MeshUsesFlx(vs)) {
        return; /* flx = 0: the shader keeps the resident cn.a, as the upload path's raw copy does */
    }
    xf.flx[0] = vs.alphaLight[0];
    xf.flx[1] = vs.alphaLight[1];
    xf.flx[2] = vs.alphaLight[2];
    xf.flx[3] = 1.0f;
    sWin.rrFlx++;
}

/* C6 [replay]: the x scale gdx3ds_vtx_transform packs a CPU vertex with (gdx3ds_vtx_math.h). */
float ReplayXscaleOf(const Gdx3dsVtxMathState& vs) {
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        return 160.0f / (160.0f - 12.0f);
    }
    if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        return (4.0f / 3.0f) / vs.aspect;
    }
    return 1.0f;
}

/* n60 NRR: inside an armed record window (the stock commands of a not-yet-recorded machine) the
 * legacy draw runs in performance mode, so every run of the mesh is bound once and recorded. */
inline bool NrrRecArmed(int entryIdx) {
    return &gdx3ds_nrr_mach_recording != nullptr && gdx3ds_nrr_mach_recording(entryIdx) != 0;
}

bool sNrrRecOpen = false; /* this XfDraw records its runs (gdx3ds_nrr_mach_rec_begin said yes) */

/* The recorder's view of one bound run: which aux inputs (and the xf constant colour) read the env
 * register, and whether the run inherits it from the body section's gDPSetEnvColor (no run up to
 * it set its own). A far speck drawn with a predecessor's last-run state takes the racer's env on
 * every env input regardless (its gDPSetEnvColor comes after that run). */
GDX_COLD void NrrRecRun(const Mesh* m, unsigned r, const GdxSeamMaterial& mat, const PackCtx& c, const GdxSeamRegs& R,
                        const Gdx3dsXfDraw& xf, const Gdx3dsVboPackAux& aux, const uint16_t* idx, uint32_t nIdx) {
    bool envInherited = true;
    for (unsigned k = 0; k <= r; k++) {
        if (m->runs[k].envKind == GDX3DS_MESH_COLOR_CONST) {
            envInherited = false;
        }
    }
    uint8_t envMask = 0;
    for (int j = 0; j < aux.numInputs && j < 7; j++) {
        if (mat.inputMap[0][j] == GDXSEAM_CCMUX_ENVIRONMENT) {
            envMask |= (uint8_t)(1u << j);
        }
    }
    if (envInherited) {
        envMask |= 0x80u; /* the run takes the body section's gDPSetEnvColor */
    }
    uint8_t colEnv = (c.ci >= 0 && !c.colorIsShade && mat.inputMap[0][c.ci] == GDXSEAM_CCMUX_ENVIRONMENT) ? 1 : 0;
    if (c.fogFactorConst) {
        colEnv |= 2u; /* nimach: xf.col alpha is the RDP fog colour's alpha (XfFillRun) */
    }
    gdx3ds_nrr_mach_rec_run(&xf, &aux, idx, nIdx, envMask, colEnv, R.env_color->a, mat.use_blend_color ? 1 : 0);
}

/* One surviving run on the GPU. false = refused before anything was drawn (caller walks). */
bool XfRun(GdxInterpSeam& g, const Mesh* m, unsigned r, const GdxSeamMaterial& mat, Gdx3dsVtxMathState& vs,
           int firstTri, Gdx3dsXfDraw& xf, const uint8_t* res) {
    const Gdx3dsMeshRun& run = m->runs[r];
    struct EmitTimer {
        uint64_t t0;
        ~EmitTimer() {
            sWin.emitTicks += svcGetSystemTick() - t0;
        }
    } emitTimer = { svcGetSystemTick() };
    for (int j = 0; j < mat.numInputs && j < 7; j++) {
        if (mat.inputMap[0][j] == GDXSEAM_CCMUX_LOD_FRACTION || mat.inputMap[1][j] == GDXSEAM_CCMUX_LOD_FRACTION) {
            return false;
        }
    }
    Gdx3dsVboPackParams params;
    memset(&params, 0, sizeof(params));
    if (!gdx3ds_mesh_backend_xf_params(&params)) {
        sWin.xfFbParams++;
        return false;
    }
    XFP(XFP_PARAMS);
    const uint16_t* tris = &m->tris[(size_t)run.firstTri * 3];
    const Gdx3dsLoadedVtx first = *XfVertex(vs, m, tris[(size_t)firstTri * 3]);
    XFP(XFP_VERTEX);
    PackCtx c;
    if (!PreparePackCtx(g, mat, params, &first, c)) {
        return false;
    }
    XFP(XFP_PACKCTX);
    XfFillRun(c, mat, m->xfRunTex[r * 2 + 0], m->xfRunTex[r * 2 + 1], vs, xf);
    XFP(XFP_FILLRUN);
    Gdx3dsVboPackAux aux;
    BuildAux(g.R(), mat, first, aux);
    const int cullMode = CullModeOf(*g.R().geometry_mode);
    const bool invert = (*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0;
    xf.cullMode = cullMode;
    if (invert && cullMode == GDX3DS_CULL_FRONT) {
        xf.cullMode = GDX3DS_CULL_BACK;
    } else if (invert && cullMode == GDX3DS_CULL_BACK) {
        xf.cullMode = GDX3DS_CULL_FRONT;
    }
    xf.clsW = first.w;
    g.ApplyDrawConstants();
    /* Retained: the run's indices are a slice of the resident buffer (drawn in place). */
    const uint16_t* idx = tris;
    xf.idxResident = 0;
    if (res != nullptr) {
        idx = (const uint16_t*)(res + ResIdxOffset(m)) + (size_t)run.firstTri * 3u;
        xf.idxResident = 1;
        sWin.rrRuns++;
    }
    XFP(XFP_AUX);
    const uint64_t td0 = svcGetSystemTick();
    gdx3ds_xfprof_inrun = 1;
    gdx3ds_mesh_backend_xf_draw(idx, (uint32_t)run.triCount * 3u, &aux, &xf);
    gdx3ds_xfprof_inrun = 0;
    if (GDX_UNLIKELY(sNrrRecOpen) && res != nullptr) {
        NrrRecRun(m, r, mat, c, g.R(), xf, aux, idx, (uint32_t)run.triCount * 3u);
    }
    sWin.drawTicks += svcGetSystemTick() - td0;
    XFP(XFP_B_TAIL);
    sWin.xfRuns++;
    sWin.xfTris += run.triCount;
    sWin.runs++;
    sTot.runs++;
    return true;
}

/* ---- [native] machines_mattab: the per-run material records (C5 Stage 1b) ---------------
 *
 * One record per (slot, LOD, run), owned by the Mesh (so a rebake -- a new Mesh -- starts
 * empty, and the records die with it), allocated on the mesh's first gpuxf draw with the lever
 * on and touched only by the drawing thread (DrawMesh). The interpreter owns what goes in them
 * and proves when one may be served (gdx_tri.cpp MachMatOps); this side only stores them and
 * counts what happened. */
GdxSeamMatRec* MatRecsFor(Mesh* m) {
    if (m->matState == 0) {
        if (m->matWays == 0) {
            m->matWays = GDXSEAM_MATREC_WAYS;
        }
        const size_t n = (size_t)(m->nRuns > 0 ? m->nRuns : 1u) * m->matWays;
        m->matRecs = (GdxSeamMatRec*)calloc(n, sizeof(GdxSeamMatRec));
        m->matState = m->matRecs != nullptr ? 1 : 2;
        if (m->matRecs != nullptr) {
            sMatBytes += (unsigned long)(n * sizeof(GdxSeamMatRec));
        }
    }
    return m->matRecs;
}

void NoteMatOutcome(const GdxSeamMatOutcome& o) {
    if (o.result < GDXSEAM_MM_RESULT_COUNT) {
        sWin.mtOut[o.result]++;
    }
    sWin.mtBanked += o.banked;
    gdx3ds_hitch_add(GDX_HITCH_EV_MATTAB_BANK, o.banked); /* HITCH */
    sWin.mtInval += o.wasValid;
    sWin.mtBinds += o.binds;
    sWin.mtChecked += o.checked;
    sWin.mtMismatch += o.mismatch;
    sWin.mtLive += (o.live & 1u) + ((o.live >> 1) & 1u);
    for (int b = 0; b < 10; b++) {
        sWin.mtKeyBits[b] += (o.keyBits >> b) & 1u;
    }
}

/* [mattab] run-order analysis: one mesh's bound runs in stock order. */
constexpr int kOrdMax = 16;
struct RunOrder {
    const void* prg[kOrdMax];
    uint32_t tex[kOrdMax];
    uint8_t pinned[kOrdMax];
    int n;
};

inline uint32_t RunTexIdentity(const GdxSeamMaterial& mat) {
    if (!mat.usedTextures[0]) {
        return 0xFFFFFFFFu;
    }
    /* Within one machine mesh every tile holds its own texture (the texture-set list packs
     * one texture per tile), so the tile plus its extent identifies the texture. */
    return (mat.effective_tile[0] << 24) ^ (mat.tex_width[0] << 12) ^ mat.tex_height[0];
}

void NoteRunOrder(RunOrder& o, const GdxInterpSeam& g, const GdxSeamMaterial& mat) {
    if (o.n >= kOrdMax) {
        return;
    }
    const bool decal = (*g.R().other_mode_l & (3u << 10)) == (3u << 10); /* ZMODE_DEC */
    o.prg[o.n] = mat.prg;
    o.tex[o.n] = RunTexIdentity(mat);
    o.pinned[o.n] = (mat.use_alpha || decal) ? 1 : 0;
    o.n++;
}

unsigned DistinctOpaque(const RunOrder& o, bool tex) {
    unsigned distinct = 0;
    for (int i = 0; i < o.n; i++) {
        if (o.pinned[i]) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = !o.pinned[j] && (tex ? o.tex[j] == o.tex[i] : o.prg[j] == o.prg[i]);
        }
        distinct += seen ? 0u : 1u;
    }
    return distinct;
}

void CloseRunOrder(const RunOrder& o) {
    if (o.n == 0) {
        return;
    }
    unsigned pinned = 0;
    for (int i = 0; i < o.n; i++) {
        pinned += o.pinned[i];
        if (i > 0) {
            sWin.ordPrgSw += o.prg[i] != o.prg[i - 1] ? 1u : 0u;
            sWin.ordTexSw += o.tex[i] != o.tex[i - 1] ? 1u : 0u;
        }
    }
    const unsigned dPrg = DistinctOpaque(o, false);
    const unsigned dTex = DistinctOpaque(o, true);
    /* floor: each distinct opaque material entered once, each pinned run entered (at worst) once */
    sWin.ordPrgMin += (dPrg + pinned) > 0 ? dPrg + pinned - 1u : 0u;
    sWin.ordTexMin += (dTex + pinned) > 0 ? dTex + pinned - 1u : 0u;
    sWin.ordRuns += (unsigned)o.n;
    sWin.ordPinned += pinned;
}

/* The run's BindMaterial, through its record when the lever is on. */
bool BindRunMaterial(GdxInterpSeam& g, Mesh* m, unsigned r, GdxSeamMaterial& mat) {
    if (!sMatTab) {
        return g.BindMaterial(mat);
    }
    GdxSeamMatRec* recs = MatRecsFor(m);
    if (recs == nullptr) {
        sWin.mtNoHeap++;
        return g.BindMaterial(mat);
    }
    GdxSeamMatOutcome o;
    memset(&o, 0, sizeof(o));
    const int mode = sMatTab | (sMatLive ? GDXSEAM_MATREC_LIVE : 0);
    const int rc = g.BindMaterialRec(&recs[(size_t)r * m->matWays], m->matWays, mode, mat, o);
    if (rc < 0) {
        return g.BindMaterial(mat); /* this interpreter keeps no records (the LUS seam) */
    }
    NoteMatOutcome(o);
    return rc == 1;
}

/* The mesh-level refusals: what the shader cannot express. */
bool XfMeshEligible(const Mesh* m, const Gdx3dsVtxMathState& vs) {
    if (vs.lighting && vs.textureGen && !vs.flxAlpha && vs.textureGenLinear) {
        sWin.xfFbTgLin++;
        return false;
    }
    if (vs.lighting && vs.numDir > 3) {
        sWin.xfFbLights++;
        return false;
    }
    if (!m->xfScaleOk) {
        sWin.xfFbTexs++;
        return false;
    }
    return true;
}

/* Every run of an invisible mesh: exactly what the CPU path does when RunHasSurvivor says no
 * (state applied, nothing resolved or drawn). */
void XfSkipInvisible(GdxInterpSeam& g, const Mesh* m) {
    for (unsigned r = 0; r < m->nRuns; r++) {
        const Gdx3dsMeshRun& run = m->runs[r];
        ApplyRunState(g, run);
        if (run.triCount == 0) {
            continue;
        }
        sWin.emptyRuns++;
        sTot.emptyRuns++;
        sWin.tris += run.triCount;
        sTot.tris += run.triCount;
    }
    sWin.xfInvisible++;
}

/* F3DFLX alpha: gdx3ds_vtx_transform's LUT lookup, verbatim (same float expression, same
 * truncation and clamps), written into the uploaded copy's alpha byte -- the shader reads that
 * byte as the vertex alpha whenever G_FOG is off, which is exactly when the CPU keeps it. The
 * one per-vertex CPU stage this path keeps: 3 multiplies and a byte load instead of a T&L. */
void XfPatchFlxAlpha(const Gdx3dsVtxMathState& vs, uint8_t* v, unsigned count) {
    for (unsigned k = 0; k < count; k++, v += 16) {
        const int8_t* n = (const int8_t*)(v + 12);
        const float intensity = vs.alphaLight[0] * n[0] + vs.alphaLight[1] * n[1] + vs.alphaLight[2] * n[2];
        int lutIndex = (int)intensity;
        if (lutIndex < -128) {
            lutIndex = -128;
        }
        if (lutIndex > 127) {
            lutIndex = 127;
        }
        const int dmemIndex = vs.dmaIoDmem + 128 + lutIndex;
        if (dmemIndex >= 0 && dmemIndex < GDX3DS_VTX_DMEM_SIZE) {
            v[15] = vs.dmem[dmemIndex];
        }
    }
}

void XfUploadVertices(const Mesh* m, const Gdx3dsVtxMathState& vs, void* dst) {
    const bool flx = vs.lighting && vs.textureGen && vs.flxAlpha;
    uint8_t* out = (uint8_t*)dst;
    for (unsigned ci = 0; ci < m->nChunks; ci++) {
        const Gdx3dsMeshChunk& ch = m->chunks[ci];
        uint8_t* d = out + (size_t)ch.baseVertex * 16u;
        memcpy(d, ch.src, (size_t)ch.count * 16u);
        if (flx) {
            XfPatchFlxAlpha(vs, d, ch.count);
        }
    }
    if (flx) {
        sWin.xfFbFlx++; /* repurposed: meshes whose alpha came from the FLX LUT */
    }
}


/* The GPU-transform draw of a whole mesh. false = not taken (nothing was touched; the CPU
 * path draws it). */
/* Performance mode ([native] machines_retained_clip=1) may hand the PICA a mesh that straddles
 * the clip volume, but only when every box corner is in front of the eye: the error bound
 * below divides by the smallest corner w, so it stays a real bound, and near-plane crossers
 * (rare for a machine; the camera sits behind the player) keep the CPU clipper. */
inline bool StraddleAllowed(float wMin, int entryIdx) {
    return (sRetainedClip || NrrRecArmed(entryIdx)) && wMin > kXfMinW;
}



bool XfDraw(GdxInterpSeam& g, Mesh* m, Gdx3dsVtxMathState& vs, int entryIdx) {
    if (!XfMeshEligible(m, vs)) {
        return false;
    }
    XfBox box;
    if (!XfMeshBox(m, box)) {
        return false;
    }
    float wMin = 0.0f;
    const XfCover cover = XfClassify(vs, box, &wMin);
    if (cover == XF_OUTSIDE) {
        XfSkipInvisible(g, m);
        return true;
    }
    const bool straddle = cover == XF_STRADDLE;
    if (straddle && !StraddleAllowed(wMin, entryIdx)) {
        sWin.xfFbClip++;
        return false;
    }
    const unsigned errMpx = XfErrorBoundMpx(vs, box, wMin);
    if (errMpx > sWin.xfMaxErrMpx) {
        sWin.xfMaxErrMpx = errMpx;
    }
    if (sGpuXfMaxErr >= 0 && errMpx > (unsigned)sGpuXfMaxErr) {
        sWin.xfFbErr++;
        return false;
    }
    const uint8_t* res = RetainedSource(m, entryIdx, vs);
    if (straddle && res == nullptr) {
        sWin.xfFbClip++; /* only the retained path draws straddlers */
        return false;
    }
    const void* verts = res;
    if (res == nullptr) {
        void* pool = gdx3ds_mesh_backend_xf_vertices(m->nVerts);
        if (pool == nullptr) {
            sWin.xfFbPool++;
            return false;
        }
        XfUploadVertices(m, vs, pool);
        verts = pool;
    }
    sWin.xfVerts += m->nVerts;
    sWin.verts += m->nVerts;
    sTot.verts += m->nVerts;
    sXfEpoch++;
    Gdx3dsXfDraw xf;
    XfFillMesh(vs, xf);
    xf.stateElide = sMatTab;
    if (res != nullptr) {
        XfFillFlx(vs, xf);
        sWin.rrMeshes++;
        sWin.rrStraddle += straddle ? 1u : 0u;
    }
    xf.verts = verts;
    xf.nVerts = m->nVerts;
    /* Parity mode keeps the survivor screen (an invisible run is never bound, exactly as
     * GfxSpTri1 leaves it); performance mode binds every non-empty run and lets the PICA cull. */
    const bool recArmed = res != nullptr && NrrRecArmed(entryIdx);
    const bool screen = !(res != nullptr && (sRetainedClip || recArmed));
    sNrrRecOpen = recArmed && gdx3ds_nrr_mach_rec_begin(entryIdx, m->nRuns, res) != 0;
    int nrrNeed = 0;
    const bool rejVariant = g.RejectVariant();
    const bool cpuClipOn = CpuClipEnabled();
    const int cullMode = CullModeOf(*g.R().geometry_mode);
    const int cullInvert = (*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0;
    RunOrder order;
    order.n = 0;
    for (unsigned r = 0; r < m->nRuns; r++) {
        const Gdx3dsMeshRun& run = m->runs[r];
        XFP(XFP_MESH);
        const uint64_t tb0 = svcGetSystemTick();
        ApplyRunState(g, run);
        XFP(XFP_STATE);
        if (run.triCount == 0) {
            continue;
        }
        nrrNeed++;
        sWin.tris += run.triCount;
        sTot.tris += run.triCount;
        int firstTri = 0;
        uint64_t stateScreenTicks = 0; /* [mattab] bind split: run state + survivor screen */
        if (screen) {
            firstTri = XfFirstSurvivor(vs, m, run, rejVariant, cullMode, cullInvert, cpuClipOn);
            XFP(XFP_SCREEN);
            if (sMatTab) {
                stateScreenTicks = svcGetSystemTick() - tb0;
            }
        } else {
            sWin.rrNoScreen++;
        }
        if (firstTri < 0) {
            sWin.emptyRuns++;
            sTot.emptyRuns++;
            sWin.bindTicks += svcGetSystemTick() - tb0;
            continue;
        }
        GdxSeamMaterial mat;
        memset(&mat, 0, sizeof(mat));
        const bool bound = BindRunMaterial(g, m, r, mat);
        XFP(XFP_BIND);
        const uint64_t bindTicks = svcGetSystemTick() - tb0;
        sWin.bindTicks += bindTicks;
        sWin.mtBindTicks += bindTicks;
        sWin.mtScrTicks += stateScreenTicks;
        if (bound && sMatTab) {
            NoteRunOrder(order, g, mat);
        }
        if (bound && XfRun(g, m, r, mat, vs, firstTri, xf, res)) {
            continue;
        }
        sWin.bindFallbacks++;
        sTot.bindFallbacks++;
        WalkRun(g, m, run);
        g.Flush();
        sWin.walkDraws++;
        sTot.walkDraws++;
        sWin.xfWalk++;
    }
    CloseRunOrder(order);
    if (sNrrRecOpen) {
        sNrrRecOpen = false;
        gdx3ds_nrr_mach_rec_end(nrrNeed);
    }
    sWin.xfMeshes++;
    return true;
}

void NativeDraw(GdxInterpSeam& g, Mesh* m, bool allowWalkFallback, int entryIdx) {
    XFP_START();
    bool native = true;
    Gdx3dsVtxMathState vs;
    if (GDX_UNLIKELY(!g.LatchVtxState(vs))) {
        native = false;
        sWin.bindFallbacks++;
        sTot.bindFallbacks++;
    } else if (gdx3ds_replay_rec) {
        gdx3ds_replay_set_mesh_xscale(ReplayXscaleOf(vs)); /* C6: the CPU runs' packed x scale */
        gdx3ds_replay_note_mesh_mp(&vs.mp[0][0]);
    }
    /* [native] dress_retained: a fogged decoration stays on the CPU path unless
     * dress_retained_fog=1 -- the shader's fog factor (rcp * z, then flr) can differ from the
     * CPU's z / w by one ulp at an integer boundary, i.e. one RGB5 quantum of fog. */
    /* [native] nrr_deco: a decoration drawn inside a record window takes the GPU transform (the
     * record is what that draw binds; the NRR then draws it within the 3D tolerance, design 7.3). */
    const bool xfFogRefused = sDressCpuIfFog && vs.fog && !NrrRecArmed(entryIdx);
    if (native && sGpuXf && allowWalkFallback && !xfFogRefused) {
        const uint64_t tx0 = svcGetSystemTick();
        const bool taken = XfDraw(g, m, vs, entryIdx);
        sWin.xfTicks += svcGetSystemTick() - tx0;
        if (taken) {
            return;
        }
    }
    if (native) {
        const uint64_t tt0 = svcGetSystemTick();
        /* T&L every mesh vertex once, with the texture scale in force at its G_VTX. */
        for (unsigned ci = 0; ci < m->nChunks; ci++) {
            const Gdx3dsMeshChunk& ch = m->chunks[ci];
            vs.texScaleS = ch.texS;
            vs.texScaleT = ch.texT;
            const uint8_t* src = (const uint8_t*)ch.src;
            for (unsigned k = 0; k < ch.count; k++) {
                gdx3ds_vtx_transform(&vs, (const Gdx3dsVtxIn*)(src + (size_t)k * 16u), &sLoaded[ch.baseVertex + k]);
            }
            if (gdx3ds_xf_census_on) {
                XfCensusChunk(vs, ch);
            }
        }
        sWin.tnlTicks += svcGetSystemTick() - tt0;
    }
    const bool rejVariant = g.RejectVariant();
    const bool cpuClipOn = CpuClipEnabled(); /* one read per draw, shared by the screen and the emit loop */
    const int cullMode = native ? CullModeOf(*g.R().geometry_mode) : GDX3DS_CULL_NONE;
    const int cullInvert = native ? ((*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0) : 0;
    for (unsigned r = 0; r < m->nRuns; r++) {
        const Gdx3dsMeshRun& run = m->runs[r];
        const uint64_t tb0 = svcGetSystemTick();
        ApplyRunState(g, run);
        if (run.triCount == 0) {
            continue;
        }
        /* An invisible run must not resolve draw state — GfxSpTri1 returns before its state
         * block for every screened-out triangle. See RunHasSurvivor. */
        if (native && !RunHasSurvivor(m, run, rejVariant, cullMode, cullInvert, cpuClipOn)) {
            sWin.emptyRuns++;
            sTot.emptyRuns++;
            sWin.tris += run.triCount;
            sTot.tris += run.triCount;
            sWin.bindTicks += svcGetSystemTick() - tb0;
            continue;
        }
        bool done = false;
        if (native) {
            GdxSeamMaterial mat;
            memset(&mat, 0, sizeof(mat));
            /* [native] dress_retained: a decoration's CPU-path runs use the material records too
             * (machines_mattab; the machines' own CPU fallback keeps the full resolve). */
            const bool bound = entryIdx >= kEntries ? BindRunMaterial(g, m, r, mat) : g.BindMaterial(mat);
            sWin.bindTicks += svcGetSystemTick() - tb0;
            if (bound) {
                done = NativeRun(g, m, run, mat, rejVariant, cpuClipOn);
            }
            if (!done) {
                sWin.bindFallbacks++;
                sTot.bindFallbacks++;
            }
        }
        if (!done && (allowWalkFallback || !native)) {
            WalkRun(g, m, run);
            g.Flush();
            sWin.walkDraws++;
            sTot.walkDraws++;
        }
    }
}

uint64_t sDrawTicksTotal = 0; /* monotone DrawMesh time, drawing thread ([debug] machsplit) */

/* hotcold: DrawMesh's ownership-assertion trip (a stale mesh reached a draw). */
GDX_COLD void StaleDraw(const Mesh* m, int slot, int lod) {
    Bump(sLife.staleDraw);
    if (sLifeLogs < 8) {
        sLifeLogs++;
        Log("[native] meshlife STALE DRAW tick=%llu tid=%lu slot=%d lod=%d mesh=%p magic=%08lx",
            (unsigned long long)svcGetSystemTick(), (unsigned long)SelfTid(), slot, lod + 1, (const void*)m,
            (unsigned long)(m != nullptr ? m->magic : 0u));
    }
}

/* hotcold: [native] machines_walk_only (an instrument): every run walked by the interpreter. */
GDX_COLD void WalkOnlyDraw(GdxInterpSeam& g, const Mesh* m) {
    for (unsigned r = 0; r < m->nRuns; r++) {
        ApplyRunState(g, m->runs[r]);
        if (m->runs[r].triCount != 0) {
            WalkRun(g, m, m->runs[r]);
        }
    }
    g.Flush();
    sWin.walkDraws++;
    sTot.walkDraws++;
    sWin.draws++;
    sTot.draws++;
}

void DrawMesh(GdxInterpSeam& g, Mesh* m, int slot, int lod, int entryIdx) {
    /* Debug assertion for the ownership invariant: a live mesh always carries kMeshMagic,
     * and FreeMesh poisons the header before releasing it. A nonzero stale= on the
     * `[native] meshlife` receipt means something freed a mesh a draw still held -- i.e.
     * the race this file's ownership note exists to prevent has regressed. */
    if (GDX_UNLIKELY(m == nullptr || m->magic != kMeshMagic)) {
        StaleDraw(m, slot, lod);
        return;
    }
    /* COVERAGE: which machine slot at which LOD actually reached the screen this frame. The
     * 4P material bug (B10/machfix) was invisible to a frame hash unless the scene also
     * recycled a texture id, and the LOD ladder is exactly the axis a hand-placed SHOT never
     * walked. value = (lod << 8) | slot. */
    if (slot < kSlots) {
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_MACH,
                                       ((uint32_t)(lod & 0xFF) << 8) | (uint32_t)(slot & 0xFF)));
    }
    const uint64_t t0 = svcGetSystemTick();
    sDrawingTid.store(SelfTid(), std::memory_order_relaxed);
    sDrawingMesh.store((uintptr_t)m, std::memory_order_release);
    g.Flush();
    if (sVerify) {
        VerifyDraw(g, m, slot, lod);
    } else if (sWalkOnly) {
        WalkOnlyDraw(g, m);
    } else {
        NativeDraw(g, m, true, entryIdx);
        sWin.draws++;
        sTot.draws++;
    }
    sDrawingMesh.store(0, std::memory_order_release);
    const uint64_t dt = svcGetSystemTick() - t0;
    sWin.ticks += dt;
    sDrawTicksTotal += dt;
}

/* Registry lookup + lazy bake for a resolved list pointer (the bridge's G_DL conversion and
 * the owned walker's G_DL hook share it). Returns the entry to draw, or NULL to keep the list
 * on the interpreter (not registered / masked / bake failed / feature off). */
Entry* LookupForDraw(const void* src, Gdx3dsMeshResolveFn resolve, void* user) {
    if (!Enabled()) {
        return nullptr;
    }
    EnsureRegistry();
    Entry* e = Find(src);
    if (e == nullptr) {
        return nullptr;
    }
    if ((sSlotMask & (1u << e->slot)) == 0 || (sLodMask & (1u << e->lod)) == 0) {
        return nullptr;
    }
    /* Overlap census: this registry read is happening on the conversion (game) thread with
     * a render task live, i.e. inside the bridgemain window the free used to land in. */
    const bool offThread = OffThreadWhileRendering();
    if (offThread) {
        Bump(sLife.lookupOvl);
    }
    const uint32_t gen = sGeneration.load(std::memory_order_acquire);
    if (e->mesh == nullptr || e->gen != gen) {
        if (offThread) {
            Bump(sLife.bakeOvl); /* a BAKE -- and pre-fix, a FreeMesh -- inside that window */
        }
        if (e->failed && e->gen == gen) {
            return nullptr;
        }
        if (resolve == nullptr || !Bake(*e, resolve, user)) {
            return nullptr;
        }
    }
    return e;
}

/* [native] machines_speck: the far speck's entry. Every slot's LOD-6 list is the same list
 * (D_800CDDB0[slot*6+5] == D_7045098), so one entry serves them all: slot 0's LOD-6 index,
 * which the body registry never fills (it registers LODs 1-5). It is keyed on the RESOLVED
 * list (the walker resolves the placeholder operand to the loaded asset image); a different
 * resolution re-registers it and rebakes, and the retained buffer is rebuilt only when the
 * bytes differ. Not hashed: Find() and everything keyed on it (texskip's body probes) are
 * unchanged. Drawing thread only (the owned walker's G_DL hook). */
constexpr int kSpeckEntry = kNativeLods;
unsigned sSpeckDraws = 0, sSpeckFails = 0; /* window counts ([speck] receipt) */
const void* sSpeckKey = nullptr;           /* the resolved list the entry was registered for */

/* `packets` is the speck list re-packed as host-built 16-byte packets (the bake walker's input;
 * the asset image holds 8-byte packets), stable for as long as `resolved` is. */
Entry* LookupSpeckForDraw(const void* resolved, const void* packets, Gdx3dsMeshResolveFn resolve, void* user) {
    if (!sSpeck || !Enabled() || resolve == nullptr || resolved == nullptr || packets == nullptr) {
        return nullptr;
    }
    EnsureRegistry();
    Entry& e = sEntries[kSpeckEntry];
    if (sSpeckKey != resolved || e.src != packets) {
        sSpeckKey = resolved;
        e.src = packets;
        e.slot = 0;
        e.lod = (uint8_t)kNativeLods;
        e.gen = 0; /* forces the rebake below; the old mesh stays installed until it lands */
        e.failed = 0;
    }
    const uint32_t gen = sGeneration.load(std::memory_order_acquire);
    if (e.mesh == nullptr || e.gen != gen) {
        if (e.failed && e.gen == gen) {
            return nullptr;
        }
        if (!Bake(e, resolve, user)) {
            sSpeckFails++;
            return nullptr;
        }
    }
    if (e.mesh != nullptr && e.mesh->matState == 0) {
        e.mesh->matWays = GDXSEAM_MATREC_WAYS_MAX; /* before its records are allocated */
    }
    return &e;
}

} // namespace

/* ---- n60 nrr_trk: one material's gpuxf state (gfx/nrr/gdx3ds_nrr_trk.h) --------------------------
 *
 * The NRR track draws resident rings under whatever material the interpreter's registers hold at
 * its marker (the lean walk emitted the stock texture-state GBI before it). This is XfRun's state
 * half for such a draw, with nothing about a mesh: bind the material through the caller's records
 * (BindMaterialRec, the machines_mattab proof), then the run uniforms and the aux record from the
 * first vertex, exactly as a machine run builds them. The caller supplies the matrix, the eye
 * offset and the vertex buffer; the resident vertices carry the FINAL short U/V (tcs = 1). */
bool gdx3ds_mesh_nrr_material(GdxInterpSeam& g, GdxSeamMatRec* recs, int ways, int mode, const Gdx3dsVtxMathState& vs,
                              const Gdx3dsLoadedVtx& first, Gdx3dsXfDraw& xf, Gdx3dsVboPackAux& aux,
                              GdxSeamMatOutcome& o, GdxNrrMatInfo* info) {
    GdxSeamMaterial mat;
    memset(&o, 0, sizeof(o));
    const int rc = g.BindMaterialRec(recs, ways, mode, mat, o);
    if (rc < 0 ? !g.BindMaterial(mat) : rc != 1) {
        return false;
    }
    for (int j = 0; j < mat.numInputs && j < 7; j++) {
        if (mat.inputMap[0][j] == GDXSEAM_CCMUX_LOD_FRACTION || mat.inputMap[1][j] == GDXSEAM_CCMUX_LOD_FRACTION) {
            return false;
        }
    }
    Gdx3dsVboPackParams params;
    memset(&params, 0, sizeof(params));
    if (!gdx3ds_mesh_backend_xf_params(&params)) {
        return false;
    }
    PackCtx c;
    if (!PreparePackCtx(g, mat, params, &first, c)) {
        return false;
    }
    XfFillMesh(vs, xf);
    XfFillRun(c, mat, 0x8000, 0xFFFF, vs, xf);
    xf.tcs[0] = 1.0f; /* the resident rings hold the interpreter's final short U/V */
    xf.tcs[1] = 1.0f;
    BuildAux(g.R(), mat, first, aux);
    const int cullMode = CullModeOf(*g.R().geometry_mode);
    const bool invert = (*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0;
    xf.cullMode = cullMode;
    if (invert && cullMode == GDX3DS_CULL_FRONT) {
        xf.cullMode = GDX3DS_CULL_BACK;
    } else if (invert && cullMode == GDX3DS_CULL_BACK) {
        xf.cullMode = GDX3DS_CULL_FRONT;
    }
    xf.clsW = first.w;
    if (info != nullptr) {
        memset(info, 0, sizeof(*info));
        info->numInputs = aux.numInputs;
        memcpy(info->inputMap, mat.inputMap, sizeof(info->inputMap));
        info->ci = (int8_t)c.ci;
        info->ai = (int8_t)c.ai;
        info->colConst = (c.ci >= 0 && !c.colorIsShade) ? 1 : 0;
        info->alphaConst = (params.wantVtxAlpha && !params.fogFactorToAlpha && c.ai >= 0 && !c.alphaIsShade &&
                            !c.constAlphaIsShade)
                               ? 1
                               : 0;
        info->useAlpha = mat.use_alpha ? 1 : 0;
        info->useFog = mat.use_fog ? 1 : 0;
        info->useBlendColor = mat.use_blend_color ? 1 : 0;
        info->fogFactorConst = c.fogFactorConst ? 1 : 0;
        info->prg = mat.prg;
    }
    return cullMode != GDX3DS_CULL_BOTH;
}

/* ---- C API --------------------------------------------------------------------------- */

extern "C" int gdx3ds_mesh_speck_enabled(void) {
    Enabled(); /* latches the ini keys */
    return sSpeck;
}

int gdx3ds_mesh_speck_hook_seam(GdxInterpSeam& I, const void* listPtr, const void* packets,
                                Gdx3dsMeshResolveFn resolve, void* user) {
    Entry* e = LookupSpeckForDraw(listPtr, packets, resolve, user);
    Mesh* mesh = e != nullptr ? e->mesh : nullptr;
    if (mesh == nullptr) {
        return 0;
    }
    const int c5Prev = (&gdx3ds_c5cat_enter != nullptr) ? gdx3ds_c5cat_enter(6 /* MESH */) : -1;
    DrawMesh(I, mesh, e->slot, e->lod, (int)(e - sEntries));
    if (c5Prev >= 0) {
        gdx3ds_c5cat_leave(c5Prev);
    }
    sSpeckDraws++;
    return 1;
}

extern "C" uintptr_t gdx3ds_mesh_bridge_lookup(uint64_t dlHostPtr, Gdx3dsMeshResolveFn resolve, void* user) {
    const void* src = (sizeof(void*) == 4) ? (const void*)(uintptr_t)(dlHostPtr & 0xFFFFFFFFu)
                                           : (const void*)(uintptr_t)dlHostPtr;
    Entry* e = LookupForDraw(src, resolve, user);
    if (e == nullptr) {
        return 0;
    }
    return (uintptr_t)&sStubs[e - sEntries];
}

void gdx3ds_mesh_apply_tail(GdxInterpSeam& I, int entryIdx, const uint8_t* envRgb) {
    if (entryIdx < 0 || entryIdx >= kEntries) {
        return;
    }
    if (envRgb != nullptr) {
        const GdxSeamRgba rgba = { envRgb[0], envRgb[1], envRgb[2], 255 };
        I.SetEnvColor(rgba);
    }
    const Mesh* m = sEntries[entryIdx].mesh;
    if (m == nullptr || m->magic != kMeshMagic) {
        return;
    }
    for (unsigned r = 0; r < m->nRuns; r++) {
        ApplyRunState(I, m->runs[r]);
    }
}

int gdx3ds_mesh_dl_hook_seam(GdxInterpSeam& I, const void* listPtr, Gdx3dsMeshResolveFn resolve, void* user) {
    const uintptr_t p = (uintptr_t)listPtr;
    const uintptr_t base = (uintptr_t)sStubs;
    Entry* e = nullptr;
    Mesh* mesh = nullptr;
    if (p >= base && p < base + sizeof(sStubs) && ((p - base) % sizeof(Stub)) == 0) {
        /* The bridge's redirect (LUS path): the stub names its entry. */
        const Stub* stub = (const Stub*)listPtr;
        if (stub->magic != kStubMagic || stub->entry >= (uint32_t)kEntries) {
            return 0;
        }
        e = &sEntries[stub->entry];
        /* Load the pointer ONCE. The entry is repointed by whichever thread bakes (main,
         * under bridgemain=1), so a second read could see a different generation's mesh. */
        mesh = e->mesh;
        if (mesh == nullptr) {
            return 1; /* nothing to draw (cannot happen: the bridge only redirects baked entries) */
        }
    } else {
        /* No conversion pass (owned walker): the game's own registered list reaches G_DL and
         * bakes here with the caller's resolver. Without one (the LUS path, where the bridge
         * already redirected every drawable list to its stub) a non-stub target is never a
         * mesh: keep that path exactly as cheap as it was. */
        if (resolve == nullptr || !Enabled()) {
            return 0;
        }
        e = LookupForDraw(listPtr, resolve, user);
        if (e == nullptr) {
            return 0;
        }
        mesh = e->mesh; /* loaded once, as above */
        if (mesh == nullptr) {
            return 0;
        }
    }
    const int c5Prev = (&gdx3ds_c5cat_enter != nullptr) ? gdx3ds_c5cat_enter(6 /* MESH */) : -1;
    DrawMesh(I, mesh, e->slot, e->lod, (int)(e - sEntries));
    if (c5Prev >= 0) {
        gdx3ds_c5cat_leave(c5Prev);
    }
    return 1;
}

/* ---- [native] dress_retained (C5 Stage 3b): decoration bodies on the machine registry ------
 *
 * Inside the decoration scope (decomp p=0x6C marker, gdx3ds_dress.h) the owned walker hands every
 * G_DL target here before walking it. A decoration body (gates, signs, buildings: G_TEXTURE,
 * G_VTX, G_TRI1/2 and nothing else) is registered as a dress entry on first sight, widened into
 * the machine walker's 16-byte packet form and baked like a machine body; from then on it draws
 * through DrawMesh (gpuxf, the GPU-resident buffer under machines_retained, the material records
 * under machines_mattab, the CPU mesh path for a straddler) with the MP the walker latched from
 * the decoration's G_MTX. A list the walker cannot bake (the texture setup lists: loads, tiles)
 * is remembered as failed and walked by the interpreter as before.
 *
 * Exactness guard: a dress entry keeps a copy of its list bytes and an FNV of its vertex bytes;
 * both are re-checked before every draw, and any difference rebakes the entry (an asset reloaded
 * at the same address for another course). The machine receipts are unaffected: DrawMesh's
 * counters are moved into the dress window around each dress draw. */
namespace {

struct DressEntry {
    const void* src;     /* the walker's resolved list pointer (narrow packets) */
    uint8_t* bytes;      /* copy of the list as baked (nCmds * stride) */
    uint32_t nBytes;
    uint32_t vtxHash;    /* FNV-1a of every chunk's vertex bytes as baked */
    uint8_t failed;
    /* A body with no G_TEXTURE of its own loads its vertices under the texture scale in force
     * at the G_DL (Course_GadgetsDraw's gSPTexture): the bake takes that scale, and a draw under
     * any other scale rebakes. */
    uint8_t inheritsScale;
    uint16_t inhS, inhT;
};
DressEntry sDress[kDressEntries];
int sDressCount = 0;
/* [native] nrr_deco: bumped when an entry is baked again (its mesh, and so its resident buffer,
 * is replaced): records made from the old one are dropped (gdx3ds_mesh_dress_epoch). */
uint32_t sDressEpoch = 0;
uint32_t sDressMask = 0xFFFFFFFFu; /* [debug] dress_mask (hex): entries drawn natively (bisect) */
int sDressFog = 0; /* [native] dress_retained_fog: fogged decorations may take the GPU transform */
struct DressWin {
    unsigned draws, gpu, invis, walked, bakes, fails, rebakes, full, stale, mtHits, lookups;
    uint64_t ticks;
} sDressWin = {};
uint8_t sDressWide[GDX3DS_MESH_MAX_CMDS * 16];

uint32_t DressVtxHash(const Mesh* m) {
    uint32_t h = 2166136261u;
    for (unsigned ci = 0; ci < m->nChunks; ci++) {
        const uint8_t* p = (const uint8_t*)m->chunks[ci].src;
        const size_t n = (size_t)m->chunks[ci].count * 16u;
        for (size_t k = 0; k < n; k++) {
            h = (h ^ p[k]) * 16777619u;
        }
    }
    return h;
}

constexpr uint8_t kOpTexture = 0xD7u;
constexpr uint8_t kOpTri1 = 0x05u;
constexpr uint8_t kOpTri2 = 0x06u;

inline void DressReadCmd(const uint8_t* src, size_t i, int stride, int isBig, uint32_t* w0, uint32_t* w1) {
    memcpy(w0, src + i * (size_t)stride, 4);
    memcpy(w1, src + i * (size_t)stride + 4, 4);
    if (isBig) {
        *w0 = __builtin_bswap32(*w0);
        *w1 = __builtin_bswap32(*w1);
    }
}

/* Where the list sets its own texture state: -1 = never (it inherits the scale in force), 0 =
 * before its first triangle (self-contained), 1 = only after some triangle (not bakeable: the
 * first triangles would need the inherited scale and the rest the list's own). */
int DressTextureShape(const uint8_t* src, size_t listCmds, int stride, int isBig) {
    const size_t maxCmds = listCmds < (size_t)GDX3DS_MESH_MAX_CMDS ? listCmds : (size_t)GDX3DS_MESH_MAX_CMDS;
    bool sawTri = false;
    for (size_t i = 0; i < maxCmds; i++) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        DressReadCmd(src, i, stride, isBig, &w0, &w1);
        const uint8_t op = (uint8_t)(w0 >> 24);
        if (op == kOpTexture) {
            return sawTri ? 1 : 0;
        }
        if (op == kOpTri1 || op == kOpTri2) {
            sawTri = true;
        }
        if (op == kEndDl) {
            break;
        }
    }
    return -1;
}

/* The narrow list, widened into the machine walker's host packet layout (w0 LE @0, w1 u64 @8),
 * through its G_ENDDL, after an optional synthetic G_TEXTURE carrying the inherited scale.
 * Returns the byte length consumed from the narrow list, 0 = no G_ENDDL. */
uint32_t DressWiden(const uint8_t* src, size_t listCmds, int stride, int isBig, bool prefixScale, uint16_t s,
                    uint16_t t) {
    const size_t room = (size_t)GDX3DS_MESH_MAX_CMDS - (prefixScale ? 1u : 0u);
    const size_t maxCmds = listCmds < room ? listCmds : room;
    memset(sDressWide, 0, sizeof(sDressWide));
    uint8_t* d = sDressWide;
    if (prefixScale) {
        const uint32_t w0 = ((uint32_t)kOpTexture << 24) | (1u << 1);
        const uint32_t w1 = ((uint32_t)s << 16) | (uint32_t)t;
        memcpy(d, &w0, 4);
        memcpy(d + 8, &w1, 4);
        d += 16;
    }
    for (size_t i = 0; i < maxCmds; i++, d += 16) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        DressReadCmd(src, i, stride, isBig, &w0, &w1);
        memcpy(d, &w0, 4);
        memcpy(d + 8, &w1, 4);
        if ((w0 >> 24) == kEndDl) {
            return (uint32_t)((i + 1) * (size_t)stride);
        }
    }
    return 0;
}

int DressFind(const void* src) {
    for (int i = 0; i < sDressCount; i++) {
        if (sDress[i].src == src) {
            return i;
        }
    }
    return -1;
}

GDX_COLD int DressRegister(const void* src) {
    if (sDressCount >= kDressEntries) {
        sDressWin.full++;
        return -1;
    }
    const int i = sDressCount++;
    memset(&sDress[i], 0, sizeof(sDress[i]));
    sDress[i].src = src;
    Entry& e = sEntries[kEntries + i];
    memset(&e, 0, sizeof(e));
    e.src = src;
    e.slot = kDressSlot;
    e.lod = (uint8_t)i;
    return i;
}

/* (Re)bake dress entry i from the list as it is now, under the texture scale in force. */
GDX_COLD bool DressBake(int i, const uint8_t* list, size_t listCmds, int stride, int isBig, uint16_t liveS, uint16_t liveT,
               Gdx3dsMeshResolveFn resolve, void* user) {
    DressEntry& d = sDress[i];
    Entry& e = sEntries[kEntries + i];
    const int shape = DressTextureShape(list, listCmds, stride, isBig);
    if (shape > 0) {
        d.failed = 1;
        sDressWin.fails++;
        Log("[dress] not a mesh entry=%d list=%p: G_TEXTURE after a triangle", i, e.src);
        return false;
    }
    const bool inherits = shape < 0;
    const uint32_t nBytes = DressWiden(list, listCmds, stride, isBig, inherits, liveS, liveT);
    const bool had = e.mesh != nullptr;
    sDressEpoch += had ? 1u : 0u;
    e.mesh = had ? e.mesh : nullptr;
    e.failed = 0;
    e.gen = sGeneration.load(std::memory_order_acquire) + 1u; /* forces Bake to run */
    if (nBytes == 0 || !Bake(e, resolve, user, sDressWide) || e.mesh == nullptr) {
        d.failed = 1;
        sDressWin.fails++;
        Log("[dress] not a mesh entry=%d list=%p: %s at cmd %u op %02X", i, e.src,
            nBytes == 0 ? "no G_ENDDL" : gdx3ds_mesh_walk_strerror(sWalkScratch.status), sWalkScratch.errCmd,
            sWalkScratch.errOpcode);
        return false;
    }
    free(d.bytes);
    d.bytes = (uint8_t*)malloc(nBytes);
    if (d.bytes == nullptr) {
        d.failed = 1;
        return false;
    }
    memcpy(d.bytes, list, nBytes);
    d.nBytes = nBytes;
    d.vtxHash = DressVtxHash(e.mesh);
    d.failed = 0;
    d.inheritsScale = inherits ? 1 : 0;
    d.inhS = liveS;
    d.inhT = liveT;
    if (inherits) {
        /* The synthetic G_TEXTURE only seeded the chunks' scale: the runs inherit the live
         * texture state at the G_DL exactly as the list does (ApplyRunState leaves it alone). */
        for (unsigned r = 0; r < e.mesh->nRuns; r++) {
            e.mesh->runs[r].texSet = 0;
        }
    }
    sDressWin.bakes++;
    Log("[dress] bake entry=%d list=%p cmds=%u runs=%u chunks=%u verts=%u tris=%u", i, e.src,
        (unsigned)(nBytes / (uint32_t)stride), e.mesh->nRuns, e.mesh->nChunks, e.mesh->nVerts, e.mesh->nTris);
    sDressWin.rebakes += had ? 1u : 0u;
    return true;
}

/* The entry is still exactly what was baked: same list bytes, same vertex bytes, and (for a
 * body that inherits it) the same texture scale in force. */
bool DressCurrent(int i, const uint8_t* list, uint16_t liveS, uint16_t liveT) {
    const DressEntry& d = sDress[i];
    const Mesh* m = sEntries[kEntries + i].mesh;
    if (d.inheritsScale && (d.inhS != liveS || d.inhT != liveT)) {
        return false;
    }
    return m != nullptr && d.bytes != nullptr && memcmp(d.bytes, list, d.nBytes) == 0 && DressVtxHash(m) == d.vtxHash;
}

/* DrawMesh for a dress entry, with its counters moved out of the machine window. */
void DressDraw(GdxInterpSeam& I, int i) {
    const Stats winBefore = sWin;
    const Stats totBefore = sTot;
    const uint64_t t0 = svcGetSystemTick();
    Entry& e = sEntries[kEntries + i];
    sDressCpuIfFog = sDressFog == 0;
    DrawMesh(I, e.mesh, kDressSlot, i, kEntries + i);
    sDressCpuIfFog = false;
    sDressWin.ticks += svcGetSystemTick() - t0;
    sDressWin.draws++;
    sDressWin.gpu += sWin.xfMeshes - winBefore.xfMeshes;
    sDressWin.invis += sWin.xfInvisible - winBefore.xfInvisible;
    sDressWin.walked += sWin.xfWalk - winBefore.xfWalk;
    sDressWin.mtHits += sWin.mtOut[GDXSEAM_MM_HIT] - winBefore.mtOut[GDXSEAM_MM_HIT];
    sWin = winBefore;
    sTot = totBefore;
}

GDX_COLD void LogDress() {
    if (sDressWin.lookups == 0) {
        return;
    }
    int baked = 0;
    int failed = 0;
    for (int i = 0; i < sDressCount; i++) {
        baked += sEntries[kEntries + i].mesh != nullptr ? 1 : 0;
        failed += sDress[i].failed ? 1 : 0;
    }
    Log("[dress] frames=64 lookups=%u draws=%u gpu=%u invis=%u cpu=%u walkruns=%u mthit=%u | entries=%d baked=%d "
        "notmesh=%d bakes=%u rebakes=%u stale=%u fails=%u full=%u | us=%lu/f",
        sDressWin.lookups, sDressWin.draws, sDressWin.gpu, sDressWin.invis,
        sDressWin.draws - sDressWin.gpu - sDressWin.invis, sDressWin.walked, sDressWin.mtHits, sDressCount, baked,
        failed, sDressWin.bakes, sDressWin.rebakes, sDressWin.stale, sDressWin.fails, sDressWin.full,
        (unsigned long)(sDressWin.ticks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull));
    memset(&sDressWin, 0, sizeof(sDressWin));
}

} // namespace

namespace {
/* hotcold: the dress ini latch (first lookup only), out of line. */
GDX_COLD void DressLatch() {
    sDressMask = ParseHex(gdx3ds_config_get_string("debug", "dress_mask", "ffffffff"), 0xFFFFFFFFu);
    sDressFog = gdx3ds_config_get_int("native", "dress_retained_fog", 0) != 0 ? 1 : 0;
    Log("[dress] init dress_retained_fog=%d (%s) mask=%08lx", sDressFog,
        sDressFog ? "fogged decorations take the GPU transform: <=1 RGB5 quantum of fog" : "exact: fogged "
                                                                                         "decorations stay on "
                                                                                         "the CPU mesh path",
        (unsigned long)sDressMask);
}
} // namespace

int gdx3ds_mesh_dress_model_ok(const void* listPtr) {
    return (DressFind(listPtr) >= 0 || sDressCount < kDressEntries / 2) ? 1 : 0;
}

int gdx3ds_mesh_dress_hook_seam(GdxInterpSeam& I, const void* listPtr, size_t listCmds, int stride, int isBig,
                                Gdx3dsMeshResolveFn resolve, void* user) {
    if (listPtr == nullptr || resolve == nullptr || Enabled() != 1 || (stride != 8 && stride != 16)) {
        return 0;
    }
    if (stride != 8) {
        return 0; /* the decoration bodies are o2r asset lists (narrow packets) */
    }
    if (sDressWin.lookups == 0 && sDressCount == 0 && &gdx3ds_config_get_string != nullptr) {
        DressLatch();
    }
    sDressWin.lookups++;
    const uint8_t* list = (const uint8_t*)listPtr;
    const uint16_t liveS = *I.R().tex_scale_s;
    const uint16_t liveT = *I.R().tex_scale_t;
    int i = DressFind(listPtr);
    if (i < 0) {
        i = DressRegister(listPtr);
        if (i < 0) {
            return 0;
        }
        if (!DressBake(i, list, listCmds, stride, isBig, liveS, liveT, resolve, user)) {
            return 0;
        }
    } else if (sDress[i].failed) {
        return 0;
    } else if (((sDressMask >> (i & 31)) & 1u) == 0) {
        return 0; /* [debug] dress_mask: bisect lever (walk this entry) */
    } else if (!DressCurrent(i, list, liveS, liveT)) {
        sDressWin.stale++;
        if (!DressBake(i, list, listCmds, stride, isBig, liveS, liveT, resolve, user)) {
            return 0;
        }
    }
    DressDraw(I, i);
    return 1;
}

/* ---- n60finish [native] finish_mesh: the 3D FINISH banner as a retained GPU-transform mesh -----
 *
 * Menus_DrawFinish draws aFinishDL (six extruded letters, ~380 frames after every finish line) under
 * two G_MTX of its own: G_GEOMETRYMODE / G_TEXTURE, then eighteen texture-load groups (two textures,
 * the 8x8 side and the 16x16 face, alternating twelve times, then the face six more times), each
 * followed by its G_VTX / G_TRI2 block -- 287 commands, 256 vertices, 222 triangles, about half of
 * them back faces. Walked, that is 222 SpTri + 256 CPU T&Ls + 18 TMEM loads every frame
 * (n60lap-progress.md §2). The decomp writes a G_NOOP p=0x6A right before the G_DL when the lever
 * is on (decomp-native-finish-mesh.patch); the walker hands the next G_DL here.
 *
 * BAKE (first sight, and again whenever the list bytes or its address change): the narrow list is
 * cut into SEGMENTS at every texture-load group (and at any G_GEOMETRYMODE / G_TEXTURE after
 * geometry): a segment = its PRELUDE (the state commands before its first G_VTX, the load group
 * among them) + its triangles. The two distinct group word sequences are the two textures (0, 1).
 * Vertices (raw 16-byte Vtx, read in place) and mesh-global u16 indices go ONCE into a GPU-resident
 * linear buffer; one mesh run per segment. Anything but those opcodes is not bakeable (logged once;
 * the list stays on the walker).
 *
 * DRAW (per frame), in stock order:
 *  - the containment / float24 error-bound gates of the machines' gpuxf path on the G_MTX the walker
 *    latched (a banner reaching the screen edge or too close is walked: the CPU clipper is exact);
 *  - each segment's prelude runs through the walker's own handlers (Exec) -- EXCEPT a load group
 *    whose texture this frame already bound: those loads are the cost, and identical words
 *    rewrite identical tiles and TMEM;
 *  - the survivor screen (GfxSpTri1 binds no state for a segment whose every triangle is culled or
 *    rejected); then the first segment of each texture binds through ResolveDrawState (the stock
 *    import) and the binding is saved (BindingSave); every later segment of that texture re-applies
 *    it (BindingApply: the cache slot, sampler, shader -- exactly what the stock re-import's cache
 *    hit binds) with the run uniforms computed at that first bind;
 *  - one indexed GPU-transform draw per segment from the resident buffer. Per frame only the matrix
 *    and the uniforms change.
 *  - the state the list leaves: the registers end on its last load group (re-armed when the stock
 *    path's last group had no surviving triangle after it) and the vertex cache holds its last G_VTX.
 * Stereo: the draws go through the gpuxf DrawTriangles path, so each one takes its class row from
 * clsW like a machine run (SCENE: the banner's projection is perspective).
 * A refusal after the first prelude ran (a fogged / lit state, a bind the gpuxf path cannot
 * express) hands the REST of the list, from that segment's first G_VTX, to the walker: still the
 * stock result. */
namespace {

constexpr int kFinSegs = 40;
constexpr size_t kFinMaxCmds = 640;
constexpr unsigned kFinMaxVerts = 1024; /* == GDX3DS_MESH_MAX_VERTS: the survivor screen's scratch */
constexpr unsigned kFinMaxTris = 1024;
constexpr unsigned kFinMaxChunks = 64;
constexpr uint16_t kFinNone = 0xFFFFu;
/* The partition's screen-x gap: the GPU places a vertex within the gpuxf error bound (~0.02 px here)
 * and snaps it to 1/16 px; 1/1024 NDC is ~0.2 px of the 400-px axis, several times both together. */
constexpr float kFinPartMarginNdc = 1.0f / 1024.0f;
constexpr uint8_t kOpVtx = 0x01u;
constexpr uint8_t kOpGeometryMode = 0xD9u;
constexpr uint8_t kOpLoadSync = 0xE6u;
constexpr uint8_t kOpPipeSync = 0xE7u;
constexpr uint8_t kOpTileSync = 0xE8u;
constexpr uint8_t kOpSetTileSize = 0xF2u;
constexpr uint8_t kOpLoadBlock = 0xF3u;
constexpr uint8_t kOpSetTile = 0xF5u;
constexpr uint8_t kOpSetTimg = 0xFDu;

struct FinSeg {
    uint16_t preBegin, preEnd; /* the prelude: list commands [preBegin, preEnd) */
    uint16_t grpBegin, grpEnd; /* its load group (grpBegin == grpEnd: none) */
    /* the prelude's commands other than the group and the syncs, as indices into cmdIdx: before
     * the group [execBegin, execMid), after it [execMid, execEnd) */
    uint16_t execBegin, execMid, execEnd;
    uint8_t tex;               /* the texture (0 / 1) its triangles are drawn with */
};

struct FinEntry {
    const void* src;  /* the resolved list (narrow packets) */
    uint8_t* bytes;   /* copy of the list through its G_ENDDL, as baked */
    uint32_t nBytes;
    uint8_t failed;   /* not bakeable: walked */
    Mesh* mesh;
    uint8_t* res;     /* resident buffer: nVerts raw Vtx, then nTris * 3 u16 */
    uint32_t resBytes;
    XfBox box;
    FinSeg seg[kFinSegs];
    int nSeg;
    uint16_t grpBegin[2], grpEnd[2]; /* each texture's first load group */
    uint8_t lastTex;                 /* the texture of the list's last load group */
    uint16_t lastVtx;                /* the list's last G_VTX (kFinNone: none) */
    uint16_t endCmd;                 /* its G_ENDDL */
    uint8_t oneColor;                /* every vertex carries the same colour bytes (FinDraw FAST) */
    uint16_t cmdIdx[kFinMaxCmds];    /* the preludes' executed commands (FinSeg exec ranges) */
    uint16_t nCmdIdx;
    /* PARTITIONED draw (FinDrawParted): the triangles re-sorted by texture into a second resident
     * index buffer -- every segment of texture partA in list order, then every segment of the
     * other -- so the banner is two draws. Valid only where no swapped pair overlaps on screen. */
    uint8_t partOk;    /* the preludes after the first geometry are redundant; two textures */
    uint8_t partA;     /* the texture drawn first (segment 0's) */
    uint16_t* res2;    /* linear: nTris * 3 u16, partitioned */
    uint32_t partSplit; /* triangles of partA */
    uint32_t mesh2Bytes;
    uint8_t nPairs;
    uint8_t pairA[64], pairB[64]; /* swapped pairs (earlier partB segment, later partA segment) */
    float segLo[kFinSegs][3], segHi[kFinSegs][3]; /* object-space box of each segment's vertices */
};
FinEntry sFin = {};

/* Per texture, this frame: the first bind's material and the run-uniform inputs computed from the
 * registers right after it (the tile UV parameters are read live, and the registers move on). */
struct FinTex {
    bool valid;
    GdxSeamMaterial mat;
    Gdx3dsVboPackParams params;
    PackCtx c;
};
FinTex sFinTex[2];
int sFinDiagLogs = 0; /* [debug] finish_prof: overlap details logged */
float sFinZc = -1.0f; /* the clip z scale the gpuxf shader applies (0.5 / 1), < 0 unknown (FinPartDisjoint) */

int sFinLever = -1; /* [native] finish_mesh: 0 off (default), 1 on */
struct FinWin {
    unsigned frames, draws, segs, empty, binds, applies, applyMiss, groupsRun, groupsSkipped, rearm, merged, fast;
    unsigned parted, partOverlap; /* frames drawn partitioned / refused it (a swapped pair may overlap) */
    unsigned fbOut, fbClip, fbErr, fbVs, fbBind, fbStale, tails, bakes, fails;
    unsigned maxErrMpx;
    uint64_t ticks;
    uint64_t ph[5]; /* [debug] finish_prof: prelude exec / screen / bind+apply / emit / tail */
    uint64_t bx[6]; /* [debug] finish_prof: the backend's xfprof sections of the banner's draws */
} sFinWin = {};
int sFinProf = 0; /* [debug] finish_prof: the per-phase clock of the receipt (svcGetSystemTick per phase) */
uint64_t sFinT = 0;
inline void FinPh(int ph) {
    if (GDX_UNLIKELY(sFinProf)) {
        const uint64_t t = svcGetSystemTick();
        sFinWin.ph[ph] += t - sFinT;
        sFinT = t;
    }
}

GDX_COLD void FinLatch() {
    sFinLever = gdx3ds_config_get_int("native", "finish_mesh", 0) == 1 ? 1 : 0;
    sFinProf = gdx3ds_config_get_int("debug", "finish_prof", 0) != 0 ? 1 : 0;
    const int on = sFinLever && sGpuXf;
    Log("[finish] init finish_mesh=%d gpuxf=%d -> %s", sFinLever, sGpuXf,
        on ? "the FINISH banner draws as a retained GPU-transform mesh" : "off (the banner is walked)");
    sFinLever = on;
}

bool FinOn() {
    if (GDX_LIKELY(sFinLever >= 0)) {
        return sFinLever == 1;
    }
    Enabled(); /* latches gpuxf with the rest of the mesh keys */
    if (sEnabled < 0 || &gdx3ds_config_get_int == nullptr) {
        return false;
    }
    FinLatch();
    return sFinLever == 1;
}

inline bool FinIsGroupOp(uint8_t op) {
    return op == kOpSetTimg || op == kOpSetTile || op == kOpLoadBlock || op == kOpSetTileSize;
}

inline bool FinIsSyncOp(uint8_t op) {
    return op == kOpLoadSync || op == kOpPipeSync || op == kOpTileSync;
}

/* The list words as they are now still equal the baked copy (word-wise: ~600 words). */
bool FinListCurrent(const uint8_t* list) {
    if (sFin.bytes == nullptr) {
        return false;
    }
    const uint32_t* a = (const uint32_t*)sFin.bytes;
    uint32_t w = 0;
    for (uint32_t i = 0; i < sFin.nBytes / 4u; i++) {
        uint32_t b;
        memcpy(&b, list + (size_t)i * 4u, 4);
        w |= a[i] ^ b;
    }
    return w == 0;
}

void FinRelease() {
    if (sFin.mesh != nullptr) {
        RetireMesh(sFin.mesh);
        sFin.mesh = nullptr;
    }
    if (sFin.res != nullptr) {
        ResRetire(sFin.res, sFin.resBytes);
        sFin.res = nullptr;
        sFin.resBytes = 0;
    }
    if (sFin.res2 != nullptr) {
        ResRetire((uint8_t*)sFin.res2, sFin.mesh2Bytes);
        sFin.res2 = nullptr;
    }
    sFin.partOk = 0;
    free(sFin.bytes);
    sFin.bytes = nullptr;
    sFin.nBytes = 0;
}

/* The bake's working set (drawing thread only). */
struct FinBuild {
    Gdx3dsMeshChunk chunks[kFinMaxChunks];
    uint16_t tris[kFinMaxTris * 3];
    Gdx3dsMeshRun runs[kFinSegs];
    unsigned nChunks, nTris, nVerts;
    uint8_t slotChunk[GDX3DS_MESH_SLOTS];
    uint8_t slotLocal[GDX3DS_MESH_SLOTS];
};
FinBuild sFinB;

GDX_COLD bool FinFail(const char* why, unsigned cmd, unsigned op) {
    sFin.failed = 1;
    sFinWin.fails++;
    Log("[finish] not a retained mesh list=%p: %s at cmd %u op %02X (walked)", sFin.src, why, cmd, op);
    return false;
}

GDX_COLD bool FinAddTri(unsigned a, unsigned b, unsigned c, unsigned cmd, unsigned op) {
    const unsigned v[3] = { a, b, c };
    if (sFinB.nTris >= kFinMaxTris) {
        return FinFail("triangle capacity", cmd, op);
    }
    for (int k = 0; k < 3; k++) {
        if (v[k] >= GDX3DS_MESH_SLOTS || sFinB.slotChunk[v[k]] == 0xFF) {
            return FinFail("unloaded vertex slot", cmd, op);
        }
        const Gdx3dsMeshChunk& ch = sFinB.chunks[sFinB.slotChunk[v[k]]];
        sFinB.tris[sFinB.nTris * 3 + k] = (uint16_t)(ch.baseVertex + sFinB.slotLocal[v[k]]);
    }
    sFinB.nTris++;
    return true;
}

GDX_COLD bool FinAddVtx(uint32_t w0, uint32_t w1, uint16_t texS, uint16_t texT, unsigned cmd,
                        Gdx3dsMeshResolveFn resolve, void* user) {
    const unsigned n = (w0 >> 12) & 0xFFu;
    const unsigned end = (w0 >> 1) & 0x7Fu;
    if (n == 0 || end < n || end > GDX3DS_MESH_SLOTS) {
        return FinFail("G_VTX slot range", cmd, kOpVtx);
    }
    const unsigned v0 = end - n;
    if (sFinB.nChunks >= kFinMaxChunks || sFinB.nVerts + n > kFinMaxVerts) {
        return FinFail("vertex capacity", cmd, kOpVtx);
    }
    const void* src = resolve((uint64_t)w1, (size_t)n * 16u, user);
    if (src == nullptr) {
        return FinFail("vertex pointer refused", cmd, kOpVtx);
    }
    Gdx3dsMeshChunk& ch = sFinB.chunks[sFinB.nChunks];
    memset(&ch, 0, sizeof(ch));
    ch.srcW1 = w1;
    ch.src = src;
    ch.count = (uint16_t)n;
    ch.baseVertex = (uint16_t)sFinB.nVerts;
    ch.texS = texS;
    ch.texT = texT;
    for (unsigned k = 0; k < n; k++) {
        sFinB.slotChunk[v0 + k] = (uint8_t)sFinB.nChunks;
        sFinB.slotLocal[v0 + k] = (uint8_t)k;
    }
    sFinB.nChunks++;
    sFinB.nVerts += n;
    return true;
}

/* Which texture a load group is: 0 / 1 by its word sequence (a third distinct one is refused). */
GDX_COLD int FinGroupTex(const uint8_t* list, uint16_t b, uint16_t e) {
    for (int t = 0; t < 2; t++) {
        if (sFin.grpBegin[t] == kFinNone) {
            sFin.grpBegin[t] = b;
            sFin.grpEnd[t] = e;
            return t;
        }
        const uint16_t tb = sFin.grpBegin[t];
        const uint16_t te = sFin.grpEnd[t];
        if (te - tb == e - b && memcmp(list + (size_t)tb * 8u, list + (size_t)b * 8u, (size_t)(e - b) * 8u) == 0) {
            return t;
        }
    }
    return -1;
}

/* Close the open segment into a mesh run. */
void FinCloseSeg(int k, unsigned firstTri) {
    Gdx3dsMeshRun& r = sFinB.runs[k];
    memset(&r, 0, sizeof(r));
    r.firstTri = (uint16_t)firstTri;
    r.triCount = (uint16_t)(sFinB.nTris - firstTri);
    r.primKind = GDX3DS_MESH_COLOR_INHERIT;
    r.envKind = GDX3DS_MESH_COLOR_INHERIT;
}

/* Walk the narrow list into segments, chunks and triangles (sFin + sFinB). */
GDX_COLD bool FinScan(const uint8_t* list, size_t nCmds, Gdx3dsMeshResolveFn resolve, void* user) {
    memset(&sFinB, 0, sizeof(sFinB));
    memset(sFinB.slotChunk, 0xFF, sizeof(sFinB.slotChunk));
    sFin.grpBegin[0] = sFin.grpBegin[1] = kFinNone;
    sFin.lastVtx = kFinNone;
    sFin.nCmdIdx = 0;
    sFin.nSeg = 1;
    FinSeg* s = &sFin.seg[0];
    memset(s, 0, sizeof(*s));
    s->tex = 0xFF;
    s->preEnd = kFinNone;
    unsigned segFirstTri = 0;
    bool inGroup = false;
    uint16_t texS = 0;
    uint16_t texT = 0;
    int curTex = -1;
    for (size_t i = 0; i < nCmds; i++) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        DressReadCmd(list, i, 8, 0, &w0, &w1);
        const uint8_t op = (uint8_t)(w0 >> 24);
        const bool geom = op == kOpVtx || op == kOpTri1 || op == kOpTri2;
        const bool hasGeom = s->preEnd != kFinNone;
        const bool newSeg = (FinIsGroupOp(op) && !inGroup && (hasGeom || s->grpBegin != s->grpEnd)) ||
                            ((op == kOpGeometryMode || op == kOpTexture) && hasGeom);
        if (newSeg) {
            if (sFin.nSeg >= kFinSegs) {
                return FinFail("segment capacity", (unsigned)i, op);
            }
            if (!hasGeom) {
                s->preEnd = (uint16_t)i;
            }
            FinCloseSeg(sFin.nSeg - 1, segFirstTri);
            segFirstTri = sFinB.nTris;
            if (s->grpBegin == s->grpEnd) {
                s->execMid = sFin.nCmdIdx;
            }
            s->execEnd = sFin.nCmdIdx;
            s = &sFin.seg[sFin.nSeg++];
            memset(s, 0, sizeof(*s));
            s->preBegin = (uint16_t)i;
            s->preEnd = kFinNone;
            s->execBegin = sFin.nCmdIdx;
            s->tex = curTex < 0 ? 0xFF : (uint8_t)curTex;
        }
        if (FinIsGroupOp(op)) {
            if (!inGroup) {
                s->grpBegin = (uint16_t)i;
                s->execMid = sFin.nCmdIdx;
                inGroup = true;
            }
            s->grpEnd = (uint16_t)(i + 1);
            continue;
        }
        if (FinIsSyncOp(op)) {
            continue; /* no-ops on this port; inside a group they are part of its words */
        }
        if (inGroup) {
            inGroup = false;
            const int t = FinGroupTex(list, s->grpBegin, s->grpEnd);
            if (t < 0) {
                return FinFail("a third texture-load group", s->grpBegin, kOpSetTimg);
            }
            curTex = t;
            s->tex = (uint8_t)t;
            sFin.lastTex = (uint8_t)t;
        }
        if (geom && s->preEnd == kFinNone) {
            s->preEnd = (uint16_t)i;
            if (s->tex == 0xFF) {
                return FinFail("geometry before any load group", (unsigned)i, op);
            }
        }
        if ((op == kOpGeometryMode || op == kOpTexture) && sFin.nCmdIdx < kFinMaxCmds) {
            sFin.cmdIdx[sFin.nCmdIdx++] = (uint16_t)i; /* a prelude state command (executed per frame) */
        }
        switch (op) {
            case kOpGeometryMode:
                break;
            case kOpTexture:
                texS = (uint16_t)(w1 >> 16);
                texT = (uint16_t)w1;
                break;
            case kOpVtx:
                if (!FinAddVtx(w0, w1, texS, texT, (unsigned)i, resolve, user)) {
                    return false;
                }
                sFin.lastVtx = (uint16_t)i;
                break;
            case kOpTri1:
                if (!FinAddTri(((w0 >> 16) & 0xFFu) / 2, ((w0 >> 8) & 0xFFu) / 2, (w0 & 0xFFu) / 2, (unsigned)i, op)) {
                    return false;
                }
                break;
            case kOpTri2:
                if (!FinAddTri(((w0 >> 16) & 0xFFu) / 2, ((w0 >> 8) & 0xFFu) / 2, (w0 & 0xFFu) / 2, (unsigned)i, op) ||
                    !FinAddTri(((w1 >> 16) & 0xFFu) / 2, ((w1 >> 8) & 0xFFu) / 2, (w1 & 0xFFu) / 2, (unsigned)i, op)) {
                    return false;
                }
                break;
            case kEndDl:
                if (s->preEnd == kFinNone) {
                    s->preEnd = (uint16_t)i; /* trailing state: a segment with no triangles */
                }
                FinCloseSeg(sFin.nSeg - 1, segFirstTri);
                if (s->grpBegin == s->grpEnd) {
                    s->execMid = sFin.nCmdIdx;
                }
                s->execEnd = sFin.nCmdIdx;
                sFin.endCmd = (uint16_t)i;
                return true;
            default:
                return FinFail("unexpected opcode", (unsigned)i, op);
        }
    }
    return FinFail("no G_ENDDL", (unsigned)nCmds, 0);
}

/* The two load groups land at one TMEM base and the last one covers the other: the TMEM a stock walk
 * leaves (every group in order) equals what running each texture's first group once leaves. */
GDX_COLD bool FinTmemOrderFree(const uint8_t* list) {
    if (sFin.grpBegin[1] == kFinNone) {
        return true; /* one texture */
    }
    uint32_t base[2] = { 0, 0 };
    uint32_t bytes[2] = { 0, 0 };
    for (int t = 0; t < 2; t++) {
        uint32_t loadTile = 8;
        uint32_t texels = 0;
        uint32_t siz = 0;
        for (uint16_t i = sFin.grpBegin[t]; i < sFin.grpEnd[t]; i++) {
            uint32_t w0 = 0;
            uint32_t w1 = 0;
            DressReadCmd(list, i, 8, 0, &w0, &w1);
            if ((w0 >> 24) == kOpLoadBlock) {
                loadTile = (w1 >> 24) & 7u;
                texels = ((w1 >> 12) & 0xFFFu) + 1u;
            }
        }
        for (uint16_t i = sFin.grpBegin[t]; i < sFin.grpEnd[t]; i++) {
            uint32_t w0 = 0;
            uint32_t w1 = 0;
            DressReadCmd(list, i, 8, 0, &w0, &w1);
            if ((w0 >> 24) == kOpSetTile && ((w1 >> 24) & 7u) == loadTile) {
                base[t] = w0 & 0x1FFu;
                siz = (w0 >> 19) & 3u;
            }
        }
        if (loadTile == 8 || texels == 0) {
            return false;
        }
        bytes[t] = siz == 0 ? (texels + 1u) / 2u : texels << (siz - 1u);
    }
    const int last = sFin.lastTex;
    return base[0] == base[1] && bytes[last] >= bytes[1 - last];
}

/* The partitioned draw's static half. Its premise: every G_GEOMETRYMODE / G_TEXTURE after the
 * list's first geometry repeats the latest command of its opcode word for word (a repeated
 * gSPGeometryMode / gSPTexture writes what is already there), so running all the preludes' state
 * commands up front leaves the registers every segment's triangles see exactly as the walk does. */
GDX_COLD void FinPartBake(const uint8_t* list) {
    sFin.partOk = 0;
    sFin.nPairs = 0;
    const Mesh* m = sFin.mesh;
    if (sFin.grpBegin[1] == kFinNone || !sFin.oneColor) {
        return;
    }
    /* G_GEOMETRYMODE: the bits each command clears / sets are tracked (known value per bit); one
     * after the first geometry must clear only known-0 bits and set only known-1 bits. G_TEXTURE:
     * the same words as the latest G_TEXTURE. */
    bool seenGeom = false;
    uint32_t knownMask = 0;
    uint32_t knownVal = 0;
    uint32_t lastTex[2] = { 0, 0 };
    bool haveTex = false;
    for (uint16_t i = 0; i < sFin.endCmd; i++) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        DressReadCmd(list, i, 8, 0, &w0, &w1);
        const uint8_t op = (uint8_t)(w0 >> 24);
        if (op == kOpVtx || op == kOpTri1 || op == kOpTri2) {
            seenGeom = true;
            continue;
        }
        bool noop = true;
        if (op == kOpGeometryMode) {
            const uint32_t clr = ~(w0 & 0x00FFFFFFu) & ~w1; /* SpGeometryMode(~w0[0:24], w1) */
            const uint32_t set = w1;
            noop = (clr & ~knownMask) == 0 && (clr & knownVal) == 0 && (set & ~knownMask) == 0 &&
                   (set & ~knownVal) == 0;
            knownMask |= clr | set;
            knownVal = (knownVal & ~clr) | set;
        } else if (op == kOpTexture) {
            noop = haveTex && lastTex[0] == w0 && lastTex[1] == w1;
            lastTex[0] = w0;
            lastTex[1] = w1;
            haveTex = true;
        } else {
            continue;
        }
        if (seenGeom && !noop) {
            Log("[finish] partitioned draw off: cmd %u op %02X changes state after the first triangle", i, op);
            return;
        }
    }
    const uint8_t a = sFin.seg[0].tex;
    for (int k = 1; k < sFin.nSeg; k++) {
        if (m->runs[k].triCount != 0 && (m->xfRunTex[k * 2] != m->xfRunTex[0] || m->xfRunTex[k * 2 + 1] != m->xfRunTex[1])) {
            Log("[finish] partitioned draw off: segment %d loads its vertices under another texture scale", k);
            return;
        }
    }
    uint16_t* idx = (uint16_t*)linearAlloc((size_t)m->nTris * 3u * sizeof(uint16_t));
    if (idx == nullptr) {
        return;
    }
    unsigned n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < sFin.nSeg; k++) {
            if ((sFin.seg[k].tex == a) != (pass == 0)) {
                continue;
            }
            const Gdx3dsMeshRun& r = m->runs[k];
            memcpy(idx + (size_t)n * 3u, m->tris + (size_t)r.firstTri * 3u, (size_t)r.triCount * 3u * sizeof(uint16_t));
            n += r.triCount;
        }
        if (pass == 0) {
            sFin.partSplit = n;
        }
    }
    sFin.mesh2Bytes = (uint32_t)m->nTris * 3u * (uint32_t)sizeof(uint16_t);
    gdx3ds_mesh_backend_flush_resident(idx, sFin.mesh2Bytes);
    sResBytes += sFin.mesh2Bytes;
    sResCount++;
    sFin.res2 = idx;
    for (int k = 0; k < sFin.nSeg; k++) {
        const Gdx3dsMeshRun& r = m->runs[k];
        float lo[3] = { 1e30f, 1e30f, 1e30f };
        float hi[3] = { -1e30f, -1e30f, -1e30f };
        for (unsigned j = 0; j < (unsigned)r.triCount * 3u; j++) {
            const uint16_t gi = m->tris[(size_t)r.firstTri * 3u + j];
            const Gdx3dsMeshChunk& ch = m->chunks[m->vtxChunk[gi]];
            const Gdx3dsVtxIn* v = (const Gdx3dsVtxIn*)((const uint8_t*)ch.src + (size_t)(gi - ch.baseVertex) * 16u);
            for (int c = 0; c < 3; c++) {
                lo[c] = (float)v->ob[c] < lo[c] ? (float)v->ob[c] : lo[c];
                hi[c] = (float)v->ob[c] > hi[c] ? (float)v->ob[c] : hi[c];
            }
        }
        memcpy(sFin.segLo[k], lo, sizeof(lo));
        memcpy(sFin.segHi[k], hi, sizeof(hi));
    }
    /* The pairs the partition swaps: an earlier segment of the second texture, a later of the first. */
    for (int i = 0; i < sFin.nSeg; i++) {
        for (int j = i + 1; j < sFin.nSeg; j++) {
            if (sFin.seg[i].tex != a && sFin.seg[j].tex == a && m->runs[i].triCount != 0 && m->runs[j].triCount != 0) {
                if (sFin.nPairs >= 64) {
                    return; /* partOk stays 0 */
                }
                sFin.pairA[sFin.nPairs] = (uint8_t)i;
                sFin.pairB[sFin.nPairs] = (uint8_t)j;
                sFin.nPairs++;
            }
        }
    }
    sFin.partA = a;
    sFin.partOk = 1;
}

GDX_COLD bool FinBake(const uint8_t* list, size_t listCmds, Gdx3dsMeshResolveFn resolve, void* user) {
    FinRelease();
    sFin.failed = 0;
    sFinWin.bakes++;
    const size_t maxCmds = listCmds < kFinMaxCmds ? listCmds : kFinMaxCmds;
    if (!FinScan(list, maxCmds, resolve, user)) {
        return false;
    }
    if (!FinTmemOrderFree(list)) {
        return FinFail("the two loads do not share one TMEM span", sFin.grpBegin[1], kOpLoadBlock);
    }
    if (sFinB.nTris == 0) {
        return FinFail("no triangles", sFin.endCmd, kEndDl);
    }
    sFin.nBytes = (uint32_t)(sFin.endCmd + 1u) * 8u;
    sFin.bytes = (uint8_t*)malloc(sFin.nBytes);
    Mesh* m = (Mesh*)calloc(1, sizeof(Mesh));
    if (sFin.bytes == nullptr || m == nullptr) {
        free(m);
        return FinFail("no heap", 0, 0);
    }
    memcpy(sFin.bytes, list, sFin.nBytes);
    m->magic = kMeshMagic;
    m->nRuns = (uint16_t)sFin.nSeg;
    m->nChunks = (uint16_t)sFinB.nChunks;
    m->nTris = (uint16_t)sFinB.nTris;
    m->nVerts = (uint16_t)sFinB.nVerts;
    m->runs = DupArray(sFinB.runs, sFin.nSeg);
    m->chunks = DupArray(sFinB.chunks, sFinB.nChunks);
    m->tris = DupArray(sFinB.tris, (size_t)sFinB.nTris * 3);
    m->vtxChunk = (uint8_t*)malloc(sFinB.nVerts);
    m->xfRunTex = (uint16_t*)malloc(sizeof(uint16_t) * 2u * (unsigned)sFin.nSeg);
    if (m->runs == nullptr || m->chunks == nullptr || m->tris == nullptr || m->vtxChunk == nullptr ||
        m->xfRunTex == nullptr) {
        FreeMesh(m);
        return FinFail("no heap", 0, 0);
    }
    for (unsigned c = 0; c < sFinB.nChunks; c++) {
        for (unsigned k = 0; k < sFinB.chunks[c].count; k++) {
            m->vtxChunk[sFinB.chunks[c].baseVertex + k] = (uint8_t)c;
        }
    }
    m->xfScaleOk = XfComputeRunScales(m);
    if (!m->xfScaleOk) {
        FreeMesh(m);
        return FinFail("a run mixes texture scales", 0, kOpTexture);
    }
    sFin.res = ResBuild(m);
    if (sFin.res == nullptr) {
        FreeMesh(m);
        return FinFail("no linear memory", 0, 0);
    }
    sFin.resBytes = ResBytesFor(m);
    sResBytes += sFin.resBytes;
    sResCount++;
    m->res = sFin.res;
    m->resState = 1;
    XfMeshBox(m, sFin.box);
    sFin.oneColor = 1;
    for (unsigned c = 0; c < sFinB.nChunks && sFin.oneColor; c++) {
        const uint8_t* v = (const uint8_t*)sFinB.chunks[c].src;
        for (unsigned k = 0; k < sFinB.chunks[c].count; k++) {
            if (memcmp(v + (size_t)k * 16u + 12u, (const uint8_t*)sFinB.chunks[0].src + 12u, 4) != 0) {
                sFin.oneColor = 0;
                break;
            }
        }
    }
    sFin.mesh = m;
    FinPartBake(list);
    Log("[finish] bake list=%p cmds=%u segs=%d chunks=%u verts=%u tris=%u tex=%s groups=%u/%u..%u/%u last=%u "
        "res=%luB oneColor=%u parted=%u pairs=%u",
        sFin.src, (unsigned)sFin.endCmd + 1u, sFin.nSeg, sFinB.nChunks, sFinB.nVerts, sFinB.nTris,
        sFin.grpBegin[1] == kFinNone ? "1" : "2", sFin.grpBegin[0], sFin.grpEnd[0], sFin.grpBegin[1],
        sFin.grpEnd[1], sFin.lastTex, (unsigned long)sFin.resBytes, sFin.oneColor, sFin.partOk, sFin.nPairs);
    return true;
}

/* The stock remainder: list commands [from, G_ENDDL) through the walker's handlers. */
void FinStockTail(GdxInterpSeam& g, uint16_t from) {
    sFinWin.tails++;
    g.ExecHookCmds(from, sFin.endCmd);
}

void FinRunGroup(GdxInterpSeam& g, int t) {
    g.ExecHookCmds(sFin.grpBegin[t], sFin.grpEnd[t]);
    sFinWin.groupsRun++;
}

/* First bind of texture t this frame (the registers hold its load group, armed): the stock
 * ResolveDrawState, the binding saved, the run-uniform inputs computed while the tiles are live. */
bool FinBindFirst(GdxInterpSeam& g, int t, const Gdx3dsLoadedVtx& first) {
    FinTex& tc = sFinTex[t];
    tc.valid = false;
    memset(&tc.mat, 0, sizeof(tc.mat));
    if (!g.BindMaterial(tc.mat)) {
        return false;
    }
    for (int j = 0; j < tc.mat.numInputs && j < 7; j++) {
        if (tc.mat.inputMap[0][j] == GDXSEAM_CCMUX_LOD_FRACTION || tc.mat.inputMap[1][j] == GDXSEAM_CCMUX_LOD_FRACTION) {
            return false;
        }
    }
    memset(&tc.params, 0, sizeof(tc.params));
    if (!gdx3ds_mesh_backend_xf_params(&tc.params)) {
        return false;
    }
    if (!PreparePackCtx(g, tc.mat, tc.params, &first, tc.c)) {
        return false;
    }
    tc.valid = g.BindingSave(t) != 0;
    sFinZc = tc.mat.clip.zIsFrom0To1 ? 0.5f : 1.0f; /* FinPartDisjoint's eye skew (shader conv.z) */
    return true;
}

struct FinVs {
    Gdx3dsVtxMathState vs;
    Gdx3dsXfDraw xf;
    uint32_t geo;
    bool live;
    int cullMode, invert;
};

/* (Re)latch the vertex state after a prelude moved the geometry mode; false = a state the
 * retained draw does not take (fog: the GPU's fog factor may differ by one RGB5 quantum). */
bool FinLatchVs(GdxInterpSeam& g, FinVs& v) {
    if (!g.LatchVtxState(v.vs) || v.vs.fog || !XfMeshEligible(sFin.mesh, v.vs)) {
        return false;
    }
    if (gdx3ds_replay_rec) {
        gdx3ds_replay_set_mesh_xscale(ReplayXscaleOf(v.vs));
        gdx3ds_replay_note_mesh_mp(&v.vs.mp[0][0]);
    }
    XfFillMesh(v.vs, v.xf);
    v.xf.verts = sFin.res;
    v.xf.nVerts = sFin.mesh->nVerts;
    v.xf.idxResident = 1;
    /* machines_mattab's backend elision: GPU state identical to what the backend itself last issued
     * (this resident buffer, a unit's same texture, the alpha test) is not re-issued. */
    v.xf.stateElide = 1;
    v.geo = *g.R().geometry_mode;
    v.cullMode = CullModeOf(v.geo);
    v.invert = (*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0 ? 1 : 0;
    v.live = true;
    sXfEpoch++;
    return true;
}

/* One pending indexed draw: consecutive segments of one texture under one vertex state are one draw
 * (their triangles are contiguous in the resident index buffer and nothing between them changes a
 * register the draw reads). */
struct FinPend {
    bool open;
    int k0, t;
    const uint16_t* idxBase; /* the index buffer triBegin counts in (NULL: the mesh's own) */
    unsigned triBegin, triCount;
    Gdx3dsLoadedVtx first; /* the aux / stereo-class vertex */
};

void FinSubmit(GdxInterpSeam& g, FinPend& p, FinVs& v) {
    if (!p.open) {
        return;
    }
    p.open = false;
    const Mesh* m = sFin.mesh;
    const FinTex& tc = sFinTex[p.t];
    XfFillRun(tc.c, tc.mat, m->xfRunTex[p.k0 * 2 + 0], m->xfRunTex[p.k0 * 2 + 1], v.vs, v.xf);
    Gdx3dsVboPackAux aux;
    BuildAux(g.R(), tc.mat, p.first, aux);
    v.xf.cullMode = v.cullMode;
    if (v.invert && v.cullMode == GDX3DS_CULL_FRONT) {
        v.xf.cullMode = GDX3DS_CULL_BACK;
    } else if (v.invert && v.cullMode == GDX3DS_CULL_BACK) {
        v.xf.cullMode = GDX3DS_CULL_FRONT;
    }
    v.xf.clsW = p.first.w;
    g.ApplyDrawConstants();
    const uint16_t* base = p.idxBase != nullptr ? p.idxBase : (const uint16_t*)(sFin.res + ResIdxOffset(m));
    const uint16_t* idx = base + (size_t)p.triBegin * 3u;
    if (GDX_UNLIKELY(sFinProf)) {
        uint64_t before[6];
        for (int i = 0; i < 6; i++) {
            before[i] = gdx3ds_xfprof_ticks[XFP_B_TEX + i];
        }
        const int was = gdx3ds_xfprof_on;
        gdx3ds_xfprof_on = 1;
        gdx3ds_xfprof_inrun = 1;
        gdx3ds_xfprof_last = svcGetSystemTick();
        gdx3ds_mesh_backend_xf_draw(idx, p.triCount * 3u, &aux, &v.xf);
        gdx3ds_xfprof_mark(XFP_B_TAIL, svcGetSystemTick());
        gdx3ds_xfprof_inrun = 0;
        gdx3ds_xfprof_on = was;
        for (int i = 0; i < 6; i++) {
            sFinWin.bx[i] += gdx3ds_xfprof_ticks[XFP_B_TEX + i] - before[i];
        }
    } else {
        gdx3ds_mesh_backend_xf_draw(idx, p.triCount * 3u, &aux, &v.xf);
    }
    sFinWin.segs++;
}

/* A segment's prelude minus its load group (the syncs, no-ops here, are left out at bake). */
inline void FinExecPre(GdxInterpSeam& g, uint16_t b, uint16_t e) {
    if (e > b) {
        g.ExecHookCmdList(&sFin.cmdIdx[b], (size_t)(e - b));
    }
}

/* What the list leaves behind: the registers on its last load group with the imports armed (a stock
 * walk leaves them armed exactly when its last group had no surviving triangle after it; armed
 * either way, the next resolve imports the same TMEM -- the same cache entry -- that the unarmed
 * stock state has bound), and the vertex cache on its last G_VTX, transformed under the same state
 * as the stock walk's. */
void FinLeave(GdxInterpSeam& g, int regsTex, bool regsArmed) {
    if (regsTex != sFin.lastTex) {
        FinRunGroup(g, sFin.lastTex);
        regsArmed = true;
    }
    if (!regsArmed) {
        g.R().textures_changed[0] = true; /* a load arms both units together (gdx_tmem.cpp) */
        g.R().textures_changed[1] = true;
        sFinWin.rearm++;
    }
    if (sFin.lastVtx != kFinNone) {
        g.ExecHookCmds(sFin.lastVtx, sFin.lastVtx + 1u);
    }
    FinPh(4);
    sFinWin.draws++;
}

/* The partition swaps the screen order of each pair (an earlier segment of the second texture, a
 * later one of the first). With no z-buffer that is invisible exactly when no pixel can be covered
 * by both: their screen x extents (the projected corners of each segment's object box; every w > 1
 * here, so the box's image lies inside its corners' hull) are apart by more than a margin covering
 * the GPU's own vertex placement (kFinPartMarginNdc). With the 3D slider up the
 * test runs per eye on the skewed x the shader produces, x' = x + e * k * (zc * z + zw * w - dc * w)
 * (SCENE row, e = -1 / +1): divided by w the terms in w shift every point of an eye alike, so the
 * per-eye extents are those of x/w + e * k * zc * z/w (zc = the backend's clip z scale, latched
 * from the first bind; until then the eye's whole z/w spread is the margin). */
bool FinPartDisjoint(const Gdx3dsVtxMathState& vs) {
    float k = 0.0f;
    if (Gdx3dsStereo::Active()) {
        float row[4];
        Gdx3dsStereo::ClassRow(Gdx3dsStereo::ClassifyDraw(false), row);
        k = fabsf(row[2]);
    }
    const int nEyes = k > 0.0f ? 2 : 1;
    const bool zcKnown = sFinZc > 0.0f;
    float lo[2][kFinSegs], hi[2][kFinSegs], zlo[kFinSegs], zhi[kFinSegs];
    uint64_t done = 0;
    for (int p = 0; p < sFin.nPairs; p++) {
        const int seg[2] = { sFin.pairA[p], sFin.pairB[p] };
        for (int q = 0; q < 2; q++) {
            const int i = seg[q];
            if ((done >> i) & 1u) {
                continue;
            }
            done |= (uint64_t)1u << i;
            lo[0][i] = lo[1][i] = zlo[i] = 1e30f;
            hi[0][i] = hi[1][i] = zhi[i] = -1e30f;
            for (int c = 0; c < 8; c++) {
                float cl[4];
                XfClipOf(vs, (c & 1) ? sFin.segHi[i][0] : sFin.segLo[i][0], (c & 2) ? sFin.segHi[i][1] : sFin.segLo[i][1],
                         (c & 4) ? sFin.segHi[i][2] : sFin.segLo[i][2], cl);
                const float x = cl[0] / cl[3];
                const float z = cl[2] / cl[3];
                zlo[i] = z < zlo[i] ? z : zlo[i];
                zhi[i] = z > zhi[i] ? z : zhi[i];
                for (int e = 0; e < nEyes; e++) {
                    const float xe = (nEyes == 1 || !zcKnown) ? x : x + (e == 0 ? -k : k) * sFinZc * z;
                    lo[e][i] = xe < lo[e][i] ? xe : lo[e][i];
                    hi[e][i] = xe > hi[e][i] ? xe : hi[e][i];
                }
            }
        }
        const int i = seg[0];
        const int j = seg[1];
        float margin = kFinPartMarginNdc;
        if (nEyes == 2 && !zcKnown) {
            const float zl = zlo[i] < zlo[j] ? zlo[i] : zlo[j];
            const float zh = zhi[i] > zhi[j] ? zhi[i] : zhi[j];
            margin += k * (zh - zl);
        }
        for (int e = 0; e < nEyes; e++) {
            if (!(hi[e][i] + margin < lo[e][j] || hi[e][j] + margin < lo[e][i])) {
                if (GDX_UNLIKELY(sFinProf) && sFinDiagLogs < 24) {
                    sFinDiagLogs++;
                    Log("[finish] overlap pair %d/%d eye %d k=%.4f zc=%.2f i=[%.4f %.4f] j=[%.4f %.4f] z i=[%.4f %.4f] "
                        "j=[%.4f %.4f] mono i=[%.4f %.4f]",
                        i, j, e, (double)k, (double)sFinZc, (double)lo[e][i], (double)hi[e][i], (double)lo[e][j],
                        (double)hi[e][j], (double)zlo[i], (double)zhi[i], (double)zlo[j], (double)zhi[j],
                        (double)lo[0][i], (double)hi[0][i]);
                }
                return false;
            }
        }
    }
    return true;
}

/* The banner in two draws (see FinEntry::partOk): every state command of the list already ran (in
 * list order), the vertex state is latched. 0 = refused before anything was drawn (the registers
 * may hold a load group: the ordered path starts from "unknown" and runs the groups it needs). */
int FinDrawParted(GdxInterpSeam& g, FinVs& v, int* regsTex) {
    const Mesh* m = sFin.mesh;
    if (!FinPartDisjoint(v.vs)) {
        sFinWin.partOverlap++;
        return 0;
    }
    FinPend pend;
    for (int pass = 0; pass < 2; pass++) {
        const int t = pass == 0 ? sFin.partA : 1 - sFin.partA;
        const unsigned b = pass == 0 ? 0u : sFin.partSplit;
        const unsigned n = pass == 0 ? sFin.partSplit : (unsigned)m->nTris - sFin.partSplit;
        if (n == 0) {
            continue;
        }
        FinRunGroup(g, t);
        *regsTex = t;
        const Gdx3dsLoadedVtx first = *XfVertex(v.vs, m, sFin.res2[(size_t)b * 3u]);
        if (!FinBindFirst(g, t, first)) {
            sFinWin.fbBind++;
            sFinTex[0].valid = false;
            sFinTex[1].valid = false;
            if (pass == 0) {
                return 0;
            }
            /* Cannot happen once the first texture bound (same combiner and modes, same format):
             * the second texture's triangles are left undrawn and counted. */
            return 1;
        }
        sFinWin.binds++;
        memset(&pend, 0, sizeof(pend));
        pend.open = true;
        pend.k0 = 0;
        pend.t = t;
        pend.idxBase = sFin.res2;
        pend.triBegin = b;
        pend.triCount = n;
        pend.first = first;
        FinSubmit(g, pend, v);
    }
    sFinWin.parted++;
    return 1;
}

/* The per-frame draw. 0 = not taken (nothing executed: the walker walks the list).
 *
 * FAST mode (every vertex carries the same colour bytes and the whole box is in front of w = 1):
 * no survivor screen. The screen only decides whether GfxSpTri1 would bind state for a segment
 * whose every triangle is culled -- backend state the next resolve diffs against, never a pixel --
 * and which vertex the batch aux and the stereo class read: with one colour and every w > 1 any
 * vertex gives the same aux and class. The PICA culls exactly what the CPU would (the machines'
 * gpuxf argument), so every segment is drawn and consecutive segments of one texture merge. */
int FinDraw(GdxInterpSeam& g) {
    Mesh* m = sFin.mesh;
    FinVs v;
    memset(&v, 0, sizeof(v));
    if (!g.LatchVtxState(v.vs)) {
        sFinWin.fbVs++;
        return 0;
    }
    float wMin = 0.0f;
    const XfCover cover = XfClassify(v.vs, sFin.box, &wMin);
    if (cover != XF_INSIDE) {
        if (cover == XF_OUTSIDE) {
            sFinWin.fbOut++;
        } else {
            sFinWin.fbClip++;
        }
        return 0;
    }
    const unsigned errMpx = XfErrorBoundMpx(v.vs, sFin.box, wMin);
    sFinWin.maxErrMpx = errMpx > sFinWin.maxErrMpx ? errMpx : sFinWin.maxErrMpx;
    if (sGpuXfMaxErr >= 0 && errMpx > (unsigned)sGpuXfMaxErr) {
        sFinWin.fbErr++;
        return 0;
    }
    const bool fast = sFin.oneColor && wMin > 1.0f;
    sFinWin.fast += fast ? 1u : 0u;
    g.Flush();
    g.BindingReset();
    sFinTex[0].valid = false;
    sFinTex[1].valid = false;
    const bool rejVariant = g.RejectVariant();
    const bool cpuClipOn = CpuClipEnabled();
    int regsTex = -1;       /* the texture whose load group the registers hold */
    bool regsArmed = false; /* ... loaded and not yet resolved (textures_changed armed) */
    FinPend pend;
    memset(&pend, 0, sizeof(pend));
    if (GDX_UNLIKELY(sFinProf)) {
        sFinT = svcGetSystemTick();
    }
    bool preDone = false;
    if (fast && sFin.partOk) {
        FinExecPre(g, 0, sFin.nCmdIdx); /* every state command, in list order (FinPartBake) */
        preDone = true;
        if (FinLatchVs(g, v)) {
            const int drawn = FinDrawParted(g, v, &regsTex);
            if (drawn) {
                FinPh(3);
                FinLeave(g, regsTex, false);
                return 1;
            }
        } else {
            v.live = false;
        }
    }
    for (int k = 0; k < sFin.nSeg; k++) {
        const FinSeg& s = sFin.seg[k];
        const int t = s.tex;
        /* The prelude: its state commands; its load group only while this texture has no binding
         * this frame and the registers do not already hold it. */
        if (!preDone) {
            FinExecPre(g, s.execBegin, s.execMid);
        }
        if (s.grpBegin != s.grpEnd) {
            if (!sFinTex[t].valid && regsTex != t) {
                FinSubmit(g, pend, v); /* a load before the pending draw's submit is fine; this keeps order plain */
                FinRunGroup(g, t);
                regsTex = t;
                regsArmed = true;
            } else {
                sFinWin.groupsSkipped++;
            }
        }
        if (!preDone) {
            FinExecPre(g, s.execMid, s.execEnd);
        }
        FinPh(0);
        const Gdx3dsMeshRun& run = m->runs[k];
        if (run.triCount == 0) {
            continue;
        }
        const bool relatch = !v.live || *g.R().geometry_mode != v.geo;
        if (fast && !relatch && pend.open && pend.t == t && pend.triBegin + pend.triCount == run.firstTri &&
            m->xfRunTex[k * 2] == m->xfRunTex[pend.k0 * 2] && m->xfRunTex[k * 2 + 1] == m->xfRunTex[pend.k0 * 2 + 1]) {
            pend.triCount += run.triCount; /* same texture, same state: one draw */
            sFinWin.merged++;
            continue;
        }
        FinSubmit(g, pend, v);
        FinPh(3);
        if (relatch && !FinLatchVs(g, v)) {
            sFinWin.fbVs++;
            if (regsTex != t || !regsArmed) {
                FinRunGroup(g, t);
            }
            FinStockTail(g, s.preEnd);
            return 1;
        }
        int firstTri = 0;
        if (!fast) {
            firstTri = XfFirstSurvivor(v.vs, m, run, rejVariant, v.cullMode, v.invert, cpuClipOn);
            FinPh(1);
            if (firstTri < 0) {
                sFinWin.empty++;
                continue; /* GfxSpTri1 resolves no state for a run with no survivor */
            }
        }
        const Gdx3dsLoadedVtx first = *XfVertex(v.vs, m, m->tris[((size_t)run.firstTri + (size_t)firstTri) * 3]);
        bool bound = false;
        if (sFinTex[t].valid) {
            bound = g.BindingApply(t) != 0;
            sFinWin.applies += bound ? 1u : 0u;
            sFinWin.applyMiss += bound ? 0u : 1u;
        }
        if (!bound) {
            if (regsTex != t || !regsArmed) {
                FinRunGroup(g, t);
                regsTex = t;
            }
            regsArmed = false;
            if (!FinBindFirst(g, t, first)) {
                sFinWin.fbBind++;
                FinStockTail(g, s.preEnd);
                return 1;
            }
            sFinWin.binds++;
        }
        FinPh(2);
        pend.open = true;
        pend.k0 = k;
        pend.t = t;
        pend.triBegin = run.firstTri;
        pend.triCount = run.triCount;
        pend.first = first;
    }
    FinSubmit(g, pend, v);
    FinPh(3);
    FinLeave(g, regsTex, regsArmed);
    return 1;
}

GDX_COLD void FinReceipt() {
    if (sFinWin.draws == 0 && sFinWin.fbOut + sFinWin.fbClip + sFinWin.fbErr + sFinWin.fbVs + sFinWin.fbBind +
                                      sFinWin.fails + sFinWin.bakes ==
                                  0) {
        return;
    }
    Log("[finish] frames=64 draws=%u fast=%u parted=%u overlap=%u drawcalls=%u merged=%u empty=%u bind=%u apply=%u amiss=%u groups run=%u "
        "skip=%u rearm=%u | "
        "fb out=%u clip=%u err=%u vs=%u bind=%u stale=%u tails=%u | bakes=%u fails=%u maxerr=%umpx | us=%lu/f "
        "(pre=%lu scr=%lu bind=%lu emit=%lu tail=%lu)",
        sFinWin.draws, sFinWin.fast, sFinWin.parted, sFinWin.partOverlap, sFinWin.segs, sFinWin.merged, sFinWin.empty, sFinWin.binds, sFinWin.applies, sFinWin.applyMiss,
        sFinWin.groupsRun, sFinWin.groupsSkipped, sFinWin.rearm, sFinWin.fbOut, sFinWin.fbClip, sFinWin.fbErr,
        sFinWin.fbVs, sFinWin.fbBind, sFinWin.fbStale, sFinWin.tails, sFinWin.bakes, sFinWin.fails,
        sFinWin.maxErrMpx, (unsigned long)(sFinWin.ticks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sFinWin.ph[0] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sFinWin.ph[1] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sFinWin.ph[2] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sFinWin.ph[3] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sFinWin.ph[4] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull));
    if (sFinProf) {
        Log("[finish] backend us/f tex=%lu const=%lu atfog=%lu xf=%lu submit=%lu tail=%lu",
            (unsigned long)(sFinWin.bx[0] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
            (unsigned long)(sFinWin.bx[1] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
            (unsigned long)(sFinWin.bx[2] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
            (unsigned long)(sFinWin.bx[3] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
            (unsigned long)(sFinWin.bx[4] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
            (unsigned long)(sFinWin.bx[5] * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull));
    }
    memset(&sFinWin, 0, sizeof(sFinWin));
}

} // namespace

int gdx3ds_mesh_finish_enabled(void) {
    return FinOn() ? 1 : 0;
}

int gdx3ds_mesh_finish_hook_seam(GdxInterpSeam& I, const void* listPtr, size_t listCmds, int stride, int isBig,
                                 Gdx3dsMeshResolveFn resolve, void* user) {
    if (!FinOn() || listPtr == nullptr || resolve == nullptr || stride != 8 || isBig) {
        return 0;
    }
    const uint64_t t0 = svcGetSystemTick();
    const uint8_t* list = (const uint8_t*)listPtr;
    if (sFin.src != listPtr || !FinListCurrent(list)) {
        if (sFin.src == listPtr && sFin.failed && sFin.bytes == nullptr) {
            return 0; /* known not bakeable at this address */
        }
        sFinWin.fbStale += sFin.src != nullptr ? 1u : 0u;
        sFin.src = listPtr;
        if (!FinBake(list, listCmds, resolve, user)) {
            FinRelease();
            return 0;
        }
    }
    const int taken = FinDraw(I);
    sFinWin.ticks += svcGetSystemTick() - t0;
    return taken;
}

/* nicourse [native] ni_course: the 3D course preview's models (gdx3ds_course_mesh.inc). */
#include "gdx3ds_course_mesh.inc"

/* ---- [native] machines_texskip queries (the owned walker's gdx_texskip.cpp) ------------- */

/* [native] machines_walkfast (c5mach2): texskip asks this of EVERY G_DL the owned walker meets
 * (~35 per race frame: track, HUD, sky, ...). racer.c's D_800CDD38 is a table of 30 pointers
 * that is never reassigned (only the lists' contents are rebuilt), so a pointer outside
 * [min, max] of the table cannot be in it: the same -1 the scan returns, without the scan. */
namespace {
struct SetListBounds {
    uintptr_t lo, hi;
    bool valid;
};
SetListBounds sSetBounds = { 0, 0, false };
int sWalkFast = 0; /* [native] machines_walkfast */

bool OutsideSetLists(const void* list) {
    if (!sSetBounds.valid) {
        sSetBounds.lo = UINTPTR_MAX;
        sSetBounds.hi = 0;
        for (int s = 0; s < kSlots; s++) {
            const uintptr_t p = (uintptr_t)D_800CDD38[s];
            sSetBounds.lo = p < sSetBounds.lo ? p : sSetBounds.lo;
            sSetBounds.hi = p > sSetBounds.hi ? p : sSetBounds.hi;
        }
        sSetBounds.valid = true;
    }
    const uintptr_t p = (uintptr_t)list;
    return p < sSetBounds.lo || p > sSetBounds.hi;
}

} // namespace

extern "C" int gdx3ds_mesh_texlist_slot(const void* list) {
    if (D_800CDD38 == nullptr || list == nullptr) {
        return -1;
    }
    if (sWalkFast && OutsideSetLists(list)) {
        return -1;
    }
    for (int s = 0; s < kSlots; s++) {
        if (D_800CDD38[s] == list) {
            return s;
        }
    }
    return -1;
}

extern "C" int gdx3ds_mesh_is_speck(const void* list) {
    /* racer.c: every slot's LOD-6 entry is the same far-speck list (D_7045098). */
    return (D_800CDDB0 != nullptr && list != nullptr && D_800CDDB0[kNativeLods] == list) ? 1 : 0;
}

extern "C" int gdx3ds_mesh_is_body(const void* list) {
    if (list == nullptr || Enabled() != 1) {
        return 0;
    }
    EnsureRegistry();
    return Find(list) != nullptr ? 1 : 0;
}

extern "C" int gdx3ds_mesh_body_probe(const void* body, Gdx3dsMeshResolveFn resolve, void* user, uint8_t inhTile,
                                      uint8_t inhUse, int* slot, uint8_t* tiles) {
    if (resolve == nullptr) {
        return 0;
    }
    Entry* e = LookupForDraw(body, resolve, user);
    const Mesh* m = e != nullptr ? e->mesh : nullptr;
    if (m == nullptr) {
        return 0;
    }
    const TexelUse use = { (inhUse & 1u) != 0, (inhUse & 2u) != 0, (inhUse & 4u) != 0 };
    *slot = e->slot;
    *tiles = ComputeReqTiles(m, inhTile, use);
    return 1;
}

/* [debug] machsplit (gdx_machsplit.cpp): the monotone native mesh draw clock, so the MACH
 * split can subtract the MESH bucket's time from the section command that contained it. */
extern "C" uint64_t gdx3ds_mesh_draw_ticks_total(void) {
    return sDrawTicksTotal;
}

extern "C" uint32_t gdx3ds_mesh_dress_epoch(void) {
    return sDressEpoch;
}

extern "C" uint32_t gdx3ds_mesh_generation(void) {
    return sGeneration.load(std::memory_order_acquire);
}

namespace {
inline unsigned long TicksToUs(uint64_t t) {
    return (unsigned long)(t * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC);
}

/* [mattab] receipt, every 64 frames with the lever on:
 *   runs      runs bound through a record (hit + every miss / verify full resolve)
 *   hit       served from the record; miss by reason: empty (first use) key (a register, tile or
 *             span view differs) vp (viewport/scissor pending) noimp (a unit owes an import the
 *             record has no hit for) tex (the recorded cache slot no longer holds the key)
 *             epoch (the texture-source class epoch moved) smp (sampler target unrecorded);
 *             full = verify mode's full resolves
 *   banked    records (re)written; inval = records that had served and no longer could (a key
 *             part, the cache slot or the epoch moved; a standing noimp miss is not one)
 *   binds     backend state calls the hits issued (only differences are issued)
 *   vfy       verify: compares of a would-hit record with the full resolve / mismatches (must be 0)
 *   elide     backend re-issues skipped (vbo, texture binds, alpha tests; verify: would-skip)
 *   recs      live record bytes; nomem = runs refused a record for lack of heap
 *   us/mesh   the whole machine draw per drawn mesh; bind = the material path per bound run */
GDX_COLD void LogMatTab() {
    unsigned elide[3] = { 0, 0, 0 };
    gdx3ds_mesh_backend_elide_take(elide);
    unsigned runs = 0;
    for (int i = 0; i < GDXSEAM_MM_RESULT_COUNT; i++) {
        runs += sWin.mtOut[i];
    }
    const unsigned boundRuns = sWin.xfRuns + sWin.xfWalk;
    Log("[mattab] level=%d runs=%u hit=%u miss empty=%u key=%u vp=%u noimp=%u tex=%u epoch=%u smp=%u full=%u | "
        "banked=%u inval=%u binds=%u live=%u",
        sMatTab, runs, sWin.mtOut[GDXSEAM_MM_HIT], sWin.mtOut[GDXSEAM_MM_MISS_EMPTY], sWin.mtOut[GDXSEAM_MM_MISS_KEY],
        sWin.mtOut[GDXSEAM_MM_MISS_VP], sWin.mtOut[GDXSEAM_MM_MISS_NOIMP], sWin.mtOut[GDXSEAM_MM_MISS_TEX],
        sWin.mtOut[GDXSEAM_MM_MISS_EPOCH], sWin.mtOut[GDXSEAM_MM_MISS_SMP], sWin.mtOut[GDXSEAM_MM_FULL],
        sWin.mtBanked, sWin.mtInval, sWin.mtBinds, sWin.mtLive);
    const unsigned* kb = sWin.mtKeyBits;
    Log("[mattab] keymiss cc=%u omL=%u omH=%u geo=%u tile=%u gray=%u t0=%u t1=%u lt0=%u lt1=%u", kb[0], kb[1], kb[2],
        kb[3], kb[4], kb[5], kb[6], kb[7], kb[8], kb[9]);
    sTot.mtMismatch += sWin.mtMismatch;
    Log("[mattab] vfy checks=%u mism=%u (cum %u) | elide vbo=%u texbind=%u atest=%u | recs=%luB nomem=%u | "
        "us/mesh=%lu bind=%lu/run (state+screen %lu) meshes=%u",
        sWin.mtChecked, sWin.mtMismatch, sTot.mtMismatch, elide[0], elide[1], elide[2], sMatBytes, sWin.mtNoHeap,
        sWin.draws > 0 ? TicksToUs(sWin.ticks) / sWin.draws : 0ul,
        boundRuns > 0 ? TicksToUs(sWin.mtBindTicks) / boundRuns : 0ul,
        boundRuns > 0 ? TicksToUs(sWin.mtScrTicks) / boundRuns : 0ul, sWin.draws);
    Log("[mattab] order runs=%u pinned=%u envsw=%u envfloor=%u texsw=%u texfloor=%u (switches between "
        "consecutive bound runs; floor = if the opaque runs were free to move)",
        sWin.ordRuns, sWin.ordPinned, sWin.ordPrgSw, sWin.ordPrgMin, sWin.ordTexSw, sWin.ordTexMin);
}
} // namespace

namespace {
GDX_COLD void MeshWindowReceipt();
} // namespace

extern "C" void gdx3ds_mesh_frame_begin(void) {
    gdx3ds_xf_census_frame(); /* [debug] xf_census cadence (independent of the machines lever) */
    /* Frame boundary on the DRAWING thread (the render thread when [debug] renderthread is
     * on, else main): the only place a retired mesh is ever free()d. Before the cadence
     * early-out -- the queue must drain every frame, not every 64th. */
    RetireDrain();
    if (&gdx3ds_nrr_mach_frame_begin != nullptr) {
        gdx3ds_nrr_mach_frame_begin(); /* n60 NRR machines: disarm, deferred frees, receipt */
    }
    sFrame++;
    if ((sFrame & 63) == 0 && sFinLever == 1) {
        FinReceipt(); /* n60finish [native] finish_mesh (silent while the banner is not drawn) */
    }
    CmFrameBegin(); /* nicourse [native] ni_course: frames that drew a course model, receipt every 64 */
    if ((sFrame & 63) != 0 || sEnabled != 1) {
        return;
    }
    if (sWin.draws == 0 && sWin.bakes == 0 && sWin.bakeFails == 0) {
        return; /* idle windows stay silent */
    }
    MeshWindowReceipt();
}

/* hotcold: the every-64th-frame machines receipts (and the window reset), out of line. */
namespace {
GDX_COLD void MeshWindowReceipt() {
    int baked = 0;
    for (int i = 0; i < kEntries; i++) {
        if (sEntries[i].mesh != nullptr) {
            baked++;
        }
    }
    Log("[native] machines=1%s lists=%d baked=%d bakes=%u/%u fails=%u draws=%u walk=%u runs=%u verts=%u tris=%u "
        "rej=%u out=%u cull=%u clip=%u fan=%u fb=%u pool=%u/%u empty=%u misidx=%u us=%lu/f (tnl=%lu bind=%lu emit=%lu draw=%lu) "
        "linfree=%luK",
        sWalkOnly ? " walk_only=1" : (sVerify ? " verify=1" : ""), sRegisteredCount, baked, sWin.bakes, sTot.bakes,
        sTot.bakeFails, sWin.draws, sWin.walkDraws, sWin.runs, sWin.verts, sWin.tris, sWin.rejected, sWin.allOut,
        sWin.culled, sWin.clipped, sWin.fanTris, sWin.bindFallbacks, sWin.poolRefused, sWin.poolTrunc, sWin.emptyRuns,
        sWin.idxMisaligned,
        (unsigned long)(sWin.ticks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sWin.tnlTicks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sWin.bindTicks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)((sWin.emitTicks - sWin.drawTicks) * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        (unsigned long)(sWin.drawTicks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull),
        gdx3ds_mesh_backend_linear_free_kib());
    if (sGpuXf) {
        /* draws gpu = meshes the GPU transform drew, invis = meshes skipped with no T&L (all
         * corners outside one plane), cpu = meshes the CPU path drew; fb = the refusals by
         * reason (tglin / lights / texs / clip / err / pool / params; flx is NOT a refusal: it
         * counts GPU meshes whose alpha the CPU took from the FLX LUT); walk = runs handed
         * to the interpreter; scr = vertices the survivor screen transformed; maxerr = worst
         * float24 bound seen (1/1000 px, gated at gpuxf_maxerr); us = the whole gpuxf path. */
        const unsigned cpuMeshes = sWin.draws - sWin.xfMeshes - sWin.xfInvisible;
        Log("[gpuxf] draws gpu=%u invis=%u cpu=%u runs=%u tris=%u verts=%u walk=%u scr=%u miss=%u | flx=%u fb "
            "tglin=%u lights=%u texs=%u clip=%u err=%u pool=%u params=%u | maxerr=%umpx us=%lu/f",
            sWin.xfMeshes, sWin.xfInvisible, cpuMeshes, sWin.xfRuns, sWin.xfTris, sWin.xfVerts, sWin.xfWalk,
            sWin.xfScreened, sWin.xfGateMiss, sWin.xfFbFlx, sWin.xfFbTgLin, sWin.xfFbLights, sWin.xfFbTexs,
            sWin.xfFbClip, sWin.xfFbErr, sWin.xfFbPool, sWin.xfFbParams, sWin.xfMaxErrMpx,
            (unsigned long)(sWin.xfTicks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / 64ull));
    }
    if (sRetained) {
        /* [native] machines_retained receipt (C5 Stage 1): meshes/runs drawn from the resident
         * buffers (no per-frame vertex or index copy), FLX meshes whose alpha the shader
         * computed, perf-mode straddlers (clip=1 only), runs bound without the survivor
         * screen (clip=1 only); fb lut = FLX meshes whose DMEM LUT was not the stock table
         * (drawn by the gpuxf upload path instead), fb alloc = meshes refused for lack of
         * linear memory; resident = live buffers / bytes; builds / reused = per window /
         * cumulative (reused = a rebake whose bytes did not change); retired = buffers queued
         * for the deferred free. */
        Log("[rr] machines retained=1 clip=%d meshes=%u runs=%u flx=%u straddle=%u noscreen=%u | fb lut=%u "
            "alloc=%u | resident=%d bytes=%lu builds=%u/%u reused=%u/%u retired=%u",
            sRetainedClip, sWin.rrMeshes, sWin.rrRuns, sWin.rrFlx, sWin.rrStraddle, sWin.rrNoScreen, sWin.rrFbLut,
            sWin.rrFbAlloc, sResCount, sResBytes, sWin.rrBuilds, sTot.rrBuilds, sWin.rrReused, sTot.rrReused,
            sWin.rrRetired);
    }
    if (sMatTab) {
        LogMatTab();
    }
    LogDress(); /* [native] dress_retained */
    if (gdx3ds_xfprof_on) {
        /* [xfprof] us per gpuxf bound run, by section (diagnostic; the clock reads inflate). */
        static const char* const kXfpName[XFP_COUNT] = { "state", "screen", "bind", "params", "vertex",
                                                          "packctx", "fillrun", "aux", "b_tex", "b_const",
                                                          "b_atfog", "b_xf", "b_submit", "b_tail", "mesh" };
        char buf[320];
        const unsigned runs = sWin.xfRuns > 0 ? sWin.xfRuns : 1u;
        int n = snprintf(buf, sizeof(buf), "[xfprof] runs=%u meshes=%u us/run", sWin.xfRuns, sWin.xfMeshes);
        for (int k = 0; k < XFP_COUNT && n > 0 && n < (int)sizeof(buf); k++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%.1f", kXfpName[k],
                          (double)gdx3ds_xfprof_ticks[k] * 1000.0 / CPU_TICKS_PER_MSEC / runs);
        }
        Log("%s", buf);
        memset(gdx3ds_xfprof_ticks, 0, sizeof(gdx3ds_xfprof_ticks));
    }
    if (sSpeck) {
        /* [speck] far specks drawn as the native mesh this window / bakes refused (walked). */
        Log("[speck] machines_speck=1 draws=%u fails=%u", sSpeckDraws, sSpeckFails);
        sSpeckDraws = 0;
        sSpeckFails = 0;
    }
    if (sVerify) {
        Log("[native] verify draws=%u tris=%u triMismatch=%u countMismatch=%u matMismatch=%u auxDiff=%u "
            "(cumulative triMismatch=%u; auxDiff excludes the per-vertex inputs, so it must be 0)",
            sWin.vfyDraws, sWin.vfyTris, sWin.vfyTriMismatch, sWin.vfyCountMismatch, sWin.vfyMatMismatch,
            sWin.vfyAuxDiff, sTot.vfyTriMismatch + sWin.vfyTriMismatch);
        sTot.vfyTriMismatch += sWin.vfyTriMismatch;
        sTot.vfyDraws += sWin.vfyDraws;
    }
    /* Mesh-lifetime receipt (the ownership note at the top of this file). overlap= counts
     * retires that happened while the other thread was inside DrawMesh -- the strictest
     * form, and rare. The `window` group is the structural measurement and does not depend
     * on catching both threads in DrawMesh at one instant:
     *   lookupOvl  registry reads made from a non-drawing thread with a render TASK live
     *              (the bridgemain overlap, counted at the point of use)
     *   bakeOvl    of those, the ones that had to bake
     *   inflight   of those, the ones that superseded an existing mesh -- i.e. EXACTLY the
     *              free the pre-fix code performed in place, inside the window
     * All three are expected to be nonzero and are harmless with the deferral on.
     * stale= and unsafe= are the regression detectors and MUST stay 0. */
    Log("[native] meshlife defer=%d retired=%u freed=%u held=%u | window lookupOvl=%u bakeOvl=%u inflight=%u "
        "overlap=%u/%u | bakeWait=%u stale=%u unsafe=%u",
        sDeferFree, Get(sLife.retired), Get(sLife.freed), Get(sLife.held), Get(sLife.lookupOvl),
        Get(sLife.bakeOvl), Get(sLife.inflight), Get(sLife.overlapSame), Get(sLife.overlap),
        Get(sLife.bakeWait), Get(sLife.staleDraw), Get(sLife.unsafeFree));
    const Stats keepTot = sTot;
    memset(&sWin, 0, sizeof(sWin));
    sTot = keepTot;
}
} // namespace

extern "C" void gdx3ds_mesh_lists_rebuilt(void) {
    /* Release: everything the game wrote into the rebuilt lists must be visible to the
     * thread that observes the bump and rebakes from them (audit S3 -- the ordering half
     * of the free/draw race). Called from the game thread only. */
    sGeneration.fetch_add(1u, std::memory_order_release);
}
