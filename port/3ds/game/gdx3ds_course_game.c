/* port/3ds/game/gdx3ds_course_game.c -- nicourse `[native] ni_course` (nointerp S5): the game side of the
 * 3D course preview (docs/research/native/nicourse-progress.md).
 *
 * The course select / GP next-course card / records course select draw up to six course models a
 * frame (course_model.c func_i5_80115E64): per model a G_VIEWPORT, then Course_DrawModel's G_VTX /
 * G_TRI2 run over a vertex buffer func_800A2D2C filled ONCE when the cup was chosen. Only the camera
 * matrix (orbit yaw, 1 degree a frame), the viewport (the carousel scroll) and the prim colour change
 * from frame to frame. With the lever on, decomp-native-course-preview.patch brackets each
 * Course_DrawModel call:
 *
 *   gdx_course_model_mark_begin(gfx)  reserves two Gfx right before the run (returns gfx + 2);
 *   gdx_course_model_mark_end(gfx, vtx, count) fills them once the run is written:
 *     [0] w0 = (G_NOOP << 24) | (0x6B << 16) | nCmds           w1 = vtx (the decomp's tagged host pointer)
 *     [1] w0 = (G_NOOP << 24) | (0x6B << 16) | 0x8000 | count  w1 = generation
 *   nCmds = the run's command count, count = its vertex count, generation = gdx_course_model_gen at the
 *   time the list was built. func_800A2D2C bumps the generation on entry (the same patch), so a cup
 *   change, the X cup's random courses or any rewrite of a model buffer re-keys every model.
 *
 * The owned walker (port/3ds/gfx/n64/gdx_nicourse.cpp) draws the run from a GPU-resident buffer and
 * skips it, or leaves it to the stock walk (the second marker word is then a no-op). Lever off: no
 * marker words at all, the list is the stock list. Only words are added; no game state is read or
 * written besides the generation counter this file owns. Compiled into gdx3ds_game. */
#include "global.h"

#include <stdint.h>

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_native_ready(void) __attribute__((weak));
extern int gdx3ds_native_raw(const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_native_verify_forces_fold_off(void) __attribute__((weak));

#define GDXCOURSE_MARKER_P 0x6Bu

static int sCourseLever = -1; /* [native] ni_course: 0 off, 1 on, 2 verify (latched at first use) */
static Gfx* sCourseMark = NULL;
uint32_t gdx_course_model_gen = 1;

static int CourseLatch(void) {
    int level;
    int requested;
    if (&gdx3ds_native_ready == NULL || &gdx3ds_native_raw == NULL || !gdx3ds_native_ready() ||
        &gdx3ds_config_get_int == NULL) {
        return -1;
    }
    level = gdx3ds_native_raw("ni_course", 0);
    if (level < 0 || level > 2) {
        level = 0;
    }
    requested = level;
    /* `interp` is deliberately never implied by any other lever (gdx3ds_native_cfg.c: "it changes
     * WHICH interpreter runs, not what it does, and is always explicit") -- without it the owned
     * walker that would consume these markers never runs, so emitting them would be dead weight.
     * This forced-off case cost a whole session's worth of emulator legs once already (every A/B
     * gate "passed" because both legs silently ran the same stock path): log it loudly, once. */
    if (requested != 0 && gdx3ds_config_get_int("native", "interp", 0) == 0) {
        level = 0;
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[nicourse] ni_course=%d IGNORED: needs [native] interp=1 (the owned walker "
                         "consumes the markers; interp is never implied by another lever) -- falling "
                         "back to the stock list\n",
                         requested);
        }
    } else if (level != 0 && &gdx3ds_native_verify_forces_fold_off != NULL && gdx3ds_native_verify_forces_fold_off()) {
        level = 0; /* interp_verify: the LUS pass walks the list as is */
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[nicourse] ni_course=%d IGNORED: interp_verify forces the LUS pass to walk "
                         "the list as is\n",
                         requested);
        }
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[nicourse] init game ni_course=%d (marker p=0x%02X x2 around each course model's run)\n", level,
                     GDXCOURSE_MARKER_P);
    }
    return level;
}

Gfx* gdx_course_model_mark_begin(Gfx* gfx) {
    if (sCourseLever < 0) {
        sCourseLever = CourseLatch();
    }
    sCourseMark = NULL;
    if (sCourseLever <= 0 || gfx == NULL) {
        return gfx;
    }
    sCourseMark = gfx;
    return gfx + 2;
}

void gdx_course_model_mark_end(Gfx* gfx, Vtx* vtx, s32 vtxCount) {
    Gfx* mark = sCourseMark;
    uintptr_t nCmds;
    sCourseMark = NULL;
    if (mark == NULL) {
        return;
    }
    nCmds = (uintptr_t)(gfx - mark) - 2u;
    if (nCmds > 0x7FFFu || vtxCount < 0 || vtxCount > 0x7FFF) {
        nCmds = 0; /* never in practice; nCmds 0 = the walker leaves the run to the stock walk */
    }
    mark[0].words.w0 = ((uint32_t)G_NOOP << 24) | (GDXCOURSE_MARKER_P << 16) | (uint32_t)nCmds;
    mark[0].words.w1 = _GFXW1_PTR(vtx);
    mark[1].words.w0 = ((uint32_t)G_NOOP << 24) | (GDXCOURSE_MARKER_P << 16) | 0x8000u | (uint32_t)vtxCount;
    mark[1].words.w1 = (GfxW1)gdx_course_model_gen;
}

void gdx_course_model_gen_bump(void) {
    gdx_course_model_gen++;
}
