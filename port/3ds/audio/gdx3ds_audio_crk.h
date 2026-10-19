/* port/3ds/audio/gdx3ds_audio_crk.h -- [audiocrk]: crackle, split by cause (o3dscrackle).
 *
 * WHY. The Old 3DS crackles "like a B" at 60 fps, and every audio receipt the card carried
 * ([audioprime] under edges, the [core1] watch) says the ring is healthy: one partial chunk
 * per ~2 minutes. Five different mechanisms all sound like crackle and the shipped receipts
 * cannot tell them apart:
 *   1. ring underrun      -- the producer missed its deadline: partial/dry chunk, silence
 *                            spliced in (under/dry/pad, rmin = the slack that was left);
 *   2. DSP-queue underrun -- the ring was fine but the DRAIN missed its 16 ms refill, so
 *                            the ndsp channel ran out of wave buffers (dspdry, stop, late);
 *   3. ndsp frame drops   -- libctru's ndsp thread missed a DSP frame IRQ (ndrop);
 *   4. clipping           -- the delivered PCM touches the rails, or sits in the band a
 *                            nonlinear DSP stage would act on (rail, hot, peak);
 *   5. splice/boundary    -- a step discontinuity exactly at a 512-frame wave-buffer
 *                            boundary or at a producer push boundary (cstep, pstep), which
 *                            the per-pair interior rate (step) is the control for.
 * Each window line separates them. Pure C, no libctru: the drain/producer feed it, the
 * host test (port/tests/audiocrk_tests.c) drives it directly.
 *
 * THREADS. Drain-side fields are written by the drain only; producer-side fields by the
 * producer only. The window rollover (fold + reset) runs on the drain, so a producer-side
 * max/min can lose one sample across a reset -- the port's benign aligned-int diag race. */
#ifndef GDX3DS_AUDIO_CRK_H
#define GDX3DS_AUDIO_CRK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CRK_RAIL 32767
#define GDX_CRK_HOT 24576 /* 0.75 full scale: where a soft clipper would plausibly act */
#define GDX_CRK_STEP_DEFAULT 8192
#define GDX_CRK_POLL_LATE_US 16000u /* one 512-frame chunk at 32 kHz */
#define GDX_CRK_EV_SIDE 4           /* frames kept each side of a push-boundary step */

typedef struct {
    /* drain: ring side (cause 1) */
    uint32_t chunks;     /* chunks submitted (prime-held silence excluded) */
    uint32_t partial;    /* 0 < real < chunk */
    uint32_t dry;        /* real == 0 */
    uint32_t padFrames;  /* silence frames spliced in */
    uint32_t dropFrames; /* ring overrun drop-oldest frames (a splice too: cstep shows it) */
    uint32_t ringMin;    /* lowest ring level a pull saw (frames); UINT32_MAX = none */
    /* drain: DSP hop (causes 2, 3) */
    uint32_t polls;
    uint32_t pollLate;   /* poll-to-poll >= one chunk period */
    uint32_t pollMaxUs;
    uint32_t dspDry;     /* polls that found zero wave buffers queued/playing */
    uint32_t stops;      /* channel playing -> not playing transitions */
    uint32_t ndrop;      /* ndspGetDroppedFrames delta */
    /* drain: content (causes 4, 5) */
    uint32_t rail;       /* samples at +32767 / -32768 */
    uint32_t hot;        /* samples with |x| >= GDX_CRK_HOT */
    uint32_t peak;       /* max |x| */
    uint32_t stepIn;     /* interior adjacent-sample |delta| >= threshold, per channel */
    uint32_t pairsIn;
    uint32_t dmaxIn;
    uint32_t stepChunk;  /* same test across a wave-buffer boundary (real -> real only) */
    uint32_t pairsChunk;
    uint32_t dmaxChunk;
    /* producer */
    uint32_t stepPush;   /* same test across a producer push boundary */
    uint32_t pairsPush;
    uint32_t dmaxPush;
    /* sum of |delta| per class: dsum/pairs = mean step, which sees EVERY boundary, not only
     * the ones above the threshold (a small consistent discontinuity raises the mean) */
    uint64_t dsumIn;
    uint64_t dsumChunk;
    uint64_t dsumPush;
    uint32_t ticks;
    uint32_t tickMaxUs;
    uint32_t bufMin;     /* lowest buffered() at a producer wake; UINT32_MAX = none */
} GdxCrkWindow;

typedef struct {
    int32_t stepThreshold;
    GdxCrkWindow win;
    GdxCrkWindow total;
    int16_t lastChunk[2];
    int haveLastChunk;   /* the previous chunk ENDED on a real frame */
    int16_t pushTail[GDX_CRK_EV_SIDE][2]; /* last frames of the previous push, oldest first */
    int havePush;
    /* the most recent push-boundary step, for a detail receipt: frames -4..-1 | 0..+3
     * (frames a push did not have read 0) */
    int16_t evPush[2 * GDX_CRK_EV_SIDE][2];
    uint32_t lastDropTotal;
    int haveDropTotal;
    uint32_t lastRingDrops;
    int haveRingDrops;
    int lastPlaying;
} GdxCrk;

void gdx_crk_init(GdxCrk* c, int32_t stepThreshold);
void gdx_crk_window_reset(GdxCrkWindow* w);

/* Drain: one submitted chunk. `pcm` = interleaved stereo, `real` real frames followed by
 * padding; `ringBefore` = the ring level the pull saw. A pure-padding chunk breaks the
 * boundary chain (the next real frame is not compared against stale audio). */
void gdx_crk_note_chunk(GdxCrk* c, const int16_t* pcm, uint32_t real, uint32_t chunkFrames,
                        uint32_t ringBefore);
/* Drain: top of a poll. `intervalUs` 0 = suppressed (first poll / after a park). */
void gdx_crk_note_poll(GdxCrk* c, uint32_t queued, uint32_t intervalUs, int playing);
/* Drain: libctru's cumulative dropped-frame counter. */
void gdx_crk_note_drop_total(GdxCrk* c, uint32_t dropTotal);
/* Producer: one push (interleaved stereo). Only the boundary pair is examined. Returns 1
 * when that pair was a step (c->evPush then holds the four frames around it). */
int gdx_crk_note_push(GdxCrk* c, const int16_t* samples, uint32_t frames);
/* Drain: the ring's cumulative drop-oldest counter. */
void gdx_crk_note_ring_drops(GdxCrk* c, uint32_t dropTotal);
void gdx_crk_note_tick(GdxCrk* c, uint32_t us);
void gdx_crk_note_wake(GdxCrk* c, uint32_t buffered);
/* HOME/sleep resume: the stream restarts from silence, so no boundary is meaningful. */
void gdx_crk_break_chain(GdxCrk* c);

/* Anything in the window that is audible-suspect (causes 1-5, interior steps excluded). */
int gdx_crk_window_bad(const GdxCrkWindow* w);
/* total += win (counters add, max/min fold). */
void gdx_crk_fold(GdxCrkWindow* total, const GdxCrkWindow* win);

/* Two receipt lines (each < 256 bytes): `[audiocrk] <tag> t=..ms` cause 1-3, then content.
 * Returns the length written into each buffer. */
int gdx_crk_format_sched(const GdxCrkWindow* w, const char* tag, unsigned long tMs, char* out,
                         size_t cap);
int gdx_crk_format_content(const GdxCrkWindow* w, const char* tag, char* out, size_t cap);
/* Third line: mean |delta| x100 per class (interior / chunk boundary / push boundary). */
int gdx_crk_format_mean(const GdxCrkWindow* w, const char* tag, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
