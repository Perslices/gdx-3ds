/* gdx_audio_mix_census — HEADROOM as a number, at the one place voices become a bus.
 *
 * WHY THIS EXISTS.  The port has four audio instruments and none of them can see a clip:
 *
 *   [audioring]  sil_ppk / dry / drop   -- did the samples ARRIVE (delivery)
 *   [audioring]  level mag= / peak=     -- a MEAN over real frames; a 30-sample transient
 *                                          clip moves `mag` by nothing at all
 *   [audio-diet] reads= / sum=          -- were the right BYTES fetched (supply)
 *   [ndsphash]   fold=                  -- is the render CONSISTENT, never whether it is right
 *   dsp_tests.c                         -- are the KERNELS correct, one voice at a time
 *
 * Every one of those is green on the hardware run where the user still hears the countdown
 * crackle.  What none of them looks at is the MIX: several loud voices summing into a
 * saturating 16-bit bus.  Each kernel can be bit-exact and the sum can still sit on the rail.
 *
 * WHAT IS MEASURED, and the two halves are different claims:
 *
 *   BUS ADDS   every accumulate into a mix bus (A_ENVMIXER's four dry/wet destinations,
 *              A_MIXER's gain mix, A_ADDMIXER's unity add) is examined BEFORE the s16 clamp.
 *              That is the only moment the overshoot exists: after ClampS16 a sample that
 *              wanted +40000 and a sample that wanted +32768 are the same byte pair.
 *              -> adds, sat, peak, and a headroom histogram.
 *
 *   OUTPUT     the fully mixed dry L/R buses as A_INTERLEAVE reads them -- what actually
 *              leaves the DSP.  A railed sample there is audible; a railed intermediate that
 *              a later negative voice pulls back down may not be.
 *              -> outs, railed, railrun (the longest CONSECUTIVE railed run), outpeak.
 *
 * READING THE HISTOGRAM.  Every bucket label is its UPPER edge, as a multiple of s16 full
 * scale (32768).  `<1` is the last bucket that does not clip; everything from `<1.25x` up is
 * overdrive that the clamp is eating.  This project has misread an upper-edge histogram as a
 * lower-edge one once already -- the labels are spelled out in the receipt for that reason.
 *
 * COST WHEN OFF.  `gdx_audiomix_armed` is a plain int, zero at startup and written once at
 * init.  Every tap is `if (gdx_audiomix_armed)` on that one int: one perfectly-predicted
 * compare, no call, no memory traffic, nothing allocated (the whole census is one static
 * struct).  The bus-add tap sits in A_ENVMIXER's per-sample loop, so it is the one tap that
 * is not per-op; port/n64_audio_hle.c funnels all six bus writes through a single MixBusWrite()
 * so there is exactly one such compare per bus write and exactly one place to read.
 *
 * LEVERS ([debug] section, both default 0/off):
 *   audiomix         0 = off; 1 = counters + periodic + FINAL; 2 = + one line per clipping task
 *   audiomix_report  audio tasks between periodic receipts (default 1800 ~= 30 s)
 *
 * Plain C99, no platform headers: port/tests/dsp_tests.c compiles this file UNMODIFIED and
 * asserts on the snapshot, the same discipline gdx_ndsp_hash.c uses.
 */
#ifndef GDX_AUDIO_MIX_CENSUS_H
#define GDX_AUDIO_MIX_CENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of headroom buckets.  Bucket i holds |preClamp| < kEdge[i] * 32768; see the .c. */
#define GDX_AUDIOMIX_BUCKETS 8

typedef struct GdxAudioMixStats {
    uint64_t adds;      /* bus-add samples examined (pre-clamp)                        */
    uint64_t sat;       /* of those, how many the s16 clamp actually had to cut        */
    uint64_t hr[GDX_AUDIOMIX_BUCKETS]; /* headroom histogram, UPPER edges (see the .c)  */
    uint32_t peak;      /* max |pre-clamp| accumulator this session                    */
    uint64_t outs;      /* mixed output samples examined at A_INTERLEAVE               */
    uint64_t railed;    /* of those, how many sat at exactly +32767 / -32768           */
    uint32_t railRun;   /* longest CONSECUTIVE railed run in the output                */
    uint32_t outPeak;   /* max |output sample|                                         */
    uint32_t tasks;     /* audio tasks (command lists) seen                            */
    uint32_t satTasks;  /* tasks in which at least one bus add saturated               */
    uint32_t voicesMax; /* most A_ENVMIXER voices in any one task                      */
    uint32_t worstTask;    /* task index with the most saturating adds                 */
    uint32_t worstSat;     /* that task's saturating-add count                         */
    uint32_t worstVoices;  /* that task's voice count                                  */
    uint32_t worstPeak;    /* that task's peak |pre-clamp|                             */
    /* CODEC census. Which decoder the session's voices actually went through, which is the
       question dspclick-progress.md 6.4 left open about the countdown sample. In the SHIPPED
       audio/rom path decomp never emits flags|4 (its codec switch has only CODEC_ADPCM and
       CODEC_S8), so `adpcm2` is expected to be 0 and a nonzero would mean that reading of the
       decomp is wrong. */
    uint64_t decAdpcm4; /* A_ADPCM ops, standard 4-bit VADPCM (block-convolution path)     */
    uint64_t decAdpcm2; /* A_ADPCM ops with flags|4, "small ADPCM" (sequential path)       */
    uint64_t decS8;     /* A_S8DEC ops, uncompressed signed 8-bit                          */
} GdxAudioMixStats;

/* One int, read directly by the taps.  Never written outside gdx_audiomix_configure(). */
extern int gdx_audiomix_armed;

void gdx_audiomix_init(void);                         /* reads the INI on 3DS, else off */
void gdx_audiomix_configure(int mode, unsigned int reportTasks);
int  gdx_audiomix_mode(void);
void gdx_audiomix_reset(void);

/* Taps.  Each is called only with gdx_audiomix_armed nonzero. */
void gdx_audiomix_task_begin(void);
void gdx_audiomix_note_voice(void);
void gdx_audiomix_note_decode(int codecIsSmallAdpcm, int codecIsS8);
void gdx_audiomix_note_bus_add(int32_t preClamp);
void gdx_audiomix_note_out(int32_t sample);
void gdx_audiomix_task_end(void);

void gdx_audiomix_snapshot(GdxAudioMixStats* out);
void gdx_audiomix_report(const char* tag);            /* one receipt, 4 lines */
void gdx_audiomix_report_final(void);                 /* no-op when disarmed */

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_MIX_CENSUS_H */
