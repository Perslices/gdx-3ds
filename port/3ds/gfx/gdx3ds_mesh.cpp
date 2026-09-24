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
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_native_cfg.h"

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

namespace {

constexpr int kSlots = 30;
constexpr int kLodsPerSlot = 6;
constexpr int kNativeLods = 5;
constexpr int kEntries = kSlots * kLodsPerSlot;
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
};

constexpr uint32_t kStubMagic = 0x4D455348u; /* "MESH" */
constexpr uint32_t kEndDl = 0xDFu;

Stub sStubs[kEntries];
Entry sEntries[kEntries];
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

/* receipts */
unsigned long sFrame = 0;
struct Stats {
    unsigned draws, walkDraws, runs, verts, tris, rejected, allOut, culled, clipped, fanTris, bindFallbacks, poolRefused,
        poolTrunc, emptyRuns, idxMisaligned;
    unsigned bakes, bakeFails, rebakes;
    unsigned vfyDraws, vfyTris, vfyTriMismatch, vfyCountMismatch, vfyMatMismatch, vfyAuxDiff;
    uint64_t ticks, tnlTicks, bindTicks, emitTicks, drawTicks;
} sWin = {}, sTot = {};
int sBakeLogs = 0;
int sFailLogs = 0;

/* per-draw scratch (render thread only) */
Gdx3dsLoadedVtx sLoaded[GDX3DS_MESH_MAX_VERTS];
uint16_t sRemap[GDX3DS_MESH_MAX_VERTS];
uint16_t sIdx[kMaxIdxScratch];
Gdx3dsMeshWalkOut sWalkScratch;

void Log(const char* fmt, ...) {
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

void TruncLoud(uint32_t maxVerts, uint32_t nOut, uint32_t need, unsigned triCount, unsigned tri) {
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

/* Returns the enabled flag, latching the INI keys once the loader says they are ready. */
int Enabled() {
    if (sEnabled >= 0) {
        return sEnabled;
    }
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

void EnsureRegistry() {
    if (sRegistered) {
        return;
    }
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
void RetireMesh(Mesh* m) {
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
void RetireDrain() {
    if (!OnDrawingThread()) {
        return;
    }
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

bool Bake(Entry& e, Gdx3dsMeshResolveFn resolve, void* user) {
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
    gdx3ds_mesh_walk(e.src, GDX3DS_MESH_MAX_CMDS, resolve, user, &w);
    if (w.status != GDX3DS_MESH_WALK_OK || w.nTris == 0 || w.nVerts == 0) {
        e.failed = 1;
        RetireOld(e, old);
        sWin.bakeFails++;
        sTot.bakeFails++;
        if (sFailLogs < 64) {
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
    e.mesh = m; /* the single publishing store; only now is `old` unreachable */
    RetireMesh(old);
    sWin.bakes++;
    sTot.bakes++;
    const uint64_t dt = svcGetSystemTick() - t0;
    if (sBakeLogs < 160 || sVerbose) {
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

/* Native draw of one run. Returns false if the backend refused (caller replays). */
bool NativeRun(GdxInterpSeam& g, const Mesh* m, const Gdx3dsMeshRun& run, const GdxSeamMaterial& mat,
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
        const PackInput cv = InputFor(R, mat, c.ci, 0, &sLoaded[0]);
        c.constColor = cv.c;
        if (mat.inputMap[0][c.ci] == GDXSEAM_CCMUX_SHADE_ALPHA) {
            return false; /* per-vertex rgb from shade alpha: not modelled, replay */
        }
    }
    if (c.ai >= 0 && !c.alphaIsShade) {
        const uint8_t mux = mat.inputMap[1][c.ai];
        c.constAlphaIsShade = (mux == GDXSEAM_CCMUX_SHADE_ALPHA);
        const PackInput av = InputFor(R, mat, c.ai, 1, &sLoaded[0]);
        c.constAlpha = av.c;
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
    if (nIdx % 3u != 0u) {
        sWin.idxMisaligned++;
        sTot.idxMisaligned++;
        if (!sTruncLogged) {
            sTruncLogged = 1;
            Log("GEOMLOSS: machines index buffer MISALIGNED (nIdx=%u, not a multiple of 3) — trimmed to "
                "%u; a cap broke mid-triangle, which would have drawn the rest of the run from "
                "mismatched vertices",
                nIdx, nIdx - (nIdx % 3u));
        }
        nIdx -= nIdx % 3u;
    }
    if (nIdx == 0) {
        gdx3ds_mesh_backend_draw(0, sIdx, 0, nullptr); /* releases the reservation */
        return true;
    }
    /* Batch aux record: vertex 0 of the first surviving triangle (GfxSpTri1 fills it on the
     * batch's first append, after reject/cull). */
    Gdx3dsVboPackAux aux;
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

void CaptureCb(const Gdx3dsMeshCapture* cap, void* user) {
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

void NativeDraw(GdxInterpSeam& g, const Mesh* m, bool allowWalkFallback);

int sVerifyLogs = 0;
void VerifyDraw(GdxInterpSeam& g, const Mesh* m, int slot, int lod) {
    if (sCap[0] == nullptr) {
        sCap[0] = (CapTri*)malloc(sizeof(CapTri) * kCapMaxTris);
        sCap[1] = (CapTri*)malloc(sizeof(CapTri) * kCapMaxTris);
        if (sCap[0] == nullptr || sCap[1] == nullptr) {
            Log("[native] verify: capture buffers unavailable, drawing natively without verify");
            sVerify = 0;
            NativeDraw(g, m, true);
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
    NativeDraw(g, m, false);
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

void NativeDraw(GdxInterpSeam& g, const Mesh* m, bool allowWalkFallback) {
    bool native = true;
    Gdx3dsVtxMathState vs;
    if (!g.LatchVtxState(vs)) {
        native = false;
        sWin.bindFallbacks++;
        sTot.bindFallbacks++;
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
            const bool bound = g.BindMaterial(mat);
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

void DrawMesh(GdxInterpSeam& g, const Mesh* m, int slot, int lod) {
    /* Debug assertion for the ownership invariant: a live mesh always carries kMeshMagic,
     * and FreeMesh poisons the header before releasing it. A nonzero stale= on the
     * `[native] meshlife` receipt means something freed a mesh a draw still held -- i.e.
     * the race this file's ownership note exists to prevent has regressed. */
    if (m == nullptr || m->magic != kMeshMagic) {
        Bump(sLife.staleDraw);
        if (sLifeLogs < 8) {
            sLifeLogs++;
            Log("[native] meshlife STALE DRAW tick=%llu tid=%lu slot=%d lod=%d mesh=%p magic=%08lx",
                (unsigned long long)svcGetSystemTick(), (unsigned long)SelfTid(), slot, lod + 1, (const void*)m,
                (unsigned long)(m != nullptr ? m->magic : 0u));
        }
        return;
    }
    /* COVERAGE: which machine slot at which LOD actually reached the screen this frame. The
     * 4P material bug (B10/machfix) was invisible to a frame hash unless the scene also
     * recycled a texture id, and the LOD ladder is exactly the axis a hand-placed SHOT never
     * walked. value = (lod << 8) | slot. */
    gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_MACH,
                                   ((uint32_t)(lod & 0xFF) << 8) | (uint32_t)(slot & 0xFF)));
    const uint64_t t0 = svcGetSystemTick();
    sDrawingTid.store(SelfTid(), std::memory_order_relaxed);
    sDrawingMesh.store((uintptr_t)m, std::memory_order_release);
    g.Flush();
    if (sVerify) {
        VerifyDraw(g, m, slot, lod);
    } else if (sWalkOnly) {
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
    } else {
        NativeDraw(g, m, true);
        sWin.draws++;
        sTot.draws++;
    }
    sDrawingMesh.store(0, std::memory_order_release);
    sWin.ticks += svcGetSystemTick() - t0;
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

} // namespace

/* ---- C API --------------------------------------------------------------------------- */

extern "C" uintptr_t gdx3ds_mesh_bridge_lookup(uint64_t dlHostPtr, Gdx3dsMeshResolveFn resolve, void* user) {
    const void* src = (sizeof(void*) == 4) ? (const void*)(uintptr_t)(dlHostPtr & 0xFFFFFFFFu)
                                           : (const void*)(uintptr_t)dlHostPtr;
    Entry* e = LookupForDraw(src, resolve, user);
    if (e == nullptr) {
        return 0;
    }
    return (uintptr_t)&sStubs[e - sEntries];
}

int gdx3ds_mesh_dl_hook_seam(GdxInterpSeam& I, const void* listPtr, Gdx3dsMeshResolveFn resolve, void* user) {
    const uintptr_t p = (uintptr_t)listPtr;
    const uintptr_t base = (uintptr_t)sStubs;
    Entry* e = nullptr;
    const Mesh* mesh = nullptr;
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
    DrawMesh(I, mesh, e->slot, e->lod);
    return 1;
}

extern "C" void gdx3ds_mesh_frame_begin(void) {
    /* Frame boundary on the DRAWING thread (the render thread when [debug] renderthread is
     * on, else main): the only place a retired mesh is ever free()d. Before the cadence
     * early-out -- the queue must drain every frame, not every 64th. */
    RetireDrain();
    sFrame++;
    if ((sFrame & 63) != 0 || sEnabled != 1) {
        return;
    }
    if (sWin.draws == 0 && sWin.bakes == 0 && sWin.bakeFails == 0) {
        return; /* idle windows stay silent */
    }
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

extern "C" void gdx3ds_mesh_lists_rebuilt(void) {
    /* Release: everything the game wrote into the rebuilt lists must be visible to the
     * thread that observes the bump and rebakes from them (audit S3 -- the ordering half
     * of the free/draw race). Called from the game thread only. */
    sGeneration.fetch_add(1u, std::memory_order_release);
}
