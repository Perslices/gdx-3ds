/* port/3ds/gfx/gdx3ds_pace.h — n60gp: the swap-anchored vblank gate (docs/research/native/
 * n60gp-progress.md §1.2, §5.1).
 *
 * The stock frame begin (gdx3ds_gpu_prof.c, CADENCE) waits for the NEXT vblank unless more than
 * one LCD period passed since the previous begin returned. That elapsed-time test does not know
 * the vblank phase: when the previous begin returned late after its vblank (after a frame that
 * overran, or behind the GX queue wait), the next begin can arrive just after a vblank with less
 * than a period elapsed, and the wait then idles a whole period although that vblank already
 * latched the previous frame. The frame repeats a vblank its work did not cause (the "phase trap").
 *
 * `[debug] pace_swapgate` anchors the decision on the swap instead. C3D_FrameBegin(0) runs first
 * (the GX queue wait; after it the previous frame's top swap request is stamped by citro3d's
 * onQueueFinish, vendor/citro3d gdx/c3d_gdx.h), and an END GUARD right before C3D_FrameEnd waits
 * for VBlank0 when none fired since that swap request. The guard alone keeps every guarantee of
 * the stock sync: a VBlank0 lies between the previous swap request and this frame's display
 * transfer + swap, so at most one swap per vblank interval (no frame dropped, no transfer into the
 * buffer being scanned out, never more than 60 presents per second).
 *
 *   pace_swapgate = 0  stock (default; one int test)
 *                 = 1  HYBRID: the begin waits for VBlank0 only when none fired since the previous
 *                      swap request AND the stock rule would wait (less than a period since the
 *                      previous begin); + the end guard. Same latency as stock in a frame that
 *                      fits, no phase trap, and after an overrun the CPU keeps working.
 *                 = 2  END: the begin never waits for a vblank, only the end guard paces. The CPU
 *                      never idles while a frame can be built (maximum throughput, a frame's
 *                      overrun is absorbed by the next one's slack) at up to one more frame of
 *                      display latency in frames that fit (the present locks to the vblank).
 *     Without a fresh stamp (no top swap since the previous begin, or the vendored citro3d is not
 *     linked) a begin uses the stock rule. Only timing changes: game state and frames are the
 *     same (gates: [gstate], [fh], SHOTs).
 *   pace_ab_period = N (default 0): time-sliced A/B, stock in even blocks of N fresh begins
 *     (rounded to the 256-begin receipt window), pace_swapgate's rule in odd blocks.
 *   pace_swapgate_3d = M (n60stereo, default -1 = pace_swapgate): the rule while the 3D right eye
 *     runs. With pace_ab_period, 3D alternates pace_swapgate (even blocks) and M (odd blocks).
 *   pace_receipt  = 1  [pace] receipt every 256 fresh begins (also with the gate off):
 *     [pace] frame=N n= phase= | swaps= vbl= rep= vbl1= | waits= skips= long= trap= nostamp= eg= |
 *            wait_ms= eg_ms= work_ms= | hold=1/2/3/4+ tail_ms= max= late=
 *       n = fresh begins, phase = the rule (0/1/2; with an A/B the window never straddles a phase),
 *       swaps = top swap requests stamped, vbl = VBlank0 events, rep = vbl - swaps (vblank
 *       intervals that showed no new frame: the repeats the user sees), waits / skips = begins
 *       that waited for a vblank / did not, trap = begins where a VBlank0 had already passed since
 *       the previous swap request while the stock rule waits (counted in every phase: under the
 *       gate they are the waits it avoided), nostamp = stock-rule fallbacks, long = begin waits
 *       longer than one period, vbl1 = VBlank1 events, eg = end-guard waits, wait_ms / eg_ms =
 *       mean begin / end-guard vblank wait per begin, work_ms = mean begin-to-begin time outside
 *       the begin waits (end-guard waits included), hold = presented frames that stayed 1 / 2 / 3 /
 *       4+ vblanks on screen, tail_ms / max = the GPU tail (end guard entry -> top swap request),
 *       late = tails over 4 ms.
 *   pace_spin_us / pace_spin_jitter_us (test only, default 0): spin base + [0, jitter) µs
 *       (deterministic sequence) at the top of every fresh begin, i.e. synthetic CPU work, so the
 *       emulator lane (whose frames are far under one period) can reproduce the hardware W
 *       distribution and exercise both rules. */
#ifndef GDX3DS_PACE_H
#define GDX3DS_PACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the gate (or its A/B) or the receipt or the synthetic spin is armed; latched once the
 * config is loaded. 0 = the caller runs its stock begin unchanged. */
int gdx3ds_pace_armed(void);

/* The whole fresh-frame begin when armed: synthetic spin, then the stock rule's verdict
 * (`stockSkip()` returns 1 when the CADENCE rule skips the vblank wait; called after the spin),
 * C3D_FrameBegin and the vblank wait the active rule wants. Returns the ticks spent in the GX
 * queue wait in *queueTicks and in the vblank wait in *vblankTicks (either may be NULL). */
void gdx3ds_pace_frame_begin(int (*stockSkip)(void), uint64_t* queueTicks, uint64_t* vblankTicks);

/* Right before C3D_FrameEnd (after the [overlap] drain): under the gate, wait for VBlank0 when
 * none fired since the previous frame's top swap request, so this frame's display transfer and
 * swap never come before the previous swap is latched. A no-op in the stock phase. */
void gdx3ds_pace_frame_end_guard(void);

/* n60st3 ([debug] spike_ms): what the latest fresh begin saw, for the spike receipt. All ticks;
 * 0 = unknown (the pace module is not armed, no consecutive swap, no frame end yet). */
typedef struct GdxPaceFrameInfo {
    uint64_t queueTicks;      /* this begin's GX queue wait (the previous frame's GPU tail) */
    uint64_t vblankTicks;     /* this begin's vblank wait */
    uint64_t prevRenderTicks; /* the previous frame: its begin return -> its end guard entry */
    uint64_t prevEndGuardTicks; /* the previous frame's end-guard wait */
    uint64_t prevGpuTailTicks;  /* the previous frame: its end guard entry -> its top swap request */
    uint32_t swapGap;         /* vblanks between the two latest top swap requests (1 = 60 fps) */
    int phase;                /* the rule of this begin (0 stock, 1 hybrid, 2 end) */
} GdxPaceFrameInfo;
void gdx3ds_pace_frame_info(GdxPaceFrameInfo* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PACE_H */
