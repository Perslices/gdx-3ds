/* port/3ds/audio/gdx3ds_audio_crk.c -- [audiocrk] census, see the header. Pure C. */
#include "gdx3ds_audio_crk.h"

#include <stdio.h>
#include <string.h>

#define CRK_NONE 0xFFFFFFFFu

void gdx_crk_window_reset(GdxCrkWindow* w) {
    memset(w, 0, sizeof(*w));
    w->ringMin = CRK_NONE;
    w->bufMin = CRK_NONE;
}

void gdx_crk_init(GdxCrk* c, int32_t stepThreshold) {
    memset(c, 0, sizeof(*c));
    c->stepThreshold = (stepThreshold > 0) ? stepThreshold : GDX_CRK_STEP_DEFAULT;
    gdx_crk_window_reset(&c->win);
    gdx_crk_window_reset(&c->total);
    c->lastPlaying = 0;
}

static uint32_t AbsDelta(int16_t a, int16_t b) {
    int32_t d = (int32_t)a - (int32_t)b;
    return (uint32_t)((d < 0) ? -d : d);
}

static uint32_t AbsSample(int16_t v) {
    int32_t x = v;
    return (uint32_t)((x < 0) ? -x : x);
}

/* One boundary pair per channel: prev frame -> next frame. */
static void NoteBoundary(const int16_t prev[2], const int16_t* next, int32_t threshold,
                         uint32_t* steps, uint32_t* pairs, uint32_t* dmax, uint64_t* dsum) {
    for (uint32_t ch = 0; ch < 2u; ch++) {
        uint32_t d = AbsDelta(next[ch], prev[ch]);
        (*pairs)++;
        *dsum += d;
        if (d >= (uint32_t)threshold) {
            (*steps)++;
        }
        if (d > *dmax) {
            *dmax = d;
        }
    }
}

static void ScanContent(GdxCrk* c, const int16_t* pcm, uint32_t real) {
    GdxCrkWindow* w = &c->win;
    const uint32_t n = real * 2u;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t a = AbsSample(pcm[k]);
        if (a >= (uint32_t)GDX_CRK_RAIL) {
            w->rail++;
        }
        if (a >= (uint32_t)GDX_CRK_HOT) {
            w->hot++;
        }
        if (a > w->peak) {
            w->peak = a;
        }
        if (k >= 2u) {
            uint32_t d = AbsDelta(pcm[k], pcm[k - 2u]);
            w->pairsIn++;
            w->dsumIn += d;
            if (d >= (uint32_t)c->stepThreshold) {
                w->stepIn++;
            }
            if (d > w->dmaxIn) {
                w->dmaxIn = d;
            }
        }
    }
}

void gdx_crk_note_chunk(GdxCrk* c, const int16_t* pcm, uint32_t real, uint32_t chunkFrames,
                        uint32_t ringBefore) {
    GdxCrkWindow* w = &c->win;
    w->chunks++;
    if (ringBefore < w->ringMin) {
        w->ringMin = ringBefore;
    }
    if (real < chunkFrames) {
        w->padFrames += chunkFrames - real;
        if (real == 0u) {
            w->dry++;
        } else {
            w->partial++;
        }
    }
    if (real == 0u) {
        c->haveLastChunk = 0;
        return;
    }
    if (c->haveLastChunk) {
        NoteBoundary(c->lastChunk, pcm, c->stepThreshold, &w->stepChunk, &w->pairsChunk,
                     &w->dmaxChunk, &w->dsumChunk);
    }
    ScanContent(c, pcm, real);
    /* A padded chunk ends in silence: the next chunk's first frame follows padding, not
     * audio, so it is not a real->real boundary. */
    if (real == chunkFrames) {
        c->lastChunk[0] = pcm[(real - 1u) * 2u];
        c->lastChunk[1] = pcm[(real - 1u) * 2u + 1u];
        c->haveLastChunk = 1;
    } else {
        c->haveLastChunk = 0;
    }
}

void gdx_crk_note_poll(GdxCrk* c, uint32_t queued, uint32_t intervalUs, int playing) {
    GdxCrkWindow* w = &c->win;
    w->polls++;
    if (intervalUs != 0u) {
        if (intervalUs > w->pollMaxUs) {
            w->pollMaxUs = intervalUs;
        }
        if (intervalUs >= GDX_CRK_POLL_LATE_US) {
            w->pollLate++;
        }
    }
    if (queued == 0u) {
        w->dspDry++;
    }
    if (c->lastPlaying && !playing) {
        w->stops++;
    }
    c->lastPlaying = playing ? 1 : 0;
}

void gdx_crk_note_drop_total(GdxCrk* c, uint32_t dropTotal) {
    if (c->haveDropTotal && dropTotal >= c->lastDropTotal) {
        c->win.ndrop += dropTotal - c->lastDropTotal;
    }
    c->lastDropTotal = dropTotal;
    c->haveDropTotal = 1;
}

void gdx_crk_note_ring_drops(GdxCrk* c, uint32_t dropTotal) {
    if (c->haveRingDrops && dropTotal >= c->lastRingDrops) {
        c->win.dropFrames += dropTotal - c->lastRingDrops;
    }
    c->lastRingDrops = dropTotal;
    c->haveRingDrops = 1;
}

int gdx_crk_note_push(GdxCrk* c, const int16_t* samples, uint32_t frames) {
    GdxCrkWindow* w = &c->win;
    int stepped = 0;
    if (frames == 0u || samples == NULL) {
        return 0;
    }
    if (c->havePush) {
        const uint32_t before = w->stepPush;
        NoteBoundary(c->pushTail[GDX_CRK_EV_SIDE - 1], samples, c->stepThreshold, &w->stepPush,
                     &w->pairsPush, &w->dmaxPush, &w->dsumPush);
        if (w->stepPush != before) {
            stepped = 1;
            for (uint32_t f = 0; f < (uint32_t)GDX_CRK_EV_SIDE; f++) {
                for (uint32_t ch = 0; ch < 2u; ch++) {
                    c->evPush[f][ch] = c->pushTail[f][ch];
                    c->evPush[GDX_CRK_EV_SIDE + f][ch] = (f < frames) ? samples[f * 2u + ch] : 0;
                }
            }
        }
    }
    for (uint32_t f = 0; f < (uint32_t)GDX_CRK_EV_SIDE; f++) {
        /* tail slot f holds frame (frames - EV_SIDE + f); missing frames read 0 */
        const int32_t src = (int32_t)frames - GDX_CRK_EV_SIDE + (int32_t)f;
        for (uint32_t ch = 0; ch < 2u; ch++) {
            c->pushTail[f][ch] = (src >= 0) ? samples[(uint32_t)src * 2u + ch] : 0;
        }
    }
    c->havePush = 1;
    return stepped;
}

void gdx_crk_note_tick(GdxCrk* c, uint32_t us) {
    c->win.ticks++;
    if (us > c->win.tickMaxUs) {
        c->win.tickMaxUs = us;
    }
}

void gdx_crk_note_wake(GdxCrk* c, uint32_t buffered) {
    if (buffered < c->win.bufMin) {
        c->win.bufMin = buffered;
    }
}

void gdx_crk_break_chain(GdxCrk* c) {
    c->haveLastChunk = 0;
    c->havePush = 0;
    c->lastPlaying = 0;
}

int gdx_crk_window_bad(const GdxCrkWindow* w) {
    return (w->partial | w->dry | w->dropFrames | w->dspDry | w->stops | w->ndrop | w->pollLate | w->rail |
            w->stepChunk | w->stepPush) != 0u;
}

static void FoldMax(uint32_t* dst, uint32_t v) {
    if (v > *dst) {
        *dst = v;
    }
}

static void FoldMin(uint32_t* dst, uint32_t v) {
    if (v < *dst) {
        *dst = v;
    }
}

void gdx_crk_fold(GdxCrkWindow* t, const GdxCrkWindow* w) {
    t->chunks += w->chunks;
    t->partial += w->partial;
    t->dry += w->dry;
    t->padFrames += w->padFrames;
    t->dropFrames += w->dropFrames;
    FoldMin(&t->ringMin, w->ringMin);
    t->polls += w->polls;
    t->pollLate += w->pollLate;
    FoldMax(&t->pollMaxUs, w->pollMaxUs);
    t->dspDry += w->dspDry;
    t->stops += w->stops;
    t->ndrop += w->ndrop;
    t->rail += w->rail;
    t->hot += w->hot;
    FoldMax(&t->peak, w->peak);
    t->stepIn += w->stepIn;
    t->pairsIn += w->pairsIn;
    FoldMax(&t->dmaxIn, w->dmaxIn);
    t->stepChunk += w->stepChunk;
    t->pairsChunk += w->pairsChunk;
    FoldMax(&t->dmaxChunk, w->dmaxChunk);
    t->stepPush += w->stepPush;
    t->pairsPush += w->pairsPush;
    FoldMax(&t->dmaxPush, w->dmaxPush);
    t->dsumIn += w->dsumIn;
    t->dsumChunk += w->dsumChunk;
    t->dsumPush += w->dsumPush;
    t->ticks += w->ticks;
    FoldMax(&t->tickMaxUs, w->tickMaxUs);
    FoldMin(&t->bufMin, w->bufMin);
}

static long NoneAsMinus1(uint32_t v) {
    return (v == CRK_NONE) ? -1L : (long)v;
}

static int Clamp(int n, size_t cap) {
    if (n < 0) {
        return 0;
    }
    if ((size_t)n >= cap) {
        return (int)cap - 1;
    }
    return n;
}

int gdx_crk_format_sched(const GdxCrkWindow* w, const char* tag, unsigned long tMs, char* out,
                         size_t cap) {
    int n = snprintf(out, cap,
                     "[audiocrk] %s t=%lums chunks=%lu under=%lu dry=%lu pad=%lu drop=%lu "
                     "rmin=%ld | polls=%lu late=%lu pmax=%luus dspdry=%lu stop=%lu ndrop=%lu",
                     tag, tMs, (unsigned long)w->chunks, (unsigned long)w->partial,
                     (unsigned long)w->dry, (unsigned long)w->padFrames,
                     (unsigned long)w->dropFrames, NoneAsMinus1(w->ringMin),
                     (unsigned long)w->polls, (unsigned long)w->pollLate,
                     (unsigned long)w->pollMaxUs, (unsigned long)w->dspDry,
                     (unsigned long)w->stops, (unsigned long)w->ndrop);
    return Clamp(n, cap);
}

int gdx_crk_format_content(const GdxCrkWindow* w, const char* tag, char* out, size_t cap) {
    int n = snprintf(out, cap,
                     "[audiocrk] %s pcm rail=%lu hot=%lu peak=%lu step=%lu/%lu dmax=%lu "
                     "cstep=%lu/%lu cdmax=%lu pstep=%lu/%lu pdmax=%lu | ticks=%lu tmax=%luus "
                     "bufmin=%ld",
                     tag, (unsigned long)w->rail, (unsigned long)w->hot, (unsigned long)w->peak,
                     (unsigned long)w->stepIn, (unsigned long)w->pairsIn,
                     (unsigned long)w->dmaxIn, (unsigned long)w->stepChunk,
                     (unsigned long)w->pairsChunk, (unsigned long)w->dmaxChunk,
                     (unsigned long)w->stepPush, (unsigned long)w->pairsPush,
                     (unsigned long)w->dmaxPush, (unsigned long)w->ticks,
                     (unsigned long)w->tickMaxUs, NoneAsMinus1(w->bufMin));
    return Clamp(n, cap);
}

static unsigned long Mean100(uint64_t sum, uint32_t pairs) {
    return (pairs != 0u) ? (unsigned long)((sum * 100u) / pairs) : 0ul;
}

int gdx_crk_format_mean(const GdxCrkWindow* w, const char* tag, char* out, size_t cap) {
    int n = snprintf(out, cap, "[audiocrk] %s dmean100 in=%lu c=%lu p=%lu",
                     tag, Mean100(w->dsumIn, w->pairsIn), Mean100(w->dsumChunk, w->pairsChunk),
                     Mean100(w->dsumPush, w->pairsPush));
    return Clamp(n, cap);
}
