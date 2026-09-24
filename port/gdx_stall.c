/* Main-thread load-stall census. See gdx_stall.h for the contract and the why. */
#include "gdx_stall.h"

#include <stddef.h>
#include <string.h>

#include "port_log.h"

/* gdx_host_ms_now(): the same monotonic millisecond clock the [seg-src]/[audio-load]
 * receipts already use, so a span timed here and a receipt printed there agree. */
double gdx_host_ms_now(void);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned long stall_thread_id(void) { return (unsigned long)GetCurrentThreadId(); }
#elif defined(GDX_PLATFORM_3DS)
#include <3ds.h>
/* On the 3DS every game fiber is a REAL libctru thread (port/3ds/os/gdx_fiber_3ds.c: "the 3DS
 * has no ucontext and no fiber primitive, so each GdxFiber is a real libctru thread parked on a
 * LightEvent"), and main_3ds.cpp runs the Main fiber inline from gdx_vi_tick(). A raw
 * svcGetThreadId compare therefore answers "which OS thread" when the question is "is this the
 * game's logical thread", and every load span the game opens -- seg_blob, fs, seg_decode,
 * venue_tex, tex_build -- landed in the undifferentiated off-thread total, leaving the table
 * able to print only gpu_wait and other (audit-2026-09-24 finding 4). n64_sched.c's logical-id
 * test is the one that knows about fibers; gdx3ds_mainprof.c and gdx3ds_renderthread.cpp answer
 * the identical question with it. It reads "host" before gdx_sched_init() records the id, which
 * is exactly the boot-thread behaviour the raw test wanted. */
extern int gdx_sched_on_host_thread(void);
#define GDX_STALL_LOGICAL_MAIN 1
static unsigned long stall_thread_id(void) {
    u32 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return (unsigned long)id;
}
#else
#include <pthread.h>
static unsigned long stall_thread_id(void) { return (unsigned long)(uintptr_t)pthread_self(); }
#endif

static const char* const kCauseNames[GDX_STALL_CAUSE_COUNT] = {
    "seg_blob", "seg_decode", "venue_tex", "tex_build", "audio_load",
    "fs",       "heap",       "gpu_wait",  "other",
};

typedef struct {
    unsigned int count;   /* spans over the threshold */
    unsigned int allN;    /* every span, threshold or not */
    double total;         /* self ms, every span */
    double worst;         /* worst single self ms */
    unsigned long worstFrame;
} GdxStallBucket;

#define GDX_STALL_MAX_DEPTH 12

static int sEnabled;
static double sSpanThreshold = 8.0;
static double sFrameThreshold = 50.0;
static unsigned long sMainThread;
static int sMainThreadSet;

/* Main-thread only: the ranked table is written exclusively from the frame thread. */
static GdxStallBucket sBuckets[GDX_STALL_CAUSE_COUNT];
/* Off-thread spans (the boot preload worker, the audio producer) are a different
 * question from a frame stall, so they are kept out of the table entirely and
 * summed here instead. Integer microseconds under an atomic add: several worker
 * threads can close a span at once, and a plain `double +=` would tear. */
static unsigned long long sOffThreadUs;
static unsigned int sOffThreadSpans;

/* Open-span stack. `childMs` accumulates the children's self time so a parent is
 * charged only for what it did itself. THREAD-LOCAL: the shim this instruments is
 * called from the game/main thread, the render thread, the boot preload worker and
 * the audio producer, and one shared stack would be corrupted by any two of them
 * overlapping. On Win32/ucontext fibers share their host thread's TLS; on the 3DS each fiber
 * is its own thread and so gets its own stack, which is equally correct here because a span
 * always begins and ends inside one fiber. Whether that stack's spans are MAIN-thread spans is
 * a separate question, answered by stall_on_main() above, not by which TLS block they sat in. */
static __thread struct {
    int cause;
    double t0;
    double childMs;
    int mine; /* 1 when opened on the main thread */
} sStack[GDX_STALL_MAX_DEPTH];
static __thread int sDepth;

/* Per-frame receipt budget. On the 3DS every gdx_port_logf line is an SD write, so an
 * unbounded `[stall] cause=` stream would itself manufacture the stalls this census exists
 * to find -- worst on the device, which is the only place the numbers are true. The table
 * still counts EVERY span; only the per-span lines are capped, and the frame line says how
 * many were dropped. */
#define GDX_STALL_RECEIPTS_PER_FRAME 4
static int sReceiptsThisFrame;
static unsigned int sReceiptsDropped;

/* Frame accounting. */
static double sFrameT0;
static double sFrameAttributed;
static unsigned long sFrameNumber;
static unsigned int sFrameOverruns;

void gdx_stall_configure(int enabled, int spanThresholdMs, int frameThresholdMs) {
    sEnabled = (enabled != 0);
    if (spanThresholdMs > 0) {
        sSpanThreshold = (double)spanThresholdMs;
    }
    if (frameThresholdMs > 0) {
        sFrameThreshold = (double)frameThresholdMs;
    }
    if (sEnabled) {
        /* Claim the main thread HERE, not at the frame loop: the boot preload worker starts
         * before the loop does, and with no main thread registered yet its multi-second
         * archive reads counted as main-thread stalls -- the exact confusion this census
         * exists to avoid. gdx_stall_configure runs on the boot thread, which IS the thread
         * that goes on to run the frame loop. */
        gdx_stall_set_main_thread();
        gdx_port_logf("[stall] armed span>=%dms frame>=%dms\n", (int)sSpanThreshold,
                      (int)sFrameThreshold);
    }
}

int gdx_stall_enabled(void) { return sEnabled; }

void gdx_stall_set_main_thread(void) {
    sMainThread = stall_thread_id();
    sMainThreadSet = 1;
}

static int stall_on_main(void) {
#ifdef GDX_STALL_LOGICAL_MAIN
    /* 3DS: the LOGICAL scheduler thread -- the host loop thread and every game fiber it
     * created -- not the raw OS thread. See the note beside stall_thread_id above. */
    return gdx_sched_on_host_thread() != 0;
#else
    /* Before the frame loop registers itself every thread counts as main: boot-time
     * spans all run on the boot thread, and that IS the thread that becomes main. */
    return (!sMainThreadSet) || (stall_thread_id() == sMainThread);
#endif
}

/* Charge `ms` of self time to `cause` and print a receipt when it crosses the span
 * threshold. Main-thread spans only; the caller has already classified the thread. */
static void stall_charge(int cause, double ms) {
    GdxStallBucket* b;
    if (cause < 0 || cause >= GDX_STALL_CAUSE_COUNT) {
        return;
    }
    if (ms < 0.0) {
        ms = 0.0; /* a clock that went backwards must not make a bucket negative */
    }
    b = &sBuckets[cause];
    b->allN++;
    b->total += ms;
    if (ms > b->worst) {
        b->worst = ms;
        b->worstFrame = sFrameNumber;
    }
    sFrameAttributed += ms;
    if (ms >= sSpanThreshold) {
        b->count++;
        if (sReceiptsThisFrame < GDX_STALL_RECEIPTS_PER_FRAME) {
            sReceiptsThisFrame++;
            gdx_port_logf("[stall] cause=%s ms=%.1f frame=%lu\n", kCauseNames[cause], ms,
                          sFrameNumber);
        } else {
            sReceiptsDropped++;
        }
    }
}

/* Off-thread span: advisory total only, never a table row. */
static void stall_charge_offthread(double ms) {
    if (ms < 0.0) {
        ms = 0.0;
    }
    __atomic_add_fetch(&sOffThreadUs, (unsigned long long)(ms * 1000.0), __ATOMIC_RELAXED);
    __atomic_add_fetch(&sOffThreadSpans, 1u, __ATOMIC_RELAXED);
}

void gdx_stall_begin(GdxStallCause cause) {
    if (!sEnabled) {
        return;
    }
    if (sDepth >= GDX_STALL_MAX_DEPTH) {
        sDepth++; /* count the overflow so the matching end still balances */
        return;
    }
    sStack[sDepth].cause = (int)cause;
    sStack[sDepth].t0 = gdx_host_ms_now();
    sStack[sDepth].childMs = 0.0;
    sStack[sDepth].mine = stall_on_main();
    sDepth++;
}

void gdx_stall_end(void) {
    double wall;
    double self;
    int idx;
    if (!sEnabled || sDepth <= 0) {
        return;
    }
    sDepth--;
    if (sDepth >= GDX_STALL_MAX_DEPTH) {
        return; /* overflow span: nothing was pushed */
    }
    idx = sDepth;
    wall = gdx_host_ms_now() - sStack[idx].t0;
    self = wall - sStack[idx].childMs;
    if (self < 0.0) {
        self = 0.0;
    }
    if (idx > 0) {
        /* The parent paid this wall time too; subtract it so its self time is its own. */
        sStack[idx - 1].childMs += wall;
    }
    if (sStack[idx].mine) {
        stall_charge(sStack[idx].cause, self);
    } else {
        stall_charge_offthread(self);
    }
}

void gdx_stall_note(GdxStallCause cause, double ms) {
    if (!sEnabled) {
        return;
    }
    if (sDepth > 0 && sDepth <= GDX_STALL_MAX_DEPTH) {
        sStack[sDepth - 1].childMs += ms;
    }
    if (stall_on_main()) {
        stall_charge((int)cause, ms);
    } else {
        stall_charge_offthread(ms);
    }
}

void gdx_stall_frame(unsigned long frameNumber) {
    double now;
    if (!sEnabled) {
        return;
    }
    now = gdx_host_ms_now();
    if (sFrameT0 > 0.0) {
        const double wall = now - sFrameT0;
        double unattributed = wall - sFrameAttributed;
        if (unattributed < 0.0) {
            unattributed = 0.0;
        }
        if (wall >= sFrameThreshold) {
            sFrameOverruns++;
            /* The remainder is charged to `other` only on an overrun frame: on an
             * ordinary frame it is just the frame's own work, not a stall. */
            sBuckets[GDX_STALL_OTHER].allN++;
            sBuckets[GDX_STALL_OTHER].total += unattributed;
            if (unattributed > sBuckets[GDX_STALL_OTHER].worst) {
                sBuckets[GDX_STALL_OTHER].worst = unattributed;
                sBuckets[GDX_STALL_OTHER].worstFrame = sFrameNumber;
            }
            gdx_port_logf("[stall] frame=%lu ms=%.1f attributed=%.1f other=%.1f dropped=%u\n",
                          sFrameNumber, wall, sFrameAttributed, unattributed, sReceiptsDropped);
        }
    }
    sFrameT0 = now;
    sFrameAttributed = 0.0;
    sFrameNumber = frameNumber;
    sReceiptsThisFrame = 0;
    sReceiptsDropped = 0;
}

void gdx_stall_report(const char* tag, int reset) {
    int order[GDX_STALL_CAUSE_COUNT];
    int i;
    int j;
    double grand = 0.0;
    if (!sEnabled) {
        return;
    }
    for (i = 0; i < GDX_STALL_CAUSE_COUNT; i++) {
        order[i] = i;
        grand += sBuckets[i].total;
    }
    /* Insertion sort by total ms, worst first: nine entries, no allocation. */
    for (i = 1; i < GDX_STALL_CAUSE_COUNT; i++) {
        const int key = order[i];
        for (j = i - 1; j >= 0 && sBuckets[order[j]].total < sBuckets[key].total; j--) {
            order[j + 1] = order[j];
        }
        order[j + 1] = key;
    }
    gdx_port_logf("[stall-table] %s frames_over=%u total_ms=%.1f offthread_ms=%.1f/%u\n",
                  (tag != NULL) ? tag : "window", sFrameOverruns, grand,
                  (double)__atomic_load_n(&sOffThreadUs, __ATOMIC_RELAXED) / 1000.0,
                  __atomic_load_n(&sOffThreadSpans, __ATOMIC_RELAXED));
    for (i = 0; i < GDX_STALL_CAUSE_COUNT; i++) {
        const GdxStallBucket* b = &sBuckets[order[i]];
        if (b->allN == 0u) {
            continue;
        }
        gdx_port_logf("[stall-table]   %-10s n=%u over=%u total=%.1f worst=%.1f@frame%lu\n",
                      kCauseNames[order[i]], b->allN, b->count, b->total, b->worst,
                      b->worstFrame);
    }
    if (reset) {
        memset(sBuckets, 0, sizeof(sBuckets));
        __atomic_store_n(&sOffThreadUs, 0ull, __ATOMIC_RELAXED);
        __atomic_store_n(&sOffThreadSpans, 0u, __ATOMIC_RELAXED);
        sFrameOverruns = 0u;
    }
}
