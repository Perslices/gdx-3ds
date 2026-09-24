/* port/3ds/gdx3ds_mainprof.h — G-MAINPROF: the [mprof] sectioned MAIN-THREAD profiler.
 *
 * WHY THIS EXISTS (docs/research/native/NEXT.md task A8). `[prof]` (gdx3ds_gpu_prof.h)
 * splits the render thread's CPU build into seven sections. Nothing had ever split main's,
 * even though the whole game frame -- logic, DL build, audio pump, asset loads and the
 * bridge pre-pass -- runs there.
 *
 * A8 was filed on the reading that `[rt] waitMain` 14-16 ms vs `waitRender` < 1 ms meant
 * the render thread idles waiting for the game thread. It does not: `waitMain` is
 * accumulated in `WaitDone()` (gdx3ds_renderthread.cpp:144), which only MAIN calls
 * ("all main waits", :85), so those numbers say main is BLOCKED 14-16 ms/frame waiting for
 * the render thread. The render thread is still the bottleneck. See
 * docs/research/native/mainprof-progress.md M0. This profiler is therefore a HEADROOM map
 * -- what main would cost if it ever became the bottleneck, and which of its phases -- not
 * a bottleneck map. The `sub` column below is what makes the distinction measurable.
 *
 * SHAPE AND DISCIPLINE: deliberately the same as `[prof]`.
 *   - ini-gated: `[debug] mainprof = 1`, latched once between frames.
 *   - zero cost when off: every hook site is one int load + branch (gdx3ds_mprof_active)
 *     before any timer read; the decomp-side checkpoints are one call that tests the same
 *     int, in family with the GDX_CK / gdx_diag_verbose() calls already on those lines.
 *   - windowed means: 64-frame windows, ms/frame, reset at emit, no division until emit.
 *   - a receipt the tooling parses: `[mprof]` (see below), read by tools/dev/gdxdev/prof.py
 *     and tabled by `gdx-dev prof --main`.
 *   - svc + filelog, like GpuProfLog: retail hardware discards svcOutputDebugString.
 *
 * THE MODEL — one flat timeline, not a nesting tree.
 * `[prof]` can use exclusive/self time because its sections nest inside one call stack on
 * one thread. Main's frame does not: the host loop calls into the cooperative fiber
 * scheduler, a game fiber runs until it parks, control returns to the host mid-way through
 * the game's frame function, the host does more work, and the fiber is resumed later in the
 * same host iteration (renderthread-audit.md §1, §8). So instead of a stack, this TU keeps
 * ONE "current bucket" plus the tick at which it opened; gdx3ds_mprof_enter(b) closes the
 * open bucket into its accumulator and opens `b`. Every microsecond of main-thread wall
 * time between the first enter() of a frame and the last therefore lands in exactly one
 * bucket: the buckets PARTITION main's frame, they do not merely sample it, and their sum
 * is the frame's main-thread wall time (`tot=`).
 *
 * Consequences of the flat model, stated so nobody re-derives them from a table:
 *   - A game fiber that parks (the DP-done wait, the framebuffer spin) simply stops being
 *     charged at the host's next enter(): the park itself costs nothing and is not a bucket.
 *   - The few instructions a resumed fiber runs before it reaches its next checkpoint are
 *     charged to whichever host bucket was open when gdx_dispatch()/gdx_vi_tick() resumed
 *     it (`vi` and `disp`). That is the only attribution smear in the model and it is
 *     bounded by a loop re-check.
 *   - `oth` is the bucket that is open when nothing has claimed the clock -- boot before
 *     the first checkpoint, teardown, and the gap between a frame function returning
 *     (GDX3DS_MPROF_CK_IDLE) and the host's next enter(). It is a real residual, not a
 *     subtraction, so unlike `[prof]`'s `oth` it can never go negative.
 *
 * THE BUCKETS. Game-fiber work first (that IS the submitting side), host loop second.
 *
 *   logic  func_800690FC — the game-mode update: physics, AI, 30 machines, HUD state
 *          (plus Gfx_InitBuffer's pool-half flip, which is a pointer swap)
 *   dlb    func_80069698 — the display-list build (the game writing the DL main submits)
 *   brm    gdx_gfx_job_prepare — the bridge pre-pass, on MAIN under `[debug] bridgemain=1`
 *          (balance-progress.md M1). Same milliseconds the `[rt] brMain=` column reports;
 *          carried here so one line ranks it against the game's own work.
 *   aud    all audio work that lands on main: Audio_Update on the game fiber (sys_gfx.c,
 *          the ROM audio driver's per-frame pump), gdx_audio_thread_notify_frame (the
 *          producer condvar kick) and an M_AUDTASK run through gdx_audio_lle_run from a
 *          game fiber. NOT the HLE producer itself: that is a core-2 thread of its own.
 *   lseg   Gfx_LoadSegments
 *   sla    the DP-status drain + Segment_LoadAssets + Transition_SetBackgroundBuffer +
 *          osViSwapBuffer (everything the game does after its DP-done wait)
 *   seg    EnsureAssetSegmentImage — asset segment faults, nested out of whatever bucket
 *          was open (first loads, mode reloads); main-context calls only
 *   dma    GdxSegmentSourceRead — the single ROM/archive read funnel (Dma_RomCopy, MIO0
 *          staging, asset byte loads), nested out; main-context calls only
 *   stask  Gfx_SetTask -> osSpTaskStartGo self time, i.e. submit minus brm minus sub
 *   sub    the backpressure wait inside gdx3ds_rt_submit_task: main blocked because
 *          render(N) has not finished and the game is about to rewrite the pool half it
 *          reads. Together with `jdp` this is THE bottleneck test: large means the render
 *          thread is still the constraint (it is, on hardware), ~0 means main became it.
 *
 *   apt    aptMainLoop (the APT pump; HOME/sleep transitions happen inside it)
 *   pace   gdx3ds_rt_pace_vblank — the deliberate vblank alignment wait (slack, not work;
 *          only entered when the render thread is on, and the adaptive policy SKIPS the
 *          wait whenever the LCD boundary is already past, which on hardware crowd frames
 *          it always is — `pace=0/64` in the [rt] receipt)
 *   inp    gdx_dev_gates_refresh + HandleEvents + gdx_controller_poll + fixed-aspect tick
 *   beg    gdx3ds_rt_frame_begin (queue BEGIN to the render thread)
 *   vi     gdx_vi_tick host side + the fiber resume through the framebuffer spin
 *   disp   gdx_dispatch + gdx_sched_drain_deferred_wakes, host side
 *   jdp    gdx3ds_rt_wait_task — the join / DP-done post
 *   jtop   gdx3ds_rt_join_idle at the loop top (APT frame boundary)
 *   end    gdx3ds_rt_frame_end (queue END)
 *   tail   gdx_disk_save_tick, the oracle frame mark, gdx3ds_net_tick, the FPS HUD, the
 *          touch menu, gdx_frame_pacer_tick and the per-window telemetry emits
 *   oth    residual (see above)
 *
 * RECEIPT (two parts, same frame, so each line stays well inside the log's line cap):
 *   [mprof] frame=N n=64 part=1 nD=… dl=… tot=… fib=… host=… | logic=… dlb=… brm=…
 *           aud=… lseg=… sla=… seg=… dma=… stask=… sub=…
 *   [mprof] frame=N n=64 part=2 | apt=… pace=… inp=… beg=… vi=… disp=… jdp=… jtop=…
 *           end=… tail=… oth=…
 * All ms are per-frame window means. nD = draws/frame from the concurrent [prof] window
 * (0 unless `[debug] gputrace=1`), the same load proxy `gdx-dev prof --by-vtxn` buckets on.
 * dl = mean submitted display-list KB/frame — a load proxy main can see with gputrace off.
 * fib = Σ the game-fiber buckets, host = Σ the host-loop buckets; fib + host == tot.
 *
 * DIAGNOSTIC ONLY. Nothing here writes game state. The timer reads are svcGetSystemTick,
 * the replay is tick-driven under `[debug] det=1`, and the receipt is emitted from the
 * host loop at a fixed frame phase (window boundaries only) — never from inside a game
 * fiber — so an mprof run stays bit-parity with an mprof=0 run. `[fh]` proves it.
 */
#ifndef GDX3DS_MAINPROF_H
#define GDX3DS_MAINPROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Game-fiber buckets (the submitting side). Keep these first: the emit sums
     * [0, GDX3DS_MPROF_FIB_END) for the fib= column. */
    GDX3DS_MPROF_LOGIC = 0,
    GDX3DS_MPROF_DLB,
    GDX3DS_MPROF_BRM,
    GDX3DS_MPROF_AUD,
    GDX3DS_MPROF_LSEG,
    GDX3DS_MPROF_SLA,
    GDX3DS_MPROF_SEG,
    GDX3DS_MPROF_DMA,
    GDX3DS_MPROF_STASK,
    GDX3DS_MPROF_SUB,
    GDX3DS_MPROF_FIB_END,

    /* Host-loop buckets. */
    GDX3DS_MPROF_APT = GDX3DS_MPROF_FIB_END,
    GDX3DS_MPROF_PACE,
    GDX3DS_MPROF_INP,
    GDX3DS_MPROF_BEG,
    GDX3DS_MPROF_VI,
    GDX3DS_MPROF_DISP,
    GDX3DS_MPROF_JDP,
    GDX3DS_MPROF_JTOP,
    GDX3DS_MPROF_END,
    GDX3DS_MPROF_TAIL,

    /* Residual — must be last and is the bucket the clock idles in. */
    GDX3DS_MPROF_OTH,
    GDX3DS_MPROF_COUNT
};

/* Checkpoint ids the decomp's frame function passes to gdx_mprof_ck() (port/n64_sched.c
 * forwards to gdx3ds_mprof_ck). Plain small ints because decomp TUs are built for every
 * platform and must not include a 3DS header. Keep in sync with the patch
 * port/3ds/patches/decomp-3ds-mainprof-checkpoints.patch. */
#define GDX3DS_MPROF_CK_IDLE    0 /* frame function returned: nothing is claiming the clock */
#define GDX3DS_MPROF_CK_AUDIO   1 /* -> Audio_Update */
#define GDX3DS_MPROF_CK_LOGIC   2 /* -> Gfx_InitBuffer + func_800690FC */
#define GDX3DS_MPROF_CK_LSEG    3 /* -> Gfx_LoadSegments */
#define GDX3DS_MPROF_CK_DLBUILD 4 /* -> func_80069698 */
#define GDX3DS_MPROF_CK_DPWAIT  5 /* -> Gfx_FullSync + the DP-done park */
#define GDX3DS_MPROF_CK_SEGLOAD 6 /* -> DP drain + Segment_LoadAssets + capture + swap */
#define GDX3DS_MPROF_CK_FBSPIN  7 /* -> the framebuffer spin (yields to the host) */
#define GDX3DS_MPROF_CK_SETTASK 8 /* -> Gfx_SetTask -> osSpTaskStartGo */

/* 1 iff `[debug] mainprof=1` latched on. Every hook site tests this before anything else. */
extern int gdx3ds_mprof_active;
/* The open bucket and the tick it opened at. Written only from main context. */
extern int gdx3ds_mprof_cur;
extern uint64_t gdx3ds_mprof_t0;
extern uint64_t gdx3ds_mprof_ticks[GDX3DS_MPROF_COUNT];
/* svcGetSystemTick behind a plain function, so hook sites in TUs that never include
 * <3ds.h> (the bridge, the segment source) need no svc declaration. */
long long gdx3ds_mprof_now(void);
/* 1 when the caller is the host thread or one of the game fibers it created — i.e. the
 * logical main thread (port/3ds/os/gdx_fiber_3ds.c returns the logical scheduler id for
 * both). Hook sites that the render thread / audio thread / preload thread can also reach
 * must use gdx3ds_mprof_enter_safe, which tests this. */
int gdx3ds_mprof_on_main(void);

/* Close the open bucket, open `b`, return the bucket that was open (or -1 when off, which
 * gdx3ds_mprof_leave then ignores). Call ONLY from sites that are main-context by
 * construction (the host loop, the game frame function, osSpTaskStartGo). */
static inline int gdx3ds_mprof_enter(int b) {
    uint64_t now;
    int prev;
    if (!gdx3ds_mprof_active) {
        return -1;
    }
    now = (uint64_t)gdx3ds_mprof_now();
    gdx3ds_mprof_ticks[gdx3ds_mprof_cur] += now - gdx3ds_mprof_t0;
    gdx3ds_mprof_t0 = now;
    prev = gdx3ds_mprof_cur;
    gdx3ds_mprof_cur = b;
    return prev;
}

/* Same, for sites the render/audio/preload threads also reach (the bridge's segment
 * faults, the ROM read funnel). One extra TLS-pointer compare when armed. */
static inline int gdx3ds_mprof_enter_safe(int b) {
    if (!gdx3ds_mprof_active || !gdx3ds_mprof_on_main()) {
        return -1;
    }
    return gdx3ds_mprof_enter(b);
}

static inline void gdx3ds_mprof_leave(int prev) {
    if (prev >= 0) {
        (void)gdx3ds_mprof_enter(prev);
    }
}

/* Decomp frame-function checkpoint (see the GDX3DS_MPROF_CK_* ids). */
void gdx3ds_mprof_ck(int step);

/* Host loop. frame_begin latches the ini gate on the first call and starts the frame's
 * clock; frame_end folds the frame into the window and emits the receipt on the window
 * boundary. Both run on main, between frames. */
void gdx3ds_mprof_frame_begin(void);
void gdx3ds_mprof_frame_end(unsigned long frame);
/* One submitted display list, for the dl= load proxy (osSpTaskStartGo, main). */
void gdx3ds_mprof_note_task(unsigned long dlBytes);
/* 1 iff armed — for the boot receipt and the DBG tab. */
int gdx3ds_mprof_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_MAINPROF_H */
