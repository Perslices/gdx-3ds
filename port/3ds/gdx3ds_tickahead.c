/* port/3ds/gdx3ds_tickahead.c -- N60TICK4: tick-ahead on the Old 3DS. Contract: gdx3ds_tickahead.h;
 * design and gates: docs/research/native/n60tick4-progress.md.
 *
 * This file only decides WHERE things run. The overlap itself is the render thread's pipe mode
 * (gdx3ds_renderthread.cpp), unchanged: osSpTaskStartGo hands list N to the render thread and
 * posts SP-done, the game runs tick N+1 and parks on DP-done(N), the host loop joins render(N)
 * and posts it. What moves is the host: on the Old 3DS the render thread is created on core 0
 * and the host (with every game fiber it creates) on core 1. */
#include "gdx3ds_tickahead.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_audio.h"
#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

extern int gdx3ds_audio_grant_syscore(void);           /* gdx3ds_audio_ndsp.c (idempotent) */
extern void gdx3ds_rt_emit_receipt(unsigned long frame); /* gdx3ds_renderthread.cpp */

#define TA_HOST_STACK (256u * 1024u) /* = the rsf StackSize of the main thread it replaces */
#define TA_DEFAULT_MIN_PCT 50
#define TA_DEFAULT_EVERY 512
#define TA_HOST_CORE 1
#define TA_RENDER_CORE 0

static int sMode = GDX3DS_TICKAHEAD_OFF;
static int sBooted = 0;
static int sEvery = TA_DEFAULT_EVERY;
static int sSharePct = 0;

/* Host-thread window (written by the host thread only). */
static uint64_t sWinStart = 0;
static uint64_t sPrevTick = 0;
static uint64_t sMaxPeriod = 0;
static uint32_t sWinFrames = 0;
static uint32_t sWinRep = 0;

typedef struct HostCall {
    int (*fn)(void*);
    void* arg;
    int result;
} HostCall;

/* APT proxy (mode 1): the host posts sAptReq and waits on sAptDone; the main thread runs
 * aptMainLoop() and answers. One requester (the host thread), one server (main): each event has
 * a single waiter and a single signaller. sHostDone (set by the host thread when hostMain
 * returns, then sAptReq signalled) ends the server loop. */
static LightEvent sAptReq;
static LightEvent sAptDone;
static volatile int sAptResult = 1;
static volatile int sHostDone = 0;
static uint64_t sAptWinTicks = 0; /* host: pump round trips in the window */
static uint64_t sAptWinMax = 0;

static void TaLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void TaLog(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
    svcOutputDebugString(buf, (s32)len);
    gdx3ds_filelog_write(buf, len);
}

/* svcGetProcessorID returns 0..3 on the console; Azahar returns a non-core word. */
static int CurrentCore(void) {
    const s32 id = svcGetProcessorID();
    return (id >= 0 && id <= 3) ? (int)id : -1;
}

static u32 ShareReadback(void) {
    u32 pct = 0;
    if (R_FAILED(APT_GetAppCpuTimeLimit(&pct))) {
        return 0;
    }
    return pct;
}

int gdx3ds_tickahead_boot(void) {
    if (sBooted) {
        return sMode;
    }
    sBooted = 1;
    const int want = gdx3ds_config_get_int("debug", "tick_ahead", 0);
    if (want != GDX3DS_TICKAHEAD_CORE1 && want != GDX3DS_TICKAHEAD_PIPE0) {
        return sMode; /* off: silent */
    }
    sEvery = gdx3ds_config_get_int("debug", "tick_ahead_every", TA_DEFAULT_EVERY);
    if (!gdx3ds_is_old3ds()) {
        TaLog("[tickahead] REFUSED tick_ahead=%d: New 3DS (the core-2 render thread already overlaps "
              "render and logic there) -> ordinary path", want);
        return sMode;
    }
    if (want == GDX3DS_TICKAHEAD_PIPE0) {
        sMode = GDX3DS_TICKAHEAD_PIPE0;
        TaLog("[tickahead] init mode=2 (pipe protocol, host on core 0 = the main thread; render thread "
              "core %d) -- bisect lever, no overlap", TA_RENDER_CORE);
        return sMode;
    }
    int minPct = gdx3ds_config_get_int("debug", "tick_ahead_min_pct", TA_DEFAULT_MIN_PCT);
    if (minPct < 5) {
        minPct = 5;
    }
    const int granted = gdx3ds_audio_grant_syscore();
    sSharePct = (int)ShareReadback();
    if (!granted || sSharePct < minPct) {
        TaLog("[tickahead] REFUSED tick_ahead=1: core-1 share %d%% < tick_ahead_min_pct=%d (granted=%d, "
              "syscore_pct asks %d; a CIA above 30 needs -DGDX_3DS_CIA_MAXCPU) -> ordinary path",
              sSharePct, minPct, granted, gdx3ds_audio_syscore_limit_percent());
        return sMode;
    }
    sMode = GDX3DS_TICKAHEAD_CORE1;
    TaLog("[tickahead] init mode=1 host core %d (share %d%%, min %d) render core %d every=%d", TA_HOST_CORE,
          sSharePct, minPct, TA_RENDER_CORE, sEvery);
    return sMode;
}

int gdx3ds_tickahead_mode(void) {
    return sMode;
}

int gdx3ds_tickahead_render_core(void) {
    return sMode != GDX3DS_TICKAHEAD_OFF ? TA_RENDER_CORE : -1;
}

static void HostEntry(void* p) {
    HostCall* call = (HostCall*)p;
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    TaLog("[tickahead] host thread running on core %d prio 0x%02lX (share readback %lu%%)", CurrentCore(),
          (unsigned long)prio, (unsigned long)ShareReadback());
    call->result = call->fn(call->arg);
    sHostDone = 1;
    __dmb();
    LightEvent_Signal(&sAptReq); /* release the main thread's pump loop */
}

/* Main thread, mode 1: serve the host's APT pump requests until the host returns. */
static void ServeAptPump(void) {
    for (;;) {
        LightEvent_Wait(&sAptReq);
        __dmb();
        if (sHostDone) {
            return;
        }
        sAptResult = aptMainLoop() ? 1 : 0;
        __dmb();
        LightEvent_Signal(&sAptDone);
    }
}

int gdx3ds_tickahead_apt_main_loop(void) {
    if (sMode != GDX3DS_TICKAHEAD_CORE1) {
        return aptMainLoop() ? 1 : 0;
    }
    const uint64_t t0 = svcGetSystemTick();
    LightEvent_Signal(&sAptReq);
    LightEvent_Wait(&sAptDone);
    __dmb();
    const uint64_t dt = svcGetSystemTick() - t0;
    sAptWinTicks += dt;
    if (dt > sAptWinMax) {
        sAptWinMax = dt;
    }
    return sAptResult;
}

int gdx3ds_tickahead_run_host(int (*hostMain)(void* arg), void* arg) {
    if (sMode != GDX3DS_TICKAHEAD_CORE1) {
        return hostMain(arg);
    }
    static HostCall sCall;
    sCall.fn = hostMain;
    sCall.arg = arg;
    sCall.result = 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE); /* the main thread's: the fibers keep prio + 1 */
    LightEvent_Init(&sAptReq, RESET_ONESHOT);
    LightEvent_Init(&sAptDone, RESET_ONESHOT);
    Thread t = threadCreate(HostEntry, &sCall, TA_HOST_STACK, prio, TA_HOST_CORE, false);
    if (t == NULL) {
        sMode = GDX3DS_TICKAHEAD_OFF;
        TaLog("[tickahead] FALLBACK: threadCreate(core %d) failed -> host stays on the main thread, "
              "ordinary path", TA_HOST_CORE);
        return hostMain(arg);
    }
    /* The main thread now only serves the APT pump until the host returns (teardown included);
     * between pumps it is blocked and core 0 belongs to the render thread. */
    ServeAptPump();
    threadJoin(t, U64_MAX);
    threadFree(t);
    return sCall.result;
}

void gdx3ds_tickahead_frame_end(unsigned long frame) {
    if (sMode == GDX3DS_TICKAHEAD_OFF) {
        return;
    }
    const uint64_t now = svcGetSystemTick();
    if (sPrevTick != 0) {
        const uint64_t period = now - sPrevTick;
        if (period > sMaxPeriod) {
            sMaxPeriod = period;
        }
        if (period > (uint64_t)(CPU_TICKS_PER_MSEC * 20.0)) {
            sWinRep++;
        }
    } else {
        sWinStart = now;
    }
    sPrevTick = now;
    sWinFrames++;
    if (sEvery <= 0 || sWinFrames < (uint32_t)sEvery) {
        return;
    }
    const double k = CPU_TICKS_PER_MSEC;
    const double avg = (double)(now - sWinStart) / k / (double)sWinFrames;
    TaLog("[tickahead] frame=%lu mode=%d n=%u loop avg=%.2f max=%.2f rep=%u apt avg=%.3f max=%.3f host core=%d "
          "share=%lu%%",
          frame, sMode, (unsigned)sWinFrames, avg, (double)sMaxPeriod / k, (unsigned)sWinRep,
          (double)sAptWinTicks / k / (double)sWinFrames, (double)sAptWinMax / k, CurrentCore(),
          (unsigned long)ShareReadback());
    sAptWinTicks = 0;
    sAptWinMax = 0;
    gdx3ds_rt_emit_receipt(frame); /* [rt]: waitMain (host joins), waitRender (render idle), ovl ... */
    sWinStart = now;
    sWinFrames = 0;
    sWinRep = 0;
    sMaxPeriod = 0;
}
