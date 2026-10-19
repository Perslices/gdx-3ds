/* port/3ds/gfx/nrr/gdx3ds_nrr_crumb.c -- n60hwdiag: durable breadcrumbs + the boot-cycling NRR
 * bisect. Contract: gdx3ds_nrr_crumb.h. Progress: docs/research/native/n60hwdiag-progress.md.
 *
 * Why raw FS and not stdio: libctru's archive_dev passes FS_WRITE_FLUSH to FSFILE_Write only for an
 * O_SYNC file, so an fflush'd stdio line can still sit in the FS module when the console is powered
 * off. Every crumb here is one FSFILE_Write(..., FS_WRITE_FLUSH) on a handle kept open for the
 * session; nothing is buffered on our side.
 *
 * Threads: crumbs come from the main thread (host loop, game fiber, the walk on renderthread=0);
 * a recursive lock keeps a crumb from another thread whole anyway. */
#include "gdx3ds_nrr_crumb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "../../vendor/citro3d/gdx/c3d_gdx.h" /* C3Dgdx_QueueWait hook, C3Dgdx_QueueWindowStats */

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_override(const char* section, const char* key, const char* value) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern void gdx3ds_filelog_set_session_path(const char* path) __attribute__((weak));
extern void gdx3ds_filelog_set_durable(int on) __attribute__((weak));
extern void gdx3ds_filelog_flush(void) __attribute__((weak));
extern const char* gdx3ds_fps_hud_build_id(void) __attribute__((weak));
extern unsigned int gGameFrameCount __attribute__((weak));
extern int gGameMode __attribute__((weak));   /* decomp s32 */
extern int gCourseIndex __attribute__((weak)); /* decomp s32 */

#define CRUMB_PATH "/3ds/gdiffuser/nrr_crumbs.txt"
#define CRUMB_COUNTER_PATH "/3ds/gdiffuser/nrr_bisect.txt"
#define CRUMB_LOG_FMT "sdmc:/3ds/gdiffuser/log-nrrboot%03u.txt"
#define CRUMB_FILE_CAP (4u * 1024u * 1024u)
#define CRUMB_EPOCH_FRAMES 8      /* frame begin/end crumbs at the start of an epoch */
#define CRUMB_AFTER_FRAMES 2      /* ... and for the frames right after any other crumb */
#define CRUMB_SYNC_FRAMES_DEFAULT 600
#define CRUMB_SUBSETS 8
#define CRUMB_GXQ_EVERY 256 /* frames between [gxq] window-stat lines */

int gGdxNrrCrumbs = 0;

static RecursiveLock sLock;
static int sLockInited = 0;
static FS_Archive sArchive = 0;
static Handle sFile = 0;
static u64 sFileOffset = 0;
static int sFileBroken = 0;
static unsigned long sId = 0;
static u64 sBootTick = 0;
static u64 sLastWriteTicks = 0;
static u64 sWriteTicksTotal = 0;
static u64 sWriteTicksMax = 0;
static unsigned long sWrites = 0;

static unsigned sEpoch = 0;
static int sEpochMode = -999;
static int sEpochCourse = -999;
static uint16_t sWant[NRR_CRUMB_SITES];
static int sFrameCrumbs = 0;  /* frames whose begin/end still get crumbs */
static int sInFrameCrumb = 0; /* a frame crumb must not extend the frame window */
static int sSyncFrames = 0;   /* [debug] filelog_sync_frames */
static int sSyncLeft = 0;
static int sCurCat = -1;

/* The bisect. */
static int sBisectOn = 0;
static int sBisectForce = -1;
static unsigned sBoot = 0;
static int sSubset = -1;
static int sCounterOk = 0;
static char sTag[24];
static char sLogPath[64];

/* Boot N runs subset N % 8. The old RUN V first: boot 0 says whether the gxQueue fix (0340435) holds. */
enum { SUB_ALL_D2 = 0, SUB_NRR0, SUB_HUD, SUB_TRK, SUB_MACH, SUB_SKY, SUB_ALL_D0, SUB_ALL_D1 };
static const char* const kSubsetName[CRUMB_SUBSETS] = { "all-d2", "nrr0", "hud", "trk", "mach", "sky", "all-d0", "all-d1" };
static const char* const kSubsetWhat[CRUMB_SUBSETS] = {
    "all-d2 (the old RUN V, with the gxQueue fix): hud+trk+mach+sky, nrr_direct=2",
    "nrr=0 (this tree's baseline at this ini)",
    "hud only (nrr_direct as the ini)",
    "trk only (nrr_direct as the ini)",
    "mach only (nrr_direct as the ini)",
    "sky only (nrr_direct as the ini)",
    "hud+trk+mach+sky, nrr_direct=0",
    "hud+trk+mach+sky, nrr_direct=1 (direct words drawn)",
};
static unsigned long sGxqFrames = 0;
static int sGxqSelftest = 0; /* [debug] gxq_selftest=N: lose one GX completion at the N-th race frame */
static int sRaceFrames = 0;

static int CfgInt(const char* sec, const char* key, int fallback) {
    return gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int(sec, key, fallback) : fallback;
}

static void EnsureLock(void) {
    if (!sLockInited) {
        RecursiveLock_Init(&sLock);
        sLockInited = 1;
    }
}

static unsigned long TicksToUs(u64 t) {
    return (unsigned long)(t * 1000u / (u64)CPU_TICKS_PER_MSEC);
}

static int FormatQueueWindows(char* out, int cap);

/* ---------------------------------------------------------------- raw FS ---- */

static int OpenArchive(void) {
    if (sArchive != 0) {
        return 1;
    }
    return R_SUCCEEDED(FSUSER_OpenArchive(&sArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
}

static int OpenRw(Handle* h, const char* path) {
    return R_SUCCEEDED(FSUSER_OpenFile(h, sArchive, fsMakePath(PATH_ASCII, path), FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, 0));
}

static void OpenCrumbFile(void) {
    u64 size = 0;
    if (!OpenArchive() || !OpenRw(&sFile, CRUMB_PATH)) {
        sFile = 0;
        sFileBroken = 1;
        return;
    }
    if (R_SUCCEEDED(FSFILE_GetSize(sFile, &size))) {
        sFileOffset = size;
    }
}

/* One flushed append; the time it took is reported in the next crumb's w=. */
static void WriteDurable(const char* s, u32 len) {
    u32 written = 0;
    const u64 t0 = svcGetSystemTick();
    if (sFile == 0 || sFileBroken || sFileOffset + len > CRUMB_FILE_CAP) {
        return;
    }
    if (R_FAILED(FSFILE_Write(sFile, &written, sFileOffset, s, len, FS_WRITE_FLUSH))) {
        sFileBroken = 1;
        return;
    }
    sFileOffset += written;
    sLastWriteTicks = svcGetSystemTick() - t0;
    sWriteTicksTotal += sLastWriteTicks;
    if (sLastWriteTicks > sWriteTicksMax) {
        sWriteTicksMax = sLastWriteTicks;
    }
    sWrites++;
}

/* The counter: the number of the boot about to run (missing or unreadable = 0); k+1 goes back,
 * flushed, before this function returns. */
static int ReadAndAdvanceCounter(unsigned* bootOut) {
    Handle h = 0;
    char buf[16];
    char out[16];
    u32 got = 0;
    u32 written = 0;
    unsigned boot = 0;
    if (!OpenArchive() || !OpenRw(&h, CRUMB_COUNTER_PATH)) {
        return 0;
    }
    memset(buf, 0, sizeof(buf));
    if (R_SUCCEEDED(FSFILE_Read(h, &got, 0, buf, sizeof(buf) - 1)) && got > 0) {
        boot = (unsigned)strtoul(buf, NULL, 10);
    }
    const int n = snprintf(out, sizeof(out), "%u\n", boot + 1u);
    const int ok = R_SUCCEEDED(FSFILE_SetSize(h, 0)) &&
                   R_SUCCEEDED(FSFILE_Write(h, &written, 0, out, (u32)n, FS_WRITE_FLUSH)) && written == (u32)n;
    FSFILE_Close(h);
    *bootOut = boot;
    return ok;
}

/* ---------------------------------------------------------------- crumbs ---- */

static void StartSyncWindow(void) {
    if (sSyncFrames <= 0) {
        return;
    }
    if (sSyncLeft == 0 && gdx3ds_filelog_set_durable != NULL) {
        gdx3ds_filelog_set_durable(1);
    }
    sSyncLeft = sSyncFrames;
}

static void EndSyncWindow(void) {
    if (gdx3ds_filelog_set_durable != NULL) {
        gdx3ds_filelog_set_durable(0);
    }
    char win[96];
    FormatQueueWindows(win, sizeof(win));
    gdx3ds_nrr_crumb("filelog sync window closed after %d frames | crumb writes n=%lu avg_us=%lu max_us=%lu | %s",
                     sSyncFrames, sWrites, sWrites ? TicksToUs(sWriteTicksTotal) / sWrites : 0ul,
                     TicksToUs(sWriteTicksMax), win);
}

/* A change of (gGameMode, gCourseIndex) is a new epoch: budgets reset, frame crumbs and the
 * synchronous filelog window restart. */
static void EpochPoll(void) {
    const int mode = &gGameMode != NULL ? gGameMode : -1;
    const int course = &gCourseIndex != NULL ? gCourseIndex : -1;
    if (mode == sEpochMode && course == sEpochCourse) {
        return;
    }
    sEpochMode = mode;
    sEpochCourse = course;
    sEpoch++;
    memset(sWant, 0, sizeof(sWant));
    sFrameCrumbs = CRUMB_EPOCH_FRAMES;
    StartSyncWindow();
    gdx3ds_nrr_crumb("epoch %u: gGameMode=%d gCourseIndex=%d linfree=%uK", sEpoch, mode, course,
                     gdx3ds_nrr_crumb_linfree_kb());
}

void gdx3ds_nrr_crumb(const char* fmt, ...) {
    char line[560];
    va_list ap;
    if (!gGdxNrrCrumbs) {
        return;
    }
    EnsureLock();
    RecursiveLock_Lock(&sLock);
    EpochPoll();
    const u64 now = svcGetSystemTick();
    int n = snprintf(line, sizeof(line), "[crumb] #%lu f=%u e=%u ms=%lu w=%lu | ", ++sId,
                     &gGameFrameCount != NULL ? gGameFrameCount : 0u, sEpoch,
                     (unsigned long)((now - sBootTick) / (u64)CPU_TICKS_PER_MSEC), TicksToUs(sLastWriteTicks));
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    if (gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(line, (size_t)n);
    }
    line[n++] = '\n';
    WriteDurable(line, (u32)n);
    if (!sInFrameCrumb && sFrameCrumbs < CRUMB_AFTER_FRAMES) {
        sFrameCrumbs = CRUMB_AFTER_FRAMES;
    }
    RecursiveLock_Unlock(&sLock);
}

int gdx3ds_nrr_crumb_want(int site, int limit) {
    if (!gGdxNrrCrumbs || site < 0 || site >= NRR_CRUMB_SITES) {
        return 0;
    }
    EnsureLock();
    RecursiveLock_Lock(&sLock);
    EpochPoll();
    const int ok = sWant[site] < limit;
    if (ok) {
        sWant[site]++;
    }
    RecursiveLock_Unlock(&sLock);
    return ok;
}

void gdx3ds_nrr_crumb_set_cat(int cat) {
    sCurCat = cat;
}

int gdx3ds_nrr_crumb_cat(void) {
    return sCurCat;
}

int gdx3ds_nrr_crumb_buf(char* out, int cap, const char* name, const void* p) {
    const u32 pa = p != NULL ? osConvertVirtToPhys(p) : 0u;
    return snprintf(out, (size_t)cap, "%s=%p pa=0x%08lx al=%lu%s", name, p, (unsigned long)pa,
                    (unsigned long)((uintptr_t)p & 0x7Fu), (p != NULL && pa == 0) ? " NOT-GPU-VISIBLE" : "");
}

unsigned gdx3ds_nrr_crumb_linfree_kb(void) {
    return (unsigned)(linearSpaceFree() / 1024u);
}

/* ---------------------------------------------------------------- GX queue ---- */

static int FormatQueueWindows(char* out, int cap) {
    unsigned w[3];
    C3Dgdx_QueueWindowStats(w);
    return snprintf(out, (size_t)cap, "ctx save/restore windows=%u completed-inside=%u changed-inside=%u", w[0], w[1],
                    w[2]);
}

/* GPU external registers 0x1EF00030..3C (busy/IRQ state) and the P3D command-list registers
 * 0x238..0x23D (sizes, addresses, jumps), raw, through GSP. */
static int FormatGpuRegs(char* out, int cap) {
    u32 ext[4];
    u32 cmd[6];
    memset(ext, 0, sizeof(ext));
    memset(cmd, 0, sizeof(cmd));
    const Result r0 = GSPGPU_ReadHWRegs(0x400030, ext, sizeof(ext));
    const Result r1 = GSPGPU_ReadHWRegs(0x401000 + 0x238 * 4, cmd, sizeof(cmd));
    return snprintf(out, (size_t)cap,
                    "gpu ext[30..3c]=%08lx %08lx %08lx %08lx (rc %08lx) cmdlist size=%08lx/%08lx addr=%08lx/%08lx "
                    "(rc %08lx)", (unsigned long)ext[0], (unsigned long)ext[1], (unsigned long)ext[2],
                    (unsigned long)ext[3], (unsigned long)r0, (unsigned long)cmd[0], (unsigned long)cmd[1],
                    (unsigned long)cmd[2], (unsigned long)cmd[3], (unsigned long)r1);
}

static void QueueStuck(const char* where, const struct tag_gxCmdQueue_s* q, unsigned waitedMs, int forcing) {
    char regs[200];
    char win[96];
    char line[480];
    const volatile gxCmdQueue_s* vq = (const volatile gxCmdQueue_s*)q;
    FormatGpuRegs(regs, sizeof(regs));
    FormatQueueWindows(win, sizeof(win));
    const int n = snprintf(line, sizeof(line),
                           "[gxq] STUCK %s waited=%u ms queue num=%u cur=%u last=%u max=%u%s | %s | %s", where, waitedMs,
                           (unsigned)vq->numEntries, (unsigned)vq->curEntry, (unsigned)vq->lastEntry,
                           (unsigned)vq->maxEntries, forcing ? " | forcing" : " | still waiting (a wedged queue is never abandoned)",
                           regs, win);
    if (gGdxNrrCrumbs) {
        gdx3ds_nrr_crumb("%s", line);
    } else if (gdx3ds_filelog_write != NULL && n > 0) {
        gdx3ds_filelog_write(line, (size_t)n);
    }
    if (gdx3ds_filelog_flush != NULL) {
        gdx3ds_filelog_flush();
    }
}

static void GxqWindowLine(void) {
    char win[96];
    char line[160];
    FormatQueueWindows(win, sizeof(win));
    const int n = snprintf(line, sizeof(line), "[gxq] frames=%lu %s watch=%d ms", sGxqFrames, win, C3Dgdx_QueueWatchMs);
    if (gdx3ds_filelog_write != NULL && n > 0) {
        gdx3ds_filelog_write(line, (size_t)n);
    }
}

/* [debug] gxq_selftest=N (test only, needs nrr_crumbs + gxq_watch_ms): at the N-th frame of a race
 * (gGameMode 1) the next watched queue wait reports two synthetic STUCK slices. */
static void GxqSelftestTick(void) {
    if (sGxqSelftest <= 0 || &gGameMode == NULL || gGameMode != 1) {
        return;
    }
    if (++sRaceFrames != sGxqSelftest) {
        return;
    }
    C3Dgdx_QueueSelftestArm();
    gdx3ds_nrr_crumb("gxq selftest armed at race frame %d: the next watched wait reports 2 synthetic STUCK slices",
                     sRaceFrames);
}

static void LatchQueueWatch(void) {
    const int ms = CfgInt("debug", "gxq_watch_ms", 0);
    C3Dgdx_QueueWatchMs = ms > 0 ? ms : 0;
    sGxqSelftest = CfgInt("debug", "gxq_selftest", 0);
    if (C3Dgdx_QueueWatchMs > 0) {
        C3Dgdx_QueueStuckHook = QueueStuck;
    }
}

/* ---------------------------------------------------------------- frames ---- */

static void FrameCrumb(const char* what) {
    sInFrameCrumb = 1;
    gdx3ds_nrr_crumb("%s", what);
    sInFrameCrumb = 0;
}

void gdx3ds_nrr_crumb_frame_begin_pre(void) {
    if (!gGdxNrrCrumbs) {
        return;
    }
    EpochPoll();
    if (sFrameCrumbs > 0) {
        FrameCrumb("frame begin > C3D_FrameBegin (waits for the previous frame's GPU work)");
    }
}

void gdx3ds_nrr_crumb_frame_begin_post(void) {
    if (gGdxNrrCrumbs && sFrameCrumbs > 0) {
        FrameCrumb("frame begin < C3D_FrameBegin returned");
    }
}

void gdx3ds_nrr_crumb_frame_end_pre(void) {
    if (gGdxNrrCrumbs && sFrameCrumbs > 0) {
        FrameCrumb("frame end > C3D_FrameEnd (submits this frame's command list)");
    }
}

void gdx3ds_nrr_crumb_frame_end_post(void) {
    if (!gGdxNrrCrumbs) {
        return;
    }
    if (++sGxqFrames % CRUMB_GXQ_EVERY == 0) {
        GxqWindowLine();
    }
    GxqSelftestTick();
    if (sFrameCrumbs > 0) {
        FrameCrumb("frame end < C3D_FrameEnd returned");
        sFrameCrumbs--;
    }
    if (sSyncLeft > 0 && --sSyncLeft == 0) {
        EndSyncWindow();
    }
}

/* ---------------------------------------------------------------- bisect ---- */

static void Force(const char* key, int value) {
    char v[8];
    snprintf(v, sizeof(v), "%d", value);
    if (gdx3ds_config_override != NULL) {
        gdx3ds_config_override("native", key, v);
    }
}

static int SubsetIsAll(int k) {
    return k == SUB_ALL_D2 || k == SUB_ALL_D0 || k == SUB_ALL_D1;
}

static int SubsetDirect(int k, int iniDirect) {
    if (k == SUB_ALL_D2) {
        return 2;
    } else if (k == SUB_ALL_D0) {
        return 0;
    } else if (k == SUB_ALL_D1) {
        return 1;
    }
    return iniDirect;
}

static void ApplySubset(int k) {
    const int iniDirect = CfgInt("native", "nrr_direct", 0);
    if (k == SUB_NRR0) {
        Force("nrr", 0);
        return;
    }
    Force("nrr", 1);
    Force("nrr_hud", k == SUB_HUD || SubsetIsAll(k));
    Force("nrr_trk", k == SUB_TRK || SubsetIsAll(k));
    Force("nrr_mach", k == SUB_MACH || SubsetIsAll(k));
    Force("nrr_sky", k == SUB_SKY || SubsetIsAll(k));
    Force("nrr_direct", SubsetDirect(k, iniDirect));
}

static void SelectSubset(void) {
    sCounterOk = 0;
    if (sBisectOn) {
        sCounterOk = ReadAndAdvanceCounter(&sBoot);
    }
    sSubset = sBisectForce >= 0 ? sBisectForce % CRUMB_SUBSETS : (int)(sBoot % CRUMB_SUBSETS);
    ApplySubset(sSubset);
    snprintf(sTag, sizeof(sTag), "b%u s%d:%s", sBoot, sSubset, kSubsetName[sSubset]);
    if (sBisectOn) {
        snprintf(sLogPath, sizeof(sLogPath), CRUMB_LOG_FMT, sBoot);
        if (gdx3ds_filelog_set_session_path != NULL) {
            gdx3ds_filelog_set_session_path(sLogPath);
        }
    }
}

void gdx3ds_nrr_crumb_boot_select(void) {
    sBootTick = svcGetSystemTick();
    sBisectOn = CfgInt("debug", "nrr_bisect_cycle", 0) != 0;
    sBisectForce = CfgInt("debug", "nrr_bisect_force", -1);
    const int crumbs = CfgInt("debug", "nrr_crumbs", 0) != 0;
    sSyncFrames = CfgInt("debug", "filelog_sync_frames", crumbs ? CRUMB_SYNC_FRAMES_DEFAULT : 0);
    LatchQueueWatch();
    if (sBisectOn || sBisectForce >= 0) {
        SelectSubset();
    }
    if (!crumbs) {
        return;
    }
    OpenCrumbFile();
    gGdxNrrCrumbs = 1;
}

void gdx3ds_nrr_crumb_boot_receipt(void) {
    char line[256];
    const char* build = gdx3ds_fps_hud_build_id != NULL ? gdx3ds_fps_hud_build_id() : "?";
    if (sSubset >= 0) {
        int n = snprintf(line, sizeof(line),
                         "[nrrbisect] boot=%u subset=%d (%s: %s) cycle=%d force=%d counter=%s log=%s build=%s",
                         sBoot, sSubset, kSubsetName[sSubset], kSubsetWhat[sSubset], sBisectOn, sBisectForce,
                         !sBisectOn ? "off" : (sCounterOk ? "advanced" : "FAILED"), sLogPath[0] ? sLogPath : "log.txt",
                         build);
        if (gdx3ds_filelog_write != NULL && n > 0) {
            gdx3ds_filelog_write(line, (size_t)n);
        }
        if (gGdxNrrCrumbs) {
            gdx3ds_nrr_crumb("%s", line);
        }
    }
    if (gGdxNrrCrumbs) {
        gdx3ds_nrr_crumb("boot: nrr_crumbs=1 file=sdmc:" CRUMB_PATH " offset=%lu%s filelog_sync_frames=%d build=%s "
                         "linfree=%uK gxq_watch_ms=%d",
                         (unsigned long)sFileOffset, sFileBroken ? " (FILE OPEN FAILED: filelog copy only)" : "",
                         sSyncFrames, build, gdx3ds_nrr_crumb_linfree_kb(), C3Dgdx_QueueWatchMs);
        StartSyncWindow(); /* the boot itself, up to the first epoch's own window */
    }
}

const char* gdx3ds_nrr_crumb_tag(void) {
    return sTag;
}
