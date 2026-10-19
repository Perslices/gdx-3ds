/* port/3ds/test/gdx3ds_dynlod_host_tests.c — DYNLOD controller under synthetic frame-time
 * sequences, on the build machine (tools/ci-3ds.sh, "host unit tests" block).
 *
 * WHY THIS TEST EXISTS (docs/research/native/wobble-progress.md §3 hypothesis 1): the
 * hardware report after the native run G was "distant objects wobbling" and "a bit more
 * pop-in". A dynlod tier change re-tiers EVERY distant rival at once, by a 2.5x (REDUCED)
 * or 6x-beyond-120 (MINIMAL) view-depth bias (decomp-port-rival-detail.patch), so a
 * controller that keeps flipping tiers IS visible pop-in. Under the LUS path the render
 * task sat at 14-15 ms, above the RAISE line and far above the LOWER line, so the tier
 * ratcheted to the ceiling once and stayed: stable, just reduced. The native paths made
 * the task cheap enough to land inside the 12-15 ms band, where both directions can fire.
 *
 * THE MODEL. A dynlod test that replays a fixed frame-time tape is testing an open loop
 * that does not exist: raising the tier makes the very frame the controller measures
 * cheaper. So each case here has a PLANT — `base` is the render-task wall at tier 0 and
 * `savingX10PerTier` is what one tier of rival reduction takes off it. The measured Azahar
 * A/B (docs/research/dynlod-progress.md run C vs run B2, frame-aligned storm windows) is
 * -5.0 ms for tier 0 -> tier 2, i.e. 2.5 ms per tier; the hardware hand-set MINIMAL number
 * (+8-9 fps in 30-machine crowds) works out to the same 2.5-3.0 ms per tier. Cases with
 * `savingX10PerTier = 0` are the degenerate open-loop tape, kept because the directional
 * cases (does it raise at all / does it come back down) are clearest there.
 *
 * NOISE. `sigmaX10` is per-frame jitter (Irwin-Hall 12, deterministic xorshift32 seed) and
 * `spikePermil` / `spikeX10` add the occasional load beat the real logs show. Both are
 * shape, not tuning: every case is driven through the same generator so before/after
 * numbers are comparable.
 *
 * THE ASSERTION is transitions per 1000 frames. A tier change the player can see is a
 * pop; a controller that tracks a real load change costs one or two of them, a controller
 * hunting inside its own deadband costs ten or more. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h> /* port/3ds/test/host/3ds.h on this include path */

#include "gdx3ds_dynlod.h"

/* Host-test seam, compiled into gdx3ds_dynlod.c only under GDX3DS_DYNLOD_HOST_TEST:
 * re-latch [perf] from the config stub and clear every counter, so one process can
 * drive many independent synthetic sequences. */
void gdx3ds_dynlod_test_reset(void);

/* ------------------------------------------------------------------ stubs -- */

u64 gdx3ds_test_tick = 0;

/* Captured receipt lines: the test asserts that every transition is logged WITH the
 * averages that caused it, which is what makes a hardware log decodable. */
#define GDX_DYNLOD_TEST_TIER_CEIL 3 /* mirrors GDX_DYNLOD_TIER_CEIL: NATIVE/REDUCED/MINIMAL/LOW */

#define LOG_CAP 4096
static char sLog[LOG_CAP][160];
static int sLogN = 0;

Result svcOutputDebugString(const char* str, s32 length) {
    if (sLogN < LOG_CAP) {
        int n = length < (s32)sizeof(sLog[0]) - 1 ? length : (s32)sizeof(sLog[0]) - 1;
        memcpy(sLog[sLogN], str, (size_t)n);
        sLog[sLogN][n] = '\0';
        sLogN++;
    }
    return 0;
}

void gdx3ds_filelog_write(const char* msg, size_t len) {
    (void)msg;
    (void)len;
}

/* Config stub: a tiny key table the cases overwrite before each reset. */
#define CFG_CAP 16
static struct {
    const char* section;
    const char* key;
    int value;
} sCfg[CFG_CAP];
static int sCfgN = 0;

static void CfgClear(void) {
    sCfgN = 0;
}

static void CfgSet(const char* section, const char* key, int value) {
    if (sCfgN < CFG_CAP) {
        sCfg[sCfgN].section = section;
        sCfg[sCfgN].key = key;
        sCfg[sCfgN].value = value;
        sCfgN++;
    }
}

int gdx3ds_config_get_int(const char* section, const char* key, int fallback) {
    for (int i = 0; i < sCfgN; i++) {
        if (strcmp(sCfg[i].section, section) == 0 && strcmp(sCfg[i].key, key) == 0) {
            return sCfg[i].value;
        }
    }
    return fallback;
}

int gdx3ds_config_get_bool(const char* section, const char* key, int fallback) {
    return gdx3ds_config_get_int(section, key, fallback);
}

/* gdx3ds_perf_preset.c stand-in: no preset, so a [perf] key reads exactly as the ini (the
 * preset module has its own host test, gdx3ds_perf_preset_host_tests.c). */
int gdx3ds_perf_get_int(const char* key, int stockDefault) {
    return gdx3ds_config_get_int("perf", key, stockDefault);
}

static int sFloor = 0;

int gdx_rival_detail_floor(void) {
    return sFloor;
}

/* ------------------------------------------------------------- generator -- */

static u32 sRng = 0;

static u32 NextRand(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

/* Irwin-Hall 12: sum of 12 uniforms, mean 6, variance 1 -> standard normal to well past
 * the +-3 sigma this test cares about, with no libm and no float non-determinism. */
static int NoiseX10(int sigmaX10) {
    if (sigmaX10 <= 0) {
        return 0;
    }
    long acc = 0;
    for (int i = 0; i < 12; i++) {
        acc += (long)(NextRand() % 1000u);
    }
    /* acc in [0,11988], mean 5994. Var(U{0..999}) = 1000^2/12, so Var(acc) = 1000^2 and one
     * sigma of acc is exactly 1000 units. */
    return (int)(((acc - 5994) * sigmaX10) / 1000);
}

typedef struct {
    const char* name;
    int loX10Ini;           /* 0 = shipped default; otherwise what [perf] rival_auto_lo_x10 asks for */
    int baseX10;            /* render-task wall at tier 0, tenths of ms */
    int savingX10PerTier;   /* what one tier of rival reduction takes off it */
    int sigmaX10;           /* per-frame jitter, one sigma */
    int spikePermil;        /* load-beat probability, per mille */
    int spikeX10;           /* load-beat size */
    int startTier;          /* tier the case begins at (driven up by a hot prefix) */
    int frames;
    int maxTransitions;     /* the gate */
    /* B38 BIMODAL PLANT. The hardware (hw-crash-0923) does not show a controller hunting
       inside its deadband -- every lower has over=0/4 and a block mean of 4.3-8.9, every
       raise has over>=3/4 and avg4 of 15.6-31.2. The INPUT crosses the whole band, by
       7-28 ms, which is 3-9x the controller's 2.5-3.0 ms authority. That is a rival pack
       coming into and out of view, and it is what these three fields model: every
       burstPeriod frames, burstLen frames cost burstX10 more. */
    int burstPeriod;
    int burstLen;
    int burstX10;
    int lowerBlocksIni;     /* [perf] rival_auto_lower_blocks; 0 = leave at default */
    /* Every case above uses positional initialisers, so a trailing field defaults to 0 --
       and 0 is a MEANINGFUL value for rival_auto_lower_maxover ("no frame over hi"). The
       flag is what says whether to set it at all, so that 0 cannot be reached by accident. */
    int setMaxOver;
    int lowerMaxOverIni;
} Case;

/* C4p, the Old 3DS preset: its RAISE line and ceiling as the ini would carry them, and the
   floor the preset sets, applied by CountTransitions to every case while set. All-zero = the
   shipped defaults (every case before section 5c). Kept out of Case so the positional
   initialisers above stay complete. */
static struct {
    int hiX10Ini;
    int tierMaxIni;
    int floor;
} sBand = {0, 0, 0};

/* Frame index inside the measured window, for the bimodal plant. */
static int sFrameIdx = 0;

/* Drive one frame: the controller's own tier decides the cost it then measures. */
static void RunFrame(const Case* c) {
    const int tier = gdx3ds_dynlod_tier();
    int msX10 = c->baseX10 - (tier * c->savingX10PerTier) + NoiseX10(c->sigmaX10);
    if (c->spikePermil > 0 && (int)(NextRand() % 1000u) < c->spikePermil) {
        msX10 += c->spikeX10;
    }
    if (c->burstPeriod > 0 && (sFrameIdx % c->burstPeriod) < c->burstLen) {
        msX10 += c->burstX10;
    }
    sFrameIdx++;
    if (msX10 < 1) {
        msX10 = 1;
    }
    gdx3ds_dynlod_task_begin();
    gdx3ds_test_tick += ((u64)msX10 * (u64)(SYSCLOCK_ARM11 / 10)) / 1000u;
    gdx3ds_dynlod_task_end();
}

/* Push the tier to `target` with a deliberately expensive prefix, then let the controller
 * settle, so the measured window starts from a known tier instead of from boot. */
static void PrimeToTier(const Case* c, int target) {
    Case hot = *c;
    hot.baseX10 = 1000; /* a storm frame: over any RAISE line, the Old 3DS preset's 45.0 included */
    hot.savingX10PerTier = 0;
    hot.sigmaX10 = 0;
    hot.spikePermil = 0;
    for (int i = 0; i < 2000 && gdx3ds_dynlod_tier() < target; i++) {
        RunFrame(&hot);
    }
}

static int CountTransitions(const char* label, const Case* c, u32 seed, int quiet, int* outEndTier) {
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    if (c->loX10Ini != 0) {
        CfgSet("perf", "rival_auto_lo_x10", c->loX10Ini);
    }
    if (c->lowerBlocksIni != 0) {
        CfgSet("perf", "rival_auto_lower_blocks", c->lowerBlocksIni);
    }
    if (c->setMaxOver) {
        CfgSet("perf", "rival_auto_lower_maxover", c->lowerMaxOverIni);
    }
    if (sBand.hiX10Ini != 0) {
        CfgSet("perf", "rival_auto_hi_x10", sBand.hiX10Ini);
    }
    if (sBand.tierMaxIni != 0) {
        CfgSet("perf", "rival_tier_max", sBand.tierMaxIni);
    }
    sFloor = sBand.floor;
    gdx3ds_dynlod_test_reset();
    sRng = seed;
    sLogN = 0;

    sFrameIdx = 0;
    if (c->startTier > 0) {
        PrimeToTier(c, c->startTier);
    }
    sFrameIdx = 0;
    /* Drain the prime's counters and start the measured window clean. */
    char scratch[256];
    gdx3ds_dynlod_receipt(scratch, (int)sizeof(scratch));
    const int before = sLogN;

    for (int i = 0; i < c->frames; i++) {
        RunFrame(c);
    }

    int transitions = 0;
    for (int i = before; i < sLogN; i++) {
        if (strncmp(sLog[i], "[dynlod] raise", 14) == 0 || strncmp(sLog[i], "[dynlod] lower", 14) == 0) {
            transitions++;
        }
    }
    if (getenv("GDX_DYNLOD_TEST_DUMP") != NULL) {
        for (int i = before; i < sLogN; i++) {
            if (strncmp(sLog[i], "[dynlod] raise", 14) == 0 || strncmp(sLog[i], "[dynlod] lower", 14) == 0) {
                printf("      | %s\n", sLog[i]);
            }
        }
    }
    *outEndTier = gdx3ds_dynlod_tier();
    const int per1000 = (int)(((long)transitions * 1000L) / (long)c->frames);
    if (!quiet) {
        printf("  %-14s base=%d.%d save=%d.%d/tier sigma=%d.%d start=%d -> transitions=%d "
               "(%d/1000 frames) endTier=%d  [gate <=%d/1000]\n",
               label, c->baseX10 / 10, c->baseX10 % 10, c->savingX10PerTier / 10,
               c->savingX10PerTier % 10, c->sigmaX10 / 10, c->sigmaX10 % 10, c->startTier, transitions,
               per1000, *outEndTier, c->maxTransitions);
    }
    return per1000;
}

/* A single seed proves nothing about a noisy loop: every noisy case is replayed over
 * SEEDS independent streams and gated on the WORST of them. */
#define SEEDS 8
static const u32 kSeeds[SEEDS] = {0x9e3779b9u, 0x1234567u, 0xdeadbeefu, 0x5bd1e995u,
                                  0x27d4eb2fu, 0x85ebca6bu, 0xc2b2ae35u, 0x165667b1u};

static int WorstOverSeeds(const Case* c, int* outWorstEndTier, int* outBestEndTier) {
    int worst = 0;
    int endTier = 0;
    int worstEnd = 0;
    int bestEnd = GDX_DYNLOD_TEST_TIER_CEIL; /* min over seeds: start above any tier */
    for (int i = 0; i < SEEDS; i++) {
        const int t = CountTransitions(c->name, c, kSeeds[i], 1, &endTier);
        if (t > worst) {
            worst = t;
            worstEnd = endTier;
        }
        if (endTier < bestEnd) {
            bestEnd = endTier;
        }
    }
    *outWorstEndTier = worstEnd;
    *outBestEndTier = bestEnd;
    printf("  %-14s base=%d.%d save=%d.%d/tier sigma=%d.%d lo=%s -> worst %d/1000 over %d seeds "
           "[gate <=%d/1000]\n",
           c->name, c->baseX10 / 10, c->baseX10 % 10, c->savingX10PerTier / 10,
           c->savingX10PerTier % 10, c->sigmaX10 / 10, c->sigmaX10 % 10,
           c->loX10Ini ? "ini" : "default", worst, SEEDS, c->maxTransitions);
    return worst;
}

/* ------------------------------------------------------------------ cases -- */

static int sFailures = 0;

static void Check(int cond, const char* what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        sFailures++;
    }
}

/* Per-tier saving from the frame-aligned Azahar A/B (dynlod-progress.md run C vs B2:
 * -5.0 ms for two tiers) — the only measured control authority this controller has. */
#define SAVE_PER_TIER 25

int main(void) {
    printf("=== gdx3ds_dynlod_host_tests ===\n");
    sFloor = 0;

    /* --- 1. directional: the controller still does its job ------------------ */
    printf("directional (open loop — the tier does not change the cost):\n");

    /* Steady 16.0 ms, no noise, no plant: above the RAISE line forever, so the tier must
     * ratchet to the ceiling and then stop. Exactly two changes, ever. */
    Case steady16 = {"steady16", 0, 160, 0, 0, 0, 0, 0, 1000, 2, 0, 0, 0, 0, 0, 0};
    int endTier = 0;
    int t = CountTransitions("steady 16ms", &steady16, kSeeds[0], 0, &endTier);
    Check(t <= steady16.maxTransitions, "steady 16 ms must ratchet at most twice");
    Check(endTier == 2, "steady 16 ms must end pinned at MINIMAL");

    /* Steady 11.0 ms from the floor: under the RAISE line, already at the floor, so there
     * is nothing to do. Zero changes. */
    Case steady11 = {"steady11", 0, 110, 0, 0, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, 0};
    t = CountTransitions("steady 11ms", &steady11, kSeeds[0], 0, &endTier);
    Check(t == 0, "steady 11 ms at the floor must not move the tier");
    Check(endTier == 0, "steady 11 ms must stay at NATIVE");

    /* Recovery: the load really did go away (8.0 ms even with full detail), so the tier
     * must walk all the way back to the floor and give the detail back. */
    printf("directional (closed loop — the tier moves the cost):\n");
    Case recover = {"recover", 0, 80, SAVE_PER_TIER, 0, 0, 0, 2, 1000, 4, 0, 0, 0, 0, 0, 0};
    t = CountTransitions("recover 8ms", &recover, kSeeds[0], 0, &endTier);
    Check(endTier == 0, "a genuinely cheap frame must return every tier to the floor");
    Check(t <= recover.maxTransitions, "recovery must cost at most one change per tier");

    /* --- 2. in band, open loop --------------------------------------------- */
    /* The literal synthetic sequences: a noisy tape whose mean sits inside the 12-15 ms
     * band. Without the plant the loop cannot hunt — it can only ratchet — so these are
     * the control for case 3 below, not the interesting case. */
    printf("in band, open loop (control — the tape ignores the tier):\n");
    Case band[3] = {
        {"band12.5", 0, 125, 0, 12, 15, 60, 0, 1000, 2, 0, 0, 0, 0, 0, 0},
        {"band13.5", 0, 135, 0, 12, 15, 60, 0, 1000, 2, 0, 0, 0, 0, 0, 0},
        {"band14.5", 0, 145, 0, 12, 15, 60, 0, 1000, 2, 0, 0, 0, 0, 0, 0},
    };
    int worstEnd = 0, bestEnd = 0;
    for (int i = 0; i < 3; i++) {
        t = WorstOverSeeds(&band[i], &worstEnd, &bestEnd);
        Check(t <= band[i].maxTransitions, "an open-loop tape must only ratchet");
    }

    /* --- 3. in band, closed loop — THE CASE THAT MATTERS -------------------- */
    /* The native paths put the tier-0 render task inside the band. Raising a tier now
     * moves the measurement by SAVE_PER_TIER, i.e. by about as much as the whole
     * hi-lo deadband, so the controller can cross its own LOWER line by acting — raise,
     * lower ~30 frames later, raise again. Every one of those is every distant rival
     * jumping a LOD step. The gate is 4 per 1000 frames: at 60 fps that is one visible
     * re-tier every four seconds, which is already the outer edge of "tracking the load"
     * rather than hunting. */
    printf("in band, closed loop (the native regime — hunting shows here):\n");
    Case loop[3] = {
        {"loop12.5", 0, 125, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
        {"loop13.5", 0, 135, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
        {"loop14.5", 0, 145, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
    };
    for (int i = 0; i < 3; i++) {
        t = WorstOverSeeds(&loop[i], &worstEnd, &bestEnd);
        Check(t <= loop[i].maxTransitions, "the controller must not hunt inside its own deadband");
    }

    /* A stiffer plant (3.0 ms/tier, the hardware hand-set MINIMAL figure) at the top of
     * the band: the worst realistic combination. */
    Case stiff = {"loop15.0stiff", 0, 150, 30, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0};
    t = WorstOverSeeds(&stiff, &worstEnd, &bestEnd);
    Check(t <= stiff.maxTransitions, "a stiffer plant must not make the controller hunt");

    /* The narrow deadband the pre-fix build SHIPPED, asked for explicitly through the ini
       key that still exists. It has to be refused, or an old gdiffuser.ini on an SD card
       reintroduces the oscillation the rest of this fix removes. */
    printf("narrow deadband demanded by ini ([perf] rival_auto_lo_x10=120, the pre-fix default):\n");
    Case narrow[3] = {
        {"ini12.5", 120, 125, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
        {"ini13.5", 120, 135, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
        {"ini14.5", 120, 145, SAVE_PER_TIER, 12, 15, 60, 0, 1000, 4, 0, 0, 0, 0, 0, 0},
    };
    for (int i = 0; i < 3; i++) {
        t = WorstOverSeeds(&narrow[i], &worstEnd, &bestEnd);
        Check(t <= narrow[i].maxTransitions, "an ini must not be able to ask for an unstable deadband");
    }

    /* --- 3a-bis. B38: the SLOW cycle, and it is the input, not the controller --- */
    /* hw-crash-0923 (post-deadband-fix, lo=9.0 lowerN=90 dwell=15) has 27 transitions, all
       1<->2 against floor=1, and six raises land 15-117 frames after a lower. That reads
       like a residual limit cycle. It is not one, and this case is the demonstration.

       Every LOWER in that log carries over=0/4 and a block mean of 4.3-8.9 ms; every RAISE
       carries over=3/4 or 4/4 and avg4 of 15.6-31.2 ms. So the controller never acted on
       ambiguous evidence in either direction -- the INPUT moved 7-28 ms, which is 3-9x the
       2.5-3.0 ms of authority a tier change has over it. No deadband that still lets the
       controller act can sit across a swing that size.

       The plant below is that shape: a quiet 7.0 ms base with a 40-frame burst of +14.0 ms
       every 250 frames -- a rival pack coming into and out of view. The assertions are
       about WHAT MUST REMAIN TRUE, not about the transition count: the controller must
       still protect the frame rate (it must end a burst at a raised tier) and the receipt
       must carry the new sensor. The count itself is printed for the record. */
    printf("B38 bimodal load (a rival pack arrives every 250 frames):\n");
    {
        /*        name        lo  base save            sig sp spX st frames gate  burstP len burst blk over? maxover */
        Case biA = {"bimodal-shipped", 0, 70, SAVE_PER_TIER, 8, 0, 0, 1, 3000, 999, 250, 40, 140, 0, 0, 0};
        Case biB = biA; biB.name = "bimodal-blocks2";  biB.lowerBlocksIni = 2;
        Case biC = biA; biC.name = "bimodal-maxover0"; biC.setMaxOver = 1; biC.lowerMaxOverIni = 0;
        Case biD = biA; biD.name = "bimodal-both";     biD.lowerBlocksIni = 2; biD.setMaxOver = 1; biD.lowerMaxOverIni = 0;
        int eA = 0, eB = 0, eC = 0, eD = 0;
        const int nA = CountTransitions(biA.name, &biA, kSeeds[0], 0, &eA);
        const int nB = CountTransitions(biB.name, &biB, kSeeds[0], 0, &eB);
        const int nC = CountTransitions(biC.name, &biC, kSeeds[0], 0, &eC);
        const int nD = CountTransitions(biD.name, &biD, kSeeds[0], 0, &eD);
        printf("    per-1000: shipped=%d(endTier=%d) blocks2=%d(%d) maxover0=%d(%d) both=%d(%d)\n",
               nA, eA, nB, eB, nC, eC, nD, eD);

        /* Both levers only ever WITHHOLD a lower, so neither may raise the count. */
        Check(nB <= nA, "rival_auto_lower_blocks must not increase transitions");
        Check(nC <= nA, "rival_auto_lower_maxover must not increase transitions");
        Check(nD <= nA, "the two levers together must not increase transitions");

        /* THE FINDING, pinned so that nobody turns these on believing they are free.
           Neither lever alone changes the count at all: the quiet gap between bursts (210
           frames) is wider than the evidence either one asks for (180 frames), so a
           qualifying block still lands in it. Together they ask for 180 CONSECUTIVE frames
           with nothing over hi, which the 210-frame gap only rarely aligns with -- so the
           count falls, but it falls because the controller stops coming back down and
           parks at the ceiling. That is the LUS ratchet (permanently reduced rival detail)
           which is the failure mode the whole dynlod feature exists to avoid.

           The mechanism this encodes: the load's period (250 frames) is comparable to the
           controller's own evidence window (90-194 frames), and no statistic over a window
           that short can tell "quiet now" from "quiet but about to burst". */
        Check(nA == nB && nA == nC,
              "B38: neither lever alone moves a bimodal plant -- the quiet gap outlasts the evidence");
        Check(nD < nA && eD > eA,
              "B38: the pair only helps by parking the tier at the ceiling, which is the ratchet");
    }

    /* --- 3a-ter. B38: the documented LOWER floor is wrong, and hardware says so --- */
    /* dynlod-progress-native.md states "the earliest a LOWER can follow any change is
       dwell + lowerFrames = 105 frames (~1.75 s)". The shipped code does not do that. The
       block accumulator starts on the frame the dwell completes, so the first block CLOSES
       at since = dwell + lowerFrames - 1 = 104 -- and the guard is `since >= 105`, so that
       first block is computed, consumed and thrown away. The earliest LOWER is therefore
       the SECOND block, at 194 frames (~3.2 s).

       This is not a reading of the source: all 13 lowers in hw-crash-0923 carry a `since`
       that is exactly 14 mod 90, and the smallest is 194, never 104. Pinned here because
       the constant is load-bearing for B38 -- 194 is most of the observed 200-300 frame
       cycle period -- and because it is currently accidental rather than chosen. */
    printf("LOWER floor after a change (the doc says 105; hardware says 194):\n");
    {
        CfgClear();
        CfgSet("perf", "rival_detail_auto", 1);
        gdx3ds_dynlod_test_reset();
        sRng = kSeeds[0];
        sLogN = 0;
        Case hotc = {"hot", 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        Case cheapc = {"cheap", 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        sFrameIdx = 0;
        while (gdx3ds_dynlod_tier() < 1) {
            RunFrame(&hotc);
        }
        const int before = sLogN;
        for (int i = 0; i < 400; i++) {
            RunFrame(&cheapc);
        }
        int sinceAtLower = -1;
        for (int i = before; i < sLogN && sinceAtLower < 0; i++) {
            if (strncmp(sLog[i], "[dynlod] lower", 14) == 0) {
                const char* q = strstr(sLog[i], "since=");
                if (q != NULL) {
                    sinceAtLower = atoi(q + 6);
                }
            }
        }
        printf("  first lower after a raise: since=%d  (dwell+lowerN=%d, dwell+2*lowerN-1=%d)\n",
               sinceAtLower, 15 + 90, 15 + 2 * 90 - 1);
        Check(sinceAtLower == 194, "the first block after a change is discarded: the floor is 194, not 105");
        Check(sinceAtLower % 90 == 14, "every LOWER lands at since = 14 mod 90, as hw-crash-0923 shows");
    }

    /* --- 3b. it still ACTS: step response ---------------------------------- */
    /* Asymmetric hysteresis degenerates into "never does anything" if the constants are
       wrong, and a controller that never acts passes every oscillation gate. So: a cheap
       stretch, then a real crowd arrives (17.0 ms at full detail), then it disperses. The
       tier must climb while the crowd is there and come all the way back afterwards, and
       the latencies are bounded — protecting the frame rate has to be fast, giving detail
       back is allowed to be slow. */
    printf("step response (a crowd arrives, then disperses):\n");
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    gdx3ds_dynlod_test_reset();
    sRng = kSeeds[0];
    sLogN = 0;
    Case quiet = {"quiet", 0, 100, SAVE_PER_TIER, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Case crowd = {"crowd", 0, 170, SAVE_PER_TIER, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 300; i++) {
        RunFrame(&quiet);
    }
    Check(gdx3ds_dynlod_tier() == 0, "a 10.0 ms stretch must not raise the tier at all");
    int raiseLatency = -1;
    for (int i = 0; i < 600; i++) {
        RunFrame(&crowd);
        if (raiseLatency < 0 && gdx3ds_dynlod_tier() > 0) {
            raiseLatency = i;
        }
    }
    const int crowdTier = gdx3ds_dynlod_tier();
    int lowerLatency = -1;
    for (int i = 0; i < 900; i++) {
        RunFrame(&quiet);
        if (lowerLatency < 0 && gdx3ds_dynlod_tier() == 0) {
            lowerLatency = i;
        }
    }
    printf("  10.0ms -> 17.0ms: raised in %d frames (tier %d) ; back to 10.0ms: floor again in %d frames\n",
           raiseLatency, crowdTier, lowerLatency);
    Check(crowdTier >= 1, "a real 17.0 ms crowd must still raise the tier");
    Check(raiseLatency >= 0 && raiseLatency <= 60, "protecting the frame rate must take under a second");
    Check(gdx3ds_dynlod_tier() == 0, "the tier must return to the floor once the crowd disperses");
    Check(lowerLatency >= 0 && lowerLatency <= 600, "giving detail back must still happen inside 10 s");

    /* --- 3c. a course-load frame is not a crowd ---------------------------- */
    /* dynlod-progress.md run A: "3 raises on boot/load frames (852 ms load frame -> 1->2)".
       One stalled frame is not a reason to re-tier every distant rival. */
    printf("load stall (one 850 ms course-load frame inside a quiet stretch):\n");
    gdx3ds_dynlod_test_reset();
    sRng = kSeeds[0];
    sLogN = 0;
    Case stall = {"stall", 0, 8500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 300; i++) {
        if (i == 150) {
            RunFrame(&stall);
        } else {
            RunFrame(&quiet);
        }
    }
    int stallTransitions = 0;
    for (int i = 0; i < sLogN; i++) {
        if (strncmp(sLog[i], "[dynlod] raise", 14) == 0 || strncmp(sLog[i], "[dynlod] lower", 14) == 0) {
            stallTransitions++;
        }
    }
    printf("  transitions across the stall: %d (tier %d)\n", stallTransitions, gdx3ds_dynlod_tier());
    Check(stallTransitions == 0, "one load-stall frame must not re-tier every distant rival");

    /* --- 4. killswitch: auto=0 must be untouched by any of this ------------- */
    printf("killswitch:\n");
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 0);
    gdx3ds_dynlod_test_reset();
    sLogN = 0;
    sFloor = 1;
    Case hot = {"hot", 0, 400, 0, 0, 0, 0, 0, 500, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 500; i++) {
        RunFrame(&hot);
    }
    Check(gdx3ds_dynlod_auto_enabled() == 0, "auto=0 must stay off");
    Check(gdx3ds_dynlod_effective_level(1) == 1, "auto=0 must return exactly the floor");
    Check(gdx3ds_dynlod_effective_level(0) == 0, "auto=0 must return exactly the floor");
    int transitions = 0;
    for (int i = 0; i < sLogN; i++) {
        if (strncmp(sLog[i], "[dynlod] raise", 14) == 0 || strncmp(sLog[i], "[dynlod] lower", 14) == 0) {
            transitions++;
        }
    }
    Check(transitions == 0, "auto=0 must never move the tier");
    printf("  auto=0: transitions=%d effective(floor=1)=%d\n", transitions,
           gdx3ds_dynlod_effective_level(1));
    sFloor = 0;

    /* --- 5. the floor still wins ------------------------------------------- */
    printf("floor:\n");
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    gdx3ds_dynlod_test_reset();
    sFloor = 2;
    Case cheap = {"cheap", 0, 50, 0, 0, 0, 0, 0, 500, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 500; i++) {
        RunFrame(&cheap);
    }
    Check(gdx3ds_dynlod_effective_level(2) == 2, "the tier can never fall below the user's floor");
    Check(gdx3ds_dynlod_tier() == 2, "a raised floor snaps the tier up");
    printf("  floor=2 on 5.0 ms frames: tier=%d\n", gdx3ds_dynlod_tier());
    sFloor = 0;

    /* --- 5b. rival_tier_max: 2 unless asked, 3 reachable only when asked ------ */
    printf("tier max:\n");
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    gdx3ds_dynlod_test_reset();
    Case storm = {"storm", 0, 400, 0, 0, 0, 0, 0, 500, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 500; i++) {
        RunFrame(&storm);
    }
    Check(gdx3ds_dynlod_tier_max() == 2, "the shipped ceiling is MINIMAL");
    Check(gdx3ds_dynlod_tier() == 2, "without rival_tier_max the controller must stop at MINIMAL");
    printf("  default: tierMax=%d tier after 500 storm frames=%d\n", gdx3ds_dynlod_tier_max(),
           gdx3ds_dynlod_tier());
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    CfgSet("perf", "rival_tier_max", 3);
    gdx3ds_dynlod_test_reset();
    for (int i = 0; i < 500; i++) {
        RunFrame(&storm);
    }
    Check(gdx3ds_dynlod_tier_max() == 3, "rival_tier_max=3 must open LOW");
    Check(gdx3ds_dynlod_tier() == 3, "with rival_tier_max=3 a storm must reach LOW");
    printf("  rival_tier_max=3: tier after 500 storm frames=%d\n", gdx3ds_dynlod_tier());
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    CfgSet("perf", "rival_tier_max", 9);
    gdx3ds_dynlod_test_reset();
    Check(gdx3ds_dynlod_tier_max() == 3, "rival_tier_max is clamped to the tiers the decomp hook knows");

    /* --- 5c. the Old 3DS preset's band, under an Old 3DS plant --------------- */
    /* gdx3ds_perf_preset.c: floor REDUCED, ceiling LOW, hi 45.0 / lo 30.0. The plants are the
     * hardware windows of hw-c4run2-0929 (o3dslod-progress.md §2): a 30-machine GP race at
     * 81 ms with tier 2 (so ~88 at tier 0 with the emulator's measured ~3.5 ms/tier), a
     * one-machine race at 41 ms whose cost no rival tier can move, and a menu at 27 ms. The
     * gate is the same as everywhere: a controller that tracks load, not one that hunts. */
    printf("old 3ds preset band (hi 45.0 lo 30.0 floor 1 max 3):\n");
    sBand.hiX10Ini = 450;
    sBand.tierMaxIni = 3;
    sBand.floor = 1;
    Case o3dsGp = {"o3ds-gp", 300, 880, 35, 60, 15, 200, 0, 1000, 2, 0, 0, 0, 0, 0, 0};
    t = WorstOverSeeds(&o3dsGp, &worstEnd, &bestEnd);
    Check(t <= o3dsGp.maxTransitions, "an O3DS GP race must ratchet to LOW (two steps), not hunt");
    Check(bestEnd == 3, "an O3DS GP race must end at LOW on every seed");
    Case o3dsTt = {"o3ds-tt", 300, 410, 0, 40, 15, 200, 0, 1000, 2, 0, 0, 0, 0, 0, 0};
    t = WorstOverSeeds(&o3dsTt, &worstEnd, &bestEnd);
    Check(t <= o3dsTt.maxTransitions, "a one-machine O3DS race must not re-tier the field");
    Case o3dsMenu = {"o3ds-menu", 300, 270, 0, 20, 0, 0, 3, 1000, 2, 0, 0, 0, 0, 0, 0};
    t = CountTransitions("o3ds-menu", &o3dsMenu, kSeeds[0], 0, &endTier);
    Check(endTier == 1, "a quiet O3DS menu must give detail back down to the preset floor");
    Check(t == 2, "a quiet O3DS menu must lower exactly twice, LOW -> MINIMAL -> REDUCED");
    sBand.hiX10Ini = 0;
    sBand.tierMaxIni = 0;
    sBand.floor = 0;
    sFloor = 0;

    /* --- 6. every transition must be decodable from a hardware log ---------- */
    /* A [dynlod] transition line has to carry the averages that caused it, or a hardware
     * log cannot tell a load change from a hunt. */
    printf("receipt:\n");
    CfgClear();
    CfgSet("perf", "rival_detail_auto", 1);
    gdx3ds_dynlod_test_reset();
    sLogN = 0;
    Case ramp = {"ramp", 0, 200, 0, 0, 0, 0, 0, 200, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 200; i++) {
        RunFrame(&ramp);
    }
    int checkedLine = 0;
    for (int i = 0; i < sLogN; i++) {
        if (strncmp(sLog[i], "[dynlod] raise", 14) != 0 && strncmp(sLog[i], "[dynlod] lower", 14) != 0) {
            continue;
        }
        checkedLine++;
        Check(strstr(sLog[i], "avg4=") != NULL, "a transition line must carry the 4-frame average");
        Check(strstr(sLog[i], "blk=") != NULL, "a transition line must carry the LOWER block average");
        Check(strstr(sLog[i], "n=") != NULL, "a transition line must carry the LOWER block length");
        Check(strstr(sLog[i], "hi=") != NULL, "a transition line must carry the thresholds it compared against");
        Check(strstr(sLog[i], "lo=") != NULL, "a transition line must carry the thresholds it compared against");
        Check(strstr(sLog[i], "since=") != NULL, "a transition line must carry the dwell it satisfied");
        if (checkedLine == 1) {
            printf("  sample: %s\n", sLog[i]);
        }
    }
    Check(checkedLine >= 2, "the ramp must produce transitions to inspect");

    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
