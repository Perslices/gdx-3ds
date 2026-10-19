/* gdx3ds_hsx.h -- [native] hudsky_emit: the compact emitter's backend program (hudsky,
 * docs/research/native/hudsky-progress.md).
 *
 * A retained HUD run (c5hud3 hud_retain) used to be replayed as a list of backend calls, one
 * virtual call each, and every batch it drained went through the general DrawTriangles tail
 * (11.5 KB of code, most of it diagnostics). The compact emitter hands the backend the whole
 * run as ONE program: a small array of ops (state setters, batch begins, float slices, draws)
 * that the backend executes in one tight loop with its own setters inlined, draining each
 * batch through a lean packed-draw tail that carries only what a steady-state packed draw
 * executes. The ops are the recorded backend calls, in the recorded order, with the recorded
 * arguments; the floats are the recorded (or, for a variant hit, re-packed) floats. Nothing is
 * merged, reordered or re-sampled, so the GPU sees the same commands.
 *
 * The program is position-independent: float slices and aux records are indices into the
 * arrays passed with it, so a variant hit that rewrites floats in place needs no recompile. */
#ifndef GDX3DS_HSX_H
#define GDX3DS_HSX_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_vbopack.h"

#ifdef __cplusplus
extern "C" {
#endif

enum Gdx3dsHsxOpCode {
    GDX3DS_HSX_UNLOAD = 0, /* p: shader */
    GDX3DS_HSX_LOAD,       /* p: shader */
    GDX3DS_HSX_SELTEX,     /* i0 unit, i1 texture id */
    GDX3DS_HSX_SAMPLER,    /* i0 unit, a linear, i1 cms, i2 cmt */
    GDX3DS_HSX_DEPTH,      /* a test, b mask */
    GDX3DS_HSX_DECAL,      /* a */
    GDX3DS_HSX_VIEWPORT,   /* i0 x, i1 y, i2 w, i3 h */
    GDX3DS_HSX_SCISSOR,    /* i0 x, i1 y, i2 w, i3 h */
    GDX3DS_HSX_ALPHA,      /* a */
    GDX3DS_HSX_PRIMDEPTH,  /* f (the interpreter patches a live value in before the call) */
    GDX3DS_HSX_ALPHATHR,   /* f (likewise) */
    GDX3DS_HSX_FOGNOTE,    /* i0 mul, i1 off, i2 exact */
    GDX3DS_HSX_CLEARDEPTH, /* i0 x, i1 y, i2 w, i3 h */
    GDX3DS_HSX_ATLASARM,   /* a */
    GDX3DS_HSX_BEGIN,      /* a new packed batch: the cursor moves to the pool's free tail */
    GDX3DS_HSX_FLOATS,     /* i0 first triangle in the batch, i1 triangles, i2 float index */
    GDX3DS_HSX_DRAW,       /* i0 triangles, i1 aux index */
    GDX3DS_HSX_COUNT
};

typedef struct Gdx3dsHsxOp {
    uint8_t op;
    uint8_t a, b, c;
    int32_t i[4];
    float f;
    const void* p;
    uint8_t screen; /* DRAW: the batch was begun by this run (C6 tag = screen space), else the live tag */
} Gdx3dsHsxOp;

/* One run's program. `cursor` in: the open batch's pool pointer at the run's entry (NULL when
 * none is open); out: the pool pointer of the batch left open (NULL when none). */
typedef struct Gdx3dsHsxProgram {
    const Gdx3dsHsxOp* ops;
    uint32_t nOps;
    const float* floats;
    const Gdx3dsVboPackAux* auxs;
    float* cursor;
    uint16_t liveTag; /* C6 [replay]: the interpreter's batch tag at the run's entry */
    uint32_t begins; /* out: batches begun */
    uint32_t draws;  /* out: batches drawn */
} Gdx3dsHsxProgram;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HSX_H */
