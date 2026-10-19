/* gdx3ds_track_retained.h — [native] track_retained (NEXT.md C5 Stage 3, c5s3-progress.md).
 *
 * The course's material runs, recorded by the [native] track shim exactly as today, are
 * replayed as GPU-transformed indexed draws of the game's OWN course vertices instead of a
 * CPU transform + per-triangle pack. The PICA sees each vertex relative to an integer origin
 * at the camera eye (and the matching translated matrix, composed on the CPU in double), so
 * no large world coordinate ever reaches float24; every triangle is gated by the float24
 * error bound of the shader's dp4 and falls back to the CPU path when it fails.
 *
 * Levers ([native], all default 0 / off, NOT covered by `all=1`):
 *   track_retained        0 off | 1 performance (the PICA clips) | 2 parity (triangles the CPU
 *                         clipper would split stay on the CPU, so the picture is the CPU path's)
 *   track_retained_maxerr float24 gate, 1/1000 px (default 125 = 1/8 px); -1 = no gate
 *   track_retained_rebase 1 (default) eye-rebased vertices | 0 world-space (emulator-only
 *                         diagnostic: float24 would smear it on hardware; the gate refuses it)
 *   track_retained_sideclip 0 (default) clip the side planes on the CPU only for a folded
 *                         sub-window viewport (4P) | 1 always
 * Requires [native] track=1. Receipts: `[trkrr] init`, `[trkrr] frame=` every 64 frames. */
#ifndef GDX3DS_TRACK_RETAINED_H
#define GDX3DS_TRACK_RETAINED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
struct Gdx3dsVboPackAux;
struct Gdx3dsXfDraw;
/* Backend (gfx_citro3d.cpp): gdx3ds_mesh_backend_xf_draw with float positions and UVs -- 24-byte
 * vertices {float x, y, z; float s, t; u8 cn[4]} -- for the near-plane polygons. */
void gdx3ds_track_backend_xf_draw_fpos(const uint16_t* idx, uint32_t nIdx, const struct Gdx3dsVboPackAux* aux,
                                       const struct Gdx3dsXfDraw* xf);
/* 1 while the current viewport is a folded sub-window ([vpfix]: 4P split screen). The scissor then
 * bounds the viewport's rect, not the clip volume, so the retained path clips the side planes too. */
int gdx3ds_track_backend_viewport_folded(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class GdxInterpSeam;
struct GdxTrkRecord;

/* One p=0x60 marker: replay run `run` of `rec` through the retained path (with per-run and
 * per-triangle CPU fallback). */
void gdx3ds_track_retained_run(GdxInterpSeam& I, GdxTrkRecord* rec, uint32_t run);
#endif

#endif /* GDX3DS_TRACK_RETAINED_H */
