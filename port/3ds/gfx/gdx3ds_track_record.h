/* gdx3ds_track_record.h — [native] track: the per-GfxPool op record shared by the game-side
 * sink (gdx3ds_track_sink.c, writer) and the interpreter-side replay
 * (gdx3ds_track_replay.cpp, reader). Two records exist, indexed by the game's GfxPool
 * parity, so the game thread fills pool N while the render thread replays pool N^1 — the
 * same double-buffer contract the game's own gfxBuffer/courseVtxBuffer live under. */
#ifndef GDX3DS_TRACK_RECORD_H
#define GDX3DS_TRACK_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXTRK_MAGIC 0x54524B31u /* "TRK1" */
#define GDXTRK_MAX_OPS 5120      /* <=510 chunk loads + templates + 8 ST patches per chunk */
#define GDXTRK_MAX_RUNS 256      /* material runs (markers) per pool-frame */

/* G_NOOP marker: w0 = (G_NOOP<<24) | (p<<16) | runIndex, w1 = GdxTrkRecord*. */
#define GDXTRK_MARKER_P_END 0x5A   /* replay runs[runIndex] here */
#define GDXTRK_MARKER_P_START 0x5B /* verify mode: snapshot the vertex cache here */

enum { GDXTRK_OP_VTX = 1, GDXTRK_OP_MOD = 2, GDXTRK_OP_DL = 3 };

typedef struct GdxTrkOp {
    uint8_t kind;
    uint8_t n;    /* VTX: vertex count */
    uint8_t slot; /* VTX: first cache slot; MOD: the slot */
    uint8_t tmpl; /* DL: template index */
    uint32_t arg; /* VTX: const Vtx* (host pointer); MOD: packed ST */
} GdxTrkOp;

typedef struct GdxTrkRun {
    uint16_t opStart;
    uint16_t opEnd;
} GdxTrkRun;

/* Written by the replay thread, read by the game thread on that pool's next begin. */
typedef struct GdxTrkReplayStats {
    uint32_t markers;
    uint32_t runs;
    uint32_t vtxOps;
    uint32_t verts;
    uint32_t tris;      /* template tris walked */
    uint32_t fast;      /* tris appended by the fast path */
    uint32_t exact;     /* tris routed through GfxSpTri1 (first-of-batch, clipper) */
    uint32_t draws;     /* native batch flushes */
    uint32_t combines;  /* mid-template SETCOMBINE events applied */
    uint32_t fallback;  /* tris that stayed in the interpreter batch (packing refused) */
    uint32_t verifyOk;
    uint32_t verifyBad;
} GdxTrkReplayStats;

typedef struct GdxTrkRecord {
    uint32_t magic;
    uint32_t mode;  /* 1 native, 2 verify */
    uint32_t frame; /* gGameFrameCount the record belongs to */
    uint32_t nOps;
    uint32_t nRuns;
    GdxTrkOp ops[GDXTRK_MAX_OPS];
    GdxTrkRun runs[GDXTRK_MAX_RUNS];
    GdxTrkReplayStats rp;
} GdxTrkRecord;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_TRACK_RECORD_H */
