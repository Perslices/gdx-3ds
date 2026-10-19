#include "sram_buffer.h"
#include "port_log.h"
#include "gdx_save_replace.h"
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>
#else
#include <cstdlib>
#include <unistd.h>   // readlink, access
#include <sys/stat.h> // mkdir
#include <cerrno>
#endif

#ifdef GDX_PLATFORM_3DS
#include <3ds.h>
#include "gdx3ds_hitch.h" // gdx3ds_hitch_aud_inflight: the async writer yields to audio-stream reads
#endif

// EKSHIP (docs/research/native/o3dsek-plan.md §7, S1; user decision 2026-10-08): the 3DS Expansion Kit
// build is its own title and keeps its OWN save, saves/fzerox-ek.sav. The shipped game and an EK build
// read one ProfileSave section differently (SaveEditCup vs SaveDDCups), so a shared fzerox.sav was
// rewritten by whichever ran last. The EK never writes, renames or removes fzerox.sav; it may READ it
// once, to seed its own save, only when that file is proven EK-layout (gdx_save_ek_layout.h). The
// same crash-safe replace and [debug] sram_async apply to the EK file. Desktop builds are EK by default
// and keep fzerox.sav (one build, one layout), so this is 3DS-only; the shipped 3DS build is unchanged
// (GDX_SRAM_FILE_NAME is the same literal there, concatenated into the same strings).
#if defined(GDX_PLATFORM_3DS) && defined(EXPANSION_KIT)
#define GDX_SRAM_EK_OWN_SAVE 1
#include "gdx_save_ek_layout.h"
#define GDX_SRAM_FILE_NAME "fzerox-ek.sav"
#define GDX_SRAM_SHARED_FILE_NAME "fzerox.sav"
#else
#define GDX_SRAM_FILE_NAME "fzerox.sav"
#endif

// Host-side persistence for the game's cart-SRAM save (settings, course/death-race
// records, cup completion, character engine tuning, single-slot player/staff ghost).
// Host .cpp TU (G-Diffuser exe target, not the gdiffuser_game decomp object library),
// so the CRT is available here. decomp/src/overlays/ovl_i2/save.c only ever calls the
// three C-linkage entry points below; it never sees fopen/FILE*.
extern "C" {

static uint8_t s_sramBuffer[GDX_SRAM_SIZE];
static bool s_initialized = false;

// Windows: <exedir>/saves/fzerox.sav. POSIX: <cwd>/saves/fzerox.sav, matching the 64DD
// disk-save sidecar (disk_savefile.cpp) and the ghost I/O (gdx_ghost_io.c) so every save
// artifact shares one per-user directory rather than the often read-only exe directory.
// Each platform's gdx_sram_path migrates its legacy save on first resolution, and only
// when no current-convention save exists, so an already-migrated one is never clobbered.
#ifdef _WIN32
static bool gdx_sram_path(wchar_t* outPath, size_t outCapChars) {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = L'\0';

    const wchar_t* fileName = L"fzerox.sav";
    const wchar_t* savesDir = L"saves\\";
    if (wcslen(exePath) + wcslen(savesDir) + wcslen(fileName) >= outCapChars) {
        return false;
    }
    wcscpy_s(outPath, outCapChars, exePath);
    wcscat_s(outPath, outCapChars, L"saves");
    if (!CreateDirectoryW(outPath, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    wcscat_s(outPath, outCapChars, L"\\");
    wcscat_s(outPath, outCapChars, fileName);

    wchar_t legacyPath[MAX_PATH * 2] = {};
    if (wcslen(exePath) + wcslen(fileName) < sizeof(legacyPath) / sizeof(legacyPath[0])) {
        wcscpy_s(legacyPath, sizeof(legacyPath) / sizeof(legacyPath[0]), exePath);
        wcscat_s(legacyPath, sizeof(legacyPath) / sizeof(legacyPath[0]), fileName);
        if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(legacyPath) != INVALID_FILE_ATTRIBUTES) {
            if (MoveFileExW(legacyPath, outPath, MOVEFILE_WRITE_THROUGH)) {
                gdx_port_logf("[sram] migrated legacy fzerox.sav into saves\\ folder\n");
            }
        }
    }
    return true;
}
#else
// POSIX resolves CWD-relative rather than exe-relative because the exe directory is
// read-only inside an AppImage, /opt or Flatpak deployment, where a flush would silently
// fail and lose the save. Matches disk_savefile.cpp's savesDirectory() ("." as the exe dir
// on POSIX, mkdir "./saves" 0755) and the ghost I/O convention.

// rename(2) is atomic within a filesystem; a cross-device EXDEV falls back to a byte copy
// plus unlink. Callers only invoke this when dstPath does not yet exist, so an existing
// current-convention save is never overwritten.
#ifndef GDX_PLATFORM_3DS
static bool gdx_sram_move_file(const char* srcPath, const char* dstPath) {
    if (rename(srcPath, dstPath) == 0) {
        return true;
    }
    if (errno != EXDEV) {
        return false;
    }

    FILE* in = fopen(srcPath, "rb");
    if (in == nullptr) {
        return false;
    }
    FILE* out = fopen(dstPath, "wb");
    if (out == nullptr) {
        fclose(in);
        return false;
    }
    char buf[8192];
    size_t rd;
    bool ok = true;
    while ((rd = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, rd, out) != rd) {
            ok = false;
            break;
        }
    }
    if (ferror(in) != 0) {
        ok = false;
    }
    if (fflush(out) != 0) {
        ok = false;
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    fclose(in);
    if (!ok) {
        remove(dstPath); // don't leave a half-written destination behind
        return false;
    }
    remove(srcPath); // best-effort: the copy is the source of truth now
    return true;
}
#endif

#ifdef GDX_PLATFORM_3DS
extern "C" const char* gdx3ds_fs_base_path(void); // "sdmc:/3ds/gdiffuser/", trailing slash
#endif

static bool gdx_sram_path(char* outPath, size_t outCap) {
    const char* fileName = GDX_SRAM_FILE_NAME;
#ifdef GDX_PLATFORM_3DS
    /* 3DS: saves live under the port's SD base, not the CWD (a bare "saves/" resolves to
       the SD ROOT under sdmc CWD semantics -- that is where early boots dropped
       sdmc:/saves/fzerox.sav). Route through gdx3ds_fs_base_path() like every other
       persistence surface (gdx3ds_fs.h contract). */
    {
        const char* base = gdx3ds_fs_base_path();
        if (strlen(base) + strlen("saves/") + strlen(fileName) >= outCap) {
            return false;
        }
        strcpy(outPath, base);
        strcat(outPath, "saves");
        if (mkdir(outPath, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        strcat(outPath, "/");
        strcat(outPath, fileName);
        return true;
    }
#else
    // (#else, not fall-through: the desktop migration's two 4 KB path buffers must not land in
    // the 3DS frame -- the [debug] sram_async writer thread calls this on its own small stack.)
    const char* savesDir = "saves";

    if (strlen(savesDir) + 1 + strlen(fileName) >= outCap) {
        return false;
    }
    if (mkdir(savesDir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    strcpy(outPath, savesDir);
    strcat(outPath, "/");
    strcat(outPath, fileName);

    // Older POSIX builds wrote exe-relative <exedir>/saves/fzerox.sav. Migrate that in, but
    // ONLY when no CWD-relative save exists yet, so an already-migrated (or freshly written)
    // save is never clobbered.
    if (access(outPath, F_OK) != 0) {
        char exePath[4096];
        ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (n > 0) {
            exePath[n] = '\0';
            char* slash = strrchr(exePath, '/');
            if (slash != nullptr) {
                slash[1] = '\0'; // keep trailing separator
                char legacyPath[4096 + 32];
                if (strlen(exePath) + strlen("saves/") + strlen(fileName) < sizeof(legacyPath)) {
                    strcpy(legacyPath, exePath);
                    strcat(legacyPath, "saves/");
                    strcat(legacyPath, fileName);
                    if (access(legacyPath, F_OK) == 0 && gdx_sram_move_file(legacyPath, outPath)) {
                        gdx_port_logf("[sram] migrated legacy exe-relative fzerox.sav into CWD saves/ folder\n");
                    }
                }
            }
        }
    }
    return true;
#endif
}
#endif

// ---------------------------------------------------------------------------------------------
// N60SAVE (docs/research/native/n60save-progress.md). Two fixes, both receipted as [sram] lines:
//
// 1. The save now PERSISTS on the 3DS. The old flush staged fzerox.sav.tmp and rename()d it over
//    fzerox.sav. On the console's SD archive that rename fails whenever fzerox.sav exists: libctru's
//    archive_rename only falls back to delete+rename for result description 0x3FC, and the SD
//    archive answers 0xC82044BE (description 0xBE). So every save after the very first was dropped
//    ("[sram] WARNING: could not replace fzerox.sav" on every hardware log). The replace now goes
//    through gdx_save_replace (port/gdx_save_replace.h): FINAL -> .bak, .tmp -> FINAL, drop .bak,
//    with a complete copy on disk after every step, and the boot (gdx_save_recover) picks the first
//    32 KB copy of FINAL, .tmp, .bak. Escape hatch: [debug] sram_legacy_replace=1 restores the
//    single rename (3DS only; it reproduces the bug).
//
// 2. [debug] sram_async (3DS, default 1 since n3ds: a hitch fix, on for every model; 0 = the
//    synchronous write on main): the SD write leaves the main thread. The in-memory image
//    stays the game's only source of truth (reads never touch the SD), so game state is identical
//    either way. gdx_sram_write copies the bytes in and marks the image dirty ("write queued", or
//    "write coalesced" when an earlier write is still pending); a low-priority writer thread waits
//    [debug] sram_async_ms (default 100) so the finish's burst of writes becomes ONE 32 KB file
//    write, snapshots the image under the lock, and writes it in 8 KiB chunks, letting an in-flight
//    audio-stream read go first before each chunk (as the async file log does). "[sram] written
//    ms= ok=" closes each write. gdx_sram_flush_pending() writes anything still pending
//    synchronously, bounded, from the exit chain and the HOME/sleep suspend hook.
// ---------------------------------------------------------------------------------------------

#define GDX_SRAM_WRITE_CHUNK (8u * 1024u)
#define GDX_SRAM_AUDIO_YIELD_MAX_MS 50

[[maybe_unused]] static int s_legacyReplace = 0;
static int s_async = 0;
[[maybe_unused]] static unsigned s_asyncDebounceMs = 100;
static unsigned s_flushSeq = 0; // completed SD writes (receipts only)

// CRC-32 (IEEE, reflected) of a whole image: the receipts' crc= lets a gate compare the file on
// the SD with the in-memory image (`crc32` of fzerox.sav on the host == the last written crc=).
static unsigned gdx_sram_crc32(const uint8_t* image) {
    static unsigned table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        tableReady = true;
    }
    unsigned crc = 0xFFFFFFFFu;
    for (unsigned i = 0; i < GDX_SRAM_SIZE; i++) {
        crc = table[(crc ^ image[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct SramFlushReceipt {
    unsigned elapsedUs;
    unsigned audioYields;
    const char* replaceMode;
    bool ok;
} SramFlushReceipt;

static unsigned long long gdx_sram_now_us(void) {
#ifdef GDX_PLATFORM_3DS
    return svcGetSystemTick() / (unsigned long long) (CPU_TICKS_PER_MSEC / 1000.0);
#elif defined(_WIN32)
    return (unsigned long long) GetTickCount64() * 1000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000ull + (unsigned long long) ts.tv_nsec / 1000ull;
#endif
}

#ifdef GDX_PLATFORM_3DS
extern "C" int gdx3ds_config_get_bool(const char* section, const char* key, int fallback);
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback);

static void gdx_sram_configure_levers(void) {
    s_legacyReplace = gdx3ds_config_get_bool("debug", "sram_legacy_replace", 0) ? 1 : 0;
    s_async = gdx3ds_config_get_bool("debug", "sram_async", 1) ? 1 : 0;
    int ms = gdx3ds_config_get_int("debug", "sram_async_ms", 100);
    if (ms < 0) {
        ms = 0;
    } else if (ms > 5000) {
        ms = 5000;
    }
    s_asyncDebounceMs = (unsigned) ms;
}
#else
static void gdx_sram_configure_levers(void) {
    // Desktop: rename() replaces atomically (POSIX) / MoveFileEx does (Windows); no levers.
}
#endif

#ifdef _WIN32
static void gdx_sram_load_from_disk(void) {
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] no existing fzerox.sav; starting fresh (first boot creates it on first write).\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] fzerox.sav size mismatch (%ld bytes, expected %u); starting with a fresh save.\n", sz,
                      GDX_SRAM_SIZE);
        fclose(f);
        return;
    }

    if (fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) != GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: failed reading fzerox.sav; starting with a fresh save.\n");
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    } else {
        gdx_port_logf("[sram] loaded %u bytes from fzerox.sav\n", GDX_SRAM_SIZE);
    }
    fclose(f);
}

static bool gdx_sram_flush_image(const uint8_t* image, bool yieldToAudio, SramFlushReceipt* receipt) {
    (void) yieldToAudio;
    receipt->replaceMode = "movefileex";
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return false;
    }

    // Stage to fzerox.sav.tmp and MoveFileEx-replace, so a crash mid-write cannot truncate
    // or half-write the live save -- the previous fzerox.sav stays intact until the temp
    // is complete.
    wchar_t tempPath[MAX_PATH * 2] = {};
    if (wcslen(path) + 4 >= sizeof(tempPath) / sizeof(tempPath[0])) {
        gdx_port_logf("[sram] WARNING: save path too long for temp file; save not persisted.\n");
        return false;
    }
    wcscpy_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), path);
    wcscat_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), L".tmp");

    FILE* f = nullptr;
    if (_wfopen_s(&f, tempPath, L"wb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open fzerox.sav.tmp for writing; save not persisted.\n");
        return false;
    }
    bool ok = fwrite(image, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing fzerox.sav.tmp; save not persisted.\n");
        _wremove(tempPath);
        return false;
    }
    if (!MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        gdx_port_logf("[sram] WARNING: could not replace fzerox.sav; save not persisted.\n");
        _wremove(tempPath);
        return false;
    }
    return true;
}
#else
// Sized for the real paths ("sdmc:/3ds/gdiffuser/saves/fzerox.sav", "saves/fzerox.sav"); a
// longer one fails gdx_sram_path's capacity check. Small on purpose: the async writer's stack.
typedef struct SramPaths {
    char finalPath[512];
    char tmpPath[512 + 8];
    char bakPath[512 + 8];
} SramPaths;

static bool gdx_sram_resolve_paths(SramPaths* paths) {
    if (!gdx_sram_path(paths->finalPath, sizeof(paths->finalPath))) {
        return false;
    }
    if (strlen(paths->finalPath) + 4 >= sizeof(paths->tmpPath)) {
        return false;
    }
    snprintf(paths->tmpPath, sizeof(paths->tmpPath), "%s.tmp", paths->finalPath);
    snprintf(paths->bakPath, sizeof(paths->bakPath), "%s.bak", paths->finalPath);
    return true;
}

static int gdx_sram_fs_rename(void* ctx, const char* from, const char* to) {
    (void) ctx;
    return rename(from, to);
}

static int gdx_sram_fs_remove(void* ctx, const char* path) {
    (void) ctx;
    return remove(path);
}

static int gdx_sram_fs_exists(void* ctx, const char* path) {
    (void) ctx;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

// A usable save is exactly GDX_SRAM_SIZE bytes; a torn write is shorter. Section checksums are
// the game's own (save.c re-initialises any section whose checksum fails).
static int gdx_sram_fs_valid(void* ctx, const char* path) {
    (void) ctx;
    struct stat st;
    return stat(path, &st) == 0 && st.st_size == (off_t) GDX_SRAM_SIZE ? 1 : 0;
}

static GdxSaveFsOps gdx_sram_fs_ops(void) {
    GdxSaveFsOps ops;
    ops.rename_file = gdx_sram_fs_rename;
    ops.remove_file = gdx_sram_fs_remove;
    ops.file_exists = gdx_sram_fs_exists;
    ops.file_valid = gdx_sram_fs_valid;
    ops.ctx = nullptr;
    return ops;
}

static bool gdx_sram_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] " GDX_SRAM_FILE_NAME " size mismatch (%ld bytes, expected %u); starting with a fresh save.\n", sz,
                      GDX_SRAM_SIZE);
        fclose(f);
        return false;
    }
    bool ok = fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    fclose(f);
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed reading " GDX_SRAM_FILE_NAME "; starting with a fresh save.\n");
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    }
    return ok;
}

#ifdef GDX_SRAM_EK_OWN_SAVE
static bool gdx_sram_ek_seed(const SramPaths* paths); // defined after the writer (it persists the seed)
#endif

// Boot: the newest complete copy wins (FINAL, else .tmp, else .bak) and is promoted to FINAL.
static void gdx_sram_load_from_disk(void) {
    SramPaths paths;
    if (!gdx_sram_resolve_paths(&paths)) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }
    GdxSaveFsOps ops = gdx_sram_fs_ops();
    int promoted = 0;
    GdxSaveSource source = gdx_save_recover(&ops, paths.finalPath, paths.tmpPath, paths.bakPath, 0, &promoted);
    if (source == GDX_SAVE_SOURCE_NONE) {
#ifdef GDX_SRAM_EK_OWN_SAVE
        if (!gdx_sram_fs_exists(nullptr, paths.finalPath) && !gdx_sram_fs_exists(nullptr, paths.tmpPath) &&
            !gdx_sram_fs_exists(nullptr, paths.bakPath) && gdx_sram_ek_seed(&paths)) {
            return;
        }
#endif
        if (gdx_sram_fs_exists(nullptr, paths.finalPath)) {
            gdx_sram_read_file(paths.finalPath); // logs the size mismatch, keeps the fresh image
        } else {
            gdx_port_logf("[sram] no existing " GDX_SRAM_FILE_NAME "; starting fresh (first boot creates it on first write).\n");
        }
        return;
    }
    if (source != GDX_SAVE_SOURCE_FINAL) {
        gdx_port_logf("[sram] recovered the save from " GDX_SRAM_FILE_NAME ".%s (promoted to " GDX_SRAM_FILE_NAME ": %d)\n",
                      gdx_save_source_name(source), promoted);
    }
    const char* readPath = gdx_save_source_path(source, promoted, paths.finalPath, paths.tmpPath, paths.bakPath);
    if (gdx_sram_read_file(readPath)) {
        gdx_port_logf("[sram] loaded %u bytes from " GDX_SRAM_FILE_NAME " crc=%08x\n", GDX_SRAM_SIZE, gdx_sram_crc32(s_sramBuffer));
    }
}

#ifdef GDX_PLATFORM_3DS
// The async writer lets an in-flight audio-stream SD read go first (bounded), like the file log.
static bool gdx_sram_yield_to_audio_read(void) {
    int waitedMs = 0;
    while (gdx3ds_hitch_aud_inflight && waitedMs < GDX_SRAM_AUDIO_YIELD_MAX_MS) {
        svcSleepThread(1000000LL);
        waitedMs++;
    }
    return waitedMs > 0;
}
#endif

static bool gdx_sram_write_chunks(FILE* f, const uint8_t* image, bool yieldToAudio, SramFlushReceipt* receipt) {
    unsigned done = 0;
    while (done < GDX_SRAM_SIZE) {
        unsigned chunk = GDX_SRAM_SIZE - done;
        if (chunk > GDX_SRAM_WRITE_CHUNK) {
            chunk = GDX_SRAM_WRITE_CHUNK;
        }
#ifdef GDX_PLATFORM_3DS
        if (yieldToAudio && gdx_sram_yield_to_audio_read()) {
            receipt->audioYields++;
        }
#else
        (void) yieldToAudio;
        (void) receipt;
#endif
        if (fwrite(image + done, 1, chunk, f) != chunk) {
            return false;
        }
        done += chunk;
    }
    return true;
}

// Writes the full image to TMP and makes it durable before anything renames it.
static bool gdx_sram_write_tmp(const char* tmpPath, const uint8_t* image, bool yieldToAudio,
                               SramFlushReceipt* receipt) {
    FILE* f = fopen(tmpPath, "wb");
    if (f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open " GDX_SRAM_FILE_NAME ".tmp for writing; save not persisted.\n");
        return false;
    }
    setvbuf(f, nullptr, _IONBF, 0); // one FS write per chunk
    bool ok = gdx_sram_write_chunks(f, image, yieldToAudio, receipt);
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fsync(fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing " GDX_SRAM_FILE_NAME ".tmp; save not persisted.\n");
        remove(tmpPath);
    }
    return ok;
}

// Pre-n60save behaviour, kept as [debug] sram_legacy_replace=1: fails on the 3DS SD archive
// whenever fzerox.sav already exists.
static bool gdx_sram_replace_legacy(const SramPaths* paths) {
    if (rename(paths->tmpPath, paths->finalPath) != 0) {
        gdx_port_logf("[sram] WARNING: could not replace " GDX_SRAM_FILE_NAME " (errno=%d, legacy single rename); "
                      "save not persisted.\n",
                      errno);
        remove(paths->tmpPath);
        return false;
    }
    return true;
}

static bool gdx_sram_replace_safe(const SramPaths* paths, SramFlushReceipt* receipt) {
    GdxSaveFsOps ops = gdx_sram_fs_ops();
    GdxSaveReplaceReport report;
#ifdef GDX_PLATFORM_3DS
    const int tryDirectFirst = 0; // the SD archive never renames over a file: skip the failing IPC
#else
    const int tryDirectFirst = 1; // POSIX rename(2) replaces atomically
#endif
    GdxSaveReplaceResult rc =
        gdx_save_replace(&ops, paths->tmpPath, paths->finalPath, paths->bakPath, tryDirectFirst, 0, &report);
    receipt->replaceMode = report.usedBackup ? "bak" : "direct";
    if (rc != GDX_SAVE_REPLACE_OK) {
        gdx_port_logf("[sram] WARNING: could not replace " GDX_SRAM_FILE_NAME " (step=%s errno=%d rolled_back=%d); "
                      "save not persisted, the previous save is intact.\n",
                      rc == GDX_SAVE_REPLACE_ERR_ROLL_TO_BAK ? "final->bak" : "tmp->final", errno, report.rolledBack);
        return false;
    }
    return true;
}

static bool gdx_sram_flush_image(const uint8_t* image, bool yieldToAudio, SramFlushReceipt* receipt) {
    SramPaths paths;
    receipt->replaceMode = s_legacyReplace ? "legacy" : "none";
    if (!gdx_sram_resolve_paths(&paths)) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return false;
    }
    if (!gdx_sram_write_tmp(paths.tmpPath, image, yieldToAudio, receipt)) {
        return false;
    }
    if (s_legacyReplace) {
        return gdx_sram_replace_legacy(&paths);
    }
    return gdx_sram_replace_safe(&paths, receipt);
}
#endif

// One timed SD write of IMAGE plus its receipt.
static void gdx_sram_flush_timed(const uint8_t* image, bool yieldToAudio, const char* mode, unsigned coalesced) {
    SramFlushReceipt receipt = {};
    const unsigned long long t0 = gdx_sram_now_us();
    receipt.ok = gdx_sram_flush_image(image, yieldToAudio, &receipt);
    receipt.elapsedUs = (unsigned) (gdx_sram_now_us() - t0);
    s_flushSeq++;
    gdx_port_logf("[sram] written ms=%u.%03u ok=%d seq=%u mode=%s writes=%u replace=%s audio_yields=%u crc=%08x\n",
                  receipt.elapsedUs / 1000u, receipt.elapsedUs % 1000u, receipt.ok ? 1 : 0, s_flushSeq, mode,
                  coalesced, receipt.replaceMode, receipt.audioYields, gdx_sram_crc32(image));
}

#ifdef GDX_PLATFORM_3DS
// ---- [debug] sram_async: the writer thread ------------------------------------------------------
#define GDX_SRAM_WRITER_STACK (32u * 1024u) /* gdx_port_logf alone holds 2 KB + vfprintf; see n60save-progress */
#define GDX_SRAM_WRITER_PRIO 0x38 /* below main (0x30): only takes otherwise-idle core-0 time */

static LightLock s_imageLock;     // guards s_sramBuffer writes vs the writer's snapshot, and s_dirty*
static LightLock s_fileLock;      // one SD writer at a time (the thread, or a synchronous flush)
static LightEvent s_writerWake;
static Thread s_writerThread = nullptr;
static bool s_asyncStarted = false;
static unsigned s_dirtyWrites = 0; // gdx_sram_write calls since the last snapshot (0 = clean)
static uint8_t* s_writeImage = nullptr; // the writer's private snapshot (allocated only with the lever on)

// Snapshot the image if dirty. Caller holds s_fileLock. Returns the coalesced write count.
static unsigned gdx_sram_take_snapshot(void) {
    LightLock_Lock(&s_imageLock);
    const unsigned writes = s_dirtyWrites;
    if (writes != 0) {
        memcpy(s_writeImage, s_sramBuffer, GDX_SRAM_SIZE);
        s_dirtyWrites = 0;
    }
    LightLock_Unlock(&s_imageLock);
    return writes;
}

static void gdx_sram_writer_main(void* arg) {
    (void) arg;
    for (;;) {
        LightEvent_Wait(&s_writerWake);
        if (s_asyncDebounceMs > 0) {
            svcSleepThread((s64) s_asyncDebounceMs * 1000000LL); // coalesce the finish's burst
        }
        LightLock_Lock(&s_fileLock);
        const unsigned writes = gdx_sram_take_snapshot();
        if (writes != 0) {
            gdx_sram_flush_timed(s_writeImage, true, "async", writes);
        }
        LightLock_Unlock(&s_fileLock);
    }
}

static void gdx_sram_start_writer(void) {
    s_writeImage = static_cast<uint8_t*>(malloc(GDX_SRAM_SIZE));
    if (s_writeImage == nullptr) {
        s_async = 0;
        return;
    }
    LightLock_Init(&s_imageLock);
    LightLock_Init(&s_fileLock);
    LightEvent_Init(&s_writerWake, RESET_ONESHOT);
    s_writerThread = threadCreate(gdx_sram_writer_main, nullptr, GDX_SRAM_WRITER_STACK, GDX_SRAM_WRITER_PRIO, 0, true);
    s_asyncStarted = s_writerThread != nullptr;
    if (!s_asyncStarted) {
        s_async = 0;
        free(s_writeImage);
        s_writeImage = nullptr;
    }
}

static void gdx_sram_log_levers(void) {
    gdx_port_logf("[sram] init async=%d writer prio=0x%X debounce=%u ms legacy_replace=%d "
                  "(debug.sram_async, debug.sram_async_ms, debug.sram_legacy_replace)%s\n",
                  s_async, GDX_SRAM_WRITER_PRIO, s_asyncDebounceMs, s_legacyReplace,
                  (gdx3ds_config_get_bool("debug", "sram_async", 1) && !s_async) ? ": writer start failed, sync"
                                                                                 : "");
}

// Main thread: the bytes land in the image (the game's source of truth) and the writer is woken.
static void gdx_sram_write_async(unsigned int offset, const void* src, unsigned int size) {
    const unsigned long long t0 = gdx_sram_now_us();
    LightLock_Lock(&s_imageLock);
    memcpy(s_sramBuffer + offset, src, size);
    const unsigned pending = ++s_dirtyWrites;
    LightLock_Unlock(&s_imageLock);
    LightEvent_Signal(&s_writerWake);
    const unsigned mainUs = (unsigned) (gdx_sram_now_us() - t0);
    gdx_port_logf("[sram] write %s off=%u size=%u pending=%u main_us=%u\n", pending > 1 ? "coalesced" : "queued",
                  offset, size, pending, mainUs);
}

void gdx_sram_flush_pending(unsigned int timeoutMs, const char* why) {
    if (!s_asyncStarted) {
        return;
    }
    const unsigned long long t0 = gdx_sram_now_us();
    unsigned waitedMs = 0;
    while (LightLock_TryLock(&s_fileLock) != 0) { // the writer is mid-write: let it finish, bounded
        if (waitedMs >= timeoutMs) {
            gdx_port_logf("[sram] flush %s: writer still busy after %u ms; leaving it to finish "
                          "(the previous save stays intact)\n",
                          why, waitedMs);
            return;
        }
        svcSleepThread(1000000LL);
        waitedMs++;
    }
    const unsigned writes = gdx_sram_take_snapshot();
    if (writes != 0) {
        gdx_sram_flush_timed(s_writeImage, false, why, writes);
    }
    LightLock_Unlock(&s_fileLock);
    gdx_port_logf("[sram] flush %s: pending_writes=%u waited_ms=%u total_ms=%u image_crc=%08x\n", why, writes,
                  waitedMs, (unsigned) ((gdx_sram_now_us() - t0) / 1000ull), gdx_sram_crc32(s_sramBuffer));
}
#else
static void gdx_sram_start_writer(void) {
}

static void gdx_sram_log_levers(void) {
}

static void gdx_sram_write_async(unsigned int offset, const void* src, unsigned int size) {
    (void) offset;
    (void) src;
    (void) size;
}

void gdx_sram_flush_pending(unsigned int timeoutMs, const char* why) {
    (void) timeoutMs;
    (void) why;
}
#endif

#ifdef GDX_SRAM_EK_OWN_SAVE
// The shared save's path: FINAL's directory + fzerox.sav.
static bool gdx_sram_shared_path(const SramPaths* paths, char* outPath, size_t outCap) {
    const size_t finalLen = strlen(paths->finalPath);
    const size_t leafLen = strlen(GDX_SRAM_FILE_NAME);
    if (finalLen < leafLen || finalLen - leafLen + strlen(GDX_SRAM_SHARED_FILE_NAME) >= outCap) {
        return false;
    }
    memcpy(outPath, paths->finalPath, finalLen - leafLen);
    strcpy(outPath + (finalLen - leafLen), GDX_SRAM_SHARED_FILE_NAME);
    return true;
}

// Writes the seeded image to the EK save now, through the normal crash-safe path, so the copy happens
// once: from the next boot on the EK save exists and fzerox.sav is never consulted again.
static void gdx_sram_persist_seed(void) {
    if (s_asyncStarted) {
        LightLock_Lock(&s_fileLock);
    }
    gdx_sram_flush_timed(s_sramBuffer, false, "ek-seed", 1);
    if (s_asyncStarted) {
        LightLock_Unlock(&s_fileLock);
    }
}

// Called only when no copy of the EK save (FINAL, .tmp, .bak) exists. [debug] ek_save_seed=0 never
// reads fzerox.sav at all. A decline starts fresh; the game's first save write then creates the EK
// save, after which this never runs again.
static bool gdx_sram_ek_seed(const SramPaths* paths) {
    if (!gdx3ds_config_get_bool("debug", "ek_save_seed", 1)) {
        gdx_port_logf("[sram] ek-seed: off (debug.ek_save_seed=0); " GDX_SRAM_FILE_NAME " starts fresh, "
                      GDX_SRAM_SHARED_FILE_NAME " not read\n");
        return false;
    }
    char sharedPath[512];
    if (!gdx_sram_shared_path(paths, sharedPath, sizeof(sharedPath))) {
        gdx_port_logf("[sram] ek-seed: declined verdict=path; " GDX_SRAM_FILE_NAME " starts fresh\n");
        return false;
    }
    GdxSaveEkSeed seed;
    if (!gdx_save_ek_seed_from_file(sharedPath, s_sramBuffer, &seed)) {
        gdx_port_logf("[sram] ek-seed: declined verdict=%s shared_bytes=%ld staff=0x%03X dd_marks=%u; "
                      GDX_SRAM_FILE_NAME " starts fresh; " GDX_SRAM_SHARED_FILE_NAME " untouched (read-only)\n",
                      gdx_save_ek_verdict_name(seed.verdict), seed.legacyBytes, seed.report.staffGhostCompletion,
                      seed.report.ddMarks);
        return false;
    }
    gdx_port_logf("[sram] ek-seed: copied " GDX_SRAM_SHARED_FILE_NAME " -> " GDX_SRAM_FILE_NAME
                  " verdict=proven staff=0x%03X dd_marks=%u crc=%08x; " GDX_SRAM_SHARED_FILE_NAME
                  " untouched (read-only)\n",
                  seed.report.staffGhostCompletion, seed.report.ddMarks, gdx_sram_crc32(s_sramBuffer));
    gdx_sram_persist_seed();
    return true;
}
#endif

void gdx_sram_init(void) {
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    gdx_sram_configure_levers();
    if (s_async) {
        gdx_sram_start_writer();
    }
    gdx_sram_log_levers();
#ifdef GDX_SRAM_EK_OWN_SAVE
    gdx_port_logf("[sram] ek: own save saves/" GDX_SRAM_FILE_NAME " (EK title; saves/" GDX_SRAM_SHARED_FILE_NAME
                  " is never written by this build)\n");
#endif
    gdx_sram_load_from_disk();
}

void gdx_sram_read(unsigned int offset, void* dst, unsigned int size) {
    gdx_sram_init(); // load order is not guaranteed: Sram_Init() may not have run yet
    if (dst == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: read out of range (offset=%u size=%u); returning zeros.\n", offset, size);
        memset(dst, 0, size);
        return;
    }
    memcpy(dst, s_sramBuffer + offset, size);
}

void gdx_sram_write(unsigned int offset, const void* src, unsigned int size) {
    gdx_sram_init();
    if (src == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: write out of range (offset=%u size=%u); ignored.\n", offset, size);
        return;
    }
    if (s_async) {
        gdx_sram_write_async(offset, src, size);
        return;
    }
    memcpy(s_sramBuffer + offset, src, size);
    gdx_sram_flush_timed(s_sramBuffer, false, "sync", 1); // write-through on the caller
}

} // extern "C"
