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
