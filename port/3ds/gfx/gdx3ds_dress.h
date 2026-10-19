/* gdx3ds_dress.h -- C5 Stage 3b: the course dressing (gadgets + decorations) and the effect and
 * shadow texture loads, made cheap on the Old 3DS (docs/research/native/c5dress-progress.md).
 *
 * Three levers, each default 0, each needs [native] interp=1 and is off under interp_verify:
 *
 *   [native] gadget_batch  1 = the gadget pass of Course_GadgetsDraw (landmines, jumps, the course
 *                          effects: dash plates, pit / dirt / ice strips) is recorded by a GBI shim
 *                          (gdx3ds_gad_shim_begin.h) and replayed at a G_NOOP marker as native
 *                          batches (gdx3ds_gad_replay.cpp: exact first tri, fast rest, one draw per
 *                          material run); the pass's texture loads (the mine / course-effect setup
 *                          lists, the jump texture) are served by the direct-load path: resolved
 *                          once per course, copied into TMEM only when something reads it.
 *                          2 = verify (stock GBI too, per-run VBO hash compare; loads verified).
 *   [native] dress_retained 1 = the decoration bodies (gates, signs, buildings) are drawn through
 *                          the machine mesh registry (gdx3ds_mesh.cpp dress entries): baked once,
 *                          GPU-resident under machines_retained, material records under
 *                          machines_mattab; the passes' texture loads go direct. 2 = the same
 *                          draws with the scope's loads in verify mode (every verify leg keeps
 *                          all direct-load levels, hud_direct included, at 2: one TMEM ledger).
 *   [native] fxshd_direct  1 = the texture loads of the effects section (boost flames and the seven
 *                          class setup lists) and of the shadow runs go direct. 2 = verify.
 *
 * The game's draw functions keep running in full for their logic; only what they emit changes.
 *
 * G_NOOP markers (w0 = G_NOOP<<24 | p<<16 | low16), all written only while a lever is armed:
 *   p=0x6C  direct-load scope: low byte = GDXDRESS_SCOPE_* (0 closes the scope)
 *   p=0x6D  gadget run END (low16 = run index, w1 = GdxGadRecord*): replay the run here
 *   p=0x6E  gadget run START (verify only): snapshot the vertex cache and registers
 *   p=0x6A  n60finish finish_mesh: the next G_DL is the FINISH banner (gdx_finish_mesh_marker) */
#ifndef GDX3DS_DRESS_H
#define GDX3DS_DRESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXDRESS_MARKER_P_SCOPE 0x6C
#define GDXGAD_MARKER_P_END 0x6D
#define GDXGAD_MARKER_P_START 0x6E

enum {
    GDXDRESS_SCOPE_NONE = 0,
    GDXDRESS_SCOPE_GADGET = 1, /* gadget_batch: mines, jumps, course effects */
    GDXDRESS_SCOPE_DECO = 2,   /* dress_retained: the gate/sign and building passes */
    GDXDRESS_SCOPE_FX = 3,     /* fxshd_direct: boost flames + the effect classes */
    GDXDRESS_SCOPE_COUNT = 4
};

/* Resolved lever level of a scope (0 off, 1 on, 2 verify); latched at first call. */
int gdx_dress_level(int scope);
/* Decomp side: write the scope marker into *slot when the scope's lever is armed. Returns 1 when
 * a marker was written (the caller advances its cursor). */
int gdx_dress_scope_marker(void* slot, int scope, int open);
/* n60lap [native] finish_direct: the FINISH banner's scope marker (GDXDRESS_SCOPE_FX), written only
 * when finish_direct=1 and fxshd_direct is armed. Same return as gdx_dress_scope_marker. */
int gdx_finish_scope_marker(void* slot, int open);
/* n60finish [native] finish_mesh: the G_NOOP p=0x6A (low byte 1) written right before the FINISH
 * banner's G_DL when finish_mesh=1 (and the owned walker draws): the walker hands that G_DL to the
 * mesh registry's retained banner (gdx3ds_mesh.cpp). Returns 1 when a marker was written. */
#define GDXFINISH_MARKER_P 0x6A
int gdx_finish_mesh_marker(void* slot);

/* ---- the gadget record (game-side sink gdx3ds_dress_sink.c, render-side replay) ---- */
#define GDXGAD_MAGIC 0x47414431u /* "GAD1" */
#define GDXGAD_MAX_OPS 4096      /* 48 mines + 8 jumps + 32 dash plates + the strips; overflow emits stock GBI */
#define GDXGAD_MAX_RUNS 64

enum { GDXGAD_OP_VTX = 1, GDXGAD_OP_TRI = 2, GDXGAD_OP_MOD = 3, GDXGAD_OP_TEX = 4 };

typedef struct GdxGadOp {
    uint8_t kind;
    uint8_t a;    /* VTX: n | TRI: v0 | MOD: vertex slot | TEX: tile */
    uint8_t b;    /* VTX: v0 | TRI: v1 | MOD: where | TEX: level */
    uint8_t c;    /* TRI: v2 | TEX: on */
    uint32_t arg; /* VTX: const Vtx* | MOD: value | TEX: (sc << 16) | tc */
} GdxGadOp;

typedef struct GdxGadRun {
    uint16_t opStart;
    uint16_t opEnd;
} GdxGadRun;

/* Written by the replay (drawing thread), read by the game thread on that pool's next begin. */
typedef struct GdxGadReplayStats {
    uint32_t markers;
    uint32_t vtxOps;
    uint32_t verts;
    uint32_t mods;
    uint32_t tris;
    uint32_t fast;
    uint32_t exact;
    uint32_t fallback;
    uint32_t draws;
    uint32_t splitTile;
    uint32_t verifyOk;
    uint32_t verifyBad;
} GdxGadReplayStats;

typedef struct GdxGadRecord {
    uint32_t magic;
    uint32_t mode;  /* 1 native, 2 verify */
    uint32_t frame; /* gGameFrameCount the record belongs to */
    uint32_t nOps;
    uint32_t nRuns;
    GdxGadOp ops[GDXGAD_MAX_OPS];
    GdxGadRun runs[GDXGAD_MAX_RUNS];
    GdxGadReplayStats rp;
} GdxGadRecord;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_DRESS_H */
