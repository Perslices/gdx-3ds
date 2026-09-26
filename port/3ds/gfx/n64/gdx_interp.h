/* port/3ds/gfx/n64/gdx_interp.h -- the owned F3DEX2 interpreter (plan 06, Tier 1).
 *
 * One instance lives on the render thread. RunFrame walks the task's Gfx stream through
 * DlSrc, keeps the RSP/RDP state mirrors below (shaped like the LUS structs wherever parity
 * depends on them, plan §3.4) and drives the citro3d backend through gdxn::Backend.
 *
 * TUs: gdx_dl.cpp (walker), gdx_interp.cpp (frame, pure-state handlers), gdx_rsp.cpp (matrices,
 * lights, T&L), gdx_tmem.cpp (TMEM spans + loads), gdx_texcache.cpp (content cache + decoders),
 * gdx_tri.cpp (state diff, batch emission), gdx_rect.cpp (viewport/scissor/rects), gdx_cc.cpp.
 * Milestone state: docs/research/native/interp-progress.md. */
#ifndef GDX_INTERP_H
#define GDX_INTERP_H

#include <stddef.h>
#include <new>
#include <stdint.h>

#include "gdx_backend.h"
#include "gdx_cc.h"
#include "gdx_dlsrc.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx_gbi.h"

typedef struct GdxGfxJob GdxGfxJob;
class GdxOwnedSeam; /* gdx_seam_owned.cpp: the native replay seam over this interpreter */

namespace gdxn {

constexpr int kMaxLights = 32;
constexpr int kMaxVertices = 64;
constexpr int kMatrixStack = 11;
constexpr int kCallStack = 32;
constexpr int kMaxTriBuffer = 256;
constexpr unsigned kTexCacheSize = 1024;
constexpr unsigned kTexCacheHash = 2048;
/* Decode scratch. kTexUploadBytes is the INITIAL size (the historic fixed 256 KiB, a plan
 * deviation: LUS has a flat 4 MiB). DROPSITES: it used to be a hard ceiling -- a texture whose
 * decode needed more was skipped, and because ImportTexture marked the entry uploaded anyway,
 * the cache id kept whatever the previous owner of that backend id had drawn. The scratch now
 * grows on demand up to kTexUploadMaxBytes, which is the largest upload the PICA backend can
 * accept at all (GfxRenderingAPIC3D::UploadTexture refuses padW/padH > 1024, so 1024*1024
 * RGBA8 is the true ceiling). Above that no decode could ever have been displayed, so the
 * import takes the neutral fallback instead of a stale sample. */
constexpr uint32_t kTexUploadBytes = 256u * 1024u;
constexpr uint32_t kTexUploadMaxBytes = 1024u * 1024u * 4u;
/* The neutral fallback an undecodable import uploads in place of a stale sample: opaque white,
 * 8x8 because 8 is the PICA minimum side (UploadTexture pads below it and the padding would
 * otherwise be transparent black wherever the UVs ran past the single texel). */
constexpr uint32_t kFallbackTexDim = 8u;
constexpr uint32_t kFallbackTexBytes = kFallbackTexDim * kFallbackTexDim * 4u;

/* N64 Light_t / lookat / point light, host-endian 16 bytes (LUS F3DLight union). */
struct Light {
    uint8_t col[3];
    uint8_t pad1;
    uint8_t colc[3];
    uint8_t pad2;
    int8_t dir[3];
    uint8_t pad3;
    uint8_t pad4[4];
};
struct PointLight {
    uint8_t col[3];
    uint8_t unk3;
    uint8_t colc[3];
    uint8_t unk7;
    int16_t pos[3];
    uint8_t unkE;
    uint8_t pad;
};
union LightU {
    Light l;
    PointLight p;
};

/* N64 Vtx_t, 16 bytes; host-endian in wide/host-N64 lists, big-endian in raw-BE lists. */
struct Vtx {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
};

struct Rgba {
    uint8_t r, g, b, a;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    Rgba color;
    uint8_t clip_rej;
    uint8_t clip_out;
};

struct Rect {
    int16_t x, y;
    uint32_t width, height;
};

struct Rsp {
    float modelview_matrix_stack[kMatrixStack][4][4];
    uint8_t modelview_matrix_stack_size;
    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light lookat[2];
    LightU current_lights[kMaxLights + 1];
    float current_lights_coeffs[kMaxLights][3];
    float current_lookat_coeffs[2][3];
    uint8_t current_num_lights; /* includes ambient */
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    uint32_t extra_geometry_mode;

    float viewport_z_scale;
    float viewport_z_trans;

    struct {
        uint16_t s, t; /* U0.16 */
    } texture_scaling_factor;

    LoadedVertex loaded_vertices[kMaxVertices + 4];

    uint8_t dmem[4096];
    uint16_t dma_io_dmem;
    bool dma_io_loaded;
    float f3dflx_alpha_light[3];
    bool f3dflx_alpha_light_valid;
};

struct Tile {
    uint8_t fmt, siz;
    uint8_t cms, cmt;
    uint8_t masks, maskt;
    uint8_t shifts, shiftt;
    float uls, ult, lrs, lrt;
    uint16_t tmem; /* 64-bit word units */
    uint32_t line_size_bytes;
    uint8_t palette;
    uint16_t tmem_index;
};

/* One TMEM load record (LUS LoadedTexture minus the resource handle). `immutable` is the
 * LUS "resource-backed" class (o2r-keyed asset-segment texture): legacy decode from the source
 * bytes, same-content skip eligible. `native_rgba16` texels are host-order (byte-swapped into
 * the mirror, the way CopyRawTextureBytes fed LUS). */
struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t size_bytes;
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint16_t tmem_start;
    uint16_t tmem_word_count;
    uint8_t immutable;
    uint8_t native_rgba16;
    uint8_t masked;  /* always 0 on this port (no replacement packs) */
    uint8_t blended; /* always 0 */
};

struct Rdp {
    const uint8_t* palettes[2];
    const uint8_t* palette_dram_addr[2];
    uint8_t palette_staging[2][256];
    struct {
        const uint8_t* addr; /* resolved lazily at load time */
        uint8_t fmt;
        uint8_t siz;
        uint32_t width;
        uint8_t immutable;
        uint8_t native_rgba16;
        Cmd cmd; /* the SETTIMG operand token */
    } texture_to_load;

    /* Span model (plan §3.6): base slots hold the record; interior words derive lazily. */
    LoadedTexture loaded_texture[512];
    uint16_t tmem_span_base[512];  /* 0 = none, else base + 1 */
    uint16_t tmem_span_words[512]; /* valid at a base */
    uint8_t tmem_span_load_kind[512];
    /* [tmemblock] the SOURCE-STABILITY stamp of the last mirror written at this base:
     * [0] gNativeRgba16Generation, [1] gDmaGeneration -- sampled BEFORE the copy -- and the
     * source class they were sampled under. Read only at a base whose span is still live and
     * whose descriptor still matches; every path that retires a span already zeroes
     * tmem_span_words[base], which SameContentDescriptor tests before the stamp is consulted.
     * 8.5 KiB on the one heap-allocated Interp (gdx_rt_glue.cpp:684). tmemblock-progress.md S2. */
    uint64_t tmem_span_src_stamp[512][2];
    uint8_t tmem_span_src_cls[512];
    uint16_t tmem_span_list[64];
    uint16_t tmem_span_list_n; /* 0xFFFF = invalid (rebuild) */
    alignas(8) uint8_t tmem[4096];
    uint32_t tmem_generation;

    Tile texture_tile[8];
    bool textures_changed[2];
    uint8_t first_tile_index;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    uint8_t prim_lod_fraction;
    uint16_t prim_depth;
    Rgba env_color, prim_color, fog_color, blend_color, fill_color, grayscale_color;
    Rgba key_center, key_scale;
    int16_t convert_k[6];

    Rect viewport, scissor;
    bool viewport_or_scissor_changed;
    uintptr_t z_buf_address;
    uintptr_t color_image_address;
};

/* Texture cache key: content, never identity (plan §3.7). Mirrors TextureCacheKey. */
struct TexKey {
    const uint8_t* addr;
    const uint8_t* pal[2];
    uint8_t fmt, siz, palette_index;
    uint32_t size_bytes;
    uint32_t line_size_bytes;
    uint32_t full_image_line_size_bytes;
    uint16_t tile_width, tile_height;
    uint8_t cms, cmt, masks, maskt;
    uint32_t content_hash;
    uint32_t palette_content_hash; /* [native] palhash; 0 when the lever is off or fmt is not CI */
};

struct TexEntry {
    TexKey key;
    uint32_t texture_id;
    bool uploaded;
    bool used;
    int16_t lruPrev, lruNext;
    int16_t hashNext;
};

/* Backend-facing state the last batch was drawn with (the diff set, plan §3.9). */
struct RenderState {
    uint8_t depth_test_and_mask; /* 0xFF = unknown */
    uint8_t decal_mode;          /* 0/1, 0xFF = unknown */
    uint8_t alpha_blend;         /* 0/1, 0xFF = unknown */
    Rect viewport, scissor;
    void* shader_program;
    int16_t textures[2]; /* TexEntry index bound per unit, -1 none */
    bool sampler_valid[2];
    bool sampler_linear[2];
    uint8_t sampler_cms[2];
    uint8_t sampler_cmt[2];
};

/* Why an open batch was cut. One class per cause so the [ncensus] table ranks them:
 * the five ex-FLUSH_CONST register classes are split apart because only some of them can
 * ride per vertex (the prim/env fold), and lumping them hid that. */
enum FlushReason : uint8_t {
    FLUSH_NONE = 0,
    FLUSH_SHADER,
    FLUSH_TEXTURE,
    FLUSH_DEPTH,
    FLUSH_DECAL,
    FLUSH_VIEWPORT,
    FLUSH_SCISSOR,
    FLUSH_ALPHA,
    FLUSH_PRIM,      /* SETPRIMCOLOR value change (incl. prim_lod_fraction) */
    FLUSH_ENV,       /* SETENVCOLOR value change */
    FLUSH_FOGCOLOR,  /* SETFOGCOLOR value change */
    FLUSH_BLENDA,    /* SETBLENDCOLOR alpha change ([native] blendflush) */
    FLUSH_PRIMDEPTH, /* SETPRIMDEPTH value change */
    FLUSH_FBOP,
    FLUSH_FULL,
    FLUSH_TASK_END,
    FLUSH_FOG,      /* G_MW_FOG line change */
    FLUSH_COMBINER,
    FLUSH_SAMPLER,
    FLUSH_HOOK, /* a native replay hook (track/fx marker, machines G_DL) drained the batch */
    FLUSH_COUNT
};

/* [native] ncensus: batch-size buckets, by triangles in the drained batch. */
enum { kBatchHistBuckets = 8 };

/* [native] dspcensus: where a dispatched command's microseconds go. The walker is ITERATIVE
 * (G_DL pushes a frame and returns), so Exec() never re-enters Walk and per-opcode time is
 * SELF time by construction -- unlike LUS's [profop], whose 06 bucket is inclusive. The
 * already-attributed [prof] child sections (vtx/tri/imp/drw/mtx) are subtracted so the census
 * sums to `dsp`, the dispatch remainder, and not to the whole frame.
 *   DSP_PH_FETCH     ReadCmd + the readable-bound test + the op/cmds bookkeeping + RemapF3d
 *   DSP_PH_DLRESOLVE DlSrc::ResolveList inside G_DL (the bridge seam: segment epoch, source
 *                    translation, KnownCommandLimit, LooksLikeDisplayList, list facts)
 *   DSP_PH_DLHOOK    the registered machines G_DL hook -- **THIS IS NOT DISPATCH.** It is
 *                    DrawMesh: the [native] machines feature doing real drawing work, billed
 *                    to G_DL because the hook is called from the G_DL handler. Its own [prof]
 *                    child sections are subtracted from DE's SELF time but NOT from this
 *                    bucket, so it is inclusive where every other phase here is self, and it
 *                    tracks machine count (16 us/call in the thinnest window, 94 us/call in a
 *                    30-machine storm). The dispatch part of G_DL is DLRESOLVE. Anyone reading
 *                    this census for a dispatch win must skip this line -- dispatch-progress.md
 *                    and loadblock-progress.md both say so, and it is still the biggest number
 *                    in the G_DL row.
 *   DSP_PH_DLPUSH    call-stack push / tail-call bookkeeping
 * Loop overhead is `dsp` minus the sum; it is not a bucket. */
enum DspPhase {
    DSP_PH_FETCH = 0,
    DSP_PH_DLRESOLVE,
    DSP_PH_DLHOOK,
    DSP_PH_DLPUSH,
    /* F3 (G_LOADBLOCK), the census's #1: the per-load source classification and the mirror.
     *   DSP_PH_TIMGRES   DlSrc::ResolveTimg (the bridge's SETTIMG resolution)
     *   DSP_PH_TIMGCLASS TexelsAreNativeRgba16 + TimgIsResource -- the native-RGBA16 range
     *                    scan and the o2r asset lookup, re-run on EVERY load
     *   DSP_PH_TMEMSAME  the same-content skip test
     *   DSP_PH_TMEMCOPY  MirrorToTmem
     *   DSP_PH_TMEMSTORE StoreLoadedTexture + NoteTmemLoadKind (the span store) */
    DSP_PH_TIMGRES,
    DSP_PH_TIMGCLASS,
    DSP_PH_TMEMSAME,
    DSP_PH_TMEMCOPY,
    DSP_PH_TMEMSTORE,
    DSP_PH_COUNT
};

/* GfxSpTri1's resolved draw state: what SpTri's state block leaves behind after binding the
 * backend (combiner, textures, sampler, shader, blend) and before the packed emission. Banked
 * as the tri memo the native replay seam reads (gdx_seam_owned.cpp PackState) and produced
 * on demand for the machines binder (ResolveDrawState). */
struct DrawState {
    Combiner* comb;
    void* prg;
    uint32_t tm;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2], effective_tile[2];
    bool usedTextures[2]; /* ShaderGetInfo */
    uint8_t numInputs;
    ClipParams clip;
    bool use_alpha, use_fog, use_blend_color;
};

struct Counters {
    uint32_t op[256];
    uint32_t listsByClass[3]; /* Wide, HostN64, RawBE */
    uint32_t listsF3d;
    uint32_t lists;
    uint32_t cmds;
    uint32_t tris;       /* submitted (non-rect) */
    uint32_t trisEmitted;
    uint32_t trisRejected;
    uint32_t trisCulled;
    uint32_t vtx;
    uint32_t batches;
    uint32_t packRefused;
    uint32_t packSplit; /* GEOMLOSS: begins capped below kMaxTriBuffer by pool pressure */
    uint32_t flush[FLUSH_COUNT];
    /* --- [native] ncensus (inert when the lever is off) --- */
    uint32_t flushDrain[FLUSH_COUNT]; /* the subset of flush[] that actually drained tris */
    uint64_t flushTicks[FLUSH_COUNT]; /* svcGetSystemTick spent inside those drains */
    uint32_t batchHist[kBatchHistBuckets];
    uint32_t batchTris;               /* sum of tris over drained batches */
    uint32_t maxBatchesPerTask;
    uint32_t foldable[2];             /* prim / env splits the const-input rule would absorb */
    uint32_t folded[2];               /* prim / env splits actually absorbed ([native] ownfold) */
    uint32_t unknownOps;
    uint32_t droppedOps;
    uint32_t stackOverflow;
    uint32_t runaway;
    uint32_t noopLists;
    uint32_t branchZTaken;
    uint32_t tasks;
    uint32_t rootRejects;
    uint32_t texImports, texHits, texMisses, texEvicts, texDecodeOverflow;
    uint32_t texClears, texInvalidates, texInvalidated;
    /* --- DROPSITES: the texture-cache silent-loss site (gdx_texcache.cpp UploadPrepare). --- */
    uint32_t texDecodeGrow;     /* scratch reallocations (a texture that used to be skipped) */
    uint32_t texNoUpload;       /* imports that reached the end of the decode switch with no upload */
    uint32_t texFallback;       /* neutral fallback uploads issued in place of a stale sample */
    /* --- DROPSITES: G_VTX past kMaxVertices (gdx_rsp.cpp SpVertex). --- */
    uint32_t vtxOver;           /* G_VTX loads whose [dest, dest+n) left the 64-slot window */
    uint32_t vtxOverDropped;    /* vertices those loads did NOT transform */
    /* --- DROPSITES: clip-polygon truncation (gdx_tri.cpp SpTri). --- */
    uint32_t clipTrunc;         /* Sutherland-Hodgman planes that hit the 16-vertex output cap */
    uint32_t tmemLoads, tmemSkips, timgUnresolved;
    /* [native] tmemskip receipt. `tmemDescSame` counts loads whose whole descriptor matched the
       live span (everything SameContentLoad tests EXCEPT the immutable gate) -- i.e. the loads a
       staleness test is even allowed to look at. `tmemStale` are those the test then rejected,
       `tmemSkipImm`/`tmemSkipGen` split the skips by which gate fired, and tmemClsN/S/D/U count
       every load by source class so the class the win actually rides on is visible rather than
       assumed. All inert with the lever off except tmemDescSame/tmemCls*, which the census arms. */
    uint32_t tmemDescSame, tmemStale, tmemSkipImm, tmemSkipGen;
    uint32_t tmemClsN, tmemClsS, tmemClsD, tmemClsU;
    /* WHY the descriptor did not match, first failing test wins. This is the column that says
       whether a staleness test can ever pay: the gate is per TMEM BASE against the load that
       last wrote it, not per texture, so if the game cycles several textures through one base
       inside a task the descriptor never matches and no staleness term can help.
         span   no live span starts at this base (retired by an overlapping load, or never set)
         kind   a span is there, written by the OTHER load kind (LOADBLOCK vs LOADTILE)
         words  a span is there, of a different length
         addr   same span, same length -- a DIFFERENT SOURCE POINTER, i.e. another texture
         other  same pointer, some other descriptor field moved (sizes, strides, swap, o2r) */
    uint32_t tmemMissSpan, tmemMissKind, tmemMissWords, tmemMissAddr, tmemMissOther;
    /* [memotear] [debug] tmemfault receipt (NEXT.md B54): the refusal arm, exercised on
       purpose. tmemFaultGen/tmemFaultCls are the injections made by each mechanism (the source
       generation moved; the class changed under the span); tmemFaultCaught are the ones the
       gate then REFUSED, and tmemFaultMissed the ones it skipped anyway. caught == gen + cls
       and missed == 0 is the claim. All zero unless [debug] tmemfault != 0. */
    uint32_t tmemFaultGen, tmemFaultCls, tmemFaultCaught, tmemFaultMissed;
    uint32_t timgMemoHit, timgMemoMiss, timgMemoInert; /* [native] timgmemo receipt */
    /* Miss BREAKDOWN -- which part of the key failed. A memo that misses is useless in three
       different ways and they have three different fixes, so the receipt says which:
         Empty  the slot was never filled (cold, or thrashed by collisions)
         Epoch  the key matched and the freshness term did not  -> invalidation churn
         Key    the slot held a different operand                -> collision or genuine variety */
    uint32_t timgMemoMissEmpty, timgMemoMissEpoch, timgMemoMissKey;
    uint32_t timgEpochChanges; /* times the class epoch differed from the previous load's */
    uint32_t rects;
    uint32_t vtxUnresolved, mtxUnresolved;
    /* --- [native] dspcensus (inert when the lever is off) --- */
    uint64_t opTicks[256];              /* per-opcode SELF ticks, [prof] child sections removed */
    uint64_t phTicks[DSP_PH_COUNT];     /* walk-phase ticks */
    uint32_t phCalls[DSP_PH_COUNT];
    uint64_t walkTicks;                 /* the whole Walk(), for the unattributed remainder */
};

/* RAII [prof] section; one int test when tracing is off. */
class ProfScope {
  public:
    ProfScope(Backend* b, int sec) : mB(b), mSec(sec), mOn(b->ProfActive()) {
        if (mOn) mT0 = mB->ProfEnter(&mSnap);
    }
    ~ProfScope() {
        if (mOn) mB->ProfExit(mSec, mT0, mSnap);
    }

  private:
    Backend* mB;
    int mSec;
    bool mOn;
    uint64_t mT0 = 0, mSnap = 0;
};

class Interp {
  public:
    explicit Interp(Backend* backend);
    ~Interp();

    void RunFrame(GdxGfxJob* job, const GdxFrameConfig& cfg, int variant);

    const Counters& Stats() const {
        return mStats;
    }
    void ResetStats();
    bool UnknownLoggedOnce(uint8_t op);

    /* Per-opcode call-out for other native tracks (machines: G_DL registered-pointer hook,
     * plan 07 §3.1). fn returns 1 to consume the command (the walker skips the sub-list). */
    typedef int (*DlHook)(void* user, const void* listPtr, Interp* interp);
    void RegisterDlHook(DlHook fn, void* user) {
        mDlHook = fn;
        mDlHookUser = user;
    }
    /* G_NOOP / G_SPNOOP call-out (track + fx marker replay, plan 08 §3.7 / 09 §3.1): the
     * command's two words exactly as the LUS G_NOOP handler receives them. */
    typedef void (*NoopHook)(void* user, uint32_t w0, uint32_t w1, Interp* interp);
    void RegisterNoopHook(NoopHook fn, void* user) {
        mNoopHook = fn;
        mNoopHookUser = user;
    }

    /* --- the native replay seam (gdx_seam_owned.cpp) --- */
    /* G_SETCOMBINE from its raw words (the OP_SETCOMBINE decode). */
    void SetCombineWords(uint32_t w0, uint32_t w1);
    /* G_VTX from an already-resolved source (host-endian unless isBig). */
    void TransformVertices(const uint8_t* src, size_t n, size_t dest, bool isBig);
    /* The full triangle path (gdx_tri.cpp). */
    void SpTri(uint8_t v1, uint8_t v2, uint8_t v3, bool isRect);
    /* SpTri's state block alone: bind the backend for the current registers, no emission. */
    bool ResolveDrawState(bool isRect, DrawState& out);
    size_t PendingTris() const {
        return mBufNumTris;
    }
    friend class ::GdxOwnedSeam;

    Rsp& RspState() {
        return mRsp;
    }
    Rdp& RdpState() {
        return mRdp;
    }
    Backend* GetBackend() {
        return mBackend;
    }
    int Variant() const {
        return mVariant;
    }

    /* [nverify] both interpreters share one backend in verify mode; each pass must forget what it
     * believes the backend holds (shader, depth/decal/alpha, bound textures, samplers). */
    void ResetRenderStateCache();
    /* [nverify] re-apply this interpreter's cached backend state (the other pass changed it). */
    void ResyncBackend();
    /* [nverify] end-of-task state words, same layout as gdx_gfx_lus_state. */
    unsigned StateSnapshot(uint32_t* out, unsigned cap) const;

    /* --- texture-cache invalidation (gdx_texcache.cpp) ---
     * The owned cache is a second cache beside the LUS one, so the queued requests the game
     * raises (gdx_rdram_mode_reset's arena-rewind clear, minimap.c's per-race address
     * invalidate) must reach it too. Render thread only, like every other cache mutation. */
    void TexCacheClear();
    void TexCacheDeleteAddress(const uint8_t* addr);

    /* [native] texhash_ci: fold the decoded source span (and, for CI, the live palette bytes)
     * into the key for the CI/RGBA32 classes. 0 restores the identity-only key. */
    void SetCiContentHash(bool on) {
        mHashCiRgba32 = on;
    }

    /* Forcing knobs for DROPSITE 1, geomloss's vbopoolkib for this site. Both default to 0 =
     * shipped behaviour.
     *   [debug] texscratchkib=N    -- start the decode scratch at N KiB instead of 256, so an
     *                                 ordinary texture exercises the GROWTH path.
     *   [debug] texscratchmaxkib=N -- cap the growth ceiling at N KiB, so an ordinary texture
     *                                 exercises the OVERFLOW + neutral-fallback path. Setting
     *                                 both to the same N reproduces a pre-fix hard-ceiling
     *                                 scratch of exactly that size (256 = the shipped pre-fix). */
    void SetTexScratchKib(uint32_t startKib, uint32_t maxKib) {
        SetTexScratchBytes(startKib * 1024u, maxKib * 1024u);
    }
    /* The same knob in bytes, so the host tests can straddle an 8x8 decode (256 B) without
     * building a megabyte display list. */
    void SetTexScratchBytes(uint32_t startBytes, uint32_t maxBytes) {
        mTexUploadMax = maxBytes;
        if (startBytes == 0) {
            return;
        }
        const uint32_t want = startBytes;
        if (want == mTexUploadCap) {
            return;
        }
        uint8_t* p = new (std::nothrow) uint8_t[want];
        if (p == nullptr) {
            return;
        }
        delete[] mTexUpload;
        mTexUpload = p;
        mTexUploadCap = want;
    }
    /* [debug] vtxmax -- shrink the effective G_VTX window below kMaxVertices. Test knob only;
     * the storage is unchanged, so nothing can overrun. 0 = shipped (kMaxVertices). */
    void SetVtxSoftCap(uint32_t n) {
        mVtxSoftCap = (n == 0 || n > static_cast<uint32_t>(kMaxVertices)) ? static_cast<uint32_t>(kMaxVertices) : n;
    }
    /* [native] vtxclamp -- 0 restores the pre-fix behaviour: an over-range G_VTX loads nothing
     * at all. A/B only. */
    void SetVtxClamp(bool on) {
        mVtxClamp = on;
    }
    /* [debug] clippolymax -- shrink the clip-polygon cap below 16 to force the truncation arm.
     * 0 = shipped. Never raised above 16: that is the buffer's real size. */
    void SetClipPolyMax(uint32_t n) {
        mClipPolyMax = (n < 3u || n > 16u) ? 16u : n;
    }
    /* [native] texstalefix -- 0 restores the pre-fix behaviour: an import that uploaded nothing
     * is still marked uploaded, so the id keeps the previous texture's pixels. A/B only. */
    void SetTexStaleFix(bool on) {
        mTexStaleFix = on;
    }

    /* DROPSITES: session totals for the drop counters, which the windowed `Counters` cannot
     * carry. Measured: a forced growth run put `texgrow>0` in 2 of 91 receipt windows -- a
     * per-window counter that reads 0 in the other 89 is how `texDecodeOverflow` stayed
     * invisible in the first place, so every drop also gets a number that never resets. */
    struct SessionDrops {
        uint32_t texOvf, texGrow, texNoUp, texFb, vtxOver, vtxDropped, clipTrunc;
    };
    SessionDrops Drops() const {
        SessionDrops d = mSesDrops;
        d.texOvf += mStats.texDecodeOverflow;
        d.texGrow += mStats.texDecodeGrow;
        d.texNoUp += mStats.texNoUpload;
        d.texFb += mStats.texFallback;
        d.vtxOver += mStats.vtxOver;
        d.vtxDropped += mStats.vtxOverDropped;
        d.clipTrunc += mStats.clipTrunc;
        return d;
    }

    uint32_t TexDecodeHwBytes() const {
        return mTexDecodeHwBytes;
    }
    uint32_t ClipPolyHw() const {
        return mClipPolyHw;
    }
    uint32_t VtxHwEnd() const {
        return mVtxHwEnd;
    }
    uint32_t TexScratchCap() const {
        return mTexUploadCap;
    }
    unsigned TexLive() const {
        return mTexCount;
    }
    unsigned TexLiveHw() const {
        return mTexCountHw;
    }

    /* Public for the emission helpers in gdx_tri.cpp. */
    void Flush(FlushReason why);
    LoadedTexture LoadedTexAt(uint16_t word) const;

  private:
    struct Frame {
        ListRef list;
        size_t idx;
    };

    void SpReset();
    void Walk(const ListRef& root);
    void WalkCensus(); /* [native] dspcensus: Walk with the per-opcode/per-phase clocks */
    bool RemapF3d(Cmd& c, const ListRef& list, size_t* f3dXferSize);
    int Exec(const Cmd& c, Frame& f, size_t f3dXferSize);
    bool PushList(const ListRef& list);

    /* --- gdx_rsp.cpp --- */
    void SpMatrix(uint8_t params, const Cmd& c, const ListRef& list);
    void SpPopMatrix(uint32_t count);
    void SpVertex(size_t n, size_t dest, const Cmd& c, const ListRef& list);
    void SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val);
    void SpMovemem(uint8_t index, uint8_t offset, const Cmd& c, size_t xferSize, const ListRef& list);
    void SpDmaIo(const Cmd& c);
    void SpLine3d(uint8_t v1, uint8_t v2, uint8_t halfWidth);
    void CalculateNormalDir(const Light* light, float coeffs[3]);
    float AdjXForAspectRatio(float x) const;

    /* --- gdx_interp.cpp (pure state) --- */
    void SpGeometryMode(uint32_t clear, uint32_t set);
    void SpMoveword(uint8_t index, uint16_t offset, const Cmd& c);
    void SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on);
    void SpSetOtherMode(uint32_t shift, uint32_t numBits, uint64_t mode);
    void DpSetOtherMode(uint32_t h, uint32_t l);
    void DpSetTextureImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c);
    void DpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb2, uint32_t alpha2);
    void DpSetEnvColor(Rgba c);
    void DpSetPrimColor(uint8_t m, uint8_t l, Rgba c);
    void DpSetFogColor(Rgba c);
    void DpSetBlendColor(Rgba c);
    void DpSetFillColor(uint32_t packed);
    void DpSetPrimDepth(uint16_t z);
    void DpSetColorImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c);
    void DpSetZImage(const Cmd& c);

    /* --- gdx_tmem.cpp --- */
    void DpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                   uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts);
    void DpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
    void DpLoadTlut(uint8_t tile, uint32_t highIndex);
    void DpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
    void DpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    bool ResolveTextureSource(size_t requiredBytes);
    void StoreLoadedTexture(uint16_t start, const LoadedTexture& t);
    void NoteTmemLoadKind(uint16_t base, uint8_t kind, uint32_t writeEndByte);
    /* Everything SameContentLoad tests EXCEPT the source bytes: the live span's geometry and the
       full LoadedTexture descriptor. Split out so the immutable gate and the [native] tmemskip
       staleness gate are two different answers to "are the SOURCE BYTES the same", asked over
       one shared, unchanged definition of "is everything else the same". */
    bool SameContentDescriptor(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const;

    /* [memotear] Fault injection for the tmemskip refusal arm: called on a load that HAS been
       admitted and whose stamp/class both still match, i.e. the load that is about to skip.
       Returns true when it has forced the world stale (and says which way through `outKind`),
       having applied the fault to the state the gate is about to read. */
    bool TmemInjectStaleness(uint16_t base, unsigned cls, uintptr_t src, uint32_t readBytes);
    bool SameContentLoad(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const;
    /* [native] tmemskip: decide the load, stamp it, and keep the receipt. Returns true when the
       mirror + span store can be skipped outright; otherwise fills `outStamp`/`outCls` with the
       stamp to record once the mirror has been written. */
    bool TmemLoadIsRedundant(uint16_t base, uint8_t kind, const LoadedTexture& fresh, uint32_t readBytes,
                             uint64_t outStamp[2], unsigned* outCls);
    void RecordTmemSrcStamp(uint16_t base, const uint64_t stamp[2], unsigned cls);
    void NoteDescriptorMiss(uint16_t base, uint8_t kind, const LoadedTexture& fresh);
    void MirrorToTmem(uint8_t* dst, const uint8_t* src, uint32_t n, bool nativeRgba16);
    void ArmTexturesChanged();

    /* --- gdx_texcache.cpp --- */
    void ImportTexture(int unit, int tile);
    bool TexCacheLookup(int unit, const TexKey& key);
    void DecodeRgba16(int unit, int tile, const LoadedTexture& lt);
    void DecodeRgba32(int tile, const LoadedTexture& lt);
    void DecodeIA4(int tile, const LoadedTexture& lt);
    void DecodeIA8(int tile, const LoadedTexture& lt);
    void DecodeIA16(int tile, const LoadedTexture& lt);
    void DecodeI4(int tile, const LoadedTexture& lt);
    void DecodeI8(int tile, const LoadedTexture& lt);
    void DecodeCi4(int tile, const LoadedTexture& lt);
    void DecodeCi8(int tile, const LoadedTexture& lt);
    bool UploadPrepare(uint32_t w, uint32_t h);
    void UploadNeutralFallback(int unit);
    void Upload(uint32_t w, uint32_t h);
    void TexCacheRetireSlot(int16_t slot);

    /* --- gdx_tri.cpp --- */
    void* LookupOrCreateShader(uint64_t id0, uint64_t id1);
    /* True when a VALUE change of prim (env) colour can ride per vertex in the OPEN packed
     * batch, i.e. no TexEnv stage constant of its shader reads that register. Mirrors the LUS
     * GdxHudRegFoldable ([native] fx_hud) over the same backend contract. */
    bool RegFoldable(bool isPrim) const;
    void NoteRegChange(bool isPrim, FlushReason why);

    /* --- gdx_rect.cpp --- */
    void DpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
    void CalcAndSetViewport(const int16_t* vp /* vscale[4], vtrans[4] */);
    void AdjustViewportOrScissor(Rect* area);
    void DrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
    void DpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                            int16_t ult, int16_t dsdx, int16_t dtdy, bool flip);
    void DpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);

    Backend* mBackend;
    DlSrc mSrc;
    Rsp mRsp;
    Rdp mRdp;
    RenderState mRs;
    Counters mStats;
    GdxFrameConfig mCfg;
    int mVariant = 0;
    bool mLineSection = false;
    /* [nverify] latched once per task in RunFrame (the glue arms/disarms verify immediately
     * around that call). The per-command and per-triangle diagnostics in gdx_dl.cpp /
     * gdx_tri.cpp are gated on this bool, so they cost one predictable branch when
     * [native] interp_verify=0. Audit 2026-09-23 finding 31. */
    bool mVerifyActive = false;

    ListRef mBranchZTarget;
    bool mBranchZValid = false;

    Frame mStack[kCallStack];
    int mSp = 0;

    /* Combiner pool + texture cache. */
    CombinerPool mCcPool;
    TexEntry mTex[kTexCacheSize];
    int16_t mTexHash[kTexCacheHash];
    int16_t mTexLruHead = -1, mTexLruTail = -1;
    unsigned mTexCount = 0;
    uint32_t mTexFreeIds[kTexCacheSize];
    unsigned mTexFreeIdCount = 0;
    bool mHashCiRgba32 = true;
    uint8_t* mTexUpload = nullptr;
    uint32_t mTexUploadCap = 0;   /* bytes mTexUpload can currently hold */
    uint32_t mTexUploadMax = 0;   /* [debug] texscratchmaxkib; 0 = kTexUploadMaxBytes */
    bool mUploadDone = false;     /* did this import actually reach Upload()? */
    uint8_t mFallbackTexels[kFallbackTexBytes]; /* opaque white, DROPSITE 1's neutral upload */
    uint8_t mDropsiteTexLogged = 0;  /* one-shot DROPSITE line for the decode ceiling */
    uint8_t mDropsiteVtxLogged = 0;  /* one-shot DROPSITE line for an over-range G_VTX */
    uint8_t mDropsiteClipLogged = 0; /* one-shot DROPSITE line for a truncated clip polygon */
    uint8_t mDropsiteStaleLogged = 0;/* one-shot DROPSITE line for an import that uploaded nothing */
    /* Session high-water marks -- deliberately NOT in Counters, which ResetStats() clears every
     * receipt window. A drop site's margin is only meaningful over the whole run. */
    uint32_t mTexDecodeHwBytes = 0; /* largest decode the run ever asked the scratch for */
    uint32_t mClipPolyHw = 0;       /* largest clip-polygon vertex count the run ever produced */
    uint32_t mVtxHwEnd = 0;         /* largest dest_index + n_vertices a G_VTX ever asked for */

    /* Batch emission state ([triloop] packed VBO). */
    size_t mBufNumTris = 0;
    float* mPackDst = nullptr;
    bool mPackBatch = false;
    /* GEOMLOSS: triangles this batch may hold before it must be flushed. kMaxTriBuffer in
     * the normal case; lower when the backend's pool cannot hold a whole batch, which is
     * where the old code returned early and lost the geometry outright. */
    size_t mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
    Gdx3dsVboPackParams mPackParams;
    Gdx3dsVboPackAux mPackAux;
    uint32_t mClipFanDepth = 0;
    bool mTrectRectAtlasOk = false;
    bool mTrectBatchOn = false;
    float mByteToUnit[256];

    /* The tri memo: valid from a resolved state until the batch it belongs to is flushed. */
    DrawState mTriMemo;
    bool mTriMemoValid = false;
    /* The combiner the OPEN packed batch was begun under: the fold key (== LUS sGdxPackComb). */
    const Combiner* mPackComb = nullptr;
    /* A batch that absorbed a prim/env fold must not take the [trectbatch] deferred import
     * (== LUS sGdxPackFolded); see fx-progress.md "Frame 1418". */
    bool mPackFolded = false;
    int mCensus = 0;  /* [native] ncensus */
    int mOwnFold = 0; /* [native] ownfold */
    int mDspCensus = 0; /* [native] dspcensus: 0 off, 1 per-opcode self time, 2 + walk phases */
    uint32_t mVtxSoftCap = static_cast<uint32_t>(kMaxVertices); /* [debug] vtxmax */
    bool mVtxClamp = true;                                      /* [native] vtxclamp */
    bool mTexStaleFix = true;                                   /* [native] texstalefix */
    uint32_t mClipPolyMax = 16;                                 /* [debug] clippolymax */
    unsigned mTexCountHw = 0;                                   /* B23: LRU occupancy high-water */
    SessionDrops mSesDrops = {};                                /* never reset by ResetStats */
    int mTimgMemo = 0;  /* [native] timgmemo: cross-frame SETTIMG classification memo (B4/F3) */
    uint32_t mTimgEpochLast = 0; /* [native] timgmemo receipt: epoch-churn detector */
    /* [native] tmemskip -- widen the same-content skip past the o2r resource class, using the
     * generation/DMA staleness test the bridge already runs (gdx_dlsrc_bridge.h). 0 off,
     * 1 the two classes that are stable WITHOUT a compare (native-RGBA16 by generation, and
     * registered ROM-backed host ranges), 2 additionally the DMA-tracked class (RDRAM and
     * framebuffer shadows), whose generation does NOT see a plain CPU store into RDRAM.
     * Default 0. The invalidation argument is docs/research/native/tmemblock-progress.md S2. */
    int mTmemSkip = 0;
    int mTmemCls = 0; /* [native] tmemcls: class census only; changes no behaviour */
    /* [memotear] [debug] tmemfault -- fault injection for the refusal arm. 0 off (default and
     * the only value the shipped build ever resolves); N forces staleness on every Nth load
     * that would otherwise have skipped. Inert unless mTmemSkip != 0. See
     * docs/research/native/memotear-progress.md S7. */
    int mTmemFault = 0;
    uint32_t mTmemFaultTick = 0;
    uint32_t mBatchesAtTaskStart = 0;

    /* [native] timgmemo -- the cross-frame SETTIMG classification memo.
     *
     * WHAT: ResolveTextureSource derives three answers per G_LOADBLOCK/LOADTILE/LOADTLUT --
     * the resolved pointer, TexelsAreNativeRgba16 (a linear scan of gNativeRgba16Ranges) and
     * TimgIsResource (a binary search of the generated asset table) -- for a SETTIMG operand
     * that usually did not change since the last frame. dispatch-progress.md measured that at
     * timgres 1.43 + timgclass 0.93 = 2.36 ms/task, the largest removable block in the census.
     *
     * WHY IT IS AN Interp MEMBER, not a process-wide table next to the bridge's gResolveMemo:
     * the owned interpreter is walked by ONE thread (sInterp is constructed once in
     * gdx_ninterp_job_run and only ever entered from there), so this memo is single-owner BY
     * CONSTRUCTION. That is the same conclusion resolverace reached for the bridge's memo
     * family the expensive way -- and it means this table needs no GdxMemoOwnedHere() latch,
     * no release/acquire pair, and cannot be the seventh member of that bug class.
     *
     * KEY: (resolved pointer, w1, w1hi, requiredBytes) + the bridge's classEpoch. The resolved
     * pointer is an INPUT here, not a cached output -- the memo covers the classification only,
     * and ResolveTimg still runs. That is what makes the key honest AND cross-frame: given p,
     * both classifications are pure functions of the key plus exactly two counted tables
     * (gNativeRgba16Generation and the venue base) and read no segment state at all.
     *
     * Memoising the POINTER too was tried first and measured: it has to be keyed on the
     * adapter's mSegGen, and mSegGen folds gSegments[1], the double-buffered GfxPool the game
     * rewrites every frame, so the epoch turned over once per task and the hit rate was 3.7 %
     * (hit=256 miss=6592 per 64-task window). The bridge's own gWideStubMemo has the same key
     * and the same result -- 0 hits, 6592 misses. See loadblock-progress.md S2.
     *
     * Direct-mapped, no eviction policy: a collision is a miss, which is correct, just slower.
     * ~128 distinct operands per crowd task against 256 slots. */
    struct TimgClassEntry {
        uint32_t w1;
        uint32_t w1hi;
        uint32_t required;
        uint32_t epoch;
        uintptr_t addr;
        uint8_t native_rgba16;
        uint8_t immutable;
        uint8_t filled;
    };
    static constexpr unsigned kTimgMemoSlots = 256; /* power of two: the index is a mask */
    TimgClassEntry mTimgClassMemo[kTimgMemoSlots];

    DlHook mDlHook = nullptr;
    void* mDlHookUser = nullptr;
    NoopHook mNoopHook = nullptr;
    void* mNoopHookUser = nullptr;
    uint32_t mUnknownLogged[8];
};

} // namespace gdxn

#endif /* GDX_INTERP_H */
