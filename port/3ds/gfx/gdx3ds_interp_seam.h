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

/* --- [native] machines_mattab: one material run's recorded resolve (C5 Stage 1b) --------- */

/* Opaque per-run record storage the mesh code owns (all-zero = empty). The owned interpreter
 * keeps a gdxn::MachMatRec in it (gdx_seam_owned.cpp asserts it fits). */
#if defined(__LP64__) || defined(_WIN64)
#define GDXSEAM_MATREC_BYTES 384 /* host builds (64-bit pointers) */
#else
#define GDXSEAM_MATREC_BYTES 328 /* the 3DS: sizeof(gdxn::MachMatRec) on ARM32, exactly */
#endif
struct GdxSeamMatRec {
    alignas(8) unsigned char bytes[GDXSEAM_MATREC_BYTES];
};
/* Records per run: the same (slot, LOD) list is drawn in more than one render mode per frame
 * (the body pass and the highlight pass), so each run keeps one record per mode. */
#define GDXSEAM_MATREC_WAYS 2
/* [native] machines_speck (c5mach2): the shared far speck inherits the state of whichever
 * machine was drawn before it (a different combiner/tile per slot), so its one run keeps more
 * ways. The upper bound a BindMaterialRec call may pass. */
#define GDXSEAM_MATREC_WAYS_MAX 8

/* == gdxn::MachMatResult / MachMatOutcome (asserted at the owned adapter). */
enum {
    GDXSEAM_MM_HIT = 0,
    GDXSEAM_MM_MISS_EMPTY,
    GDXSEAM_MM_MISS_KEY,
    GDXSEAM_MM_MISS_VP,
    GDXSEAM_MM_MISS_NOIMP,
    GDXSEAM_MM_MISS_TEX,
    GDXSEAM_MM_MISS_EPOCH,
    GDXSEAM_MM_MISS_SMP,
    GDXSEAM_MM_FULL,
    GDXSEAM_MM_RESULT_COUNT
};
struct GdxSeamMatOutcome {
    uint8_t result, binds, banked, checked, mismatch, wasValid;
    uint16_t keyBits; /* MM_MISS_KEY: which key parts differed (gdxn::MachMatOutcome) */
    uint8_t live;     /* [native] machines_mattab_live: units imported live by this hit (bit per unit) */
};
/* BindMaterialRec mode flag: a unit whose record holds no import hit (a non-immutable source:
 * the race setup's tile 0) imports live instead of missing. */
#define GDXSEAM_MATREC_LIVE 0x10

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
    /* [native] machines_mattab: BindMaterial through a run's `ways` records (mode 1 use, 2 verify).
     * 1 = bound, 0 = a state shape this path does not handle (as BindMaterial's false),
     * -1 = this interpreter keeps no records (the caller uses BindMaterial). */
    virtual int BindMaterialRec(GdxSeamMatRec* recs, int ways, int mode, GdxSeamMaterial& out,
                                GdxSeamMatOutcome& o) {
        (void)recs;
        (void)ways;
        (void)mode;
        (void)out;
        (void)o;
        return -1;
    }
    /* [native] track_mattab (C5 Stage 3d): the retained course's segment probe served from a
     * course material table of nBuckets x ways records (gdxn::Interp::TrackMatBefore/After).
     * `tri` = the probe triangle's three vertices as the interpreter's cache would hold them.
     * Before: 1 = served (state bound, packed batch open, aux latched: the probe must not run),
     * 2 = applied but the batch did not open (the probe's triangle is dropped), 3 = applied but
     * the stock probe must still run (pool nearly out), 4 = the triangle binds and draws nothing
     * (dropped), 0 = not served (probe, then After), -1 = this interpreter keeps no records.
     * After (probe: 0 resolved, 1 dropped, 2 drawn): bank, and in mode 2 compare. */
    virtual int TrackMatBefore(GdxSeamMatRec* table, int nBuckets, int ways, int mode, bool serve,
                               const GdxSeamVertex* tri, GdxSeamMatOutcome& o) {
        (void)table;
        (void)nBuckets;
        (void)ways;
        (void)mode;
        (void)serve;
        (void)tri;
        (void)o;
        return -1;
    }
    virtual void TrackMatAfter(GdxSeamMatRec* table, int nBuckets, int ways, int mode, int probe,
                               GdxSeamMatOutcome& o) {
        (void)table;
        (void)nBuckets;
        (void)ways;
        (void)mode;
        (void)probe;
        (void)o;
    }
    /* [native] machines: everything GfxSpVertex reads from the RSP, latched once per draw. */
    virtual bool LatchVtxState(Gdx3dsVtxMathState& out) = 0;
    /* The per-draw constants Flush() applies before every submit (prim depth, alpha-compare
     * threshold) from the current registers. */
    virtual void ApplyDrawConstants() = 0;
    /* n60retry: the registers hold a viewport the backend does not have yet (the next triangle's
     * state resolve would set it, with the scissor), and putting both there now. */
    virtual bool ViewportPending() const {
        return false;
    }
    virtual void ApplyViewportScissor() {}
    /* The scissor the registers hold (x, y, w, h as the backend's SetScissor takes them): what a
     * triangle drawn now is clipped to. false = not known (the caller takes the backend's). */
    virtual bool RegisterScissor(int32_t out[4]) const {
        (void)out;
        return false;
    }
    /* n60finish [native] finish_mesh: execute commands [begin, end) of the list the G_DL hook is
     * being offered (valid inside the hook call only) through the walker's own handlers, in order,
     * as if the walker had reached them. The caller passes no G_DL / G_ENDDL / branch.
     * 0 = this interpreter cannot (the caller keeps the list on the walker). */
    virtual int ExecHookCmds(size_t begin, size_t end) {
        (void)begin;
        (void)end;
        return 0;
    }
    /* The same for the n listed command indices, in the order given. */
    virtual int ExecHookCmdList(const uint16_t* idx, size_t n) {
        (void)idx;
        (void)n;
        return 0;
    }
    /* n60finish: the backend binding the LAST BindMaterial left behind (depth, decal, alpha, shader,
     * each used unit's texture-cache slot + sampler), saved in slot 0..1, and re-applied later in
     * the same list without the texture loads that produced it -- exactly the binds a stock
     * ResolveDrawState would issue for that state (diffed against what the backend holds). Apply
     * returns 0 (nothing touched) when the slot is empty or a saved unit's cache entry no longer
     * holds the key it was saved with. Reset empties both slots. */
    virtual int BindingSave(int slot) {
        (void)slot;
        return 0;
    }
    virtual int BindingApply(int slot) {
        (void)slot;
        return 0;
    }
    virtual void BindingReset() {}

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
/* [native] gadget_batch (C5 Stage 3b): the gadget run marker (p=0x6D end / 0x6E verify start). */
void gdx3ds_gad_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1);
/* [native] dress_retained (C5 Stage 3b): G_DL hook inside the decoration scope. `listPtr` is the
 * resolved narrow (8-byte packet) decoration list; the machine registry bakes it into a dress
 * entry on first sight and draws it like a machine mesh. Returns 1 when drawn natively (the
 * caller skips the list), 0 to walk it (not bakeable: a texture setup list, a load inside). */
int gdx3ds_mesh_dress_hook_seam(GdxInterpSeam& I, const void* listPtr, size_t listCmds, int stride, int isBig,
                                Gdx3dsMeshResolveFn resolve, void* user);
/* [native] machines_speck (c5mach2): draw the resolved LOD-6 far speck natively; 1 = taken.
 * `packets` = the same list re-packed as host-built 16-byte packets (the bake walker's input). */
int gdx3ds_mesh_speck_hook_seam(GdxInterpSeam& I, const void* listPtr, const void* packets,
                                Gdx3dsMeshResolveFn resolve, void* user);
/* nimach: 1 when a settings reflection model may draw from the dress registry: it is registered
 * already, or fewer than half the entries are taken (the models never crowd out a race's decorations:
 * entries are never freed). */
int gdx3ds_mesh_dress_model_ok(const void* listPtr);
/* nimach: the interpreter state a machine body entry's stock draw leaves behind (every run's texture /
 * combine / prim / env words, in order, as DrawMesh applies them) -- applied without drawing, after an
 * NRR menu entry, so the walk after the marker sees the stock registers (a body list sets its own env
 * colour in its last runs). No-op when the entry has no live mesh. Drawing thread. */
void gdx3ds_mesh_apply_tail(GdxInterpSeam& I, int entryIdx, const uint8_t* envRgb /* the stock gDPSetEnvColor before the list (alpha 255), or NULL */);
/* n60finish [native] finish_mesh: the G_DL the p=0x6A marker armed (Menus_DrawFinish's aFinishDL,
 * the spinning 3D FINISH banner). The narrow list is baked once into a GPU-resident mesh split at
 * its texture loads and drawn with the GPU transform; only the first load of each of its two
 * textures runs per frame. Returns 1 when drawn (the caller skips the list), 0 to walk it. */
int gdx3ds_mesh_finish_hook_seam(GdxInterpSeam& I, const void* listPtr, size_t listCmds, int stride, int isBig,
                                 Gdx3dsMeshResolveFn resolve, void* user);
/* 1 when [native] finish_mesh is on and the GPU transform can take it (latches the mesh ini keys). */
int gdx3ds_mesh_finish_enabled(void);

/* nicourse [native] ni_course: one course model of the 3D course preview -- Course_DrawModel's G_VTX /
 * G_TRI2 run after its G_NOOP p=0x6B marker pair (gdx_nicourse.cpp, gdx3ds_course_game.c). */
struct Gdx3dsCourseRun {
    const uint8_t* words; /* the run's first command in the walker's list */
    uint32_t stride;      /* the list's packet stride (16: host-built) */
    uint32_t nCmds;       /* the run's commands */
    uint32_t vtxW1;       /* the model's vertex buffer (the marker's w1: the G_VTX operand's low word) */
    uint32_t vtxCount;    /* its vertex count */
    uint32_t gen;         /* the buffer's content generation (game side) */
    int offscreen;        /* the viewport misses the scissor / window: no pixel can be drawn */
};
/* A baked model as the walker's hot path draws it: the resident raw Vtx + u16 indices (stock triangle
 * order), the object box, the run-relative G_VTX the leaving state re-runs. */
struct Gdx3dsCourseModel {
    const void* verts;
    const uint16_t* idx;
    uint32_t nIdx;
    float lo[3], hi[3];
    uint32_t tail[2]; /* the second-to-last and the last G_VTX (run-relative, ~0u none) */
};
/* What the cold draw bound, for the walker's material memo (round 2): the gpuxf uniform block (28 vec4,
 * Gdx3dsXfDraw from mvp to off), the combiner inputs, and the facts the memo is valid under. */
struct Gdx3dsCourseCold {
    int drawn;          /* 1 = a native draw went out (the rest is filled) */
    float xf[28][4];
    float in[7][4];
    int numInputs;
    int cullMode;       /* GDX3DS_CULL_* after G_EX_INVERT_CULLING */
    int xMode;          /* the vertex math x mode (classification) */
    float aspect;
    int memoOk;         /* no texture, fog, lighting or texgen: the block is a function of the key */
};
/* The window counters (the mesh module prints them; the walker's hot path adds to them). */
struct Gdx3dsCourseWin {
    unsigned frames, models, drawn, nrr, cold, memos, memoFails, offscreen, outside, straddle, hits, bakes, fails;
    unsigned fbWords, fbVs, fbClip, fbErr, fbBind, fbOpen;
    unsigned long tris, verts;
    unsigned maxErrMpx;
    unsigned vfyModels, vfyBad, vfyWords, vfyMiss;
    unsigned long long ticks;
};
extern Gdx3dsCourseWin gdx3ds_course_win;
extern int gdx3ds_course_frame_models; /* models seen since the last frame boundary */
/* 0 off, 1 on, 2 verify (render-side latch; needs gpuxf and the mesh module). */
int gdx3ds_mesh_course_level(void);
/* gpuxf_maxerr (the float24 bound, mpx; < 0 = off), latched with the level. ni_course_clip (always
 * on: a model straddling its clip volume draws through the PICA clipper; no documented use of the
 * off path -- removed, W6 Fable review) used to be read through a sibling getter here. */
int gdx3ds_mesh_course_maxerr(void);
/* Hot: the model of the run's key (baked on first sight), its words checked against the baked copy.
 * NULL = walk the run (not bakeable, a word differs). */
const Gdx3dsCourseModel* gdx3ds_mesh_course_model(GdxInterpSeam& I, const Gdx3dsCourseRun& run,
                                                  Gdx3dsMeshResolveFn resolve, void* user);
/* Cold: the round-1 draw of a model (gpuxf gates, ResolveDrawState bind, the backend's xf draw).
 * 1 = taken (drawn, or nothing to draw); 0 = nothing touched. `out` says what was bound. */
int gdx3ds_mesh_course_cold(GdxInterpSeam& I, const Gdx3dsCourseModel* m, Gdx3dsCourseCold* out);
/* ni_course=2: compare the baked copy (words + vertex bytes) of the run's key with the live ones. */
void gdx3ds_mesh_course_verify(const Gdx3dsCourseRun& run, Gdx3dsMeshResolveFn resolve, void* user);

#endif /* GDX3DS_INTERP_SEAM_H */
