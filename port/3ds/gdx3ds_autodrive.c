/* port/3ds/gdx3ds_autodrive.c — N60TEX: [debug] autodrive, the game's own CPU driver for player 1.
 *
 * WHY. No corpus replay finishes a race: an open-loop pad script leaves the course within ~15 s
 * (replays/README.md, the time-attack section), so the time trial's FINISH text, the post-finish
 * HUD and the results screen were never seen on the emulator lane. The hardware log was the only
 * evidence for them.
 *
 * WHAT. With `[debug] autodrive = 1`, Racer_UpdateMovement (decomp-3ds-autodrive.patch) drives a
 * player racer with Cpu_GenerateInputs -- the same routine the game already uses for a player
 * after the finish line (cpu.c: `racer->id < gNumPlayers` branch) -- instead of the pad. The pad
 * still drives the menus, so a replay reaches the race and the CPU completes the laps. A
 * MEASUREMENT lever: it changes the game, so both legs of an A/B must carry it.
 *
 * DEFAULT 0 = the pad drives, exactly as before (the hook is one cached int test).
 */

#include "gdx3ds_config.h"

extern void gdx_dbg_logf(const char* fmt, ...);

static int sAutodrive = -1;

static int AutodriveLevel(void) {
    if (sAutodrive < 0) {
        sAutodrive = gdx3ds_config_get_int("debug", "autodrive", 0);
        if (sAutodrive != 0) {
            gdx_dbg_logf("[autodrive] armed: player 1 is driven by Cpu_GenerateInputs (measurement lever)\n");
        }
    }
    return sAutodrive;
}

/* Non-zero when racer `racerId` should take the CPU driver's inputs instead of its pad. */
int gdx_autodrive_racer(int racerId) {
    if (racerId != 0) {
        return 0;
    }
    return AutodriveLevel() != 0;
}

/* ---- n60atlas: [debug] course_force (census lever, default -1 = off) --------------------------------
 *
 * The course a replay races is whatever its menu walk selected. For a per-course census (every
 * course's track materials, docs/research/native/n60atlas-progress.md) one replay is run per course
 * with `[debug] course_force = N` (+ `autodrive = 1`): at the mode-init checkpoint of a race mode
 * (n64_sched.c gdx_transition_step_mark "GMI_B..GMI_C", right before sGamemodeInitFuncs runs
 * Course_Init) gCourseIndex is set to N. 48..53 run the X cup generator (Course_GenerateRandomCourse).
 * A MEASUREMENT lever: it changes the game. */
extern int gCourseIndex;
extern int gGameMode;

static int sCourseForce = -2;

void gdx3ds_course_force_hook(const char* label) {
    if (sCourseForce == -2) {
        sCourseForce = gdx3ds_config_get_int("debug", "course_force", -1);
        if (sCourseForce >= 0) {
            gdx_dbg_logf("[courseforce] armed course=%d (census lever)\n", sCourseForce);
        }
    }
    if (sCourseForce < 0 || label == 0 || label[0] != 'G' || label[4] != 'B' || label[7] != 'G' || label[11] != 'C') {
        return;
    }
    const int mode = gGameMode & 0x1F;
    if (mode != 1 && mode != 2 && mode != 0xE) { /* GP_RACE, PRACTICE, TIME_ATTACK */
        return;
    }
    gdx_dbg_logf("[courseforce] mode=%d course %d -> %d\n", mode, gCourseIndex, sCourseForce);
    gCourseIndex = sCourseForce;
}
