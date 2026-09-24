/* gdx3ds_native_cfg_host_tests.c — the [native] lever resolver (gdx3ds_native_cfg.c).
 *
 * Three rules live in gdx3ds_native_lever() and each one has burned a session:
 *   1. `all=1` covers the Tier-2 set only. `interp` is read as written — the 2026-09-17
 *      integration gate lost the course for a whole race because `all=1` implied `interp=1`.
 *   2. `all=1` raises a Tier-2 lever that reads 0, and never lowers one that reads 1.
 *   3. `interp_verify=1` with `interp=1` forces `fx_hud` to 0. fx_hud folds LUS's own 2D
 *      batches across prim/env changes and the owned walker has no equivalent, so with both
 *      on, interp_verify's batch-for-batch oracle reports a mismatch on every folded 2D
 *      batch — measured at 179 tasks over the canonical replay, with all 6002 frame hashes
 *      identical (docs/research/native/verifymatrix-progress.md).
 *
 * Build line is in tools/ci-3ds.sh. The ini is stubbed here rather than linking
 * gdx3ds_config.c, so a case is one table of key/value pairs.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../gdx3ds_native_cfg.h"

/* ---- ini stub ------------------------------------------------------------------------ */

typedef struct {
    const char* key;
    int value;
} Kv;

static const Kv* sIni = NULL;
static int sIniCount = 0;
static int sLoaded = 1;

int gdx3ds_config_get_int(const char* section, const char* key, int fallback) {
    int i;
    if (strcmp(section, "native") != 0) {
        return fallback;
    }
    for (i = 0; i < sIniCount; i++) {
        if (strcmp(sIni[i].key, key) == 0) {
            return sIni[i].value;
        }
    }
    return fallback;
}

int gdx3ds_config_loaded(void) {
    return sLoaded;
}

/* Mach-O refuses a weak undefined at link, so the receipt sink is a real symbol here
 * (interp-progress.md, "Gotchas"). It doubles as the boot-receipt assertion below. */
static char sReceipt[512];

void gdx_dbg_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sReceipt, sizeof(sReceipt), fmt, ap);
    va_end(ap);
}

/* ---- harness ------------------------------------------------------------------------- */

static int sFailures = 0;

static void check(const char* what, int got, int want) {
    if (got != want) {
        printf("FAIL %s: got %d want %d\n", what, got, want);
        sFailures++;
    }
}

static void use(const Kv* ini, int n) {
    sIni = ini;
    sIniCount = n;
}

#define USE(a) use((a), (int)(sizeof(a) / sizeof((a)[0])))

int main(void) {
    /* 1. all=1 is the Tier-2 set only; interp is read as written. */
    {
        static const Kv ini[] = { { "all", 1 } };
        USE(ini);
        check("all=1 -> track", gdx3ds_native_lever("track", 0), 1);
        check("all=1 -> machines", gdx3ds_native_lever("machines", 0), 1);
        check("all=1 -> fx_sink", gdx3ds_native_lever("fx_sink", 0), 1);
        check("all=1 -> fx_sprites", gdx3ds_native_lever("fx_sprites", 0), 1);
        check("all=1 -> fx_hud", gdx3ds_native_lever("fx_hud", 0), 1);
        check("all=1 leaves interp alone", gdx3ds_native_lever("interp", 0), 0);
        check("all=1 fx mask", gdx3ds_native_fx_mask(), 7);
    }

    /* An explicit interp=1 stands on its own, and all=0 changes nothing. */
    {
        static const Kv ini[] = { { "all", 0 }, { "interp", 1 } };
        USE(ini);
        check("interp=1 explicit", gdx3ds_native_lever("interp", 0), 1);
        check("all=0 -> track stays 0", gdx3ds_native_lever("track", 0), 0);
    }

    /* 2. A Tier-2 lever set to 1 by hand survives all=0. */
    {
        static const Kv ini[] = { { "all", 0 }, { "track", 1 } };
        USE(ini);
        check("track=1 with all=0", gdx3ds_native_lever("track", 0), 1);
    }

    /* 3. interp_verify=1 with interp=1 forces fx_hud off, and touches nothing else. */
    {
        static const Kv ini[] = { { "interp", 1 }, { "interp_verify", 1 }, { "fx_hud", 1 },
                                  { "fx_sink", 1 }, { "fx_sprites", 1 }, { "track", 1 },
                                  { "machines", 1 }, { "ownfold", 1 } };
        USE(ini);
        check("verify forces fx_hud off", gdx3ds_native_lever("fx_hud", 0), 0);
        check("verify forces ownfold off", gdx3ds_native_lever("ownfold", 0), 0);
        check("verify flag", gdx3ds_native_verify_forces_fold_off(), 1);
        check("fx_sink untouched", gdx3ds_native_lever("fx_sink", 0), 1);
        check("fx_sprites untouched", gdx3ds_native_lever("fx_sprites", 0), 1);
        check("track untouched", gdx3ds_native_lever("track", 0), 1);
        check("machines untouched", gdx3ds_native_lever("machines", 0), 1);
        check("fx mask drops the hud bit", gdx3ds_native_fx_mask(), 3);
    }

    /* The same through the all master: all=1 raises fx_hud AND ownfold (both are Tier-2 and
     * both are the same 2D fold, one per interpreter), verify lowers both again. Without this
     * the oracle compares a folded owned batch against a deliberately unfolded LUS one
     * (audit-2026-09-24 finding 5a). */
    {
        static const Kv ini[] = { { "all", 1 }, { "interp", 1 }, { "interp_verify", 1 } };
        USE(ini);
        check("all=1 + verify -> fx_hud off", gdx3ds_native_lever("fx_hud", 0), 0);
        check("all=1 + verify -> ownfold off", gdx3ds_native_lever("ownfold", 0), 0);
        check("all=1 + verify -> fx_sink on", gdx3ds_native_lever("fx_sink", 0), 1);
    }

    /* interp_verify=2 must behave exactly like =1: gdx_rt_glue.cpp arms the oracle on != 0,
     * and the force-off used to test == 1, so =2 reopened the asymmetry on BOTH levers with
     * no receipt saying so (audit-2026-09-24 finding 5b). */
    {
        static const Kv ini[] = { { "all", 1 }, { "interp", 1 }, { "interp_verify", 2 } };
        USE(ini);
        check("interp_verify=2 flag", gdx3ds_native_verify_forces_fold_off(), 1);
        check("interp_verify=2 -> fx_hud off", gdx3ds_native_lever("fx_hud", 0), 0);
        check("interp_verify=2 -> ownfold off", gdx3ds_native_lever("ownfold", 0), 0);
        check("interp_verify=2 -> fx_sink on", gdx3ds_native_lever("fx_sink", 0), 1);
    }

    /* ownfold is raised by all=1 like any other Tier-2 lever when the oracle is NOT running. */
    {
        static const Kv ini[] = { { "all", 1 }, { "interp", 1 } };
        USE(ini);
        check("all=1 without verify -> ownfold on", gdx3ds_native_lever("ownfold", 0), 1);
        check("all=1 without verify -> fx_hud on", gdx3ds_native_lever("fx_hud", 0), 1);
    }

    /* interp_verify without interp does nothing: gdx_ninterp_job_run returns 0 when
     * interp is off, so verify mode never runs and the fold must be left alone. */
    {
        static const Kv ini[] = { { "interp", 0 }, { "interp_verify", 1 }, { "fx_hud", 1 },
                                  { "ownfold", 1 } };
        USE(ini);
        check("verify without interp leaves fx_hud", gdx3ds_native_lever("fx_hud", 0), 1);
        check("verify without interp leaves ownfold", gdx3ds_native_lever("ownfold", 0), 1);
        check("verify flag off without interp", gdx3ds_native_verify_forces_fold_off(), 0);
    }

    /* Before the ini is read, every lever is its compiled-in default. */
    {
        static const Kv ini[] = { { "all", 1 }, { "fx_hud", 1 } };
        USE(ini);
        sLoaded = 0;
        check("not ready -> fallback", gdx3ds_native_lever("track", 0), 0);
        check("not ready -> fx_hud fallback", gdx3ds_native_lever("fx_hud", 0), 0);
        sLoaded = 1;
    }

    /* The boot receipt has to say that the fold was forced off, or the next agent reads
     * `fx_hud=0` and thinks they typed it. */
    {
        static const Kv ini[] = { { "all", 1 }, { "interp", 1 }, { "interp_verify", 1 } };
        USE(ini);
        sReceipt[0] = '\0';
        gdx3ds_native_boot_receipt();
        if (strstr(sReceipt, "fx_hud+ownfold forced 0") == NULL) {
            printf("FAIL boot receipt does not report the forced fold: %s\n", sReceipt);
            sFailures++;
        }
        if (strstr(sReceipt, "fx_hud=0") == NULL) {
            printf("FAIL boot receipt does not print the resolved fx_hud: %s\n", sReceipt);
            sFailures++;
        }
    }

    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
