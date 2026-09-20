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
static const char* const kTier2Levers[] = { "track", "machines", "fx_sink", "fx_sprites", "fx_hud" };

int gdx3ds_native_is_tier2(const char* key) {
    size_t i;
    for (i = 0; i < sizeof(kTier2Levers) / sizeof(kTier2Levers[0]); i++) {
        if (strcmp(kTier2Levers[i], key) == 0) {
            return 1;
        }
    }
    return 0;
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
    char buf[256];
    if (sDone || !gdx3ds_native_ready() || &gdx_dbg_logf == NULL) {
        return;
    }
    sDone = 1;
    /* Every value is the RESOLVED state (the `all` master applied to the Tier-2 levers; interp
     * as written), so the receipt says what runs, not what the ini spelled. */
    snprintf(buf, sizeof(buf),
             "[native] all=%d interp=%d machines=%d track=%d fx_sink=%d fx_sprites=%d fx_hud=%d fx=%d "
             "census trk=%d fx=%d (resolved: all=tier2 only, interp explicit; ini latched at boot)",
             gdx3ds_native_raw("all", 0), gdx3ds_native_lever("interp", 0), gdx3ds_native_lever("machines", 0),
             gdx3ds_native_lever("track", 0), gdx3ds_native_lever("fx_sink", 0), gdx3ds_native_lever("fx_sprites", 0),
             gdx3ds_native_lever("fx_hud", 0), gdx3ds_native_fx_mask(), gdx3ds_native_raw("trkcensus", 0),
             gdx3ds_native_raw("fxcensus", 0));
    gdx_dbg_logf("%s\n", buf);
}
