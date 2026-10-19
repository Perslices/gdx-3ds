/* port/3ds/gdx3ds_frameskip.c — FRAMESKIP 3DS glue: lever, clock, task drop, receipts.
 * Contract in gdx3ds_frameskip.h; the decision itself is gdx3ds_frameskip_gov.c. */
#include "gdx3ds_frameskip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_hitch.h"
#include "gdx3ds_mainprof.h"
#include "gdx3ds_oracle.h"
#include "gfx/gdx3ds_replay.h" /* C6: the skipped tick is presented at the next slot's first vblank */
#include "gdx3ds_c5tick.h"

#define GDX_FRAMESKIP_FIXED_MAX 9
/* 5: the hardware GP race renders at 67.8 ms and a skipped tick costs ~4.3 ms ([mprof] logic +
 * dlb + seg + host on the 0929 log), so reaching 59.94 needs (67.8 + 4.3k)/(k+1) <= 16.68, i.e.
 * k >= 4.4. The governor only uses as many as the scene needs. */
#define GDX_FRAMESKIP_AUTO_MAX_DEFAULT 5
#define GDX_FRAMESKIP_WINDOW 256u

/* N64 NTSC field rate, 60/1.001 Hz, in ARM11 system ticks (~4,472,806). */
#define GDX_FRAMESKIP_INTERVAL_TICKS (((uint64_t)SYSCLOCK_ARM11 * 1001u) / 60000u)

/* One top-LCD refresh (59.8337 Hz) in ARM11 system ticks (~4,480,915). */
#define GDX_STEADY_PERIOD_TICKS ((uint64_t)((double)SYSCLOCK_ARM11 / 59.8337))
/* A vblank target this close (or closer) counts as reached: waiting would cost a whole period. */
#define GDX_STEADY_GUARD_TICKS ((uint64_t)CPU_TICKS_PER_MSEC)
/* A present issued later than this before the slot's display vblank cannot land on it: the
 * GPU's draw + display transfer run ~3-4 ms after C3D_FrameEnd ([gpu] gpu= on hardware). */
#define GDX_STEADY_PRESENT_MARGIN_TICKS ((uint64_t)CPU_TICKS_PER_MSEC * 4u)

extern void gdx3ds_gpuprof_set_external_pacing(int on); /* gfx/gdx3ds_gpu_prof.h */

static GdxFrameskipGov sGov;
static int sMode = GDX_FRAMESKIP_OFF;

/* STEADY state (main thread only). */
static GdxSteadyCtl sSteady;
static GdxFrameskipSlot sSlot;
static unsigned long sWinCatchup = 0;
static int sSteadyAlign = 1;       /* [debug] steady_align: wait for the slot's start vblank */
static int sSteadyHold = 1;        /* [debug] steady_hold: hold the present to S+D-1 */
static unsigned long sWinMissed = 0;
static unsigned long sWinDChanges = 0;
static uint64_t sWinRwMax = 0;
static int sLastSlotMissed = 0; /* HITCH: the latest scored slot missed its vblank */
static uint64_t sSlotSkipMax = 0; /* HITCH: the worst skip tick since the last paced render */
static unsigned long sWinHardMissed = 0;
static unsigned long sWinSpikes = 0;
static int sSteadyLastHard = 0;
static int sEnabled = 0;
static volatile int sDropTasks = 0; /* armed for exactly one SKIP iteration, main thread */
static int sRtMode = 0;
static int sFixedSkip = 0;          /* FIXED's N, latched from the ini (FIXED is ini-only) */
static int sPendingMode = -1;       /* DBG-tab request, applied at the next iteration (-1 = none) */
static unsigned long sSkippedTotal = 0;
static unsigned long sHollowForced = 0; /* C5TICK: renders turned into skips for a hollow list */

/* Window counters (main thread only). */
static uint64_t sWinStartTick = 0;
static unsigned long sWinFrames = 0;
static unsigned long sWinRendered = 0;
static unsigned long sWinSkipped = 0;
static unsigned long sWinVetoed = 0;
static unsigned long sWinDroppedTasks = 0;
static int sWinMaxRun = 0;
static uint64_t sWinMaxLag = 0;
static int sRun = 0;
/* QUICKFIX [overlap] cadence: skip iterations since the latest rendered one (never reset by the
 * receipt window, unlike sRun), and that count as it stood when the latest render was decided. */
static int sSkipsSinceRender = 0;
static int sRenderGap = 0;

/* RACE0 (n60fs, [debug] frameskip = race0; contract in gdx3ds_frameskip.h). Main thread only. */
static int sRace0 = 0;                     /* the meta mode is on */
static int sRace0Other = GDX_FRAMESKIP_AUTO; /* [debug] race0_other: the mode outside races */
static int sRace0Heavy = GDX_FRAMESKIP_AUTO; /* [debug] race0_heavy_mode: a tripped race (default race0_other) */
static int sRace0Arm = 1; /* [debug] race0_arm: the cost guard samples only once the race proper runs */
static int sRace0PostFinish = 1; /* n60results [debug] race0_post_finish: results screens out of scope */
static GdxRace0Ctl sRace0Ctl;
static uint64_t sRace0PrevBegin = 0;       /* the previous iteration's begin tick */
static int sRace0PrevRendered = 0;         /* the previous iteration rendered */
static int sRace0PrevScope = 0;            /* the previous iteration was in raw race scope */
static int sRace0PrevCapture = 0;          /* the previous iteration was an oracle capture (readback) */
static uint64_t sRace0SteadyRw = 0;        /* the latest steady slot's render work (hold excluded) */
static unsigned long sRace0Switches = 0;
static unsigned long sRace0Deferred = 0;   /* switches held one tick for a pending hollow build */
static unsigned long sRace0LastSwitchFrame = 0;

static void FrameskipLog(const char* line) {
    svcOutputDebugString(line, strlen(line));
    gdx3ds_filelog_write(line, strlen(line));
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* "auto" -> AUTO; an integer N >= 1 -> FIXED N; anything else (absent, 0, garbage) -> OFF. */
static int ParseMode(const char* value, int* fixedSkip) {
    char* end = NULL;
    long n;
    if (value == NULL || value[0] == '\0') {
        return GDX_FRAMESKIP_OFF;
    }
    if (strcmp(value, "auto") == 0) {
        return GDX_FRAMESKIP_AUTO;
    }
    if (strcmp(value, "steady") == 0) {
        return GDX_FRAMESKIP_STEADY;
    }
    n = strtol(value, &end, 10);
    if (end == value || n <= 0) {
        return GDX_FRAMESKIP_OFF;
    }
    *fixedSkip = ClampInt((int)n, 1, GDX_FRAMESKIP_FIXED_MAX);
    return GDX_FRAMESKIP_FIXED;
}

static const char* ModeName(int mode) {
    if (mode == GDX_FRAMESKIP_FIXED) {
        return "fixed";
    } else if (mode == GDX_FRAMESKIP_AUTO) {
        return "auto";
    } else if (mode == GDX_FRAMESKIP_STEADY) {
        return "steady";
    } else {
        return "off";
    }
}

/* [debug] steady_min / steady_max (defaults 1 / 4), steady_start (default 2). Returns the
 * gov's maxSkip (D-1). */
static int InitSteady(char* line, size_t cap, const char* source) {
    const int dMax = ClampInt(gdx3ds_config_get_int("debug", "steady_max", 4), 1, 4);
    const int dMin = ClampInt(gdx3ds_config_get_int("debug", "steady_min", 1), 1, dMax);
    const int dStart = ClampInt(gdx3ds_config_get_int("debug", "steady_start", 2), dMin, dMax);
    gdx_steady_init(&sSteady, dMin, dMax, dStart, GDX_STEADY_PERIOD_TICKS, GDX_STEADY_PRESENT_MARGIN_TICKS);
    sSteadyAlign = gdx3ds_config_get_int("debug", "steady_align", 1) != 0;
    sSteadyHold = gdx3ds_config_get_int("debug", "steady_hold", 1) != 0;
    gdx_steady_set_robust(&sSteady, gdx3ds_config_get_int("debug", "steady_robust", 1) != 0);
    sSlotSkipMax = 0;
    snprintf(line, cap,
             "[frameskip] mode=steady D=%d range=%d..%d period=%lu ticks window=%u align=%d hold=%d robust=%d "
             "(%s): one render per D vblanks, present held to the slot's vblank",
             sSteady.d, dMin, dMax, (unsigned long)GDX_STEADY_PERIOD_TICKS, GDX_FRAMESKIP_WINDOW, sSteadyAlign,
             sSteadyHold, sSteady.robust, source);
    return sSteady.d - 1;
}

static int MaxSkipFor(int mode) {
    if (mode == GDX_FRAMESKIP_AUTO) {
        return ClampInt(gdx3ds_config_get_int("debug", "frameskip_max", GDX_FRAMESKIP_AUTO_MAX_DEFAULT), 1,
                        GDX_FRAMESKIP_FIXED_MAX);
    } else if (mode == GDX_FRAMESKIP_FIXED) {
        return sFixedSkip;
    } else {
        return 0;
    }
}

static void ResetWindow(uint64_t now) {
    sWinStartTick = now;
    sWinFrames = 0;
    sWinRendered = 0;
    sWinSkipped = 0;
    sWinVetoed = 0;
    sWinDroppedTasks = 0;
    sWinMaxRun = 0;
    sWinMaxLag = 0;
    sRun = 0;
    sWinMissed = 0;
    sWinCatchup = 0;
    sWinDChanges = 0;
    sWinRwMax = 0;
    sWinHardMissed = 0;
    sWinSpikes = 0;
}

/* The previous mode's tick ran rendered (OFF) or its count is stale: the first render of the new
 * mode follows a rendered tick as far as the [overlap] cadence can know. */
static void ResetRenderGap(void) {
    sSkipsSinceRender = 0;
    sRenderGap = 0;
}

/* Latch `mode` (the refusal is the caller's) and emit its `[frameskip]` receipt. `live` = a
 * DBG-tab switch at the between-iterations safe point: the governor and the slot restart
 * through gdx_frameskip_live_switch (no debt, no held present, first decision renders) and the
 * window receipt restarts. At boot the path is the original one, byte for byte. STEADY owns
 * the vblank alignment, so citro3d's FrameBegin must not also sync (gpuprof external pacing);
 * every other rtMode-0 mode gives the pacing back to FrameBegin. */
static void ApplyMode(int mode, const char* source, int live) {
    char line[192];
    int maxSkip = MaxSkipFor(mode);
    if (mode == GDX_FRAMESKIP_STEADY) {
        maxSkip = InitSteady(line, sizeof(line), source);
    } else if (mode != GDX_FRAMESKIP_OFF) {
        snprintf(line, sizeof(line), "[frameskip] mode=%s max=%d target=59.94Hz interval=%lu ticks window=%u (%s)",
                 ModeName(mode), maxSkip, (unsigned long)GDX_FRAMESKIP_INTERVAL_TICKS, GDX_FRAMESKIP_WINDOW, source);
    } else {
        snprintf(line, sizeof(line), "[frameskip] off (%s)", source);
    }
    if (live) {
        gdx_frameskip_live_switch(&sGov, &sSlot, mode, maxSkip);
        ResetWindow(0);
        ResetRenderGap();
    } else {
        gdx_frameskip_gov_init(&sGov, mode, maxSkip, GDX_FRAMESKIP_INTERVAL_TICKS);
        gdx_frameskip_slot_reset(&sSlot, maxSkip + 1);
    }
    if (sRtMode == 0 && (live || mode == GDX_FRAMESKIP_STEADY)) {
        gdx3ds_gpuprof_set_external_pacing(mode == GDX_FRAMESKIP_STEADY);
    }
    sDropTasks = 0;
    sEnabled = mode != GDX_FRAMESKIP_OFF;
    sMode = mode;
    FrameskipLog(line);
}

static int InitRace0(void);

void gdx3ds_frameskip_init(int rtMode) {
    char line[192];
    char source[48];
    const char* raw = gdx3ds_config_get_string("debug", "frameskip", "0");
    int mode = ParseMode(raw, &sFixedSkip);
    sRtMode = rtMode;
    if (strcmp(raw, "race0") == 0) {
        mode = rtMode == 0 ? InitRace0() : GDX_FRAMESKIP_AUTO; /* rtMode != 0: refused below */
    }
    if (mode != GDX_FRAMESKIP_OFF && rtMode != 0) {
        snprintf(line, sizeof(line),
                 "[frameskip] REFUSED (debug.frameskip=%s): render thread on (rtMode=%d); frame skip is an "
                 "rtMode 0 feature -- running every frame", raw, rtMode);
        FrameskipLog(line);
        mode = GDX_FRAMESKIP_OFF;
    }
    snprintf(source, sizeof(source), "debug.frameskip=%s", raw);
    ApplyMode(mode, source, 0);
}

/* [debug] race0_other (auto | steady, default auto), race0_scope_ticks (8), race0_heavy_ms (33,
 * 0 = cost guard off), race0_light_ms (25). Boot is never in a race: starts in the other mode. */
static int InitRace0(void) {
    char line[224];
    const char* other = gdx3ds_config_get_string("debug", "race0_other", "auto");
    const int scopeTicks = ClampInt(gdx3ds_config_get_int("debug", "race0_scope_ticks", 8), 1, 600);
    const int heavyMs = ClampInt(gdx3ds_config_get_int("debug", "race0_heavy_ms", 33), 0, 1000);
    const int lightMs = ClampInt(gdx3ds_config_get_int("debug", "race0_light_ms", 25), 0, heavyMs);
    sRace0Other = strcmp(other, "steady") == 0 ? GDX_FRAMESKIP_STEADY : GDX_FRAMESKIP_AUTO;
    gdx_race0_init(&sRace0Ctl, scopeTicks, (uint64_t)heavyMs * (uint64_t)CPU_TICKS_PER_MSEC,
                   (uint64_t)lightMs * (uint64_t)CPU_TICKS_PER_MSEC);
    gdx_race0_set_dwell(&sRace0Ctl, ClampInt(gdx3ds_config_get_int("debug", "race0_dwell", 0), 0, 36000));
    sRace0Arm = gdx3ds_config_get_int("debug", "race0_arm", 1) != 0;
    sRace0PostFinish = gdx3ds_config_get_int("debug", "race0_post_finish", 1) != 0;
    {
        const char* heavy = gdx3ds_config_get_string("debug", "race0_heavy_mode", other);
        sRace0Heavy = strcmp(heavy, "steady") == 0 ? GDX_FRAMESKIP_STEADY : GDX_FRAMESKIP_AUTO;
    }
    sRace0 = 1;
    snprintf(line, sizeof(line),
             "[frameskip] mode=race0 other=%s heavy_mode=%s scope_ticks=%d heavy_ms=%d light_ms=%d dwell=%d arm=%d "
             "post_finish=%d ring=%d: every tick renders in races (frameskip 0), %s elsewhere",
             ModeName(sRace0Other), ModeName(sRace0Heavy), scopeTicks, heavyMs, lightMs, sRace0Ctl.dwellTicks, sRace0Arm,
             sRace0PostFinish, GDX_RACE0_RING, ModeName(sRace0Other));
    FrameskipLog(line);
    return sRace0Other;
}

int gdx3ds_frameskip_available(void) {
    return sRtMode == 0;
}

int gdx3ds_frameskip_request_mode(int mode) {
    if (sRtMode != 0) {
        return 0;
    }
    if (mode != GDX_FRAMESKIP_OFF && mode != GDX_FRAMESKIP_AUTO && mode != GDX_FRAMESKIP_STEADY) {
        return 0;
    }
    if (sRace0) {
        sRace0 = 0;
        FrameskipLog("[frameskip] race0 off: DBG-tab mode request overrides it");
    }
    sPendingMode = mode;
    return 1;
}

int gdx3ds_frameskip_mode(void) {
    return sPendingMode >= 0 ? sPendingMode : sMode;
}

/* The safe point: top of an iteration, before its decision. The previous iteration has
 * ended (end_iteration disarmed the task drop) and, if it rendered, presented (the STEADY
 * hold ran in before_present), so nothing is half-done. */
static void ApplyPendingMode(void) {
    const int mode = sPendingMode;
    if (gdx3ds_c5tick_hollow_pending()) {
        /* C5TICK: the previous build was made hollow on the prediction that this iteration skips.
         * Keep the old mode one more iteration (ForceHollowSkip honours the prediction); with the
         * change pending, next_skips refuses, so the build running now is full. */
        sRace0Deferred++;
        return;
    }
    sPendingMode = -1;
    if (mode == sMode) {
        return;
    }
    ApplyMode(mode, sRace0 ? "race0: live" : "menu: live", 1);
}

void gdx3ds_frameskip_mode_label(char* buf, unsigned cap) {
    const int mode = gdx3ds_frameskip_mode();
    const char* prefix = sRace0 ? "r0 " : "";
    if (mode == GDX_FRAMESKIP_STEADY && mode == sMode) {
        snprintf(buf, cap, "%ssteady D%d", prefix, sSteady.d);
    } else if (mode == GDX_FRAMESKIP_FIXED) {
        snprintf(buf, cap, "fixed %d", sFixedSkip);
    } else {
        snprintf(buf, cap, "%s%s", prefix, ModeName(mode));
    }
}

unsigned long gdx3ds_frameskip_skipped_total(void) {
    return sSkippedTotal;
}

int gdx3ds_frameskip_enabled(void) {
    return sEnabled;
}

int gdx3ds_frameskip_render_gap(void) {
    if (!sEnabled) {
        return 0;
    }
    return sRenderGap;
}

static void NoteDecision(const GdxFrameskipDecision* d) {
    sWinFrames++;
    if (d->skip) {
        sWinSkipped++;
        sSkipsSinceRender++;
        sRun++;
        if (sRun > sWinMaxRun) {
            sWinMaxRun = sRun;
        }
    } else {
        sWinRendered++;
        sRun = 0;
        sRenderGap = sSkipsSinceRender;
        sSkipsSinceRender = 0;
    }
    if (d->vetoed) {
        sWinVetoed++;
    }
    if (d->lag > sWinMaxLag) {
        sWinMaxLag = d->lag;
    }
}

/* The vblank wait is citro3d's own C3D_FrameSync (wait until the frame counters its GSP event
 * handler advances on every VBlank0/1 move), NOT gspWaitForVBlank: a main-thread
 * gspWaitForEvent(VBlank0) beside citro3d's handler crashed the emulator's host process within
 * seconds in 5 of 5 lane runs (align=1 or hold=1), and 0 of 1 with both off. C3D_FrameSync is
 * the exact wait the lever-off loop makes every frame (gpuprof SYNCDRAW). */
static void SteadyVblankWait(void) {
    /* C5 STAGE 0: a pacing wait is slack, not work -- [mprof] pace / [c5cat] pace. */
    const int mpPrev = gdx3ds_mprof_enter(GDX3DS_MPROF_PACE);
    const uint64_t t0 = svcGetSystemTick();
    C3D_FrameSync();
    gdx3ds_hitch_add(GDX_HITCH_EV_PACE_TICKS, (uint32_t)(svcGetSystemTick() - t0)); /* HITCH: slack */
    gdx3ds_mprof_leave(mpPrev);
}

static void SleepTicks(uint64_t ticks) {
    /* C5 STAGE 0: the schedule sleep is slack, not work -- [mprof] pace / [c5cat] pace. */
    const int mpPrev = gdx3ds_mprof_enter(GDX3DS_MPROF_PACE);
    const uint64_t t0 = svcGetSystemTick();
    svcSleepThread((s64)(ticks * 1000000000ull / SYSCLOCK_ARM11));
    gdx3ds_hitch_add(GDX_HITCH_EV_PACE_TICKS, (uint32_t)(svcGetSystemTick() - t0)); /* HITCH: slack */
    gdx3ds_mprof_leave(mpPrev);
}

/* Wait for the real vblank nearest `target` (an estimate kept fresh by the returns of earlier
 * waits). Sleeps to half a period before it, then blocks on the GSP vblank event, so a phase
 * error under half a period cannot make it wait a whole extra refresh. Returns the tick after
 * the vblank, or 0 when the target was already reached (no wait). */
static uint64_t WaitVblankNear(uint64_t target, int enabled) {
    const uint64_t now = svcGetSystemTick();
    if (!enabled) {
        return 0;
    }
    if (now + GDX_STEADY_GUARD_TICKS >= target) {
        return 0;
    }
    if (target - now > GDX_STEADY_PERIOD_TICKS / 2u) {
        SleepTicks(target - now - GDX_STEADY_PERIOD_TICKS / 2u);
    }
    SteadyVblankWait();
    return svcGetSystemTick();
}

/* Slot start: the render iteration begins on the vblank D periods after the previous slot's. */
/* A late slot start lost `lost` vblanks of logic. Repay them as extra skip ticks in this slot,
 * but only as many as fit beside the slot's own D-1 in half a vblank after the present (the
 * hold leaves one): the repayment must not make the NEXT slot late too. What does not fit is
 * lost (a hitch then costs game time instead of cascading into more misses). */
static int CatchupTicks(int lost) {
    const uint64_t budget = GDX_STEADY_PERIOD_TICKS / 2u;
    const uint64_t skip = sSteady.skipAvg != 0 ? sSteady.skipAvg : (uint64_t)CPU_TICKS_PER_MSEC;
    const int room = (int)(budget / skip) - (sSteady.d - 1);
    int c = lost < room ? lost : room;
    if (c > 4) {
        c = 4;
    }
    return c > 0 ? c : 0;
}

static void SteadyBeginSlot(void) {
    const uint64_t period = GDX_STEADY_PERIOD_TICKS;
    uint64_t target;
    uint64_t woke;
    uint64_t now;
    sSlot.beginMissed = 0;
    sSlot.catchup = 0;
    if (sSlot.vblank == 0) {
        if (sSteadyAlign) {
            SteadyVblankWait();
        }
        sSlot.vblank = svcGetSystemTick();
        sSlot.renderBegin = sSlot.vblank;
        sSlot.d = sSteady.d;
        return;
    }
    target = sSlot.vblank + (uint64_t)sSlot.d * period;
    woke = WaitVblankNear(target, sSteadyAlign);
    now = svcGetSystemTick();
    if (woke != 0) {
        sSlot.vblank = woke;
    } else if (now > target + period / 2u) {
        sSlot.beginMissed = 1; /* the slot's vblank is gone: start on the latest one that passed */
        sSlot.vblank = target + ((now - target) / period) * period;
        sSlot.catchup = CatchupTicks((int)((now - target + period / 2u) / period));
    } else {
        sSlot.vblank = target;
    }
    sSlot.renderBegin = now;
    sSlot.d = sSteady.d;
    sWinCatchup += (unsigned long)sSlot.catchup;
}

static void SteadyNoteBegin(const GdxFrameskipDecision* d, uint64_t now) {
    if (sSlot.lastWasSkip && sSlot.lastBeginTick != 0) {
        const uint64_t skipCost = now - sSlot.lastBeginTick;
        gdx_steady_note_skip(&sSteady, skipCost);
        if (skipCost > sSlotSkipMax) {
            sSlotSkipMax = skipCost;
        }
    }
    sSlot.lastWasSkip = d->skip;
    sSlot.lastBeginTick = now;
    sSlot.pacedRender = !d->skip && !d->vetoed;
    if (!d->skip) {
        sSlot.slotPaced = sSlot.pacedRender; /* C6: the slot this render opens, kept through its skips */
    }
    if (sSlot.pacedRender) {
        SteadyBeginSlot();
        /* C6 [replay]: at D = 2 the previous slot's skipped tick is shown for this vblank while
         * this slot's tick runs (its GPU work overlaps the logic; FrameBegin waits for it). */
        gdx3ds_replay_slot_begin(sSlot.d, sSlot.beginMissed, sSlot.catchup);
    }
}

/* C5TICK: the prediction above held by construction; only an unpredictable capture request (a
 * net CAPTURE) can make the governor want to render the task of a hollow build. It must not:
 * skip anyway (the capture is lost and counted; the hollow list is never walked). */
static void ForceHollowSkip(GdxFrameskipDecision* d) {
    if (d->skip || !gdx3ds_c5tick_hollow_pending()) {
        return;
    }
    d->skip = 1;
    sGov.skipRun++;
    sHollowForced++;
}

static unsigned long TicksToMsX10(uint64_t ticks);

/* The previous iteration's work, when it rendered in race scope: begin-to-begin, or for a paced
 * steady render its slot work with the present hold left out (a hold is pacing, not cost). */
static uint64_t Race0Sample(uint64_t now) {
    if (sRace0PrevBegin == 0 || !sRace0PrevRendered || !sRace0PrevScope || sRace0PrevCapture) {
        return 0; /* a SHOT/[fh] capture's readback is harness work, not render cost */
    }
    if (sMode == GDX_FRAMESKIP_STEADY && sRace0SteadyRw != 0) {
        return sRace0SteadyRw;
    }
    return now - sRace0PrevBegin;
}

static void LogRace0Switch(unsigned long frame, int target, int why, int gameMode, int changeState) {
    char line[256];
    const int guard = why == GDX_RACE0_WHY_HEAVY || why == GDX_RACE0_WHY_LIGHT;
    const unsigned long pct = guard ? TicksToMsX10(sRace0Ctl.guardPct) : 0;
    snprintf(line, sizeof(line),
             "[frameskip] race0 -> %s at frame %lu reason=%s game=%d chg=%d from=%s after=%lu ticks %s=%lu.%lu ms "
             "switches=%lu deferred=%lu trips=%d dwell=%d",
             target == GDX_FRAMESKIP_OFF ? "0" : ModeName(target), frame, gdx_race0_why_name(why), gameMode,
             changeState, sMode == GDX_FRAMESKIP_OFF ? "0" : ModeName(sMode), frame - sRace0LastSwitchFrame,
             why == GDX_RACE0_WHY_LIGHT ? "p75" : "p25", pct / 10, pct % 10, sRace0Switches, sRace0Deferred,
             sRace0Ctl.trips, gdx_race0_dwell_now(&sRace0Ctl));
    FrameskipLog(line);
}

/* RACE0: read the race scope, feed the controller, and latch a mode change for the safe point
 * right after (ApplyPendingMode, which holds it one tick behind a hollow build). */
static void Race0Tick(unsigned long frame) {
    const uint64_t now = svcGetSystemTick();
    int gameMode = 0;
    int changeState = 0;
    int why = GDX_RACE0_WHY_NONE;
    const int scope = gdx3ds_frameskip_game_race_scope(&gameMode, &changeState);
    const uint64_t sample = (sRace0Arm && !gdx3ds_frameskip_game_racing()) ? 0 : Race0Sample(now);
    const int postFinish = sRace0PostFinish && scope && gdx3ds_frameskip_game_post_finish();
    const int result = gdx_race0_step_post(&sRace0Ctl, scope, postFinish, sample, &why);
    sRace0PrevBegin = now;
    sRace0PrevScope = scope;
    sRace0SteadyRw = 0;
    if (result == GDX_RACE0_HOLD) {
        return;
    }
    {
        const int target = result == GDX_RACE0_TO_EVERY ? GDX_FRAMESKIP_OFF
                           : why == GDX_RACE0_WHY_HEAVY   ? sRace0Heavy
                                                          : sRace0Other;
        sRace0Switches++;
        LogRace0Switch(frame, target, why, gameMode, changeState);
        sRace0LastSwitchFrame = frame;
        sPendingMode = target;
    }
}

static int DecideIteration(unsigned long frame);

int gdx3ds_frameskip_begin_iteration(unsigned long frame) {
    int skip;
    if (sRace0) {
        Race0Tick(frame);
        sRace0PrevCapture = gdx3ds_oracle_wants_next_frame();
    }
    if (sPendingMode >= 0) {
        ApplyPendingMode();
    }
    skip = sEnabled ? DecideIteration(frame) : 0;
    sRace0PrevRendered = !skip;
    return skip;
}

static int DecideIteration(unsigned long frame) {
    GdxFrameskipDecision d;
    uint64_t now;
    now = svcGetSystemTick();
    if (sWinStartTick == 0) {
        sWinStartTick = now;
    }
    if (sMode == GDX_FRAMESKIP_STEADY) {
        sGov.maxSkip = sSlot.d - 1 + sSlot.catchup;
    }
    d = gdx_frameskip_gov_decide(&sGov, now, frame, gdx3ds_oracle_wants_next_frame());
    if (d.skip && gdx3ds_replay_verify_take()) {
        d.skip = 0; /* C6 [debug] replay_verify: replay this tick AND render it, then compare */
    }
    if (d.sleepTicks != 0) {
        SleepTicks(d.sleepTicks);
    }
    if (sMode == GDX_FRAMESKIP_STEADY) {
        SteadyNoteBegin(&d, now);
    }
    ForceHollowSkip(&d);
    NoteDecision(&d);
    if (d.skip) {
        sSkippedTotal++;
    }
    sDropTasks = d.skip;
    return d.skip;
}

/* C5TICK ([debug] dl_hollow, port/3ds/gdx3ds_c5tick.h): the display list built in iteration N
 * is walked in iteration N+1 (the game's framebuffer spin yields between the build and
 * Gfx_SetTask). A hollow build is only allowed when N+1 is certain to skip; this is that
 * prediction, taken at N's build, from the same state the governor will decide from:
 *   FIXED  (N+1)'s frame number is off-phase;
 *   STEADY the slot still has skip ticks left (skipRun < D-1+catchup, both fixed until the
 *          next render);
 *   AUTO   the clock is already a whole tick behind N+1's due time (lag only grows) and the
 *          cap leaves room;
 * and in every mode the oracle will not veto N+1 (fh_every, a pending or next-line SHOT) and no
 * mode change is pending. */
int gdx3ds_frameskip_replay_plan(int* skipRun) {
    *skipRun = sGov.skipRun;
    if (!sEnabled || sMode != GDX_FRAMESKIP_STEADY || sPendingMode >= 0 || !sSlot.slotPaced) {
        return 0;
    }
    return sSlot.d - 1 + sSlot.catchup;
}

int gdx3ds_frameskip_next_skips(unsigned long nextFrame) {
    uint64_t now;
    if (!sEnabled || sPendingMode >= 0 || !sGov.armed) {
        return 0;
    }
    if (gdx3ds_oracle_wants_frame_after_next()) {
        return 0;
    }
    if (sMode == GDX_FRAMESKIP_FIXED) {
        return (nextFrame % (unsigned long)(sGov.maxSkip + 1)) != 0;
    } else if (sMode == GDX_FRAMESKIP_STEADY) {
        return sGov.skipRun < sSlot.d - 1 + sSlot.catchup;
    } else if (sMode == GDX_FRAMESKIP_AUTO) {
        if (sGov.skipRun >= sGov.maxSkip) {
            return 0;
        }
        now = svcGetSystemTick();
        return now > sGov.due && (now - sGov.due) >= sGov.interval;
    }
    return 0;
}

static unsigned long TicksToMsX10(uint64_t ticks) {
    return (unsigned long)((ticks * 10u) / (uint64_t)CPU_TICKS_PER_MSEC);
}

/* missBits/hardBits: the controller's history as it decided (ChangeD clears them after). */
static void LogDChange(int from, int result, uint64_t rw, uint32_t missBits, uint32_t hardBits) {
    char line[224];
    snprintf(line, sizeof(line),
             "[frameskip] steady D %d->%d (%s) rw=%lu.%lu rw90=%lu.%lu cln90=%lu.%lu skip=%lu.%lu ms "
             "missBits=0x%02lx hardBits=0x%02lx",
             from, sSteady.d, result == GDX_STEADY_DOWN ? "down: missed slots" : "up: headroom",
             TicksToMsX10(rw) / 10, TicksToMsX10(rw) % 10,
             TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90)) / 10, TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90)) % 10,
             TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90)) / 10,
             TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90)) % 10,
             TicksToMsX10(sSteady.skipAvg) / 10, TicksToMsX10(sSteady.skipAvg) % 10,
             (unsigned long)(missBits & 0xFFu), (unsigned long)(hardBits & 0xFFu));
    FrameskipLog(line);
}

/* Hold: present at vblank S+D-1 so the swap latches at S+D (the next slot's first vblank). A
 * present that could not make S+D (late work) is a missed slot, like a late start. */
void gdx3ds_frameskip_before_present(void) {
    uint64_t rw;
    uint64_t display;
    uint64_t woke;
    int missed;
    int result;
    int from;
    uint32_t missBefore;
    uint32_t hardBefore;
    if (sMode != GDX_FRAMESKIP_STEADY || !sSlot.pacedRender) {
        return;
    }
    rw = svcGetSystemTick() - sSlot.renderBegin;
    sRace0SteadyRw = rw;
    display = sSlot.vblank + (uint64_t)sSlot.d * GDX_STEADY_PERIOD_TICKS;
    if (sSlot.d > 1) {
        woke = WaitVblankNear(display - GDX_STEADY_PERIOD_TICKS, sSteadyHold);
        if (woke != 0) {
            sSlot.vblank = woke - (uint64_t)(sSlot.d - 1) * GDX_STEADY_PERIOD_TICKS;
            display = woke + GDX_STEADY_PERIOD_TICKS;
        }
    }
    missed = sSlot.beginMissed || svcGetSystemTick() + GDX_STEADY_PRESENT_MARGIN_TICKS > display;
    sLastSlotMissed = missed;
    if (missed) {
        sWinMissed++;
    }
    if (rw > sWinRwMax) {
        sWinRwMax = rw;
    }
    from = sSteady.d;
    missBefore = sSteady.missBits;
    hardBefore = sSteady.hardMissBits;
    result = gdx_steady_note_slot_ex(&sSteady, rw, missed, sSlotSkipMax);
    sSlotSkipMax = 0;
    sSteadyLastHard = missed && !sSteady.lastExplained;
    if (sSteadyLastHard) {
        sWinHardMissed++;
    }
    if (sSteady.lastSpike) {
        sWinSpikes++;
    }
    if (result != GDX_STEADY_HOLD) {
        sWinDChanges++;
        LogDChange(from, result, rw, (missBefore << 1) | (missed ? 1u : 0u),
                   (hardBefore << 1) | (sSteadyLastHard ? 1u : 0u));
    }
    sSlot.pacedRender = 0;
}

/* Rate x10 over the window, as "whole.tenth" without float printf. */
static unsigned long RateX10(unsigned long count, unsigned long dtMs) {
    return dtMs != 0 ? (count * 10000ul) / dtMs : 0;
}

static void EmitSteadyWindow(unsigned long frame) {
    char line[224];
    const unsigned long p50 = TicksToMsX10(gdx_steady_rw_pct(&sSteady, 50));
    const unsigned long p90 = TicksToMsX10(gdx_steady_rw_pct(&sSteady, 90));
    const unsigned long mx = TicksToMsX10(sWinRwMax);
    const unsigned long sk = TicksToMsX10(sSteady.skipAvg);
    const unsigned long c90 = TicksToMsX10(gdx_steady_rw_pct_clean(&sSteady, 90));
    snprintf(line, sizeof(line),
             "[frameskip] steady frame=%lu D=%d hz=%lu.%lu dchg=%lu missed=%lu catchup=%lu rw50=%lu.%lu "
             "rw90=%lu.%lu rwmax=%lu.%lu skip=%lu.%lu ms hard=%lu spikes=%lu cln90=%lu.%lu",
             frame, sSteady.d, 598u / (unsigned)sSteady.d / 10u, (598u / (unsigned)sSteady.d) % 10u, sWinDChanges,
             sWinMissed, sWinCatchup, p50 / 10, p50 % 10, p90 / 10, p90 % 10, mx / 10, mx % 10, sk / 10, sk % 10,
             sWinHardMissed, sWinSpikes, c90 / 10, c90 % 10);
    FrameskipLog(line);
    sWinHardMissed = 0;
    sWinSpikes = 0;
    sWinMissed = 0;
    sWinCatchup = 0;
    sWinDChanges = 0;
    sWinRwMax = 0;
}

static void EmitWindow(unsigned long frame) {
    char line[224];
    const uint64_t now = svcGetSystemTick();
    const unsigned long dtMs = (unsigned long)((now - sWinStartTick) / (uint64_t)CPU_TICKS_PER_MSEC);
    const unsigned long tps = RateX10(sWinFrames, dtMs);
    const unsigned long fps = RateX10(sWinRendered, dtMs);
    snprintf(line, sizeof(line),
             "[frameskip] frame=%lu n=%lu dtMs=%lu rendered=%lu skipped=%lu veto=%lu dropped=%lu "
             "tps=%lu.%lu fps=%lu.%lu maxrun=%d lagmaxms=%lu",
             frame, sWinFrames, dtMs, sWinRendered, sWinSkipped, sWinVetoed, sWinDroppedTasks, tps / 10, tps % 10,
             fps / 10, fps % 10, sWinMaxRun, (unsigned long)(sWinMaxLag / (uint64_t)CPU_TICKS_PER_MSEC));
    FrameskipLog(line);
    if (sMode == GDX_FRAMESKIP_STEADY) {
        EmitSteadyWindow(frame);
    }
    sWinStartTick = now;
    sWinFrames = 0;
    sWinRendered = 0;
    sWinSkipped = 0;
    sWinVetoed = 0;
    sWinDroppedTasks = 0;
    sWinMaxRun = 0;
    sWinMaxLag = 0;
}

void gdx3ds_frameskip_end_iteration(unsigned long frame) {
    if (!sEnabled) {
        return;
    }
    sDropTasks = 0;
    if (sWinFrames >= GDX_FRAMESKIP_WINDOW) {
        EmitWindow(frame);
    }
}

void gdx3ds_frameskip_hitch_info(int* d, int* late) {
    *d = sMode == GDX_FRAMESKIP_STEADY ? sSlot.d : 0;
    *late = sLastSlotMissed;
}

int gdx3ds_frameskip_drop_task(void) {
    if (sDropTasks == 0) {
        return 0;
    }
    sWinDroppedTasks++;
    return 1;
}

unsigned long gdx3ds_frameskip_hollow_forced(void) {
    return sHollowForced;
}
