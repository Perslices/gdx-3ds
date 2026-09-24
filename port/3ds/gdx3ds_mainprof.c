/* port/3ds/gdx3ds_mainprof.c — G-MAINPROF implementation. The contract, the bucket
 * semantics and the receipt format are all in gdx3ds_mainprof.h; read that first. */
#include "gdx3ds_mainprof.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

/* Logical-main test (host thread or any game fiber it created). port/n64_sched.c. */
extern int gdx_sched_on_host_thread(void);
/* draws/frame of the last emitted [prof] window (port/3ds/gfx/gdx3ds_gpu_prof.c). 0 when
 * `[debug] gputrace=0`, in which case the dl= column is the load proxy instead. Read
 * racily from main; it is a scene descriptor, never a control input. */
extern unsigned gdx3ds_prof_last_nd;

#define GDX3DS_MPROF_WINDOW 64

int gdx3ds_mprof_active = 0;
int gdx3ds_mprof_cur = GDX3DS_MPROF_OTH;
uint64_t gdx3ds_mprof_t0 = 0;
uint64_t gdx3ds_mprof_ticks[GDX3DS_MPROF_COUNT];

static int sChecked = 0;
static int sEnabled = 0;
static uint64_t sWinTicks[GDX3DS_MPROF_COUNT];
static unsigned sWinFrames = 0;
static unsigned long sWinDlBytes = 0;
static unsigned long sWinTasks = 0;

long long gdx3ds_mprof_now(void) {
    return (long long)svcGetSystemTick();
}

int gdx3ds_mprof_on_main(void) {
    return gdx_sched_on_host_thread();
}

int gdx3ds_mprof_enabled(void) {
    return sEnabled;
}

/* svc + filelog, exactly like GpuProfLog: retail hardware DISCARDS
 * svcOutputDebugString, so the filelog is the only sink a hardware run can show. */
static void MProfLog(const char* fmt, ...) {
    char buf[224];
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

static float TicksToMs(uint64_t dt) {
    return (float)((double)dt / CPU_TICKS_PER_MSEC);
}

/* Decomp frame-function checkpoint. One switch over a small dense id; the gate is tested
 * inside gdx3ds_mprof_enter, so an unarmed run pays a call, a load and a branch. */
void gdx3ds_mprof_ck(int step) {
    int bucket;
    if (!gdx3ds_mprof_active) {
        return;
    }
    switch (step) {
    case GDX3DS_MPROF_CK_AUDIO:   bucket = GDX3DS_MPROF_AUD; break;
    case GDX3DS_MPROF_CK_LOGIC:   bucket = GDX3DS_MPROF_LOGIC; break;
    case GDX3DS_MPROF_CK_LSEG:    bucket = GDX3DS_MPROF_LSEG; break;
    case GDX3DS_MPROF_CK_DLBUILD: bucket = GDX3DS_MPROF_DLB; break;
    /* Gfx_FullSync + the DP-done park: the park returns to the host, which re-opens its
     * own bucket at once, so this only ever charges the FullSync itself. Folded into
     * stask rather than given a column nobody would read. */
    case GDX3DS_MPROF_CK_DPWAIT:  bucket = GDX3DS_MPROF_STASK; break;
    case GDX3DS_MPROF_CK_SEGLOAD: bucket = GDX3DS_MPROF_SLA; break;
    /* The framebuffer spin yields to the host on its first miss (n64_sched.c gdx_yield),
     * so like the DP park it charges only the first re-check. */
    case GDX3DS_MPROF_CK_FBSPIN:  bucket = GDX3DS_MPROF_SLA; break;
    case GDX3DS_MPROF_CK_SETTASK: bucket = GDX3DS_MPROF_STASK; break;
    case GDX3DS_MPROF_CK_IDLE:
    default:                      bucket = GDX3DS_MPROF_OTH; break;
    }
    (void)gdx3ds_mprof_enter(bucket);
}

void gdx3ds_mprof_note_task(unsigned long dlBytes) {
    if (!gdx3ds_mprof_active) {
        return;
    }
    sWinDlBytes += dlBytes;
    sWinTasks++;
}

/* Latch the ini gate once, after the config is loaded (same boot-order-proof shape as
 * GpuProfEnsureInit). mainprof is its own key: a main-thread profile must be runnable
 * with gputrace off, and a gputrace run must not silently also pay for this one. */
static void MProfEnsureInit(void) {
    if (sChecked) {
        return;
    }
    if (gdx3ds_config_loaded() == 0) {
        return; /* config not in yet — latch at a later frame, never as 0 by accident */
    }
    sChecked = 1;
    sEnabled = gdx3ds_config_get_int("debug", "mainprof", 0) != 0;
    if (sEnabled) {
        MProfLog("[mprof] main-thread profiling enabled (window=%d frames; diagnostic only)",
                 GDX3DS_MPROF_WINDOW);
    }
}

void gdx3ds_mprof_frame_begin(void) {
    MProfEnsureInit();
    if (!sEnabled) {
        return;
    }
    /* Armed between frames only, so gdx3ds_mprof_cur can never flip mid-bucket. */
    if (!gdx3ds_mprof_active) {
        gdx3ds_mprof_active = 1;
        memset(gdx3ds_mprof_ticks, 0, sizeof(gdx3ds_mprof_ticks));
        gdx3ds_mprof_cur = GDX3DS_MPROF_OTH;
    }
    gdx3ds_mprof_t0 = (uint64_t)gdx3ds_mprof_now();
    gdx3ds_mprof_cur = GDX3DS_MPROF_APT; /* the loop's first real section */
}

static void MProfEmit(unsigned long frame, unsigned n) {
    const float inv = 1.0f / (float)n;
    float ms[GDX3DS_MPROF_COUNT];
    float fib = 0.0f;
    float host = 0.0f;
    int i;
    for (i = 0; i < GDX3DS_MPROF_COUNT; i++) {
        ms[i] = TicksToMs(sWinTicks[i]) * inv;
        if (i < GDX3DS_MPROF_FIB_END) {
            fib += ms[i];
        } else if (i != GDX3DS_MPROF_OTH) {
            host += ms[i];
        }
    }
    MProfLog("[mprof] frame=%lu n=%u part=1 nD=%u dl=%.1f tot=%.2f fib=%.2f host=%.2f | "
             "logic=%.2f dlb=%.2f brm=%.2f aud=%.2f lseg=%.2f sla=%.2f seg=%.2f dma=%.2f "
             "stask=%.2f sub=%.2f",
             frame, n, gdx3ds_prof_last_nd,
             (double)((float)sWinDlBytes * inv / 1024.0f),
             (double)(fib + host + ms[GDX3DS_MPROF_OTH]), (double)fib, (double)host,
             (double)ms[GDX3DS_MPROF_LOGIC], (double)ms[GDX3DS_MPROF_DLB],
             (double)ms[GDX3DS_MPROF_BRM], (double)ms[GDX3DS_MPROF_AUD],
             (double)ms[GDX3DS_MPROF_LSEG], (double)ms[GDX3DS_MPROF_SLA],
             (double)ms[GDX3DS_MPROF_SEG], (double)ms[GDX3DS_MPROF_DMA],
             (double)ms[GDX3DS_MPROF_STASK], (double)ms[GDX3DS_MPROF_SUB]);
    MProfLog("[mprof] frame=%lu n=%u part=2 tasks=%lu | apt=%.2f pace=%.2f inp=%.2f "
             "beg=%.2f vi=%.2f disp=%.2f jdp=%.2f jtop=%.2f end=%.2f tail=%.2f oth=%.2f",
             frame, n, sWinTasks,
             (double)ms[GDX3DS_MPROF_APT], (double)ms[GDX3DS_MPROF_PACE],
             (double)ms[GDX3DS_MPROF_INP], (double)ms[GDX3DS_MPROF_BEG],
             (double)ms[GDX3DS_MPROF_VI], (double)ms[GDX3DS_MPROF_DISP],
             (double)ms[GDX3DS_MPROF_JDP], (double)ms[GDX3DS_MPROF_JTOP],
             (double)ms[GDX3DS_MPROF_END], (double)ms[GDX3DS_MPROF_TAIL],
             (double)ms[GDX3DS_MPROF_OTH]);
}

void gdx3ds_mprof_frame_end(unsigned long frame) {
    int i;
    if (!gdx3ds_mprof_active) {
        return;
    }
    /* Close the last open bucket of this frame, then fold. Nothing is timed between this
     * call and the next frame_begin, which is exactly where the receipt is emitted — so
     * the emit's own cost is outside every bucket and cannot inflate the profile. */
    (void)gdx3ds_mprof_enter(GDX3DS_MPROF_OTH);
    for (i = 0; i < GDX3DS_MPROF_COUNT; i++) {
        sWinTicks[i] += gdx3ds_mprof_ticks[i];
        gdx3ds_mprof_ticks[i] = 0;
    }
    sWinFrames++;
    if (sWinFrames < GDX3DS_MPROF_WINDOW) {
        return;
    }
    MProfEmit(frame, sWinFrames);
    memset(sWinTicks, 0, sizeof(sWinTicks));
    sWinFrames = 0;
    sWinDlBytes = 0;
    sWinTasks = 0;
}
