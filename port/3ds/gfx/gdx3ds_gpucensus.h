/* port/3ds/gfx/gdx3ds_gpucensus.h -- n60gpu: a census of the GPU work of a frame, per eye and per
 * scene category, read from the frame's own GX queue and PICA command lists
 * (docs/research/native/n60gpu-progress.md).
 *
 * `[debug] gpucensus` (default 0 = off, every hook is a branch on a latched int):
 *   1  STATS: after C3D_FrameEnd the frame's GX queue is walked (read-only): every command list is
 *      decoded register by register, and each draw is counted under its eye (the colour buffer the
 *      GPU writes: main target = L, the stereo right target = R) and its scene category (the [c5cat]
 *      marker in force where its words were written). Per draw: vertices, triangles, blending,
 *      depth test / write, alpha test, texture units (format, size, FCRAM / VRAM). Per entry:
 *      lists and words, clears (memory fills), display transfers, texture copies. Receipt
 *      `[gpucen]` every `[debug] gpucensus_window` frames (default 128).
 *   2  FILL (emulator only, the image is destroyed): in the frames it counts, every fragment
 *      writes +1 into the red channel (texenv = constant, blend ADD ONE ONE, no alpha / stencil
 *      test, depth test ALWAYS without depth write), so the main target's red channel after the
 *      frame = how many fragments each pixel rasterised. The counted category rotates per frame
 *      (all, sky, trk, dress, mach, shd, fx, hud, misc); the other categories' draws get 0
 *      vertices. Mono only, `[debug] overlap_kick = 0` (the lists are patched before they run).
 *      Receipt `[gpufill]`.
 *   3  FILL, depth kept: as 2 but every category draws and the depth test / write are the game's:
 *      the red channel counts the fragments that PASS the depth test (what reaches blending).
 *
 * The census never changes the image in mode 1 (it reads queue entries after they were queued);
 * modes 2/3 are measurement runs only. */
#ifndef GDX3DS_GPUCENSUS_H
#define GDX3DS_GPUCENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = off, else the latched [debug] gpucensus mode. */
int gdx3ds_gpucensus_mode(void);
/* 1 when the census wants the [c5cat] scene markers written (port/n64_sched.c gdx_c5cat_armed). */
int gdx3ds_gpucensus_markers_armed(void);
/* A scene-category marker (the 0x61 marker arm, gdx_rt_glue.cpp): the category in force from the
 * command-list word being written now. */
void gdx3ds_gpucensus_marker(unsigned category);
/* After C3D_FrameBegin (StartFrame, first begin of the frame). */
void gdx3ds_gpucensus_frame_begin(void);
/* Right before C3D_FrameEnd (after the overlap drain and the stereo frame end): modes 2/3 patch the
 * frame's lists here, before the queue runs them. */
void gdx3ds_gpucensus_before_frame_end(void);
/* Right after C3D_FrameEnd: the stats walk; modes 2/3 read the count image back. The targets are
 * the main (left) and right-eye framebuffers (NULL when absent). */
void gdx3ds_gpucensus_after_frame_end(const void* mainColor, const void* mainDepth, const void* rightColor,
                                      const void* rightDepth);
/* Texture upload census: one RGBA8 texture (R, G, B, A bytes per texel) as uploaded; counts which
 * narrower PICA formats would hold it bit-exactly. */
void gdx3ds_gpucensus_note_upload(const uint8_t* rgba, uint32_t width, uint32_t height, int atlas);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_GPUCENSUS_H */
