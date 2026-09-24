/* port/3ds/gdx3ds_dynlod.c — DYNLOD controller. Contract in the header. */
#include "gdx3ds_dynlod.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

extern int gdx_rival_detail_floor(void); /* gdx3ds_menu.c: the user's [perf] rival_detail */

#define GDX_DYNLOD_TIER_MAX 2
#define GDX_DYNLOD_RAISE_FRAMES 4
/* A RAISE needs a MAJORITY of the window over hi, not just a window mean over hi. The mean
   of 4 frames is not outlier-robust: one load beat of +6 ms lifts it by 1.5 ms, and a
   course-load frame lifts it by hundreds — dynlod-progress.md run A recorded exactly that
   ("3 raises on boot/load frames (852 ms load frame)"). Requiring 3 of 4 frames to be
   individually over the line makes a single stall unable to re-tier every distant rival,
   while a genuinely overloaded crowd (most frames over budget) still raises within a few
   dozen frames. */
#define GDX_DYNLOD_RAISE_OVER_MIN ((GDX_DYNLOD_RAISE_FRAMES / 2) + 1)
#define GDX_DYNLOD_DWELL_FRAMES 15 /* [perf] rival_auto_dwell default; applies to BOTH directions */

/* The stability floor on the deadband. A tier change is not an observation, it is an
 * ACTION on the thing being observed: one tier of rival reduction takes 2.5-3.0 ms off the
 * render task (Azahar A/B, docs/research/dynlod-progress.md run C vs B2: -5.0 ms for two
 * tiers; the hardware hand-set MINIMAL +8-9 fps in crowds works out the same). If
 * hi - lo is no wider than that, the controller can cross its OWN lower line by raising:
 * raise at hi, measure below lo, lower, measure above hi, raise. That limit cycle is the
 * bug this constant exists to make unreachable — including by ini, which is why lo is
 * clamped against it at init rather than merely defaulted below it. 6.0 ms = 2x the
 * measured per-tier authority: one factor so a single change cannot span the band, one as
 * margin, because the authority is a measured range and not a constant. */
#define GDX_DYNLOD_MIN_DEADBAND_X10 60

static int sInited = 0;
static int sAuto = 1;             /* [perf] rival_detail_auto (menu writes, task thread reads) */
static int sHiX10 = 150;          /* raise when the 4-frame average exceeds this (tenths of ms) */
static int sLoX10 = 90;           /* lower when an sLowerFrames block averages below this */
static int sLowerFrames = 90;     /* LOWER block length; 1.5 s at 60 fps */
static int sDwellFrames = GDX_DYNLOD_DWELL_FRAMES;
static int sLoClamped = 0;        /* 1 when the ini asked for a deadband narrower than the floor */

static volatile int sTier = 0;    /* controller tier; effective = max(floor, sTier) */
static u64 sTaskT0 = 0;
static int sTaskOpen = 0;
static u32 sHistX10[GDX_DYNLOD_RAISE_FRAMES]; /* last N task walls, tenths of ms */
static unsigned sHistN = 0;
static unsigned sHistAt = 0;
static u32 sLowSumX10 = 0;        /* block average over sLowerFrames frames for the LOWER test */
static unsigned sLowCount = 0;
/* B38 SENSOR. The hardware log could say a block's MEAN was under lo, and that the last four
   frames were under hi (over=0/4), but not whether the block itself was bimodal -- a 90-frame
   mean of 8 ms is equally consistent with 90 quiet frames and with 75 quiet frames plus 15 at
   20 ms. Those two want opposite fixes, and nothing in the receipt could tell them apart. One
   compare per frame closes that, and `blkover=` on the transition line is what the next
   hardware log needs before rival_auto_lower_blocks is defaulted to anything. */
static unsigned sLowOverHi = 0;   /* frames in the OPEN block individually over hi */
static unsigned sLowStreak = 0;   /* consecutive closed blocks that qualified for a LOWER */
static int sLowerBlocks = 1;      /* [perf] rival_auto_lower_blocks; 1 = shipped behaviour */
static int sLowerMaxOver = -1;    /* [perf] rival_auto_lower_maxover; <0 = off = shipped */
static unsigned sSinceChange = 0xFFFFu; /* frames since the last tier change (dwell, both ways) */

/* Receipt counters: window (reset on drain) + cumulative. */
static unsigned sWinFrames = 0;
static u64 sWinTicks = 0;
static u64 sWinMaxTicks = 0;
static unsigned sWinRaises = 0, sWinLowers = 0;
static unsigned sTotRaises = 0, sTotLowers = 0;

static void DynLodLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void gdx3ds_dynlod_init(void) {
    if (sInited) {
        return;
    }
    sInited = 1;
    sAuto = gdx3ds_config_get_bool("perf", "rival_detail_auto", 1) ? 1 : 0;
    sHiX10 = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_hi_x10", 150), 10, 1000);
    /* lo is clamped to hi - GDX_DYNLOD_MIN_DEADBAND_X10, not just defaulted below it: a
       deadband narrower than the controller's own authority is the oscillation, and the
       ini must not be able to ask for it. The old shipped 12.0 is such a value, so an
       existing gdiffuser.ini with rival_auto_lo_x10=120 is honoured as 9.0 and says so. */
    int loMax = sHiX10 - GDX_DYNLOD_MIN_DEADBAND_X10;
    if (loMax < 5) {
        loMax = 5;
    }
    const int loWanted = gdx3ds_config_get_int("perf", "rival_auto_lo_x10", 90);
    sLoX10 = ClampInt(loWanted, 5, loMax);
    sLoClamped = (loWanted > loMax) ? 1 : 0;
    sLowerFrames = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_lower_frames", 90), 1, 600);
    /* B38. How many CONSECUTIVE qualifying blocks a LOWER needs. 1 is the shipped behaviour
       and stays the default: the hardware evidence says the controller is tracking a real
       load swing, not hunting, so raising this trades pops for holding reduced rival detail
       longer, and that trade wants a console to judge it. See vtxmath-progress.md B38. */
    sLowerBlocks = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_lower_blocks", 1), 1, 8);
    /* B38, and the one that is symmetric with the RAISE test. A RAISE needs a MAJORITY of
       its window individually over hi precisely because a mean is not outlier-robust; the
       LOWER test never got the same treatment and is still a bare block mean, which a
       bimodal block passes while containing the very frames that will re-raise. This caps
       how many frames of a block may be individually over hi and still let it qualify.
       -1 = no cap = shipped behaviour. 0 is "lower only after a whole block with nothing
       over budget". Default off: see vtxmath-progress.md B38 for why the hardware evidence
       does not yet justify turning it on. */
    sLowerMaxOver = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_lower_maxover", -1), -1, 600);
    sDwellFrames = ClampInt(gdx3ds_config_get_int("perf", "rival_auto_dwell", GDX_DYNLOD_DWELL_FRAMES),
                            GDX_DYNLOD_RAISE_FRAMES, 600);
    sTier = ClampInt(gdx_rival_detail_floor(), 0, GDX_DYNLOD_TIER_MAX);
    sSinceChange = 0xFFFFu;
    char line[192];
    snprintf(line, sizeof(line),
             "[dynlod] init auto=%d floor=%d hi=%d.%d lo=%d.%d%s lowerN=%d lowerBlk=%d lowerMaxOver=%d "
             "raiseN=%d/%d dwell=%d band=%d.%d",
             sAuto, sTier, sHiX10 / 10, sHiX10 % 10, sLoX10 / 10, sLoX10 % 10,
             sLoClamped ? "(clamped)" : "", sLowerFrames, sLowerBlocks, sLowerMaxOver, GDX_DYNLOD_RAISE_OVER_MIN,
             GDX_DYNLOD_RAISE_FRAMES, sDwellFrames,
             (sHiX10 - sLoX10) / 10, (sHiX10 - sLoX10) % 10);
    DynLodLog(line);
}

int gdx3ds_dynlod_auto_enabled(void) {
    return sAuto;
}

void gdx3ds_dynlod_set_auto(int on) {
    sAuto = on ? 1 : 0;
    if (!sAuto) {
        sTier = 0; /* effective level collapses to the floor immediately */
        sHistN = 0;
        sHistAt = 0;
        sLowSumX10 = 0;
        sLowCount = 0;
        sLowOverHi = 0;
        sLowStreak = 0;
        sSinceChange = 0xFFFFu;
    }
}

int gdx3ds_dynlod_effective_level(int floorLevel) {
    if (!sAuto) {
        return floorLevel;
    }
    const int tier = sTier;
    return tier > floorLevel ? tier : floorLevel;
}

int gdx3ds_dynlod_tier(void) {
    return sAuto ? sTier : 0;
}

void gdx3ds_dynlod_task_begin(void) {
    if (!sAuto) {
        return;
    }
    if (!sInited) {
        gdx3ds_dynlod_init();
    }
    sTaskT0 = svcGetSystemTick();
    sTaskOpen = 1;
}

/* Every transition, with the numbers that caused it — a hardware log has to be able to
   tell "the load changed" from "the controller is hunting", and only the averages can.
   NOT gated on verbose/gputrace: after the hysteresis fix a whole race produces a handful
   of these lines, and they are the evidence that the fix held. */
static void DynLodLogTransition(const char* dir, int from, int to, unsigned avg4X10, unsigned over,
                                int blkX10, unsigned blkN, unsigned blkOver) {
    char line[192];
    char blk[24];
    if (blkX10 >= 0) {
        snprintf(blk, sizeof(blk), "%d.%d", blkX10 / 10, blkX10 % 10);
    } else {
        snprintf(blk, sizeof(blk), "-"); /* no block closed on this frame */
    }
    snprintf(line, sizeof(line),
             "[dynlod] %s %d->%d avg4=%u.%u over=%u/%d blk=%s n=%u blkover=%u streak=%u/%d since=%u "
             "hi=%d.%d lo=%d.%d floor=%d",
             dir, from, to, avg4X10 / 10, avg4X10 % 10, over, GDX_DYNLOD_RAISE_FRAMES, blk, blkN,
             blkOver, sLowStreak, sLowerBlocks, sSinceChange, sHiX10 / 10, sHiX10 % 10, sLoX10 / 10,
             sLoX10 % 10, gdx_rival_detail_floor());
    DynLodLog(line);
}

static unsigned HistAverageX10(void) {
    u32 sum = 0;
    for (unsigned i = 0; i < GDX_DYNLOD_RAISE_FRAMES; i++) {
        sum += sHistX10[i];
    }
    return sum / GDX_DYNLOD_RAISE_FRAMES;
}

/* How many of the last GDX_DYNLOD_RAISE_FRAMES frames were individually over hi. */
static unsigned HistOverHi(void) {
    unsigned over = 0;
    for (unsigned i = 0; i < GDX_DYNLOD_RAISE_FRAMES; i++) {
        if (sHistX10[i] > (u32)sHiX10) {
            over++;
        }
    }
    return over;
}

/* A tier change invalidates every average in flight: the frames before it were produced by
   a different amount of rival detail, so judging the new tier with them is judging the old
   one. Clearing here is what makes the dwell mean "this tier has been measured on its own
   for N frames". */
static void DynLodOnChanged(void) {
    sSinceChange = 0;
    sHistN = 0;
    sHistAt = 0;
    sLowSumX10 = 0;
    sLowCount = 0;
    sLowOverHi = 0;
    sLowStreak = 0;
}

static void DynLodStep(unsigned msX10) {
    const int floorLevel = ClampInt(gdx_rival_detail_floor(), 0, GDX_DYNLOD_TIER_MAX);
    int tier = sTier;
    if (tier < floorLevel) {
        tier = floorLevel; /* the user raised the floor above us — judge the new tier afresh */
        sTier = tier;
        DynLodOnChanged();
    }
    sHistX10[sHistAt] = msX10;
    sHistAt = (sHistAt + 1u) % GDX_DYNLOD_RAISE_FRAMES;
    if (sHistN < GDX_DYNLOD_RAISE_FRAMES) {
        sHistN++;
    }
    if (sSinceChange < 0xFFFFu) {
        sSinceChange++;
    }

    /* DWELL, both directions. The pre-fix controller applied it to RAISE only, so a LOWER
       could land on the frame after a RAISE if a block boundary happened to fall there. */
    const int dwellDone = sSinceChange >= (unsigned)sDwellFrames;

    /* LOWER test: the average of a whole sLowerFrames-frame block below lo (a block, not a
       run of consecutive frames — per-frame jitter of +-2 ms around the threshold would
       otherwise keep the tier stuck at the ceiling forever). The block accumulates only
       after the dwell, so the earliest a LOWER can follow any change is
       dwell + lowerFrames frames — 105 at the defaults, 1.75 s at 60 fps. */
    int blockAvgX10 = -1;
    unsigned blockN = 0;
    unsigned blockOver = 0;
    if (dwellDone) {
        sLowSumX10 += msX10;
        sLowCount++;
        if (msX10 > (u32)sHiX10) {
            sLowOverHi++;
        }
        if (sLowCount >= (unsigned)sLowerFrames) {
            blockAvgX10 = (int)(sLowSumX10 / sLowCount);
            blockN = sLowCount;
            blockOver = sLowOverHi;
            /* A block qualifies on its mean, exactly as before. The streak is what
               rival_auto_lower_blocks counts; a block that does not qualify breaks it, so
               "N consecutive" means N blocks of uninterrupted cheap frames and not N
               qualifying blocks scattered among expensive ones. */
            const int meanOk = blockAvgX10 < sLoX10;
            const int overOk = (sLowerMaxOver < 0) || ((int)sLowOverHi <= sLowerMaxOver);
            if (meanOk && overOk) {
                sLowStreak++;
            } else {
                sLowStreak = 0;
            }
            sLowSumX10 = 0;
            sLowCount = 0;
            sLowOverHi = 0;
        }
    }

    const unsigned avg4X10 = HistAverageX10();
    const unsigned overHi = HistOverHi();
    /* ASYMMETRY. Protecting the frame rate is cheap and urgent: a RAISE needs a majority of
       the 4-frame window individually over hi (and its mean over hi) plus `dwell` frames
       since the last change, ~0.25 s. Giving detail back is expensive and can only
       be wrong: a LOWER needs a whole lowerFrames block averaging under lo AND
       dwell + lowerFrames frames since the last change. Undoing a RAISE therefore costs
       1.75 s of sustained evidence, while reacting to a real crowd costs a quarter second. */
    if (dwellDone && sHistN >= GDX_DYNLOD_RAISE_FRAMES && tier < GDX_DYNLOD_TIER_MAX &&
        avg4X10 > (unsigned)sHiX10 && overHi >= GDX_DYNLOD_RAISE_OVER_MIN) {
        DynLodLogTransition("raise", tier, tier + 1, avg4X10, overHi, blockAvgX10, blockN, blockOver);
        tier++;
        sWinRaises++;
        sTotRaises++;
        DynLodOnChanged();
    } else if (blockAvgX10 >= 0 && blockAvgX10 < sLoX10 && tier > floorLevel &&
               sLowStreak >= (unsigned)sLowerBlocks &&
               sSinceChange >= (unsigned)(sDwellFrames + sLowerFrames)) {
        DynLodLogTransition("lower", tier, tier - 1, avg4X10, overHi, blockAvgX10, blockN, blockOver);
        tier--;
        sWinLowers++;
        sTotLowers++;
        DynLodOnChanged();
    }
    sTier = tier;
}

void gdx3ds_dynlod_task_end(void) {
    if (!sAuto || !sTaskOpen) {
        return;
    }
    sTaskOpen = 0;
    const u64 dt = svcGetSystemTick() - sTaskT0;
    sWinFrames++;
    sWinTicks += dt;
    if (dt > sWinMaxTicks) {
        sWinMaxTicks = dt;
    }
    const unsigned msX10 = (unsigned)((dt * 10u) / (u64)CPU_TICKS_PER_MSEC);
    DynLodStep(msX10);
}

int gdx3ds_dynlod_receipt(char* buf, int cap) {
    if (buf == NULL || cap <= 0) {
        return 0;
    }
    const unsigned n = sWinFrames != 0 ? sWinFrames : 1u;
    const unsigned avgX10 = (unsigned)((sWinTicks * 10u) / ((u64)CPU_TICKS_PER_MSEC * n));
    const unsigned maxX10 = (unsigned)((sWinMaxTicks * 10u) / (u64)CPU_TICKS_PER_MSEC);
    const int len = snprintf(buf, (size_t)cap,
                             "[dynlod] auto=%d floor=%d tier=%d raises=%u/%u lowers=%u/%u ms=%u.%u max=%u.%u n=%u "
                             "hi=%u.%u lo=%u.%u since=%u lowerN=%d dwell=%d",
                             sAuto, gdx_rival_detail_floor(), gdx3ds_dynlod_tier(), sWinRaises, sTotRaises,
                             sWinLowers, sTotLowers, avgX10 / 10, avgX10 % 10, maxX10 / 10, maxX10 % 10,
                             sWinFrames, (unsigned)sHiX10 / 10, (unsigned)sHiX10 % 10, (unsigned)sLoX10 / 10,
                             (unsigned)sLoX10 % 10, sSinceChange, sLowerFrames, sDwellFrames);
    sWinFrames = 0;
    sWinTicks = 0;
    sWinMaxTicks = 0;
    sWinRaises = 0;
    sWinLowers = 0;
    return len > 0 ? len : 0;
}

#ifdef GDX3DS_DYNLOD_HOST_TEST
/* Host-test seam (port/3ds/test/gdx3ds_dynlod_host_tests.c, built by tools/ci-3ds.sh).
 * Never compiled into the 3DS binary: port/CMakeLists.txt does not define the macro.
 * Re-latches [perf]/[debug] from config and clears every counter so one process can drive
 * many independent synthetic frame-time sequences. */
void gdx3ds_dynlod_test_reset(void) {
    sInited = 0;
    sTier = 0;
    sTaskT0 = 0;
    sTaskOpen = 0;
    memset(sHistX10, 0, sizeof(sHistX10));
    sHistN = 0;
    sHistAt = 0;
    sLowSumX10 = 0;
    sLowCount = 0;
    sSinceChange = 0xFFFFu;
    sLoClamped = 0;
    sWinFrames = 0;
    sWinTicks = 0;
    sWinMaxTicks = 0;
    sWinRaises = 0;
    sWinLowers = 0;
    sTotRaises = 0;
    sTotLowers = 0;
    gdx3ds_dynlod_init();
}
#endif
