/* gdx3ds_dlb_shim.h — C5TICK: the decomp side of the per-tick display-list build (dlb)
 * split and of the hollow-tick lever. Included by game.c, race.c and racer.c
 * (decomp-3ds-c5tick-dlb.patch) under `#if defined(PORT) && defined(GDX_PLATFORM_3DS)`;
 * everywhere else the macros are empty and the game is stock.
 *
 * GDX_DLB(sec): a section boundary inside func_80069698 (the game's DL build). Time from one
 *   marker to the next is charged to the section the first one names; with
 *   `[debug] dlwrite = 1` the same boundaries delimit the write-set check (which bytes of the
 *   game's static memory a section changed). One call + one int test when both are off.
 *
 * GDX_DL_SKIP(sec, view): 1 when section `sec` of viewport `view` must not be built, because
 *   (a) this tick's display list will be DROPPED unwalked (frameskip skipped the tick and
 *       `[debug] dl_hollow = 1`: a HOLLOW tick), or
 *   (b) on a rendered tick, a native path has declared that it replaces the section
 *       (gdx3ds_c5tick_consume(); the stage's stub may append the few commands it still needs,
 *       which is why the macro takes `gfx` by address).
 *   Only side-effect-free sections can ever skip (the port refuses the rest): the draw code the
 *   game's state depends on -- the racer visibility pass, the intro speed clamp, the clear
 *   countdown, the HUD with its sounds -- always runs. Hollow is scoped to Race_Draw
 *   (RHEAD .. TRANS). Proof and list: port/3ds/patches/README.md, decomp-3ds-c5tick-dlb.patch.
 *   When the body section skips, gdx_dl_poses() (`[debug] dl_hollow_poses`, default 1) still
 *   builds every visible machine's model matrix (GfxPool Mtx + racer->modelMatrix): the pose
 *   data path a skipped-tick pose replay reads. Camera matrices come from the camera update.
 * GDX_DL_SKIP_LABEL(l): the jump target a skipped section lands on (`l:;`, present in every
 *   build so the gotos resolve off-3DS too).
 *
 * Section ids are shared with port/3ds/gdx3ds_c5tick.c (kSecNames, kSkippable); keep them in step. */
#ifndef GDX3DS_DLB_SHIM_H
#define GDX3DS_DLB_SHIM_H

#define GDX_DLB_SEC_OTHER 0   /* func_80069698 entry: a non-race mode's draw function */
#define GDX_DLB_SEC_RHEAD 1   /* Race_Draw: clear + setup DLs + colour image */
#define GDX_DLB_SEC_SKY 2     /* Background_Draw */
#define GDX_DLB_SEC_TRK 3     /* Course_Draw */
#define GDX_DLB_SEC_DRESS 4   /* Course_GadgetsDraw */
#define GDX_DLB_SEC_RMID 5    /* ucode + segment table + projection matrix */
#define GDX_DLB_SEC_CAM 6     /* Camera_Draw */
#define GDX_DLB_SEC_VIS 7     /* Racer_Draw: camera setup + the racer visibility / LOD pass */
#define GDX_DLB_SEC_SHD 8     /* Racer_Draw: shadows */
#define GDX_DLB_SEC_BODY 9    /* Racer_Draw: highlight, lights, machine bodies, ghosts */
#define GDX_DLB_SEC_BOOST 10  /* Racer_Draw: booster flames */
#define GDX_DLB_SEC_EFX 11    /* Racer_Draw: explosions, debris, sparks (the fx bracket) */
#define GDX_DLB_SEC_RHUD 12   /* Racer_Draw: countdown sign, markers, approach warning */
#define GDX_DLB_SEC_P1HUD 13  /* Menus_Player1SpecialDraw */
#define GDX_DLB_SEC_MENUS 14  /* Menus_Draw */
#define GDX_DLB_SEC_TRANS 15  /* Transition_Draw */
#define GDX_DLB_SEC_END 16    /* func_80069698 return: closes the tick's build */
#define GDX_DLB_SEC_COUNT 17

#if defined(PORT) && defined(GDX_PLATFORM_3DS)
extern void gdx_dlb_mark(int sec);
extern int gdx_dl_skip(int sec, int view, void** gfx);
extern int gdx_dl_poses(void);
#define GDX_DLB(sec) gdx_dlb_mark(GDX_DLB_SEC_##sec)
#define GDX_DL_SKIP(sec, view) gdx_dl_skip(GDX_DLB_SEC_##sec, (view), (void**) &gfx)
#else
#define GDX_DLB(sec)
#define GDX_DL_SKIP(sec, view) 0
#endif
#define GDX_DL_SKIP_LABEL(label) label:;

#endif /* GDX3DS_DLB_SHIM_H */
