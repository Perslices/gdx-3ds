/* port/3ds/gdx3ds_core1probe.h -- N60TICK: the core-1 probe ([debug] core1_probe, DEFAULT 0).
 *
 * docs/research/native/n60tick-progress.md §3. A diagnostic that answers, on the console and
 * with the game (and its audio producer) running, the three questions the core-1 decision
 * needs:
 *   1. what share APT_SetAppCpuTimeLimit(30/50/80) actually grants (rc and the value read
 *      back; a CIA's exheader caps it at MaxCpu & 0x7F, Luma pm/reslimit.c), measured by a
 *      spinning core-1 thread BELOW the audio threads' priority (= what a worker gets);
 *   2. the wall time a core-1 worker needs for a fixed job (calibrated CPU on core 0) while
 *      audio runs, at a priority below the producer and equal to it;
 *   3. GSP (4 KB GSPGPU_FlushDataCache) and FS (an IPC with no card I/O, and a 512 B card
 *      read) latency seen from core 0, idle vs core 1 busy.
 *
 * Phases, each `core1_probe_window` ticks (default 240), per percent of `core1_probe_pcts`
 * (default "30,50,80"): idle, job_lo (worker prio 0x30), job_eq (0x19, the producer's), spin
 * (0x30, the whole window); then one cool window at the granted share; repeat. First window
 * after `core1_probe_start` ticks (default 600).
 *
 * HOME / sleep: suspend() stops the spin, waits (bounded) for the worker to park, and puts
 * the share back to what audio was granted BEFORE the audio park and the suspend drop (C4j /
 * homefix lessons: nothing of ours runs on core 1 across a transition, the share is the
 * granted one); resume() restarts the current window.
 *
 * Receipts: `[c1p] init ...`, one `[c1p] w=...` line per window, `[c1p] park/resume`,
 * `[c1p] stop`.
 *
 * N60CORE1 (docs/research/native/n60core1-progress.md): every window line also carries what the
 * share costs the GAME -- `loop=med/p90/max` host-loop period (us), `rep=k/n` periods > 20 ms,
 * `under=+` drain underrun edges, `ndrop=+` libctru ndsp dropped DSP frames, `ndsp=` the core of
 * the ndsp thread ([debug] ndsp_core1). With the probe off and [debug] ndsp_core1 or
 * syscore_pct set, the same hook emits a `[core1] frame=` watch line every 1024 ticks (loop
 * avg/max, rep, share readback, under/ndrop deltas). A CIA above 30% needs a CIA built with
 * -DGDX_3DS_CIA_MAXCPU=80 (the unit is CIA-only: no .3dsx leg exists for it). Unarmed, every entry point is one int test; nothing here reads or writes game
 * state (main-thread timing only), so [gstate] is unchanged with the lever on. */
#ifndef GDX3DS_CORE1PROBE_H
#define GDX3DS_CORE1PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Host loop, once per iteration (tail). Lazily latches the lever on the first call. */
void gdx3ds_core1probe_tick(unsigned long frame);
/* APT suspend side, before the audio park: park the worker, restore the granted share. */
void gdx3ds_core1probe_suspend(const char* phase);
/* APT restore side, after the cpu state is re-applied: restart the current window. */
void gdx3ds_core1probe_resume(const char* phase);
/* Teardown, before the audio shutdown: join the worker, restore the granted share. */
void gdx3ds_core1probe_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
