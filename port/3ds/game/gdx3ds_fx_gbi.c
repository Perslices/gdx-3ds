/* gdx3ds_fx_gbi.c — [native] fx: the real GBI emitters behind gdx3ds_fx_shim_begin.h.
 *
 * Compiled with the game's own gbi.h and defines (F3DEX_GBI_2, PORT), each helper simply
 * invokes the ORIGINAL macro, so whatever racer.c would have encoded without the shim is
 * encoded here byte for byte — including the 32-bit PORT's tagged host pointer in w1
 * (_GFXW1_PTR). No macro body is copied. */
#include "global.h"

#include <stdint.h>

void gdxfx_gbi_dl(void* g, const void* dl) {
    gSPDisplayList((Gfx*)g, (const Gfx*)dl);
}

void gdxfx_gbi_vertex(void* g, const void* v, int n, int v0) {
    gSPVertex((Gfx*)g, (const Vtx*)v, n, v0);
}

void gdxfx_gbi_tri1(void* g, int a, int b, int c, int flag) {
    gSP1Triangle((Gfx*)g, a, b, c, flag);
}

void gdxfx_gbi_tri2(void* g, int a, int b, int c, int flag0, int d, int e, int f, int flag1) {
    gSP2Triangles((Gfx*)g, a, b, c, flag0, d, e, f, flag1);
}

void gdxfx_gbi_prim(void* g, int m, int l, int r, int gg, int b, int a) {
    gDPSetPrimColor((Gfx*)g, m, l, r, gg, b, a);
}

void gdxfx_gbi_env(void* g, int r, int gg, int b, int a) {
    gDPSetEnvColor((Gfx*)g, r, gg, b, a);
}

void gdxfx_gbi_texture(void* g, int s, int t, int level, int tile, int on) {
    gSPTexture((Gfx*)g, s, t, level, tile, on);
}

void gdxfx_gbi_pipesync(void* g) {
    gDPPipeSync((Gfx*)g);
}

void gdxfx_gbi_settimg(void* g, int fmt, int siz, int width, const void* img) {
    gDPSetTextureImage((Gfx*)g, fmt, siz, width, img);
}

void gdxfx_gbi_noop(void* g) {
    gDPNoOp((Gfx*)g);
}

/* The replay marker: a G_NOOP whose w0 carries the marker tag + run index and whose w1 is
 * the record pointer, written like any host pointer so the bridge's default arm carries it
 * through to the interpreter's noop handler unchanged (same shape as the track marker). */
void gdxfx_gbi_marker(void* g, uint32_t w0, const void* record) {
    Gfx* _g = (Gfx*)g;
    _g->words.w0 = w0;
    _g->words.w1 = _GFXW1_PTR(record);
}
