/* gdx3ds_track_gbi.c — [native] track: the real GBI emitters behind gdx3ds_track_shim.h.
 *
 * Compiled with the game's own gbi.h and defines (F3DEX_GBI_2, PORT), each helper simply
 * invokes the ORIGINAL macro, so whatever course.c would have encoded without the shim is
 * encoded here byte for byte — including the 32-bit PORT's tagged host pointer in w1
 * (_GFXW1_PTR). No macro body is copied. */
#include "global.h"

#include <stdint.h>

void gdxtrk_gbi_vertex(void* g, const void* v, int n, int v0) {
    gSPVertex((Gfx*)g, (const Vtx*)v, n, v0);
}

void gdxtrk_gbi_dl(void* g, const void* dl) {
    gSPDisplayList((Gfx*)g, (const Gfx*)dl);
}

void gdxtrk_gbi_modify(void* g, int vtx, int where, uint32_t val) {
    gSPModifyVertex((Gfx*)g, vtx, where, val);
}

void gdxtrk_gbi_texture(void* g, int s, int t, int level, int tile, int on) {
    gSPTexture((Gfx*)g, s, t, level, tile, on);
}

void gdxtrk_gbi_loadsync(void* g) {
    gDPLoadSync((Gfx*)g);
}

void gdxtrk_gbi_settimg(void* g, int fmt, int siz, int width, const void* img) {
    gDPSetTextureImage((Gfx*)g, fmt, siz, width, img);
}

void gdxtrk_gbi_loadblock(void* g, int tile, int uls, int ult, int lrs, int dxt) {
    gDPLoadBlock((Gfx*)g, tile, uls, ult, lrs, dxt);
}

void gdxtrk_gbi_noop(void* g) {
    gDPNoOp((Gfx*)g);
}

/* The replay marker: a G_NOOP whose w0 carries the marker tag + run index and whose w1 is
 * the record pointer, written like any host pointer so the bridge's default arm carries it
 * through to the interpreter's noop handler unchanged. */
void gdxtrk_gbi_marker(void* g, uint32_t w0, const void* record) {
    Gfx* _g = (Gfx*)g;
    _g->words.w0 = w0;
    _g->words.w1 = _GFXW1_PTR(record);
}
