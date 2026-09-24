/* gdx3ds_fx_record.h — [native] fx: the per-GfxPool op record shared by the game-side sink
 * (gdx3ds_fx_sink.c, writer, Racer_Draw's effects bracket) and the interpreter-side replay
 * (gdx3ds_fx_replay.cpp, reader, Interpreter::GdxFxMarker). Same double-buffer contract as the
 * track record: the game thread fills pool N while the render thread replays pool N^1. */
#ifndef GDX3DS_FX_RECORD_H
#define GDX3DS_FX_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXFX_MAGIC 0x46583031u /* "FX01" */
/* Worst case per frame: 40 explosions x 4 ops + 128 debris x 2 + 32 sparks x 2 + 32 cspark x
 * 5 + 32 smoke x 4 + 30 force field x 3 + 64 fall x 5 ~ 1100; four viewports fit in 4096. */
#define GDXFX_MAX_OPS 4096
#define GDXFX_MAX_RUNS 512

/* G_NOOP marker: w0 = (G_NOOP<<24) | (p<<16) | runIndex, w1 = GdxFxRecord*. Track uses
 * 0x5A/0x5B; fx takes the next pair. */
#define GDXFX_MARKER_P_END 0x5C   /* replay runs[runIndex] here */
#define GDXFX_MARKER_P_START 0x5D /* verify mode: snapshot the vertex cache + registers here */

/* Effect classes (by setup display list), in stream order. */
enum {
    GDXFX_CLS_EXPLOSION = 0, /* D_4007FB8: gExplosions1 (32-ring) + gExplosions2 (8-ring) */
    GDXFX_CLS_DEBRIS = 1,    /* D_4007FD8: gMachineDebris (128-ring), 3 verts, vertex colour */
    GDXFX_CLS_SPARK = 2,     /* D_4008000: gFlyingSparks (32-ring), 3 verts */
    GDXFX_CLS_CSPARK = 3,    /* D_4008028: gCollisionSparks (32-ring), LOD tile per sprite */
    GDXFX_CLS_SMOKE = 4,     /* D_4008130: gSmokes (32-ring), LOD tile per sprite */
    GDXFX_CLS_PITFF = 5,     /* aSetupPitForceFieldDL: per racer (not a ring; inside the bracket) */
    GDXFX_CLS_FALLX = 6,     /* aSetupFallExplosionDL: gFallExplosions (32-ring), two passes */
    GDXFX_CLS_OTHER = 7,     /* any other list inside the bracket (never expected) */
    GDXFX_CLASS_COUNT = 8
};

enum { GDXFX_OP_VTX = 1, GDXFX_OP_TRI = 2, GDXFX_OP_PRIM = 3, GDXFX_OP_ENV = 4, GDXFX_OP_TEX = 5 };

typedef struct GdxFxOp {
    uint8_t kind;
    uint8_t a; /* VTX: n | TRI: v0 | PRIM/ENV: r | TEX: tile */
    uint8_t b; /* VTX: v0 | TRI: v1 | PRIM/ENV: g | TEX: level */
    uint8_t c; /* TRI: v2 | PRIM/ENV: b | TEX: on */
    uint32_t arg; /* VTX: const Vtx* | PRIM: (lodfrac<<8)|alpha | ENV: alpha | TEX: (sc<<16)|tc */
} GdxFxOp;

typedef struct GdxFxRun {
    uint16_t opStart;
    uint16_t opEnd;
    uint8_t cls;
    uint8_t pad[3];
} GdxFxRun;

/* Written by the replay thread, read by the game thread on that pool's next begin. */
typedef struct GdxFxReplayStats {
    uint32_t markers;
    uint32_t runs;
    uint32_t vtxOps;
    uint32_t verts;
    uint32_t tris;      /* tris replayed */
    uint32_t fast;      /* appended by the fast path */
    uint32_t exact;     /* routed through GfxSpTri1 (first-of-batch, clipper) */
    uint32_t fallback;  /* tris that stayed in the interpreter batch (packing refused) */
    uint32_t draws;     /* native batch flushes */
    uint32_t foldPrim;  /* prim value changes absorbed into the vertex colour (no split) */
    uint32_t foldEnv;
    uint32_t splitPrim; /* batch splits, by reason */
    uint32_t splitEnv;
    uint32_t splitTile;
    uint32_t verifyOk;
    uint32_t verifyBad;
    uint32_t drawsByClass[GDXFX_CLASS_COUNT];
} GdxFxReplayStats;

typedef struct GdxFxRecord {
    uint32_t magic;
    uint32_t mode;  /* 1 native, 2 verify */
    uint32_t fold;  /* [native] fx_sprites: fold prim/env into the vertex colour where expressible */
    uint32_t census; /* [native] fxcensus: the replay logs the class's combiner routing once */
    /* [native] fxceiling — CENSUS ONLY, deliberately renders WRONG pixels. It measures the
     * draw-count ceiling of the two candidate B5 optimisations instead of arguing it:
     *   bit0  every prim/env value change folds (a perfect per-vertex carry of both registers)
     *   bit1  the explosion class does not break its run at the per-sprite frame load
     * Never a shippable path: with bit0 the stage constants are stale and with bit1 every
     * sprite samples the last frame loaded. `[fh]` MUST differ from the baseline — if it does
     * not, the lever did nothing. */
    uint32_t ceiling;
    uint32_t frame; /* gGameFrameCount the record belongs to */
    uint32_t nOps;
    uint32_t nRuns;
    GdxFxOp ops[GDXFX_MAX_OPS];
    GdxFxRun runs[GDXFX_MAX_RUNS];
    GdxFxReplayStats rp;
} GdxFxRecord;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_FX_RECORD_H */
