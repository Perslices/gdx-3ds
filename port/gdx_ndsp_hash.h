/* gdx_ndsp_hash — a content-addressed digest of what the DSP was asked to render, and of
 * what it rendered.  `[debug] ndsphash`, default 0.
 *
 * WHY THIS EXISTS.  Nothing in this port has ever listened.  Four tracks changed audio
 * behaviour (`audiodiet` streamed the 10.25 MB sample table off the heap, `ekdeadlock`
 * unparked a load, `audioasync` chunked a blocking disk read, `audioring` added ring
 * instrumentation) and each one stated the same limitation: `audiodiet` §6.6 proved only that
 * "the driver asked for the same bytes and got identical bytes" and called the rendering an
 * INFERENCE; `covfinish` refused an audio census family because a feature "could be recorded
 * but never SAMPLED, so it would read as covered with no gate behind it"; `audioasync` §5
 * added a `level mag=/peak=` census and quoted it as weak — it catches "the samples stopped
 * arriving" and "the samples arrived as noise", not a subtly wrong sample, a swapped
 * instrument or a pitch error.
 *
 * WHY A NAIVE PCM HASH CANNOT WORK, which is the finding that shapes this file.  The audio
 * producer is its own OS thread and it is NOT deterministic under `[debug] det=1`:
 * `audiodiet` measured 3452 vs 3439 reads on two identical runs, and `audioasync` measured
 * `sil_ppk` at 37,056 vs 37,648.  The per-tick AI buffer LENGTH is derived from the 3DS ring
 * occupancy (`osAiGetLength()` -> `gAiBufLengths[]` in decomp/src/audio/rom/lib/thread.c:80),
 * and the ring occupancy is wall-clock driven, so two identical runs cut the same music into
 * different-sized chunks and the sequence player's note-ons land on different samples.  A
 * sample-exact hash of the output stream therefore fails at random, which is worse than no
 * gate at all -- exactly how `[audio-diet] sum` failed.
 *
 * WHAT IS STABLE, and it is the whole idea: the audio command list is the audio analogue of a
 * display list.  `AudioSynth_Update` emits an ABI command list per tick describing exactly
 * what to render -- which sample, at which pitch (A_RESAMPLE), at which envelope volumes
 * (A_ENVMIXER), into which mix -- and `gdx_audio_lle_run` -> `gdx_audio_hle_run` executes it.
 * The list is a complete description of the render.  So:
 *
 *      KEY   = digest of one tick's command list, CONTENT-ADDRESSED
 *      VALUE = digest of everything that tick wrote back to RDRAM (the PCM)
 *
 * and the instrument is the MAP from key to value.  Two runs are compared on the keys they
 * SHARE: the same command list must render the same bytes, in any run, at any tick index, at
 * any wall-clock.  That property is timing-independent BY CONSTRUCTION, which is the thing
 * every previous attempt lacked.  Keys only one run produced are the drift (a tick the other
 * run never performed) and are reported separately -- `only_in_a` / `only_in_b`, the same
 * vocabulary `tools/dev/fhdiff.py` uses for frames.
 *
 * CONTENT-ADDRESSING is what keeps heap addresses out of the key.  A_LOADBUFF / A_LOADADPCM /
 * A_SETLOOP / A_FILTER name RDRAM addresses, and those are sample-cache slots and audio-heap
 * offsets that move between runs and move when `[debug] audio_stream` changes the byte source.
 * For those opcodes the key folds w0 plus a digest of the BYTES AT that address instead of the
 * address itself.  That is the property that makes `audio_stream=0` vs `audio_stream=1`
 * comparable at all -- and it is what turns `audiodiet`'s inference into a measurement.
 *
 * WHAT THIS DOES NOT COVER is stated in docs/research/native/ndsphash-progress.md §6 and is
 * not small: the SPLICE.  Two runs cut the stream differently, so this says nothing about
 * where a note landed in time.  It says the same render request produced the same bytes.
 *
 * EXECUTOR.  The value side lives inside the HLE interpreter (port/n64_audio_hle.c), which is
 * the 3DS default (`[audio] lle=0`).  Under `[audio] lle=1` the instrument prints
 * `executor=LLE unsupported` and produces no pairs rather than producing misleading ones.
 *
 * THREADING.  Every entry point runs on the audio producer thread, inside one
 * `gdx_audio_hle_run` call, which is single-threaded by construction.  The configure and
 * report entry points run at boot and at shutdown.  No lock is taken and nothing is allocated
 * on the per-task path.
 *
 * COST WHEN OFF.  One int compare per entry point; `gdx_ndsphash_enabled()` is the first line
 * of each.  No allocation, no log line, no behaviour change.
 */
#ifndef GDX_NDSP_HASH_H
#define GDX_NDSP_HASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- configuration -- */

/* mode: 0 = off, 1 = counters + FINAL receipt only, 2 = also stream the per-tick
 *       `[ndsphash] e` pair lines that tools/dev/ndspdiff.py consumes.
 * every: record one task in `every` (1 = all).  Bounds log volume on long sessions; both
 *       sides of a comparison must use the same value or the shared-key count collapses.
 * faultTask: 0 = no injection.  Otherwise the 1-based task index at which to flip ONE BIT of
 *       ONE rendered sample, AFTER the key is sealed -- the negative control that proves the
 *       gate fires.  See gdx_ndsphash_fault_render() below.
 * faultSource: 0 = no injection.  Otherwise the 1-based task index at which to flip one bit
 *       of the sample bytes a load delivers, BEFORE they are folded into the key -- the other
 *       fault class, which lands in only_in_a/only_in_b rather than in divergent.
 */
void gdx_ndsphash_configure(int mode, int every, int faultTask, int faultSource);

/* Resolve the levers from the INI on 3DS ([debug] ndsphash / ndsphash_every /
 * ndsphash_fault / ndsphash_fault_source) and print the one-shot arming receipt.  A no-op
 * off-3DS, where gdx_ndsphash_configure() is the only way in (that is what the host suite
 * uses). */
void gdx_ndsphash_init(void);

/* 1 while the instrument is armed.  The hot-path guard. */
int gdx_ndsphash_enabled(void);

/* ----------------------------------------------------- the audio determinism pin -- */

/* 1 while `[debug] ndsphash_pin` is set, which makes port/3ds/lus_glue/gdx3ds_audiobridge.c's
 * osAiGetLength() report a constant instead of the live 3DS ring occupancy.
 *
 * WHY THIS IS NEEDED, measured rather than assumed.  Without it, two identical runs of the
 * canonical replay share only 17.2% of their keys (1801 of ~10,500): they agree exactly for
 * the first 205 audio tasks -- the boot silence -- and then drift apart permanently the moment
 * real audio starts.  The mechanism is one line, decomp/src/audio/rom/lib/thread.c:46:
 *
 *      aiSamplesLeft = osAiGetLength() / 4;
 *      gAiBufLengths[i] = ((samplesPerFrameTarget - aiSamplesLeft + 0x80) & 0xFFF0) + 0x10;
 *
 * The AI buffer length is derived from how full the ring happens to be, the ring is drained by
 * a real-time thread on another core, so the per-tick frame count differs between runs.  Every
 * subsequent sample position and envelope step then differs, and the two command streams never
 * realign.  That is the SAME root cause as `audiodiet`'s 3452-vs-3439 reads and `audioasync`'s
 * 37,056-vs-37,648 sil_ppk; this is the first time it has been attributed to a line.
 *
 * Pinning it is a PARITY MODE, in the same sense `[debug] det=1` pins RNG seeds: it changes the
 * chunking of the output, so it is not the shipped configuration and a claim made under it must
 * say so.  It does NOT change the content pipeline -- the sequence player, the synthesiser, the
 * soundfont, the sample bytes and the DSP kernels are all untouched -- which is exactly why a
 * digest taken under the pin still gates them.  On 3DS osAiGetLength has ONE caller (that line);
 * the producer's own pacing loop reads gdx3ds_audio_buffered() directly and is unaffected.
 */
int gdx_ndsphash_ai_pin(void);

/* Set the pin.  Called from gdx_ndsphash_init() on 3DS; the test seam off it. */
void gdx_ndsphash_set_ai_pin(int on);

/* 1 while `[debug] ndsphash_lockstep` is set: the SECOND determinism pin, and the one that
 * actually closes the drift.
 *
 * ndsphash_pin alone raised two identical runs from 17.2% to 26.2% shared command lists --
 * real, and nowhere near a gate.  The residue is the other half of the same problem: the audio
 * producer is a free-running thread on a spare core, pacing itself against the ring's
 * occupancy, while the game thread is replay-driven.  How many audio ticks elapse between two
 * game events therefore varies, so a note-on lands at a different sample position, and every
 * later tick's command list inherits it.  audiodiet saw the consequence (3452 vs 3439 reads)
 * and called the audio thread's read multiset non-deterministic "even under det=1"; this is
 * why.
 *
 * Under the lockstep pin the tick is produced INLINE from gdx_audio_thread_notify_frame(), on
 * the game thread, once per frame, and the producer thread stops producing.  Audio tick N then
 * belongs to game frame N by construction, and the game's frames are already deterministic
 * under [debug] det=1 -- that is what `fhdiff divergent=0` means.
 *
 * It is a PARITY MODE and a loud one: production no longer tracks playback, so the ring
 * over- or under-runs and the audio you would HEAR is not the shipped experience.  What it
 * does not touch is the content pipeline -- same sequence player, same soundfont, same sample
 * bytes, same synthesiser, same DSP kernels -- which is the whole reason a digest taken under
 * it still gates them.  Never quote a timing, a sil_ppk or a level from a lockstep run.
 */
int gdx_ndsphash_lockstep(void);
void gdx_ndsphash_set_lockstep(int on);

/* ------------------------------------------------------------------- the per-task tap -- */

/* Start a task.  Seeds the key with the command list's own words, normalised: for the
 * address-bearing opcodes only w0 is folded here and the CONTENT arrives through
 * gdx_ndsphash_fold_load() below.  Returns 1 if this task is being recorded (sampling and
 * arming both satisfied), 0 otherwise -- the caller may skip its folds on 0. */
int gdx_ndsphash_task_begin(const void* cmds, unsigned int byteCount);

/* Fold the CONTENT of bytes an address-bearing load named, into the KEY.  Called from the
 * A_LOADBUFF / A_LOADADPCM / A_SETLOOP / A_FILTER handlers with the resolved host pointer. */
void gdx_ndsphash_fold_load(const void* src, unsigned int byteCount);

/* Fold the CONTENT of bytes the task wrote back to RDRAM, into the VALUE.  Called from the
 * A_SAVEBUFF handler.  This is the rendered PCM (and the reverb ring state, deliberately:
 * a reverb regression is an audio regression). */
void gdx_ndsphash_fold_save(const void* dst, unsigned int byteCount);

/* End the task: seal (key, value), fold them into the session digest, and -- at mode 2 --
 * stream the pair.  Also detects a WITHIN-run contradiction (the same key rendering two
 * different values in one session), reported as `selfdiv`, which needs no second run at all. */
void gdx_ndsphash_task_end(void);

/* ------------------------------------------------------------------ fault injection -- */

/* Return 1 if THIS task is the configured render-fault task and the flip has not yet been
 * spent.  The A_SAVEBUFF handler calls it once per save and, on 1, flips bit 0 of the first
 * byte it writes.  One bit, one sample, one 16 ms chunk in a session of ~10,000 -- the
 * smallest real change there is, and the gate is only worth the name if it catches it. */
int gdx_ndsphash_fault_render(void);

/* Same, for the source-fault class: flip one bit of a delivered sample byte before it is
 * folded into the key. */
int gdx_ndsphash_fault_source(void);

/* ------------------------------------------------------------------------ receipts -- */

/* The FINAL whole-session line.  Called from the audio shutdown path; idempotent. */
void gdx_ndsphash_report_final(void);

/* Test seam: drop all state so one host process can run several cases.  Not called by the
 * game. */
void gdx_ndsphash_reset_for_test(void);

/* Test seam: the session totals, so the host suite can assert on them without parsing a log.
 * Any pointer may be NULL. */
void gdx_ndsphash_stats_for_test(unsigned int* outTasks, unsigned int* outPairs,
                                 unsigned int* outSelfDiv, uint64_t* outFold);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NDSP_HASH_H */
