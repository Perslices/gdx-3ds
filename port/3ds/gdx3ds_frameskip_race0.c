/* port/3ds/gdx3ds_frameskip_race0.c — RACE0 controller (n60fs): which frameskip mode runs.
 * Pure (no 3DS dependency); contract in gdx3ds_frameskip.h (the RACE0 section), host-tested by
 * gdx3ds_frameskip_host_tests. */
#include "gdx3ds_frameskip.h"

#include <string.h>

void gdx_race0_init(GdxRace0Ctl* ctl, int scopeTicks, uint64_t heavyAt, uint64_t lightAt) {
    memset(ctl, 0, sizeof(*ctl));
    ctl->scopeTicks = scopeTicks < 1 ? 1 : scopeTicks;
    ctl->heavyAt = heavyAt;
    ctl->lightAt = lightAt;
}

static void ClearRing(GdxRace0Ctl* ctl) {
    ctl->rwN = 0;
    ctl->rwPos = 0;
}

static void PushSample(GdxRace0Ctl* ctl, uint64_t sample) {
    ctl->rw[ctl->rwPos] = sample;
    ctl->rwPos = (ctl->rwPos + 1) % GDX_RACE0_RING;
    if (ctl->rwN < GDX_RACE0_RING) {
        ctl->rwN++;
    }
}

uint64_t gdx_race0_pct(const GdxRace0Ctl* ctl, int pct) {
    uint64_t sorted[GDX_RACE0_RING];
    const int n = ctl->rwN;
    int idx;
    if (n == 0) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        sorted[i] = ctl->rw[i];
    }
    for (int i = 1; i < n; i++) {
        const uint64_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    idx = (pct * (n - 1) + 50) / 100;
    if (idx > n - 1) {
        idx = n - 1;
    }
    return sorted[idx];
}

/* The raw scope must disagree for scopeTicks consecutive ticks to flip the debounced one.
 * Returns 1 when it flipped; a flip resets the cost guard. */
static int DebounceScope(GdxRace0Ctl* ctl, int rawScope) {
    if ((rawScope != 0) == ctl->scope) {
        ctl->scopeRun = 0;
        return 0;
    }
    ctl->scopeRun++;
    if (ctl->scopeRun < ctl->scopeTicks) {
        return 0;
    }
    ctl->scope = rawScope != 0;
    ctl->scopeRun = 0;
    ctl->heavy = 0;
    ctl->trips = 0;
    ctl->heavyTicks = 0;
    ClearRing(ctl);
    return 1;
}

void gdx_race0_set_dwell(GdxRace0Ctl* ctl, int dwellTicks) {
    ctl->dwellTicks = dwellTicks < 0 ? 0 : dwellTicks;
}

int gdx_race0_dwell_now(const GdxRace0Ctl* ctl) {
    if (!ctl->heavy || ctl->dwellTicks == 0 || ctl->trips == 0) {
        return 0;
    }
    return ctl->dwellTicks;
}

static int DwellHeld(const GdxRace0Ctl* ctl) {
    return ctl->heavyTicks >= gdx_race0_dwell_now(ctl);
}

/* Trip on a ring that is mostly slow (p25), release on one that is mostly fast (p75) once the
 * trip's dwell has passed. */
static void UpdateCostGuard(GdxRace0Ctl* ctl) {
    if (ctl->scope && ctl->heavy) {
        ctl->heavyTicks++;
    }
    if (!ctl->scope || ctl->heavyAt == 0 || ctl->rwN < GDX_RACE0_RING) {
        return;
    }
    const uint64_t p25 = gdx_race0_pct(ctl, 25);
    const uint64_t p75 = gdx_race0_pct(ctl, 75);
    if (!ctl->heavy && p25 >= ctl->heavyAt) {
        ctl->heavy = 1;
        ctl->trips++;
        ctl->heavyTicks = 0;
        ctl->guardPct = p25;
        ClearRing(ctl);
    } else if (ctl->heavy && p75 <= ctl->lightAt && DwellHeld(ctl)) {
        ctl->heavy = 0;
        ctl->guardPct = p75;
        ClearRing(ctl);
    }
}

static int ReasonFor(const GdxRace0Ctl* ctl, int scopeFlipped, int racing) {
    if (scopeFlipped) {
        return ctl->scope ? GDX_RACE0_WHY_SCOPE_ENTER : GDX_RACE0_WHY_SCOPE_EXIT;
    } else if (racing) {
        return GDX_RACE0_WHY_LIGHT;
    } else {
        return GDX_RACE0_WHY_HEAVY;
    }
}

/* n60results: the player(s) finished -- the results screens run in the other mode at once (no
 * debounce, no cost guard). Returns 1 when it left the scope. */
static int PostFinishExit(GdxRace0Ctl* ctl) {
    if (!ctl->scope) {
        ctl->scopeRun = 0;
        return 0;
    }
    ctl->scope = 0;
    ctl->scopeRun = 0;
    ctl->heavy = 0;
    ctl->trips = 0;
    ctl->heavyTicks = 0;
    ClearRing(ctl);
    return 1;
}

int gdx_race0_step(GdxRace0Ctl* ctl, int rawScope, uint64_t sample, int* why) {
    return gdx_race0_step_post(ctl, rawScope, 0, sample, why);
}

int gdx_race0_step_post(GdxRace0Ctl* ctl, int rawScope, int postFinish, uint64_t sample, int* why) {
    const int postExit = postFinish ? PostFinishExit(ctl) : 0;
    const int scopeFlipped = postFinish ? postExit : DebounceScope(ctl, rawScope);
    int racing;
    if (postFinish) {
        rawScope = 0;
    }
    if (ctl->scope && rawScope && sample != 0) {
        PushSample(ctl, sample);
    }
    UpdateCostGuard(ctl);
    racing = ctl->scope && !ctl->heavy;
    *why = GDX_RACE0_WHY_NONE;
    if (racing == ctl->racing) {
        return GDX_RACE0_HOLD;
    }
    ctl->racing = racing;
    *why = postExit ? GDX_RACE0_WHY_POST_FINISH : ReasonFor(ctl, scopeFlipped, racing);
    return racing ? GDX_RACE0_TO_EVERY : GDX_RACE0_TO_OTHER;
}

const char* gdx_race0_why_name(int why) {
    if (why == GDX_RACE0_WHY_SCOPE_ENTER) {
        return "race-scope-enter";
    } else if (why == GDX_RACE0_WHY_SCOPE_EXIT) {
        return "race-scope-exit";
    } else if (why == GDX_RACE0_WHY_HEAVY) {
        return "heavy-frames";
    } else if (why == GDX_RACE0_WHY_LIGHT) {
        return "light-frames";
    } else if (why == GDX_RACE0_WHY_POST_FINISH) {
        return "post-finish";
    } else {
        return "none";
    }
}
