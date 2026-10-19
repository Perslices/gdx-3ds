/* port/3ds/gdx3ds_dynlod.h — DYNLOD (LOCKED-60 Task J): automatic rival-detail tier.
 *
 * The decomp's RIVAL-DETAIL hook (decomp-port-rival-detail.patch, Racer_Draw reads
 * gdx_rival_detail_level once per frame) biases the LOD tiers of the non-player machines:
 * 0=NATIVE 1=REDUCED 2=MINIMAL. Hand-set MINIMAL is worth +8-9 fps in 30-machine crowds
 * on hardware but costs distant detail everywhere else. This controller measures the
 * render task's wall time per frame (one svcGetSystemTick pair around gdx_gfx_run — the
 * render thread's job in pipe mode, the inline path otherwise; no gputrace needed) and
 * moves the EFFECTIVE tier between the user's manual setting (the FLOOR) and MINIMAL (the
 * ceiling) with ASYMMETRIC hysteresis.
 *
 * The asymmetry is not taste. A tier change is an ACTION on the signal being measured:
 * one tier of rival reduction takes 2.5-3.0 ms off the render task (Azahar A/B,
 * docs/research/dynlod-progress.md run C vs B2; the hardware hand-set MINIMAL +8-9 fps in
 * crowds agrees). The original shipped band was hi 15.0 / lo 12.0 — 3.0 ms wide, i.e. no
 * wider than one tier of authority — so once the native renderer brought the task inside
 * that band the controller could cross its own LOWER line simply by RAISING: raise, drop
 * below lo, lower ~30 frames later, raise again. Each of those transitions re-tiers EVERY
 * distant rival at once (2.5x, or 6x beyond 120, view-depth bias), which is visible as
 * distant machines flickering between LODs. The host test measured 26-27 transitions per
 * 1000 frames in that regime; see docs/research/native/dynlod-progress-native.md.
 *
 *   - RAISE (protect the frame rate — cheap, urgent): a majority
 *     (GDX_DYNLOD_RAISE_OVER_MIN of GDX_DYNLOD_RAISE_FRAMES) of the last frames are
 *     INDIVIDUALLY over [perf] rival_auto_hi_x10 / 10 ms (default 15.0) AND their average
 *     is too. The majority test is what stops one course-load stall from re-tiering the
 *     whole field. Needs [perf] rival_auto_dwell (default 15) frames since the last
 *     change, so ~0.25 s.
 *   - LOWER (give detail back — expensive, only ever undone by a raise): a whole
 *     [perf] rival_auto_lower_frames (default 90) frame block AVERAGES below
 *     [perf] rival_auto_lo_x10 / 10 ms (default 9.0), the block having started only after
 *     the dwell — so the earliest a LOWER can follow any change is dwell + lowerFrames
 *     = 105 frames, ~1.75 s.
 *   - lo is CLAMPED to hi - GDX_DYNLOD_MIN_DEADBAND_X10 (6.0 ms) at init, not merely
 *     defaulted below it: an ini must not be able to ask for a deadband narrower than the
 *     controller's own authority. An existing gdiffuser.ini carrying the old
 *     rival_auto_lo_x10=120 is honoured as 9.0, and the init receipt says "(clamped)".
 *   - any change clears the raise window and the lower block: frames measured under a
 *     different amount of rival detail describe the old tier, not the new one.
 *
 * Killswitch [perf] rival_detail_auto=0 (default 1, read live via the menu latch): the
 * effective level is exactly the manual setting and the tick pair is skipped, so the old
 * path is byte-identical.
 *
 * Threads: note_task_* run on whichever thread executes the gfx task (render thread in
 * pipe/sync mode, main inline); the tier is a single aligned int read by Racer_Draw on
 * main once per frame (single writer, benign one-frame lag). The receipt is drained by
 * the bridge on the [race-dl] cadence from the same thread that runs the task. */
#ifndef GDX3DS_DYNLOD_H
#define GDX3DS_DYNLOD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Latch config ([perf] rival_detail_auto + thresholds). Called from gdx3ds_menu_init;
 * also lazily from the first task note if that ever runs first. */
void gdx3ds_dynlod_init(void);

/* Killswitch latch (menu row + config). */
int gdx3ds_dynlod_auto_enabled(void);
void gdx3ds_dynlod_set_auto(int on);

/* The effective rival-detail level for the decomp hook: max(floor, auto tier) with the
 * controller on, exactly `floor` with it off. */
int gdx3ds_dynlod_effective_level(int floorLevel);
/* The controller's ceiling: 2 (MINIMAL) by default, 3 (LOW) with [perf] rival_tier_max = 3. */
int gdx3ds_dynlod_tier_max(void);
/* The controller's current tier (0..tier_max) for the menu's live row. */
int gdx3ds_dynlod_tier(void);

/* One pair per gfx task around gdx_gfx_run. Both are no-ops with the killswitch off. */
void gdx3ds_dynlod_task_begin(void);
void gdx3ds_dynlod_task_end(void);

/* [dynlod] receipt: formats one line (no newline) and resets the window counters.
 * Returns the length, 0 when nothing was measured in the window. */
int gdx3ds_dynlod_receipt(char* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_DYNLOD_H */
