/* port/3ds/gdx3ds_hudtop_custom.c -- nihudtop Phase 3 dev-only lever: force
 * GDX_HUD_PLACEMENT_CUSTOM's per-element top-screen mask from two [debug] ini keys, so the new
 * gdx_hud_element_on_top gate can be exercised on-device (emulator captures, docs/research/
 * native/nihudtop-progress.md) before any real per-element UI exists to drive it.
 *
 * Deliberately its OWN translation unit, not port/3ds/gdx3ds_bottomhud.c: that file owns the
 * shipped MODE-chip lever (`[hud] placement`, 0..GDX_HUD_PLACEMENT_MODE_COUNT-1) and is being
 * edited concurrently on native/nibottom for the bottom-screen DASH; this lever is independent of
 * it by construction (gdx_hud_set_custom_top_mask is checked in port/gdx_hud_placement.c BEFORE
 * gdx_hud_placement_mode() is ever read -- see that file), so it needs no changes there and none
 * are made here.
 *
 * [debug] hud_custom_active (default 0): nonzero forces every gdx_hud_element_on_top(elem, 1)
 *   answer to come from hud_custom_top_mask's bit `elem` instead of the real mode (ALL_TOP/SPLIT/
 *   ALL_BOTTOM/whatever the chip is parked on). numPlayers>1 is never affected either way
 *   (FORCED ALL_TOP IN 2-4P, enforced inside gdx_hud_top_element_mask itself).
 * [debug] hud_custom_top_mask (default 0, parsed as a C integer literal -- "0x..." hex or plain
 *   decimal both work via strtoul's base 0): bit i is GDX_HUD_ELEM_i, port/gdx_hud_placement.h.
 *
 * Read once at boot, like gdx3ds_bottomhud_init's own [debug]/[hud] keys -- this lever has no
 * runtime UI yet, so there is nothing to re-latch per frame.
 *
 * MERGE-TIME NOTE (native/n60lead integration): main_3ds.cpp's boot sequence calls
 * gdx3ds_menu_init() -- which calls gdx3ds_bottomhud_init(), the real CUSTOM-mode driver, via
 * DashSyncCustomTopMask() -- BEFORE this file's own init. With [debug] hud_custom_active unset
 * (its default, 0, true for every build except one deliberately exercising this dev lever), an
 * earlier unconditional call to gdx_hud_set_custom_top_mask(0, 0) here would silently clobber
 * whatever gdx3ds_bottomhud_init() had just set -- including the [debug] hud_edit_start=1
 * verification path, which forces CUSTOM mode (and a non-trivial mask) on at boot specifically so
 * it can be captured. So: only call gdx_hud_set_custom_top_mask at all when this lever is
 * EXPLICITLY active; otherwise leave the real DASH-driven state (gdx3ds_bottomhud.c's own
 * DashSyncCustomTopMask) completely alone. */
#include <stdlib.h>

#include "gdx3ds_config.h" // port/3ds/os/ -- gdx3ds_config_get_bool/_get_string

void gdx_hud_set_custom_top_mask(unsigned mask, int customActive); // port/gdx_hud_placement.c

void gdx3ds_hudtop_custom_init(void) {
    int active = gdx3ds_config_get_bool("debug", "hud_custom_active", 0);
    const char* maskStr;
    unsigned mask;

    if (!active) {
        return; /* never fight gdx3ds_bottomhud_init's own real custom-mask state */
    }

    maskStr = gdx3ds_config_get_string("debug", "hud_custom_top_mask", "0");
    mask = (unsigned) strtoul(maskStr, NULL, 0);

    gdx_hud_set_custom_top_mask(mask, active);
}
