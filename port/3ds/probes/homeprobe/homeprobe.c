/* port/3ds/probes/homeprobe/homeprobe.c -- the Old 3DS HOME-freeze bisect title (NEXT.md C4j,
 * docs/research/native/o3dshome2-progress.md).
 *
 * The game freezes the Old 3DS when HOME is pressed: libctru completes the whole jump
 * (the game's own receipts prove GSPGPU_ReleaseRight and APT_JumpToHomeMenu ran), the HOME
 * menu starts loading (the SYSTEM region loses ~6 MB) and then never draws. FBI -- also a
 * libctru CIA -- goes to HOME fine on the same console. This title is FBI-simple and adds
 * the game's ingredients one at a time, chosen at runtime, so one SD round trip bisects:
 *
 *   A  plain: citro3d clearing the top screen, console on the bottom (the game's gfx shape)
 *   X  A + ndsp: DSP component loaded, one channel streaming a quiet tone from linear memory
 *   Y  X + the game's core-1 shape: APT_SetAppCpuTimeLimit(30), a busy 0x18 thread and a
 *        parked 0x19 thread on core 1, both parked by the suspend hook like the game's
 *   B  Y + the heap filled the way the game fills it (malloc + touch until ~512 KB is left)
 *
 * Built twice (CMakeLists.txt): once with the game's own gdiffuser.rsf + banner, once with an
 * FBI-style rsf, under distinct title IDs so both install next to the game.
 *
 * Log: sdmc:/3ds/homeprobe/log-<title>.txt, one flushed line per event, with a 500 ms
 * "parked" heartbeat from a core-0 watcher thread while the app is suspended. */
#include <3ds.h>
#include <citro3d.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef HOMEPROBE_TAG
#define HOMEPROBE_TAG "probe"
#endif

#define DISPLAY_TRANSFER_FLAGS                                                                      \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TONE_RATE 32000
#define TONE_FRAMES 1024
#define TONE_BUFS 2
#define SYSCORE_PERCENT 30
#define HEAP_LEAVE_FREE (512u * 1024u)
#define HEAP_CHUNK (256u * 1024u)

typedef enum { MODE_PLAIN = 0, MODE_NDSP, MODE_CORE1, MODE_HEAP } ProbeMode;

static const char* const kModeNames[] = {"A plain", "X ndsp", "Y ndsp+core1", "B ndsp+core1+heap"};

static FILE* sLog;
static u64 sBootTick;
static volatile int sSuspended;
static volatile u64 sSuspendTick;
static volatile int sQuit;
static aptHookCookie sAptCookie;
static LightEvent sParkEvent;

static ndspWaveBuf sWaveBufs[TONE_BUFS];
static s16* sWaveMem;
static int sNdspUp;

static Thread sBusyThread;
static Thread sIdleThread;
static Thread sWatchThread;
static volatile u32 sBusyLoops;
static int sCore1Granted;

static void** sHeapChunks;
static unsigned sHeapChunkCount;
static unsigned long sHeapBytes;

/* ---- log ------------------------------------------------------------------------------ */

static unsigned long msSince(u64 tick) {
    return (unsigned long)((svcGetSystemTick() - tick) / CPU_TICKS_PER_MSEC);
}

static void logLine(const char* fmt, ...) {
    char line[256];
    int n = snprintf(line, sizeof(line), "[%7lums] ", msSince(sBootTick));
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
    va_end(args);
    if (!sSuspended) {
        printf("%s\n", line); /* the console framebuffer is not ours to draw while suspended */
    }
    if (sLog != NULL) {
        fprintf(sLog, "%s\n", line);
        fflush(sLog);
    }
}

static void openLog(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/homeprobe", 0777);
    sLog = fopen("sdmc:/3ds/homeprobe/log-" HOMEPROBE_TAG ".txt", "a");
}

/* ---- APT: park our threads like the game does, and say what happened ----------------- */

static void aptProbeHook(APT_HookType hook, void* param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            sSuspendTick = svcGetSystemTick();
            LightEvent_Clear(&sParkEvent);
            sSuspended = 1;
            logLine("apt %s hook: threads parking, handing over to libctru",
                    hook == APTHOOK_ONSUSPEND ? "SUSPEND (HOME)" : "SLEEP (lid)");
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            if (sCore1Granted) {
                APT_SetAppCpuTimeLimit(SYSCORE_PERCENT); /* NS drops the share on resume */
            }
            sSuspended = 0;
            LightEvent_Signal(&sParkEvent);
            logLine("apt %s hook after %lums away -> RESUMED OK",
                    hook == APTHOOK_ONRESTORE ? "RESTORE" : "WAKEUP", msSince(sSuspendTick));
            break;
        default:
            break;
    }
}

static void parkWhileSuspended(void) {
    while (sSuspended && !sQuit) {
        LightEvent_Wait(&sParkEvent);
    }
}

/* Core 0 watcher: the only thread that can talk while main sits inside the HOME jump. */
static void watchThreadMain(void* arg) {
    (void)arg;
    int wasActive = 1;
    while (!sQuit) {
        svcSleepThread(500000000LL);
        const int active = aptIsActive() ? 1 : 0;
        if (sSuspended || active != wasActive) {
            logLine("watch suspended=%d active=%d gpu=%d t=+%lums sysFreeK=%lu busyLoops=%lu",
                    sSuspended, active, gspHasGpuRight() ? 1 : 0, msSince(sSuspendTick),
                    (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) / 1024u),
                    (unsigned long)sBusyLoops);
        }
        wasActive = active;
    }
}

/* ---- X: ndsp streaming a quiet tone --------------------------------------------------- */

static void fillTone(s16* samples, unsigned frames) {
    for (unsigned i = 0; i < frames; i++) {
        const s16 v = (s16)(600.0f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / TONE_RATE));
        samples[2 * i] = v;
        samples[2 * i + 1] = v;
    }
}

static int startNdsp(void) {
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        logLine("ndspInit FAILED rc=0x%08lX (sdmc:/3ds/dspfirm.cdc missing?) -- mode X/Y/B invalid",
                (unsigned long)rc);
        return 0;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, TONE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    sWaveMem = (s16*)linearAlloc(TONE_BUFS * TONE_FRAMES * 4);
    if (sWaveMem == NULL) {
        logLine("linearAlloc for wavebufs FAILED");
        ndspExit();
        return 0;
    }
    memset(sWaveBufs, 0, sizeof(sWaveBufs));
    for (int i = 0; i < TONE_BUFS; i++) {
        s16* samples = sWaveMem + (size_t)i * TONE_FRAMES * 2;
        fillTone(samples, TONE_FRAMES);
        sWaveBufs[i].data_vaddr = samples;
        sWaveBufs[i].nsamples = TONE_FRAMES;
        DSP_FlushDataCache(samples, TONE_FRAMES * 4);
        ndspChnWaveBufAdd(0, &sWaveBufs[i]);
    }
    logLine("ndsp UP: 32 kHz stereo tone streaming on channel 0");
    return 1;
}

static void refillNdsp(void) {
    for (int i = 0; i < TONE_BUFS; i++) {
        if (sWaveBufs[i].status == NDSP_WBUF_DONE) {
            ndspChnWaveBufAdd(0, &sWaveBufs[i]);
        }
    }
}

/* ---- Y: the game's core-1 shape ------------------------------------------------------- */

static void busyThreadMain(void* arg) {
    (void)arg;
    while (!sQuit) {
        parkWhileSuspended();
        const u64 until = svcGetSystemTick() + 3ull * CPU_TICKS_PER_MSEC; /* ~3 ms of work */
        while (svcGetSystemTick() < until) {
            sBusyLoops++;
        }
        svcSleepThread(2000000LL);
    }
}

static void idleThreadMain(void* arg) {
    (void)arg;
    while (!sQuit) {
        parkWhileSuspended();
        svcSleepThread(5000000LL);
    }
}

static void startCore1Threads(void) {
    Result rc = APT_SetAppCpuTimeLimit(SYSCORE_PERCENT);
    if (R_FAILED(rc)) {
        logLine("APT_SetAppCpuTimeLimit(%d) FAILED rc=0x%08lX -- mode Y/B invalid", SYSCORE_PERCENT,
                (unsigned long)rc);
        return;
    }
    sCore1Granted = 1;
    sBusyThread = threadCreate(busyThreadMain, NULL, 16 * 1024, 0x18, 1, false);
    sIdleThread = threadCreate(idleThreadMain, NULL, 16 * 1024, 0x19, 1, false);
    logLine("core 1: share %d%% granted, busy thread %s (0x18), idle thread %s (0x19)", SYSCORE_PERCENT,
            sBusyThread != NULL ? "up" : "FAILED", sIdleThread != NULL ? "up" : "FAILED");
}

/* ---- B: fill the heap like the game --------------------------------------------------- */

static void fillHeap(void) {
    sHeapChunks = (void**)calloc(1024, sizeof(void*));
    if (sHeapChunks == NULL) {
        return;
    }
    while (sHeapChunkCount < 1024) {
        void* chunk = malloc(HEAP_CHUNK);
        if (chunk == NULL) {
            break;
        }
        memset(chunk, 0xA5, HEAP_CHUNK);
        sHeapChunks[sHeapChunkCount++] = chunk;
        sHeapBytes += HEAP_CHUNK;
    }
    /* Hand the last couple of chunks back so newlib and libctru keep working room. */
    unsigned long released = 0;
    while (sHeapChunkCount > 0 && released < HEAP_LEAVE_FREE) {
        free(sHeapChunks[--sHeapChunkCount]);
        released += HEAP_CHUNK;
        sHeapBytes -= HEAP_CHUNK;
    }
    logLine("heap filled: %lu KB held in %u chunks, linearFree=%lu KB", sHeapBytes / 1024u,
            sHeapChunkCount, (unsigned long)(linearSpaceFree() / 1024u));
}

/* ---- mode select + frame loop --------------------------------------------------------- */

static ProbeMode chooseMode(void) {
#ifdef HOMEPROBE_AUTOMODE
    return (ProbeMode)HOMEPROBE_AUTOMODE; /* emulator smoke build only: no buttons to press */
#endif
    printf("\n HOME probe (" HOMEPROBE_TAG ")\n\n");
    printf(" Press ONE button to pick a mode:\n\n");
    printf("  A  plain (citro3d + console)\n");
    printf("  X  + DSP tone (ndsp)\n");
    printf("  Y  + DSP + core-1 threads\n");
    printf("  B  + DSP + core-1 + full heap\n\n");
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_A) {
            return MODE_PLAIN;
        }
        if (down & KEY_X) {
            return MODE_NDSP;
        }
        if (down & KEY_Y) {
            return MODE_CORE1;
        }
        if (down & KEY_B) {
            return MODE_HEAP;
        }
        gspWaitForVBlank();
    }
    sQuit = 1;
    return MODE_PLAIN;
}

static void startMode(ProbeMode mode) {
    if (mode >= MODE_NDSP) {
        sNdspUp = startNdsp();
    }
    if (mode >= MODE_CORE1) {
        startCore1Threads();
    }
    if (mode >= MODE_HEAP) {
        fillHeap();
    }
}

static u32 frameColor(u32 frame) {
    const u32 phase = frame % 240u;
    const u32 level = phase < 120u ? phase : 240u - phase;
    return 0x202020FFu | ((0x40u + level) << 24);
}

static void runFrames(C3D_RenderTarget* top, ProbeMode mode) {
    printf("\x1b[2J\n Mode %s running.\n\n", kModeNames[mode]);
    printf(" NOW PRESS HOME.\n\n Note: did the HOME menu appear?\n");
    printf(" If it does, Resume, then Close\n from the HOME menu.\n\n START exits.\n");
    u32 frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        if (sNdspUp) {
            refillNdsp();
        }
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, frameColor(frame++), 0);
        C3D_FrameDrawOn(top);
        C3D_FrameEnd(0);
    }
}

static void shutdownMode(void) {
    sQuit = 1;
    LightEvent_Signal(&sParkEvent);
    if (sBusyThread != NULL) {
        threadJoin(sBusyThread, 1000000000LL);
    }
    if (sIdleThread != NULL) {
        threadJoin(sIdleThread, 1000000000LL);
    }
    if (sNdspUp) {
        ndspExit();
    }
}

int main(void) {
    sBootTick = svcGetSystemTick();
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    openLog();
    LightEvent_Init(&sParkEvent, RESET_STICKY);
    aptHook(&sAptCookie, aptProbeHook, NULL);
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    logLine("=== homeprobe " HOMEPROBE_TAG " boot: model=%s sysFreeK=%lu appFreeK=%lu",
            isNew3ds ? "new3ds" : "old3ds", (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) / 1024u),
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024u));
    sWatchThread = threadCreate(watchThreadMain, NULL, 8 * 1024, 0x18, 0, false);

    const ProbeMode mode = chooseMode();
    if (!sQuit) {
        logLine("mode %s chosen", kModeNames[mode]);
        startMode(mode);
        runFrames(top, mode);
    }
    logLine("leaving (START or close order)");
    shutdownMode();
    if (sWatchThread != NULL) {
        threadJoin(sWatchThread, 1000000000LL);
    }
    if (sLog != NULL) {
        fclose(sLog);
    }
    C3D_RenderTargetDelete(top);
    C3D_Fini();
    gfxExit();
    return 0;
}
