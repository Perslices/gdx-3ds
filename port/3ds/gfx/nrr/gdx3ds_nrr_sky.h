/* port/3ds/gfx/nrr/gdx3ds_nrr_sky.h -- [native] nrr_sky: the sky/background on the compact
 * native race renderer (NRR, docs/research/native/n60-design.md §4.4; progress and inventory:
 * docs/research/native/n60sky-progress.md).
 *
 * GAME SIDE (port/3ds/game/gdx3ds_nrr_sky_game.c). The SKY section of Race_Draw is consumed
 * (gdx3ds_c5tick_consume): Background_Draw is not built on rendered race ticks. Its stub writes
 *   - on a learn frame only: the stock sky's state commands with a swallowed dummy triangle at
 *     each draw point, bracketed by LEARN markers (the render side reads what the interpreter's
 *     own state block bound there: shader, texture-cache entry, sampler, blend, pack params);
 *   - the stock head (perspective normalize, projection, Camera_Draw, the sky setup list,
 *     the model-view load): the track after it reads that RSP/RDP state;
 *   - one DRAW marker per part (A: gradient, stars, sprites; B: floor, clouds), w1 = the
 *     per-GfxPool snapshot below; whatever stays legacy between the parts is written there;
 *   - the stock sky's final render-mode / combine / prim state (the "tail"), no loads.
 * The 1P race only (1 view, no ending, no records): every other caller gets Background_Draw.
 *
 * RENDER SIDE (port/3ds/gfx/n64/gdx_nrr_sky.cpp, Interp members). At a DRAW marker: transform
 * the snapshot's vertices with the MP the head left (exactly the stock T&L inputs), pack them in
 * the PICA layout, bind the learned state blocks by diff against the interpreter's own backend
 * cache, one packed draw per state block. The PICA clips (performance mode).
 *
 * Markers: the draws ride the core's p=0x63 marker (cat NRR_CAT_SKY; view slot 0 = part A or the
 * whole sky, slot 1 = part B). The learn regions use their own G_NOOP p=0x64 (cold, this category
 * only): w0 = (G_NOOP<<24) | (0x64<<16) | (op<<8), w1 = (learn serial << 8) | kind. */
#ifndef GDX3DS_NRR_SKY_H
#define GDX3DS_NRR_SKY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NRRSKY_LEARN_P 0x64u

enum GdxNrrSkyOp {
    GDX_NRRSKY_OP_LEARN_BEGIN = 1,
    GDX_NRRSKY_OP_LEARN_END = 2,
};

/* Draw parts. */
#define GDX_NRRSKY_PART_A 1u /* gradient, native stars, native sprites */
#define GDX_NRRSKY_PART_B 2u /* floor, clouds */

/* Learn kinds: one state block each. */
enum GdxNrrSkyKind {
    GDX_NRRSKY_K_GRAD = 0,
    GDX_NRRSKY_K_FLOOR,
    GDX_NRRSKY_K_CLOUD,
    GDX_NRRSKY_K_STAR,    /* two learn rectangles: the screen -> clip map is fitted from them */
    GDX_NRRSKY_K_SPRITE0, /* + slot, 6 slots (day palettes: constant per venue) */
    GDX_NRRSKY_K_COUNT = GDX_NRRSKY_K_SPRITE0 + 6
};

/* Snapshot flags. */
#define GDX_NRRSKY_F_CLOUDS 0x01u
#define GDX_NRRSKY_F_SPLIT 0x02u  /* stock commands sit between part A (slot 0) and part B (slot 1) */
#define GDX_NRRSKY_F_VIEW 0x04u   /* niview: the whole sky of view `slot` of a multi-view race (no split) */

/* A visible star: Background_DrawStars' texrect corner (10.2 fixed, as the stock truncates it) and
 * its prim colour. The learn rectangles sit at these N64 positions. */
typedef struct GdxNrrSkyStar {
    int16_t xl, yl;
    uint8_t rgba[4];
} GdxNrrSkyStar;

#define GDX_NRRSKY_STAR_LEARN_XL 64
#define GDX_NRRSKY_STAR_LEARN_YL 64
#define GDX_NRRSKY_STAR_LEARN_DX 160
#define GDX_NRRSKY_STAR_LEARN_DY 240

typedef struct GdxNrrSkySprite {
    float mp[4][4]; /* billboard MV (unk_2C368[i], fixed -> float) x the head's projection */
    uint8_t kind;   /* GDX_NRRSKY_K_SPRITE0 + slot */
    uint8_t pad[3];
} GdxNrrSkySprite;

/* What the render side reads, written on the game fiber when the stub runs, in the core's
 * per-GfxPool frame arena (NrrSnapAlloc). Vertex and matrix pointers point into the GfxPool the
 * list lives in (written by Background_Update in logic, double-buffered with the list); nothing
 * points at live game state. */
typedef struct GdxNrrSkyFrame {
    uint32_t frame;     /* gGameFrameCount at the build */
    uint32_t identity;  /* learn generation: the blocks are valid for this value only */
    const void* vtx;    /* Vtx[20] of this pool: floor 0-7, skybox 8-11, clouds 12-19 */
    const void* spriteVtx; /* Vtx[4] (unk_2C4E8) */
    uint8_t flags;
    uint8_t spriteCount; /* native (day) sprites in front of the eye */
    uint8_t starCount;
    uint8_t pad;
    GdxNrrSkySprite sprites[6];
    const GdxNrrSkyStar* stars; /* in the frame arena */
    uint8_t floorShade[4];      /* the floor batch's vertex-0 colour (first vertex the stock CPU clipper emits) */
    uint8_t floorShadeOk;       /* 0: no floor triangle survives (the floor draws nothing either) */
    uint8_t pad2[3];
} GdxNrrSkyFrame;

/* Render side -> game side: TransformVertices' aspect factor on x, as the last draw used it
 * (the game side's floor-shade clip replica needs the stock clip-space x). */
extern volatile float gdx3ds_nrr_sky_xs;

/* Render half of the category init (n64/gdx_nrr_sky.cpp): installs the draw. */
void gdx3ds_nrr_sky_render_init(void);

/* Render side -> game side: ask the next build for a learn sequence (a state block went stale:
 * a texture-cache entry was retired, or a learn failed). Single-threaded: NRR refuses the render
 * thread. */
extern volatile int gdx3ds_nrr_sky_relearn;
/* Render side -> game side: native drawing failed in a way learning cannot fix (an unsupported
 * state block); the stub then builds the stock sky for the rest of the session. */
extern volatile int gdx3ds_nrr_sky_broken;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_SKY_H */
