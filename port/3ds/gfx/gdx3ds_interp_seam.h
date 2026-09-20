/* gdx3ds_interp_seam.h — the interpreter-side contract every native replay consumer is
 * written against: the track marker replay (gdx3ds_track_replay.cpp), the effects marker
 * replay (gdx3ds_fx_replay.cpp), the shared exact/fast triangle path (gdx3ds_native_tri.cpp)
 * and the retained machine meshes (gdx3ds_mesh.cpp).
 *
 * Two implementations, one per interpreter:
 *   gdx3ds_seam_lus.cpp      the vendored LUS Fast::Interpreter (the [native] interp=0 path)
 *   n64/gdx_seam_owned.cpp   the owned gdxn::Interp ([native] interp=1)
 *
 * The consumers never include an interpreter header. Registers they read per vertex or per
 * op are LIVE VIEWS (GdxSeamRegs: raw pointers into the interpreter's own RSP/RDP mirrors,
 * whose addresses are stable for the interpreter's life); the actions — vertex loads, the
 * exact triangle path, texture/combiner/colour setters, flush, draw-state resolution — are
 * virtuals. The two interpreters keep their mirrors "shaped like the LUS structs wherever
 * parity depends on them" (06-interp-plan.md §3.4), so the field layouts below are asserted
 * against both at the adapters and never copied.
 *
 * This header is LUS-free and host-compilable. */
#ifndef GDX3DS_INTERP_SEAM_H
#define GDX3DS_INTERP_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_mesh_walk.h" /* Gdx3dsMeshResolveFn */
#include "gdx3ds_vbopack.h"

struct Gdx3dsVtxMathState;

/* --- shared value shapes (both interpreters' structs have exactly these layouts) --------- */

struct GdxSeamRgba {
    uint8_t r, g, b, a;
};

/* == Fast::LoadedVertex == gdxn::LoadedVertex (x y z w u v color clip_rej clip_out). */
struct GdxSeamVertex {
    float x, y, z, w;
    float u, v;
    GdxSeamRgba color;
    uint8_t clip_rej;
    uint8_t clip_out;
};

struct GdxSeamTileUv {
    uint8_t shifts, shiftt;
    float uls, ult;
};

struct GdxSeamClip {
    bool zIsFrom0To1;
    bool invertY;
};

/* GBI values shared by both interpreters (lus_gbi.h / gdx_gbi.h agree on every one). */
enum {
    GDXSEAM_CCMUX_PRIMITIVE = 3,
    GDXSEAM_CCMUX_SHADE = 4,
    GDXSEAM_CCMUX_ENVIRONMENT = 5,
    GDXSEAM_CCMUX_PRIMITIVE_ALPHA = 10,
    GDXSEAM_CCMUX_SHADE_ALPHA = 11,
    GDXSEAM_CCMUX_ENV_ALPHA = 12,
    GDXSEAM_CCMUX_LOD_FRACTION = 13,
    GDXSEAM_CCMUX_PRIM_LOD_FRAC = 14,
    GDXSEAM_CCMUX_KEY_CENTER = 20,
    GDXSEAM_CCMUX_KEY_SCALE = 21,
    GDXSEAM_CCMUX_CONVERT_K4 = 22,
    GDXSEAM_CCMUX_CONVERT_K5 = 23,
    GDXSEAM_ACMUX_PRIMITIVE = 3,
    GDXSEAM_ACMUX_SHADE = 4,
    GDXSEAM_ACMUX_ENVIRONMENT = 5,
    GDXSEAM_ACMUX_PRIM_LOD_FRAC = 6,
};
#define GDXSEAM_G_TL_LOD (1u << 16)               /* other_mode_l */
#define GDXSEAM_G_MDSFT_TEXTFILT 12               /* other_mode_h */
#define GDXSEAM_G_TF_BILERP (2u << GDXSEAM_G_MDSFT_TEXTFILT)
#define GDXSEAM_G_BL_CLR_BL 2                     /* other_mode_l >> 30 */
#define GDXSEAM_G_BL_CLR_FOG 3
#define GDXSEAM_G_EX_INVERT_CULLING 0x00000001u   /* extra_geometry_mode */
#define GDXSEAM_G_EX_WIDESCREEN_STRETCH 0x00000004u
#define GDXSEAM_G_MWO_POINT_ST 0x14

/* --- live register views ------------------------------------------------------------- */

struct GdxSeamRegs {
    /* RSP */
    GdxSeamVertex* vertices; /* loaded_vertices[kSeamMaxVertices + 4] (the RSP cache mirror) */
    uint32_t* geometry_mode;
    uint32_t* extra_geometry_mode;
    int16_t* fog_mul;
    int16_t* fog_offset;
    uint16_t* tex_scale_s; /* texture_scaling_factor.s/.t (U0.16) */
    uint16_t* tex_scale_t;
    /* RDP */
    GdxSeamRgba* prim_color;
    GdxSeamRgba* env_color;
    GdxSeamRgba* fog_color;
    GdxSeamRgba* blend_color;
    GdxSeamRgba* key_center;
    GdxSeamRgba* key_scale;
    int16_t* convert_k; /* [6] */
    uint8_t* prim_lod_fraction;
    uint8_t* first_tile_index;
    bool* textures_changed; /* [2] */
    uint32_t* other_mode_l;
    uint32_t* other_mode_h;
    uint64_t* combine_mode;
    uint16_t* prim_depth;
    bool* viewport_or_scissor_changed;
};

#define GDXSEAM_MAX_VERTICES 64 /* MAX_VERTICES == gdxn::kMaxVertices */
#define GDXSEAM_VERTEX_CACHE_COUNT (GDXSEAM_MAX_VERTICES + 4)

/* --- the resolved draw state of a triangle (GfxSpTri1's state block, minus the emission) -- */

struct GdxSeamMaterial {
    const uint8_t (*inputMap)[7]; /* ColorCombiner::shader_input_mapping[2][7] of the bound combiner */
    bool combUsedTextures[2];     /* ColorCombiner::usedTextures */
    void* prg;                    /* opaque shader program */
    uint32_t tm;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2], effective_tile[2];
    bool usedTextures[2];         /* ShaderGetInfo */
    uint8_t numInputs;
    GdxSeamClip clip;
    bool use_alpha, use_fog, use_blend_color;
};

/* --- the seam --------------------------------------------------------------------------- */

struct GdxTrackPackState; /* gdx3ds_track_interp.h */

class GdxInterpSeam {
  public:
    virtual ~GdxInterpSeam() {
    }

    const GdxSeamRegs& R() const {
        return mR;
    }

    /* G_VTX from a host-endian Vtx array: the interpreter's own transform, lighting, texgen,
     * fog and clip flags. */
    virtual void SpVertex(size_t n, size_t dest, const void* vtx) = 0;
    /* G_MODIFYVTX (where = GDXSEAM_G_MWO_POINT_ST). */
    virtual void SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val) = 0;
    /* gSPTexture. */
    virtual void SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) = 0;
    /* G_SETCOMBINE from its raw words, exactly as the interpreter's dispatch would run it
     * (including whatever draw-state bookkeeping that dispatch entry does). */
    virtual void SetCombineRaw(uint32_t w0, uint32_t w1) = 0;
    /* The prim/env HANDLERS (value-change flush semantics of the interpreter). */
    virtual void SetPrimColor(uint8_t m, uint8_t l, GdxSeamRgba c) = 0;
    virtual void SetEnvColor(GdxSeamRgba c) = 0;
    /* Draw the interpreter's own pending batch. */
    virtual void Flush() = 0;
    /* Triangles buffered in the interpreter's own batch. */
    virtual size_t PendingTris() const = 0;
    /* The interpreter's full triangle path (reject / clip / cull / state / packed append). */
    virtual void SpTri(uint8_t a, uint8_t b, uint8_t c) = 0;
    /* Adoption: the `appended` triangles SpTri just packed at the pool cursor (on top of the
     * `n0` that were pending before) now belong to a Gdx_NativeBatch — forget them. */
    virtual void PopAppended(size_t n0, size_t appended) = 0;
    /* Snapshot of the packed-batch latches + the last resolved tri state (fast-path prep). */
    virtual void PackState(GdxTrackPackState* out) = 0;
    /* F3DEX2 Reject / FZeroFlxReject screening applies to this task. */
    virtual bool RejectVariant() const = 0;
    /* Tile UV parameters (shift/ul) of a tile descriptor. */
    virtual void TileUv(uint32_t tile, GdxSeamTileUv* out) const = 0;

    /* [native] machines: GfxSpTri1's draw-state resolution against the CURRENT registers
     * (depth/decal/viewport, combiner, texture import + sampler, shader, blend), binding the
     * backend exactly as a triangle would. false = a state shape this path does not handle. */
    virtual bool BindMaterial(GdxSeamMaterial& out) = 0;
    /* [native] machines: everything GfxSpVertex reads from the RSP, latched once per draw. */
    virtual bool LatchVtxState(Gdx3dsVtxMathState& out) = 0;
    /* The per-draw constants Flush() applies before every submit (prim depth, alpha-compare
     * threshold) from the current registers. */
    virtual void ApplyDrawConstants() = 0;

  protected:
    GdxSeamRegs mR = {};
};

/* --- consumer entry points (interpreter-neutral) --------------------------------------- */

/* [native] track: the G_NOOP marker (p=0x5A end / 0x5B verify start). */
void gdx3ds_track_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1);
/* [native] fx: the G_NOOP marker (p=0x5C end / 0x5D verify start). */
void gdx3ds_fx_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1);
/* [native] machines: G_DL hook. `listPtr` is the resolved sub-list host pointer — a mesh stub
 * (the bridge's redirect, LUS path) or the game's own registered D_800CDDB0 list (owned walker,
 * which never converts). `resolve`/`user` reproduce the caller's G_VTX pointer resolution for a
 * lazy bake (NULL = the bridge did the redirecting: only stub targets draw). Returns 1 when the
 * mesh was drawn natively (the caller skips the list). */
int gdx3ds_mesh_dl_hook_seam(GdxInterpSeam& I, const void* listPtr, Gdx3dsMeshResolveFn resolve, void* user);

#endif /* GDX3DS_INTERP_SEAM_H */
