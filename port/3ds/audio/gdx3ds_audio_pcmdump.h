/* port/3ds/audio/gdx3ds_audio_pcmdump.h -- AUDIOFIX round 3 (original)/round 6 (RAM-buffered
 * redesign): a hardware PCM capture lever.
 *
 * WHY. Every [audiocrk]/[audioprime] receipt is a STATISTIC (counts, peaks, step rates) --
 * useful for ruling mechanisms in or out, useless for actually listening to what reached the
 * DSP. This module settles which SIDE of a boundary a click is on by recording the ACTUAL
 * bytes handed to ndsp, not a summary of them.
 *
 * WHAT IT CAPTURES. The exact post-ring, post-padding, post-testtone interleaved s16 stereo
 * content FillAndSubmitSlot hands to DSP_FlushDataCache/ndspChnWaveBufAdd -- i.e. the content
 * whose audible defects, if any, are downstream of EVERYTHING this port's own code can still
 * change (ring, drain, wave-buffer chunking, underrun padding).
 *
 * HOW IT STAYS OFF THE AUDIO DEADLINE, AND OFF THE SD CARD DURING THE RACE (round 6). The
 * original (round 3-5) design streamed to SD incrementally through a small ring, which on real
 * hardware (card n60-3d9) dropped 1300-1900 chunks per 20 s run -- the SD-write thread simply
 * could not keep up with the SD card's real latency, more than half the capture silently gone.
 * Round 6 instead captures the WHOLE window into one preallocated RAM buffer
 * (gdx_pcmdump_submit is still a bounded LightLock+memcpy, now into that buffer, never SD) and
 * defers EVERY file write until the window ends: the race finishes, the player opens the HOME
 * menu (gdx_pcmdump_notify_suspend), or the app exits (gdx_pcmdump_shutdown). [debug]
 * pcm_dump_s=N (default 0 = inert, no thread, no allocation) requests N seconds of race-active
 * audio; gdx_pcmdump_init() checks the free heap and halves N (down to a 5 s floor, below
 * which it disables instead) if N seconds' buffer would not leave a safety margin free
 * afterward -- logged either way. Every blocking file op (the one-shot flush, once triggered)
 * runs on this module's own low-priority (0x3F, below the game/preload threads) thread that
 * owns no core grant, so it steals nothing from gameplay and a slow SD write delays only itself.
 *
 * FILES. sdmc:/3ds/gdiffuser/pcm-<variant>.wav (canonical 44-byte PCM16 header, patched with
 * the true frame count once the flush is done) and pcm-<variant>.csv (one line per chunk
 * written: frame offset into the WAV, the drain's chunk index, and its [audiocrk]-comparable
 * timestamp -- a chunk_index of 0xFFFFFFFF flags a synthesized-silence row, never a real
 * chunk), where <variant> is gdx3ds_launch_variant_name() ("main" when no launch variant fired).
 */
#ifndef GDX3DS_AUDIO_PCMDUMP_H
#define GDX3DS_AUDIO_PCMDUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads [debug] pcm_dump_s itself; a value <= 0 leaves the module completely inert (no thread,
 * no allocation, no file). Call once from gdx3ds_audio_init(), after the config is loaded. */
void gdx_pcmdump_init(void);

/* Drain thread only, once per submitted chunk, with the SAME buffer about to reach
 * DSP_FlushDataCache. frames == 0 is ignored. Never blocks past a LightLock/memcpy. */
void gdx_pcmdump_submit(const int16_t* pcm, uint32_t frames, uint32_t chunkIndex, uint32_t tMs);

/* Any thread. Round 6: the HOME-menu-pause flush trigger. Call from gdx3ds_audio_suspend()
 * (the APTHOOK_ONSUSPEND path) -- just raises a flag; the actual flush runs on this module's
 * own writer thread, so a HOME press is never delayed by it. Safe to call when inert. */
void gdx_pcmdump_notify_suspend(void);

/* Joins the writer thread (bounded -- round 6: 8 s, since this may be the FIRST trigger, i.e.
 * a full one-shot flush of the whole window) and flushes/closes the files if neither the
 * race-finished nor the HOME-pause trigger fired first. Safe to call when never armed. */
void gdx_pcmdump_shutdown(void);

/* Diagnostic export: frames captured so far / the target, and chunks dropped (ring-full, never
 * written) so a run's [audio-out] boot line and this module's own one-shot can report progress
 * without a second lock dance. Harmless when inert (both read as 0/0). */
void gdx_pcmdump_progress(uint32_t* capturedFrames, uint32_t* targetFrames, uint32_t* dropped);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_AUDIO_PCMDUMP_H */
