/* port/3ds/gdx3ds_spike.c — n60st3: the frame-spike attribution receipt. Contract in
 * include/gdx3ds_spike.h. Main thread only (the host loop); zero heap. */
#include "gdx3ds_spike.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_mainprof.h"
#include "gfx/gdx3ds_pace.h"

/* Weak: the right-eye and overlap modules live in the gfx library (always linked with the game,
 * not with every test harness). */
extern unsigned long long gdx3ds_stereo_frame_cpu_ticks(unsigned* chunks) __attribute__((weak));
extern unsigned long long gdx3ds_overlap_last_drain_ticks(int* kicks) __attribute__((weak));

#define SPIKE_PENDING 4
#define SPIKE_LINE_CAP 400
#define SPIKE_EMIT_GAP 8u
#define SPIKE_BUDGET 8u
#define SPIKE_BUDGET_WINDOW 256u
#define SPIKE_SUM_EVERY 1024u

enum { WHY_GATE = 1, WHY_QUEUE = 2, WHY_TAIL = 4, WHY_CPU = 8 };

/* One iteration's numbers, kept for the next one's line. */
typedef struct SpikeFrame {
    uint64_t wallTicks;
    uint64_t tailTicks;  /* the marked tail sub-sections */
    uint64_t drainTicks;
} SpikeFrame;

static int sChecked = 0;
static int sArmed = 0;
static uint64_t sThresholdTicks = 0;
static uint64_t sBeginTick = 0;
static uint64_t sMark[GDX_SPIKE_MARK_COUNT];
static SpikeFrame sPrev;

static char sPending[SPIKE_PENDING][SPIKE_LINE_CAP];
static int sPendingLen[SPIKE_PENDING];
static int sPendingHead = 0;
static int sPendingCount = 0;
static unsigned long sNextEmitFrame = 0;
static unsigned long sBudgetWindowStart = 0;
static unsigned sBudgetUsed = 0;
static unsigned long sSuppressed = 0;
static int sLastWasSpike = 0;

static unsigned long sSumFrames = 0;
static unsigned long sSumSpikes = 0;
static unsigned long sSumWhy[4];
static unsigned long sSumNone = 0;
static uint64_t sSumGtailMax = 0;

static void SpikeLog(const char* buf, int len) {
    svcOutputDebugString(buf, (size_t)len);
    gdx3ds_filelog_write(buf, (size_t)len);
}

static void EnsureInit(void) {
    if (sChecked || gdx3ds_config_loaded() == 0) {
        return;
    }
    sChecked = 1;
    const int ms = gdx3ds_config_get_int("debug", "spike_ms", 0);
    sArmed = ms > 0;
    if (!sArmed) {
        return;
    }
    sThresholdTicks = (uint64_t)ms * (uint64_t)CPU_TICKS_PER_MSEC;
    char line[200];
    const int n = snprintf(line, sizeof(line),
                           "[spike] armed (debug.spike_ms=%d): wall > %d ms; %d-line buffer, 1 line per %u "
                           "iterations, %u per %u; prev-frame begin/tail/GPU-tail attribution",
                           ms, ms, SPIKE_PENDING, SPIKE_EMIT_GAP, SPIKE_BUDGET, SPIKE_BUDGET_WINDOW);
    if (n > 0) {
        SpikeLog(line, n);
    }
}

void gdx3ds_spike_frame_begin(void) {
    EnsureInit();
    if (!sArmed) {
        return;
    }
    sBeginTick = svcGetSystemTick();
    memset(sMark, 0, sizeof(sMark));
}

void gdx3ds_spike_mark(int mark) {
    if (sArmed && mark >= 0 && mark < GDX_SPIKE_MARK_COUNT) {
        sMark[mark] = svcGetSystemTick();
    }
}

/* ---- the line ---- */

typedef struct LineBuf {
    char* p;
    int len;
    int cap;
} LineBuf;

static void Put(LineBuf* b, const char* fmt, ...) {
    va_list ap;
    int n;
    if (b->len >= b->cap - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += n < b->cap - b->len ? n : b->cap - b->len - 1;
    }
}

static unsigned MsX100(uint64_t ticks) {
    return (unsigned)((ticks * 100u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

static void PutMs(LineBuf* b, const char* name, uint64_t ticks) {
    const unsigned x = MsX100(ticks);
    Put(b, " %s=%u.%02u", name, x / 100u, x % 100u);
}

static uint64_t Span(int from, int to) {
    if (sMark[from] == 0 || sMark[to] < sMark[from]) {
        return 0;
    }
    return sMark[to] - sMark[from];
}

static uint64_t SpanToEnd(int from, uint64_t endTick) {
    if (sMark[from] == 0 || endTick < sMark[from]) {
        return 0;
    }
    return endTick - sMark[from];
}

/* The marked tail of this iteration: the save tick, then everything from the post-frame tail to
 * the end of the loop body. */
static uint64_t TailTicks(uint64_t endTick) {
    return Span(GDX_SPIKE_MARK_SAVE0, GDX_SPIKE_MARK_SAVE1) + SpanToEnd(GDX_SPIKE_MARK_POST0, endTick);
}

static void MprofAptInp(uint64_t* apt, uint64_t* inp) {
    uint64_t t[GDX3DS_MPROF_COUNT];
    *apt = 0;
    *inp = 0;
    if (gdx3ds_mprof_snapshot(t, GDX3DS_MPROF_COUNT) != 0) {
        *apt = t[GDX3DS_MPROF_APT];
        *inp = t[GDX3DS_MPROF_INP];
    }
}

static int Classify(uint64_t wall, const GdxPaceFrameInfo* pace, uint64_t tl) {
    const uint64_t ms1 = (uint64_t)CPU_TICKS_PER_MSEC;
    const uint64_t waits = pace->queueTicks + pace->vblankTicks + tl;
    int why = 0;
    if (pace->vblankTicks >= 8u * ms1) {
        why |= WHY_GATE;
    }
    if (pace->queueTicks >= 2u * ms1) {
        why |= WHY_QUEUE;
    }
    if (tl >= 3u * ms1) {
        why |= WHY_TAIL;
    }
    if (wall > waits && wall - waits >= (167u * ms1) / 10u) {
        why |= WHY_CPU;
    }
    return why;
}

static void PutWhy(LineBuf* b, int why) {
    static const char* const kNames[4] = { "gate", "queue", "tail", "cpu" };
    int first = 1;
    Put(b, " | why=");
    for (int i = 0; i < 4; i++) {
        if (why & (1 << i)) {
            Put(b, "%s%s", first ? "" : "+", kNames[i]);
            first = 0;
        }
    }
    if (first) {
        Put(b, "none");
    }
}

static void PutBegin(LineBuf* b, const GdxPaceFrameInfo* pace) {
    Put(b, " | beg");
    PutMs(b, "q", pace->queueTicks);
    PutMs(b, "v", pace->vblankTicks);
    Put(b, " ph=%d", pace->phase);
}

static void PutPrevious(LineBuf* b, const GdxPaceFrameInfo* pace) {
    Put(b, " | prev");
    PutMs(b, "ms", sPrev.wallTicks);
    PutMs(b, "rend", pace->prevRenderTicks);
    PutMs(b, "eg", pace->prevEndGuardTicks);
    PutMs(b, "gtail", pace->prevGpuTailTicks);
    Put(b, " gap=%lu", (unsigned long)pace->swapGap);
    PutMs(b, "dr", sPrev.drainTicks);
    PutMs(b, "tl", sPrev.tailTicks);
}

static void PutTail(LineBuf* b, uint64_t endTick, uint64_t tl) {
    uint64_t apt = 0;
    uint64_t inp = 0;
    MprofAptInp(&apt, &inp);
    Put(b, " |");
    PutMs(b, "tl", tl);
    PutMs(b, "apt", apt);
    PutMs(b, "inp", inp);
    PutMs(b, "save", Span(GDX_SPIKE_MARK_SAVE0, GDX_SPIKE_MARK_SAVE1));
    PutMs(b, "rpl", Span(GDX_SPIKE_MARK_POST0, GDX_SPIKE_MARK_POST1));
    PutMs(b, "tele", Span(GDX_SPIKE_MARK_POST1, GDX_SPIKE_MARK_TELE1));
    PutMs(b, "hud", Span(GDX_SPIKE_MARK_TELE1, GDX_SPIKE_MARK_HUD1));
    PutMs(b, "end", SpanToEnd(GDX_SPIKE_MARK_HUD1, endTick));
}

static void PutStereo(LineBuf* b, uint64_t drain) {
    unsigned chunks = 0;
    const uint64_t st = &gdx3ds_stereo_frame_cpu_ticks != NULL ? gdx3ds_stereo_frame_cpu_ticks(&chunks) : 0;
    Put(b, " | st_us=%lu ch=%u", (unsigned long)(st / (uint64_t)CPU_TICKS_PER_USEC), chunks);
    PutMs(b, "dr", drain);
}

static void BufferSpike(unsigned long frame, uint64_t wall, uint64_t endTick, uint64_t tl, uint64_t drain,
                        const GdxPaceFrameInfo* pace, int why) {
    LineBuf b;
    if (sPendingCount >= SPIKE_PENDING) {
        sSuppressed++;
        return;
    }
    const int slot = (sPendingHead + sPendingCount) % SPIKE_PENDING;
    b.p = sPending[slot];
    b.len = 0;
    b.cap = SPIKE_LINE_CAP;
    Put(&b, "[spike] f=%lu", frame);
    PutMs(&b, "ms", wall);
    PutBegin(&b, pace);
    PutPrevious(&b, pace);
    PutTail(&b, endTick, tl);
    PutStereo(&b, drain);
    PutWhy(&b, why);
    sPendingLen[slot] = b.len;
    sPendingCount++;
}

static void CountSpike(int why, const GdxPaceFrameInfo* pace) {
    sSumSpikes++;
    for (int i = 0; i < 4; i++) {
        if (why & (1 << i)) {
            sSumWhy[i]++;
        }
    }
    if (why == 0) {
        sSumNone++;
    }
    if (pace->prevGpuTailTicks > sSumGtailMax) {
        sSumGtailMax = pace->prevGpuTailTicks;
    }
}

static uint64_t LastDrainTicks(void) {
    int kicks = 0;
    return &gdx3ds_overlap_last_drain_ticks != NULL ? gdx3ds_overlap_last_drain_ticks(&kicks) : 0;
}

void gdx3ds_spike_frame_end(unsigned long frame) {
    if (!sArmed || sBeginTick == 0) {
        return;
    }
    const uint64_t endTick = svcGetSystemTick();
    const uint64_t wall = endTick - sBeginTick;
    const uint64_t tl = TailTicks(endTick);
    const uint64_t drain = LastDrainTicks();
    sSumFrames++;
    sLastWasSpike = wall > sThresholdTicks;
    if (sLastWasSpike) {
        GdxPaceFrameInfo pace;
        gdx3ds_pace_frame_info(&pace);
        const int why = Classify(wall, &pace, tl);
        CountSpike(why, &pace);
        BufferSpike(frame, wall, endTick, tl, drain, &pace, why);
    }
    sPrev.wallTicks = wall;
    sPrev.tailTicks = tl;
    sPrev.drainTicks = drain;
}

static int BudgetAllows(unsigned long frame) {
    if (frame - sBudgetWindowStart >= SPIKE_BUDGET_WINDOW) {
        sBudgetWindowStart = frame;
        sBudgetUsed = 0;
    }
    return sBudgetUsed < SPIKE_BUDGET && frame >= sNextEmitFrame;
}

static void EmitPending(unsigned long frame) {
    if (sPendingCount == 0 || !BudgetAllows(frame)) {
        return;
    }
    char* line = sPending[sPendingHead];
    int len = sPendingLen[sPendingHead];
    if (sSuppressed != 0 && len < SPIKE_LINE_CAP - 24) {
        len += snprintf(line + len, (size_t)(SPIKE_LINE_CAP - len), " supp=%lu", sSuppressed);
        sSuppressed = 0;
    }
    SpikeLog(line, len);
    sPendingHead = (sPendingHead + 1) % SPIKE_PENDING;
    sPendingCount--;
    sBudgetUsed++;
    sNextEmitFrame = frame + SPIKE_EMIT_GAP;
}

static void EmitSummary(unsigned long frame) {
    char line[224];
    const unsigned g = MsX100(sSumGtailMax);
    const int n = snprintf(line, sizeof(line),
                           "[spike] sum f=%lu n=%lu spikes=%lu gate=%lu queue=%lu tail=%lu cpu=%lu none=%lu "
                           "gtail_max=%u.%02u supp=%lu",
                           frame, sSumFrames, sSumSpikes, sSumWhy[0], sSumWhy[1], sSumWhy[2], sSumWhy[3], sSumNone,
                           g / 100u, g % 100u, sSuppressed);
    if (n > 0) {
        SpikeLog(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
    sSumFrames = 0;
    sSumSpikes = 0;
    memset(sSumWhy, 0, sizeof(sSumWhy));
    sSumNone = 0;
    sSumGtailMax = 0;
}

void gdx3ds_spike_after_frame(unsigned long frame) {
    if (!sArmed) {
        return;
    }
    if (!sLastWasSpike) {
        EmitPending(frame);
    }
    if (sSumFrames >= SPIKE_SUM_EVERY) {
        EmitSummary(frame);
    }
}
