/* port/3ds/gdx3ds_tickahead.h -- N60TICK4: tick-ahead on the Old 3DS ([debug] tick_ahead, DEFAULT 0).
 *
 * Design: docs/research/native/n60tick4-progress.md. The Old 3DS gives the app core 0 at full
 * speed and a share of core 1 (APT_SetAppCpuTimeLimit). Tick-ahead is the New 3DS render-thread
 * PIPE mode with the roles swapped: the render thread (gdx3ds_renderthread.cpp: StartFrame, the
 * interpreter walk + NRR draws, EndFrame, the oracle) runs on CORE 0, and the whole game host
 * (the frame loop, the APT pump, every game fiber -- fibers are kernel threads created on the
 * core of the thread that converted itself, gdx_fiber_3ds.c) runs on a thread created on
 * CORE 1. Render(N) on core 0 overlaps tick N+1's logic + display-list build on core 1; the game's
 * own DP-done protocol is the join, so the logic is the same code in the same order.
 *
 *   tick_ahead = 0   off (default): nothing here runs beyond one config read.
 *   tick_ahead = 1   the real thing: host thread on core 1, render thread on core 0, pipe.
 *   tick_ahead = 2   bisect lever: the same pipe protocol with the host left on the main thread
 *                    (core 0). No overlap; proves the protocol separately from the core move.
 *   tick_ahead_min_pct = 50   mode 1 needs at least this core-1 share granted (readback); below
 *                    it the boot falls back to the ordinary sequential path (receipt REFUSED).
 *   tick_ahead_every = 512    frames between [tickahead] window receipts (0 = never).
 *
 * New 3DS: refused with a receipt (the core-2 render thread already overlaps there).
 * Receipts: `[tickahead] init ...` once, `[tickahead] host thread ...` from the host thread, then
 * one `[tickahead] frame=` window line (+ the `[rt]` window) every tick_ahead_every frames. */
#ifndef GDX3DS_TICKAHEAD_H
#define GDX3DS_TICKAHEAD_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_TICKAHEAD_OFF = 0,
    GDX3DS_TICKAHEAD_CORE1 = 1, /* host on core 1, render on core 0 */
    GDX3DS_TICKAHEAD_PIPE0 = 2  /* host on core 0 (main), render thread on core 0: protocol only */
};

/* Boot, on the main thread, after the syscore share exists (audio init) and BEFORE
 * gdx_sched_init: latches the lever, checks the model and the share. Returns the mode. */
int gdx3ds_tickahead_boot(void);
/* The latched mode (0 before boot). */
int gdx3ds_tickahead_mode(void);
/* Runs hostMain(arg) -- the rest of main(): gdx_sched_init, bootproc, the frame loop, the
 * teardown -- on the core-1 host thread in mode 1 (the main thread serves the APT pump, then
 * joins), or
 * inline otherwise. If the core-1 thread cannot be created, falls back to inline with the mode
 * reset to 0 (receipt). Returns hostMain's result. */
int gdx3ds_tickahead_run_host(int (*hostMain)(void* arg), void* arg);
/* The APT pump. Mode 1: the host thread (core 1) hands aptMainLoop() to the MAIN thread (core 0,
 * idle in run_host) and blocks until it returns, so every APT hook (HOME / sleep / close: the
 * render-thread park, the audio park, the syscore drop, libctru's own suspend steps) runs on the
 * same thread and core as on the ordinary path, at full speed, while the host and every game
 * fiber are blocked. Other modes: aptMainLoop() inline. */
int gdx3ds_tickahead_apt_main_loop(void);
/* The core the render thread must be created on (0), or -1 when tick-ahead is off. */
int gdx3ds_tickahead_render_core(void);
/* Host loop, once per iteration (tail): the window receipt. One int test when off. */
void gdx3ds_tickahead_frame_end(unsigned long frame);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_TICKAHEAD_H */
