/* port/3ds/gfx/gdx3ds_gputl.h -- n60gpu: [debug] gputl, the GPU timeline of each frame measured on the
 * console, per eye and per chunk, cheap enough to leave on (docs/research/native/n60gpu-progress.md §5).
 *
 * How: libctru's GSP event thread runs gxCmdQueueInterrupt (which counts the finished GX entry and
 * submits the next one) and THEN the user callback of the same event (gspgpu.o, checked in the
 * disassembly). We register callbacks for PSC0 (memory fill), PPF (display transfer / texture copy)
 * and P3D (command list); each stamps svcGetSystemTick and the queue's completed-entry count. At the
 * frame end the frame's entries are classified (kind, eye, chunk) and the submit moments noted (each
 * [overlap] kick, C3D_FrameEnd's run); at the next frame begin -- after C3D_FrameBegin waited for that
 * queue -- every entry has its completion stamp, and
 *   busy(k) = done(k) - max(done(k-1), avail(k))
 * is the GPU (+ GSP dispatch) time of entry k. Sums per eye (L / R by the colour buffer the entry's
 * prologue binds) and per chunk (entries submitted at kick 1, kick 2, frame end) give the GPU ms of
 * each eye's sky..dress, mach..fx and HUD, the display transfers and the clears; done(last) - frame
 * end = the GPU tail that delays the swap request.
 *
 * Receipt `[gputl]` every `[debug] gputl_window` frames (default 256). Cost: one callback per GX
 * entry (~20 per 3D frame, a ring store each), a scan of the frame's SMALL command lists (<= 96 words:
 * the eye prologues) at the frame end, ~20 entries of arithmetic at the begin. Default 0. */
#ifndef GDX3DS_GPUTL_H
#define GDX3DS_GPUTL_H

#ifdef __cplusplus
extern "C" {
#endif

int gdx3ds_gputl_on(void);
/* After C3D_FrameBegin of a fresh frame: the previous frame's queue has finished; account it. */
void gdx3ds_gputl_frame_begin(void);
/* An [overlap] kick: phase 0 right before its gxCmdQueueRun (the queued entries become available
 * now), phase 1 after the stereo chunk replay was added (those entries are available now). */
void gdx3ds_gputl_note_kick(int phase);
/* n60tail: queue entry `entry` (a command list too long for the prologue scan) ends with a colour
 * buffer switch to `eye` (0 L, 1 R): a right prologue folded into the closing left list. */
void gdx3ds_gputl_note_eye(int entry, int eye);
/* After C3D_FrameEnd: classify the frame's entries; the queue is running from now. */
void gdx3ds_gputl_frame_end(const void* mainColor, const void* rightColor);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_GPUTL_H */
