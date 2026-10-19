/* port/3ds/gfx/n64/gdx_nrr_fx.cpp -- [native] nrr_shd / nrr_fx: machine shadows, boost flames and
 * effects as NRR categories (docs/research/native/n60-design.md §4.5, n60fx-progress.md). Render
 * side. Contract: gfx/nrr/gdx3ds_nrr_fx.h.
 *
 * One small engine for the three: a category's draw is a sequence of sprite triangles, each with a
 * learned block (kind) and the per-sprite registers the stock stream would have set (prim, env).
 *
 *   LEARN (G_NOOP p=0x66, cold). The stock state commands of a draw point run through the
 *     interpreter with a dummy triangle; LEARN_END banks what the interpreter resolved for it:
 *     the core NrrStateKey (shader ids incl. the clamp bits, the texture-cache entry, sampler,
 *     depth/decal/blend), the vertex packing as a linear UV map probed from HudPackVerts, where
 *     the vertex colour and alpha come from (constant, shade, prim, env), every combiner input's
 *     source (the stage constants: learned value, or prim / env / the batch's vertex-0 shade),
 *     the cull mode. The dummy batch is discarded.
 *
 *   HOT (core p=0x63). Transform each sprite's vertices with its MP (x by the interpreter's
 *     aspect factor, TransformVertices' expressions), trivially reject / cull as SpTri does,
 *     pack through the block's map into the core ring, one core draw per run of sprites that
 *     share the block and the per-draw stage constants. The PICA clips.
 *
 * Exactness: none claimed (n60-design §7.3 tolerance). Painter's order is the stock order. */
#include <stdio.h>
#include <string.h>

#include <new>

#include "gdx_interp.h"
#include "gdx_log.h"
#include "gdx3ds_hotcold.h"
#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_fx.h"
#include "nrr/gdx3ds_nrr_internal.h" /* gdx3ds_nrr_bk_tex: a learned block's texture, copied once */

#ifdef __3DS__
#include <3ds.h>
#include <citro3d.h>
extern "C" void gdx3ds_mesh_backend_flush_resident(const void* p, uint32_t bytes);
#endif

extern "C" {
volatile uint8_t gdx3ds_nrr_fx_ready[GDX_NRRFX_CATS][GDX_NRRFX_KINDS];
volatile uint8_t gdx3ds_nrr_fx_broken[GDX_NRRFX_CATS];
void gdx3ds_nrr_shd_game_counts(unsigned long out[3]) __attribute__((weak));
void gdx3ds_nrr_fx_game_counts(unsigned long out[3]) __attribute__((weak));
void gdx3ds_nrr_dress_game_counts(unsigned long out[3]) __attribute__((weak));
}

namespace gdxn {

namespace nrrfx {

constexpr size_t kStride = GDX3DS_VBOPACK_STRIDE;

/* Where a packed colour channel or a combiner input's value comes from. */
enum Src : uint8_t {
    SRC_CONST = 0, /* the learned value */
    SRC_SHADE,     /* the vertex's colour (rgb) / alpha */
    SRC_SHADE_A,   /* rgb = the vertex's alpha */
    SRC_PRIM,
    SRC_PRIM_A,    /* rgb = prim alpha */
    SRC_ENV,
    SRC_ENV_A,     /* rgb = env alpha */
    SRC_FOG,       /* alpha = the vertex's fog factor (fogFactorToAlpha: the backend's fog stage) */
};

/* One draw point's vertex packing, linear in the stock inputs (n60sky's SkyPack): uv = min(U * mul
 * + add, clamp) + off per axis, U = (short)(tc * scale >> 16); the colour channels by source. */
struct FxPack {
    /* HudPackVerts' unit-0 sequence per axis, operation for operation: t = U / 32 * pre - tile
     * + half; t /= size (a power of two: == its reciprocal multiply); min(t * scale, clamp) + off.
     * Untextured: pre = 0, size 1, scale 0, off 0 (uv 0). */
    float uPre, vPre, uTile, vTile, half, uSize, vSize, uScale, vScale;
    float uClamp, vClamp, uOff, vOff;
    float yMul;                   /* -1 when the clip space inverts y */
    float zMul, zW;               /* z' = z * zMul + w * zW: (z + w) / 2 for a 0..1 depth range */
    float rgb[3], a;
    uint16_t scaleS, scaleT;
    uint8_t rgbSrc, aSrc;
    uint8_t textured, fog;
};

struct FxBlock {
    uint8_t usable;
    uint8_t rejVariant; /* F3DFLX2 "Rej" (interpreter variant 1/2): a vertex out of the 2x box drops the tri */
    uint8_t cullBack, cullFront, cullInvert;
    uint8_t nInputs;
    uint8_t dynInputs; /* some stage constant follows prim / env / the batch's vertex-0 shade */
    uint8_t inSrc[7][2];
    int16_t slot; /* the interpreter's texture-cache entry (-1 untextured), its id and source */
    uint32_t texId;
    const uint8_t* texAddr;
    NrrStateKey key;
    const NrrState* st;
    uint32_t stGen;
    float inputs[7][4];
    uint8_t prim0[4], env0[4]; /* the prim / env registers at the learn point (the setup list's) */
    FxPack pack;
};

/* Per category: the learned blocks and the receipt window. */
struct FxCat {
    FxBlock blocks[GDX_NRRFX_KINDS];
    /* each block's core state is built into memory of its own (NrrStateBuildInto), not the core's
     * shared 128-entry table: 54 blocks there filled it on a second course and starved the other
     * categories ([nrr] ... full=). Outside FxBlock so a relearn (memset) keeps it. */
    void* stMem[GDX_NRRFX_KINDS];
    const void* ownTex[GDX_NRRFX_KINDS]; /* the block's texture copy (NRR-owned), null = the cache's */
    uint32_t wFrames, wEnts, wDraws, wTris, wRej, wCull, wLearn, wLearnFail, wStale, wFallback;
    uint32_t learnLogs;
};

struct FxRender {
    FxCat cat[GDX_NRRFX_CATS];
    Interp* interp = nullptr; /* the walker that learned the blocks */
    int learnCat = -1;
    int learnKind = -1;
    float xs[2] = { 1.0f, 1.0f }; /* TransformVertices' x correction: x * xs[0] / xs[1] */
    int16_t scissor[4] = { 0, 0, 0, 0 };
    int16_t fogMul = 0, fogOff = 0; /* the walker's fog position and colour at the marker */
    float fogRgb[3] = { 0.0f, 0.0f, 0.0f };
};

FxRender* sFx = nullptr;
float sByteToUnit[256];

/* A run's packed colour, resolved once per run from the block's sources and the run's registers:
 * rgb = the constant, the vertex's shade (1) or its alpha (2); alpha = the constant, the vertex's
 * alpha (1) or its fog factor (2). */
struct FxColor {
    float rgb[3];
    float a;
    uint8_t rgbMode, aMode;
};

/* A run of sprites drawn as one core draw: the block and the per-draw registers. */
struct FxBatch {
    NrrCtx* ctx;
    FxCat* cat;
    FxBlock* b;
    float* base;  /* ring memory of the frame's draws */
    uint32_t cap; /* vertices */
    uint32_t start, cur;
    uint8_t regs[12]; /* the run's vertex-0 shade [0..3], prim [4..7], env [8..11] (kSrcIdx) */
    FxColor col;
    /* the shadow atlas: uv' = uv * ru + rx (per axis) maps a decal's own texture into its slot */
    uint8_t remap;
    float ru, rx, rv, ry;
    float uLo, uHi, vLo, vHi; /* the slot's margin (atlas uv): a vertex never samples a neighbour */
};

/* Source -> byte of FxBatch::regs per channel (r, g, b, a); SRC_CONST / SRC_FOG never index it. */
const uint8_t kSrcIdx[8][4] = {
    { 0, 0, 0, 0 }, { 0, 1, 2, 3 }, { 3, 3, 3, 3 }, { 4, 5, 6, 7 },
    { 7, 7, 7, 7 }, { 8, 9, 10, 11 }, { 11, 11, 11, 11 }, { 0, 0, 0, 0 },
};

GDX_COLD bool FxEnsure() {
    if (sFx == nullptr) {
        sFx = new (std::nothrow) FxRender();
        if (sFx != nullptr) {
            memset(sFx->cat, 0, sizeof(sFx->cat));
        }
    }
    return sFx != nullptr;
}

int FxCatIndex(uint32_t cat) {
    if (cat == NRR_CAT_SHD) {
        return 0;
    }
    if (cat == NRR_CAT_FX) {
        return 1;
    }
    if (cat == NRR_CAT_DRESS) {
        return 2;
    }
    return -1;
}

/* Combiner mux -> source, colour channel (GdxInput's k == 0 switch). */
uint8_t FxSrcRgb(uint8_t mux) {
    switch (mux) {
        case CCMUX_SHADE: return SRC_SHADE;
        case CCMUX_SHADE_ALPHA: return SRC_SHADE_A;
        case CCMUX_PRIMITIVE: return SRC_PRIM;
        case CCMUX_PRIMITIVE_ALPHA: return SRC_PRIM_A;
        case CCMUX_ENVIRONMENT: return SRC_ENV;
        case CCMUX_ENV_ALPHA: return SRC_ENV_A;
        default: return SRC_CONST;
    }
}

/* Combiner mux -> source, alpha channel (GdxInput's k == 1 switch: the *_ALPHA muxes give 0). */
uint8_t FxSrcAlpha(uint8_t mux) {
    switch (mux) {
        case CCMUX_SHADE: return SRC_SHADE;
        case CCMUX_SHADE_ALPHA: return SRC_SHADE;
        case CCMUX_PRIMITIVE: return SRC_PRIM;
        case CCMUX_ENVIRONMENT: return SRC_ENV;
        default: return SRC_CONST;
    }
}

/* The block's core state for the frame's scissor (a new scissor or a core flush: cold rebuild),
 * built into the block's own memory. */
GDX_COLD const NrrState* FxStateFor(FxBlock& b, void*& mem, const int16_t scissor[4]) {
    memcpy(b.key.scissor, scissor, sizeof(b.key.scissor));
    if (mem == nullptr) {
        mem = ::operator new(NrrStateBytes(), std::nothrow);
    }
    b.st = mem != nullptr ? NrrStateBuildInto(&b.key, mem) : nullptr;
    b.stGen = NrrStateGeneration();
    if (b.st != nullptr && b.pack.fog && NrrStateFogStage(b.st) < 0) {
        b.st = nullptr; /* fogged through the PICA fog unit, which the core does not drive */
    }
    return b.st;
}

/* NRR-owned texture copies (the n60mach pattern): a learned block draws from its own copy, so the
 * texture cache retiring the entry costs a relearn, not a frame without the sprite. Deduplicated
 * by size, format and a content signature (a texture reloaded with the same bytes reuses its copy);
 * never freed (bounded by the distinct small textures the categories draw). */
struct FxOwnTex {
    uint32_t hash, size, param, dim;
    int fmt;
    const void* tex; /* C3D_Tex */
};
constexpr int kOwnTexMax = 96;
FxOwnTex sOwnTex[kOwnTexMax];
int sOwnTexN = 0;
uint32_t sOwnTexBytes = 0;

#ifdef __3DS__
uint32_t FxTexSignature(const C3D_Tex* t) {
    const uint8_t* d = static_cast<const uint8_t*>(t->data);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < t->size; i++) {
        h = (h ^ d[i]) * 16777619u;
    }
    return h;
}

/* A small standalone texture (not an atlas view) copied into linear memory; null = not copied. */
GDX_COLD const void* FxOwnTexture(uint32_t texId) {
    NrrBkTex bk;
    static int sLogs = 0;
    const int got = gdx3ds_nrr_bk_tex(texId, &bk);
    const C3D_Tex* src = got ? static_cast<const C3D_Tex*>(bk.tex) : nullptr;
    if (!got || src == nullptr || bk.uOff != 0.0f || bk.vOff != 0.0f || src->data == nullptr || src->size == 0 ||
        src->size > 16384u) {
        if (sLogs < 16) {
            sLogs++;
            GDXN_LOG("[nrrfx] owntex id=%u not copied: got=%d tex=%p off=%g,%g size=%lu", texId, got,
                     static_cast<const void*>(src), got ? bk.uOff : 0.0f, got ? bk.vOff : 0.0f,
                     src != nullptr ? static_cast<unsigned long>(src->size) : 0ul);
        }
        return nullptr; /* an atlas view or a large texture: the block keeps the cache's */
    }
    const uint32_t h = FxTexSignature(src);
    for (int i = 0; i < sOwnTexN; i++) {
        const FxOwnTex& t = sOwnTex[i];
        if (t.hash == h && t.size == src->size && t.param == src->param && t.dim == src->dim && t.fmt == src->fmt) {
            return t.tex;
        }
    }
    if (sOwnTexN >= kOwnTexMax) {
        return nullptr;
    }
    void* data = linearAlloc(src->size);
    C3D_Tex* copy = static_cast<C3D_Tex*>(::operator new(sizeof(C3D_Tex), std::nothrow));
    if (data == nullptr || copy == nullptr) {
        return nullptr;
    }
    memcpy(data, src->data, src->size);
    gdx3ds_mesh_backend_flush_resident(data, src->size);
    *copy = *src;
    copy->data = data;
    FxOwnTex& t = sOwnTex[sOwnTexN++];
    t.hash = h;
    t.size = src->size;
    t.param = src->param;
    t.dim = src->dim;
    t.fmt = src->fmt;
    t.tex = copy;
    sOwnTexBytes += src->size;
    return copy;
}
#else
GDX_COLD const void* FxOwnTexture(uint32_t) {
    return nullptr;
}
#endif

/* ---- the shadow atlas: every learned shadow texture in one A8 texture, one state block, one draw.
 * Each decal texture is 32x64 I4 sampled CLAMP_TO_EDGE (the backend's RGBA8 copy, alpha only: the
 * shadow combiner reads PRIM and TEXEL0 alpha); its slot is the texture plus an 8-texel margin of
 * replicated edge texels, so a coordinate up to 8 texels outside samples what CLAMP_TO_EDGE would. */
constexpr int kAtlasW = 256, kAtlasH = 512, kAtlasMargin = 8;
constexpr uint32_t kAtlasKind = GDX_NRRSHD_KINDS; /* the atlas block's slot in the shadow table */

struct ShdAtlas {
    ShdAtlas() {
        for (int k = 0; k < GDX_NRRSHD_KINDS; k++) {
            slotX[k] = slotY[k] = -1;
        }
    }
    const void* tex = nullptr; /* C3D_Tex, A8 */
    int16_t slotX[GDX_NRRSHD_KINDS];
    int16_t slotY[GDX_NRRSHD_KINDS];
    uint8_t texW = 0, texH = 0; /* the decal texture size every slot holds */
    int nextX = 0, nextY = 0, rowH = 0;
    bool keyed = false;         /* blocks[kAtlasKind] holds the shared state */
    uint32_t baked = 0;
    uint32_t logs = 0;
};
ShdAtlas sAtlas;

#ifdef __3DS__
/* The texel (x, y) (N64 orientation, y down) of a backend RGBA8 texture: alpha. */
uint8_t AtlasSrcAlpha(const C3D_Tex* t, int x, int y) {
    const uint32_t* d = static_cast<const uint32_t*>(t->data);
    const uint32_t dy = static_cast<uint32_t>(t->height - 1 - y);
    const uint32_t off = (((dy >> 3) * (static_cast<uint32_t>(t->width) >> 3) + (static_cast<uint32_t>(x) >> 3)) << 6) |
                         (dy & 1u ? 2u : 0u) | (dy & 2u ? 8u : 0u) | (dy & 4u ? 32u : 0u) | (x & 1 ? 1u : 0u) |
                         (x & 2 ? 4u : 0u) | (x & 4 ? 16u : 0u);
    return static_cast<uint8_t>(d[off] & 0xFFu);
}

void AtlasPut(C3D_Tex* a, int x, int y, uint8_t v) {
    uint8_t* d = static_cast<uint8_t*>(a->data);
    const uint32_t dy = static_cast<uint32_t>(kAtlasH - 1 - y);
    const uint32_t off = (((dy >> 3) * (kAtlasW >> 3) + (static_cast<uint32_t>(x) >> 3)) << 6) | (dy & 1u ? 2u : 0u) |
                         (dy & 2u ? 8u : 0u) | (dy & 4u ? 32u : 0u) | (x & 1 ? 1u : 0u) | (x & 2 ? 4u : 0u) |
                         (x & 4 ? 16u : 0u);
    d[off] = v;
}

GDX_COLD C3D_Tex* AtlasEnsure() {
    if (sAtlas.tex == nullptr) {
        C3D_Tex* t = static_cast<C3D_Tex*>(::operator new(sizeof(C3D_Tex), std::nothrow));
        if (t == nullptr || !C3D_TexInit(t, kAtlasW, kAtlasH, GPU_A8)) {
            return nullptr;
        }
        memset(t->data, 0, t->size);
        sAtlas.tex = t;
    }
    return static_cast<C3D_Tex*>(const_cast<void*>(sAtlas.tex));
}

/* Bake a learned shadow texture into a slot (cold, once per kind). */
GDX_COLD void AtlasBake(uint32_t kind, const C3D_Tex* src) {
    C3D_Tex* a = AtlasEnsure();
    if (a == nullptr || src == nullptr || src->fmt != GPU_RGBA8 || src->width > 64 || src->height > 64) {
        return;
    }
    if (sAtlas.texW == 0) {
        sAtlas.texW = static_cast<uint8_t>(src->width);
        sAtlas.texH = static_cast<uint8_t>(src->height);
    }
    if (src->width != sAtlas.texW || src->height != sAtlas.texH) {
        return;
    }
    const int sw = sAtlas.texW + 2 * kAtlasMargin, sh = sAtlas.texH + 2 * kAtlasMargin;
    if (sAtlas.nextX + sw > kAtlasW) {
        sAtlas.nextX = 0;
        sAtlas.nextY += sAtlas.rowH;
        sAtlas.rowH = 0;
    }
    if (sAtlas.nextY + sh > kAtlasH) {
        return; /* full: this kind keeps its own draw */
    }
    const int x0 = sAtlas.nextX, y0 = sAtlas.nextY;
    for (int y = 0; y < sh; y++) {
        int sy = y - kAtlasMargin;
        sy = sy < 0 ? 0 : (sy >= sAtlas.texH ? sAtlas.texH - 1 : sy);
        for (int x = 0; x < sw; x++) {
            int sx = x - kAtlasMargin;
            sx = sx < 0 ? 0 : (sx >= sAtlas.texW ? sAtlas.texW - 1 : sx);
            AtlasPut(a, x0 + x, y0 + y, AtlasSrcAlpha(src, sx, sy));
        }
    }
    gdx3ds_mesh_backend_flush_resident(a->data, a->size);
    sAtlas.slotX[kind] = static_cast<int16_t>(x0);
    sAtlas.slotY[kind] = static_cast<int16_t>(y0);
    sAtlas.nextX += sw;
    sAtlas.rowH = sh > sAtlas.rowH ? sh : sAtlas.rowH;
    sAtlas.baked++;
}
#else
GDX_COLD void AtlasBake(uint32_t, const void*) {}
#endif

/* Two shadow blocks draw alike apart from their texture (the atlas block's condition). */
bool AtlasSameState(const FxBlock& a, const FxBlock& b) {
    NrrStateKey ka = a.key, kb = b.key;
    ka.tex[0] = kb.tex[0] = NRR_TEX_NONE;
    ka.texObj[0] = kb.texObj[0] = nullptr;
    memset(ka.scissor, 0, sizeof(ka.scissor));
    memset(kb.scissor, 0, sizeof(kb.scissor));
    return memcmp(&ka, &kb, sizeof(ka)) == 0 && memcmp(&a.pack, &b.pack, sizeof(a.pack)) == 0 &&
           a.rejVariant == b.rejVariant && a.cullBack == b.cullBack && a.cullFront == b.cullFront &&
           a.cullInvert == b.cullInvert && a.nInputs == b.nInputs && a.dynInputs == 0 && b.dynInputs == 0;
}

/* A learned shadow kind joins the atlas: its texture baked into a slot, and the atlas block keyed on
 * the first one (the same state with the atlas texture). Kinds that differ keep their own draw. */
GDX_COLD void AtlasLog(uint32_t kind, const FxBlock& b, const void* own, const char* what) {
    if (sAtlas.logs < 24) {
        sAtlas.logs++;
        GDXN_LOG("[nrrfx] atlas kind=%u %s own=%p cms=%u cmt=%u slot=%d,%d keyed=%d baked=%u", kind, what, own,
                 b.key.texCms[0], b.key.texCmt[0], kind < GDX_NRRSHD_KINDS ? sAtlas.slotX[kind] : -1,
                 kind < GDX_NRRSHD_KINDS ? sAtlas.slotY[kind] : -1, sAtlas.keyed ? 1 : 0, sAtlas.baked);
    }
}

GDX_COLD void AtlasLearn(FxCat& C, uint32_t kind) {
    FxBlock& b = C.blocks[kind];
    AtlasLog(kind, b, C.ownTex[kind], "learn");
    /* only clamp-to-edge decals (the shader clamp of both axes, or the tile's clamp bits: the core
     * binds GPU_CLAMP_TO_EDGE for either): the margin reproduces exactly that */
    const bool clampS = (b.key.shaderId1 & SHOPT_TEXEL0_CLAMP_S) != 0 || (b.key.texCms[0] & 2u) != 0;
    const bool clampT = (b.key.shaderId1 & SHOPT_TEXEL0_CLAMP_T) != 0 || (b.key.texCmt[0] & 2u) != 0;
    if (kind >= GDX_NRRSHD_KINDS || !b.usable || C.ownTex[kind] == nullptr || !clampS || !clampT) {
        AtlasLog(kind, b, C.ownTex[kind], "not a clamped decal");
        return;
    }
    if (sAtlas.keyed && !AtlasSameState(C.blocks[kAtlasKind], b)) {
        AtlasLog(kind, b, C.ownTex[kind], "differs");
        return;
    }
    if (sAtlas.slotX[kind] < 0 || sAtlas.tex == nullptr) {
#ifdef __3DS__
        AtlasBake(kind, static_cast<const C3D_Tex*>(C.ownTex[kind]));
#else
        AtlasBake(kind, C.ownTex[kind]);
#endif
    }
    if (sAtlas.tex == nullptr || sAtlas.slotX[kind] < 0 || sAtlas.keyed) {
        return;
    }
    FxBlock& a = C.blocks[kAtlasKind];
    a = b;
    a.slot = -1; /* the atlas is NRR-owned: no cache entry to validate */
    a.key.tex[0] = NRR_TEX_NONE;
    a.key.texObj[0] = sAtlas.tex;
    a.st = nullptr;
    sAtlas.keyed = true;
}

/* The block's cache entry was retired: with its own texture copy it still draws this frame (and is
 * learned again on the next build, which refreshes the copy if the content changed); without one
 * it cannot draw. */
GDX_COLD bool FxRetired(FxCat& C, int ci, uint32_t kind) {
    C.wStale++;
    gdx3ds_nrr_fx_ready[ci][kind] = 0;
    if (C.blocks[kind].key.texObj[0] != nullptr) {
        C.blocks[kind].slot = -1; /* draw from the copy; stop checking the retired entry */
        return true;
    }
    C.blocks[kind].usable = 0;
    return false;
}

/* A block that cannot draw this frame: the game side learns it again on its next build. */
GDX_COLD void FxStale(FxCat& C, int ci, uint32_t kind) {
    C.wStale++;
    if (kind < GDX_NRRFX_KINDS) {
        gdx3ds_nrr_fx_ready[ci][kind] = 0;
        C.blocks[kind].usable = 0;
    }
}

} // namespace nrrfx

using namespace nrrfx;

/* ---- learning (cold): Interp members, they read the resolve's results --------------------- */

void Interp::NrrFxMarker(uint32_t w0, uint32_t w1) {
    if (!FxEnsure()) {
        return;
    }
    const uint32_t op = (w0 >> 8) & 0xFFu;
    const uint32_t cat = w0 & 0xFFu;
    if (op == GDX_NRRFX_OP_LEARN_BEGIN) {
        NrrFxLearnBegin(cat, w1 & 0xFFu);
    } else if (op == GDX_NRRFX_OP_LEARN_END) {
        NrrFxLearnEnd(cat, w1 & 0xFFu);
    }
}

GDX_COLD void Interp::NrrFxLearnBegin(uint32_t cat, uint32_t kind) {
    Flush(FLUSH_HOOK); /* whatever is open is real geometry: draw it; the dummy opens its own batch */
    const int ci = FxCatIndex(cat);
    sFx->learnCat = (ci >= 0 && kind < GDX_NRRFX_KINDS) ? ci : -1;
    sFx->learnKind = static_cast<int>(kind);
}

/* Probe the packer with synthetic vertices (the UV map per axis), read where every colour comes
 * from, then the core state key. False for a state the hot path does not model. */
namespace nrrfx {
/* The probe's check of the packing sequence (cold; the hot packer inlines the same expressions). */
GDX_COLD float NrrFxPackU(const FxPack& k, float U) {
    float u = U / 32.0f * k.uPre - k.uTile + k.half;
    u = u / k.uSize * k.uScale;
    return (u < k.uClamp ? u : k.uClamp) + k.uOff;
}
GDX_COLD float NrrFxPackV(const FxPack& k, float V) {
    float w = V / 32.0f * k.vPre - k.vTile + k.half;
    w = w / k.vSize * k.vScale;
    return (w < k.vClamp ? w : k.vClamp) + k.vOff;
}
} // namespace nrrfx

GDX_COLD bool Interp::NrrFxProbe(void* blockPtr) {
    FxBlock& b = *static_cast<FxBlock*>(blockPtr);
    const DrawState& ds = mTriMemo;
    /* Fog: only the standard per-vertex fog (factor in the vertex alpha, the backend's fog stage
     * lerps to the fog colour); not the blend-colour shroud mode. */
    const bool fog = ds.use_fog && mPackParams.fogFactorToAlpha && !ds.use_blend_color;
    if (ds.usedTextures[1] || (ds.use_fog && !fog) || (mPackParams.fogFactorToAlpha && !ds.use_fog)) {
        return false;
    }
    if ((mRsp.geometry_mode & (GM_LIGHTING | GM_TEXTURE_GEN)) != 0) {
        return false;
    }
    if (((mRsp.geometry_mode & GM_FOG) != 0) != fog) {
        return false;
    }
    const Combiner* comb = ds.comb;
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
    FxPack& k = b.pack;
    memset(&k, 0, sizeof(k));
    k.textured = ds.usedTextures[0] ? 1 : 0;
    k.uSize = k.vSize = 1.0f;
    if (k.textured) {
        /* HudPackVerts' prep for unit 0 */
        const Tile& ut = mRdp.texture_tile[ds.effective_tile[0]];
        const int shifts = ut.shifts, shiftt = ut.shiftt;
        k.uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / static_cast<float>(1 << shifts)
                                                    : static_cast<float>(1 << (16 - shifts)));
        k.vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / static_cast<float>(1 << shiftt)
                                                    : static_cast<float>(1 << (16 - shiftt)));
        k.uTile = ut.uls / 4.0f;
        k.vTile = ut.ult / 4.0f;
        k.half = bilerp ? 0.5f : 0.0f;
        k.uSize = static_cast<float>(ds.tex_width[0]);
        k.vSize = static_cast<float>(ds.tex_height[0]);
        k.uScale = mPackParams.uScale[0];
        k.vScale = mPackParams.vScale[0];
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
    }
    k.rgb[0] = o[6];
    k.rgb[1] = o[7];
    k.rgb[2] = o[8];
    k.a = o[9];
    const int ci = mPackParams.vtxColorInput;
    const int ai = mPackParams.vtxAlphaInput;
    k.rgbSrc = ci > 0 ? FxSrcRgb(comb->shader_input_mapping[0][ci - 1]) : SRC_CONST;
    k.aSrc = (mPackParams.wantVtxAlpha && ai > 0) ? FxSrcAlpha(comb->shader_input_mapping[1][ai - 1]) : SRC_CONST;
    k.fog = fog ? 1 : 0;
    if (fog) {
        k.aSrc = SRC_FOG;
    }
    k.scaleS = mRsp.texture_scaling_factor.s;
    k.scaleT = mRsp.texture_scaling_factor.t;
    k.yMul = ds.clip.invertY ? -1.0f : 1.0f;
    k.zMul = ds.clip.zIsFrom0To1 ? 0.5f : 1.0f; /* (z + w) / 2 == z / 2 + w / 2 exactly */
    k.zW = ds.clip.zIsFrom0To1 ? 0.5f : 0.0f;
    if (!k.textured) {
        k.uClamp = k.vClamp = 1e30f;
    }
    /* the probe's own packing must agree with the sequence above (a mismatch: a state the hot
     * path does not model) */
    if (k.textured && (o[kStride + 4] != NrrFxPackU(k, 1024.0f) || o[kStride + 5] != NrrFxPackV(k, 1024.0f))) {
        return false;
    }
    /* The stage-constant inputs: the vertex-0 record, and which follow a register. */
    b.nInputs = mPackAux.numInputs < 7 ? mPackAux.numInputs : 7;
    memcpy(b.inputs, mPackAux.inputRgba, sizeof(b.inputs));
    b.dynInputs = 0;
    for (int j = 0; j < b.nInputs; j++) {
        /* the input riding the vertex attribute is no stage constant: its value is per vertex */
        b.inSrc[j][0] = j + 1 != ci ? FxSrcRgb(comb->shader_input_mapping[0][j]) : SRC_CONST;
        const bool alphaRides = mPackParams.wantVtxAlpha && j + 1 == ai;
        b.inSrc[j][1] = (ds.use_alpha && !alphaRides) ? FxSrcAlpha(comb->shader_input_mapping[1][j]) : SRC_CONST;
        if (fog && b.inSrc[j][1] == SRC_SHADE) {
            b.inSrc[j][1] = SRC_CONST; /* HudPackAux: under fog a shade alpha input is 1.0 */
        }
        if ((b.inSrc[j][0] | b.inSrc[j][1]) != SRC_CONST) {
            b.dynInputs = 1;
        }
    }
    memcpy(b.prim0, &mRdp.prim_color, 4);
    memcpy(b.env0, &mRdp.env_color, 4);
    b.rejVariant = (mVariant == 1 || mVariant == 2) ? 1 : 0;
    const uint32_t cull = mRsp.geometry_mode & GM_CULL_BOTH;
    b.cullBack = (cull & GM_CULL_BACK) != 0 ? 1 : 0;
    b.cullFront = (cull & GM_CULL_FRONT) != 0 ? 1 : 0;
    b.cullInvert = (mRsp.extra_geometry_mode & 0x00000001u) != 0 ? 1 : 0;
    /* The core key: what ResolveDrawState bound, in the core's terms. */
    NrrStateKey& key = b.key;
    memset(&key, 0, sizeof(key));
    key.shaderId0 = comb->shader_id0;
    key.shaderId1 = comb->shader_id1 | static_cast<uint64_t>(ds.tm) * SHOPT_TEXEL0_CLAMP_S;
    key.tex[0] = k.textured ? b.texId : NRR_TEX_NONE;
    key.tex[1] = NRR_TEX_NONE;
    key.texLinear[0] = mRs.sampler_linear[0] ? 1 : 0;
    key.texCms[0] = mRs.sampler_cms[0];
    key.texCmt[0] = mRs.sampler_cmt[0];
    key.program = NRR_PROG_2D;
    key.depthTest = (mRs.depth_test_and_mask & 1u) != 0 ? 1 : 0;
    key.depthWrite = (mRs.depth_test_and_mask & 2u) != 0 ? 1 : 0;
    key.decal = mRs.decal_mode == 1 ? 1 : 0;
    key.blend = mRs.alpha_blend == 1 ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    key.cull = NRR_CULL_NONE; /* culled on the CPU, as SpTri does */
    key.alphaTest = NRR_AT_OFF; /* the translated shader's texEdge / alphaThreshold decide it */
    key.alphaRef = mRdp.blend_color.a;
    return true;
}

GDX_COLD void Interp::NrrFxLearnEnd(uint32_t cat, uint32_t kind) {
    FxRender& R = *sFx;
    const int ci = FxCatIndex(cat);
    const bool open = ci >= 0 && R.learnCat == ci && R.learnKind == static_cast<int>(kind);
    R.learnCat = -1;
    R.learnKind = -1;
    if (!open) {
        return;
    }
    R.interp = this;
    FxCat& C = R.cat[ci];
    FxBlock& b = C.blocks[kind];
    memset(&b, 0, sizeof(b));
    b.slot = -1;
    const bool batch = mBufNumTris > 0 && mPackBatch && mTriMemoValid;
    const bool textured = batch && mTriMemo.usedTextures[0];
    const bool resolved = batch && (!textured || mRs.textures[0] >= 0);
    if (resolved) {
        if (textured) {
            b.slot = mRs.textures[0];
            b.texId = mTex[b.slot].texture_id;
            b.texAddr = mTex[b.slot].key.addr;
        }
        b.usable = NrrFxProbe(&b) ? 1 : 0;
        if (b.usable && textured) {
            C.ownTex[kind] = FxOwnTexture(b.texId);
            b.key.texObj[0] = C.ownTex[kind]; /* null: the block resolves the cache's id */
        }
        if (ci == 0) {
            AtlasLearn(C, kind);
        }
    }
    C.wLearn++;
    gdx3ds_nrr_fx_ready[ci][kind] = b.usable;
    if (!resolved || !b.usable) {
        C.wLearnFail++;
        if (resolved) {
            gdx3ds_nrr_fx_broken[ci] = 1; /* a state the hot path does not model: the stub goes stock */
        }
    }
    if (C.learnLogs < 64) {
        C.learnLogs++;
        const DrawState& ds = mTriMemo;
        GDXN_LOG("[nrrfx] learn cat=%u kind=%u ok=%d usable=%d cc=%08x%08x oml=%08x omh=%08x geo=%08x "
                 "tex=%d/%u %ux%u tm=%u alpha=%d dz=%u blend=%u | uv pre %.6g tile %.6g half %.2g clamp %.6g | v pre %.6g "
                 "tile %.6g clamp %.6g | rgb src=%u %.3f,%.3f,%.3f a src=%u %.3f | vci=%d vai=%d in=%u dyn=%u "
                 "src=%u%u%u%u/%u%u%u%u cull=%u%u scale=%04x/%04x rej=%u fog=%u",
                 cat, kind, resolved ? 1 : 0, b.usable, static_cast<unsigned>(mRdp.combine_mode >> 32),
                 static_cast<unsigned>(mRdp.combine_mode), mRdp.other_mode_l, mRdp.other_mode_h, mRsp.geometry_mode,
                 b.slot, b.texId, ds.tex_width[0], ds.tex_height[0], ds.tm, ds.use_alpha ? 1 : 0,
                 mRs.depth_test_and_mask, mRs.alpha_blend, b.pack.uPre, b.pack.uTile, b.pack.half, b.pack.uClamp,
                 b.pack.vPre, b.pack.vTile, b.pack.vClamp, b.pack.rgbSrc, b.pack.rgb[0], b.pack.rgb[1], b.pack.rgb[2],
                 b.pack.aSrc, b.pack.a, mPackParams.vtxColorInput, mPackParams.vtxAlphaInput, b.nInputs, b.dynInputs,
                 b.inSrc[0][0], b.inSrc[1][0], b.inSrc[2][0], b.inSrc[3][0], b.inSrc[0][1], b.inSrc[1][1],
                 b.inSrc[2][1], b.inSrc[3][1], b.cullBack, b.cullFront, b.pack.scaleS, b.pack.scaleT, b.rejVariant,
                 b.pack.fog);
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

/* ---- hot: Interp members the draws read through ----------------------------------------- */

/* The block's texture-cache entry still holds what was learned (the id is recycled on retire);
 * refreshed in the LRU like a cache hit. */
bool Interp::NrrFxHotTex(int16_t slot, uint32_t id, const uint8_t* addr) {
    const TexEntry& e = mTex[slot];
    if (!e.used || !e.uploaded || e.texture_id != id || e.key.addr != addr) {
        return false;
    }
    TexCacheTouch(slot);
    return true;
}

/* The walker's MP (for sections whose vertices are in its current space), TransformVertices'
 * aspect branch as one factor, and the scissor the stock draw would bind. */
void Interp::NrrFxHotHead(float mp[4][4], float xs[2], int16_t scissor[4]) const {
    if (mp != nullptr) {
        memcpy(mp, mRsp.MP_matrix, sizeof(mRsp.MP_matrix));
    }
    /* TransformVertices' branches, as two factors so the arithmetic is the same: the stretch
     * branch multiplies once, AdjXForAspectRatio multiplies by 4/3 then divides by the aspect. */
    const float aspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
    xs[0] = 1.0f;
    xs[1] = 1.0f;
    if ((mRsp.extra_geometry_mode & 0x00000004u) != 0 && aspect > (4.0f / 3.0f) && mCfg.widescreen &&
        !mCfg.forceFixedAspect) {
        xs[0] = 160.0f / (160.0f - 12.0f);
    } else if (mCfg.widescreen && !mCfg.forceFixedAspect) {
        xs[0] = 4.0f / 3.0f;
        xs[1] = aspect;
    }
    scissor[0] = static_cast<int16_t>(mRdp.scissor.x);
    scissor[1] = static_cast<int16_t>(mRdp.scissor.y);
    scissor[2] = static_cast<int16_t>(mRdp.scissor.width);
    scissor[3] = static_cast<int16_t>(mRdp.scissor.height);
}

/* The fog position and colour the stock triangles would read (TransformVertices, the fog stage). */
__attribute__((noinline)) void Interp::NrrFxHotFog(int16_t* mul, int16_t* off, float rgb[3]) const {
    *mul = mRsp.fog_mul;
    *off = mRsp.fog_offset;
    rgb[0] = mRdp.fog_color.r / 255.0f;
    rgb[1] = mRdp.fog_color.g / 255.0f;
    rgb[2] = mRdp.fog_color.b / 255.0f;
}

/* A host vertex pointer as the walker's G_VTX resolves it (an asset symbol is a placeholder the
 * bridge maps to the loaded resource). Cold: the categories cache the result per source. */
GDX_COLD const void* Interp::NrrFxResolveVtx(const void* src, uint32_t n) {
    Cmd c;
    c.w0 = (0x01u << 24) | (n << 12) | ((n & 0x7Fu) << 1);
    c.w1 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(src));
    c.w1hi = kHostTag;
    const size_t need = n * 16u;
    const uintptr_t p = mSrc.ResolveData(c, need, /*preferPhysical=*/true, 0);
    if (p == 0 || DlSrc::ReadableBytes(p) < need) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(p);
}

namespace nrrfx {

/* Resolved asset vertex sources (shadow quads: 45 static arrays). */
struct FxVtxMemo {
    const void* src[64];
    const Vtx* res[64];
    uint32_t n;
};
FxVtxMemo sVtxMemo;

GDX_COLD const Vtx* FxResolveCold(const void* src, uint32_t n) {
    const Vtx* r = static_cast<const Vtx*>(sFx->interp->NrrFxResolveVtx(src, n));
    if (r != nullptr && sVtxMemo.n < 64) {
        sVtxMemo.src[sVtxMemo.n] = src;
        sVtxMemo.res[sVtxMemo.n] = r;
        sVtxMemo.n++;
        if (sVtxMemo.n <= 4) {
            GDXN_LOG("[nrrfx] vtx %p -> %p ob0=%d,%d,%d tc0=%d,%d cn0=%u,%u,%u,%u ob1=%d,%d,%d ob2=%d,%d,%d", src,
                     static_cast<const void*>(r), r[0].ob[0], r[0].ob[1], r[0].ob[2], r[0].tc[0], r[0].tc[1], r[0].cn[0],
                     r[0].cn[1], r[0].cn[2], r[0].cn[3], r[1].ob[0], r[1].ob[1], r[1].ob[2], r[2].ob[0], r[2].ob[1],
                     r[2].ob[2]);
        }
    }
    return r;
}

const Vtx* NrrFxHotVtx(const void* src, uint32_t n) {
    for (uint32_t i = 0; i < sVtxMemo.n; i++) {
        if (sVtxMemo.src[i] == src) {
            return sVtxMemo.res[i];
        }
    }
    return FxResolveCold(src, n);
}

/* One vertex after T&L: clip position (x already scaled by the aspect factor), stock U/V, colour,
 * and SpTri's trivial-reject bits (TransformVertices' clip_rej). */
struct FxVtx {
    float c[4];
    float u, v;      /* the loaded (scaled) texture coordinates */
    uint8_t rgba[4]; /* colour; alpha = the fog factor under fog (TransformVertices) */
    uint8_t out;     /* 1 x<-w, 2 x>w, 4 y<-w, 8 y>w, 16 z>w (SpTri's clip_rej planes: AND = reject),
                        32 z<-w, 64 w<kMinClipW (with the first five, SpTri's clip_out: OR = clip),
                        128 outside the reject microcode's 2x box (SpTri's reject variant) */
};

constexpr float kMinClipW = 1.0e-5f; /* gdx_tri.cpp kMinClipW */

/* TransformVertices' fog factor of a clip-space vertex. */
__attribute__((noinline)) uint8_t NrrFxHotFogByte(const float* c) {
    float ratio;
    if (c[3] <= 0.001f) {
        ratio = -1.0f;
    } else {
        ratio = c[2] / c[3];
        ratio = ratio < -1.0f ? -1.0f : (ratio > 1.0f ? 1.0f : ratio);
    }
    float f = ratio * sFx->fogMul + sFx->fogOff;
    f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
    return static_cast<uint8_t>(f);
}

/* TransformVertices for one block: clip position (x by the aspect factor), reject and clip bits,
 * the scaled U/V, the colour (its alpha replaced by the fog factor when the block fogs). */
__attribute__((noinline)) void NrrFxHotXform(const float (*mp)[4], const float* xs, const Vtx* s, uint32_t n,
                                             const FxPack& k, FxVtx* out) {
    for (uint32_t i = 0; i < n; i++) {
        const Vtx& v = s[i];
        FxVtx& o = out[i];
        for (int j = 0; j < 4; j++) {
            o.c[j] = v.ob[0] * mp[0][j] + v.ob[1] * mp[1][j] + v.ob[2] * mp[2][j] + mp[3][j];
        }
        o.c[0] = o.c[0] * xs[0] / xs[1];
        const float x = o.c[0], y = o.c[1], z = o.c[2], w = o.c[3];
        /* a - b < 0 <=> a < b for finite floats (a rounded difference keeps its sign): the
         * clip_out and clip_rej tests of TransformVertices share these compares */
        const float w2 = 2.0f * w;
        o.out = static_cast<uint8_t>((x < -w ? 1u : 0u) | (x > w ? 2u : 0u) | (y < -w ? 4u : 0u) | (y > w ? 8u : 0u) |
                                     (z > w ? 16u : 0u) | (z < -w ? 32u : 0u) | (w < kMinClipW ? 64u : 0u) |
                                     ((w <= 0.0f || x < -w2 || x > w2 || y < -w2 || y > w2 || z < -w) ? 128u : 0u));
        o.u = static_cast<float>(static_cast<int16_t>(v.tc[0] * k.scaleS >> 16));
        o.v = static_cast<float>(static_cast<int16_t>(v.tc[1] * k.scaleT >> 16));
        memcpy(o.rgba, v.cn, 4);
        if (k.fog) {
            o.rgba[3] = NrrFxHotFogByte(o.c);
        }
    }
}

/* SpTri's reject and cull for one triangle (true = it draws). */
bool NrrFxHotKeep(const FxBlock& b, const FxVtx& a, const FxVtx& c1, const FxVtx& c2, FxCat& C) {
    if ((a.out & c1.out & c2.out & 0x1Fu) != 0) {
        C.wRej++;
        return false;
    }
    if ((b.cullBack | b.cullFront) == 0) {
        return true;
    }
    const float dx1 = a.c[0] / a.c[3] - c1.c[0] / c1.c[3];
    const float dy1 = a.c[1] / a.c[3] - c1.c[1] / c1.c[3];
    const float dx2 = c2.c[0] / c2.c[3] - c1.c[0] / c1.c[3];
    const float dy2 = c2.c[1] / c2.c[3] - c1.c[1] / c1.c[3];
    float cross = dx1 * dy2 - dy1 * dx2;
    if ((a.c[3] < 0) ^ (c1.c[3] < 0) ^ (c2.c[3] < 0)) {
        cross = -cross;
    }
    if (b.cullInvert) {
        cross = -cross;
    }
    bool culled;
    if (b.cullBack && b.cullFront) {
        culled = true;
    } else if (b.cullFront) {
        culled = cross <= 0;
    } else {
        culled = cross >= 0;
    }
    if (culled) {
        C.wCull++;
    }
    return !culled;
}

/* A source's value (0..1) for channel k (0-2 rgb, 3 alpha) from the run's registers. */
inline float FxSrcUnit(const uint8_t* regs, uint8_t src, int k) {
    return sByteToUnit[regs[kSrcIdx[src & 7][k]]];
}

/* The run's packed colour from the block's sources and the run's registers. */
__attribute__((noinline)) void NrrFxHotRunColor(FxBatch& t) {
    const FxPack& k = t.b->pack;
    FxColor& col = t.col;
    col.rgbMode = k.rgbSrc == SRC_SHADE ? 1 : (k.rgbSrc == SRC_SHADE_A ? 2 : 0);
    for (int i = 0; i < 3; i++) {
        col.rgb[i] = k.rgbSrc == SRC_CONST ? k.rgb[i] : FxSrcUnit(t.regs, k.rgbSrc, i);
    }
    col.aMode = (k.aSrc == SRC_SHADE || k.aSrc == SRC_FOG) ? 1 : 0;
    col.a = (k.aSrc == SRC_CONST || k.aSrc == SRC_FOG) ? k.a : FxSrcUnit(t.regs, k.aSrc, 3);
}

/* One packed vertex (HudPackVerts' layout) through the block's map and the run's colour. */
__attribute__((noinline)) void NrrFxHotPack(const FxPack& k, const FxColor& col, const FxVtx& s, float* o) {
    o[0] = s.c[0];
    o[1] = s.c[1] * k.yMul;
    o[2] = s.c[2] * k.zMul + s.c[3] * k.zW;
    o[3] = s.c[3];
    float u = s.u / 32.0f * k.uPre - k.uTile + k.half;
    float w = s.v / 32.0f * k.vPre - k.vTile + k.half;
    u = u / k.uSize * k.uScale;
    w = w / k.vSize * k.vScale;
    o[4] = (u < k.uClamp ? u : k.uClamp) + k.uOff;
    o[5] = (w < k.vClamp ? w : k.vClamp) + k.vOff;
    if (col.rgbMode == 0) {
        o[6] = col.rgb[0];
        o[7] = col.rgb[1];
        o[8] = col.rgb[2];
    } else if (col.rgbMode == 1) {
        o[6] = sByteToUnit[s.rgba[0]];
        o[7] = sByteToUnit[s.rgba[1]];
        o[8] = sByteToUnit[s.rgba[2]];
    } else {
        o[6] = o[7] = o[8] = sByteToUnit[s.rgba[3]];
    }
    o[9] = col.aMode == 0 ? col.a : sByteToUnit[s.rgba[3]]; /* vertex alpha or (fog) its fog factor */
    o[10] = 0.0f;
    o[11] = 0.0f;
}

/* The batch's stage constants: learned, or the per-draw registers / vertex-0 shade. */
void NrrFxHotInputs(const FxBatch& t, float (*in)[4]) {
    const FxBlock& b = *t.b;
    memcpy(in, b.inputs, sizeof(b.inputs));
    if (!b.dynInputs) {
        return;
    }
    for (int j = 0; j < b.nInputs; j++) {
        for (int k = 0; k < 4; k++) {
            const uint8_t src = b.inSrc[j][k == 3 ? 1 : 0];
            if (src != SRC_CONST) {
                in[j][k] = FxSrcUnit(t.regs, src, k);
            }
        }
    }
}

/* Draw the open run (if any). */
void NrrFxHotFlush(NrrCtx* c, FxBatch& t) {
    const uint32_t nv = t.cur - t.start;
    if (nv == 0 || t.b == nullptr) {
        t.start = t.cur;
        return;
    }
    FxBlock& b = *t.b;
    NrrStateApply(c, b.st);
    if (b.nInputs != 0) {
        float in[7][4];
        NrrFxHotInputs(t, in);
        NrrSetInputs(c, b.st, in, b.nInputs);
    }
    if (b.pack.fog) {
        NrrSetFog(c, b.st, sFx->fogRgb);
    }
    NrrDraw2DScene(c, t.base + t.start * kStride, nv); /* perspective: the border mode's scene map */
    t.cat->wDraws++;
    t.cat->wTris += nv / 3u;
    t.start = t.cur;
}

/* The block's core state for this frame: its texture-cache entry still valid, the core block
 * current for the frame's scissor. False = not drawable this frame (counted, relearned). */
bool NrrFxHotReady(FxCat& C, int ci, uint32_t kind) {
    FxBlock& b = C.blocks[kind];
    if (!b.usable) {
        C.wStale++;
        gdx3ds_nrr_fx_ready[ci][kind] = 0;
        return false;
    }
    if (b.slot >= 0 && !sFx->interp->NrrFxHotTex(b.slot, b.texId, b.texAddr) && !FxRetired(C, ci, kind)) {
        return false;
    }
    if (b.st == nullptr || b.stGen != NrrStateGeneration() ||
        memcmp(b.key.scissor, sFx->scissor, sizeof(sFx->scissor)) != 0) {
        if (FxStateFor(b, C.stMem[kind], sFx->scissor) == nullptr) {
            C.wFallback++;
            return false;
        }
    }
    return true;
}

/* Open (or continue) a run for block `kind` with these registers. False = the block cannot draw. */
bool NrrFxHotUse(NrrCtx* c, FxBatch& t, int ci, uint32_t kind, const uint8_t* prim, const uint8_t* env) {
    FxBlock* b = &t.cat->blocks[kind];
    if (prim == nullptr) {
        prim = b->prim0; /* the register the setup list left */
    }
    if (env == nullptr) {
        env = b->env0;
    }
    if (b == t.b) {
        const bool regsSame = memcmp(prim, t.regs + 4, 4) == 0 && memcmp(env, t.regs + 8, 4) == 0;
        if (regsSame) {
            return true;
        }
        if (!b->dynInputs) {
            /* the registers ride the vertex colour only (no stage constant reads them): the same
             * draw, the next vertices take the new colour (the shadow batch's per-decal prim) */
            memcpy(t.regs + 4, prim, 4);
            memcpy(t.regs + 8, env, 4);
            NrrFxHotRunColor(t);
            return true;
        }
    }
    NrrFxHotFlush(c, t);
    if (b != t.b && !NrrFxHotReady(*t.cat, ci, kind)) {
        t.b = nullptr;
        return false;
    }
    t.b = b;
    memcpy(t.regs + 4, prim, 4);
    memcpy(t.regs + 8, env, 4);
    NrrFxHotRunColor(t);
    return true;
}

/* The frame's ring is full (the clipper's fans add triangles): draw the open run and carve a new
 * chunk. False = the pool is out (counted). */
GDX_COLD bool NrrFxGrow(FxBatch& t) {
    NrrFxHotFlush(t.ctx, t);
    const uint32_t cap = 96u;
    float* base = static_cast<float*>(NrrRingAlloc(t.ctx, cap * kStride * sizeof(float)));
    if (base == nullptr) {
        t.cat->wFallback++;
        return false;
    }
    t.base = base;
    t.cap = cap;
    t.start = t.cur = 0;
    return true;
}

/* A shadow triangle's texture coordinates into its atlas slot (the slot replicates the texture's
 * edge texels into an 8-texel margin, which is what CLAMP_TO_EDGE samples out there). */
__attribute__((noinline)) void NrrShdHotRemap(const FxBatch& t, float* o) {
    for (int i = 0; i < 3; i++, o += kStride) {
        const float u = o[4] * t.ru + t.rx;
        const float v = o[5] * t.rv + t.ry;
        o[4] = u < t.uLo ? t.uLo : (u > t.uHi ? t.uHi : u);
        o[5] = v < t.vLo ? t.vLo : (v > t.vHi ? t.vHi : v);
    }
}

/* Pack one kept triangle into the open run. */
void NrrFxHotEmit(FxBatch& t, const FxVtx& a, const FxVtx& b, const FxVtx& c) {
    if (t.cur + 3u > t.cap && !NrrFxGrow(t)) {
        return;
    }
    if (t.cur == t.start) {
        memcpy(t.regs, a.rgba, 4); /* the batch's vertex 0: its shade feeds shade stage constants */
    }
    const FxPack& k = t.b->pack;
    float* o = t.base + t.cur * kStride;
    NrrFxHotPack(k, t.col, a, o);
    NrrFxHotPack(k, t.col, b, o + kStride);
    NrrFxHotPack(k, t.col, c, o + 2 * kStride);
    if (t.remap) {
        NrrShdHotRemap(t, o);
    }
    t.cur += 3u;
}

/* gdx_tri.cpp ClipDistance / Interpolate. */
float FxClipDistance(const FxVtx& v, int plane) {
    switch (plane) {
        case 0: return v.c[3] - kMinClipW;
        case 1: return v.c[0] + v.c[3];
        case 2: return v.c[3] - v.c[0];
        case 3: return v.c[1] + v.c[3];
        case 4: return v.c[3] - v.c[1];
        case 5: return v.c[2] + v.c[3];
        default: return v.c[3] - v.c[2];
    }
}

FxVtx FxClipLerp(const FxVtx& a, const FxVtx& b, float t) {
    FxVtx r;
    memset(&r, 0, sizeof(r));
    for (int k = 0; k < 4; k++) {
        r.c[k] = a.c[k] + (b.c[k] - a.c[k]) * t;
    }
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    for (int k = 0; k < 4; k++) {
        const float x = a.rgba[k] + (b.rgba[k] - a.rgba[k]) * t;
        r.rgba[k] = static_cast<uint8_t>(x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x));
    }
    return r;
}

/* SpTri's reject bits of a clipped fan vertex (UpdateClipFlags), w floored as the fan does. */
void FxFanVertex(FxVtx& v) {
    v.c[3] = v.c[3] > kMinClipW ? v.c[3] : kMinClipW;
    const float x = v.c[0], y = v.c[1], z = v.c[2], w = v.c[3];
    v.out = static_cast<uint8_t>((x < -w ? 1u : 0u) | (x > w ? 2u : 0u) | (y < -w ? 4u : 0u) | (y > w ? 8u : 0u) |
                                 (z > w ? 16u : 0u));
}

/* A triangle SpTri clips (a vertex outside any of its 7 planes): its CPU clipper -- Sutherland-
 * Hodgman, the same arithmetic -- then its fan, each piece through the reject and cull. The PICA
 * would clip these too, but not with the same vertices: the edges of a straddling shadow moved by
 * a pixel (gp2-sector m01_3445). */
GDX_COLD void NrrFxClipTri(FxBatch& t, const FxVtx& a, const FxVtx& b, const FxVtx& c) {
    FxVtx pa[16];
    FxVtx pb[16];
    FxVtx* in = pa;
    FxVtx* out = pb;
    int n = 3;
    pa[0] = a;
    pa[1] = b;
    pa[2] = c;
    for (int plane = 0; plane < 7 && n >= 3; plane++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const FxVtx& cur = in[i];
            const FxVtx& nxt = in[i + 1 == n ? 0 : i + 1];
            const float cd = FxClipDistance(cur, plane);
            const float nd = FxClipDistance(nxt, plane);
            if (cd >= 0.0f && m < 16) {
                out[m++] = cur;
            }
            if ((cd >= 0.0f) != (nd >= 0.0f) && m < 16) {
                const float den = cd - nd;
                float s = den != 0.0f ? cd / den : 0.0f;
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
                out[m++] = FxClipLerp(cur, nxt, s);
            }
        }
        FxVtx* sw = in;
        in = out;
        out = sw;
        n = m;
    }
    if (n < 3) {
        t.cat->wRej++;
        return;
    }
    for (int i = 1; i + 1 < n; i++) {
        FxVtx f0 = in[0];
        FxVtx f1 = in[i];
        FxVtx f2 = in[i + 1];
        FxFanVertex(f0);
        FxFanVertex(f1);
        FxFanVertex(f2);
        if (NrrFxHotKeep(*t.b, f0, f1, f2, *t.cat)) {
            NrrFxHotEmit(t, f0, f1, f2);
        }
    }
}

/* Triangle (a, b, c) of the transformed vertices `v`: SpTri's clip / reject / cull, then packed. */
void NrrFxHotTri(FxBatch& t, const FxVtx* v, int a, int b, int c) {
    const uint8_t clip = v[a].out | v[b].out | v[c].out;
    if ((clip & 0x80u) != 0 && t.b->rejVariant) {
        t.cat->wRej++; /* the reject microcode drops it whole (SpTri's variant screen) */
        return;
    }
    if ((clip & 0x7Fu) != 0) {
        NrrFxClipTri(t, v[a], v[b], v[c]);
        return;
    }
    if (NrrFxHotKeep(*t.b, v[a], v[b], v[c], *t.cat)) {
        NrrFxHotEmit(t, v[a], v[b], v[c]);
    }
}

/* A category draw's prologue: the walker's head (MP when wanted, aspect, scissor), the receipt,
 * an empty run over a ring chunk of capVerts packed vertices. False = the pool is out (counted). */
/* niview: a view of a multi-view race -- this view's viewport/scissor reaches the backend (Camera_Draw
 * set it; nothing drew since) with the hand-off closed (the backend changes citro3d: n60retry's
 * pattern, so the direct words' baseline at NrrBegin holds it -- applied inside the hand-off,
 * nrr_direct=2 counted its viewport words as an unmarked hand-back, endbad=), the draws keep it (keepVp:
 * cut to the view's clip square by the core, as the machines), and the blocks are keyed on the
 * full-window scissor (one block for every view). */
GDX_COLD void NrrFxColdMultiView(NrrCtx* c) {
    if (sFx->interp->ViewportScissorDirty()) {
        NrrEnd(c);
        sFx->interp->ApplyViewportScissor();
        NrrBegin(c);
    }
    c->keepVp = 1;
    gdx3ds_nrr_note_view(c->view);
}

bool NrrFxHotOpen(NrrCtx* c, FxBatch& t, FxCat& C, uint32_t ents, uint32_t capVerts, float (*mp)[4], bool multi) {
    if (GDX_UNLIKELY(multi)) {
        NrrFxColdMultiView(c);
    }
    sFx->interp->NrrFxHotHead(mp, sFx->xs, sFx->scissor);
    if (GDX_UNLIKELY(multi)) {
        memset(sFx->scissor, 0, sizeof(sFx->scissor));
    }
    C.wFrames++;
    C.wEnts += ents;
    memset(&t, 0, sizeof(t));
    t.ctx = c;
    t.cat = &C;
    t.cap = capVerts;
    t.base = static_cast<float*>(NrrRingAlloc(c, capVerts * kStride * sizeof(float)));
    if (t.base == nullptr) {
        C.wFallback++;
        return false;
    }
    return true;
}

/* ---- shadows ------------------------------------------------------------------------------ */

/* The decal's slot in the atlas (kind 0xFF: no remap, the kind's own texture). */
void NrrShdHotSlot(FxBatch& t, uint32_t kind) {
    if (kind == 0xFFu) {
        t.remap = 0;
        return;
    }
    const float x0 = static_cast<float>(sAtlas.slotX[kind] + kAtlasMargin);
    const float y0 = static_cast<float>(sAtlas.slotY[kind] + kAtlasMargin);
    t.remap = 1;
    t.ru = static_cast<float>(sAtlas.texW) / kAtlasW;
    t.rx = x0 / kAtlasW;
    t.rv = static_cast<float>(sAtlas.texH) / kAtlasH;
    t.ry = y0 / kAtlasH;
    t.uLo = (x0 - (kAtlasMargin - 0.5f)) / kAtlasW;
    t.uHi = (x0 + sAtlas.texW + (kAtlasMargin - 0.5f)) / kAtlasW;
    t.vLo = (y0 - (kAtlasMargin - 0.5f)) / kAtlasH;
    t.vHi = (y0 + sAtlas.texH + (kAtlasMargin - 0.5f)) / kAtlasH;
}

int NrrShdHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    const GdxNrrShdSnap* snap = static_cast<const GdxNrrShdSnap*>(NrrSnapGet(f, NRR_CAT_SHD, view));
    if (snap == nullptr || sFx == nullptr || sFx->interp == nullptr) {
        return 1;
    }
    FxCat& C = sFx->cat[0];
    FxBatch t;
    /* 6 vertices per decal + room for the clipper's fans (a straddling decal fans into more), so
     * the batch stays one draw */
    if (!NrrFxHotOpen(c, t, C, snap->n, static_cast<uint32_t>(snap->n) * 6u + 48u, nullptr, snap->multi != 0)) {
        return 4;
    }
    for (uint32_t i = 0; i < snap->n; i++) {
        const GdxNrrShdEnt& e = snap->e[i];
        const bool atlas = e.kind < GDX_NRRSHD_KINDS && sAtlas.keyed && sAtlas.slotX[e.kind] >= 0 &&
                           C.blocks[e.kind].usable;
        if (!NrrFxHotUse(c, t, 0, atlas ? kAtlasKind : e.kind, e.prim, nullptr)) {
            continue;
        }
        NrrShdHotSlot(t, atlas ? e.kind : 0xFFu);
        const Vtx* src = NrrFxHotVtx(e.vtx, 4);
        if (src == nullptr) {
            C.wFallback++;
            continue;
        }
        FxVtx v[4];
        NrrFxHotXform(e.mp, sFx->xs, src, 4, t.b->pack, v);
        NrrFxHotTri(t, v, 0, 1, 2);
        NrrFxHotTri(t, v, 0, 3, 1);
    }
    NrrFxHotFlush(c, t);
    return 0;
}

/* ---- fx: boost flames ---------------------------------------------------------------------- */

/* The stock gSP2Triangles / gSP1Triangle lists of Racer_Draw's booster section, per vertex layout
 * (4 per booster, or 5 with the trail) and booster count, in stock order; -1 ends a list. */
const int8_t kBoostQuad[5][9][3] = {
    { { -1 } },
    { { 1, 2, 3 }, { 0, 1, 3 }, { -1 } },
    { { 5, 6, 7 }, { 4, 5, 7 }, { 1, 2, 3 }, { 0, 1, 3 }, { -1 } },
    { { 9, 10, 11 }, { 8, 9, 11 }, { 5, 6, 7 }, { 4, 5, 7 }, { 1, 2, 3 }, { 0, 1, 3 }, { -1 } },
    { { 13, 14, 15 }, { 12, 13, 15 }, { 9, 10, 11 }, { 8, 9, 11 }, { 5, 6, 7 }, { 4, 5, 7 }, { 1, 2, 3 },
      { 0, 1, 3 }, { -1 } },
};
const int8_t kBoostTrail[5][13][3] = {
    { { -1 } },
    { { 1, 2, 3 }, { 0, 1, 3 }, { 1, 4, 3 }, { -1 } },
    { { 6, 7, 8 }, { 5, 6, 8 }, { 0, 1, 3 }, { 6, 9, 8 }, { 1, 4, 3 }, { 1, 2, 3 }, { -1 } },
    { { 11, 12, 13 }, { 10, 11, 13 }, { 5, 6, 8 }, { 11, 14, 13 }, { 6, 9, 8 }, { 6, 7, 8 }, { 1, 2, 3 },
      { 0, 1, 3 }, { 1, 4, 3 }, { -1 } },
    { { 16, 17, 18 }, { 15, 16, 18 }, { 10, 11, 13 }, { 16, 19, 18 }, { 11, 14, 13 }, { 11, 12, 13 },
      { 6, 7, 8 }, { 5, 6, 8 }, { 0, 1, 3 }, { 6, 9, 8 }, { 1, 4, 3 }, { 1, 2, 3 }, { -1 } },
};

/* Triangles of a transformed vertex block through the open run (reject, cull, pack). */
void NrrFxHotTris(FxBatch& t, const FxVtx* v, const int8_t (*tris)[3]) {
    for (int k = 0; tris[k][0] >= 0; k++) {
        NrrFxHotTri(t, v, tris[k][0], tris[k][1], tris[k][2]);
    }
}

void NrrFxHotBoost(NrrCtx* c, FxBatch& t, const GdxNrrFxSnap* snap, const float (*mp)[4]) {
    for (uint32_t i = 0; i < snap->nBoost; i++) {
        const GdxNrrBoostEnt& e = snap->boost[i];
        if (e.count == 0 || e.count > 4 || !NrrFxHotUse(c, t, 1, GDX_NRRFX_K_BOOST, nullptr, e.env)) {
            continue;
        }
        const uint32_t per = e.trail ? 5u : 4u;
        FxVtx v[20];
        NrrFxHotXform(mp, sFx->xs, static_cast<const Vtx*>(e.vtx), per * e.count, t.b->pack, v);
        NrrFxHotTris(t, v, e.trail ? kBoostTrail[e.count] : kBoostQuad[e.count]);
    }
}

/* ---- fx: the effects bracket's sprites ------------------------------------------------------ */

const int8_t kSpriteTri[2][3] = { { 0, 1, 2 }, { -1 } };
const int8_t kSpriteQuad[3][3] = { { 0, 1, 3 }, { 1, 2, 3 }, { -1 } };

void NrrFxHotSprites(NrrCtx* c, FxBatch& t, const GdxNrrFxSnap* snap, const float (*mp)[4]) {
    for (uint32_t i = 0; i < snap->nSprites; i++) {
        const GdxNrrFxSprite& e = snap->sprites[i];
        const uint8_t* prim = (e.set & GDX_NRRFX_SET_PRIM) != 0 ? e.prim : nullptr; /* else the setup list's */
        const uint8_t* env = (e.set & GDX_NRRFX_SET_ENV) != 0 ? e.env : nullptr;
        if (e.kind >= GDX_NRRFX_K_COUNT || !NrrFxHotUse(c, t, 1, e.kind, prim, env)) {
            continue;
        }
        const bool quad = e.pat == GDX_NRRFX_PAT_QUAD;
        FxVtx v[4];
        NrrFxHotXform(mp, sFx->xs, static_cast<const Vtx*>(e.vtx), quad ? 4u : 3u, t.b->pack, v);
        NrrFxHotTris(t, v, quad ? kSpriteQuad : kSpriteTri);
    }
}

/* [native] nrr_fx_dump=<tick> (diagnostic): every effect sprite of ticks tick-2..tick+2 with its
 * screen box, registers and vertex colours (cold; finds which class a tolerance blob belongs to). */
int sFxDumpTick = -1;

GDX_COLD void NrrFxDump(const GdxNrrFxSnap* snap, const float (*mp)[4], uint32_t tick) {
    for (uint32_t i = 0; i < snap->nSprites; i++) {
        const GdxNrrFxSprite& e = snap->sprites[i];
        const Vtx* v = static_cast<const Vtx*>(e.vtx);
        const int nv = e.pat == GDX_NRRFX_PAT_QUAD ? 4 : 3;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < nv; k++) {
            float c[4];
            for (int j = 0; j < 4; j++) {
                c[j] = v[k].ob[0] * mp[0][j] + v[k].ob[1] * mp[1][j] + v[k].ob[2] * mp[2][j] + mp[3][j];
            }
            const float sx = c[3] != 0.0f ? (c[0] * sFx->xs[0] / sFx->xs[1] / c[3] + 1.0f) * 200.0f : 0.0f;
            const float sy = c[3] != 0.0f ? (1.0f - c[1] / c[3]) * 120.0f : 0.0f;
            x0 = sx < x0 ? sx : x0;
            x1 = sx > x1 ? sx : x1;
            y0 = sy < y0 ? sy : y0;
            y1 = sy > y1 ? sy : y1;
        }
        GDXN_LOG("[nrrfx] dump tick=%u i=%u kind=%u pat=%u box=%.0f,%.0f-%.0f,%.0f prim=%u,%u,%u,%u env=%u,%u,%u,%u "
                 "cn=%u,%u,%u,%u/%u,%u,%u,%u/%u,%u,%u,%u",
                 tick, i, e.kind, e.pat, x0, y0, x1, y1, e.prim[0], e.prim[1], e.prim[2], e.prim[3], e.env[0], e.env[1],
                 e.env[2], e.env[3], v[0].cn[0], v[0].cn[1], v[0].cn[2], v[0].cn[3], v[1].cn[0], v[1].cn[1],
                 v[1].cn[2], v[1].cn[3], v[2].cn[0], v[2].cn[1], v[2].cn[2], v[2].cn[3]);
    }
}

GDX_COLD void NrrFxDumpMaybe(const GdxNrrFxSnap* snap, const float (*mp)[4], const NrrFrame* f) {
    const int tick = static_cast<int>(NrrFrameTick(f));
    if (tick >= sFxDumpTick - 2 && tick <= sFxDumpTick + 2) {
        NrrFxDump(snap, mp, static_cast<uint32_t>(tick));
    }
}

/* The fx category's draw: the boost flames, then the effects, in stock order. */
int NrrFxHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    const GdxNrrFxSnap* snap = static_cast<const GdxNrrFxSnap*>(NrrSnapGet(f, NRR_CAT_FX, view));
    if (snap == nullptr || sFx == nullptr) {
        return 1;
    }
    if (snap->nBoost == 0 && snap->nSprites == 0) {
        return 0;
    }
    if (sFx->interp == nullptr) {
        return 1;
    }
    float mp[4][4];
    FxBatch t;
    const uint32_t cap = static_cast<uint32_t>(snap->nBoost) * 36u + static_cast<uint32_t>(snap->nSprites) * 6u;
    if (!NrrFxHotOpen(c, t, sFx->cat[1], snap->nBoost + snap->nSprites, cap, mp, snap->multi != 0)) {
        return 4;
    }
    if (sFxDumpTick >= 0) {
        NrrFxDumpMaybe(snap, mp, f);
    }
    NrrFxHotBoost(c, t, snap, mp);
    NrrFxHotSprites(c, t, snap, mp);
    NrrFxHotFlush(c, t);
    return 0;
}

/* ---- dress: the gadget pass ------------------------------------------------------------------ */

const int8_t kMineTris[5][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 4 }, { 0, 4, 1 }, { -1 } };
const int8_t kJumpTris[7][3] = { { 0, 1, 2 }, { 3, 5, 4 }, { 0, 3, 1 }, { 1, 3, 4 }, { 0, 5, 3 }, { 0, 2, 5 }, { -1 } };
const int8_t kDashTris[5][3] = { { 0, 4, 1 }, { 0, 3, 4 }, { 0, 2, 3 }, { 2, 5, 3 }, { -1 } };

/* The stock gSPModifyVertex(G_MWO_POINT_ST) of a wrapping pair: the loaded U/V become (s', 0x8000)
 * with s' = the vertex's own s through VERTEX_MODIFIED_ST (s << 15, high half) or _ST2 (s). */
void NrrDressHotWrap(FxVtx& v, const Vtx& s, uint8_t st2) {
    const uint32_t hi = st2 ? (static_cast<uint32_t>(s.tc[0]) << 16) : (static_cast<uint32_t>(s.tc[0]) << 15);
    v.u = static_cast<float>(static_cast<int16_t>((hi & 0xFFFF0000u) >> 16));
    v.v = -32768.0f;
}

/* A course-effect strip load: per drawn pair, the wrap patch, then its two triangles. */
void NrrDressHotStrip(FxBatch& t, FxVtx* v, const Vtx* src, const GdxNrrDressItem& e) {
    for (int k = 0; k < 15 && (2 * k + 3) < e.nv; k++) {
        if (((e.pairs >> k) & 1u) == 0) {
            continue;
        }
        if ((e.mods >> k) & 1u) {
            NrrDressHotWrap(v[2 * k], src[2 * k], e.st2);
            NrrDressHotWrap(v[2 * k + 1], src[2 * k + 1], e.st2);
        }
        const int8_t tris[3][3] = { { static_cast<int8_t>(2 * k), static_cast<int8_t>(2 * k + 2),
                                      static_cast<int8_t>(2 * k + 1) },
                                    { static_cast<int8_t>(2 * k + 1), static_cast<int8_t>(2 * k + 2),
                                      static_cast<int8_t>(2 * k + 3) },
                                    { -1 } };
        NrrFxHotTris(t, v, tris);
    }
}

int NrrDressHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    const GdxNrrDressSnap* snap = static_cast<const GdxNrrDressSnap*>(NrrSnapGet(f, NRR_CAT_DRESS, view));
    if (snap == nullptr || sFx == nullptr || sFx->interp == nullptr) {
        return 1;
    }
    float mp[4][4];
    FxBatch t;
    if (!NrrFxHotOpen(c, t, sFx->cat[2], snap->n, snap->capVerts, mp, snap->multi != 0)) {
        return 4;
    }
    sFx->interp->NrrFxHotFog(&sFx->fogMul, &sFx->fogOff, sFx->fogRgb); /* noinline: a fogged category only */
    for (uint32_t i = 0; i < snap->n; i++) {
        const GdxNrrDressItem& e = snap->items[i];
        if (e.kind >= GDX_NRRDRESS_K_COUNT || e.nv > 32 || !NrrFxHotUse(c, t, 2, e.kind, nullptr, nullptr)) {
            continue;
        }
        const Vtx* src = static_cast<const Vtx*>(e.vtx);
        FxVtx v[32];
        NrrFxHotXform(mp, sFx->xs, src, e.nv, t.b->pack, v);
        if (e.pat == GDX_NRRDRESS_PAT_STRIP) {
            NrrDressHotStrip(t, v, src, e);
        } else if (e.pat == GDX_NRRDRESS_PAT_MINE) {
            NrrFxHotTris(t, v, kMineTris);
        } else if (e.pat == GDX_NRRDRESS_PAT_JUMP) {
            NrrFxHotTris(t, v, kJumpTris);
        } else {
            NrrFxHotTris(t, v, kDashTris);
        }
    }
    NrrFxHotFlush(c, t);
    return 0;
}

} // namespace nrrfx

/* ---- receipt ------------------------------------------------------------------------------ */

int Interp::NrrFxFormat(char* buf, size_t cap, uint32_t tasks) {
    if (sFx == nullptr) {
        return 0;
    }
    static const char* const kName[GDX_NRRFX_CATS] = { "shd", "fx", "dress" };
    int n = 0;
    for (int ci = 0; ci < GDX_NRRFX_CATS; ci++) {
        FxCat& C = sFx->cat[ci];
        if (C.wFrames == 0 && C.wLearn == 0 && C.wStale == 0) {
            continue;
        }
        if (n == 0) {
            n = snprintf(buf, cap, "[nrrfx] tasks=%u", tasks);
        }
        if (n > 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n),
                          " | %s frames=%u ents=%u draws=%u tris=%u rej=%u cull=%u learn=%u lfail=%u stale=%u "
                          "fallback=%u broken=%u",
                          kName[ci], C.wFrames, C.wEnts, C.wDraws, C.wTris, C.wRej, C.wCull, C.wLearn, C.wLearnFail,
                          C.wStale, C.wFallback, gdx3ds_nrr_fx_broken[ci]);
        }
        C.wFrames = C.wEnts = C.wDraws = C.wTris = C.wRej = C.wCull = 0;
        C.wLearn = C.wLearnFail = C.wStale = C.wFallback = 0;
    }
    if (n > 0 && static_cast<size_t>(n) < cap && &gdx3ds_nrr_shd_game_counts != nullptr) {
        unsigned long g[3];
        gdx3ds_nrr_shd_game_counts(g);
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | shd game learnseq=%lu stock=%lu ovf=%lu", g[0], g[1],
                      g[2]);
    }
    if (n > 0 && static_cast<size_t>(n) < cap && &gdx3ds_nrr_fx_game_counts != nullptr) {
        unsigned long g[3];
        gdx3ds_nrr_fx_game_counts(g);
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | fx game learnseq=%lu stock=%lu ovf=%lu", g[0], g[1],
                      g[2]);
    }
    if (n > 0 && static_cast<size_t>(n) < cap && &gdx3ds_nrr_dress_game_counts != nullptr) {
        unsigned long g[3];
        gdx3ds_nrr_dress_game_counts(g);
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | dress game learnseq=%lu stock=%lu ovf=%lu", g[0],
                      g[1], g[2]);
    }
    return n;
}

} // namespace gdxn

/* Render half of the shadow category's init (the game half, gdx3ds_nrr_shd_game.c, calls it). */
extern "C" void gdx3ds_nrr_dress_render_init(void) {
    for (int i = 0; i < 256; i++) {
        gdxn::nrrfx::sByteToUnit[i] = static_cast<float>(i) / 255.0f;
    }
    (void)gdxn::nrrfx::FxEnsure();
    gdx3ds_nrr_set_draw(NRR_CAT_DRESS, gdxn::nrrfx::NrrDressHotDraw);
}

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));

extern "C" void gdx3ds_nrr_fx_render_init(void) {
    for (int i = 0; i < 256; i++) {
        gdxn::nrrfx::sByteToUnit[i] = static_cast<float>(i) / 255.0f;
    }
    if (&gdx3ds_config_get_int != nullptr) {
        gdxn::nrrfx::sFxDumpTick = gdx3ds_config_get_int("native", "nrr_fx_dump", -1);
    }
    (void)gdxn::nrrfx::FxEnsure();
    gdx3ds_nrr_set_draw(NRR_CAT_FX, gdxn::nrrfx::NrrFxHotDraw);
}

extern "C" void gdx3ds_nrr_shd_render_init(void) {
    for (int i = 0; i < 256; i++) {
        gdxn::nrrfx::sByteToUnit[i] = static_cast<float>(i) / 255.0f;
    }
    (void)gdxn::nrrfx::FxEnsure();
    gdx3ds_nrr_set_draw(NRR_CAT_SHD, gdxn::nrrfx::NrrShdHotDraw);
}
