/* gdx3ds_native_cfg.c — the [native] ini section reader (gdx3ds_native_cfg.h). */
#include "gdx3ds_native_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

int gdx3ds_native_ready(void) {
    if (&gdx3ds_config_get_int == NULL) {
        return 0;
    }
    if (&gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return 0;
    }
    return 1;
}

int gdx3ds_native_raw(const char* key, int fallback) {
    if (!gdx3ds_native_ready()) {
        return fallback;
    }
    return gdx3ds_config_get_int("native", key, fallback);
}

/* The levers the `all` master covers: the Tier-2 set (native paths that bypass the
 * interpreter). `interp` is deliberately NOT here -- it changes WHICH interpreter runs, not
 * what it does, and is always explicit (integration-verify-2026-09-17 §5). */
static const char* const kTier2Levers[] = { "track", "machines", "fx_sink", "fx_sprites", "fx_hud",
                                            "ownfold" };

int gdx3ds_native_is_tier2(const char* key) {
    size_t i;
    for (i = 0; i < sizeof(kTier2Levers) / sizeof(kTier2Levers[0]); i++) {
        if (strcmp(kTier2Levers[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* "The batch-for-batch oracle is running", i.e. every 2D fold must be off on BOTH sides.
 *
 * `fx_hud` folds LUS's own 2D batches across prim/env changes; `ownfold` is the SAME fold on
 * the owned walker (gdx_tri.cpp NoteRegChange, absorbing the prim/env split). `interp_verify`
 * compares the two interpreters BATCH FOR BATCH, so a fold enabled on one side and not the
 * other reports a mismatch on every folded 2D batch -- measured at 179 tasks over the canonical
 * replay, with all 6002 frame hashes identical and every Tier-2 verifier clean
 * (verifymatrix-progress.md). Verify mode therefore turns BOTH folds off and says so.
 *
 * audit-2026-09-24 finding 5: ownfold was made Tier-2 (so `all=1` raises it) in the same merge
 * window that added this force-off for fx_hud only, which left `all=1 interp=1 interp_verify=1`
 * comparing a FOLDED owned batch against a deliberately UNFOLDED LUS one -- the same asymmetry
 * with the sides swapped. And the test was `interp_verify == 1` while gdx_rt_glue.cpp:263 arms
 * the oracle on `!= 0`, so interp_verify=2 reopened it silently; both tests are `!= 0` now so
 * the predicate cannot be narrower than the thing it protects. */
int gdx3ds_native_verify_forces_fold_off(void) {
    return gdx3ds_native_ready() && gdx3ds_config_get_int("native", "interp_verify", 0) != 0 &&
           gdx3ds_config_get_int("native", "interp", 0) != 0;
}

int gdx3ds_native_lever(const char* key, int fallback) {
    int v;
    if (!gdx3ds_native_ready()) {
        return fallback;
    }
    v = gdx3ds_config_get_int("native", key, fallback);
    if (v == 0 && gdx3ds_native_is_tier2(key) && gdx3ds_config_get_int("native", "all", 0) == 1) {
        v = 1;
    }
    if (v != 0 && (strcmp(key, "fx_hud") == 0 || strcmp(key, "ownfold") == 0) &&
        gdx3ds_native_verify_forces_fold_off()) {
        v = 0;
    }
    return v;
}

int gdx3ds_native_fx_mask(void) {
    int m = 0;
    if (gdx3ds_native_lever("fx_sink", 0) != 0) {
        m |= 1;
    }
    if (gdx3ds_native_lever("fx_sprites", 0) != 0) {
        m |= 2;
    }
    if (gdx3ds_native_lever("fx_hud", 0) != 0) {
        m |= 4;
    }
    return m;
}

void gdx3ds_native_boot_receipt(void) {
    static int sDone = 0;
    char buf[320];
    if (sDone || !gdx3ds_native_ready() || &gdx_dbg_logf == NULL) {
        return;
    }
    sDone = 1;
    /* Every value is the RESOLVED state (the `all` master applied to the Tier-2 levers; interp
     * as written), so the receipt says what runs, not what the ini spelled. */
    snprintf(buf, sizeof(buf),
             "[native] all=%d interp=%d machines=%d track=%d fx_sink=%d fx_sprites=%d fx_hud=%d ownfold=%d fx=%d "
             "census trk=%d fx=%d ncensus=%d (resolved: all=tier2 only, interp explicit; ini latched at boot)%s",
             gdx3ds_native_raw("all", 0), gdx3ds_native_lever("interp", 0), gdx3ds_native_lever("machines", 0),
             gdx3ds_native_lever("track", 0), gdx3ds_native_lever("fx_sink", 0), gdx3ds_native_lever("fx_sprites", 0),
             gdx3ds_native_lever("fx_hud", 0), gdx3ds_native_lever("ownfold", 0), gdx3ds_native_fx_mask(),
             gdx3ds_native_raw("trkcensus", 0), gdx3ds_native_raw("fxcensus", 0), gdx3ds_native_raw("ncensus", 0),
             gdx3ds_native_verify_forces_fold_off()
                 ? " [fx_hud+ownfold forced 0: interp_verify compares batch for batch, so neither side may fold]"
                 : "");
    gdx_dbg_logf("%s\n", buf);
}
