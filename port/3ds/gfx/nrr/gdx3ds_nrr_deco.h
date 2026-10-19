/* gdx3ds_nrr_deco.h -- NRR category DECO (n60deco): the course decorations (gates, signs,
 * buildings: Course_GadgetsDraw's decoration passes) on the compact native race renderer
 * (docs/research/native/n60deco-progress.md). The n60mach pattern on static lists: a decoration
 * body is a static ROM list drawn under a baked matrix, so its first sight through the interpreter
 * (the stock commands, drawn on the mesh path) IS its cold record pass, and from then on a
 * visible decoration is one pose + its recorded runs.
 *
 * Lever `[native] nrr_deco = 1` (core category, default 0, not under all=1). Rides the dress
 * stub (`nrr_dress = 1`): the stub plans the decoration passes, puts every decoration with a
 * complete record in this category's snapshot and writes the rest as stock commands after the
 * marker, each bracketed by REC markers (G_NOOP p=0x67) that arm the recorder for it.
 *
 * Record key = (feature type, TMEM owner at the draw). A decoration body samples the textures its
 * type's setup list loaded, or (NinTex left, a memo hit after a self-loading list) whatever the
 * stream left in TMEM: the owner is the last texture loader of the stock stream (a memo setup
 * list, a building's load block, or a self-loading body). Two decorations with the same key draw
 * with the same state by construction (every other register the bodies read is set by the pass
 * prologue or by the type's own draw function). */
#ifndef GDX3DS_NRR_DECO_H
#define GDX3DS_NRR_DECO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record markers: w0 = (G_NOOP<<24) | (0x67<<16) | (op<<8), w1 = key | (mesh draws << 16).
 * 0x67 was free (gdx3ds_nrr.h lists the taken ones). */
#define GDX_NRRDECO_REC_P 0x67u
#define GDX_NRRDECO_OP_BEGIN 1u
#define GDX_NRRDECO_OP_END 2u

/* Feature types 0..21 (COURSE_FEATURE_GATE_SQUARE .. BUILDING_TALL_GOLD_RIGHT); owners -1..12
 * (-1 = unknown: whatever was in TMEM when the pass began). */
#define GDX_NRRDECO_TYPES 22
#define GDX_NRRDECO_OWNERS 14
#define GDX_NRRDECO_KEYS (GDX_NRRDECO_TYPES * GDX_NRRDECO_OWNERS)
#define GDX_NRRDECO_KEY(type, owner) ((type) * GDX_NRRDECO_OWNERS + (owner) + 1) /* type = key / 14, owner = key % 14 - 1 */

/* A decoration draws at most two recorded meshes: the body under its matrix, and (NinTex signs,
 * the overhead sign) a second body under the rotation matrix multiplied onto it. */
#define GDX_NRRDECO_MAX_DRAWS 2
/* Course_GadgetsDraw's decorations: D_80225800.decorationMtx[32]. */
#define GDX_NRRDECO_MAX 32

/* One visible decoration: its matrix (the s15.16 Mtx the stock G_MTX loads), the rotation the
 * second body multiplies onto it (NULL = none), its record key. */
typedef struct GdxNrrDecoEnt {
    const int32_t* base;
    const int32_t* mul;
    uint16_t key;
    uint16_t pad;
} GdxNrrDecoEnt;

typedef struct GdxNrrDecoSnap {
    uint16_t n;
    uint16_t pad;
    uint32_t sig; /* course signature: records are dropped when it changes */
    GdxNrrDecoEnt e[GDX_NRRDECO_MAX];
} GdxNrrDecoSnap;

/* ---- game side ---- */
/* 1 when `key` has a complete record for the current mesh generation, dress epoch and course. */
int gdx3ds_nrr_deco_ready(int key);
/* The course changed (signature `sig`): every record is dropped (deferred frees). */
void gdx3ds_nrr_deco_course(uint32_t sig);
/* Copy the used part of `s` into the core's frame and write the marker (gfx unchanged when there
 * is nothing to draw or the arena is full). Receipt counts: decorations written stock this frame
 * because their key was not ready (cold), because the type is never recorded (self-loading
 * bodies), whole passes written stock because a stock decoration's context could not be rebuilt. */
void* gdx3ds_nrr_deco_publish(void* gfx, const GdxNrrDecoSnap* s, int cold, int never, int fullStock, int view);

/* ---- render side ---- */
/* The REC marker (n64/gdx_rt_glue.cpp OwnedNoopHook). */
void gdx3ds_nrr_deco_rec_marker(uint32_t w0, uint32_t w1);
/* 1 between a BEGIN and its END: the owned walker hands decoration bodies to the mesh registry
 * (gdx3ds_mesh_dress_hook_seam) so the recorder sees them. A variable: the walker tests it at
 * every G_DL. */
extern int gdx3ds_nrr_deco_arm;
/* 1 when the category is on (its init passed): the dress stub plans the decorations. */
int gdx3ds_nrr_deco_on(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_DECO_H */
