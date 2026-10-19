/* port/3ds/gdx3ds_force_mode.c — nimenu: [debug] force_mode, a one-shot boot-target override.
 *
 * WHY. Three of the MENU-capture conversion's remaining draw functions have no menu path and
 * no finishable replay: `Credits_Draw`/`EndingCutscene_Draw` are reachable only after finishing
 * the sixth course of a GP cup (decomp/src/game/game.c's `MENU_CHANGE_NEXT_COURSE` case), which
 * no scripted replay survives (docs/research/native/replays-progress.md, "Unreachable, and
 * why"), and `EADDemo_Draw` is an Expansion-Kit-only screen gated behind beating every DD staff
 * ghost on every course (decomp/src/overlays/ovl_i3/menus.c). That doc named the fix: "this
 * needs either a save file with a cleared cup or a debug mode-jump lever." This is the lever.
 *
 * WHAT. `decomp-native-force-boot-mode.patch` adds one hook inside game.c's
 * `GAMEMODE_CHANGE_INIT` case -- reached exactly once per process, only when `gGameMode == -1`
 * (the very first mode transition after `Game_Init`), so it is structurally a one-shot: there is
 * no later call site that could re-trigger it and hijack a player-driven mode change. With
 * `[debug] force_mode` set, the game boots directly into that `GameMode` (fzx_game.h) instead of
 * `GAMEMODE_TITLE`, running the normal `..._Init`/`..._Update`/`..._Draw` triple for it. Checked
 * against each target's own `_Init` before use (nimenu-progress.md): `EndingCutscene_Init`,
 * `Credits_Init` and `EADDemo_Init` all fully own the state they read (course/camera/racer
 * re-init, a vehicle-artwork scroll, a fixed background texture respectively) rather than
 * depending on a race that was actually played, so forcing straight into them from boot is safe
 * -- it will not be the game's own standings data (nothing played a GP cup), but it is the real
 * draw function on its real code path, which is what a MENU-capture conversion needs to measure
 * and gate against.
 *
 * DEFAULT -1 = off (the hook is one cached int test; `gQueuedGameMode` is left exactly as the
 * game set it, i.e. GAMEMODE_FLX_TITLE at boot). MEASUREMENT ONLY: never set outside a capture
 * run, and both legs of any A/B that uses it must carry the same value. */

#include "gdx3ds_config.h"

extern void gdx_dbg_logf(const char* fmt, ...);

static int sForceMode = -2; /* -2 = not yet read from the ini */

int gdx_debug_force_boot_mode(void) {
    if (sForceMode == -2) {
        sForceMode = gdx3ds_config_get_int("debug", "force_mode", -1);
        if (sForceMode >= 0) {
            gdx_dbg_logf("[force_mode] armed: booting directly into GameMode %d instead of TITLE "
                         "(measurement lever, one-shot)\n", sForceMode);
        }
    }
    return sForceMode;
}
