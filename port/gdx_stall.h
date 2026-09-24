#pragma once
/* Main-thread load-stall census (A10).
 *
 * WHY: on hardware the in-race beat sequence is nearly identical run to run and
 * config to config (e.g. `21 51 59 30 46 60 55 39 15 43 43 43` fps per 5 s beat),
 * and the same dips into the teens appear under every renderer lever. Those dips
 * are not draw cost -- they are blocking load work executed on the MAIN thread
 * inside the race window, which is why a 28% crowd-frame win did not move mean fps.
 * This module attributes that work: every span over a threshold is logged with a
 * cause, and a per-run histogram ranks the causes by total blocked milliseconds.
 *
 * Gate: `[debug] stall = 1` (default OFF -- no timing calls, no receipts, byte
 * identical behaviour). `[debug] stall_ms` sets the per-span threshold (default 8),
 * `[debug] stall_frame_ms` the whole-frame threshold (default 50).
 *
 * The per-span `[stall]` receipts are capped at a few per frame: on the 3DS each one is an
 * SD write, and an unbounded stream would manufacture the stalls this census exists to find.
 * The `[stall-table]` totals count every span regardless, so raising `stall_ms` on hardware
 * costs receipts, never accuracy.
 *
 * Contract: gdx_stall_begin/end nest (self time is charged to the innermost span,
 * so a parent never double-counts its children) and are MAIN-THREAD only -- spans
 * opened on any other thread are counted in a separate off-thread bucket and never
 * reach the main-thread table, because a background worker's seconds are exactly
 * what this census must NOT confuse with a frame stall.
 *
 * "Main thread" here means the LOGICAL game thread, not an OS thread id. On the 3DS every
 * game fiber is its own libctru thread (gdx_fiber_3ds.c), so gdx_stall.c asks
 * gdx_sched_on_host_thread() -- the same test gdx3ds_mprof_on_main() uses -- and a span
 * opened on the Main fiber is correctly a span on the main thread. The render thread and the
 * boot preload worker are NOT the logical game thread and stay in the off-thread total, so a
 * cause wired on the render thread (the SHOT readback + BMP writes, n64_gfx_bridge.cpp) does
 * not appear as a table row even after this fix.
 *
 * This is measurement only: it never changes what the game does or when, so a run
 * with `stall = 1` is pixel-identical to one without it (the receipts go to the log,
 * which is not a frame input).
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GDX_STALL_SEG_BLOB = 0, /* segment/audio blob first touch: archive read + inflate */
    GDX_STALL_SEG_DECODE,   /* asset-segment image build: MIO0 decode + fixups */
    GDX_STALL_VENUE_TEX,    /* venue texture segment bind + building-texture slice */
    GDX_STALL_TEX_BUILD,    /* host texture copy/convert/upload */
    GDX_STALL_AUDIO_LOAD,   /* audio font / sequence / sample load */
    GDX_STALL_FS,           /* archive/filesystem read not covered above */
    GDX_STALL_HEAP,         /* heap growth (large malloc) */
    GDX_STALL_GPU_WAIT,     /* main waiting on the render thread / GPU */
    GDX_STALL_OTHER,        /* frame overrun left after every attributed span */
    GDX_STALL_CAUSE_COUNT
} GdxStallCause;

/* Arm/disarm. `enabled` 0 makes every other entry point a no-op (the begin/end pair
 * still balances, so call sites need no gate of their own). Idempotent. */
void gdx_stall_configure(int enabled, int spanThresholdMs, int frameThresholdMs);
int gdx_stall_enabled(void);

/* Record the calling thread as "main": only spans opened here enter the ranked table. */
void gdx_stall_set_main_thread(void);

/* Open/close a span. Unbalanced ends are ignored; depth overflow degrades to charging
 * the whole span to the outermost open cause (never crashes, never negative). */
void gdx_stall_begin(GdxStallCause cause);
void gdx_stall_end(void);

/* Record a span somebody else already timed (e.g. the [seg-src] blob receipt). Charged
 * exactly like a begin/end pair, including the subtraction from the enclosing span. */
void gdx_stall_note(GdxStallCause cause, double ms);

/* Frame boundary, main thread, once per iteration of the host frame loop. Closes the
 * previous frame: charges its unattributed remainder to GDX_STALL_OTHER and prints a
 * `[stall] frame=...` line when the frame exceeded the frame threshold. */
void gdx_stall_frame(unsigned long frameNumber);

/* Print the ranked table (`[stall-table]` rows, worst total first). `tag` names the
 * window. Pass reset=1 to zero the counters so the next window is independent. */
void gdx_stall_report(const char* tag, int reset);

#ifdef __cplusplus
}

/* Scope guard for C++ call sites with several exit paths (early returns, exceptions):
 * a hand-placed gdx_stall_end() on one of them is the bug this makes impossible. */
class GdxStallScope {
  public:
    explicit GdxStallScope(GdxStallCause cause) { gdx_stall_begin(cause); }
    ~GdxStallScope() { gdx_stall_end(); }
    GdxStallScope(const GdxStallScope&) = delete;
    GdxStallScope& operator=(const GdxStallScope&) = delete;
};
#endif
