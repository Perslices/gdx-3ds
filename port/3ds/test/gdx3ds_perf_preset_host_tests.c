/* port/3ds/test/gdx3ds_perf_preset_host_tests.c — [perf] preset (port/3ds/gdx3ds_perf_preset.c)
 * on the build machine (tools/ci-3ds.sh, "host unit tests" block). NEXT.md C4p.
 *
 * The contract being pinned: a preset only changes DEFAULTS. On a New 3DS `auto` must leave
 * every [perf] key at its shipped value (the New 3DS is not allowed to see this lever at
 * all), on an Old 3DS `auto` must select the o3ds values, and an explicit ini key must beat
 * the preset in both directions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h> /* port/3ds/test/host/3ds.h on this include path */

#include "gdx3ds_perf_preset.h"

/* ------------------------------------------------------------------ stubs -- */

u64 gdx3ds_test_tick = 0;

static int sModelIsNew = 1;
static int sModelProbeFails = 0;

Result APT_CheckNew3DS(bool* out) {
    if (sModelProbeFails) {
        return 0xC8A0CFFCu; /* any failure code */
    }
    *out = sModelIsNew ? true : false;
    return 0;
}

static char sLastLine[256];

Result svcOutputDebugString(const char* str, s32 length) {
    size_t n = (size_t)length < sizeof(sLastLine) - 1 ? (size_t)length : sizeof(sLastLine) - 1;
    memcpy(sLastLine, str, n);
    sLastLine[n] = '\0';
    return 0;
}

void gdx3ds_filelog_write(const char* msg, size_t len) {
    (void)msg;
    (void)len;
}

#define CFG_CAP 8
static struct {
    const char* key;
    const char* value;
} sCfg[CFG_CAP];
static int sCfgN = 0;

static void CfgClear(void) {
    sCfgN = 0;
}

static void CfgSet(const char* key, const char* value) {
    if (sCfgN < CFG_CAP) {
        sCfg[sCfgN].key = key;
        sCfg[sCfgN].value = value;
        sCfgN++;
    }
}

static const char* CfgFind(const char* section, const char* key) {
    if (strcmp(section, "perf") != 0) {
        return NULL;
    }
    for (int i = 0; i < sCfgN; i++) {
        if (strcmp(sCfg[i].key, key) == 0) {
            return sCfg[i].value;
        }
    }
    return NULL;
}

int gdx3ds_config_get_int(const char* section, const char* key, int fallback) {
    const char* v = CfgFind(section, key);
    return v != NULL ? atoi(v) : fallback;
}

const char* gdx3ds_config_get_string(const char* section, const char* key, const char* fallback) {
    const char* v = CfgFind(section, key);
    return v != NULL ? v : fallback;
}

/* ------------------------------------------------------------------ cases -- */

static int sFailures = 0;

static void Check(int cond, const char* what) {
    if (!cond) {
        printf("  FAIL: %s\n", what);
        sFailures++;
    }
}

static void Boot(int modelIsNew) {
    sModelIsNew = modelIsNew;
    gdx3ds_perf_preset_test_reset();
    gdx3ds_perf_preset_init();
}

static void CheckStockDefaults(const char* label) {
    printf("%s: %s\n", label, sLastLine);
    Check(gdx3ds_perf_preset_o3ds() == 0, "preset must be off");
    Check(gdx3ds_perf_get_int("rival_detail", 0) == 0, "rival_detail must keep its shipped default");
    Check(gdx3ds_perf_get_int("rival_tier_max", 2) == 2, "rival_tier_max must keep its shipped default");
    Check(gdx3ds_perf_get_int("rival_auto_hi_x10", 150) == 150, "hi must keep its shipped default");
    Check(gdx3ds_perf_get_int("rival_auto_lo_x10", 90) == 90, "lo must keep its shipped default");
    Check(gdx_track_draw_distance_pct() == 100, "draw distance must stay stock");
    Check(gdx_rival_lod6_skip() == 0, "LOD-6 emission must stay stock");
}

int main(void) {
    CfgClear();
    Boot(1);
    CheckStockDefaults("new3ds auto");

    CfgClear();
    Boot(0);
    printf("old3ds auto: %s\n", sLastLine);
    Check(gdx3ds_perf_preset_o3ds() == 1, "auto must select o3ds on an Old 3DS");
    Check(strstr(sLastLine, "preset=o3ds (auto: model=old3ds)") != NULL, "the receipt must say why");
    Check(gdx3ds_perf_get_int("rival_tier_max", 2) == 3, "o3ds opens LOW");
    Check(gdx3ds_perf_get_int("rival_auto_hi_x10", 150) > 150, "o3ds moves the RAISE line up");
    Check(gdx_track_draw_distance_pct() == 100, "o3ds keeps the stock draw distance (pop-in, hw 2026-09-29)");
    Check(gdx_rival_lod6_skip() == 1, "o3ds skips the empty LOD-6 emission");
    Check(gdx3ds_perf_get_int("rival_auto_dwell", 15) == 15, "a key outside the preset keeps its default");

    CfgClear();
    CfgSet("preset", "off");
    Boot(0);
    CheckStockDefaults("old3ds preset=off");

    CfgClear();
    CfgSet("preset", "o3ds");
    Boot(1);
    printf("new3ds preset=o3ds: %s\n", sLastLine);
    Check(gdx3ds_perf_preset_o3ds() == 1, "an explicit o3ds applies on any console");

    CfgClear();
    CfgSet("rival_detail", "0");
    CfgSet("draw_distance", "100");
    Boot(0);
    printf("old3ds auto + explicit keys: %s\n", sLastLine);
    Check(gdx3ds_perf_get_int("rival_detail", 0) == 0, "an explicit key beats the preset");
    Check(gdx_track_draw_distance_pct() == 100, "an explicit draw_distance beats the preset");
    Check(gdx3ds_perf_get_int("rival_tier_max", 2) == 3, "the rest of the preset still applies");

    CfgClear();
    CfgSet("preset", "off");
    CfgSet("draw_distance", "10");
    Boot(1);
    Check(gdx_track_draw_distance_pct() == 40, "draw distance is clamped at 40");
    CfgClear();
    CfgSet("preset", "off");
    CfgSet("draw_distance", "150");
    Boot(1);
    Check(gdx_track_draw_distance_pct() == 100, "the port hook can only shorten the track");

    /* n3ds: preset = n60 carries the n60 levers (config layer); its [perf] LOD keys stay stock on
     * either console. */
    CfgClear();
    CfgSet("preset", "n60");
    Boot(1);
    printf("new3ds preset=n60: %s\n", sLastLine);
    CheckStockDefaults("new3ds preset=n60");
    Check(strstr(sLastLine, "preset=n60 (ini") != NULL, "the receipt names the n60 preset");
    CfgClear();
    CfgSet("preset", "n3ds60");
    Boot(0);
    CheckStockDefaults("old3ds preset=n3ds60");
    Check(gdx3ds_perf_preset_o3ds() == 0, "n60 replaces the o3ds LOD preset");

    CfgClear();
    sModelProbeFails = 1;
    Boot(0);
    CheckStockDefaults("model probe failed");
    sModelProbeFails = 0;

    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
