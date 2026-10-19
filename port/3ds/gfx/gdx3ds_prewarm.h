/* port/3ds/gfx/gdx3ds_prewarm.h -- N60PREWARM `[native] prewarm` (DEFAULT 0): move a race's
 * first-use work into the frame the player cannot see (docs/research/native/n60prewarm-progress.md).
 *
 * WHY. On the Old 3DS every race pays its first-use work on visible frames: the first rendered race
 * frame (~136 ms: 86 texture uploads, the NRR categories' first-sight learning), then, in the intro
 * and all through the race, a legacy "record pass" + mesh bake the first time each (machine, LOD,
 * pass) is drawn (n60mach §3.2: 2.8-5.2 ms mesh spikes in the intro, ~100 bakes over a GP race).
 *
 * THE OPAQUE FRAME. A race appears through TRANSITION_TYPE_FADE: the display list built in the
 * load tick (Transition_AppearSet + the first Transition_Update) is drawn under a black fill at
 * alpha 255 -- every pixel the race draws inside the fade rectangle is overwritten, exactly. That
 * one frame is the prewarm's window:
 *
 *   bit 1  machines: the BODY stub (Racer_NrrMachStub) appends, after the section's stock prologue,
 *          the stock commands of every (machine in the race, LOD 1-5) body pass without a complete
 *          NRR record, each followed by a far-speck draw (the speck material [P][SPECK]) and led by
 *          one speck with no predecessor -- all under one pose in front of the camera (inside the
 *          frustum, so the mesh path draws, bakes and records it; under the fade, so nothing shows).
 *          The marker arms the recorder for them: the record pass, the mesh bakes, the retained
 *          buffers, the material tables and the machines' texture uploads all happen here.
 *   bit 2  highlight passes: the same for the attack-highlight pass (D_303A7D8 + blend colour), at the
 *          end of the section, followed by the body prologue again.
 *   bit 4  render the opaque frame: veto the frameskip governor's skip of the iteration that walks the
 *          load tick's list (and the hollow build of that list), through the oracle's "wants this
 *          frame" hooks. On hardware the frame right after the ~240 ms load is otherwise skipped
 *          (auto catch-up), and the first rendered race frame -- a visible fade step -- pays the
 *          texture burst and every category's first-sight learning.
 *
 * Exactness: the prewarm draws write no game state (one file-static Mtx); the frame they are in is
 * covered by the opaque fade; records are what the stock draws of those lists produce (the same
 * recorder, the same order rules as a first sight). Receipts: `[prewarm] init mask=`, one
 * `[prewarm] mach ...` line per prewarm frame, `[prewarm] veto ...` per vetoed skip. */
#ifndef GDX3DS_PREWARM_H
#define GDX3DS_PREWARM_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_PREWARM_MACH 1u
#define GDX_PREWARM_HL 2u
#define GDX_PREWARM_RENDER 4u

/* Game side (racer.c Racer_NrrMachStub, decomp-3ds-prewarm.patch). plan: the bits to prewarm in
 * this view's section now (0 = none: lever off, not the opaque fade frame, view != 0, nothing
 * missing). bodies: after the section prologue; nopredOk = no highlight draw precedes it this
 * frame. hl: at the section's end (re-emits the prologue). Both return the new cursor. */
int gdx3ds_prewarm_mach_plan(int view, int lod6Skip);
void* gdx3ds_prewarm_mach_bodies(void* gfx, int view, int lod6Skip, int nopredOk);
void* gdx3ds_prewarm_mach_hl(void* gfx, int view, int fogR, int fogG, int fogB);

/* Oracle hooks (gdx3ds_oracle.c): 1 when the list built in the current tick (or, at an iteration's
 * begin, the one it walks) is a race's opaque appear frame and bit 4 is on. Game-state reads only. */
int gdx3ds_prewarm_wants_render(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PREWARM_H */
