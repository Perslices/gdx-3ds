/* gdx3ds_mesh.h — native retained machine meshes ([native] machines, plan 07).
 *
 * The game builds every in-race machine body ONCE into a static list
 * (racer.c D_800CDDB0[slot*6+lod], Gfx[126]) and re-walks it through the bridge +
 * interpreter every frame. This module bakes such a list lazily into material runs +
 * in-place vertex chunks + u16 indices and draws it with CPU T&L (gdx3ds_vtx_math.h,
 * bit-matching the interpreter) and one indexed PICA draw per run.
 *
 * Two hook points (see docs/research/native/machines-progress.md, design delta 1):
 *   bridge   gdx3ds_mesh_bridge_lookup() at G_DL conversion time redirects a registered
 *            (and baked) list to a stable 1-command stub list;
 *   LUS      gdx3ds_mesh_dl_hook() from gfx_dl_handler_common draws the mesh from the
 *            interpreter's live state when the stub is reached.
 * Under the owned interpreter ([native] interp=1) there is no conversion pass: its G_DL
 * hook (n64/gdx_rt_glue.cpp) hands the game's own registered list to
 * gdx3ds_mesh_dl_hook_seam (gdx3ds_interp_seam.h), which bakes it lazily with the walker's
 * vertex resolver and draws it through the same code.
 *
 * Killswitch [native] machines=0 (default): both hooks are no-ops and no list is ever
 * redirected. Receipt: `[native] machines=...` every 64 frames. */
#ifndef GDX3DS_MESH_H
#define GDX3DS_MESH_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_mesh_walk.h" /* Gdx3dsMeshResolveFn */

#ifdef __cplusplus
extern "C" {
#endif

/* Bridge side (port/n64_gfx_bridge.cpp, kOpDl of a host-built list). `dlHostPtr` is the
 * resolved host pointer of the sub-list; `resolve`/`user` reproduce the bridge's own G_VTX
 * pointer resolution for a lazy bake. Returns the interpreter-visible stub list pointer
 * to emit instead of the converted list, or 0 (not registered / masked / bake failed /
 * feature off) to keep the legacy conversion. */
uintptr_t gdx3ds_mesh_bridge_lookup(uint64_t dlHostPtr, Gdx3dsMeshResolveFn resolve, void* user);

/* Interpreter side (vendored interpreter.cpp gfx_dl_handler_common; defined in
 * gdx3ds_seam_lus.cpp). `interpreter` is the Fast::Interpreter*, `subGfx` the G_DL target
 * after SegAddr. Returns 1 when the target was a mesh stub and the mesh was drawn (the caller
 * skips the list), else 0. */
int gdx3ds_mesh_dl_hook(void* interpreter, void* subGfx);

/* Frame cadence for the receipt line; called from the backend's StartFrame. */
void gdx3ds_mesh_frame_begin(void);

/* Generation bump: the game rebuilt one or more D_800CDDB0 lists (racer.c
 * func_8008D3C4/D5F4/D97C via port/decomp_port.c gdx_native_machine_lists_rebuilt). */
void gdx3ds_mesh_lists_rebuilt(void);

/* [native] machines_texskip queries (owned walker, n64/gdx_texskip.cpp; C5 Stage 1).
 * texlist_slot: the slot whose texture-set list (racer.c D_800CDD38[slot]) `list` is, else -1.
 * is_speck: 1 = the LOD-6 far-speck list every slot shares (D_800CDDB0[slot*6+5]).
 * body_probe: 1 when `body` is a registered machine list the G_DL hook will draw natively
 *   (baking it now if needed), with its slot and the texture tiles its runs can sample given
 *   the tile and combiner texel use it inherits (inhUse: bit0 cycle-1 TEXEL0, bit1 cycle-1
 *   TEXEL1, bit2 any texel in cycle 2).
 * generation: the machine-list generation (bumped by every list rebuild). */
int gdx3ds_mesh_texlist_slot(const void* list);
int gdx3ds_mesh_is_speck(const void* list);
int gdx3ds_mesh_is_body(const void* list); /* registered machine body list (registry only, no bake) */
int gdx3ds_mesh_body_probe(const void* body, Gdx3dsMeshResolveFn resolve, void* user, uint8_t inhTile, uint8_t inhUse,
                           int* slot, uint8_t* tiles);
uint32_t gdx3ds_mesh_generation(void);
/* [native] nrr_deco: changes whenever a decoration (dress) entry is baked again. */
uint32_t gdx3ds_mesh_dress_epoch(void);
/* [debug] machsplit: monotone ticks spent in the native mesh draw (drawing thread). */
uint64_t gdx3ds_mesh_draw_ticks_total(void);
/* [native] machines_speck (c5mach2): 1 when the far speck is drawn as a native retained mesh. */
int gdx3ds_mesh_speck_enabled(void);

/* Backend entry (gfx_citro3d.cpp): reserve room for up to maxVerts packed vertices
 * (GDX3DS_VBOPACK_STRIDE floats each) + maxIndices u16 in the per-frame linear pool
 * and fill the batch pack parameters for the CURRENT shader/texture state; NULL = refuse.
 * Then draw: nVerts written at the cursor, indices copied from `idx`. */
struct Gdx3dsVboPackParams;
struct Gdx3dsVboPackAux;
float* gdx3ds_mesh_backend_begin(struct Gdx3dsVboPackParams* params, uint32_t minVerts, uint32_t maxIndices,
                                 uint32_t* capVerts); /* capVerts: vertex slots actually available */
void gdx3ds_mesh_backend_draw(uint32_t nVerts, const uint16_t* idx, uint32_t nIdx, const struct Gdx3dsVboPackAux* aux);
unsigned long gdx3ds_mesh_backend_linear_free_kib(void);

/* [native] gpuxf (o3dsgpuxf-progress.md, default OFF): the per-run shader state of a
 * GPU-transformed mesh run. The vertex stream is the mesh's RAW 16-byte N64 Vtx (copied once
 * per mesh draw into the linear pool) and shader.v.pica's gpuxf proc runs the transform.
 * Every vec4 below is uploaded to the shader uniform of the same name. */
typedef struct Gdx3dsXfDraw {
    const void* verts; /* gdx3ds_mesh_backend_xf_vertices() result */
    uint32_t nVerts;
    float mvp[4][4];   /* xmvp[i] = (mp[0][i], mp[1][i], mp[2][i], mp[3][i]) */
    float conv[4];
    float convw[4];
    float fog[4];
    float amb[4];
    float unlit[4];
    float ldir[3][4];
    float lcol[3][4];
    float look[2][4];
    float tg[4];
    float tcs[4];
    float uva[2][4];
    float uvc[2][4];
    float col[4];
    float sel[4];
    float seli[4];
    float flx[4];  /* [native] machines_retained: (alphaLight.xyz, flxOn); flxOn=0 keeps cn.a */
    float off[4];  /* [native] track_resident: (-eye.xyz, 0) added to ob before the MP multiply
                      (integers: exact in float24); 0 for every other gpuxf draw */
    float clsW;   /* packed clip w of the first surviving vertex (stereo / DISP classification) */
    int cullMode; /* GDX3DS_CULL_NONE / FRONT / BACK, G_EX_INVERT_CULLING already applied */
    /* [native] machines_retained: the indices already live in linear memory next to the
     * resident vertices (the mesh's own index buffer), so the draw references them in place
     * instead of copying them into the frame pool. */
    int idxResident;
    /* [native] machines_mattab: 1 = the backend skips re-issuing GPU state identical to what it
     * last issued itself (vertex buffer, a unit's texture binding, the alpha test); 2 = verify
     * (re-issue everything, count what would have been skipped); 0 = always re-issue. */
    int stateElide;
} Gdx3dsXfDraw;

/* n60 nrr_trk (gdx3ds_mesh_nrr_material): what a cached material needs to re-evaluate its
 * per-frame constants (prim/env colour inputs, fog colour) from the registers without a bind. */
typedef struct GdxNrrMatInfo {
    const void* prg;           /* the backend shader program the bind made current */
    uint8_t inputMap[2][7];    /* ColorCombiner::shader_input_mapping */
    uint8_t numInputs;
    int8_t ci, ai;             /* the inputs riding the vertex colour / alpha (-1 none) */
    uint8_t colConst;          /* xf.col rgb = input ci (a non-shade input) */
    uint8_t alphaConst;        /* xf.col alpha = input ai's alpha */
    uint8_t useAlpha, useFog, useBlendColor;
    uint8_t fogFactorConst;    /* xf.col alpha = fog_color.a */
} GdxNrrMatInfo;

/* Reserve nVerts * 16 bytes of the frame's linear pool for a mesh's raw vertices; NULL =
 * refuse (the caller takes the CPU path). */
void* gdx3ds_mesh_backend_xf_vertices(uint32_t nVerts);
/* Pack parameters of the CURRENT shader/texture state without reserving vertex room;
 * 0 = refuse. */
int gdx3ds_mesh_backend_xf_params(struct Gdx3dsVboPackParams* params);
/* One indexed GPU-transform draw of `nIdx` mesh-global indices into xf->verts. */
void gdx3ds_mesh_backend_xf_draw(const uint16_t* idx, uint32_t nIdx, const struct Gdx3dsVboPackAux* aux,
                                 const Gdx3dsXfDraw* xf);
/* 1 when the linked shader carries the gpuxf uniforms (latched at backend init). */
int gdx3ds_mesh_backend_xf_ready(void);
/* [native] machines_retained: make `bytes` of CPU-written linear memory visible to the GPU
 * (the resident mesh buffers are written once per bake, outside the frame pool's flush). */
void gdx3ds_mesh_backend_flush_resident(const void* p, uint32_t bytes);
/* [native] machines_mattab: state the backend did not re-issue for stateElide draws since the
 * last call (out[0] vertex buffer, out[1] texture binds, out[2] alpha tests); resets them. */
void gdx3ds_mesh_backend_elide_take(unsigned out[3]);

/* [native] machines_verify: the backend hands every draw's FINAL packed stream (+ the
 * material identity it would bind) to a capture callback right after the repack; with
 * skipSubmit the draw stops there (no GPU state, no submit, pool not advanced). */
typedef struct Gdx3dsMeshCapture {
    const float* verts;   /* GDX3DS_VBOPACK_STRIDE floats each */
    uint32_t nVerts;
    const uint16_t* idx;  /* NULL = arrays (numTris * 3 sequential vertices) */
    uint32_t nIdx;
    uint32_t numTris;
    uint64_t shaderId0, shaderId1;
    uint32_t texId[2];    /* 0xFFFFFFFF = unit unused */
    const struct Gdx3dsVboPackAux* aux; /* NULL on the legacy variable-stride path */
    int8_t vtxColorInput, vtxAlphaInput; /* 1-based inputs riding the vertex attribute (0 = none) */
} Gdx3dsMeshCapture;
typedef void (*Gdx3dsMeshCaptureFn)(const Gdx3dsMeshCapture* cap, void* user);
void gdx3ds_mesh_backend_set_capture(Gdx3dsMeshCaptureFn fn, void* user, int skipSubmit);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MESH_H */
