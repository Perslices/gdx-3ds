/* gdx_save_replace_tests.c -- host test of port/gdx_save_replace.c (n60save).
 *
 * The filesystem is a FAT-like stub: rename() onto an existing file FAILS (as the 3DS SD archive
 * does through libctru), and "power" can be cut after any N-th filesystem operation, freezing the
 * on-disk state. Every flush is then followed by a boot (gdx_save_recover) and the test asserts:
 *   - the boot always finds a COMPLETE copy, never a partial one, and never nothing once a save
 *     has ever completed;
 *   - the copy it finds is never older than the last flush that completed;
 *   - a flush that completes leaves only FINAL (or FINAL + BAK with keepBackup) on disk.
 * The same matrix runs on a POSIX-like stub (rename replaces) with tryDirectFirst=1, and with a
 * power cut during the boot recovery itself. */
#include "gdx_save_replace.h"

#include <stdio.h>
#include <string.h>

#define MAX_FILES 8
#define PATH_MAX_LEN 32

typedef struct StubFile {
    char path[PATH_MAX_LEN];
    int version;  /* save generation stored in the file */
    int complete; /* 0 = torn write (wrong size on a real card) */
} StubFile;

typedef struct StubFs {
    StubFile files[MAX_FILES];
    int count;
    int renameReplaces; /* 1 = POSIX rename semantics, 0 = FAT/3DS */
    int opsBudget;      /* operations left before the power cut; <0 = unlimited */
    int powerLost;
} StubFs;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        sChecks++;                               \
        if (!(cond)) {                           \
            sFailures++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

static const char* kFinal = "fzerox.sav";
static const char* kTmp = "fzerox.sav.tmp";
static const char* kBak = "fzerox.sav.bak";

static StubFile* StubFind(StubFs* fs, const char* path) {
    for (int i = 0; i < fs->count; i++) {
        if (strcmp(fs->files[i].path, path) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

/* Consumes one unit of power; returns 0 once the power is gone (the operation does not happen). */
static int StubSpend(StubFs* fs) {
    if (fs->powerLost) {
        return 0;
    }
    if (fs->opsBudget == 0) {
        fs->powerLost = 1;
        return 0;
    }
    if (fs->opsBudget > 0) {
        fs->opsBudget--;
    }
    return 1;
}

static void StubDelete(StubFs* fs, StubFile* f) {
    *f = fs->files[fs->count - 1];
    fs->count--;
}

static void StubPut(StubFs* fs, const char* path, int version, int complete) {
    StubFile* f = StubFind(fs, path);
    if (f == NULL) {
        f = &fs->files[fs->count++];
        snprintf(f->path, sizeof(f->path), "%s", path);
    }
    f->version = version;
    f->complete = complete;
}

static int StubRename(void* ctx, const char* from, const char* to) {
    StubFs* fs = (StubFs*) ctx;
    if (!StubSpend(fs)) {
        return -1;
    }
    StubFile* src = StubFind(fs, from);
    StubFile* dst = StubFind(fs, to);
    if (src == NULL) {
        return -1;
    }
    if (dst != NULL) {
        if (!fs->renameReplaces) {
            return -1; /* FAT / 3DS SD archive: 0xC82044BE */
        }
        StubDelete(fs, dst);
        src = StubFind(fs, from);
    }
    snprintf(src->path, sizeof(src->path), "%s", to);
    return 0;
}

static int StubRemove(void* ctx, const char* path) {
    StubFs* fs = (StubFs*) ctx;
    if (!StubSpend(fs)) {
        return -1;
    }
    StubFile* f = StubFind(fs, path);
    if (f == NULL) {
        return -1;
    }
    StubDelete(fs, f);
    return 0;
}

static int StubExists(void* ctx, const char* path) {
    return StubFind((StubFs*) ctx, path) != NULL;
}

static int StubValid(void* ctx, const char* path) {
    StubFile* f = StubFind((StubFs*) ctx, path);
    return f != NULL && f->complete;
}

static GdxSaveFsOps StubOps(StubFs* fs) {
    GdxSaveFsOps ops;
    ops.rename_file = StubRename;
    ops.remove_file = StubRemove;
    ops.file_exists = StubExists;
    ops.file_valid = StubValid;
    ops.ctx = fs;
    return ops;
}

/* The production flush: write TMP (two ops: create torn, then complete+close), then replace. */
static int StubFlush(StubFs* fs, int version, int tryDirectFirst, int keepBackup) {
    if (!StubSpend(fs)) {
        return 0;
    }
    StubPut(fs, kTmp, version, 0);
    if (!StubSpend(fs)) {
        return 0;
    }
    StubPut(fs, kTmp, version, 1);
    GdxSaveFsOps ops = StubOps(fs);
    return gdx_save_replace(&ops, kTmp, kFinal, kBak, tryDirectFirst, keepBackup, NULL) == GDX_SAVE_REPLACE_OK &&
           !fs->powerLost;
}

/* Boot: recover, then read the version FINAL (or the reported path) holds. -1 = no save. */
static int StubBoot(StubFs* fs, int keepBackup) {
    GdxSaveFsOps ops = StubOps(fs);
    int promoted = 0;
    GdxSaveSource src = gdx_save_recover(&ops, kFinal, kTmp, kBak, keepBackup, &promoted);
    const char* path = gdx_save_source_path(src, promoted, kFinal, kTmp, kBak);
    if (path == NULL) {
        return -1;
    }
    StubFile* f = StubFind(fs, path);
    CHECK(f != NULL && f->complete, "boot picked %s (%s) but it is not a complete file", path,
          gdx_save_source_name(src));
    return f != NULL ? f->version : -1;
}

static void Restore(StubFs* fs) {
    fs->powerLost = 0;
    fs->opsBudget = -1;
}

typedef struct Scenario {
    const char* name;
    int renameReplaces;
    int tryDirectFirst;
    int keepBackup;
    int startWithFinal;
    int startWithStaleBak;
    int startWithStaleTmp;
} Scenario;

static void SeedScenario(StubFs* fs, const Scenario* sc) {
    memset(fs, 0, sizeof(*fs));
    fs->renameReplaces = sc->renameReplaces;
    fs->opsBudget = -1;
    if (sc->startWithFinal) {
        StubPut(fs, kFinal, 1, 1);
    }
    if (sc->startWithStaleBak) {
        StubPut(fs, kBak, 0, 1);
    }
    if (sc->startWithStaleTmp) {
        StubPut(fs, kTmp, 0, 0);
    }
}

/* One flush of generation 2 cut after `budget` ops, then a boot (optionally itself cut). */
static void RunCut(const Scenario* sc, int budget, int bootBudget) {
    StubFs fs;
    SeedScenario(&fs, sc);
    const int oldVersion = sc->startWithFinal ? 1 : (sc->startWithStaleBak ? 0 : -1);
    fs.opsBudget = budget;
    const int completed = StubFlush(&fs, 2, sc->tryDirectFirst, sc->keepBackup);
    Restore(&fs);
    if (bootBudget >= 0) {
        fs.opsBudget = bootBudget;
        (void) StubBoot(&fs, sc->keepBackup);
        Restore(&fs);
    }
    const int loaded = StubBoot(&fs, sc->keepBackup);
    if (completed) {
        CHECK(loaded == 2, "[%s] cut=%d boot=%d: flush completed but boot loaded %d", sc->name, budget, bootBudget,
              loaded);
    } else {
        CHECK(loaded == 2 || loaded == oldVersion, "[%s] cut=%d boot=%d: loaded %d, expected %d or 2", sc->name,
              budget, bootBudget, loaded, oldVersion);
    }
    if (sc->startWithFinal || completed) {
        CHECK(loaded != -1, "[%s] cut=%d boot=%d: a save existed but boot found none", sc->name, budget, bootBudget);
    }
}

/* Uninterrupted flush: the end state is FINAL only (FINAL + BAK when keepBackup and a FINAL existed). */
static void RunClean(const Scenario* sc) {
    StubFs fs;
    SeedScenario(&fs, sc);
    CHECK(StubFlush(&fs, 2, sc->tryDirectFirst, sc->keepBackup), "[%s] clean flush failed", sc->name);
    StubFile* fin = StubFind(&fs, kFinal);
    CHECK(fin != NULL && fin->version == 2 && fin->complete, "[%s] FINAL is not generation 2", sc->name);
    CHECK(StubFind(&fs, kTmp) == NULL, "[%s] TMP left behind", sc->name);
    if (sc->keepBackup && sc->startWithFinal) {
        StubFile* bak = StubFind(&fs, kBak);
        CHECK(bak != NULL && bak->version == 1, "[%s] keepBackup: BAK is not the previous FINAL", sc->name);
    } else if (!sc->keepBackup && sc->startWithFinal) {
        /* (no FINAL + a BAK is a pre-boot crash state: the boot promotes BAK before any flush) */
        CHECK(StubFind(&fs, kBak) == NULL, "[%s] BAK left behind", sc->name);
    }
    CHECK(StubBoot(&fs, sc->keepBackup) == 2, "[%s] boot after a clean flush did not load generation 2", sc->name);
}

/* A long run of saves, each cut at a different point, each followed by a boot: never regress. */
static void RunSoak(const Scenario* sc) {
    StubFs fs;
    SeedScenario(&fs, sc);
    int lastCompleted = sc->startWithFinal ? 1 : (sc->startWithStaleBak ? 0 : -1);
    for (int gen = 2; gen < 200; gen++) {
        fs.opsBudget = (gen * 7) % 9; /* 0..8: cuts before, inside and after every step */
        if (gen % 5 == 0) {
            fs.opsBudget = -1;
        }
        const int completed = StubFlush(&fs, gen, sc->tryDirectFirst, sc->keepBackup);
        Restore(&fs);
        if (gen % 3 == 0) {
            fs.opsBudget = gen % 4; /* cut the boot's own recovery too */
            (void) StubBoot(&fs, sc->keepBackup);
            Restore(&fs);
        }
        const int loaded = StubBoot(&fs, sc->keepBackup);
        if (completed) {
            lastCompleted = gen;
        }
        CHECK(loaded >= lastCompleted, "[%s] soak gen=%d: loaded %d < last completed %d", sc->name, gen, loaded,
              lastCompleted);
        CHECK(loaded <= gen, "[%s] soak gen=%d: loaded future generation %d", sc->name, gen, loaded);
        if (loaded > lastCompleted) {
            lastCompleted = loaded; /* the boot adopted the newer complete copy: it is now the floor */
        }
    }
}

static void RunScenario(const Scenario* sc) {
    for (int budget = 0; budget <= 10; budget++) {
        for (int bootBudget = -1; bootBudget <= 4; bootBudget++) {
            RunCut(sc, budget, bootBudget);
        }
    }
    RunClean(sc);
    RunSoak(sc);
}

/* The bug itself: the legacy single rename onto an existing FINAL fails on the FAT stub. */
static void TestLegacyRenameFailsOnFat(void) {
    StubFs fs;
    memset(&fs, 0, sizeof(fs));
    fs.opsBudget = -1;
    StubPut(&fs, kFinal, 1, 1);
    StubPut(&fs, kTmp, 2, 1);
    CHECK(StubRename(&fs, kTmp, kFinal) != 0, "FAT stub must refuse rename onto an existing file");
    GdxSaveFsOps ops = StubOps(&fs);
    GdxSaveReplaceReport report;
    CHECK(gdx_save_replace(&ops, kTmp, kFinal, kBak, 1, 0, &report) == GDX_SAVE_REPLACE_OK,
          "replace must succeed on the FAT stub");
    CHECK(report.usedBackup == 1 && report.bakRemoved == 1, "FAT replace must go through BAK and remove it");
    CHECK(StubFind(&fs, kFinal)->version == 2, "FAT replace must leave generation 2 in FINAL");
}

/* A rename failure at step 3 rolls BAK back, so FINAL keeps the old save. */
static void TestRollbackOnPromoteFailure(void) {
    StubFs fs;
    memset(&fs, 0, sizeof(fs));
    fs.opsBudget = -1;
    StubPut(&fs, kFinal, 1, 1);
    /* No TMP on disk: step 3's rename fails without a power cut. */
    GdxSaveFsOps ops = StubOps(&fs);
    GdxSaveReplaceReport report;
    CHECK(gdx_save_replace(&ops, kTmp, kFinal, kBak, 0, 0, &report) == GDX_SAVE_REPLACE_ERR_PROMOTE_TMP,
          "missing TMP must fail at the promote step");
    CHECK(report.rolledBack == 1, "BAK must be rolled back to FINAL");
    CHECK(StubFind(&fs, kFinal) != NULL && StubFind(&fs, kFinal)->version == 1, "FINAL must hold the old save");
}

int main(void) {
    static const Scenario kScenarios[] = {
        {"fat existing", 0, 0, 0, 1, 0, 0},
        {"fat existing direct-first", 0, 1, 0, 1, 0, 0},
        {"fat existing stale-bak", 0, 0, 0, 1, 1, 0},
        {"fat existing stale-tmp", 0, 0, 0, 1, 0, 1},
        {"fat first-save", 0, 0, 0, 0, 0, 0},
        {"fat first-save stale-bak", 0, 0, 0, 0, 1, 0},
        {"fat keep-backup (64DD sidecar)", 0, 0, 1, 1, 0, 0},
        {"fat keep-backup stale-bak", 0, 0, 1, 1, 1, 0},
        {"posix existing direct-first", 1, 1, 0, 1, 0, 0},
        {"posix keep-backup", 1, 1, 1, 1, 1, 0},
    };
    TestLegacyRenameFailsOnFat();
    TestRollbackOnPromoteFailure();
    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); i++) {
        RunScenario(&kScenarios[i]);
    }
    if (sFailures != 0) {
        printf("gdx_save_replace_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_save_replace_tests: %d checks passed (%zu scenarios x power cut at every op)\n", sChecks,
           sizeof(kScenarios) / sizeof(kScenarios[0]));
    return 0;
}
