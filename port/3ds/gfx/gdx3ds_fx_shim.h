/* gdx3ds_fx_shim.h — [native] fx: GBI shim for the effects bracket of Racer_Draw
 * (decomp/src/game/racer.c, decomp-native-fx-shim.patch).
 *
 * Included under `#if defined(PORT) && defined(GDX_NATIVE_FX)` AFTER the game's gbi.h. It only
 * declares the sink and the real-GBI helpers plus the two bracket hooks; the macro
 * redefinitions live in gdx3ds_fx_shim_begin.h / gdx3ds_fx_shim_end.h, which the patch includes
 * INSIDE Racer_Draw around the six ring-buffer effect classes (explosions, debris, flying
 * sparks, collision sparks, smoke, fall explosions — the pit force field sits between smoke and
 * fall explosions and rides along). Outside the bracket racer.c compiles the stock macros:
 * zero cost, byte-identical output.
 *
 * Inside the bracket, when `gdxfx_active` is 0 (the default: [native] fx_sink=0 and
 * fxcensus=0) every redefined macro costs one global load and forwards to a helper compiled
 * with the game's own gbi.h (port/3ds/game/gdx3ds_fx_gbi.c) — the packet bytes are the
 * originals by construction. Active, the per-sprite commands are recorded into a per-GfxPool op
 * list (gdx3ds_fx_record.h) and replaced by ONE G_NOOP marker per material run that
 * Interpreter::GdxFxMarker replays at that exact display-list position:
 *
 *   gSPVertex / gSP1Triangle / gSP2Triangles / gDPSetPrimColor / gDPSetEnvColor / gSPTexture
 *       -> recorded (the interpreter's own GfxSpVertex / colour registers / GfxSpTexture run at
 *          the marker, so transform, clip flags and texture selection are the interpreter's)
 *   gDPPipeSync -> dropped while recording (the 3DS handler is a no-op), real otherwise
 *   gSPDisplayList (a class setup list) / gDPSetTextureImage (the explosion frame loads, which
 *       the bridge must translate) -> stay real GBI, preceded by a marker when ops are pending
 *       (the run boundary). The rest of gDPLoadTextureBlock is untouched.
 *
 * Every packet expression in the bracket is `gfx++`; the macros detect an advance by comparing
 * `gfx` before/after evaluating the packet expression, and the sink returns: 0 = emit the real
 * command into the slot, 1 = recorded (rewind the cursor: nothing was written), 2 = a marker was
 * written into the slot, emit the real command into a fresh slot. */
#ifndef GDX3DS_FX_SHIM_H
#define GDX3DS_FX_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sink (port/3ds/gfx/gdx3ds_fx_sink.c) ---- */
extern int gdxfx_active;
void gdxfx_begin(int poolIndex, unsigned frame, int playerIndex);
void gdxfx_end(void);
int gdxfx_pending(void);
int gdxfx_marker(void* slot);
int gdxfx_setup(void* slot, const void* dl);
int gdxfx_texload(void* slot);
int gdxfx_vertex(void* slot, const void* vtx, int n, int v0, int advanced);
int gdxfx_tri(void* slot, int a, int b, int c, int advanced);
int gdxfx_tri2(void* slot, int a, int b, int c, int d, int e, int f, int advanced);
int gdxfx_prim(void* slot, int m, int l, int r, int g, int b, int a, int advanced);
int gdxfx_env(void* slot, int r, int g, int b, int a, int advanced);
int gdxfx_texture(void* slot, int sc, int tc, int level, int tile, int on, int advanced);
int gdxfx_pipesync(void* slot, int advanced);

/* ---- real GBI emitters (port/3ds/game/gdx3ds_fx_gbi.c) ---- */
void gdxfx_gbi_dl(void* g, const void* dl);
void gdxfx_gbi_vertex(void* g, const void* v, int n, int v0);
void gdxfx_gbi_tri1(void* g, int a, int b, int c, int flag);
void gdxfx_gbi_tri2(void* g, int a, int b, int c, int flag0, int d, int e, int f, int flag1);
void gdxfx_gbi_prim(void* g, int m, int l, int r, int gg, int b, int a);
void gdxfx_gbi_env(void* g, int r, int gg, int b, int a);
void gdxfx_gbi_texture(void* g, int s, int t, int level, int tile, int on);
void gdxfx_gbi_pipesync(void* g);
void gdxfx_gbi_settimg(void* g, int fmt, int siz, int width, const void* img);
void gdxfx_gbi_noop(void* g);
void gdxfx_gbi_marker(void* g, uint32_t w0, const void* record);

#ifdef __cplusplus
}
#endif

/* ---- hooks for Racer_Draw (the decomp patch adds exactly these two lines) ---- */
#define GDXFX_BEGIN(player) gdxfx_begin((int)(gGfxPool - D_8024DCE0), (unsigned)gGameFrameCount, (int)(player))
#define GDXFX_END()                                           \
    {                                                         \
        if (gdxfx_active && gdxfx_pending()) {                \
            gdxfx_marker(gfx++);                              \
        }                                                     \
        gdxfx_end();                                          \
    }

#endif /* GDX3DS_FX_SHIM_H */
