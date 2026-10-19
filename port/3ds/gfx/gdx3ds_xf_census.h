/* gdx3ds_xf_census.h — [debug] xf_census: what a float24 vertex shader WOULD have done to
 * every vertex the CPU transforms (o3dsgpuxf-progress.md §2, the precision root cause).
 *
 * The PICA200 vertex shader computes in float24 (1 sign, 7 exponent, 16 mantissa bits);
 * Azahar computes in float32, so no emulator frame can show a float24 artifact. This census
 * replays the shader's position dp4 in SIMULATED float24 (each operand and each partial
 * result rounded to a 16-bit mantissa) beside the fp32 value the CPU actually used, for every
 * vertex that reaches the CPU transform, and reports per path:
 *   in    vertices in front of the eye and inside the 2w guard box (the ones that can land)
 *   px    worst screen error of x/w, y/w in pixels (clip x -> 120 px, clip y -> 200 px)
 *   w     worst relative error of w in parts per million (perspective-correct UV weights)
 *   bad   vertices whose px error exceeds 1/8 px
 *   cancel / maxratio  over ALL vertices (on screen or not): sum|w-row terms| / |w|, the
 *         cancellation factor; float24 w carries ~2^-17 x this relative error, and cancel
 *         counts vertices past 1024 (w wrong by ~1%: a clipped triangle's perspective weights)
 * Paths: dl = the interpreter's own G_VTX, seam = the native replays' SpVertex (track, fx),
 * mesh = the retained machine meshes' CPU T&L, trk = the vertices [native] track_retained hands
 * the PICA (eye-rebased ob and the rebased matrix, exactly what the shader will see). Off (the default) it is one predictable
 * branch per vertex batch. */
#ifndef GDX3DS_XF_CENSUS_H
#define GDX3DS_XF_CENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_XFC_DL = 0,
    GDX3DS_XFC_SEAM = 1,
    GDX3DS_XFC_MESH = 2,
    GDX3DS_XFC_TRK = 3,
    GDX3DS_XFC_NEAR = 4, /* n60 nrr_trk: points the PICA's near-plane clipper makes (note_clip) */
    GDX3DS_XFC_PATHS = 5
};

/* Latched [debug] xf_census (0 until the ini is ready). */
extern int gdx3ds_xf_census_on;
/* The path the next gdx3ds_xf_census_note calls belong to (the seam sets SEAM around its
 * SpVertex and restores DL). */
extern int gdx3ds_xf_census_path;

/* One vertex: model position `ob`, the MP matrix the CPU used (mp[k][i], N64 layout), the
 * x scale the CPU applied after the multiply, and the fp32 clip coordinates it produced. */
void gdx3ds_xf_census_note(const int16_t ob[3], const float mp[4][4], float xScale, float x, float y, float w);
/* The same for a vertex whose object position is a float (the PICA converts it to float24 before
 * the dp4: modelled). [native] track_retained's near-plane polygon corners. */
void gdx3ds_xf_census_note_f(const float ob[3], const float mp[4][4], float xScale, float x, float y, float w);
/* n60 nrr_trk (PICA clipping instead of the CPU near clipper): the edge a -> b crosses the near
 * plane (z + w = 0). Simulates the PICA in float24 -- both endpoints' four dp4 rows, the plane
 * distances, the parameter and the interpolated point -- against the exact (double) clip point,
 * and charges the projected error to the `near` path. */
void gdx3ds_xf_census_note_clip(const float a[3], const float b[3], const float mp[4][4], float xScale);
/* Frame cadence (every 64 frames: one [xfcensus] line, then reset). */
void gdx3ds_xf_census_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_XF_CENSUS_H */
