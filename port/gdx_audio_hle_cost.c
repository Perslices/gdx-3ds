/* gdx_audio_hle_cost — see the header for what is measured and why. */
#include "gdx_audio_hle_cost.h"

#include <stdio.h>
#include <string.h>

/* Half-octave buckets over rate = ticks*16/units. 48 buckets reach 2^24/16 = 1M ticks per unit
 * (~3.9 ms per sample at 268 MHz); anything slower is clamped into the top bucket. */
#define HLECOST_BUCKETS 48
/* An op more than two octaves (4 half-octave buckets = 4x) above its class median is PREEMPTED. */
#define HLECOST_PREEMPT_BUCKETS 4

typedef struct HleCostClassHist {
    uint64_t ticks[HLECOST_BUCKETS];
    uint32_t units[HLECOST_BUCKETS];
    uint32_t count[HLECOST_BUCKETS];
} HleCostClassHist;

typedef struct HleCostWindow {
    HleCostClassHist cls[GDX_HLECOST_COUNT];
    uint32_t runs;
    uint32_t voices;
    uint64_t outFrames;
    uint64_t maxOpTicks;
    /* n60-3d12 [audiocost]: the single costliest op PER CLASS this window (maxOpTicks above is
     * the global max across every class, already used by [synx]'s maxop= column). */
    uint64_t maxTicks[GDX_HLECOST_COUNT];
    uint64_t startTick;     /* first stamp after the last publish */
    uint64_t spanTicks;     /* wall span of the window, set at publish */
    unsigned long startFrame;
    unsigned long frames;   /* main-thread frames the window spanned, set at publish */
    /* The core probe, taken on the producer thread at publish (see ProbeCore). */
    uint64_t probeRunTicks;
    uint64_t probeGapTicks;
    uint64_t probeMaxGap;
    uint32_t probeGaps;
} HleCostWindow;

int gdx_hle_cost_armed = 0;

static long long (*sNow)(void) = NULL;
static double sTickHz = 1.0;
static HleCostWindow sAcc; /* producer-owned */
static HleCostWindow sPub; /* written by the producer only while sPublishReq is set */
static volatile int sPublishReq = 0;
static volatile int sPubValid = 0;
static volatile unsigned long sReqFrame = 0;
static uint64_t sStamp = 0;
static int sAccStarted = 0;

static uint64_t NowTicks(void) {
    return (sNow != NULL) ? (uint64_t)sNow() : 0u;
}

static int HighestBit64(uint64_t v) {
    int b = 0;
    while (v >>= 1) {
        b++;
    }
    return b;
}

static int RateBucket(uint32_t units, uint64_t ticks) {
    uint64_t rate;
    int msb;
    int bucket;
    if (units == 0u) {
        units = 1u;
    }
    rate = (ticks * 16u) / units;
    if (rate == 0u) {
        return 0;
    }
    msb = HighestBit64(rate);
    bucket = msb * 2;
    if (msb >= 1 && ((rate >> (msb - 1)) & 1u) != 0u) {
        bucket++;
    }
    return (bucket < HLECOST_BUCKETS) ? bucket : HLECOST_BUCKETS - 1;
}

void gdx_hle_cost_record(int cls, uint32_t units, uint64_t ticks) {
    HleCostClassHist* h;
    int b;
    if (cls < 0 || cls >= GDX_HLECOST_COUNT) {
        cls = GDX_HLECOST_OTHER;
    }
    h = &sAcc.cls[cls];
    b = RateBucket(units, ticks);
    h->ticks[b] += ticks;
    h->units[b] += (units != 0u) ? units : 1u;
    h->count[b]++;
    if (ticks > sAcc.maxOpTicks) {
        sAcc.maxOpTicks = ticks;
    }
    if (ticks > sAcc.maxTicks[cls]) {
        sAcc.maxTicks[cls] = ticks;
    }
}

/* THE CORE PROBE. Once per window the producer thread spins on the tick for kProbeTicks and
 * books every step longer than kGapTicks as time it was NOT running (one step is a single
 * svcGetSystemTick, ~0.6 us on hardware; a preemption is tens of microseconds or more). The run
 * share is what this core actually granted the producer at that moment, measured, and the gap
 * count/length is the scheduler's slice structure (the Old 3DS syscore's 30% app limit, the
 * system modules). It is independent of the per-op estimate, so the receipt carries both:
 * `cpu` (per-op median) and `cpuP` (synth wall x probe run share). Cost: 16 ms of wall per
 * window (one [othx] window, ~6 s at 11 fps), excluded from every op's timing. With
 * audio_ctx_narrow=0 it runs under sAudioCtxMutex, so [othx] actx can show it once a window. */
#define HLECOST_PROBE_MS 16.0
#define HLECOST_PROBE_GAP_US 25.0

static void ProbeCore(HleCostWindow* w) {
    const uint64_t probeTicks = (uint64_t)(sTickHz * HLECOST_PROBE_MS / 1000.0);
    const uint64_t gapTicks = (uint64_t)(sTickHz * HLECOST_PROBE_GAP_US / 1.0e6);
    const uint64_t t0 = NowTicks();
    uint64_t prev = t0;
    w->probeRunTicks = 0;
    w->probeGapTicks = 0;
    w->probeMaxGap = 0;
    w->probeGaps = 0;
    if (sNow == NULL) {
        return;
    }
    while (prev - t0 < probeTicks) {
        const uint64_t t = NowTicks();
        const uint64_t d = t - prev;
        if (d > gapTicks) {
            w->probeGapTicks += d;
            w->probeGaps++;
            if (d > w->probeMaxGap) {
                w->probeMaxGap = d;
            }
        } else {
            w->probeRunTicks += d;
        }
        prev = t;
    }
}

static void PublishWindow(uint64_t now) {
    const unsigned long reqFrame = sReqFrame;
    sAcc.spanTicks = sAccStarted ? (now - sAcc.startTick) : 0u;
    sAcc.frames = reqFrame - sAcc.startFrame;
    ProbeCore(&sAcc);
    memcpy(&sPub, &sAcc, sizeof(sPub));
    memset(&sAcc, 0, sizeof(sAcc));
    sAcc.startTick = now;
    sAcc.startFrame = reqFrame;
    sAccStarted = 1;
    __sync_synchronize();
    sPubValid = 1;
    sPublishReq = 0;
}

void gdx_hle_cost_arm(int on, long long (*now)(void), double tickHz) {
    sNow = now;
    sTickHz = (tickHz > 0.0) ? tickHz : 1.0;
    gdx_hle_cost_armed = (on && now != NULL) ? 1 : 0;
}

void gdx_hle_cost_run_begin(void) {
    const uint64_t now = NowTicks();
    if (!sAccStarted) {
        sAcc.startTick = now;
        sAcc.startFrame = sReqFrame;
        sAccStarted = 1;
    }
    if (sPublishReq) {
        PublishWindow(now);
        sStamp = NowTicks(); /* the probe's spin is not the first op's time */
    } else {
        sStamp = now;
    }
    sAcc.runs++;
}

void gdx_hle_cost_op_end(int cls, uint32_t units) {
    const uint64_t now = NowTicks();
    gdx_hle_cost_record(cls, units, now - sStamp);
    sStamp = now;
}

void gdx_hle_cost_note_output(uint32_t frames) {
    sAcc.outFrames += frames;
}

void gdx_hle_cost_note_voice(void) {
    sAcc.voices++;
}

void gdx_hle_cost_run_end(void) {
    /* Nothing to close: the last op_end already charged the tail of the run. Kept as the
     * symmetric hook so a later per-run statistic has one place to live. */
}

void gdx_hle_cost_request_publish(unsigned long frameNo) {
    sReqFrame = frameNo;
    __sync_synchronize();
    sPublishReq = 1;
}

void gdx_hle_cost_publish_now(void) {
    PublishWindow(NowTicks());
}

/* The units-weighted median bucket of one class, or -1 when the class never ran. */
static int MedianBucket(const HleCostClassHist* h) {
    uint64_t total = 0;
    uint64_t seen = 0;
    int b;
    for (b = 0; b < HLECOST_BUCKETS; b++) {
        total += h->units[b];
    }
    if (total == 0u) {
        return -1;
    }
    for (b = 0; b < HLECOST_BUCKETS; b++) {
        seen += h->units[b];
        if (seen * 2u >= total) {
            return b;
        }
    }
    return HLECOST_BUCKETS - 1;
}

/* `maxTicks` is the class's single costliest op this window (n60-3d12 [audiocost]'s own
 * stat, HleCostWindow.maxTicks[cls] -- separate from each bucket's wall/units/count, which
 * are already keyed by RATE, not by single-op duration). `opCount` (the sum of every
 * bucket's `count`) is likewise new: FormatSummary/FormatProbe/FormatOps never needed a raw
 * op count before, only [audiocost]'s avg-us-per-op column does. */
static GdxHleCostClassTotals ClassTotals(const HleCostClassHist* h, uint64_t maxTicks) {
    GdxHleCostClassTotals t;
    const int m = MedianBucket(h);
    double medianRate;
    int b;
    memset(&t, 0, sizeof(t));
    t.maxOpTicks = (double)maxTicks;
    if (m < 0) {
        return t;
    }
    medianRate = (h->units[m] != 0u) ? (double)h->ticks[m] / (double)h->units[m] : 0.0;
    t.medianTicksPerUnit = medianRate;
    for (b = 0; b < HLECOST_BUCKETS; b++) {
        t.wallTicks += (double)h->ticks[b];
        t.units += (double)h->units[b];
        t.opCount += (double)h->count[b];
        if (b <= m + HLECOST_PREEMPT_BUCKETS) {
            t.cpuTicks += (double)h->ticks[b];
        } else {
            t.cpuTicks += (double)h->units[b] * medianRate;
            t.gaps += (double)h->count[b];
        }
    }
    return t;
}

GdxHleCostClassTotals gdx_hle_cost_published_class(int cls) {
    GdxHleCostClassTotals none;
    if (cls < 0 || cls >= GDX_HLECOST_COUNT || !sPubValid) {
        memset(&none, 0, sizeof(none));
        return none;
    }
    return ClassTotals(&sPub.cls[cls], sPub.maxTicks[cls]);
}

static double TicksToMs(double ticks) {
    return ticks * 1000.0 / sTickHz;
}

static const char* const kClassNames[GDX_HLECOST_COUNT] = {
    "adpcm", "resample", "envmix", "mixer", "addmix", "interleave", "filter",
    "hilogain", "loadbuff", "savebuff", "clearbuff", "dmemmove", "other"
};

static int FormatSummary(char* out, size_t cap, unsigned long frameNo, int simd,
                         unsigned outputRateHz) {
    GdxHleCostClassTotals sum;
    const double frames = (sPub.frames != 0u) ? (double)sPub.frames : 1.0;
    const double runs = (sPub.runs != 0u) ? (double)sPub.runs : 1.0;
    const double audioSec = (outputRateHz != 0u) ? (double)sPub.outFrames / (double)outputRateHz : 0.0;
    int c;
    memset(&sum, 0, sizeof(sum));
    for (c = 0; c < GDX_HLECOST_COUNT; c++) {
        const GdxHleCostClassTotals t = ClassTotals(&sPub.cls[c], sPub.maxTicks[c]);
        sum.wallTicks += t.wallTicks;
        sum.cpuTicks += t.cpuTicks;
        sum.gaps += t.gaps;
    }
    {
        const double wallMs = TicksToMs(sum.wallTicks);
        const double cpuMs = TicksToMs(sum.cpuTicks);
        const double spanMs = TicksToMs((double)sPub.spanTicks);
        const double needMs = (audioSec > 0.0) ? cpuMs / audioSec : 0.0;
        const double wneedMs = (audioSec > 0.0) ? wallMs / audioSec : 0.0;
        return snprintf(out, cap,
                        "[synx] frame=%lu f=%lu simd=%d runs=%.2f/f out=%.0f/f vox=%.1f/run | "
                        "wall=%.2f cpu=%.2f pre=%.2f gaps=%.1f/f maxop=%.2fms | duty=%.1f%% "
                        "need=%.0f wneed=%.0f share=%.1f%%",
                        frameNo, sPub.frames, simd, (double)sPub.runs / frames,
                        (double)sPub.outFrames / frames, (double)sPub.voices / runs,
                        wallMs / frames, cpuMs / frames, (wallMs - cpuMs) / frames,
                        sum.gaps / frames, TicksToMs((double)sPub.maxOpTicks),
                        (spanMs > 0.0) ? 100.0 * wallMs / spanMs : 0.0, needMs, wneedMs,
                        needMs / 10.0);
    }
}

/* The core probe line: run share, the synth CPU it implies, and the slice structure. */
static int FormatProbe(char* out, size_t cap) {
    GdxHleCostClassTotals sum;
    const double frames = (sPub.frames != 0u) ? (double)sPub.frames : 1.0;
    const double probeTotal = (double)(sPub.probeRunTicks + sPub.probeGapTicks);
    const double runShare = (probeTotal > 0.0) ? (double)sPub.probeRunTicks / probeTotal : 0.0;
    int c;
    memset(&sum, 0, sizeof(sum));
    for (c = 0; c < GDX_HLECOST_COUNT; c++) {
        sum.wallTicks += ClassTotals(&sPub.cls[c], sPub.maxTicks[c]).wallTicks;
    }
    {
        const double gaps = (double)sPub.probeGaps;
        const double runs = gaps + 1.0;
        return snprintf(out, cap,
                        "[synx] core run=%.1f%% cpuP=%.2f/f | gaps=%u gapavg=%.3fms gapmax=%.3fms "
                        "runavg=%.3fms over %.1fms",
                        100.0 * runShare, TicksToMs(sum.wallTicks) * runShare / frames,
                        (unsigned)sPub.probeGaps,
                        (gaps > 0.0) ? TicksToMs((double)sPub.probeGapTicks) / gaps : 0.0,
                        TicksToMs((double)sPub.probeMaxGap),
                        TicksToMs((double)sPub.probeRunTicks) / runs, TicksToMs(probeTotal));
    }
}

/* Five classes per line keeps each line under GpuProfLog's 224-byte buffer. Per class:
 * wall/cpu ms per frame, units per frame, median ns per unit. */
static int FormatOps(int line, char* out, size_t cap) {
    const int first = line * 5;
    const double frames = (sPub.frames != 0u) ? (double)sPub.frames : 1.0;
    int len;
    int c;
    if (first >= GDX_HLECOST_COUNT) {
        return 0;
    }
    len = snprintf(out, cap, "[synop%d]", line + 1);
    for (c = first; c < first + 5 && c < GDX_HLECOST_COUNT && len > 0 && (size_t)len < cap; c++) {
        const GdxHleCostClassTotals t = ClassTotals(&sPub.cls[c], sPub.maxTicks[c]);
        len += snprintf(out + len, cap - (size_t)len, " %s=%.2f/%.2f/%.0f/%.0f", kClassNames[c],
                        TicksToMs(t.wallTicks) / frames, TicksToMs(t.cpuTicks) / frames,
                        t.units / frames, t.medianTicksPerUnit * 1.0e9 / sTickHz);
    }
    return len;
}

int gdx_hle_cost_format(int index, char* out, size_t cap, unsigned long frameNo, int simd,
                        unsigned outputRateHz) {
    if (!sPubValid || out == NULL || cap == 0u) {
        return 0;
    }
    if (index == 0) {
        return FormatSummary(out, cap, frameNo, simd, outputRateHz);
    }
    if (index == 1) {
        return FormatProbe(out, cap);
    }
    return FormatOps(index - 2, out, cap);
}

static double TicksToUs(double ticks) {
    return ticks * 1.0e6 / sTickHz;
}

/* n60-3d12 [audiocost]: per-op-class AVG and MAX cost in microseconds, plus the raw op
 * count -- the hardware-run request behind this (a stuttering producer under SFX load,
 * core 1 at 10-40 ms/tick) asked "which op to optimise next", which [synx]'s per-FRAME
 * ms/frame columns answer only indirectly. Five classes per line, same 224-byte budget as
 * FormatOps above; independent tag so a reader can grep [audiocost] without the
 * gputrace-only [synx]/[synop] lines. */
static int FormatAudioCost(int line, char* out, size_t cap, double windowSeconds) {
    const int first = line * 5;
    int len;
    int c;
    if (first >= GDX_HLECOST_COUNT) {
        return 0;
    }
    len = snprintf(out, cap, "[audiocost] l=%d win=%.1fs", line, windowSeconds);
    for (c = first; c < first + 5 && c < GDX_HLECOST_COUNT && len > 0 && (size_t)len < cap; c++) {
        const GdxHleCostClassTotals t = ClassTotals(&sPub.cls[c], sPub.maxTicks[c]);
        const double avgUs = (t.opCount > 0.0) ? TicksToUs(t.wallTicks) / t.opCount : 0.0;
        const double maxUs = TicksToUs(t.maxOpTicks);
        len += snprintf(out + len, cap - (size_t)len, " %s=%.1f/%.1f/%.0f", kClassNames[c], avgUs,
                        maxUs, t.opCount);
    }
    return len;
}

/* `windowSeconds` is the caller's own elapsed-time figure (e.g. ms-since-last-report / 1000);
 * gdx_hle_cost_published_span_seconds below gives the ledger's own wall span if the caller
 * would rather print that instead (they are close but not identical -- the ledger's span
 * starts at the PREVIOUS publish, the caller's own clock at whenever it last checked). */
int gdx_hle_cost_format_audiocost(int index, char* out, size_t cap, double windowSeconds) {
    if (!sPubValid || out == NULL || cap == 0u) {
        return 0;
    }
    return FormatAudioCost(index, out, cap, windowSeconds);
}

double gdx_hle_cost_published_span_seconds(void) {
    return sPubValid ? (double)sPub.spanTicks / sTickHz : 0.0;
}
