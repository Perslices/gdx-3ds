/* gdx_save_replace.c -- see gdx_save_replace.h for the contract and the crash-safety argument. */
#include "gdx_save_replace.h"

static int SaveExists(const GdxSaveFsOps* ops, const char* path) {
    return ops->file_exists(ops->ctx, path) != 0;
}

static int SaveValid(const GdxSaveFsOps* ops, const char* path) {
    if (!SaveExists(ops, path)) {
        return 0;
    }
    if (ops->file_valid == NULL) {
        return 1;
    }
    return ops->file_valid(ops->ctx, path) != 0;
}

static int SaveRename(const GdxSaveFsOps* ops, const char* from, const char* to) {
    return ops->rename_file(ops->ctx, from, to) == 0;
}

static int SaveRemoveIfPresent(const GdxSaveFsOps* ops, const char* path) {
    if (!SaveExists(ops, path)) {
        return 0;
    }
    return ops->remove_file(ops->ctx, path) == 0;
}

static void ClearReport(GdxSaveReplaceReport* report) {
    report->usedDirect = 0;
    report->usedBackup = 0;
    report->removedStale = 0;
    report->rolledBack = 0;
    report->bakRemoved = 0;
}

/* FINAL exists and the FS will not rename over it: roll it to BAK, promote TMP, drop BAK. */
static GdxSaveReplaceResult ReplaceViaBackup(const GdxSaveFsOps* ops, const char* tmpPath, const char* finalPath,
                                             const char* bakPath, int keepBackup, GdxSaveReplaceReport* report) {
    report->usedBackup = 1;
    report->removedStale = SaveRemoveIfPresent(ops, bakPath);
    if (!SaveRename(ops, finalPath, bakPath)) {
        ops->remove_file(ops->ctx, tmpPath); /* FINAL is untouched and still the good copy */
        return GDX_SAVE_REPLACE_ERR_ROLL_TO_BAK;
    }
    if (!SaveRename(ops, tmpPath, finalPath)) {
        /* TMP stays: if the rollback fails too, recovery still finds TMP (new) before BAK (old). */
        report->rolledBack = SaveRename(ops, bakPath, finalPath);
        return GDX_SAVE_REPLACE_ERR_PROMOTE_TMP;
    }
    if (!keepBackup) {
        report->bakRemoved = ops->remove_file(ops->ctx, bakPath) == 0;
    }
    return GDX_SAVE_REPLACE_OK;
}

GdxSaveReplaceResult gdx_save_replace(const GdxSaveFsOps* ops, const char* tmpPath, const char* finalPath,
                                      const char* bakPath, int tryDirectFirst, int keepBackup,
                                      GdxSaveReplaceReport* report) {
    GdxSaveReplaceReport local;
    if (report == NULL) {
        report = &local;
    }
    ClearReport(report);

    if (keepBackup && SaveExists(ops, finalPath)) {
        return ReplaceViaBackup(ops, tmpPath, finalPath, bakPath, keepBackup, report);
    }
    if (tryDirectFirst || !SaveExists(ops, finalPath)) {
        if (SaveRename(ops, tmpPath, finalPath)) {
            report->usedDirect = 1;
            return GDX_SAVE_REPLACE_OK;
        }
        if (!SaveExists(ops, finalPath)) {
            return GDX_SAVE_REPLACE_ERR_PROMOTE_TMP; /* TMP kept: it is the newest complete copy */
        }
    }
    return ReplaceViaBackup(ops, tmpPath, finalPath, bakPath, keepBackup, report);
}

/* Make SOURCEPATH the FINAL copy; an unusable FINAL in the way is removed first. */
static int PromoteToFinal(const GdxSaveFsOps* ops, const char* sourcePath, const char* finalPath) {
    SaveRemoveIfPresent(ops, finalPath);
    return SaveRename(ops, sourcePath, finalPath);
}

GdxSaveSource gdx_save_recover(const GdxSaveFsOps* ops, const char* finalPath, const char* tmpPath,
                               const char* bakPath, int keepBackup, int* promoted) {
    int localPromoted = 0;
    if (promoted == NULL) {
        promoted = &localPromoted;
    }
    *promoted = 0;

    if (SaveValid(ops, finalPath)) {
        SaveRemoveIfPresent(ops, tmpPath);
        if (!keepBackup) {
            SaveRemoveIfPresent(ops, bakPath);
        }
        return GDX_SAVE_SOURCE_FINAL;
    }
    if (SaveValid(ops, tmpPath)) {
        *promoted = PromoteToFinal(ops, tmpPath, finalPath);
        if (*promoted && !keepBackup) {
            SaveRemoveIfPresent(ops, bakPath);
        }
        return GDX_SAVE_SOURCE_TMP;
    }
    if (SaveValid(ops, bakPath)) {
        *promoted = PromoteToFinal(ops, bakPath, finalPath);
        return GDX_SAVE_SOURCE_BAK;
    }
    return GDX_SAVE_SOURCE_NONE;
}

const char* gdx_save_source_path(GdxSaveSource source, int promoted, const char* finalPath, const char* tmpPath,
                                 const char* bakPath) {
    if (source == GDX_SAVE_SOURCE_FINAL || promoted) {
        return finalPath;
    } else if (source == GDX_SAVE_SOURCE_TMP) {
        return tmpPath;
    } else if (source == GDX_SAVE_SOURCE_BAK) {
        return bakPath;
    }
    return NULL;
}

const char* gdx_save_source_name(GdxSaveSource source) {
    switch (source) {
        case GDX_SAVE_SOURCE_FINAL:
            return "final";
        case GDX_SAVE_SOURCE_TMP:
            return "tmp";
        case GDX_SAVE_SOURCE_BAK:
            return "bak";
        default:
            return "none";
    }
}
