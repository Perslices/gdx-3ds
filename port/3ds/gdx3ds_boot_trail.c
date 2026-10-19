/* port/3ds/gdx3ds_boot_trail.c -- n60boot: the crash-proof boot trail. Contract and why:
 * port/3ds/include/gdx3ds_boot_trail.h. Progress: docs/research/native/n60boot-progress.md.
 *
 * Same durability recipe as n60hwdiag's nrr_crumbs (gfx/nrr/gdx3ds_nrr_crumb.c): one handle kept
 * open, every line one FSFILE_Write(..., FS_WRITE_FLUSH), nothing buffered on our side. Unlike
 * the crumbs it opens before the config exists, rewrites its file per boot, and needs no lock:
 * each line reserves its byte range with one atomic add, so lines from several threads (and
 * the exception handler) never interleave. */
#include "gdx3ds_boot_trail.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_crash.h"
#include "gdx3ds_filelog.h"

#ifndef GDX3DS_BOOT_TRAIL
#define GDX3DS_BOOT_TRAIL 1
#endif
#ifndef GDX3DS_LOG_TAG
#define GDX3DS_LOG_TAG ""
#endif
/* hw bug n60-3d8, item 3: the real branch@hash now comes from this generated, build-time
 * header (port/3ds/cmake/gdx3ds_gen_build_id.cmake) instead of a configure-time -D; the
 * __has_include guard + #ifndef below is a safety net for a build that never generated or
 * wired up the include path for it. */
#if defined(__has_include)
#if __has_include("gdx3ds_build_id.h")
#include "gdx3ds_build_id.h"
#endif
#endif
#ifndef GDX3DS_BUILD_ID
#define GDX3DS_BUILD_ID "unknown"
#endif

#define BOOT_TRAIL_PATH "/3ds/gdiffuser/boot" GDX3DS_LOG_TAG ".txt"
#define BOOT_TRAIL_PREV_PATH "/3ds/gdiffuser/boot" GDX3DS_LOG_TAG "-prev.txt"
#define BOOT_TRAIL_CAP (256u * 1024u)
#define BOOT_TRAIL_LINE 320
#define BOOT_TRAIL_FRAMES_DEFAULT 2

static FS_Archive sArchive = 0;
static Handle sFile = 0;
static volatile int sOpen = 0;
static u32 sOffset = 0; /* atomic */
static u64 sOpenTick = 0;
static u64 sOpenCostTicks = 0;
static int sRotated = 0;
static int sFrames = BOOT_TRAIL_FRAMES_DEFAULT;
static u32 sWrites = 0;        /* atomic */
static u32 sWriteUsTotal = 0;  /* atomic */
static u32 sWriteUsMax = 0;    /* racy max: diagnostics only */
static u32 sBroken = 0;

static u32 TicksToUs(u64 t) {
    return (u32)(t * 1000u / (u64)CPU_TICKS_PER_MSEC);
}

/* ---------------------------------------------------------------- raw FS ---- */

static int OpenArchive(void) {
    return R_SUCCEEDED(FSUSER_OpenArchive(&sArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
}

/* boot.txt -> boot-prev.txt (the previous boot's trail survives one more boot). */
static int RotatePrevious(void) {
    FSUSER_DeleteFile(sArchive, fsMakePath(PATH_ASCII, BOOT_TRAIL_PREV_PATH));
    return R_SUCCEEDED(FSUSER_RenameFile(sArchive, fsMakePath(PATH_ASCII, BOOT_TRAIL_PATH), sArchive,
                                         fsMakePath(PATH_ASCII, BOOT_TRAIL_PREV_PATH)));
}

static int OpenFresh(void) {
    const Result rc = FSUSER_OpenFile(&sFile, sArchive, fsMakePath(PATH_ASCII, BOOT_TRAIL_PATH),
                                      FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) {
        sFile = 0;
        return 0;
    }
    FSFILE_SetSize(sFile, 0); /* the rename failed (no previous trail): start empty anyway */
    return 1;
}

static void NoteWriteCost(u64 ticks) {
    const u32 us = TicksToUs(ticks);
    __atomic_fetch_add(&sWrites, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sWriteUsTotal, us, __ATOMIC_RELAXED);
    if (us > sWriteUsMax) {
        sWriteUsMax = us;
    }
}

/* One flushed write at a reserved offset. Safe from any thread and from the exception handler. */
static void AppendRaw(const char* s, u32 len) {
    u32 written = 0;
    if (!sOpen || len == 0) {
        return;
    }
    const u32 offset = __atomic_fetch_add(&sOffset, len, __ATOMIC_ACQ_REL);
    if (offset + len > BOOT_TRAIL_CAP) {
        return;
    }
    const u64 t0 = svcGetSystemTick();
    if (R_FAILED(FSFILE_Write(sFile, &written, offset, s, len, FS_WRITE_FLUSH))) {
        sBroken++;
        return;
    }
    NoteWriteCost(svcGetSystemTick() - t0);
}

static void CrashSink(const char* text, size_t len) {
    AppendRaw(text, (u32)len);
}

/* ---------------------------------------------------------------- lines ---- */

static void AppendLine(const char* line) {
    char buf[BOOT_TRAIL_LINE];
    const u32 us = TicksToUs(svcGetSystemTick() - sOpenTick);
    int n = snprintf(buf, sizeof(buf), "+%lu.%03lu ms %s", (unsigned long)(us / 1000u), (unsigned long)(us % 1000u),
                     line);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    AppendRaw(buf, (u32)n);
}

void gdx3ds_boot_trail_file(const char* line) {
    if (!sOpen || line == NULL) {
        return;
    }
    AppendLine(line);
}

void gdx3ds_boot_trail(const char* line) {
    if (!sOpen || line == NULL) {
        return;
    }
    svcOutputDebugString(line, strlen(line));
    AppendLine(line);
}

void gdx3ds_boot_trail_f(const char* fmt, ...) {
    char line[BOOT_TRAIL_LINE - 24];
    va_list args;
    if (!sOpen) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    gdx3ds_boot_trail(line);
}

int gdx3ds_boot_trail_active(void) {
    return sOpen;
}

/* ---------------------------------------------------------------- lifecycle ---- */

void gdx3ds_boot_trail_open(void) {
    if (!GDX3DS_BOOT_TRAIL || sOpen) {
        return;
    }
    const u64 t0 = svcGetSystemTick();
    sOpenTick = t0;
    if (!OpenArchive()) {
        svcOutputDebugString("[boottrail] sdmc archive open FAILED", 36);
        return;
    }
    sRotated = RotatePrevious();
    if (!OpenFresh()) {
        svcOutputDebugString("[boottrail] boot.txt open FAILED", 32);
        return;
    }
    sOpen = 1;
    sOpenCostTicks = svcGetSystemTick() - t0;
    gdx3ds_boot_trail_f("[boottrail] open build=" GDX3DS_BUILD_ID " file=sdmc:" BOOT_TRAIL_PATH
                        " prev=%s open_us=%lu (main() entry; before config, gfxInit and the file log)",
                        sRotated ? "rotated" : "none", (unsigned long)TicksToUs(sOpenCostTicks));
    gdx3ds_crash_arm_boot(&CrashSink);
    gdx3ds_boot_trail("[boottrail] main-thread exception handler armed for the boot window");
}

void gdx3ds_boot_trail_latch_config(void) {
    if (!sOpen) {
        return;
    }
    const int on = gdx3ds_config_get_int("debug", "boot_trail", 1);
    sFrames = gdx3ds_config_get_int("debug", "boot_trail_frames", BOOT_TRAIL_FRAMES_DEFAULT);
    if (sFrames < 0) {
        sFrames = 0;
    }
    gdx3ds_boot_trail_f("[boottrail] config loaded: entries=%d/%d boot_trail=%d boot_trail_frames=%d",
                        gdx3ds_config_entry_count(), gdx3ds_config_entry_capacity(), on, sFrames);
    if (!on) {
        gdx3ds_boot_trail_close("[debug] boot_trail=0");
    }
}

void gdx3ds_boot_trail_frame(unsigned long frame) {
    if (!sOpen) {
        return;
    }
    if (frame < (unsigned long)sFrames) {
        gdx3ds_boot_trail_f("[boottrail] host frame %lu", frame);
        return;
    }
    char why[48];
    snprintf(why, sizeof(why), "host frame %lu reached", frame);
    gdx3ds_boot_trail_close(why);
}

void gdx3ds_boot_trail_close(const char* why) {
    char line[BOOT_TRAIL_LINE - 24];
    if (!sOpen) {
        return;
    }
    const u32 writes = __atomic_load_n(&sWrites, __ATOMIC_RELAXED);
    const u32 total = __atomic_load_n(&sWriteUsTotal, __ATOMIC_RELAXED);
    snprintf(line, sizeof(line),
             "[boottrail] closed (%s): crumbs=%lu bytes=%lu write_us avg=%lu max=%lu total=%lu open_us=%lu "
             "failed=%lu window_ms=%lu",
             why, (unsigned long)writes, (unsigned long)__atomic_load_n(&sOffset, __ATOMIC_RELAXED),
             (unsigned long)(writes != 0 ? total / writes : 0u), (unsigned long)sWriteUsMax, (unsigned long)total,
             (unsigned long)TicksToUs(sOpenCostTicks), (unsigned long)sBroken,
             (unsigned long)(TicksToUs(svcGetSystemTick() - sOpenTick) / 1000u));
    gdx3ds_boot_trail(line);
    gdx3ds_filelog_write(line, strlen(line));
    gdx3ds_crash_disarm_boot();
    sOpen = 0;
    FSFILE_Close(sFile);
    FSUSER_CloseArchive(sArchive);
    sFile = 0;
}
