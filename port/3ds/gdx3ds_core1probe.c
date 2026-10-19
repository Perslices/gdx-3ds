/* port/3ds/gdx3ds_core1probe.c -- N60TICK: the core-1 probe. Contract in gdx3ds_core1probe.h.
 *
 * Main thread (the host loop) drives a state machine; one worker thread on core 1 runs the
 * fixed job or the spin. The worker never logs, never touches game state and never calls
 * into the game: it only reads svcGetSystemTick and writes its own static result fields.
 * Heap only when armed (job buffers + the worker stack). */
#include "gdx3ds_core1probe.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <3ds.h>

#include "gdx3ds_audio.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_tickahead.h"

#define C1P_MAX_PCTS 4
#define C1P_SAMPLES 64                 /* per metric per window */
#define C1P_JOB_VERTS 256              /* 4 KB of float4 in, 2 KB of s16x4 out */
#define C1P_WORKER_STACK (8u * 1024u)
#define C1P_PRIO_LO 0x30               /* below the audio drain (0x18) and producer (0x19) */
#define C1P_PRIO_EQ 0x19               /* the producer's own priority (FIFO with it) */
#define C1P_GAP_US 20u                 /* a tick-to-tick step above this is a gap, not a run */
#define C1P_PARK_POLL_NS 1000000LL     /* 1 ms */
#define C1P_PARK_POLLS 50              /* ~50 ms bound, the audio park's bound */
#define C1P_FS_PATH "/3ds/gdiffuser/gdiffuser.ini"
#define C1P_LOOP_SAMPLES 512           /* host-loop periods per window (N60CORE1) */
#define C1P_LOOP_REP_US 20000u         /* a period above this repeated a vblank (missed 60) */
#define C1P_WATCH_EVERY 1024u          /* [core1] watch receipt cadence, ticks */

enum { PH_IDLE = 0, PH_JOB_LO, PH_JOB_EQ, PH_SPIN, PH_PER_PCT, PH_COOL = PH_PER_PCT };
enum { CMD_NONE = 0, CMD_JOB, CMD_SPIN, CMD_QUIT };

static const char* const kPhaseNames[] = { "idle", "job_lo", "job_eq", "spin", "cool" };

typedef struct Samples {
    uint32_t v[C1P_SAMPLES];
    int n;
} Samples;

/* ---- lever / config ---- */
static int sChecked = 0;
static int sArmed = 0;
static int sWindow = 240;
static unsigned long sStartTick = 600;
static int sPcts[C1P_MAX_PCTS] = { 30, 50, 80, 0 };
static int sPctCount = 3;
static int sJobUs = 1000;
static int sJobEvery = 4;
static int sGrantedPct = 0;
static int sIsHomebrew = 0;

/* ---- state machine (main thread) ---- */
static int sPctIndex = 0;
static int sPhase = PH_IDLE;
static int sWindowTicks = 0;
static unsigned long sWindowNum = 0;
static int sParked = 0;
static int sStarted = 0;
static int sCurPct = 0;
static Result sCurRc = 0;
static u32 sCurGot = 0;

/* ---- worker (core 1) ---- */
static Thread sWorker = NULL;
static LightEvent sGo;
static volatile int sCmd = CMD_NONE;
static volatile int sBusy = 0;         /* set by main at dispatch, cleared by the worker */
static volatile int sSpinStop = 0;
static volatile uint64_t sJobStartTick = 0;
static volatile uint64_t sJobEndTick = 0;
static volatile uint64_t sSpinRun = 0;
static volatile uint64_t sSpinGap = 0;
static volatile uint64_t sSpinGapMax = 0;
static volatile uint32_t sSpinGaps = 0;
static int sJobReps = 1;
static float sJobCpu0Us = 0.0f;
static float* sJobSrc = NULL;
static int16_t* sJobDst = NULL;
static volatile uint32_t sJobSink = 0;

/* ---- per-window measurements (main thread) ---- */
static uint64_t sDispatchTick = 0;
static int sJobPending = 0;
static unsigned sJobBusySkips = 0;
static Samples sJobWall;
static Samples sJobStart;
static Samples sGsp;
static Samples sFsIpc;
static Samples sFsRead;
static size_t sAudioMin = (size_t)-1;
static Handle sFsFile = 0;
static uint8_t sFsBuf[512];

/* ---- N60CORE1: what the share costs the GAME (main thread), per window ---- */
static uint32_t* sLoopUs = NULL;       /* heap when armed: host-loop periods this window */
static int sLoopN = 0;
static unsigned sLoopRep = 0;          /* periods > C1P_LOOP_REP_US */
static uint64_t sLoopPrevTick = 0;
static unsigned long sUnderAtStart = 0;
static uint32_t sDropAtStart = 0;

/* ---- N60CORE1: the [core1] watch (ndsp_core1 / syscore_pct set, probe off) ---- */
static int sWatch = 0;
static unsigned long sWatchTicks = 0;
static uint64_t sWatchPrevTick = 0;
static uint64_t sWatchSumTicks = 0;
static uint64_t sWatchMaxTicks = 0;
static unsigned sWatchRep = 0;
static unsigned long sWatchUnder = 0;
static uint32_t sWatchDrop = 0;

static void C1pLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
        svcOutputDebugString(buf, len);
        gdx3ds_filelog_write(buf, len);
    }
}

static uint32_t TicksToUs(uint64_t ticks) {
    return (uint32_t)(ticks / (uint64_t)CPU_TICKS_PER_USEC);
}

/* A core-1 stamp minus a core-0 stamp. On the console the system tick is one counter; the
 * emulator keeps one per emulated core and they can skew, so a negative span reads as 0 and is
 * counted (`skew=` in the window receipt). */
static unsigned sSkewed = 0;

static uint32_t CrossCoreUs(uint64_t later, uint64_t earlier) {
    if (later < earlier) {
        sSkewed++;
        return 0;
    }
    return TicksToUs(later - earlier);
}

/* ---- samples ---- */

static void SamplesReset(Samples* s) {
    s->n = 0;
}

static void SamplesAdd(Samples* s, uint32_t v) {
    if (s->n < C1P_SAMPLES) {
        s->v[s->n++] = v;
    }
}

static int CompareU32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/* Sorts in place; out = {median, p90, max} (0 when empty). */
static void SamplesStats(Samples* s, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (s->n == 0) {
        return;
    }
    qsort(s->v, (size_t)s->n, sizeof(uint32_t), CompareU32);
    out[0] = s->v[s->n / 2];
    out[1] = s->v[(s->n * 9) / 10 < s->n ? (s->n * 9) / 10 : s->n - 1];
    out[2] = s->v[s->n - 1];
}

/* ---- the job: a 4x4 float transform + s16 pack over 4 KB, `reps` times (NRR-prep shaped) ---- */

static void RunJobOnce(void) {
    static const float m[16] = { 0.98f, 0.01f, -0.17f, 0.0f, 0.02f, 1.01f, 0.05f, 0.0f,
                                 0.17f, -0.04f, 0.98f, 0.0f, 12.5f, -3.0f, 140.0f, 1.0f };
    uint32_t acc = 0;
    for (int i = 0; i < C1P_JOB_VERTS; i++) {
        const float* v = &sJobSrc[i * 4];
        const float x = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + v[3] * m[12];
        const float y = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + v[3] * m[13];
        const float z = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + v[3] * m[14];
        const float w = v[0] * m[3] + v[1] * m[7] + v[2] * m[11] + v[3] * m[15];
        const float inv = 1.0f / (w != 0.0f ? w : 1.0f);
        int16_t* o = &sJobDst[i * 4];
        o[0] = (int16_t)(x * inv);
        o[1] = (int16_t)(y * inv);
        o[2] = (int16_t)(z * inv);
        o[3] = (int16_t)w;
        acc += (uint16_t)o[0] ^ (uint16_t)o[2];
    }
    sJobSink += acc;
}

static void RunJob(int reps) {
    for (int r = 0; r < reps; r++) {
        RunJobOnce();
    }
}

static void FillJobSource(void) {
    for (int i = 0; i < C1P_JOB_VERTS * 4; i++) {
        sJobSrc[i] = (float)((i * 37) % 2048) - 1024.0f;
    }
    for (int i = 0; i < C1P_JOB_VERTS; i++) {
        sJobSrc[i * 4 + 3] = 1.0f + (float)(i % 7) * 0.125f;
    }
}

/* Main thread, core 0, at init: reps for ~sJobUs of CPU, and that CPU (min of 3, warm). */
static void CalibrateJob(void) {
    uint64_t best = UINT64_MAX;
    RunJob(4);
    for (int k = 0; k < 3; k++) {
        const uint64_t t0 = svcGetSystemTick();
        RunJob(4);
        const uint64_t dt = svcGetSystemTick() - t0;
        if (dt < best) {
            best = dt;
        }
    }
    const double perRepUs = (double)best / 4.0 / (double)CPU_TICKS_PER_USEC;
    sJobReps = perRepUs > 0.0 ? (int)((double)sJobUs / perRepUs + 0.5) : 1;
    if (sJobReps < 1) {
        sJobReps = 1;
    }
    sJobCpu0Us = (float)(perRepUs * (double)sJobReps);
}

/* ---- the worker ---- */

static void WorkerSpin(void) {
    const uint64_t gapTicks = (uint64_t)C1P_GAP_US * (uint64_t)CPU_TICKS_PER_USEC;
    uint64_t run = 0;
    uint64_t gap = 0;
    uint64_t gapMax = 0;
    uint32_t gaps = 0;
    uint64_t prev = svcGetSystemTick();
    while (!sSpinStop) {
        const uint64_t now = svcGetSystemTick();
        const uint64_t dt = now - prev;
        prev = now;
        if (dt > gapTicks) {
            gap += dt;
            gaps++;
            if (dt > gapMax) {
                gapMax = dt;
            }
        } else {
            run += dt;
        }
    }
    sSpinRun = run;
    sSpinGap = gap;
    sSpinGapMax = gapMax;
    sSpinGaps = gaps;
}

static void WorkerJob(void) {
    sJobStartTick = svcGetSystemTick();
    RunJob(sJobReps);
    sJobEndTick = svcGetSystemTick();
}

static void WorkerMain(void* arg) {
    (void)arg;
    for (;;) {
        LightEvent_Wait(&sGo);
        const int cmd = sCmd;
        if (cmd == CMD_QUIT) {
            break;
        } else if (cmd == CMD_JOB) {
            WorkerJob();
        } else if (cmd == CMD_SPIN) {
            WorkerSpin();
        }
        __dmb();
        sBusy = 0;
        __dmb();
    }
    __dmb();
    sBusy = 0;
}

static void WorkerSetPriority(int prio) {
    if (sWorker != NULL) {
        svcSetThreadPriority(threadGetHandle(sWorker), prio);
    }
}

static void WorkerDispatch(int cmd) {
    sCmd = cmd;
    sBusy = 1;
    __dmb();
    LightEvent_Signal(&sGo);
}

/* Bounded wait for the worker to be idle (parked on sGo). 1 = idle. */
static int WorkerWaitIdle(void) {
    for (int i = 0; i < C1P_PARK_POLLS; i++) {
        __dmb();
        if (!sBusy) {
            return 1;
        }
        svcSleepThread(C1P_PARK_POLL_NS);
    }
    return 0;
}

/* ---- the share ---- */

static void ApplyPct(int pct) {
    u32 got = 0;
    sCurPct = pct;
    sCurRc = APT_SetAppCpuTimeLimit((u32)pct);
    APT_GetAppCpuTimeLimit(&got);
    sCurGot = got;
}

static void RestoreGrantedPct(void) {
    if (sGrantedPct > 0) {
        APT_SetAppCpuTimeLimit((u32)sGrantedPct);
    }
}

static int PhasePct(void) {
    if (sPhase == PH_COOL) {
        return sGrantedPct > 0 ? sGrantedPct : 30;
    }
    return sPcts[sPctIndex];
}

/* ---- main-side latency samples ---- */

static void SampleGsp(void) {
    const uint64_t t0 = svcGetSystemTick();
    GSPGPU_FlushDataCache(sJobSrc, 4096);
    SamplesAdd(&sGsp, TicksToUs(svcGetSystemTick() - t0));
}

static void SampleFsIpc(void) {
    bool detected = false;
    const uint64_t t0 = svcGetSystemTick();
    FSUSER_IsSdmcDetected(&detected);
    SamplesAdd(&sFsIpc, TicksToUs(svcGetSystemTick() - t0));
}

static void SampleFsRead(void) {
    u32 got = 0;
    if (sFsFile == 0) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    const Result rc = FSFILE_Read(sFsFile, &got, 0, sFsBuf, sizeof(sFsBuf));
    if (R_SUCCEEDED(rc)) {
        SamplesAdd(&sFsRead, TicksToUs(svcGetSystemTick() - t0));
    }
}

static void OpenFsFile(void) {
    const FS_Path empty = fsMakePath(PATH_EMPTY, "");
    const FS_Path path = fsMakePath(PATH_ASCII, C1P_FS_PATH);
    Handle h = 0;
    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SDMC, empty, path, FS_OPEN_READ, 0))) {
        sFsFile = h;
    }
}

static void SampleAudio(void) {
    const size_t b = gdx3ds_audio_buffered();
    if (b < sAudioMin) {
        sAudioMin = b;
    }
}

/* ---- job collection ---- */

static void CollectJob(void) {
    __dmb();
    if (!sJobPending || sBusy) {
        return;
    }
    SamplesAdd(&sJobWall, CrossCoreUs(sJobEndTick, sDispatchTick));
    SamplesAdd(&sJobStart, CrossCoreUs(sJobStartTick, sDispatchTick));
    sJobPending = 0;
}

static void MaybeDispatchJob(void) {
    if (sJobPending) {
        sJobBusySkips++;
        return;
    }
    sDispatchTick = svcGetSystemTick();
    sJobPending = 1;
    WorkerDispatch(CMD_JOB);
}

/* ---- N60CORE1: host-loop periods ---- */

static void LoopWindowReset(void) {
    sLoopN = 0;
    sLoopRep = 0;
    sLoopPrevTick = svcGetSystemTick();
    sUnderAtStart = gdx3ds_audio_underruns();
    sDropAtStart = ndspGetDroppedFrames();
}

/* One host-loop period (tick to tick of this hook = the whole iteration: logic, render,
 * present, pace). The probe's own samples ride in it equally in every phase. */
static void SampleLoop(void) {
    const uint64_t now = svcGetSystemTick();
    const uint32_t us = TicksToUs(now - sLoopPrevTick);
    sLoopPrevTick = now;
    if (us > C1P_LOOP_REP_US) {
        sLoopRep++;
    }
    if (sLoopUs != NULL && sLoopN < C1P_LOOP_SAMPLES) {
        sLoopUs[sLoopN++] = us;
    }
}

static void LoopStats(uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (sLoopUs == NULL || sLoopN == 0) {
        return;
    }
    qsort(sLoopUs, (size_t)sLoopN, sizeof(uint32_t), CompareU32);
    out[0] = sLoopUs[sLoopN / 2];
    out[1] = sLoopUs[(sLoopN * 9) / 10 < sLoopN ? (sLoopN * 9) / 10 : sLoopN - 1];
    out[2] = sLoopUs[sLoopN - 1];
}

/* ---- windows ---- */

static void ResetWindow(void) {
    sWindowTicks = 0;
    sJobBusySkips = 0;
    sJobPending = 0;
    sSkewed = 0;
    SamplesReset(&sJobWall);
    SamplesReset(&sJobStart);
    SamplesReset(&sGsp);
    SamplesReset(&sFsIpc);
    SamplesReset(&sFsRead);
    sAudioMin = (size_t)-1;
    LoopWindowReset();
}

static void BeginWindow(void) {
    ResetWindow();
    ApplyPct(PhasePct());
    if (sPhase == PH_JOB_LO || sPhase == PH_SPIN) {
        WorkerSetPriority(C1P_PRIO_LO);
    } else if (sPhase == PH_JOB_EQ) {
        WorkerSetPriority(C1P_PRIO_EQ);
    }
    if (sPhase == PH_SPIN && !sBusy) {
        sSpinStop = 0;
        WorkerDispatch(CMD_SPIN);
    }
}

static void StopSpinAndWait(void) {
    sSpinStop = 1;
    __dmb();
    WorkerWaitIdle();
}

static int IsJobPhase(void) {
    return sPhase == PH_JOB_LO || sPhase == PH_JOB_EQ;
}

static void EmitWindow(unsigned long frame) {
    uint32_t jw[3], js[3], g[3], fi[3], fr[3], lp[3];
    LoopStats(lp);
    SamplesStats(&sJobWall, jw);
    SamplesStats(&sJobStart, js);
    SamplesStats(&sGsp, g);
    SamplesStats(&sFsIpc, fi);
    SamplesStats(&sFsRead, fr);
    const double spinTotal = (double)(sSpinRun + sSpinGap);
    const int spin = sPhase == PH_SPIN;
    const double share = (spin && spinTotal > 0.0) ? 100.0 * (double)sSpinRun / spinTotal : 0.0;
    const double runs = (double)sSpinGaps + 1.0;
    C1pLog("[c1p] w=%lu frame=%lu pct=%d rc=0x%08lX got=%lu ph=%s | job n=%d skip=%u cpu0=%.0f "
           "wall=%lu/%lu/%lu start=%lu/%lu/%lu ratio=%.2f | spin share=%.1f%% runavg=%.0f gapavg=%.0f "
           "gapmax=%lu us | gsp4k=%lu/%lu/%lu n=%d fsipc=%lu/%lu/%lu n=%d fsrd=%lu/%lu/%lu n=%d | abufmin=%lu skew=%u"
           " | loop=%lu/%lu/%lu rep=%u/%d under=+%lu ndrop=+%lu ndsp=%d",
           sWindowNum, frame, sCurPct, (unsigned long)sCurRc, (unsigned long)sCurGot, kPhaseNames[sPhase],
           sJobWall.n, sJobBusySkips, (double)sJobCpu0Us, (unsigned long)jw[0], (unsigned long)jw[1],
           (unsigned long)jw[2], (unsigned long)js[0], (unsigned long)js[1], (unsigned long)js[2],
           (sJobCpu0Us > 0.0f && jw[0] != 0) ? (double)jw[0] / (double)sJobCpu0Us : 0.0, share,
           spin ? (double)TicksToUs(sSpinRun) / runs : 0.0,
           (spin && sSpinGaps != 0) ? (double)TicksToUs(sSpinGap) / (double)sSpinGaps : 0.0,
           spin ? (unsigned long)TicksToUs(sSpinGapMax) : 0ul, (unsigned long)g[0], (unsigned long)g[1],
           (unsigned long)g[2], sGsp.n, (unsigned long)fi[0], (unsigned long)fi[1], (unsigned long)fi[2],
           sFsIpc.n, (unsigned long)fr[0], (unsigned long)fr[1], (unsigned long)fr[2], sFsRead.n,
           sAudioMin == (size_t)-1 ? 0ul : (unsigned long)sAudioMin, sSkewed, (unsigned long)lp[0],
           (unsigned long)lp[1], (unsigned long)lp[2], sLoopRep, sLoopN,
           gdx3ds_audio_underruns() - sUnderAtStart, (unsigned long)(ndspGetDroppedFrames() - sDropAtStart),
           gdx3ds_audio_ndsp_core());
}

static void AdvancePhase(void) {
    if (sPhase == PH_COOL) {
        sPhase = PH_IDLE;
        sPctIndex = 0;
        return;
    }
    sPhase++;
    if (sPhase == PH_PER_PCT) {
        sPhase = PH_IDLE;
        sPctIndex++;
        if (sPctIndex >= sPctCount) {
            sPctIndex = 0;
            sPhase = PH_COOL;
        }
    }
}

static void EndWindow(unsigned long frame) {
    if (sPhase == PH_SPIN) {
        StopSpinAndWait();
    }
    if (IsJobPhase()) {
        WorkerWaitIdle();
        CollectJob();
    }
    EmitWindow(frame);
    sWindowNum++;
    AdvancePhase();
    BeginWindow();
}

static void SampleTick(void) {
    const int k = sWindowTicks;
    SampleLoop();
    SampleAudio();
    if (IsJobPhase()) {
        CollectJob();
        if (k % sJobEvery == 0) {
            MaybeDispatchJob();
        }
    }
    if (k % 64 == 48) {
        SampleFsRead();
    } else if (k % 32 == 16) {
        SampleFsIpc();
    } else if (k % 8 == 4) {
        SampleGsp();
    }
}

/* ---- the [core1] watch: one receipt per C1P_WATCH_EVERY ticks while a core-1 lever is set ---- */

static void WatchInit(void) {
    const int ndspLever = gdx3ds_config_get_int("debug", "ndsp_core1", 0);
    /* N60SYSCORE60: the watch arms whenever syscore_pct differs from THIS model's own default
     * (30 on New 3DS, 60 on Old 3DS) -- matching gdx3ds_audio_ndsp.c's SyscoreRequestPct(), not
     * a hardcoded literal, so an Old 3DS run on its new 60% default is correctly "nothing to
     * watch" (same as a New 3DS run on 30 always was), while an explicit ini override still arms
     * it either way. */
    const int defaultPct = gdx3ds_audio_syscore_default_pct();
    const int pct = gdx3ds_config_get_int("debug", "syscore_pct", defaultPct);
    if (ndspLever == 0 && pct == defaultPct) {
        return;
    }
    sWatch = 1;
    sWatchUnder = gdx3ds_audio_underruns();
    sWatchDrop = ndspGetDroppedFrames();
    C1pLog("[core1] watch on: ndsp_core1=%d (thread core %d) syscore_pct=%d granted=%d%% old3ds=%d hb=%d "
           "every=%u ticks",
           ndspLever, gdx3ds_audio_ndsp_core(), pct, gdx3ds_audio_syscore_limit_percent(), gdx3ds_is_old3ds(),
           envIsHomebrew() ? 1 : 0, C1P_WATCH_EVERY);
}

static void WatchTick(unsigned long frame) {
    const uint64_t now = svcGetSystemTick();
    if (sWatchPrevTick != 0) {
        const uint64_t dt = now - sWatchPrevTick;
        sWatchSumTicks += dt;
        if (dt > sWatchMaxTicks) {
            sWatchMaxTicks = dt;
        }
        if (TicksToUs(dt) > C1P_LOOP_REP_US) {
            sWatchRep++;
        }
        sWatchTicks++;
    }
    sWatchPrevTick = now;
    if (sWatchTicks < C1P_WATCH_EVERY) {
        return;
    }
    const unsigned long under = gdx3ds_audio_underruns();
    const uint32_t drop = ndspGetDroppedFrames();
    u32 got = 0;
    APT_GetAppCpuTimeLimit(&got);
    C1pLog("[core1] frame=%lu n=%lu loop avg=%.2f max=%.2f ms rep=%u | share=%lu%% ndsp core=%d | under=+%lu "
           "ndrop=+%lu",
           frame, sWatchTicks, (double)TicksToUs(sWatchSumTicks) / 1000.0 / (double)sWatchTicks,
           (double)TicksToUs(sWatchMaxTicks) / 1000.0, sWatchRep, (unsigned long)got, gdx3ds_audio_ndsp_core(),
           under - sWatchUnder, (unsigned long)(drop - sWatchDrop));
    sWatchUnder = under;
    sWatchDrop = drop;
    sWatchTicks = 0;
    sWatchSumTicks = 0;
    sWatchMaxTicks = 0;
    sWatchRep = 0;
}

/* ---- init ---- */

static void ParsePcts(const char* s) {
    int n = 0;
    while (s != NULL && *s != '\0' && n < C1P_MAX_PCTS) {
        char* end = NULL;
        const long v = strtol(s, &end, 10);
        if (end == s) {
            break;
        }
        if (v >= 5 && v <= 89) {
            sPcts[n++] = (int)v;
        }
        s = (*end == ',') ? end + 1 : end;
    }
    if (n > 0) {
        sPctCount = n;
    }
}

static void ReadConfig(void) {
    sWindow = gdx3ds_config_get_int("debug", "core1_probe_window", 240);
    sStartTick = (unsigned long)gdx3ds_config_get_int("debug", "core1_probe_start", 600);
    sJobUs = gdx3ds_config_get_int("debug", "core1_probe_job_us", 1000);
    sJobEvery = gdx3ds_config_get_int("debug", "core1_probe_job_every", 4);
    ParsePcts(gdx3ds_config_get_string("debug", "core1_probe_pcts", "30,50,80"));
    if (sWindow < 32) {
        sWindow = 32;
    }
    if (sJobEvery < 2) {
        sJobEvery = 2;
    }
    if (sJobUs < 50) {
        sJobUs = 50;
    }
}

static int AllocBuffers(void) {
    sJobSrc = (float*)memalign(32, C1P_JOB_VERTS * 4 * sizeof(float));
    sJobDst = (int16_t*)memalign(32, C1P_JOB_VERTS * 4 * sizeof(int16_t));
    sLoopUs = (uint32_t*)malloc(C1P_LOOP_SAMPLES * sizeof(uint32_t));
    return sJobSrc != NULL && sJobDst != NULL && sLoopUs != NULL;
}

static int StartWorker(void) {
    if (sGrantedPct <= 0) {
        ApplyPct(30); /* New 3DS / no audio rung: core 1 needs a share before a thread runs there */
    }
    LightEvent_Init(&sGo, RESET_ONESHOT);
    sWorker = threadCreate(WorkerMain, NULL, C1P_WORKER_STACK, C1P_PRIO_LO, 1, false);
    return sWorker != NULL;
}

static void EnsureInit(void) {
    if (sChecked || gdx3ds_config_loaded() == 0) {
        return;
    }
    sChecked = 1;
    if (gdx3ds_config_get_int("debug", "core1_probe", 0) == 0) {
        WatchInit();
        return;
    }
    if (gdx3ds_tickahead_mode() == GDX3DS_TICKAHEAD_CORE1) {
        /* N60TICK4: the game host already lives on core 1; the probe's spinner and share sweep
         * would starve it. Probe P is its own launch variant (n60tick4-progress.md section 4). */
        C1pLog("[c1p] init REFUSED: [debug] tick_ahead=1 (the host thread is on core 1)");
        WatchInit();
        return;
    }
    ReadConfig();
    sGrantedPct = gdx3ds_audio_syscore_limit_percent();
    sIsHomebrew = envIsHomebrew() ? 1 : 0;
    if (!AllocBuffers()) {
        C1pLog("[c1p] init REFUSED: no memory for the job buffers");
        return;
    }
    FillJobSource();
    CalibrateJob();
    if (!StartWorker()) {
        C1pLog("[c1p] init REFUSED: threadCreate on core 1 failed (granted=%d%%)", sGrantedPct);
        RestoreGrantedPct();
        return;
    }
    OpenFsFile();
    sArmed = 1;
    C1pLog("[c1p] init on=1 old3ds=%d hb=%d granted=%d%% pcts=%d,%d,%d,%d window=%d start=%lu job_us=%d "
           "reps=%d cpu0=%.0f us every=%d fsfile=%d prio lo=0x%X eq=0x%X (spin/job on core 1; "
           "gsp/fs from core 0)",
           gdx3ds_is_old3ds(), sIsHomebrew, sGrantedPct, sPcts[0], sPctCount > 1 ? sPcts[1] : 0,
           sPctCount > 2 ? sPcts[2] : 0, sPctCount > 3 ? sPcts[3] : 0, sWindow, sStartTick, sJobUs, sJobReps,
           (double)sJobCpu0Us, sJobEvery, sFsFile != 0, C1P_PRIO_LO, C1P_PRIO_EQ);
}

/* ---- public ---- */

void gdx3ds_core1probe_tick(unsigned long frame) {
    if (!sChecked) {
        EnsureInit();
    }
    if (sWatch) {
        WatchTick(frame);
        return;
    }
    if (!sArmed || sParked || frame < sStartTick) {
        return;
    }
    if (!sStarted) {
        sStarted = 1;
        sPhase = PH_IDLE;
        sPctIndex = 0;
        BeginWindow();
    }
    SampleTick();
    sWindowTicks++;
    if (sWindowTicks >= sWindow) {
        EndWindow(frame);
    }
}

void gdx3ds_core1probe_suspend(const char* phase) {
    if (!sArmed || sParked) {
        return;
    }
    sSpinStop = 1;
    __dmb();
    const int idle = WorkerWaitIdle();
    RestoreGrantedPct();
    sParked = 1;
    C1pLog("[c1p] park phase=%s worker_idle=%d share -> %d%% (window %lu %s pct=%d dropped)", phase, idle,
           sGrantedPct, sWindowNum, kPhaseNames[sPhase], sCurPct);
}

void gdx3ds_core1probe_resume(const char* phase) {
    if (!sArmed || !sParked) {
        return;
    }
    sParked = 0;
    C1pLog("[c1p] resume phase=%s restart window %lu %s", phase, sWindowNum, kPhaseNames[sPhase]);
    BeginWindow();
}

void gdx3ds_core1probe_shutdown(void) {
    if (!sArmed) {
        return;
    }
    sArmed = 0;
    sSpinStop = 1;
    __dmb();
    WorkerWaitIdle();
    sCmd = CMD_QUIT;
    __dmb();
    LightEvent_Signal(&sGo);
    if (R_SUCCEEDED(threadJoin(sWorker, 100000000ULL))) {
        threadFree(sWorker); /* a worker that did not exit keeps its stack: never free it running */
    }
    sWorker = NULL;
    RestoreGrantedPct();
    if (sFsFile != 0) {
        FSFILE_Close(sFsFile);
        sFsFile = 0;
    }
    C1pLog("[c1p] stop windows=%lu share -> %d%%", sWindowNum, sGrantedPct);
}
