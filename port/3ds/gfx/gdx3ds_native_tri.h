/* gdx3ds_native_tri.h — the shared "exact first tri, fast rest" triangle replay used by every
 * marker-replay sink (track: gdx3ds_track_replay.cpp, effects: gdx3ds_fx_replay.cpp).
 *
 * Bodies were extracted verbatim from the track replay (native/track, verified bit-identical
 * against the stock draws over 14k runs): the first tri after any state change (and any tri
 * that needs the clipper) goes through the interpreter's own triangle path (GdxInterpSeam::
 * SpTri) so the draw state is resolved by the interpreter, then is ADOPTED into the open
 * Gdx_NativeBatch (no copy); the remaining tris take the fast path — the trifast emission's
 * expressions, expression for expression, from the GdxTrackPackState snapshot.
 *
 * Interpreter-neutral: everything comes through gdx3ds_interp_seam.h, so the same body runs
 * under the vendored LUS interpreter and the owned one. */
#ifndef GDX3DS_NATIVE_TRI_H
#define GDX3DS_NATIVE_TRI_H

#include <cstdint>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_track_interp.h"

namespace GdxNativeTri {

/* Per-run replay context. Zero-initialise, then Init(). */
struct RunCtx {
    GdxInterpSeam* I;
    bool verify;
    bool needExact; /* next tri must resolve state through the interpreter's path */
    bool prepValid; /* fast-path prep derived for the current state */
    GdxTrackPackState ps;
    /* trifast prep (per batch segment) */
    struct {
        float uOff, vOff, uPre, vPre, uNormMul, vNormMul, uNormDiv, vNormDiv;
        bool uMul, vMul;
    } prep[2];
    float clampU[2], clampV[2];
    bool halfTexel;
    int ci, ai;
    bool colorIsShade, alphaIsShade, fogFactorConst, shadeAlphaIsOne, useFog, useBlendColor;
    float fogFactorConstVal;
    uint32_t verifyHash;
    uint32_t stats_fast, stats_exact, stats_fallback, stats_tris, stats_combines, stats_draws;
};

void Init(RunCtx& c, GdxInterpSeam* I, bool verify);
/* Derive the fast-path prep from the interpreter's current state snapshot; false when the
 * fast path is unavailable (no packed batch / dirty memo / trifast off). */
bool DerivePrep(RunCtx& c);
/* One triangle: fast path when possible, else the exact path (adopted). */
void Tri(RunCtx& c, uint8_t a, uint8_t b, uint8_t d);
/* Flush (or, in verify, hash and discard) the open native batch and close it; the next tri
 * goes exact. */
void FlushNative(RunCtx& c);

} // namespace GdxNativeTri

#endif /* GDX3DS_NATIVE_TRI_H */
