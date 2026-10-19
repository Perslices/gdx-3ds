#pragma once
/*
 * gdx_save_replace -- crash-safe "replace FINAL with TMP" for filesystems whose rename() does NOT
 * overwrite an existing target (n60save, docs/research/native/n60save-progress.md).
 *
 * Why: on the Nintendo 3DS, libctru's archive_rename() calls FSUSER_RenameFile and only falls back
 * to delete-then-rename when the result's description is 0x3FC (RD_ALREADY_EXISTS). The SD archive
 * answers a rename onto an existing file with 0xC82044BE (FS description 0xBE), so rename() simply
 * fails: every hardware log carried "[sram] WARNING: could not replace fzerox.sav", and every save
 * after the first was lost.
 *
 * The replace below never leaves the user without one complete copy on disk, at every step:
 *
 *   precondition: TMP is fully written, flushed and closed by the caller.
 *   1. remove a stale BAK            FINAL valid (TMP complete)
 *   2. rename FINAL -> BAK            FINAL missing, BAK valid (old), TMP complete (new)
 *   3. rename TMP   -> FINAL          FINAL valid (new), BAK valid (old)
 *   4. remove BAK (unless keepBackup) FINAL valid (new)
 *
 * gdx_save_recover() is the boot half: it picks FINAL, else TMP, else BAK (each must pass the
 * caller's validity check) and promotes the chosen one to FINAL, so a power cut between any two
 * steps resolves to the newest complete copy.
 *
 * Pure C, no platform headers: the filesystem is a small ops table so the host test can run the
 * whole sequence against a FAT-like stub that refuses rename-over-existing and can "lose power"
 * after any operation (port/tests/gdx_save_replace_tests.c).
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxSaveFsOps {
    int (*rename_file)(void* ctx, const char* from, const char* to); /* 0 on success */
    int (*remove_file)(void* ctx, const char* path);                   /* 0 on success */
    int (*file_exists)(void* ctx, const char* path);                   /* 1 if present */
    /* Optional: 1 if PATH holds a usable save. NULL = "exists" is enough. */
    int (*file_valid)(void* ctx, const char* path);
    void* ctx;
} GdxSaveFsOps;

typedef enum GdxSaveReplaceResult {
    GDX_SAVE_REPLACE_OK = 0,
    GDX_SAVE_REPLACE_ERR_ROLL_TO_BAK = 1,  /* step 2 failed: FINAL untouched, TMP removed */
    GDX_SAVE_REPLACE_ERR_PROMOTE_TMP = 2,  /* step 3 failed: BAK rolled back to FINAL (see rolledBack) */
} GdxSaveReplaceResult;

typedef struct GdxSaveReplaceReport {
    int usedDirect;     /* 1: a plain rename(TMP, FINAL) succeeded (no FINAL, or a replacing FS) */
    int usedBackup;     /* 1: the FINAL -> BAK -> FINAL sequence ran */
    int removedStale;   /* 1: step 1 removed a leftover BAK */
    int rolledBack;     /* 1: step 3 failed and BAK was renamed back to FINAL */
    int bakRemoved;     /* 1: step 4 removed BAK */
} GdxSaveReplaceReport;

/* Replace FINAL with TMP. tryDirectFirst=1 first attempts rename(TMP, FINAL) (atomic replace on
 * POSIX); 0 goes straight to the backup sequence when FINAL exists (3DS: a guaranteed-failing IPC
 * is skipped, and the emulator exercises the same path as the console). keepBackup=1 leaves the
 * previous FINAL as BAK (the 64DD sidecar's rolling backup). report may be NULL. */
GdxSaveReplaceResult gdx_save_replace(const GdxSaveFsOps* ops, const char* tmpPath, const char* finalPath,
                                      const char* bakPath, int tryDirectFirst, int keepBackup,
                                      GdxSaveReplaceReport* report);

typedef enum GdxSaveSource {
    GDX_SAVE_SOURCE_NONE = 0,
    GDX_SAVE_SOURCE_FINAL = 1,
    GDX_SAVE_SOURCE_TMP = 2,
    GDX_SAVE_SOURCE_BAK = 3,
} GdxSaveSource;

/* Boot recovery: the first VALID of FINAL, TMP, BAK. A TMP or BAK winner is renamed to FINAL
 * (best effort: *promoted reports it); when FINAL wins, a leftover TMP is removed, and a leftover
 * BAK too unless keepBackup. Returns which copy FINAL now holds (or, if the promotion failed, which
 * path the caller should read: see gdx_save_source_path). promoted may be NULL. */
GdxSaveSource gdx_save_recover(const GdxSaveFsOps* ops, const char* finalPath, const char* tmpPath,
                               const char* bakPath, int keepBackup, int* promoted);

/* The path that holds SOURCE's bytes after gdx_save_recover (FINAL when promoted). */
const char* gdx_save_source_path(GdxSaveSource source, int promoted, const char* finalPath, const char* tmpPath,
                                 const char* bakPath);

const char* gdx_save_source_name(GdxSaveSource source);

#ifdef __cplusplus
}
#endif
