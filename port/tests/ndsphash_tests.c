/* Host unit test for port/gdx_ndsp_hash.c — the [debug] ndsphash content-addressed render
 * digest.  Compiled as `gdx3ds_ndsphash_tests` (port/CMakeLists.txt) and as
 * `gdx3ds_ndsphash_host_tests` in tools/ci-3ds.sh.  It compiles gdx_ndsp_hash.c UNMODIFIED and
 * drives it entirely through its public API plus the test-only seams
 * gdx_ndsphash_configure() / gdx_ndsphash_reset_for_test() / gdx_ndsphash_stats_for_test(),
 * the same discipline port/tests/pcm_capture_tests.c uses.
 *
 * WHAT IS WORTH TESTING HERE, and it is not "does FNV work".  The instrument's whole claim is
 * that its key is stable under the things that legitimately move between two runs, and
 * sensitive to the things that must not.  Every case below is one of those two sentences:
 *
 *   CASE 3 is the one that earns its place.  It replays the SAME command list twice with the
 *   address words changed -- a sample-cache slot that moved, exactly what `audiodiet`'s diet
 *   does to every allocation after the 10.25 MB table -- and asserts the key is IDENTICAL.
 *   Without the address masking in GdxNdspOpHasAddress() that key moves, and the cross-run
 *   comparison then reports `compared=0`: a gate that is vacuously green, which is the
 *   failure mode `covfinish` refused an audio census family over.  It fails here instead.
 *
 * The module writes its log lines through a weak gdx_dbg_logf; this file defines one so the
 * lines are captured and so the FINAL receipt's format is exercised rather than merely
 * compiled.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../gdx_ndsp_hash.h"

/* ------------------------------------------------------------------ test harness -- */

static int sChecks = 0;
static int sFailures = 0;

static void check(int cond, const char* what) {
    sChecks++;
    if (!cond) {
        sFailures++;
        printf("FAIL: %s\n", what);
    }
}

static void checkEqU(unsigned long a, unsigned long b, const char* what) {
    sChecks++;
    if (a != b) {
        sFailures++;
        printf("FAIL: %s (got %lu, want %lu)\n", what, a, b);
    }
}

/* Captured log, so a case can assert on what the instrument SAID, not only on its counters. */
static char sLog[65536];
static size_t sLogLen = 0;

void gdx_dbg_logf(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    if (sLogLen < sizeof(sLog) - 1u) {
        n = vsnprintf(sLog + sLogLen, sizeof(sLog) - sLogLen, fmt, ap);
        if (n > 0) {
            sLogLen += (size_t)n;
            if (sLogLen >= sizeof(sLog)) {
                sLogLen = sizeof(sLog) - 1u;
            }
        }
    }
    va_end(ap);
}

static void logReset(void) {
    sLogLen = 0;
    sLog[0] = '\0';
}

static int logCount(const char* needle) {
    int c = 0;
    const char* p = sLog;
    size_t n = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

/* ------------------------------------------------------------- command-list builder -- */

/* The ABI command list is a flat array of {u32 w0, u32 w1} in host order -- exactly what
   gdx_audio_hle_run reads and exactly what gdx_ndsphash_task_begin folds. */
typedef struct {
    uint32_t w0;
    uint32_t w1;
} Cmd;

#define OP_ADPCM     1u
#define OP_CLEARBUFF 2u
#define OP_RESAMPLE  5u
#define OP_MIXER     12u
#define OP_ENVMIXER  19u
#define OP_LOADBUFF  20u
#define OP_SAVEBUFF  21u

static uint32_t mkW0(uint32_t op, uint32_t rest) {
    return (op << 24) | (rest & 0x00FFFFFFu);
}

/* One plausible tick: clear the mix, load a sample, decode it, resample it at a pitch, mix it
   with envelope volumes, save the result.  `addrBase` stands for the run-to-run movement of
   every RDRAM address; `pitch` and `vol` are the render REQUEST and must reach the key. */
static void buildTick(Cmd* c, uint32_t addrBase, uint32_t pitch, uint32_t vol) {
    c[0].w0 = mkW0(OP_CLEARBUFF, 0x0940u);  c[0].w1 = 0x340u;
    c[1].w0 = mkW0(OP_LOADBUFF, 0x10u << 16); c[1].w1 = addrBase + 0x1000u;
    c[2].w0 = mkW0(OP_ADPCM, 0x0001u << 16); c[2].w1 = addrBase + 0x2000u;
    c[3].w0 = mkW0(OP_RESAMPLE, pitch);      c[3].w1 = addrBase + 0x3000u;
    c[4].w0 = mkW0(OP_MIXER, 0x0100u);       c[4].w1 = vol;
    c[5].w0 = mkW0(OP_ENVMIXER, 0x0000u);    c[5].w1 = vol;
    c[6].w0 = mkW0(OP_SAVEBUFF, 0x10u << 16); c[6].w1 = addrBase + 0x4000u;
}

#define TICK_CMDS 7
#define TICK_BYTES ((unsigned int)(TICK_CMDS * (int)sizeof(Cmd)))

/* Drive one whole task through the instrument the way n64_audio_hle.c does: begin, fold the
   content each address-bearing handler resolved, fold what was saved, end. */
static void runTick(const Cmd* c, const void* sampleBytes, unsigned int sampleLen,
                    const void* stateBytes, unsigned int stateLen,
                    const void* savedBytes, unsigned int savedLen) {
    if (gdx_ndsphash_task_begin(c, TICK_BYTES)) {
        gdx_ndsphash_fold_load(sampleBytes, sampleLen);
        gdx_ndsphash_fold_load(stateBytes, stateLen);
        gdx_ndsphash_fold_save(savedBytes, savedLen);
    }
    gdx_ndsphash_task_end();
}

/* The session fold is the one number that stands for "the whole map".  Two sessions that
   agree on it agreed on every (key, value) pair in order. */
static uint64_t foldOf(void) {
    uint64_t f = 0;
    gdx_ndsphash_stats_for_test(NULL, NULL, NULL, &f);
    return f;
}

static unsigned int selfDivOf(void) {
    unsigned int d = 0;
    gdx_ndsphash_stats_for_test(NULL, NULL, &d, NULL);
    return d;
}

/* A short, fixed "session": N ticks, each with its own sample bytes and rendered bytes, so a
   fold is a function of real varying content rather than of one repeated constant. */
static uint64_t runSession(int ticks, uint32_t addrBase, uint32_t pitch, uint32_t vol,
                           int corruptSampleAtTick, int corruptRenderAtTick) {
    Cmd c[TICK_CMDS];
    unsigned char sample[64];
    unsigned char state[32];
    unsigned char saved[64];
    int t;
    unsigned int i;

    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 1, 0, 0);

    for (t = 0; t < ticks; t++) {
        buildTick(c, addrBase, pitch, vol);
        for (i = 0; i < sizeof(sample); i++) {
            sample[i] = (unsigned char)(i * 7u + (unsigned int)t * 13u);
        }
        for (i = 0; i < sizeof(state); i++) {
            state[i] = (unsigned char)(i + (unsigned int)t);
        }
        for (i = 0; i < sizeof(saved); i++) {
            saved[i] = (unsigned char)(i * 3u + (unsigned int)t * 5u);
        }
        if (t == corruptSampleAtTick) {
            sample[0] ^= 0x01u;   /* the SOURCE fault class: changes the key */
        }
        if (t == corruptRenderAtTick) {
            saved[0] ^= 0x01u;    /* the RENDER fault class: changes the value only */
        }
        runTick(c, sample, sizeof(sample), state, sizeof(state), saved, sizeof(saved));
    }
    return foldOf();
}

/* Eight recorded tasks with varying content; defined after the cases that use it. */
static int runSessionMode2(void);

/* ------------------------------------------------------------------------- cases -- */

/* CASE 1 — off is off.  The shipped default must record nothing and say nothing. */
static void case1_default_off(void) {
    Cmd c[TICK_CMDS];
    unsigned char b[32];
    unsigned int tasks = 99, pairs = 99;

    logReset();
    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(0, 1, 0, 0);
    memset(b, 0xAB, sizeof(b));
    buildTick(c, 0x80000000u, 0x4000u, 0x7FFFu);

    check(gdx_ndsphash_enabled() == 0, "case1: disabled after configure(0)");
    check(gdx_ndsphash_task_begin(c, TICK_BYTES) == 0, "case1: task_begin refuses when off");
    gdx_ndsphash_fold_load(b, sizeof(b));
    gdx_ndsphash_fold_save(b, sizeof(b));
    gdx_ndsphash_task_end();
    gdx_ndsphash_report_final();

    gdx_ndsphash_stats_for_test(&tasks, &pairs, NULL, NULL);
    checkEqU(tasks, 0u, "case1: no tasks counted when off");
    checkEqU(pairs, 0u, "case1: no pairs sealed when off");
    checkEqU((unsigned long)sLogLen, 0u, "case1: silent when off");
}

/* CASE 2 — a repeat of the identical session is identical.  This is the "does NOT fire on a
   repeat" half, and it is the half `audiodiet`'s `sum` and `audioasync`'s `sil_ppk` both
   failed: an audio number that moves on a repeat is noise, not a gate. */
static void case2_repeat_is_identical(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    check(a == b, "case2: two identical sessions fold identically");
    checkEqU(selfDivOf(), 0u, "case2: no within-run contradiction");
}

/* CASE 3 — THE ONE THAT EARNS ITS PLACE.  Every RDRAM address moves and the fold must not.
 * A sample-cache slot, a note-state address and an AI buffer all shift by 1 MB, which is what
 * taking a 10.25 MB blob off the heap does to everything allocated after it.  The render
 * REQUEST did not change, so the key must not change.
 * Delete the address-masking arm of GdxNdspOpHasAddress() and this case fails -- and without
 * it the cross-run comparison reports compared=0, i.e. a gate that passes because it compared
 * nothing. */
static void case3_addresses_are_masked(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80400000u, 0x4000u, 0x7FFFu, -1, -1);
    check(a == b, "case3: the fold survives every RDRAM address moving by 1 MB");
}

/* CASE 4 — a PITCH error is caught.  `audioasync` §5 named this as one of the two things its
 * level census explicitly cannot see.  One resample command, one different pitch field. */
static void case4_pitch_error_is_caught(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80300000u, 0x4001u, 0x7FFFu, -1, -1);
    check(a != b, "case4: a one-step pitch change moves the fold");
}

/* CASE 5 — a VOLUME / mix change is caught. */
static void case5_volume_change_is_caught(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80300000u, 0x4000u, 0x7FFEu, -1, -1);
    check(a != b, "case5: a one-step envelope volume change moves the fold");
}

/* CASE 6 — the RENDER fault class: same request, different bytes out.  One bit of one
 * rendered sample in tick 100 of 200.  This is what `divergent` means in ndspdiff.py, and it
 * is the failure `audiodiet` could only infer its way past. */
static void case6_render_fault_is_caught(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, 100);
    check(a != b, "case6: one flipped bit of one rendered sample moves the fold");
}

/* CASE 7 — the SOURCE fault class: different bytes in.  Content-addressing means this moves
 * the KEY, so a cross-run comparison sees it as only_in_a/only_in_b rather than as
 * `divergent`.  Both are failures; ndspdiff.py gates on both, and this case is why it must. */
static void case7_source_fault_is_caught(void) {
    uint64_t a = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, -1, -1);
    uint64_t b = runSession(200, 0x80300000u, 0x4000u, 0x7FFFu, 100, -1);
    check(a != b, "case7: one flipped bit of one delivered sample byte moves the fold");
}

/* CASE 8 — the within-run contradiction check.  Feed the SAME request twice in one session
 * with different rendered bytes; the instrument must notice without any second run at all. */
static void case8_selfdiv(void) {
    Cmd c[TICK_CMDS];
    unsigned char sample[64];
    unsigned char state[32];
    unsigned char saved[64];

    logReset();
    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 1, 0, 0);
    buildTick(c, 0x80300000u, 0x4000u, 0x7FFFu);
    memset(sample, 0x11, sizeof(sample));
    memset(state, 0x22, sizeof(state));

    memset(saved, 0x33, sizeof(saved));
    runTick(c, sample, sizeof(sample), state, sizeof(state), saved, sizeof(saved));
    checkEqU(selfDivOf(), 0u, "case8: first sighting of a key is not a contradiction");

    runTick(c, sample, sizeof(sample), state, sizeof(state), saved, sizeof(saved));
    checkEqU(selfDivOf(), 0u, "case8: an identical repeat is not a contradiction");

    saved[0] ^= 0x01u;
    runTick(c, sample, sizeof(sample), state, sizeof(state), saved, sizeof(saved));
    checkEqU(selfDivOf(), 1u, "case8: same key, different value is a contradiction");
    checkEqU((unsigned long)logCount("[ndsphash] SELFDIV"), 1u, "case8: it says so in the log");
}

/* CASE 9 — mode 2 streams one self-contained pair line per task, which is what
 * tools/ndspdiff.py parses.  A truncated or interleaved log must still yield every pair that
 * made it out, so the format is one pair per line and never a batch. */
static void case9_pair_stream_format(void) {
    unsigned int tick;
    unsigned long key0, key1;
    unsigned long val;
    int got;
    const char* p;

    logReset();
    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(2, 1, 0, 0);
    (void)runSessionMode2();

    checkEqU((unsigned long)logCount("[ndsphash] e "), 8u, "case9: one pair line per task");

    p = strstr(sLog, "[ndsphash] e ");
    check(p != NULL, "case9: a pair line exists");
    if (p != NULL) {
        char k[32];
        got = sscanf(p, "[ndsphash] e %u %16s %8lx", &tick, k, &val);
        checkEqU((unsigned long)got, 3u, "case9: a pair line parses as <tick> <key16> <val8>");
        checkEqU((unsigned long)strlen(k), 16u, "case9: the key is 16 hex digits");
        (void)key0;
        (void)key1;
    }

    gdx_ndsphash_report_final();
    checkEqU((unsigned long)logCount("[ndsphash] FINAL"), 2u, "case9: two FINAL lines");
    gdx_ndsphash_report_final();
    checkEqU((unsigned long)logCount("[ndsphash] FINAL"), 2u, "case9: FINAL is idempotent");
}

/* CASE 10 — sampling.  `ndsphash_every` bounds the log on a long session; both sides of a
 * comparison must use the same value, and the task count must still tick for every task so
 * the fault-injection indices stay meaningful. */
static void case10_sampling(void) {
    Cmd c[TICK_CMDS];
    unsigned char b[32];
    unsigned int tasks = 0, pairs = 0;
    int t;

    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 4, 0, 0);
    buildTick(c, 0x80300000u, 0x4000u, 0x7FFFu);
    memset(b, 0x55, sizeof(b));
    for (t = 0; t < 40; t++) {
        runTick(c, b, sizeof(b), b, sizeof(b), b, sizeof(b));
    }
    gdx_ndsphash_stats_for_test(&tasks, &pairs, NULL, NULL);
    checkEqU(tasks, 40u, "case10: every task is counted");
    checkEqU(pairs, 10u, "case10: one task in four is recorded");
}

/* CASE 11 — a malformed or empty list is refused rather than hashed.  The audio thread holds
 * the context mutex across this call; a crash here freezes the game thread at its next
 * AudioThread_QueueCmd. */
static void case11_degenerate_lists(void) {
    Cmd c[TICK_CMDS];
    unsigned int pairs = 0;

    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 1, 0, 0);
    buildTick(c, 0x80300000u, 0x4000u, 0x7FFFu);

    check(gdx_ndsphash_task_begin(NULL, TICK_BYTES) == 0, "case11: NULL list refused");
    gdx_ndsphash_task_end();
    check(gdx_ndsphash_task_begin(c, 0u) == 0, "case11: zero-length list refused");
    gdx_ndsphash_task_end();
    check(gdx_ndsphash_task_begin(c, 4u) == 0, "case11: half-a-command list refused");
    gdx_ndsphash_task_end();

    /* A fold with no live task, and a fold of nothing, must both be no-ops rather than
       corrupting the next task's key. */
    gdx_ndsphash_fold_load(c, TICK_BYTES);
    gdx_ndsphash_fold_save(c, TICK_BYTES);
    gdx_ndsphash_fold_load(NULL, 16u);
    gdx_ndsphash_fold_save(NULL, 16u);

    gdx_ndsphash_stats_for_test(NULL, &pairs, NULL, NULL);
    checkEqU(pairs, 0u, "case11: nothing was sealed");
}

/* CASE 12 — fault injection is spent exactly once and only on its own task index. */
static void case12_fault_is_one_shot(void) {
    Cmd c[TICK_CMDS];
    unsigned char b[32];
    int t;
    int renderFires = 0;
    int sourceFires = 0;

    logReset();
    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 1, 5, 9);
    buildTick(c, 0x80300000u, 0x4000u, 0x7FFFu);
    memset(b, 0x77, sizeof(b));
    for (t = 0; t < 20; t++) {
        gdx_ndsphash_task_begin(c, TICK_BYTES);
        if (gdx_ndsphash_fault_source()) {
            sourceFires++;
        }
        gdx_ndsphash_fold_load(b, sizeof(b));
        if (gdx_ndsphash_fault_render()) {
            renderFires++;
        }
        gdx_ndsphash_fold_save(b, sizeof(b));
        gdx_ndsphash_task_end();
    }
    checkEqU((unsigned long)renderFires, 1u, "case12: the render fault fires exactly once");
    checkEqU((unsigned long)sourceFires, 1u, "case12: the source fault fires exactly once");
    checkEqU((unsigned long)logCount("[ndsphash] FAULT"), 2u, "case12: both faults are announced");

    /* And with no fault configured, nothing ever fires -- the shipped shape. */
    gdx_ndsphash_reset_for_test();
    gdx_ndsphash_configure(1, 1, 0, 0);
    renderFires = 0;
    for (t = 0; t < 20; t++) {
        gdx_ndsphash_task_begin(c, TICK_BYTES);
        if (gdx_ndsphash_fault_render() || gdx_ndsphash_fault_source()) {
            renderFires++;
        }
        gdx_ndsphash_task_end();
    }
    checkEqU((unsigned long)renderFires, 0u, "case12: no fault fires when none is configured");
}

/* Helper for case 9: eight recorded tasks with varying content. */
static int runSessionMode2(void) {
    Cmd c[TICK_CMDS];
    unsigned char b[32];
    int t;
    buildTick(c, 0x80300000u, 0x4000u, 0x7FFFu);
    for (t = 0; t < 8; t++) {
        memset(b, (int)(0x10 + t), sizeof(b));
        runTick(c, b, sizeof(b), b, sizeof(b), b, sizeof(b));
    }
    return t;
}

int main(void) {
    case1_default_off();
    case2_repeat_is_identical();
    case3_addresses_are_masked();
    case4_pitch_error_is_caught();
    case5_volume_change_is_caught();
    case6_render_fault_is_caught();
    case7_source_fault_is_caught();
    case8_selfdiv();
    case9_pair_stream_format();
    case10_sampling();
    case11_degenerate_lists();
    case12_fault_is_one_shot();

    printf("gdx3ds_ndsphash_tests: %d checks, %d failures\n", sChecks, sFailures);
    return (sFailures == 0) ? 0 : 1;
}
