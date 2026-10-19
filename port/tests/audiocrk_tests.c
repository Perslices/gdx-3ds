/* gdx3ds_audiocrk_host_tests -- [debug] audiocrk (port/3ds/audio/gdx3ds_audio_crk.c).
 *
 * The census is what a hardware card reads to decide WHICH of five mechanisms the user hears
 * as crackle, so its classification has to be right before a console run is spent on it:
 * a splice at a wave-buffer boundary must land in cstep (not step), an underrun in
 * under/dry/pad, a rail in rail, a DSP queue running dry in dspdry/stop, and padding must
 * never be read as a real->real boundary. Drives the module UNMODIFIED. */
#include "gdx3ds_audio_crk.h"

#include <stdio.h>
#include <string.h>

static int sChecks;
static int sFails;

static void Check(int cond, const char* what) {
    sChecks++;
    if (!cond) {
        sFails++;
        printf("FAIL: %s\n", what);
    }
}

#define CHUNK 512u

static void FillSine(int16_t* pcm, uint32_t frames, int16_t amp) {
    /* a slow triangle: adjacent deltas stay far below any step threshold */
    for (uint32_t f = 0; f < frames; f++) {
        int32_t phase = (int32_t)(f % 64u);
        int32_t v = (phase < 32) ? phase : 64 - phase;
        int16_t s = (int16_t)((v * amp) / 32);
        pcm[f * 2u] = s;
        pcm[f * 2u + 1u] = s;
    }
}

static void TestCleanStream(void) {
    GdxCrk c;
    int16_t pcm[CHUNK * 2u];
    gdx_crk_init(&c, 0);
    FillSine(pcm, CHUNK, 8000);
    for (int i = 0; i < 10; i++) {
        gdx_crk_note_chunk(&c, pcm, CHUNK, CHUNK, 2048u);
    }
    Check(c.win.chunks == 10u, "clean: 10 chunks");
    Check(c.win.partial == 0u && c.win.dry == 0u && c.win.padFrames == 0u, "clean: no underrun");
    Check(c.win.stepIn == 0u, "clean: no interior steps");
    Check(c.win.pairsChunk == 18u, "clean: 9 boundaries x 2 channels");
    Check(c.win.stepChunk == 0u, "clean: no boundary steps");
    {
        char line[256];
        gdx_crk_format_mean(&c.win, "w", line, sizeof(line));
        /* a 64-frame triangle of amplitude 8000: |delta| is 250 everywhere, boundaries included */
        Check(strcmp(line, "[audiocrk] w dmean100 in=25000 c=25000 p=0") == 0, "mean: clean triangle");
    }
    Check(c.win.rail == 0u && c.win.hot == 0u, "clean: no rail/hot");
    Check(c.win.ringMin == 2048u, "clean: rmin");
    Check(c.stepThreshold == GDX_CRK_STEP_DEFAULT, "default threshold");
    Check(!gdx_crk_window_bad(&c.win), "clean: window not bad");
}

static void TestBoundarySplice(void) {
    GdxCrk c;
    int16_t a[CHUNK * 2u];
    int16_t b[CHUNK * 2u];
    gdx_crk_init(&c, 8192);
    for (uint32_t k = 0; k < CHUNK * 2u; k++) {
        a[k] = 20000;
        b[k] = -20000;
    }
    gdx_crk_note_chunk(&c, a, CHUNK, CHUNK, 4096u);
    gdx_crk_note_chunk(&c, b, CHUNK, CHUNK, 4096u);
    Check(c.win.stepChunk == 2u, "splice: boundary step counted per channel");
    Check(c.win.dmaxChunk == 40000u, "splice: boundary dmax");
    Check(c.win.stepIn == 0u, "splice: not an interior step");
    Check(gdx_crk_window_bad(&c.win), "splice: window bad");
}

static void TestUnderrunAndPaddingChain(void) {
    GdxCrk c;
    int16_t full[CHUNK * 2u];
    int16_t part[CHUNK * 2u];
    gdx_crk_init(&c, 8192);
    for (uint32_t k = 0; k < CHUNK * 2u; k++) {
        full[k] = 20000;
        part[k] = (k < 100u * 2u) ? 20000 : 0; /* 100 real frames, then padding */
    }
    gdx_crk_note_chunk(&c, full, CHUNK, CHUNK, 900u);
    gdx_crk_note_chunk(&c, part, 100u, CHUNK, 100u);
    gdx_crk_note_chunk(&c, full, 0u, CHUNK, 0u); /* dry */
    gdx_crk_note_chunk(&c, full, CHUNK, CHUNK, 600u);
    Check(c.win.partial == 1u, "under: one partial");
    Check(c.win.dry == 1u, "under: one dry");
    Check(c.win.padFrames == (CHUNK - 100u) + CHUNK, "under: pad depth");
    Check(c.win.ringMin == 0u, "under: rmin 0");
    /* full -> part is real->real (both 20000: no step); part ended in padding, then dry,
     * then full: no boundary is compared across padding. */
    Check(c.win.pairsChunk == 2u, "under: only the real->real boundary is compared");
    Check(c.win.stepChunk == 0u, "under: padding never counted as a boundary step");
    Check(gdx_crk_window_bad(&c.win), "under: window bad");
}

static void TestRailAndHot(void) {
    GdxCrk c;
    int16_t pcm[CHUNK * 2u];
    gdx_crk_init(&c, 8192);
    memset(pcm, 0, sizeof(pcm));
    pcm[10] = 32767;
    pcm[11] = -32768;
    pcm[12] = 25000;
    gdx_crk_note_chunk(&c, pcm, CHUNK, CHUNK, 1000u);
    Check(c.win.rail == 2u, "rail: +32767 and -32768");
    Check(c.win.hot == 3u, "hot: the two rails plus 25000");
    Check(c.win.peak == 32768u, "peak: |-32768|");
    Check(c.win.stepIn > 0u, "rail: the jumps are interior steps");
}

static void TestPollAndDrops(void) {
    GdxCrk c;
    gdx_crk_init(&c, 0);
    gdx_crk_note_poll(&c, 2u, 0u, 0);     /* before playback: not a stop */
    gdx_crk_note_poll(&c, 2u, 2000u, 1);
    gdx_crk_note_poll(&c, 0u, 17000u, 0); /* DSP ran dry, late poll, channel stopped */
    gdx_crk_note_poll(&c, 1u, 2000u, 1);
    gdx_crk_note_drop_total(&c, 5u);      /* first read: baseline only */
    gdx_crk_note_drop_total(&c, 8u);
    Check(c.win.polls == 4u, "poll count");
    Check(c.win.dspDry == 1u, "dspdry");
    Check(c.win.pollLate == 1u, "late");
    Check(c.win.pollMaxUs == 17000u, "pollmax");
    Check(c.win.stops == 1u, "one stop (the pre-playback poll is not one)");
    Check(c.win.ndrop == 3u, "ndrop delta");
    gdx_crk_note_ring_drops(&c, 100u);
    gdx_crk_note_ring_drops(&c, 612u);
    Check(c.win.dropFrames == 512u, "ring drop delta");
    Check(gdx_crk_window_bad(&c.win), "drops: window bad");
}

static void TestPushAndProducer(void) {
    GdxCrk c;
    int16_t a[4] = { 100, 100, 30000, 30000 };
    int16_t b[4] = { -5000, -5000, 0, 0 };
    gdx_crk_init(&c, 8192);
    Check(gdx_crk_note_push(&c, a, 2u) == 0, "first push: no boundary");
    Check(gdx_crk_note_push(&c, b, 2u) == 1, "second push: stepped");
    Check(c.win.pairsPush == 2u && c.win.stepPush == 2u, "push boundary step 30000 -> -5000");
    /* tail of a = {0, 0, 100, 30000} (a has 2 frames), head of b = {-5000, 0, 0, 0} */
    Check(c.evPush[0][0] == 0 && c.evPush[1][0] == 0 && c.evPush[2][0] == 100 &&
              c.evPush[3][0] == 30000 && c.evPush[4][0] == -5000 && c.evPush[5][0] == 0 &&
              c.evPush[6][0] == 0 && c.evPush[7][0] == 0,
          "push event frames -4..-1|0..+3");
    Check(c.win.dsumPush == 70000u, "push dsum");
    Check(c.win.dmaxPush == 35000u, "push dmax");
    gdx_crk_break_chain(&c);
    gdx_crk_note_push(&c, a, 2u);
    Check(c.win.pairsPush == 2u, "break_chain: no boundary after a resume");
    gdx_crk_note_tick(&c, 900u);
    gdx_crk_note_tick(&c, 12000u);
    gdx_crk_note_wake(&c, 2100u);
    gdx_crk_note_wake(&c, 1500u);
    Check(c.win.ticks == 2u && c.win.tickMaxUs == 12000u, "ticks/tickmax");
    Check(c.win.bufMin == 1500u, "bufmin");
}

static void TestFoldAndFormat(void) {
    GdxCrk c;
    char line[256];
    int n;
    gdx_crk_init(&c, 0);
    c.win.partial = 2u;
    c.win.ringMin = 300u;
    c.win.peak = 1000u;
    gdx_crk_fold(&c.total, &c.win);
    gdx_crk_window_reset(&c.win);
    c.win.partial = 1u;
    c.win.ringMin = 700u;
    c.win.peak = 5000u;
    gdx_crk_fold(&c.total, &c.win);
    Check(c.total.partial == 3u, "fold: counters add");
    Check(c.total.ringMin == 300u, "fold: min");
    Check(c.total.peak == 5000u, "fold: max");
    gdx_crk_window_reset(&c.win);
    n = gdx_crk_format_sched(&c.win, "w", 123ul, line, sizeof(line));
    Check(n > 0 && n < 256 && strstr(line, "rmin=-1") != NULL, "format: empty rmin prints -1");
    Check(strncmp(line, "[audiocrk] w t=123ms", 20) == 0, "format: sched prefix");
    /* worst-case widths still fit the 256-byte AudioOutLogf line */
    memset(&c.win, 0xFF, sizeof(c.win));
    n = gdx_crk_format_sched(&c.win, "FINAL", 4294967295ul, line, sizeof(line));
    Check(n > 0 && (size_t)n == strlen(line) && strlen(line) < 256u, "format: sched worst case fits");
    Check(strstr(line, "ndrop=4294967295") != NULL, "format: sched not truncated");
    n = gdx_crk_format_content(&c.win, "FINAL", line, sizeof(line));
    Check(n > 0 && (size_t)n == strlen(line) && strstr(line, "bufmin=-1") != NULL,
          "format: content worst case fits");
}

int main(void) {
    TestCleanStream();
    TestBoundarySplice();
    TestUnderrunAndPaddingChain();
    TestRailAndHot();
    TestPollAndDrops();
    TestPushAndProducer();
    TestFoldAndFormat();
    printf("audiocrk: %d/%d checks passed\n", sChecks - sFails, sChecks);
    return sFails == 0 ? 0 : 1;
}
