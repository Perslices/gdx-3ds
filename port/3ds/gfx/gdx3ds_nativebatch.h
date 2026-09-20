/* gdx3ds_nativebatch.h — Gdx_NativeBatch: the shared native draw primitive (08-track-plan.md
 * §3.7). Consumers: the [native] track path (this branch, per-frame pool, state adopted from
 * the interpreter) and the machines track (explicit material). Implemented in gfx_citro3d.cpp
 * because it writes the backend's linear VBO pool cursor and submits through DrawPackedBatch,
 * so every early-out, diagnostic, stereo eye loop and viewport fold of the stock draw path
 * applies unchanged.
 *
 * Model: a batch is a contiguous range [start, cursor) of packed PICA vertices
 * (GDX3DS_VBOPACK_STRIDE floats each: pos4 uv0 rgba uv1, already clip-space, UV-scaled and
 * clamped) in the per-frame pool. Flush() issues ONE draw for the range with the state that is
 * CURRENT in the backend at that moment plus the aux record given at Begin. Ranges are never
 * reordered: a caller that needs painter's order (Z_CMP off) flushes at every state change.
 *
 * Interpreter-state caveat for Material(): it drives the backend directly, so the interpreter's
 * own mirror of depth/alpha/shader state (Interpreter::mRenderingState) goes stale. Use it only
 * from a point after which the interpreter re-resolves state (a G_SETOTHERMODE/G_SETCOMBINE or
 * a Flush-splitting opcode follows) or with state equal to the interpreter's current state.
 * The track path never calls it: its state is the interpreter's (a first triangle per run goes
 * through GfxSpTri1). */
#ifndef GDX3DS_NATIVEBATCH_H
#define GDX3DS_NATIVEBATCH_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_vbopack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Gdx3dsNativeMaterial {
    uint64_t shaderId0; /* interpreter combiner id + option bits, as LookupShader takes them */
    uint64_t shaderId1;
    uint32_t texId[2];  /* backend texture ids (NewTexture/UploadTexture); bound where the shader samples */
    uint8_t depthTest;  /* Z_CMP  -> SetDepthTestAndMask(depthTest, depthMask) */
    uint8_t depthMask;  /* Z_UPD */
    uint8_t decal;      /* ZMODE_DEC */
    uint8_t useAlpha;   /* blender reads memory colour (translucency) */
    Gdx3dsVboPackAux aux; /* prim/env/fog constants of the batch (vertex-0 record) */
} Gdx3dsNativeMaterial;

/* Open a batch at the pool cursor with the backend's CURRENT shader/textures/depth state and a
 * neutral aux record. Returns 0 (refused) when no frame/shader is active. An open batch is
 * flushed first. stereoClass: GDX3DS_STEREO_* (gdx3ds_stereo.h), -1 = backend heuristic. */
int Gdx_NativeBatchBegin(int stereoClass);
/* Same, with an explicit aux record (the track passes the interpreter's batch record). */
int Gdx_NativeBatchBeginWithAux(const Gdx3dsVboPackAux* aux, int stereoClass);
/* Apply an explicit material to the backend, then BeginWithAux(&m->aux). */
int Gdx_NativeBatchMaterial(const Gdx3dsNativeMaterial* m, int stereoClass);
int Gdx_NativeBatchOpen(void);

/* Next packed vertex slot (write GDX3DS_VBOPACK_STRIDE floats per vertex), NULL if no batch. */
float* Gdx_NativeBatchCursor(void);
/* Reserve nVerts at the cursor and return the first slot; NULL when the pool is exhausted
 * (nothing reserved). */
float* Gdx_NativeBatchVerts(uint32_t nVerts);
/* nVerts were written at the cursor by another writer (the interpreter's own packed append
 * lands at the pool cursor): advance past them. */
void Gdx_NativeBatchCommit(uint32_t nVerts);
/* Indexed draws are not built (plan M4): always NULL, callers emit expanded triangles. */
uint16_t* Gdx_NativeBatchIndices(uint32_t nIndices);
/* One draw for [start, cursor); the batch stays open at the new cursor. Returns tris drawn. */
uint32_t Gdx_NativeBatchFlush(void);
/* Verify support: FNV-1a over the pending range's bytes (into *hash, seeded by the caller),
 * then drop the range (cursor back to start). Returns tris dropped. */
uint32_t Gdx_NativeBatchDiscard(uint32_t* hash);
/* Flush and close. */
void Gdx_NativeBatchEnd(void);

/* Verify support: while armed, every packed draw the backend submits (DrawPackedBatch) is
 * hashed into the stock accumulator. StockHash returns it (and the tri count); reset != 0
 * re-seeds both. Draws issued by Gdx_NativeBatchFlush are hashed too, so verify callers
 * discard rather than flush. */
void Gdx_NativeBatchStockHashArm(int on);
uint32_t Gdx_NativeBatchStockHash(uint32_t* trisOut, int reset);
uint32_t Gdx_NativeBatchHashBytes(const void* p, size_t n, uint32_t h);
#define GDX_NATIVEBATCH_HASH_SEED 2166136261u

/* Which combiner inputs of the CURRENT shader ride as TexEnv stage constants (bit j = input
 * j, 0-based; rgb and alpha channels separately). Returns 0 when no shader is current. The
 * complement is what the vertex colour attribute carries per vertex — the fold test of the
 * effects replay (a register change is batch-transparent iff no constant reads it). */
int Gdx_NativeBatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask);

/* Receipts: draws issued by Flush since the last drain. */
uint32_t Gdx_NativeBatchDrainDraws(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NATIVEBATCH_H */
