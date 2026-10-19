/* port/3ds/gdx3ds_frameskip_gov.c — FRAMESKIP governor: the pure decision. Contract in
 * gdx3ds_frameskip.h. No 3DS dependency, so the host test links it unchanged. */
#include "gdx3ds_frameskip.h"

/* A render that starts less than this far behind schedule (or ahead of it) re-anchors the
 * schedule to now. That forgives the vblank grid: the host's own pacing is the 3DS LCD
 * (~59.83 Hz), a hair slower than the 59.94 Hz logic target, and the ~0.03 ms/frame it loses
 * would otherwise add up to a whole tick every ~9 s and skip a frame in a scene that renders
 * at full rate. A genuinely slow frame is behind by far more than an eighth of a tick. It also
 * means the schedule never banks credit: a fast stretch cannot buy a later burst. */
#define GDX_FRAMESKIP_FORGIVE_DIV 8u

/* Debt beyond this many ticks is dropped: the console cannot catch up even at the skip cap, so
 * the game degrades to slow motion instead of skipping forever to repay an old load hitch. */
static uint64_t MaxLagTicks(const GdxFrameskipGov* gov) {
    return (uint64_t)(gov->maxSkip + 1) * 2u;
}

void gdx_frameskip_gov_init(GdxFrameskipGov* gov, int mode, int maxSkip, uint64_t interval) {
    gov->mode = mode;
    gov->maxSkip = maxSkip;
    gov->interval = interval;
    gov->due = 0;
    gov->armed = 0;
    gov->skipRun = 0;
}

static int FixedWantsSkip(const GdxFrameskipGov* gov, unsigned long frame) {
    return (frame % (unsigned long)(gov->maxSkip + 1)) != 0;
}

static int AutoWantsSkip(const GdxFrameskipGov* gov, uint64_t lag) {
    return gov->skipRun < gov->maxSkip && lag >= gov->interval;
}

/* STEADY: exactly D-1 skips after every render (maxSkip = D-1, set by the divisor controller);
 * the vblank slot waits live in the glue, not in this schedule. */
static int SteadyWantsSkip(const GdxFrameskipGov* gov) {
    return gov->skipRun < gov->maxSkip;
}

static int WantsSkip(const GdxFrameskipGov* gov, unsigned long frame, uint64_t lag) {
    if (gov->mode == GDX_FRAMESKIP_FIXED) {
        return FixedWantsSkip(gov, frame);
    } else if (gov->mode == GDX_FRAMESKIP_STEADY) {
        return SteadyWantsSkip(gov);
    } else if (gov->mode == GDX_FRAMESKIP_AUTO) {
        return AutoWantsSkip(gov, lag);
    } else {
        return 0;
    }
}

/* Clamp the debt and return the lag (0 when on or ahead of schedule). */
static uint64_t ScheduleLag(GdxFrameskipGov* gov, uint64_t now) {
    const uint64_t maxLag = gov->interval * MaxLagTicks(gov);
    if (!gov->armed) {
        gov->due = now;
        gov->armed = 1;
    }
    if (now <= gov->due) {
        return 0;
    }
    if (now - gov->due > maxLag) {
        gov->due = now - maxLag;
    }
    return now - gov->due;
}

/* FIXED paces EVERY tick that is ahead of schedule, rendered or not: its skipped ticks never
 * wait on a vblank, so without this a scene the console renders at full rate would run the
 * game at (N+1)/1 of the vblank rate. AUTO only skips when behind, and its renders are paced
 * by the vblank like the lever-off loop. */
static uint64_t SleepUntilDue(const GdxFrameskipGov* gov, uint64_t now) {
    if (gov->mode != GDX_FRAMESKIP_FIXED || gov->due <= now) {
        return 0;
    }
    return gov->due - now;
}

/* A render re-anchors the schedule when it starts on time or nearly so (see FORGIVE_DIV). */
static void ForgiveOnRender(GdxFrameskipGov* gov, uint64_t now, uint64_t lag) {
    if (gov->due > now) {
        if (gov->mode == GDX_FRAMESKIP_AUTO) {
            gov->due = now; /* AUTO: no credit; FIXED slept to due, which already is "now" */
        }
    } else if (lag < gov->interval / GDX_FRAMESKIP_FORGIVE_DIV) {
        gov->due = now;
    }
}

GdxFrameskipDecision gdx_frameskip_gov_decide(GdxFrameskipGov* gov, uint64_t now, unsigned long frame, int veto) {
    GdxFrameskipDecision d = { 0, 0, 0, 0 };
    if (gov->mode == GDX_FRAMESKIP_OFF) {
        return d;
    }
    d.lag = ScheduleLag(gov, now);
    d.sleepTicks = SleepUntilDue(gov, now);
    const int wants = WantsSkip(gov, frame, d.lag);
    if (wants && !veto) {
        d.skip = 1;
        gov->skipRun++;
    } else {
        d.vetoed = wants && veto;
        gov->skipRun = 0;
        ForgiveOnRender(gov, now, d.lag);
    }
    gov->due += gov->interval;
    return d;
}

void gdx_frameskip_slot_reset(GdxFrameskipSlot* slot, int d) {
    slot->vblank = 0;
    slot->renderBegin = 0;
    slot->lastBeginTick = 0;
    slot->d = d;
    slot->catchup = 0;
    slot->beginMissed = 0;
    slot->pacedRender = 0;
    slot->slotPaced = 0;
    slot->lastWasSkip = 0;
}

void gdx_frameskip_live_switch(GdxFrameskipGov* gov, GdxFrameskipSlot* slot, int mode, int maxSkip) {
    gdx_frameskip_gov_init(gov, mode, maxSkip, gov->interval);
    gov->skipRun = maxSkip; /* the first decision renders */
    gdx_frameskip_slot_reset(slot, mode == GDX_FRAMESKIP_STEADY ? maxSkip + 1 : 1);
}
