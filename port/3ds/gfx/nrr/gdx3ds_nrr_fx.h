/* port/3ds/gfx/nrr/gdx3ds_nrr_fx.h -- [native] nrr_shd / nrr_fx: machine shadows, boost flames and
 * the race effects on the compact native race renderer (NRR, docs/research/native/n60-design.md
 * §4.5; progress, inventory and gates: docs/research/native/n60fx-progress.md).
 *
 * One render-side engine serves the three: every draw is a textured (or shaded) sprite batch whose
 * state the interpreter resolves ONCE, cold, from the stock state commands ("learning", the n60sky
 * pattern), and whose vertices the render side transforms, packs and draws with the core
 * (NrrStateApply / NrrSetInputs / NrrRingAlloc / NrrDraw2DScene). The PICA clips.
 *
 * GAME SIDE (port/3ds/game/gdx3ds_nrr_shd_game.c, ...). A consumed section's stub writes:
 *   - when a block it needs is not learned (render -> game flags below): the stock section's STATE
 *     commands with a dummy triangle bracketed by LEARN markers (G_NOOP p=0x66) at each draw
 *     point, under identity matrices, then the stock projection again;
 *   - one core marker (p=0x63) over a snapshot in the core's per-GfxPool frame arena;
 *   - the stock section's final RSP/RDP state (the "tail": its setup list and last matrix), so
 *     whatever follows reads the state the stock section leaves.
 *
 * RENDER SIDE (port/3ds/gfx/n64/gdx_nrr_fx.cpp, Interp members for the cold learn, free hot
 * functions for the draw).
 *
 * Learn markers: w0 = (G_NOOP<<24) | (0x66<<16) | (op<<8) | cat, w1 = kind. */
#ifndef GDX3DS_NRR_FX_H
#define GDX3DS_NRR_FX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NRRFX_LEARN_P 0x66u

enum GdxNrrFxOp {
    GDX_NRRFX_OP_LEARN_BEGIN = 1,
    GDX_NRRFX_OP_LEARN_END = 2,
};

/* Learned blocks per category (kinds are category-local). */
#define GDX_NRRFX_KINDS 64

/* Render side -> game side: 1 = kind `k` of category `cat` (NRR_CAT_SHD / NRR_CAT_FX /
 * NRR_CAT_DRESS) holds a usable learned block; the render side clears it when the block goes stale
 * (its texture-cache entry was retired) so the next build learns it again. Single-threaded: NRR
 * refuses the render thread. Index [0] = shadows, [1] = fx, [2] = dress. */
#define GDX_NRRFX_CATS 3
extern volatile uint8_t gdx3ds_nrr_fx_ready[GDX_NRRFX_CATS][GDX_NRRFX_KINDS];
/* Render side -> game side: a kind whose state the hot path cannot model (two textures, a fog
 * mode without a fog stage, ...): the stub builds that section the stock way for the rest of the
 * session. */
extern volatile uint8_t gdx3ds_nrr_fx_broken[GDX_NRRFX_CATS];

/* ------------------------------------------------------------------- shadows ---- */

/* Kinds: the shadow texture index (D_800CDC54[shadowType], 0..43) or the crashed shadow. */
#define GDX_NRRSHD_KIND_CRASHED 44
#define GDX_NRRSHD_KINDS 45

/* One shadow decal: its MP (the stock Mtx, fixed -> float as SpMatrix converts it, times the
 * view's projection), its four asset vertices and prim colour. */
typedef struct GdxNrrShdEnt {
    float mp[4][4];
    const void* vtx; /* Vtx[4], host-endian asset data (D_800CDBA4[type] / D_7048868) */
    uint8_t kind;
    uint8_t prim[4];
    uint8_t pad[3];
} GdxNrrShdEnt;

typedef struct GdxNrrShdSnap {
    uint16_t n;
    uint16_t multi; /* niview: a view of a multi-view race (the walk's viewport/scissor, keepVp) */
    GdxNrrShdEnt e[1]; /* n entries, stock order */
} GdxNrrShdSnap;

/* ------------------------------------------------------------------- fx --------- */

/* [native] nrr_fx_parts (default 3): 1 = the boost flames (BOOST consumed), 2 = the effects
 * bracket (explosions, debris, sparks, smoke, pit force field, fall explosions). */
#define GDX_NRRFX_PART_BOOST 1u
#define GDX_NRRFX_PART_EFX 2u

/* Fx kinds (learned blocks of category fx), each learned under its stock setup chain. */
enum GdxNrrFxKind {
    GDX_NRRFX_K_BOOST = 0,   /* aSetupBoosterDL's texture and combiner, env per racer */
    GDX_NRRFX_K_EXPL0 = 1,   /* + frame (D_800CDAB8[0..7]): explosions, prim + env per sprite */
    GDX_NRRFX_K_DEBRIS = 9,  /* untextured, vertex 0's colour written, 1-2 read the pool's bytes */
    GDX_NRRFX_K_FSPARK = 10, /* untextured, three coloured vertices */
    GDX_NRRFX_K_CSPARK0 = 11, /* + tile (timer >> 1): collision sparks, prim + env per sprite */
    GDX_NRRFX_K_SMOKE0 = 19,  /* + tile (timer >> 2): smoke, env per sprite */
    GDX_NRRFX_K_PITFF0 = 27,  /* + tile (frame & 7): the pit force field */
    GDX_NRRFX_K_COUNT = 35
};

/* One effect sprite: the stock build's vertices in the GfxPool's effects buffer, its block, its
 * triangle pattern and the per-sprite registers. */
enum { GDX_NRRFX_PAT_TRI = 0, GDX_NRRFX_PAT_QUAD = 1 };

typedef struct GdxNrrFxSprite {
    const void* vtx; /* Vtx*: the GfxPool effects buffer */
    uint8_t kind;
    uint8_t pat;     /* GDX_NRRFX_PAT_TRI: (0,1,2); QUAD: (0,1,3) (1,2,3) */
    uint8_t set;     /* GDX_NRRFX_SET_*: which registers the class's commands set (else the setup list's) */
    uint8_t pad;
    uint8_t prim[4];
    uint8_t env[4];
} GdxNrrFxSprite;

#define GDX_NRRFX_SET_PRIM 1u
#define GDX_NRRFX_SET_ENV 2u

#define GDX_NRRFX_MAX_SPRITES 320

/* One racer's boost flames: the stock build's vertices in the GfxPool's effects buffer
 * (booster count x 4, or x 5 with the speed trail), the triangle pattern, the env colour. */
typedef struct GdxNrrBoostEnt {
    const void* vtx; /* Vtx*: the GfxPool effects buffer, written exactly as the stock build */
    uint8_t count;   /* boosters (1..4) */
    uint8_t trail;   /* unk_2B3 == 2: five vertices per booster */
    uint8_t env[4];
    uint8_t pad[2];
} GdxNrrBoostEnt;

/* The fx category's per-view root (core frame arena). */
typedef struct GdxNrrFxSnap {
    uint16_t nBoost;
    uint16_t capBoost;
    GdxNrrBoostEnt* boost;
    uint16_t nSprites;
    uint8_t marked; /* the view's marker is written (by the boost stub, else the effects stub) */
    uint8_t multi;  /* niview: a view of a multi-view race (the walk's viewport/scissor, keepVp) */
    const GdxNrrFxSprite* sprites; /* the category's own per-GfxPool buffer */
} GdxNrrFxSnap;

/* ------------------------------------------------------------------- dress ------ */

/* Dress kinds: the gadget pass of Course_GadgetsDraw (mines, jumps, course effects), each learned
 * under its stock setup chain. The decorations (gates, signs, buildings) stay stock commands. */
enum GdxNrrDressKind {
    GDX_NRRDRESS_K_MINE = 0,  /* aSetupLandmineTextureDL */
    GDX_NRRDRESS_K_JUMP = 1,  /* + the jump texture */
    GDX_NRRDRESS_K_CE0 = 2,   /* + aSetupCourseEffectTextureDL, tile = effect type (0..7) */
    GDX_NRRDRESS_K_COUNT = 10
};

enum {
    GDX_NRRDRESS_PAT_MINE = 0,  /* 5 vertices: (0,1,2) (0,2,3) (0,3,4) (0,4,1) */
    GDX_NRRDRESS_PAT_JUMP = 1,  /* 6 vertices: (0,1,2) (3,5,4) (0,3,1) (1,3,4) (0,5,3) (0,2,5) */
    GDX_NRRDRESS_PAT_DASH = 2,  /* 6 vertices: (0,4,1) (0,3,4) (0,2,3) (2,5,3) */
    GDX_NRRDRESS_PAT_STRIP = 3, /* <= 32 vertices: per drawn pair k (2k,2k+2,2k+1) (2k+1,2k+2,2k+3) */
};

/* One gadget vertex load and what the stock commands draw of it. */
typedef struct GdxNrrDressItem {
    const void* vtx; /* Vtx*: the course's own buffers (D_80225800), written at course init */
    uint8_t kind;
    uint8_t pat;
    uint8_t nv;      /* vertices loaded */
    uint8_t st2;     /* STRIP: the wrap patch's ST2 packing (the remainder group) */
    uint16_t pairs;  /* STRIP: pairs whose chunk is drawn (bit k) */
    uint16_t mods;   /* STRIP: pairs whose first two vertices get the wrap ST patch */
} GdxNrrDressItem;

#define GDX_NRRDRESS_MAX_ITEMS 384

typedef struct GdxNrrDressSnap {
    uint16_t n;
    uint16_t multi; /* niview: a view of a multi-view race (the walk's viewport/scissor, keepVp) */
    uint32_t capVerts;            /* packed vertices the items can emit at most */
    const GdxNrrDressItem* items; /* the category's own per-GfxPool buffer */
} GdxNrrDressSnap;

/* Render half of the category inits (n64/gdx_nrr_fx.cpp). */
void gdx3ds_nrr_dress_render_init(void);
void gdx3ds_nrr_shd_render_init(void);
void gdx3ds_nrr_fx_render_init(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_FX_H */
