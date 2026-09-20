/* gdx3ds_track_interp.h — [native] track: the packed-batch + tri-memo snapshot every marker
 * replay's fast path runs from (gdx3ds_native_tri.cpp DerivePrep/FastTri).
 *
 * GdxTrackPackState is a read-only snapshot of the draw-state memo and packed-batch latches
 * an interpreter's triangle path leaves behind after resolving a triangle's state: exactly the
 * per-batch values the replay's fast tri append needs to reproduce the trifast emission
 * bit-for-bit (same expressions, same operand order) without re-running the state block.
 *
 * Filled by Fast::Interpreter::GdxTrackPackState (vendored interpreter.cpp, file-static memo
 * sGfxS7TriMemo + [triloop] latches) and by gdxn::Interp through the owned seam
 * (n64/gdx_seam_owned.cpp, Interp::mTriMemo + mPack*). Interpreter-neutral on purpose. */
#ifndef GDX3DS_TRACK_INTERP_H
#define GDX3DS_TRACK_INTERP_H

#include <stdint.h>

#include "gdx3ds_interp_seam.h" /* GdxSeamClip */
#include "gdx3ds_vbopack.h"

struct GdxTrackPackState {
    /* [triloop] packed batch latches */
    bool packBatch;                       /* the open batch is packed */
    float* packDst;                       /* its base write cursor */
    const Gdx3dsVboPackParams* params;
    const Gdx3dsVboPackAux* aux;          /* vertex-0 constants of the batch */
    /* [perf-s7] tri draw-state memo */
    bool memoValid;
    bool memoDirty;                       /* a state op ran since the memo was banked */
    const uint8_t (*inputMap)[7];         /* ColorCombiner::shader_input_mapping the memo was banked under */
    const void* prg;                      /* shader program the info below belongs to */
    uint32_t tm;
    uint32_t texWidth[2];
    uint32_t texHeight[2];
    uint32_t texWidth2[2];
    uint32_t texHeight2[2];
    uint32_t effectiveTile[2];
    bool usedTextures[2];
    GdxSeamClip clip;
    /* ucode attributes + tunables */
    uint32_t cullBoth;
    uint32_t cullFront;
    uint32_t cullBack;
    bool triFast;                         /* the trifast emission is the live path */
    bool rejectVariant;                   /* F3DEX2 Reject / FZeroFlxReject screening applies */
    float minClipW;                       /* the clipper's minimum w */
};

#endif /* GDX3DS_TRACK_INTERP_H */
