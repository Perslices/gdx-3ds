/* port/3ds/gfx/gdx3ds_skylate.h -- n60tail: [native] sky_late, the sky drawn AFTER the opaque track
 * (docs/research/native/n60tail-progress.md §3).
 *
 * The race frame draws the sky first (3-5 full-screen-ish layers, 2 of them blended, no depth) and
 * then the track, which paints 65-70 % of the screen with opaque pixels: ~1.9 screens of sky per eye
 * are drawn and then overwritten. sky_late keeps every command word of the frame and changes only
 * the ORDER of two runs of command lists plus the stencil words of their effects:
 *
 *   - at the SKY [c5cat] marker the open list is closed; the sky's lists run with the stencil test
 *     EQUAL 0 (no stencil write);
 *   - at the TRK marker the list is closed again; the track's lists (and the dressing's, with
 *     sky_late +4) run with the stencil test ALWAYS and zpass REPLACE 1: every fragment that writes
 *     colour also writes stencil 1;
 *   - at the next marker the list is closed; if every effect bound in the region overwrote colour
 *     whole (no blending that reads the colour buffer, logic op COPY, colour mask RGBA, default
 *     fragment mode), the sky wrote no depth, and the run held only command lists, the GX queue
 *     entries [sky][region] are rotated to [region][sky].
 *
 * Why the image is the same: the sky writes no depth, so the region's depth tests see the same
 * buffer in either order. A pixel the region wrote colour to was, before, sky overwritten whole by
 * an opaque fragment; now that fragment wrote stencil 1 and the sky's test fails there. A pixel it
 * did not write keeps stencil 0 (the frame's depth clear clears stencil, D24S8 value 0) and the
 * colour from before the region, so the sky composes over exactly what it composed over before
 * (blending and fog per draw are unchanged: the same draws, the same state, in the same order
 * among themselves). Each moved run starts with citro3d's full state (the stereo chunk machinery's
 * self-contained start), so every list runs from the GPU state it ran from before. A frame that
 * does not qualify keeps the original order; the stencil words are then inert (the sky's EQUAL 0
 * passes everywhere on a cleared buffer, the region's writes are never read).
 *
 * Lever (default 0): 1 = on (region = the track), 2 = on in even 4096-frame blocks ([gputl] tp=4),
 * + 4 = the region runs through the dressing too (5, 6). Refused under the C6 [replay], the NRR
 * verify (nrr_direct = 2) and without [c5cat] markers. Receipt `[skylate]` every 512 frames. */
#ifndef GDX3DS_SKYLATE_H
#define GDX3DS_SKYLATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* StartFrame (after the stereo frame reset): per-frame state, the A/B phase. */
void gdx3ds_skylate_frame_begin(unsigned long frameIndex);
/* A [c5cat] scene marker (gdx_rt_glue.cpp), before the [overlap] kick of the same marker. `seam` = the
 * interpreter seam (its open batch is flushed before a list boundary). */
void gdx3ds_skylate_marker(unsigned category, void* seam);
/* ClearFramebuffer of the main target with depth (queued or elided as redundant). */
void gdx3ds_skylate_note_depth_clear(void);
/* StartDrawToFramebuffer inside the frame. */
void gdx3ds_skylate_note_bind(int fbId);
/* Gdx3dsStereo::ReplayEnter, right before it closes the pre-body list / right after the body starts. */
void gdx3ds_skylate_before_enter(void);
void gdx3ds_skylate_after_enter(int bodyStart);
/* EndFrame, before the [overlap] drain: an open sky / region ends unreordered. */
void gdx3ds_skylate_frame_end(void);
/* Nonzero while the track run is open (its effects mark stencil): every NRR hand-off then calls
 * gdx3ds_skylate_note_nrr with its category (an NRR SKY one keeps the order: a held or late sky draw
 * would mark stencil itself). */
extern int gdx3ds_skylate_marking;
void gdx3ds_skylate_note_nrr(int nrrCat);
/* 4 while this frame runs with sky_late on, else 0 (the [gputl] tp= bit). */
int gdx3ds_skylate_phase(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_SKYLATE_H */
