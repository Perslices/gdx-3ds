/* port/3ds/gfx/nrr/gdx3ds_nrr_internal.h -- NRR core internals shared by the game half
 * (gdx3ds_nrr_game.c), the render half (gdx3ds_nrr.cpp) and the backend bridge
 * (gfx/gfx_citro3d_nrr.inc). Categories include gdx3ds_nrr.h only. */
#ifndef GDX3DS_NRR_INTERNAL_H
#define GDX3DS_NRR_INTERNAL_H

#include "gdx3ds_nrr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRR_FRAME_MAGIC 0x4E525231u /* "NRR1" */
#define NRR_HOST_TAG 0x47445831u    /* "GDX1": decomp gbi.h GDX_GFXW1_HOST_TAG (a host pointer in w1) */

struct NrrFrame {
    uint32_t magic;
    uint32_t tick;     /* gGameFrameCount at the reset */
    const void* pool;  /* the GfxPool this frame belongs to */
    uint32_t used;     /* arena bytes handed out */
    uint32_t ovf;      /* allocations refused */
    const void* root[NRR_CAT_COUNT][NRR_MAX_VIEWS];
    uint64_t arena[NRR_FRAME_ARENA_BYTES / 8u];
};

/* Game half -> render half. */
const char* gdx3ds_nrr_cat_name(int cat);
uint32_t gdx3ds_nrr_live_tick(void); /* gGameFrameCount now (0 without the game) */
uint32_t gdx3ds_nrr_take_ovf(void);  /* arena overflows since the last call */
/* [native] nrr_direct (0 citro3d path, 1 direct command words, 2 verify), 0 with the master off. */
int gdx3ds_nrr_direct_level(void);
void gdx3ds_nrr_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- backend bridge (gfx/gfx_citro3d_nrr.inc, inside the citro3d renderer's TU) ---- */

/* One mapped TexEnv stage, citro3d enum values. */
typedef struct NrrBkStage {
    uint8_t rgbFunc;
    uint8_t alphaFunc;
    uint8_t rgbSrc[3];
    uint8_t alphaSrc[3];
    uint8_t rgbOp[3];
    uint8_t alphaOp[3];
    int8_t constRgbInput;   /* 1-based shader input feeding the constant rgb, 0 none */
    int8_t constAlphaInput;
    int8_t constRgbFixed;   /* -1 none, 0 / 1 literal */
    int8_t constAlphaFixed;
    uint32_t constColor;    /* baked constant */
} NrrBkStage;

#define NRR_BK_STAGES 4 /* gfx_citro3d.h kMaxTexEnvStages */

/* The backend's translation of one (shaderId0, shaderId1): its own MapCombiner output. */
typedef struct NrrBkShader {
    uint8_t numStages;
    int8_t fogStage;        /* fog-blend stage index, -1 none */
    int8_t vtxColorInput;
    int8_t vtxAlphaInput;
    uint8_t inputSize;      /* 3 rgb or 4 rgba per combiner input */
    uint8_t usedTex[2];
    uint8_t clamp[2][2];    /* shader clamp per unit, per axis */
    uint8_t texEdge;        /* cc.opt_texture_edge */
    uint8_t alphaThreshold; /* cc.opt_alpha_threshold */
    NrrBkStage stages[NRR_BK_STAGES];
} NrrBkShader;

/* A backend texture id resolved to what a bind needs. tex: the C3D_Tex* to bind (the atlas page
 * for an atlas view); the UV transform folds pow2 padding and the atlas offset. */
typedef struct NrrBkTex {
    void* tex;
    float uScale, vScale, uOff, vOff;
} NrrBkTex;

/* Everything the NRR draw path needs from the backend's Init (constant for the session). */
typedef struct NrrBkInfo {
    const void* fixupMtx;     /* C3D_Mtx: the plain fixup (draws take gdx3ds_nrr_bk_proj2d) */
    const void* attrLegacy;   /* C3D_AttrInfo of the packed layout */
    const void* attrXf;       /* gpuxf raw Vtx */
    const void* attrXfF;      /* gpuxf float positions */
    int projLoc;
    int xfUnifLoc;            /* -1: the shader has no gpuxf block */
    int xfBoolLoc;
    int stkLoc;               /* n60stereo: the frame replay's per-draw stereo row (-1 none) */
} NrrBkInfo;

int gdx3ds_nrr_bk_info(NrrBkInfo* out);                       /* 0 = no backend */
int gdx3ds_nrr_bk_frame_active(void);
int gdx3ds_nrr_bk_translate(uint64_t id0, uint64_t id1, NrrBkShader* out); /* cold */
int gdx3ds_nrr_bk_tex(uint32_t texId, NrrBkTex* out);
void* gdx3ds_nrr_bk_alloc(uint32_t bytes);                    /* frame pool, 16-aligned */
void gdx3ds_nrr_bk_scissor(int x, int y, int w, int h);       /* N64 window rect -> C3D_SetScissor */
void gdx3ds_nrr_bk_depth_map(int decal);                      /* C3D_DepthMap with the backend's bias */
/* [n60misc] The projection matrix (C3D_Mtx*) the backend uploads for a screen-space packed draw:
 * the fixup, or the [display] border mode's UI map (modes 2/3 scale it). */
const void* gdx3ds_nrr_bk_proj2d(void);
/* [n60mach] The projection matrix (C3D_Mtx*) the backend uploads for a mono 3D (gpuxf) draw: the
 * fixup, or the border mode's scene map. */
const void* gdx3ds_nrr_bk_proj3d(void);
/* n60stereo: the stereo row of an [anchor] rectangle at NDC depth d (the per-draw loop's class and
 * [debug] stereo_anchor killswitch; counted in the [c3d] anchor= receipt). */
void gdx3ds_nrr_bk_anchor_row(float d, float row[4]);
/* n60 nrr_trk: the draw state the backend holds now (after an interpreter bind) as a key (cull
 * left 0), and a texture id's content serial (changes on every upload / delete of that id). */
int gdx3ds_nrr_bk_current_key(NrrStateKey* k);
uint32_t gdx3ds_nrr_bk_tex_serial(uint32_t texId);
/* n60retry: the scissor a key read now would carry (NrrStateKey.scissor; all 0 = full window). */
void gdx3ds_nrr_bk_scissor_key(int16_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_INTERNAL_H */
