/* gdx_audio_mix_census — implementation.  See gdx_audio_mix_census.h for what is measured
 * and why the MIX is the one audio stage no existing instrument can see.
 *
 * Plain C99, no decomp headers, no platform headers: port/tests/dsp_tests.c compiles this
 * file UNMODIFIED and drives it through the real DSP dispatch.
 */

#include "gdx_audio_mix_census.h"

#include <string.h>

/* Same reasoning as gdx_ndsp_hash.c: declared plainly rather than weak, because this file is
 * built into host test binaries too and each of those supplies its own sink. */
extern void gdx_dbg_logf(const char* fmt, ...);

#ifdef GDX_PLATFORM_3DS
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
#endif

int gdx_audiomix_armed = 0;

#define GDX_AUDIOMIX_S16_FULL 32768

/* Headroom histogram, expressed as SIXTEENTHS of s16 full scale so the sub-unity buckets and
 * the overdrive buckets share one integer ladder.  Every entry is the bucket's UPPER edge, and
 * the edge is EXCLUSIVE: a sample lands in the first bucket whose edge it is strictly below.
 *
 *   index   upper edge          label        meaning
 *     0     0.125 * 32768       <1/8         quiet
 *     1     0.25  * 32768       <1/4
 *     2     0.5   * 32768       <1/2
 *     3     0.75  * 32768       <3/4
 *     4     1.0   * 32768       <1           loud, still clean -- the last non-clipping bucket
 *     5     1.25  * 32768       <1.25x       overdrive: the clamp is cutting
 *     6     1.5   * 32768       <1.5x
 *     7     (none)              1.5x+
 *
 * The ladder crowds around full scale on purpose.  In this mix topology the bus is CLAMPED
 * after every single voice, so the accumulator entering the next voice is already bounded to
 * s16 and the pre-clamp value can never exceed 2x full scale minus one (32767 + 32767).  A
 * ladder reaching 4x or 8x would have had two permanently-empty buckets; the interesting
 * resolution is between 3/4 and 1.5x, which is where "one more loud voice" lands. */
static const uint32_t kBucketEdgeSixteenths[GDX_AUDIOMIX_BUCKETS - 1] = { 2u, 4u, 8u, 12u, 16u, 20u, 24u };
static const char* const kBucketLabels = "<1/8|<1/4|<1/2|<3/4|<1|<1.25x|<1.5x|1.5x+";

static struct {
    int initialised;
    int mode;
    unsigned int reportTasks;
    unsigned int eventLogs;   /* mode-2 per-task lines already emitted (capped) */
    GdxAudioMixStats st;
    /* per-task accumulators, reset by gdx_audiomix_task_begin() */
    uint32_t taskVoices;
    uint32_t taskSat;
    uint32_t taskPeak;
    uint32_t curRailRun;
} sMc;

#define GDX_AUDIOMIX_EVENT_LOG_CAP 32u

/* ------------------------------------------------------------------ configuration -- */

void gdx_audiomix_configure(int mode, unsigned int reportTasks) {
    memset(&sMc, 0, sizeof(sMc));
    sMc.initialised = 1;
    sMc.mode = (mode > 0) ? mode : 0;
    sMc.reportTasks = (reportTasks != 0u) ? reportTasks : 1800u;
    gdx_audiomix_armed = (sMc.mode > 0);
}

void gdx_audiomix_init(void) {
#ifndef GDX_PLATFORM_3DS
    if (!sMc.initialised) {
        gdx_audiomix_configure(0, 0);
    }
#else
    int mode;
    int report;

    if (sMc.initialised) {
        return;
    }
    mode = gdx3ds_config_get_int("debug", "audiomix", 0);
    report = gdx3ds_config_get_int("debug", "audiomix_report", 1800);
    gdx_audiomix_configure(mode, (report > 0) ? (unsigned int)report : 1800u);

    if (sMc.mode > 0) {
        gdx_dbg_logf("[audiomix] init mode=%d report=%u tap=bus-add+interleave "
                     "hr-labels-are-UPPER-edges\n",
                     sMc.mode, sMc.reportTasks);
    }
#endif
}

int gdx_audiomix_mode(void) {
    return sMc.mode;
}

void gdx_audiomix_reset(void) {
    int mode = sMc.mode;
    unsigned int report = sMc.reportTasks;
    gdx_audiomix_configure(mode, report);
}

/* ------------------------------------------------------------------------- taps -- */

void gdx_audiomix_task_begin(void) {
    sMc.taskVoices = 0u;
    sMc.taskSat = 0u;
    sMc.taskPeak = 0u;
    sMc.curRailRun = 0u;
}

void gdx_audiomix_note_voice(void) {
    sMc.taskVoices++;
}

/* Called once per decode op, so the receipt can say which codec the session's voices actually
   used rather than leaving it to be inferred from the decomp. */
void gdx_audiomix_note_decode(int codecIsSmallAdpcm, int codecIsS8) {
    if (codecIsS8) {
        sMc.st.decS8++;
    } else if (codecIsSmallAdpcm) {
        sMc.st.decAdpcm2++;
    } else {
        sMc.st.decAdpcm4++;
    }
}

static uint32_t GdxAudioMixMagnitude(int32_t v) {
    if (v < 0) {
        /* -INT32_MIN is UB; the bus can never reach it (a s16 sum of s16-scaled terms), but
           the census must not be the thing that invents an overflow. */
        if (v == (-2147483647 - 1)) {
            return 2147483647u;
        }
        return (uint32_t)(-v);
    }
    return (uint32_t)v;
}

static unsigned int GdxAudioMixBucket(uint32_t mag) {
    unsigned int i;
    for (i = 0; i < (unsigned int)(GDX_AUDIOMIX_BUCKETS - 1); i++) {
        /* mag < edge/16 * 32768  ==  mag*16 < edge*32768  ==  mag < edge*2048 */
        if (mag < kBucketEdgeSixteenths[i] * 2048u) {
            return i;
        }
    }
    return (unsigned int)(GDX_AUDIOMIX_BUCKETS - 1);
}

void gdx_audiomix_note_bus_add(int32_t preClamp) {
    uint32_t mag = GdxAudioMixMagnitude(preClamp);

    sMc.st.adds++;
    sMc.st.hr[GdxAudioMixBucket(mag)]++;
    if (mag > sMc.st.peak) {
        sMc.st.peak = mag;
    }
    if (mag > sMc.taskPeak) {
        sMc.taskPeak = mag;
    }
    /* The clamp cuts at +32767 and -32768, so the asymmetry is real and worth honouring:
       a value of exactly -32768 is NOT clipped. */
    if ((preClamp > 32767) || (preClamp < -32768)) {
        sMc.st.sat++;
        sMc.taskSat++;
    }
}

void gdx_audiomix_note_out(int32_t sample) {
    uint32_t mag = GdxAudioMixMagnitude(sample);

    sMc.st.outs++;
    if (mag > sMc.st.outPeak) {
        sMc.st.outPeak = mag;
    }
    if ((sample >= 32767) || (sample <= -32768)) {
        sMc.st.railed++;
        sMc.curRailRun++;
        if (sMc.curRailRun > sMc.st.railRun) {
            sMc.st.railRun = sMc.curRailRun;
        }
    } else {
        sMc.curRailRun = 0u;
    }
}

void gdx_audiomix_task_end(void) {
    sMc.st.tasks++;
    if (sMc.taskVoices > sMc.st.voicesMax) {
        sMc.st.voicesMax = sMc.taskVoices;
    }
    if (sMc.taskSat != 0u) {
        sMc.st.satTasks++;
        if (sMc.taskSat > sMc.st.worstSat) {
            sMc.st.worstSat = sMc.taskSat;
            sMc.st.worstTask = sMc.st.tasks;
            sMc.st.worstVoices = sMc.taskVoices;
            sMc.st.worstPeak = sMc.taskPeak;
        }
        if ((sMc.mode >= 2) && (sMc.eventLogs < GDX_AUDIOMIX_EVENT_LOG_CAP)) {
            sMc.eventLogs++;
            gdx_dbg_logf("[audiomix] clip task=%u voices=%u sat=%u peak=%u (%u%% of FS)\n",
                         (unsigned)sMc.st.tasks, (unsigned)sMc.taskVoices,
                         (unsigned)sMc.taskSat, (unsigned)sMc.taskPeak,
                         (unsigned)((sMc.taskPeak * 100u) / GDX_AUDIOMIX_S16_FULL));
        }
    }
    if ((sMc.reportTasks != 0u) && ((sMc.st.tasks % sMc.reportTasks) == 0u)) {
        gdx_audiomix_report("census");
    }
}

/* ---------------------------------------------------------------------- receipts -- */

void gdx_audiomix_snapshot(GdxAudioMixStats* out) {
    if (out != NULL) {
        *out = sMc.st;
    }
}

static uint32_t GdxAudioMixPpm(uint64_t part, uint64_t whole) {
    if (whole == 0u) {
        return 0u;
    }
    return (uint32_t)((part * 1000000ull) / whole);
}

void gdx_audiomix_report(const char* tag) {
    const GdxAudioMixStats* st = &sMc.st;

    if (sMc.mode <= 0) {
        return;
    }
    gdx_dbg_logf("[audiomix] %s tasks=%u voicesmax=%u adds=%llu sat=%llu (ppm=%u) "
                 "sattasks=%u peak=%u (%u%% of FS)\n",
                 tag, (unsigned)st->tasks, (unsigned)st->voicesMax,
                 (unsigned long long)st->adds, (unsigned long long)st->sat,
                 (unsigned)GdxAudioMixPpm(st->sat, st->adds),
                 (unsigned)st->satTasks, (unsigned)st->peak,
                 (unsigned)((st->peak * 100u) / GDX_AUDIOMIX_S16_FULL));
    /* The label is printed with the numbers, every time, because a histogram quoted without
       its edges is the one audio receipt this project has already misread once. */
    gdx_dbg_logf("[audiomix] %s headroom UPPER-edge hr[%s]="
                 "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
                 tag, kBucketLabels,
                 (unsigned long long)st->hr[0], (unsigned long long)st->hr[1],
                 (unsigned long long)st->hr[2], (unsigned long long)st->hr[3],
                 (unsigned long long)st->hr[4], (unsigned long long)st->hr[5],
                 (unsigned long long)st->hr[6], (unsigned long long)st->hr[7]);
    gdx_dbg_logf("[audiomix] %s out samples=%llu railed=%llu (ppm=%u) railrun=%u outpeak=%u\n",
                 tag, (unsigned long long)st->outs, (unsigned long long)st->railed,
                 (unsigned)GdxAudioMixPpm(st->railed, st->outs),
                 (unsigned)st->railRun, (unsigned)st->outPeak);
    gdx_dbg_logf("[audiomix] %s worst task=%u sat=%u voices=%u peak=%u\n",
                 tag, (unsigned)st->worstTask, (unsigned)st->worstSat,
                 (unsigned)st->worstVoices, (unsigned)st->worstPeak);
    gdx_dbg_logf("[audiomix] %s codec adpcm4=%llu adpcm2small=%llu s8=%llu\n",
                 tag, (unsigned long long)st->decAdpcm4,
                 (unsigned long long)st->decAdpcm2, (unsigned long long)st->decS8);
}

void gdx_audiomix_report_final(void) {
    if (sMc.mode <= 0) {
        return;
    }
    gdx_audiomix_report("FINAL");
}
