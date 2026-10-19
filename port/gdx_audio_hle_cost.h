/* gdx_audio_hle_cost — what HLE audio synthesis COSTS in CPU, as opposed to what it takes in
 * wall time. NEXT.md C4k, agent o3dsaudio.
 *
 * WHY THIS EXISTS. [othx] `syn=` is the wall time of the HLE run on the producer thread. On the
 * Old 3DS that thread lives on core 1, the syscore, where the application gets a 30% share
 * (APT_SetAppCpuTimeLimit) and every system module preempts it. The 2026-09-29 hardware run
 * read `syn` 54.5 ms/frame and nobody could say how much of that was synthesis and how much was
 * the thread not running. Horizon has no per-thread CPU clock a 2016 Luma exposes, so this
 * separates the two by SAMPLING:
 *
 *   The dispatch loop reads the system tick once per ABI command (one svcGetSystemTick per op,
 *   ~0.6 us on hardware), so each op gets a wall duration and a work size ("units": samples for
 *   the DSP kernels, bytes for the DMA/copy ops, 1 for register ops). Per op class the
 *   duration/unit rate goes into a half-octave histogram. Preemption can only ADD
 *   time, and it lands on a few ops (a sysmodule slice is long against a 20-200 us op), so the
 *   units-weighted MEDIAN rate is the op's unpreempted cost. An op whose rate is more than 4x
 *   that median (two octaves) is counted as PREEMPTED: its CPU is re-estimated as
 *   units x median rate, and the excess is reported as `pre=` with a `gaps=` count.
 *
 *   cpu = the estimate, wall = the measurement, pre = wall - cpu. An op slowed by less than 4x
 *   (a cache miss storm, a short IRQ) stays in cpu, so cpu is an UPPER bound on true CPU and
 *   pre a LOWER bound on lost time -- PROVIDED fewer than half of an op class's units were
 *   preempted. If the scheduler slices finer than the ops (a short app-limit period), the
 *   median itself is inflated, and that is why there is a second, independent estimate:
 *
 *   THE CORE PROBE (`[synx] core`): once per window the producer spins on the tick for 16 ms
 *   and books every step longer than 25 us as time it was not running. `run=` is the share the
 *   core actually granted the producer, `cpuP` = synth wall x that share, and the gap count and
 *   lengths are the scheduler's slice structure. cpu ~ cpuP means both hold; cpu >> cpuP means
 *   the slices are finer than the ops and cpuP is the one to believe.
 *
 * THE BUDGET. INTERLEAVE's per-channel sample count is the audio each run produced, so the
 * receipt also derives `need` = CPU ms per SECOND of audio at the output rate, and `share` =
 * need / 10 (percent of one core). Real time needs share below the core's grant: 30% on the
 * Old 3DS syscore. `wneed` is the same with wall time (what the thread actually took).
 *
 * THREADING. The producer (core 1) writes; the main thread emits in the [othx] window. The
 * producer owns the accumulator and, at the start of a run, publishes it into a second copy
 * when the main thread has asked (a flag), then resets. The main thread only ever reads the
 * published copy, which is at most one window stale. No locks, no heap: two static structs.
 *
 * COST WHEN OFF: `gdx_hle_cost_armed` is a plain int; the dispatch loop tests it once per op.
 * Armed by the gputrace latch, exactly like the [othx] ledgers. Plain C99, no platform headers:
 * the clock is injected, so port/tests can compile this file unmodified.
 */
#ifndef GDX_AUDIO_HLE_COST_H
#define GDX_AUDIO_HLE_COST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxHleCostClass {
    GDX_HLECOST_ADPCM = 0,
    GDX_HLECOST_RESAMPLE,
    GDX_HLECOST_ENVMIX,
    GDX_HLECOST_MIXER,
    GDX_HLECOST_ADDMIX,
    GDX_HLECOST_INTERLEAVE,
    GDX_HLECOST_FILTER,
    GDX_HLECOST_HILOGAIN,
    GDX_HLECOST_LOADBUFF,
    GDX_HLECOST_SAVEBUFF,
    GDX_HLECOST_CLEARBUFF,
    GDX_HLECOST_DMEMMOVE,
    GDX_HLECOST_OTHER, /* SETBUFF, ENVSETUP1/2, LOADADPCM, SETLOOP, S8DEC, INTERL, no-ops */
    GDX_HLECOST_COUNT
} GdxHleCostClass;

extern int gdx_hle_cost_armed;

/* Arm (1) / disarm (0). `now` returns a monotonically increasing tick; `tickHz` its rate. */
void gdx_hle_cost_arm(int on, long long (*now)(void), double tickHz);

/* Producer side, from gdx_audio_hle_run. run_begin stamps the chain; op_end charges the time
 * since the previous stamp to (cls, units) and re-stamps; run_end closes the run. */
void gdx_hle_cost_run_begin(void);
void gdx_hle_cost_op_end(int cls, uint32_t units);
void gdx_hle_cost_note_output(uint32_t frames); /* INTERLEAVE: audio frames this run produced */
void gdx_hle_cost_note_voice(void);             /* ADPCM/S8DEC: one voice decode */
void gdx_hle_cost_run_end(void);

/* Main-thread side. Formats the most recently PUBLISHED window as receipt lines and asks the
 * producer to publish the next one. Line `index` 0..N-1; returns 0 when there is no such line
 * (or nothing published yet). Per-frame columns divide by the frames the PUBLISHED window
 * spanned (the frame numbers passed to the two requests that bracket it). `simd` = the fast-path lever state, printed so an A/B log says which path it timed. */
int gdx_hle_cost_format(int index, char* out, size_t cap, unsigned long frameNo, int simd,
                        unsigned outputRateHz);
void gdx_hle_cost_request_publish(unsigned long frameNo);

/* Test/diagnostic access to the estimator, independent of the clock: feed synthetic op
 * samples into the accumulator and read the per-class totals of the published window. */
typedef struct GdxHleCostClassTotals {
    double wallTicks;
    double cpuTicks;
    double units;
    double gaps;
    double medianTicksPerUnit;
    double opCount;    /* n60-3d12: raw op count this class saw this window */
    double maxOpTicks; /* n60-3d12: the single costliest op in this class this window */
} GdxHleCostClassTotals;
void gdx_hle_cost_record(int cls, uint32_t units, uint64_t ticks);
void gdx_hle_cost_publish_now(void);
GdxHleCostClassTotals gdx_hle_cost_published_class(int cls);

/* n60-3d12: [audiocost] -- per-op-class AVG/MAX microsecond cost + op count, on its own
 * tag, independent of the gputrace-only [synx]/[synop] lines gdx_hle_cost_format produces.
 * Same accumulate/publish machinery (gdx_hle_cost_arm/_run_begin/_op_end/_request_publish
 * above still drive it); a caller that arms this ledger OUTSIDE gputrace (e.g. gated on
 * [debug] diag_audio alone, cheaper than gputrace's whole GPU-side tracing) can request a
 * publish and format these lines on its own cadence. Line `index` 0..N-1, 0 past the end or
 * before anything is published. `windowSeconds` is printed as-is (the caller's own elapsed
 * time since its last report); see gdx_hle_cost_published_span_seconds for the ledger's own. */
int gdx_hle_cost_format_audiocost(int index, char* out, size_t cap, double windowSeconds);
double gdx_hle_cost_published_span_seconds(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_HLE_COST_H */
