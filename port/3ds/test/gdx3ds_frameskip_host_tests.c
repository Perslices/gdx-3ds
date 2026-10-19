/* port/3ds/test/gdx3ds_frameskip_host_tests.c — the FRAMESKIP governor (gdx3ds_frameskip_gov.c)
 * against a simulated console: each host iteration costs `logic` of clock, a RENDERED one
 * costs `render` on top (rounded up to the vblank grid when vblank-paced), and a skip's
 * sleep advances the clock by what the governor asked for. The gates are the two numbers the
 * lever exists for -- logic ticks per wall second (must reach ~59.94 when the console can
 * afford it, and must never exceed it) and the skip cap -- plus the fixed mode's phase and
 * the oracle veto. Clock unit: microseconds. */
#include <stdio.h>
#include <string.h>

#include "gdx3ds_frameskip.h"

#define INTERVAL_US 16683u /* 1001/60000 s */
#define VBLANK_US 16714u   /* the 3DS LCD, ~59.83 Hz: a hair SLOWER than the logic target */

static int sFailures = 0;

static void Check(int ok, const char* what) {
    if (!ok) {
        sFailures++;
        printf("FAIL: %s\n", what);
    }
}

typedef struct Plant {
    unsigned logicUs;
    unsigned renderUs;   /* 0 with vblankPaced: the render is exactly one vblank */
    int vblankPaced;     /* rendered frames end on the next vblank edge */
    unsigned jitterUs;   /* alternating +/- on the render cost */
} Plant;

typedef struct Result {
    unsigned long ticks;
    unsigned long rendered;
    unsigned long skipped;
    unsigned long vetoed;
    int maxRun;
    uint64_t wallUs;
    int phaseOk; /* FIXED: every rendered frame had frame % (N+1) == 0 or was vetoed */
} Result;

static uint64_t NextVblank(uint64_t t) {
    return ((t / VBLANK_US) + 1u) * VBLANK_US;
}

static uint64_t RenderEnd(const Plant* p, uint64_t t, unsigned long frame) {
    if (p->vblankPaced) {
        return NextVblank(t);
    }
    {
        long cost = (long)p->renderUs;
        if (p->jitterUs != 0) {
            cost += (frame & 1u) ? (long)p->jitterUs : -(long)p->jitterUs;
        }
        return t + (uint64_t)cost;
    }
}

/* veto(frame) -> 1 forces a render (the oracle's fh_every sample / pending SHOT). */
static Result Simulate(int mode, int maxSkip, const Plant* p, unsigned long frames, int vetoEvery) {
    GdxFrameskipGov gov;
    Result r;
    uint64_t t = 1000000u;
    const uint64_t t0 = t;
    int run = 0;
    memset(&r, 0, sizeof(r));
    r.phaseOk = 1;
    gdx_frameskip_gov_init(&gov, mode, maxSkip, INTERVAL_US);
    for (unsigned long frame = 1; frame <= frames; frame++) {
        const int veto = vetoEvery > 0 && (frame % (unsigned long)vetoEvery) == 0;
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame, veto);
        t += d.sleepTicks;
        t += p->logicUs;
        r.ticks++;
        if (d.vetoed) {
            r.vetoed++;
        }
        if (d.skip) {
            r.skipped++;
            run++;
            if (run > r.maxRun) {
                r.maxRun = run;
            }
        } else {
            r.rendered++;
            run = 0;
            t = RenderEnd(p, t, frame);
            if (mode == GDX_FRAMESKIP_FIXED && (frame % (unsigned long)(maxSkip + 1)) != 0 && !veto) {
                r.phaseOk = 0;
            }
        }
    }
    r.wallUs = t - t0;
    return r;
}

static double Tps(const Result* r) {
    return (double)r->ticks * 1e6 / (double)r->wallUs;
}

static double Fps(const Result* r) {
    return (double)r->rendered * 1e6 / (double)r->wallUs;
}

static void Report(const char* name, const Result* r) {
    printf("  %-28s ticks=%lu rendered=%lu skipped=%lu veto=%lu maxrun=%d tps=%.2f fps=%.2f\n", name, r->ticks,
           r->rendered, r->skipped, r->vetoed, r->maxRun, Tps(r), Fps(r));
}


/* ---- STEADY: the divisor controller against a slot-timed plant ----
 * Each slot: the render iteration starts on vblank S, works rw, presents at
 * max(S + (D-1)P, S + rw) (the hold), is displayed at S + D*P unless the present came within the
 * GPU margin of it (a miss), then runs D-1 skip ticks; the next slot starts on the first vblank
 * at or after both S + D*P and the end of the skips. Same rules as the glue. */
#define STEADY_MARGIN_US 4000u

typedef uint64_t (*RwFn)(unsigned long slot);

typedef struct SteadyResult {
    unsigned long slots;
    unsigned long ticks;
    unsigned long misses;
    unsigned long dChanges;
    int finalD;
    unsigned long unevenAfterSettle; /* display intervals != D*P after the first 3 s */
    uint64_t wallUs;
} SteadyResult;

static SteadyResult SimulateSteady(RwFn rwOf, unsigned skipUs, unsigned long slots, int dStart) {
    GdxSteadyCtl ctl;
    SteadyResult r;
    uint64_t s = 0;
    uint64_t lastDisplay = 0;
    int lastD = 0;
    memset(&r, 0, sizeof(r));
    gdx_steady_init(&ctl, 1, 4, dStart, VBLANK_US, STEADY_MARGIN_US);
    for (unsigned long i = 0; i < slots; i++) {
        const int d = ctl.d;
        const uint64_t rw = rwOf(i);
        const uint64_t hold = s + (uint64_t)(d - 1) * VBLANK_US;
        const uint64_t present = (s + rw > hold) ? s + rw : hold;
        uint64_t display = s + (uint64_t)d * VBLANK_US;
        const int missed = present + STEADY_MARGIN_US > display;
        uint64_t end = present + (uint64_t)(d - 1) * skipUs;
        uint64_t next = display;
        if (missed) {
            display = ((present + STEADY_MARGIN_US) / VBLANK_US + 1u) * VBLANK_US;
            r.misses++;
        }
        for (int k = 0; k < d - 1; k++) {
            gdx_steady_note_skip(&ctl, skipUs);
        }
        if (gdx_steady_note_slot(&ctl, rw, missed) != GDX_STEADY_HOLD) {
            r.dChanges++;
        }
        if (lastDisplay != 0 && s > 3000000u && lastD == d && display - lastDisplay != (uint64_t)d * VBLANK_US) {
            r.unevenAfterSettle++;
        }
        lastDisplay = display;
        lastD = d;
        if (end > next) {
            next = ((end + VBLANK_US - 1u) / VBLANK_US) * VBLANK_US;
        }
        r.ticks += (unsigned long)d;
        s = next;
        r.slots++;
    }
    r.finalD = ctl.d;
    r.wallUs = s;
    return r;
}

static void ReportSteady(const char* name, const SteadyResult* r) {
    printf("  %-28s slots=%lu D=%d dchg=%lu miss=%lu uneven=%lu tps=%.2f fps=%.2f\n", name, r->slots, r->finalD,
           r->dChanges, r->misses, r->unevenAfterSettle, (double)r->ticks * 1e6 / (double)r->wallUs,
           (double)r->slots * 1e6 / (double)r->wallUs);
}

static uint32_t sLcg = 1u;
static uint32_t Lcg(void) {
    sLcg = sLcg * 1664525u + 1013904223u;
    return sLcg >> 8;
}

static uint64_t RwTrial(unsigned long i) { (void)i; return 22000u; }
static uint64_t RwRace(unsigned long i) { (void)i; return 30000u; }
static uint64_t RwHeavy(unsigned long i) { (void)i; return 45000u; }
/* The hardware GP race windows (hw-wave3 [gpu] build 26-35 ms + ~4 ms logic): 30-39 ms. */
static uint64_t RwRaceNoisy(unsigned long i) { (void)i; return 30000u + (Lcg() % 9000u); }
static uint64_t RwSpike(unsigned long i) { return (i == 400u) ? 150000u : 22000u; }
static uint64_t RwLighten(unsigned long i) { return i < 300u ? 35000u : 8000u; }
/* The lane's menus: 12 ms renders + 4 ms GPU fit 60 Hz with no headroom -- from D=2 the
 * controller must NOT step up (80% rule); from D=1 it must hold without a miss. */
static uint64_t RwMenu12(unsigned long i) { (void)i; return 12000u; }
/* A menu: light renders with a burst of load frames every ~3 s (the lane's menu log). */
static uint64_t RwMenuHitches(unsigned long i) { return ((i % 90u) == 45u || (i % 90u) == 46u) ? 120000u : 8000u; }
/* Borderline: 60% of renders fit D=2, 40% need D=3 -- must not flap. */
static uint64_t RwBorder(unsigned long i) { (void)i; return (Lcg() % 10u) < 6u ? 24000u : 31000u; }

static void SteadyTests(void) {
    {
        const SteadyResult r = SimulateSteady(RwMenu12, 800, 6000, 2);
        ReportSteady("steady / menu 12 ms from D=2", &r);
        Check(r.finalD == 2 && r.dChanges == 0 && r.misses == 0, "no step up without 20% headroom");
        const SteadyResult r1 = SimulateSteady(RwMenu12, 800, 6000, 1);
        ReportSteady("steady / menu 12 ms from D=1", &r1);
        Check(r1.finalD == 1 && r1.misses == 0, "12 ms + 4 ms GPU lands every vblank at D=1");
    }
    {
        const SteadyResult r = SimulateSteady(RwMenuHitches, 800, 9000, 2);
        ReportSteady("steady / menu + load hitches", &r);
        Check(r.dChanges <= 1 && r.finalD == 1, "periodic load hitches must not flap D (one step up, then hold)");
    }
    {
        sLcg = 11u;
        const SteadyResult r = SimulateSteady(RwBorder, 4300, 12000, 2);
        ReportSteady("steady / borderline 24|31 ms", &r);
        Check(r.dChanges <= 6, "a borderline scene settles instead of flapping (backoff)");
    }
    {
        const SteadyResult r = SimulateSteady(RwTrial, 4300, 18000, 2);
        ReportSteady("steady / time trial 22 ms", &r);
        Check(r.finalD == 2 && r.dChanges == 0 && r.misses == 0, "22 ms renders hold D=2 (30 Hz) with no miss");
        Check(r.unevenAfterSettle == 0, "steady cadence is exactly D vblanks");
    }
    {
        const SteadyResult r = SimulateSteady(RwRace, 4300, 12000, 1);
        ReportSteady("steady / race 30 ms from D=1", &r);
        Check(r.finalD == 3, "30 ms renders settle on D=3 (20 Hz)");
        Check(r.dChanges <= 2 && r.misses <= 8, "settling from D=1 is fast: a handful of misses, not a crawl");
        Check(r.unevenAfterSettle == 0, "no uneven display interval once settled");
    }
    {
        const SteadyResult r = SimulateSteady(RwHeavy, 4300, 9000, 2);
        ReportSteady("steady / heavy 45 ms", &r);
        Check(r.finalD == 4, "45 ms renders need D=4 (15 Hz)");
    }
    {
        sLcg = 7u;
        const SteadyResult r = SimulateSteady(RwRaceNoisy, 4300, 12000, 2);
        ReportSteady("steady / race 30-39 ms noisy", &r);
        Check(r.finalD == 3, "the noisy hardware race holds D=3");
        Check(r.dChanges <= 2, "no D oscillation over 10 minutes of noisy race frames");
        Check(r.unevenAfterSettle == 0, "noisy race: every frame on screen for exactly 3 vblanks");
    }
    {
        const SteadyResult r = SimulateSteady(RwSpike, 4300, 3000, 2);
        ReportSteady("steady / one 150 ms hitch", &r);
        Check(r.misses <= 2 && r.dChanges == 0, "a single hitch is a miss, not a cadence change");
    }
    {
        const SteadyResult r = SimulateSteady(RwLighten, 4300, 3000, 3);
        ReportSteady("steady / 35 -> 11 ms", &r);
        Check(r.finalD == 1, "real headroom steps back up to D=1");
        Check(r.dChanges <= 3, "stepping up is one step at a time");
    }
}

/* ---- HITCH: the glue-faithful plant with skip-tick stalls (the hardware's SD flush) ----
 * Like SimulateSteady, plus what the round-2 plant did not model: a slot whose previous slot
 * ran late STARTS late (the glue's beginMissed: more than half a vblank past its vblank, the
 * render then begins at once on the latest vblank that passed), and any skip tick can stall.
 * The controller gets the slot's worst skip tick (gdx_steady_note_slot_ex), exactly as the
 * glue passes it. skipOf(slot, k) is the cost of skip tick k after slot's present. */
typedef uint64_t (*SkipFn)(unsigned long slot, int k);

typedef struct SteadyExResult {
    SteadyResult base;
    unsigned long hardMisses;
    unsigned long upSteps;
    unsigned long downSteps;
    unsigned long slotsAtD[5];
} SteadyExResult;

static SteadyExResult SimulateSteadyEx(RwFn rwOf, SkipFn skipOf, unsigned long slots, int dStart, int robust) {
    GdxSteadyCtl ctl;
    SteadyExResult r;
    uint64_t anchor = VBLANK_US;   /* the slot's vblank */
    uint64_t prevEnd = 0;          /* when the previous slot's last skip tick finished */
    uint64_t prevSkipMax = 0;      /* the previous slot's worst skip tick (reported at this slot) */
    uint64_t lastDisplay = 0;
    int lastD = 0;
    memset(&r, 0, sizeof(r));
    gdx_steady_init(&ctl, 1, 4, dStart, VBLANK_US, STEADY_MARGIN_US);
    gdx_steady_set_robust(&ctl, robust);
    for (unsigned long i = 0; i < slots; i++) {
        const int d = ctl.d;
        const uint64_t rw = rwOf(i);
        uint64_t begin = anchor;
        int beginMissed = 0;
        if (prevEnd > anchor + VBLANK_US / 2u) {
            beginMissed = 1;
            anchor += ((prevEnd - anchor) / VBLANK_US) * VBLANK_US;
            begin = prevEnd;
        } else if (prevEnd > anchor) {
            begin = prevEnd;
        }
        const uint64_t hold = anchor + (uint64_t)(d - 1) * VBLANK_US;
        const uint64_t present = (begin + rw > hold) ? begin + rw : hold;
        uint64_t display = anchor + (uint64_t)d * VBLANK_US;
        const int missed = beginMissed || present + STEADY_MARGIN_US > display;
        uint64_t end = present;
        uint64_t skipMax = 0;
        if (missed) {
            display = ((present + STEADY_MARGIN_US) / VBLANK_US + 1u) * VBLANK_US;
            r.base.misses++;
        }
        for (int k = 0; k < d - 1; k++) {
            const uint64_t c = skipOf(i, k);
            gdx_steady_note_skip(&ctl, c);
            end += c;
            skipMax = c > skipMax ? c : skipMax;
        }
        {
            const int res = gdx_steady_note_slot_ex(&ctl, rw, missed, prevSkipMax);
            if (res != GDX_STEADY_HOLD) {
                r.base.dChanges++;
                r.upSteps += res == GDX_STEADY_UP;
                r.downSteps += res == GDX_STEADY_DOWN;
            }
            r.hardMisses += (missed && !ctl.lastExplained) ? 1u : 0u;
        }
        if (lastDisplay != 0 && anchor > 3000000u && lastD == d && !missed &&
            display - lastDisplay != (uint64_t)d * VBLANK_US) {
            r.base.unevenAfterSettle++;
        }
        lastDisplay = display;
        lastD = d;
        r.slotsAtD[d]++;
        r.base.ticks += (unsigned long)d;
        prevSkipMax = skipMax;
        prevEnd = end;
        anchor += (uint64_t)d * VBLANK_US;
        r.base.slots++;
    }
    r.base.finalD = ctl.d;
    r.base.wallUs = anchor;
    return r;
}

static void ReportSteadyEx(const char* name, const SteadyExResult* r) {
    printf("  %-34s slots=%lu D=%d dchg=%lu (up %lu down %lu) miss=%lu hard=%lu atD=%lu/%lu/%lu/%lu\n", name,
           r->base.slots, r->base.finalD, r->base.dChanges, r->upSteps, r->downSteps, r->base.misses, r->hardMisses,
           r->slotsAtD[1], r->slotsAtD[2], r->slotsAtD[3], r->slotsAtD[4]);
}

/* hw-rel2 race, the flush moved off main: renders 24-30 ms (fit D=3 at 80%). */
static uint64_t RwRaceRel2(unsigned long i) { (void)i; return 24000u + (Lcg() % 6000u); }
/* hw-rel2 as logged: rw50 ~29, rw90 ~31-33 between spikes, and the synchronous flush landing in
 * 4% of renders (rwmax 80-515 ms in almost every window). */
static uint64_t RwRaceRel2Spiky(unsigned long i) {
    const uint32_t r = Lcg() % 100u;
    (void)i;
    if (r < 4u) {
        return 80000u + (Lcg() % 435000u);
    }
    return 25000u + (Lcg() % 7000u);
}
static uint64_t SkipPlain(unsigned long slot, int k) { (void)slot; (void)k; return 3300u; }
/* hw-rel2: 8-11 SD flushes per 256 ticks at 26-270 ms each, most of them in skip ticks. */
static uint64_t SkipFlushy(unsigned long slot, int k) {
    (void)slot;
    (void)k;
    if ((Lcg() % 100u) < 4u) {
        return 26000u + (Lcg() % 244000u);
    }
    return 3300u;
}
static uint64_t SkipOneFlush(unsigned long slot, int k) { return (slot == 500u && k == 0) ? 180000u : 3300u; }
/* A scene that really gets heavier at slot 600 (27 -> 60 ms): not a spike. */
static uint64_t RwHeavierAt600(unsigned long i) { return i < 600u ? 27000u : 60000u + (Lcg() % 4000u); }
/* Every render stalls (a pathological stream of spikes): the flood rule still steps down. */
static uint64_t RwAlwaysSpiky(unsigned long i) { return (i < 200u) ? 20000u : ((i & 1u) ? 90000u : 20000u); }

static void SteadyRobustTests(void) {
    {
        sLcg = 3u;
        const SteadyExResult old = SimulateSteadyEx(RwRaceRel2Spiky, SkipFlushy, 3000, 4, 0);
        ReportSteadyEx("round-2 rules / hw-rel2 race", &old);
        sLcg = 3u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2Spiky, SkipFlushy, 3000, 4, 1);
        ReportSteadyEx("robust / hw-rel2 race", &r);
        /* The plant is not the console (it has no logging on main and no spike-inflated skip
         * average, which is what pinned hw-rel2 to D=4 outright), but the round-2 rules already
         * flap on it: every flushed slot that lands twice in 8 is a step down. */
        Check(old.downSteps >= 2 && old.slotsAtD[4] > 4u * r.slotsAtD[4],
              "plant check: the round-2 rules step down on flush misses (hw-rel2)");
        Check(r.base.finalD == 3, "robust: the hw-rel2 race steps up to D=3 and holds it");
        Check(r.slotsAtD[3] > r.base.slots * 3 / 4, "robust: most of the race at D=3");
        Check(r.downSteps <= 1, "robust: isolated spikes do not flap D");
    }
    {
        sLcg = 5u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2, SkipFlushy, 3000, 4, 1);
        ReportSteadyEx("robust / race, flushes in skips only", &r);
        Check(r.base.finalD == 3 && r.downSteps == 0, "robust: stalled skips never step D down");
    }
    {
        sLcg = 9u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceRel2, SkipOneFlush, 1500, 3, 1);
        ReportSteadyEx("robust / one 180 ms stalled skip", &r);
        Check(r.base.dChanges == 0 && r.base.misses <= 2, "one stalled skip is a miss, not a cadence change");
        Check(r.hardMisses == 0, "the stall explains its miss");
    }
    {
        sLcg = 13u;
        const SteadyExResult r = SimulateSteadyEx(RwHeavierAt600, SkipPlain, 1200, 3, 1);
        ReportSteadyEx("robust / 27 -> 60 ms scene", &r);
        Check(r.base.finalD == 4, "a heavier scene is not a spike: it steps down");
        Check(r.downSteps >= 1 && r.slotsAtD[3] <= 600u + 8u, "the step down lands within a few slots");
    }
    {
        sLcg = 17u;
        const SteadyExResult r = SimulateSteadyEx(RwAlwaysSpiky, SkipPlain, 800, 2, 1);
        ReportSteadyEx("robust / every other render spikes", &r);
        Check(r.base.finalD >= 3, "a stream of spikes is load: the flood rule steps down");
    }
    {
        sLcg = 7u;
        const SteadyExResult r = SimulateSteadyEx(RwRaceNoisy, SkipPlain, 6000, 2, 1);
        ReportSteadyEx("robust / race 30-39 ms noisy", &r);
        Check(r.base.finalD == 3 && r.base.dChanges <= 2, "robust keeps the round-2 noisy-race behaviour");
        Check(r.base.unevenAfterSettle == 0, "robust: every frame on screen for exactly D vblanks");
    }
}

/* ---- LIVE MODE SWITCH (the DBG-tab FRAMESKIP entry): gdx_frameskip_live_switch ---- */

/* Drive `n` decisions one logic tick apart (on schedule), returning the skips seen. */
static int DecideOnSchedule(GdxFrameskipGov* gov, uint64_t* t, unsigned long* frame, int n, int* firstSkip) {
    int skips = 0;
    for (int i = 0; i < n; i++) {
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(gov, *t, (*frame)++, 0);
        if (i == 0 && firstSkip != NULL) {
            *firstSkip = d.skip;
        }
        skips += d.skip;
        *t += INTERVAL_US;
    }
    return skips;
}

/* RACE0 (n60fs): run `ticks` steps of one scope with one sample per tick (0 = no render);
 * returns the number of switches and the last non-HOLD result / reason. */
typedef struct Race0Run {
    int switches;
    int lastResult;
    int lastWhy;
} Race0Run;

static Race0Run Race0Feed(GdxRace0Ctl* ctl, int scope, unsigned sampleUs, int ticks) {
    Race0Run run = { 0, GDX_RACE0_HOLD, GDX_RACE0_WHY_NONE };
    for (int i = 0; i < ticks; i++) {
        int why = 0;
        const int r = gdx_race0_step(ctl, scope, sampleUs, &why);
        if (r != GDX_RACE0_HOLD) {
            run.switches++;
            run.lastResult = r;
            run.lastWhy = why;
        }
    }
    return run;
}

static void Race0ScopeTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    r = Race0Feed(&c, 0, 45000u, 600);
    Check(r.switches == 0 && !c.racing, "race0: menus never switch (boot is the other mode)");
    r = Race0Feed(&c, 1, 17000u, 7);
    Check(r.switches == 0, "race0: 7 in-scope ticks do not flip the scope yet");
    r = Race0Feed(&c, 1, 17000u, 1);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_EVERY && r.lastWhy == GDX_RACE0_WHY_SCOPE_ENTER,
          "race0: the 8th in-scope tick enters every-tick rendering (race-scope-enter)");
    for (int blip = 0; blip < 20; blip++) {
        r = Race0Feed(&c, 0, 17000u, 7);
        Check(r.switches == 0, "race0: a 7-tick scope blip does not flap");
        r = Race0Feed(&c, 1, 17000u, 30);
        Check(r.switches == 0, "race0: back in scope after a blip, no switch");
    }
    r = Race0Feed(&c, 0, 0u, 8);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_OTHER && r.lastWhy == GDX_RACE0_WHY_SCOPE_EXIT,
          "race0: 8 out-of-scope ticks leave (race-scope-exit)");
}

static void Race0GuardTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    Race0Feed(&c, 1, 20000u, 8);
    Check(c.racing, "race0 guard: in race");
    /* GP at 45-50 fps plus a hitch train: 3 of every 16 frames at 120-235 ms. */
    for (int i = 0; i < 2000; i++) {
        int why = 0;
        const unsigned us = (i % 16) < 3 ? 150000u : 21000u;
        const int res = gdx_race0_step(&c, 1, us, &why);
        Check(res == GDX_RACE0_HOLD, "race0 guard: hitches never trip the guard");
        if (res != GDX_RACE0_HOLD) {
            break;
        }
    }
    r = Race0Feed(&c, 1, 28000u, 600);
    Check(r.switches == 0 && c.racing, "race0 guard: a 28 ms race (inside the band) stays every-tick");
    r = Race0Feed(&c, 1, 47000u, 11);
    Check(r.switches == 0, "race0 guard: 11 heavy frames of the last 16 are not enough");
    r = Race0Feed(&c, 1, 47000u, 1);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_OTHER && r.lastWhy == GDX_RACE0_WHY_HEAVY,
          "race0 guard: the 12th heavy frame of 16 (a 47 ms results overlay) leaves every-tick");
    Check(c.guardPct == 47000u, "race0 guard: the receipt keeps the p25 that tripped it");
    r = Race0Feed(&c, 1, 28000u, 600);
    Check(r.switches == 0 && !c.racing, "race0 guard: inside the band the tripped guard holds");
    r = Race0Feed(&c, 1, 18000u, 16);
    Check(r.switches == 1 && r.lastResult == GDX_RACE0_TO_EVERY && r.lastWhy == GDX_RACE0_WHY_LIGHT,
          "race0 guard: 16 light frames come back (light-frames)");
    Race0Feed(&c, 1, 47000u, 16);
    Check(!c.racing, "race0 guard: tripped again");
    r = Race0Feed(&c, 0, 47000u, 8);
    Check(r.switches == 0 && !c.racing, "race0 guard: leaving scope while tripped is no switch");
    r = Race0Feed(&c, 1, 47000u, 8);
    Check(r.switches == 1 && c.racing && r.lastWhy == GDX_RACE0_WHY_SCOPE_ENTER,
          "race0 guard: a new race starts every-tick (the guard resets on scope change)");
    r = Race0Feed(&c, 0, 0u, 8);
    Check(r.switches == 1 && !c.racing, "race0 guard: out");

    gdx_race0_init(&c, 8, 0u, 0u);
    Race0Feed(&c, 1, 20000u, 8);
    r = Race0Feed(&c, 1, 90000u, 2000);
    Check(r.switches == 0 && c.racing, "race0 guard off (heavy_ms=0): pure scope, never trips");
}

/* n60stereo: the 3D card flapped 25 times (heavy 33 ms every-tick <-> light 20 ms under auto). */
static void Race0DwellTests(void) {
    GdxRace0Ctl c;
    Race0Run r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    Race0Feed(&c, 1, 20000u, 8);
    r = Race0Feed(&c, 1, 33400u, 4000);
    Check(r.switches == 1 && !c.racing, "race0 no dwell: trips once on a 33 ms race");
    r = Race0Feed(&c, 1, 20000u, 16);
    Check(r.switches == 1 && c.racing, "race0 no dwell: comes back after 16 light frames (the flap)");
    Race0Feed(&c, 0, 0u, 8);

    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 20000u, 8);
    int flaps = 0;
    for (int cycle = 0; cycle < 40; cycle++) {
        r = Race0Feed(&c, 1, c.racing ? 33400u : 20000u, 120);
        flaps += r.switches;
    }
    Check(flaps <= 14, "race0 dwell 600: a 4800-tick alternating race switches at most 14 times (was 40)");
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 20000u, 8);
    Race0Feed(&c, 1, 33400u, 16);
    Check(!c.racing && gdx_race0_dwell_now(&c) == 600, "race0 dwell: first trip holds 600");
    r = Race0Feed(&c, 1, 20000u, 590);
    Check(r.switches == 0 && !c.racing, "race0 dwell: light frames inside the dwell do not release");
    r = Race0Feed(&c, 1, 20000u, 30);
    Check(r.switches == 1 && c.racing, "race0 dwell: released once the dwell passed");
    Race0Feed(&c, 1, 33400u, 16);
    Check(!c.racing && gdx_race0_dwell_now(&c) == 600 && c.trips == 2, "race0 dwell: every trip holds the same");
    Race0Feed(&c, 0, 0u, 8);
    Race0Feed(&c, 1, 20000u, 8);
    Race0Feed(&c, 1, 33400u, 16);
    Check(gdx_race0_dwell_now(&c) == 600 && c.trips == 1, "race0 dwell: a new race counts trips from 1");
    /* race0_arm: no samples during the intro (the caller passes 0) -> a heavy intro never trips. */
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 0u, 460);
    Check(c.racing && c.rwN == 0, "race0 arm: an unsampled 460-tick intro keeps every-tick rendering");
    r = Race0Feed(&c, 1, 15000u, 600);
    Check(r.switches == 0 && c.racing, "race0 arm: a 15 ms race after the intro never trips");
}

/* n60results race0_post_finish: the finish leaves the scope at once, never trips, cannot re-enter. */
static void Race0PostFinishTests(void) {
    GdxRace0Ctl c;
    int why = 0;
    int r;
    gdx_race0_init(&c, 8, 33000u, 25000u);
    gdx_race0_set_dwell(&c, 600);
    Race0Feed(&c, 1, 15000u, 64);
    Check(c.racing, "race0 post-finish: racing before the line");
    r = gdx_race0_step_post(&c, 1, 1, 15000u, &why);
    Check(r == GDX_RACE0_TO_OTHER && why == GDX_RACE0_WHY_POST_FINISH && !c.racing && !c.scope,
          "race0 post-finish: the first finished tick leaves (no debounce), reason post-finish");
    for (int i = 0; i < 700; i++) {
        r = gdx_race0_step_post(&c, 1, 1, 47000u, &why);
        Check(r == GDX_RACE0_HOLD, "race0 post-finish: a heavy results screen never switches");
    }
    Check(c.trips == 0 && c.rwN == 0 && !c.heavy, "race0 post-finish: no samples, no trip, no dwell");
    r = gdx_race0_step_post(&c, 0, 0, 0u, &why);
    Check(r == GDX_RACE0_HOLD, "race0 post-finish: the mode change after it is no switch");
    Race0Feed(&c, 1, 15000u, 8);
    Check(c.racing, "race0 post-finish: the next race enters as usual (race-scope-enter)");
    r = gdx_race0_step_post(&c, 0, 1, 0u, &why);
    Check(r == GDX_RACE0_TO_OTHER && why == GDX_RACE0_WHY_POST_FINISH, "race0 post-finish: wins over the debounce");
    Check(strcmp(gdx_race0_why_name(GDX_RACE0_WHY_POST_FINISH), "post-finish") == 0, "race0 post-finish: name");
}

static void LiveSwitchTests(void) {
    GdxFrameskipGov gov;
    GdxFrameskipSlot slot;
    uint64_t t = 1000000u;
    unsigned long frame = 1;
    int firstSkip = -1;

    /* steady D=3 mid-slot: one render + one of its two skips done, a held present pending. */
    gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_STEADY, 2, INTERVAL_US);
    gdx_frameskip_slot_reset(&slot, 3);
    DecideOnSchedule(&gov, &t, &frame, 4, NULL);
    slot.vblank = t;
    slot.pacedRender = 1;
    slot.catchup = 2;
    slot.lastWasSkip = 1;
    Check(gov.skipRun == 1, "setup: steady is mid-slot (one skip of two done)");
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 5);
    Check(slot.pacedRender == 0, "switch mid-slot leaves no held present pending");
    Check(slot.vblank == 0 && slot.catchup == 0 && slot.lastWasSkip == 0, "switch un-anchors the slot");
    Check(gov.mode == GDX_FRAMESKIP_AUTO && gov.maxSkip == 5 && gov.interval == INTERVAL_US,
          "switch keeps the tick interval and takes the new mode + cap");
    {
        /* Long after the old schedule (a menu pause): the new mode inherits no debt. */
        const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t + 5000000u, frame++, 0);
        Check(d.skip == 0 && d.lag == 0 && d.sleepTicks == 0, "first decision after switch renders, no lag");
    }

    /* auto deep in a skip run (behind schedule) -> steady D=2: renders, then exactly D-1 skips. */
    gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_AUTO, 5, INTERVAL_US);
    t = 1000000u;
    for (int i = 0; i < 20; i++) {
        (void)gdx_frameskip_gov_decide(&gov, t, frame++, 0);
        t += 70000u; /* slow scene: auto is skipping */
    }
    Check(gov.skipRun > 0, "setup: auto is mid skip-run");
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_STEADY, 1);
    Check(slot.d == 2 && slot.pacedRender == 0 && slot.vblank == 0, "switch into steady: slot at D, unanchored");
    {
        int pattern = 0;
        for (int i = 0; i < 6; i++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame++, 0);
            pattern = (pattern << 1) | d.skip;
            t += INTERVAL_US;
        }
        Check(pattern == 0x15, "steady D=2 after switch: render, skip, render, skip, render, skip");
    }

    /* steady mid-slot -> off: never skips again, whatever the schedule. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_OFF, 0);
    Check(DecideOnSchedule(&gov, &t, &frame, 50, NULL) == 0, "switch to off never skips");

    /* off -> auto in a fast scene: the fresh schedule does not skip. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 5);
    Check(DecideOnSchedule(&gov, &t, &frame, 600, &firstSkip) == 0 && firstSkip == 0,
          "auto after a switch does not skip an on-schedule scene");

    /* the cap binds right after a switch: a hopeless scene runs at most maxSkip in a row. */
    gdx_frameskip_live_switch(&gov, &slot, GDX_FRAMESKIP_AUTO, 3);
    {
        int run = 0;
        int maxRun = 0;
        for (int i = 0; i < 100; i++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame++, 0);
            run = d.skip ? run + 1 : 0;
            maxRun = run > maxRun ? run : maxRun;
            t += 150000u;
        }
        Check(maxRun == 3, "auto cap holds across a switch");
    }
    printf("  live switch                  done\n");
}

int main(void) {
    const Plant fast = {1000, 0, 1, 0};           /* a menu: renders inside one vblank */
    const Plant o3dsRace = {4000, 63800, 0, 0};    /* hardware GP race: ~4 ms logic, 67.8 ms/frame */
    const Plant o3dsTrial = {4000, 36000, 0, 0};   /* hardware time trial: 40 ms/frame */
    const Plant hopeless = {4000, 150000, 0, 0};   /* cannot keep up even at the cap */
    const Plant jittery = {1000, 15000, 0, 1500};  /* 13.5/16.5 ms alternating, under budget */

    printf("gdx3ds_frameskip_host_tests\n");

    {
        const Result r = Simulate(GDX_FRAMESKIP_OFF, 4, &o3dsRace, 600, 0);
        Report("off / race", &r);
        Check(r.skipped == 0, "off must never skip");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &fast, 36000, 0);
        Report("auto / fast (10 min)", &r);
        Check(r.skipped == 0, "auto must not skip a scene that renders every vblank (the 59.83 vs 59.94 drift)");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &jittery, 36000, 0);
        Report("auto / jittery under budget", &r);
        Check(r.skipped == 0, "auto must not skip for sub-tick jitter");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &o3dsRace, 3600, 0);
        Report("auto max4 / O3DS race", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "auto max4 must bring the O3DS race to ~59.94 ticks/s");
        Check(r.maxRun <= 4, "auto must respect frameskip_max");
    }
    {
        /* The default cap (5) against the race's slow tail: 30 machines on screen, p90 frames. */
        const Plant heavyRace = {4300, 75000, 0, 0};
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 5, &heavyRace, 3600, 0);
        Report("auto max5 / O3DS race p90", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "the default cap holds ~59.94 ticks/s on a p90 race frame");
        Check(r.maxRun <= 5, "auto must respect frameskip_max");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &o3dsTrial, 3600, 0);
        Report("auto max4 / O3DS time trial", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "auto must bring the O3DS time trial to ~59.94 ticks/s");
        Check(Fps(&r) > 16.0, "time trial must render more than the race does");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 2, &o3dsRace, 3600, 0);
        Report("auto max2 / O3DS race", &r);
        Check(r.maxRun <= 2, "the cap binds");
        Check(Tps(&r) < 59.0, "a cap too low for the scene degrades to slow motion, not a burst");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 4, &hopeless, 3600, 0);
        Report("auto max4 / hopeless", &r);
        Check(r.maxRun == 4, "a hopeless scene runs at the cap");
        Check(r.rendered * 5 >= r.ticks, "at the cap one frame in five still renders");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &fast, 36000, 0);
        Report("fixed 3 / fast", &r);
        Check(r.phaseOk, "fixed renders exactly the frames with frame % (N+1) == 0");
        Check(r.rendered * 4 == r.ticks, "fixed 3 renders one frame in four");
        Check(Tps(&r) < 60.0, "fixed skips must sleep to the schedule: a fast scene must not run the game fast");
        Check(Tps(&r) > 59.5, "fixed in a fast scene still runs the logic at ~59.94");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &o3dsRace, 3600, 0);
        Report("fixed 3 / O3DS race", &r);
        Check(r.phaseOk, "fixed phase holds in a slow scene");
        Check(Tps(&r) > 40.0, "fixed 3 lifts the O3DS race from ~15 to >40 ticks/s");
    }
    {
        const Result r = Simulate(GDX_FRAMESKIP_FIXED, 3, &o3dsRace, 3600, 6);
        Report("fixed 3 / veto every 6", &r);
        Check(r.phaseOk, "fixed renders only its phase or vetoed frames");
        Check(r.vetoed > 0, "a veto on an off-phase frame is counted");
    }
    {
        /* The veto always wins, in either mode: a sampled frame is never skipped. */
        GdxFrameskipGov gov;
        uint64_t t = 1000000u;
        int allRendered = 1;
        gdx_frameskip_gov_init(&gov, GDX_FRAMESKIP_AUTO, 4, INTERVAL_US);
        for (unsigned long frame = 1; frame <= 200; frame++) {
            const GdxFrameskipDecision d = gdx_frameskip_gov_decide(&gov, t, frame, 1);
            t += 70000u;
            if (d.skip) {
                allRendered = 0;
            }
        }
        Check(allRendered, "a vetoed frame is never skipped");
    }

    {
        /* n60fs: the O3DS menus/results under race0's other mode (hw-n60card2-C: legacy
         * interpreter ~44 ms render, ~2 ms skip ticks; frameskip 0 ran them at 20-24 tps). */
        const Plant o3dsMenu = {2000, 44000, 0, 0};
        const Result r = Simulate(GDX_FRAMESKIP_AUTO, 5, &o3dsMenu, 3600, 0);
        Report("auto max5 / O3DS menu (race0 other)", &r);
        Check(Tps(&r) > 59.0 && Tps(&r) < 60.0, "race0's auto brings the O3DS menus to ~59.94 ticks/s");
        Check(Fps(&r) > 18.0, "the O3DS menus still draw ~20 fps under auto");
    }
    SteadyTests();
    SteadyRobustTests();
    LiveSwitchTests();
    Race0ScopeTests();
    Race0GuardTests();
    Race0DwellTests();
    Race0PostFinishTests();

    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
