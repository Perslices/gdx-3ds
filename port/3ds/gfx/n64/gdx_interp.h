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
constexpr uint32_t kTexUploadBytes = 256u * 1024u; /* decode scratch (plan deviation: LUS has 4 MiB) */

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

enum FlushReason : uint8_t {
    FLUSH_NONE = 0,
    FLUSH_SHADER,
    FLUSH_TEXTURE,
    FLUSH_DEPTH,
    FLUSH_DECAL,
    FLUSH_VIEWPORT,
    FLUSH_SCISSOR,
    FLUSH_ALPHA,
    FLUSH_CONST,
    FLUSH_FBOP,
    FLUSH_FULL,
    FLUSH_TASK_END,
    FLUSH_FOG,
    FLUSH_COMBINER,
    FLUSH_SAMPLER,
    FLUSH_HOOK, /* a native replay hook (track/fx marker, machines G_DL) drained the batch */
    FLUSH_COUNT
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
    uint32_t flush[FLUSH_COUNT];
    uint32_t unknownOps;
    uint32_t droppedOps;
    uint32_t stackOverflow;
    uint32_t runaway;
    uint32_t noopLists;
    uint32_t branchZTaken;
    uint32_t tasks;
    uint32_t rootRejects;
    uint32_t texImports, texHits, texMisses, texEvicts, texDecodeOverflow;
    uint32_t tmemLoads, tmemSkips, timgUnresolved;
    uint32_t rects;
    uint32_t vtxUnresolved, mtxUnresolved;
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
    bool SameContentLoad(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const;
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
    void Upload(uint32_t w, uint32_t h);

    /* --- gdx_tri.cpp --- */
    void* LookupOrCreateShader(uint64_t id0, uint64_t id1);

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
    uint8_t* mTexUpload = nullptr;

    /* Batch emission state ([triloop] packed VBO). */
    size_t mBufNumTris = 0;
    float* mPackDst = nullptr;
    bool mPackBatch = false;
    Gdx3dsVboPackParams mPackParams;
    Gdx3dsVboPackAux mPackAux;
    uint32_t mClipFanDepth = 0;
    bool mTrectRectAtlasOk = false;
    bool mTrectBatchOn = false;
    float mByteToUnit[256];

    /* The tri memo: valid from a resolved state until the batch it belongs to is flushed. */
    DrawState mTriMemo;
    bool mTriMemoValid = false;

    DlHook mDlHook = nullptr;
    void* mDlHookUser = nullptr;
    NoopHook mNoopHook = nullptr;
    void* mNoopHookUser = nullptr;
    uint32_t mUnknownLogged[8];
};

} // namespace gdxn

#endif /* GDX_INTERP_H */
