/* gdx3ds_gad_shim.h -- [native] gadget_batch (C5 Stage 3b, gdx3ds_dress.h): the declarations
 * behind the gadget bracket of Course_GadgetsDraw (decomp-native-dress-shim.patch).
 *
 * gdx3ds_gad_shim_begin.h redefines, for the bracket only (push_macro / pop_macro), the GBI
 * macros the gadget pass emits: gSPVertex, gSP1Triangle, gSP2Triangles, gSPModifyVertex,
 * gSPTexture are recorded; gSPDisplayList and gDPSetTextureImage stay real GBI and close the
 * pending run first. Every real packet is written by gdx3ds_gad_gbi.c, a TU compiled with the
 * game's own gbi.h that just invokes the ORIGINAL macros, so the pass-through encoding is
 * identical by construction. With the lever off `gdxgad_active` is 0 and every macro costs one
 * global load before forwarding to those helpers. The packet variable of the bracket is
 * Course_GadgetsDraw's `gfx` (every site is `gfx++`). */
#ifndef GDX3DS_GAD_SHIM_H
#define GDX3DS_GAD_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- levers and scope markers (gdx3ds_dress_sink.c) ---- */
int gdx_dress_level(int scope);
int gdx_dress_scope_marker(void* slot, int scope, int open);

/* ---- the gadget recorder (gdx3ds_dress_sink.c) ---- */
extern int gdxgad_active;
void gdxgad_begin(int poolIndex, unsigned frame, int enabled);
void gdxgad_end(void);
int gdxgad_pending(void);
int gdxgad_marker(void* slot);
int gdxgad_boundary(void* slot, int isSetup);
int gdxgad_vertex(void* slot, const void* vtx, int n, int v0, int advanced);
int gdxgad_tri(void* slot, int a, int b, int c, int advanced);
int gdxgad_tri2(void* slot, int a, int b, int c, int d, int e, int f, int advanced);
int gdxgad_modify(void* slot, int vtx, int where, uint32_t val, int advanced);
int gdxgad_texture(void* slot, int sc, int tc, int level, int tile, int on, int advanced);

/* ---- real GBI emitters (port/3ds/game/gdx3ds_gad_gbi.c) ---- */
void gdxgad_gbi_dl(void* g, const void* dl);
void gdxgad_gbi_vertex(void* g, const void* v, int n, int v0);
void gdxgad_gbi_tri1(void* g, int a, int b, int c, int flag);
void gdxgad_gbi_tri2(void* g, int a, int b, int c, int flag0, int d, int e, int f, int flag1);
void gdxgad_gbi_modify(void* g, int vtx, int where, uint32_t val);
void gdxgad_gbi_texture(void* g, int s, int t, int level, int tile, int on);
void gdxgad_gbi_settimg(void* g, int fmt, int siz, int width, const void* img);
void gdxgad_gbi_marker(void* g, uint32_t w0, const void* record);

#ifdef __cplusplus
}
#endif

/* ---- hooks for Course_GadgetsDraw (the decomp patch adds exactly these) ---- */
/* `enabled` = the race branch (the course editor's branch writes raw state commands the bracket
 * does not model, so it always runs stock). */
#define GDXGAD_BEGIN(enabled) gdxgad_begin((int)(gGfxPool - D_8024DCE0), (unsigned)gGameFrameCount, (int)(enabled))
#define GDXGAD_END()                             \
    {                                            \
        if (gdxgad_active && gdxgad_pending()) { \
            gdxgad_marker(gfx++);                \
        }                                        \
        gdxgad_end();                            \
    }
/* Direct-load scope markers (gdx3ds_dress.h GDXDRESS_SCOPE_*): open / close around a pass. */
#define GDXDRESS_SCOPE(scope, open)                           \
    {                                                         \
        if (gdx_dress_scope_marker(gfx, (scope), (open))) {   \
            gfx++;                                            \
        }                                                     \
    }

#endif /* GDX3DS_GAD_SHIM_H */
