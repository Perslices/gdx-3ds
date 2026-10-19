/* port/3ds/audio/gdx3ds_audio_pcmdump.c -- see the header. Pure C, 3DS-only.
 *
 * AUDIOFIX round 6 (o3dscrackle, coordinator steer after n60-3d9): the round-5 ring-based
 * design (a small 128 KiB staging ring drained incrementally to SD every 100 ms) silently
 * dropped a real chunk whenever its own low-priority SD-write thread fell behind the SD
 * card's real latency -- on card n60-3d9 that was 1300-1900 DROPPED CHUNKS per 20 s run (out
 * of ~7800-9200), more than half the capture. Round 5 fixed the SYMPTOM (a silent splice with
 * no gap) but round 6 removes the CAUSE: there is no SD I/O at all while the window is open.
 *
 * NEW DESIGN. Capture into a single preallocated RAM buffer sized for the WHOLE window
 * (gdx_pcmdump_submit is a bounded LightLock+memcpy into that buffer, same discipline as
 * before, just no ring wrap and no writer-thread backlog to fall behind). The buffer is
 * never touched by SD I/O until the window ENDS -- the race finishes
 * (gdx3ds_frameskip_game_post_finish), the player opens the HOME menu
 * (gdx_pcmdump_notify_suspend, called from gdx3ds_audio_suspend), or the app exits
 * (gdx_pcmdump_shutdown) -- at which point the module's own low-priority writer thread (still
 * 0x3F, still no core grant, so it steals nothing from gameplay) does ONE sequential
 * fopen/fwrite/fclose of everything buffered so far. A capture can therefore never drop a
 * chunk to a slow SD card again: the window either holds everything in RAM, or it has not
 * started flushing yet.
 *
 * The one thing this module can still lose a chunk to is its OWN preallocated buffer running
 * out mid-window (submit() simply stops accepting once sCaptureCapacityFrames is reached --
 * expected and not a drop) or, defensively, an allocation that undershoots what gdx_pcmdump_init
 * itself just sized (should never happen; kept only as a belt-and-suspenders path). Either way
 * the silence-marker convention from round 5 (a CSV row flagged PCMDUMP_SILENCE_CHUNK_INDEX,
 * written back as real zero bytes, never a silent splice) stays as the fallback, and
 * `[pcm-dump] FINAL` keeps reporting `dropped_chunks` so a non-zero count is never silent.
 *
 * Two threads touch this module: the DRAIN (gdx_pcmdump_submit, a bounded LightLock+memcpy)
 * and this module's OWN low-priority writer thread (every blocking file op, plus polling the
 * three end-of-window conditions). Neither is the producer.
 */
#include "gdx3ds_audio_pcmdump.h"

#include <3ds.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_filelog.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
extern const char* gdx3ds_launch_variant_name(void); /* "main" when no launch variant fired */
extern int gGdxRaceActive; /* n64_gfx_bridge.cpp / decomp_port.c: a race-mode latch, set once */

/* decomp/game module: n60results' own "every human racer crossed the line" query
 * (port/3ds/game/gdx3ds_frameskip_game.c). Weak, same convention gdx_rt_glue.cpp already
 * uses for this symbol, so a build/test harness that never links the game module still
 * links this file -- the window just never gets the "race finished" trigger and instead
 * waits for HOME-pause or exit, which is a degraded capture, not a crash. */
extern int gdx3ds_frameskip_game_post_finish(void) __attribute__((weak));

#define PCMDUMP_SAMPLE_RATE 32000u
#define PCMDUMP_CHANNELS 2u
#define PCMDUMP_BYTES_PER_FRAME (PCMDUMP_CHANNELS * (uint32_t)sizeof(int16_t))
#define PCMDUMP_CHUNK_FRAMES 512u /* GDX3DS_AUDIO_CHUNK_FRAMES, duplicated here (see gdx3ds_audio_ndsp.c);
                                     submit()'s sanity guard against an unexpectedly large chunk */
#define PCMDUMP_WRITER_STACK (16u * 1024u)
#define PCMDUMP_WRITER_PRIO 0x3F /* below the game/preload threads: runs only when nothing else wants the CPU */
#define PCMDUMP_POLL_NS (100LL * 1000 * 1000) /* 100 ms: cheap flag checks only, no SD I/O, while the window is open */
#define PCMDUMP_WRITER_JOIN_NS (8LL * 1000 * 1000 * 1000) /* 8 s: shutdown may be the FIRST trigger, i.e. a
                                                              full one-shot flush of the whole window */
#define PCMDUMP_WAV_HEADER_BYTES 44u

#define PCMDUMP_SILENCE_CHUNK_INDEX 0xFFFFFFFFu /* CSV sentinel: a synthesized gap, not a real chunk */

/* Heap-headroom policy (coordinator ask, round 6): try the configured [debug] pcm_dump_s
 * duration; if the buffer (+ its small per-chunk meta table) would not fit with this much
 * free heap left over afterward, halve the duration and try again, down to a 5 s floor,
 * below which the capture is not useful enough to bother -- disable instead of shipping a
 * clip too short to A/B. 2 MiB is a deliberately generous margin on an Old 3DS's ~24 MiB
 * heap: this capture lives for the rest of the race, well past this module's own init, so
 * it must leave room for whatever the race itself still allocates afterward. */
#define PCMDUMP_HEAP_RESERVE_BYTES (2u * 1024u * 1024u)
#define PCMDUMP_SECONDS_FLOOR 5

typedef struct {
    uint32_t chunkIndex; /* the drain's sChunksSubmitted, or PCMDUMP_SILENCE_CHUNK_INDEX for a marker */
    uint32_t tMs;        /* MsSinceInit(), directly comparable to [audiocrk] window timestamps */
    uint32_t frames;     /* real frames (consumed from sCapturePcm) or silence frames (synthesized) */
} PcmDumpMetaRow;

static int sArmed;                   /* [debug] pcm_dump_s > 0 AND the heap check above passed */
static uint32_t sSecondsChosen;       /* final chosen duration, after any heap-driven fallback */
static uint32_t sCaptureCapacityFrames; /* sSecondsChosen * PCMDUMP_SAMPLE_RATE */
static int16_t* sCapturePcm;          /* sCaptureCapacityFrames * CHANNELS int16s, malloc'd once; REAL audio only */
static uint32_t sCaptureWritePos;     /* frames of real audio currently buffered; guarded by sLock */
static PcmDumpMetaRow* sMeta;         /* malloc'd once: one row per submitted chunk (+ a few for markers) */
static uint32_t sMetaCapacity;
static uint32_t sMetaCount;           /* guarded by sLock */
static uint32_t sPendingDropFrames;   /* belt-and-suspenders: see the file header. Should stay 0. */
static int sCaptureFull;              /* sCaptureWritePos reached capacity: submit() stops accepting more */
static int sLoggedFull;               /* writer thread only: the "buffered, waiting for a trigger" line fires once */
static LightLock sLock;               /* guards sCaptureWritePos/sMetaCount/sCaptureFull/sPendingDropFrames */
static int sStartRequested;           /* race went active at least once: start staging */
static int sDone;                     /* writer flushed and closed the files (normally or at shutdown) */
static uint32_t sCapturedWrittenFrames; /* real+silence frames actually fwrite()'n so far (flush in progress/done) */
static uint32_t sDroppedChunks;       /* defensive counter: should read 0 on a healthy run (see file header) */
static volatile int sStopRequested;   /* gdx_pcmdump_shutdown(): app exit */
static volatile int sSuspendNotified; /* gdx_pcmdump_notify_suspend(): HOME-menu pause (gdx3ds_audio_suspend) */
static Thread sWriterThread;
static FILE* sWavFile;
static FILE* sCsvFile;
static char sWavPath[64];

static void PcmDumpLogf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    svcOutputDebugString(buf, n);
    gdx3ds_filelog_write(buf, (size_t)n);
}

/* Total bytes malloc() could still hand out right now: the unsbrk'd remainder of the heap
 * grant, plus whatever free space already sits inside the sbrk'd arena. Same formula
 * gdx3ds_membudget.cpp's [mem-res] "totals" line uses for heapFree/arena, just without that
 * module's lever/logging machinery -- this module must work with [debug] membudget=0. */
static unsigned long PcmDumpFreeHeapBytes(void) {
    struct mallinfo mi = mallinfo();
    unsigned long heapGrant = (unsigned long)envGetHeapSize();
    unsigned long arena = (unsigned long)mi.arena;
    unsigned long headroom = (heapGrant > arena) ? (heapGrant - arena) : 0ul;
    return headroom + (unsigned long)mi.fordblks;
}

static void PutU32LE(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void PutU16LE(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* Canonical 44-byte PCM16 header. dataFrames is a PLACEHOLDER at open (0) and the true value
 * once the flush is done; WriteWavHeader is called twice on the same open file: once to
 * reserve the bytes, once (after an fseek back to 0) to patch them with the real count. */
static void WriteWavHeader(FILE* f, uint32_t dataFrames) {
    uint8_t h[PCMDUMP_WAV_HEADER_BYTES];
    uint32_t dataBytes = dataFrames * PCMDUMP_BYTES_PER_FRAME;
    uint32_t byteRate = PCMDUMP_SAMPLE_RATE * PCMDUMP_BYTES_PER_FRAME;
    memcpy(h + 0, "RIFF", 4);
    PutU32LE(h + 4, 36u + dataBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    PutU32LE(h + 16, 16u);
    PutU16LE(h + 20, 1u); /* PCM */
    PutU16LE(h + 22, (uint16_t)PCMDUMP_CHANNELS);
    PutU32LE(h + 24, PCMDUMP_SAMPLE_RATE);
    PutU32LE(h + 28, byteRate);
    PutU16LE(h + 32, (uint16_t)PCMDUMP_BYTES_PER_FRAME);
    PutU16LE(h + 34, 16u); /* bits per sample */
    memcpy(h + 36, "data", 4);
    PutU32LE(h + 40, dataBytes);
    fwrite(h, 1, sizeof(h), f);
}

/* Writer thread only, called once at flush time (never on the drain thread -- this module now
 * does ALL of its SD I/O in one place, after the window has already closed). */
static void OpenFilesOnce(void) {
    char csvPath[64];
    if (sWavFile != NULL) {
        return;
    }
    snprintf(sWavPath, sizeof(sWavPath), "sdmc:/3ds/gdiffuser/pcm-%s.wav",
             gdx3ds_launch_variant_name());
    snprintf(csvPath, sizeof(csvPath), "sdmc:/3ds/gdiffuser/pcm-%s.csv", gdx3ds_launch_variant_name());
    sWavFile = fopen(sWavPath, "wb");
    sCsvFile = fopen(csvPath, "w");
    if (sWavFile == NULL) {
        PcmDumpLogf("[pcm-dump] FAILED to open %s", sWavPath);
        return;
    }
    WriteWavHeader(sWavFile, 0); /* placeholder; patched in CloseFiles */
    if (sCsvFile != NULL) {
        fprintf(sCsvFile, "frame_offset,chunk_index,t_ms,frames\n");
    }
}

/* Writer thread only: patch the header with the TRUE frame count and close. */
static void CloseFiles(void) {
    if (sWavFile == NULL) {
        return;
    }
    fseek(sWavFile, 0, SEEK_SET);
    WriteWavHeader(sWavFile, sCapturedWrittenFrames);
    fclose(sWavFile);
    sWavFile = NULL;
    if (sCsvFile != NULL) {
        fclose(sCsvFile);
        sCsvFile = NULL;
    }
    PcmDumpLogf("[pcm-dump] FINAL %s: wrote %lu/%lu frames (%lu B), dropped_chunks=%lu", sWavPath,
               (unsigned long)sCapturedWrittenFrames, (unsigned long)sCaptureCapacityFrames,
               (unsigned long)(sCapturedWrittenFrames * PCMDUMP_BYTES_PER_FRAME),
               (unsigned long)sDroppedChunks);
}

/* Writer thread only: write `frames` frames of real zero silence (a defensive-path marker
 * row, see the file header -- expected count on a healthy run: zero calls). */
static void WriteZeroFrames(uint32_t frames) {
    static const int16_t kZero[PCMDUMP_CHUNK_FRAMES * PCMDUMP_CHANNELS] = {0};
    while (frames > 0u) {
        uint32_t n = (frames < PCMDUMP_CHUNK_FRAMES) ? frames : PCMDUMP_CHUNK_FRAMES;
        fwrite(kZero, 1, (size_t)n * PCMDUMP_BYTES_PER_FRAME, sWavFile);
        frames -= n;
    }
}

/* Writer thread only, called exactly once: the window has ended (one of the three triggers
 * fired). Opens the files, writes everything buffered in sCapturePcm/sMeta in ONE sequential
 * pass, patches the header, closes. This is the ONLY SD I/O this module ever does. */
static void FlushCapture(const char* reason) {
    uint32_t metaCount, dropped;
    LightLock_Lock(&sLock);
    if (sPendingDropFrames > 0u && sMetaCount < sMetaCapacity) {
        /* a trailing defensive drop with no following chunk to carry it -- flush it as its
         * own trailing silence marker rather than losing the time it represents */
        sMeta[sMetaCount].chunkIndex = PCMDUMP_SILENCE_CHUNK_INDEX;
        sMeta[sMetaCount].tMs = 0u;
        sMeta[sMetaCount].frames = sPendingDropFrames;
        sMetaCount++;
        sPendingDropFrames = 0u;
    }
    metaCount = sMetaCount;
    dropped = sDroppedChunks;
    LightLock_Unlock(&sLock);

    PcmDumpLogf("[pcm-dump] window closed (%s): %lu frames buffered (%.1f s), %lu meta rows, "
               "dropped_chunks=%lu -- writing now",
               reason, (unsigned long)sCaptureWritePos,
               (double)sCaptureWritePos / (double)PCMDUMP_SAMPLE_RATE, (unsigned long)metaCount,
               (unsigned long)dropped);

    OpenFilesOnce();
    if (sWavFile == NULL) {
        return; /* FAILED already logged */
    }

    uint32_t cursor = 0u;      /* read position within sCapturePcm, in frames */
    uint32_t frameOffset = 0u; /* cumulative frames written so far, for the CSV column */
    for (uint32_t i = 0; i < metaCount; i++) {
        const PcmDumpMetaRow* row = &sMeta[i];
        if (row->chunkIndex == PCMDUMP_SILENCE_CHUNK_INDEX) {
            WriteZeroFrames(row->frames);
        } else {
            fwrite(&sCapturePcm[(size_t)cursor * PCMDUMP_CHANNELS], 1,
                   (size_t)row->frames * PCMDUMP_BYTES_PER_FRAME, sWavFile);
            cursor += row->frames;
        }
        if (sCsvFile != NULL) {
            fprintf(sCsvFile, "%lu,%lu,%lu,%lu\n", (unsigned long)frameOffset,
                    (unsigned long)row->chunkIndex, (unsigned long)row->tMs,
                    (unsigned long)row->frames);
        }
        frameOffset += row->frames;
    }
    sCapturedWrittenFrames = frameOffset;
    fflush(sWavFile);
    CloseFiles();
}

static void PcmDumpWriterMain(void* arg) {
    (void)arg;
    for (;;) {
        int full;
        LightLock_Lock(&sLock);
        full = sCaptureFull;
        LightLock_Unlock(&sLock);
        if (full && !sLoggedFull) {
            sLoggedFull = 1;
            PcmDumpLogf("[pcm-dump] %lu s buffered in RAM (no SD I/O yet) -- waiting for race-finish, "
                       "HOME-pause or exit to write it",
                       (unsigned long)sSecondsChosen);
        }
        int stop = sStopRequested;
        int suspend = sSuspendNotified;
        int postFinish = (gdx3ds_frameskip_game_post_finish != NULL) &&
                         gdx3ds_frameskip_game_post_finish() != 0;
        if (stop) {
            FlushCapture("exit");
            sDone = 1;
            return;
        }
        if (suspend) {
            FlushCapture("home-pause");
            sDone = 1;
            return;
        }
        if (postFinish) {
            FlushCapture("race-finished");
            sDone = 1;
            return;
        }
        svcSleepThread(PCMDUMP_POLL_NS);
    }
}

void gdx_pcmdump_init(void) {
    int seconds = gdx3ds_config_get_int("debug", "pcm_dump_s", 0);
    if (seconds <= 0) {
        return; /* inert: no thread, no allocation -- the default */
    }

    int chosen = seconds;
    size_t bufBytes = 0, metaBytes = 0, metaRows = 0;
    unsigned long freeBefore = 0;
    for (;;) {
        bufBytes = (size_t)chosen * (size_t)PCMDUMP_SAMPLE_RATE * (size_t)PCMDUMP_BYTES_PER_FRAME;
        metaRows = (size_t)chosen * (size_t)PCMDUMP_SAMPLE_RATE / (size_t)PCMDUMP_CHUNK_FRAMES + 8u;
        metaBytes = metaRows * sizeof(PcmDumpMetaRow);
        freeBefore = PcmDumpFreeHeapBytes();
        if ((unsigned long)(bufBytes + metaBytes) + PCMDUMP_HEAP_RESERVE_BYTES <= freeBefore) {
            break; /* fits, with the reserve left over for whatever the race allocates next */
        }
        if (chosen <= PCMDUMP_SECONDS_FLOOR) {
            PcmDumpLogf("[pcm-dump] disabled: even %d s (%lu B + %lu B meta) does not fit safely "
                       "(free=%lu, reserve=%lu)",
                       chosen, (unsigned long)bufBytes, (unsigned long)metaBytes, freeBefore,
                       (unsigned long)PCMDUMP_HEAP_RESERVE_BYTES);
            return;
        }
        int next = chosen / 2;
        if (next < PCMDUMP_SECONDS_FLOOR) {
            next = PCMDUMP_SECONDS_FLOOR;
        }
        PcmDumpLogf("[pcm-dump] %d s (%lu B + %lu B meta) does not fit safely (free=%lu, "
                   "reserve=%lu): falling back to %d s",
                   chosen, (unsigned long)bufBytes, (unsigned long)metaBytes, freeBefore,
                   (unsigned long)PCMDUMP_HEAP_RESERVE_BYTES, next);
        chosen = next;
    }

    sCapturePcm = (int16_t*)malloc(bufBytes);
    sMeta = (PcmDumpMetaRow*)malloc(metaBytes);
    if (sCapturePcm == NULL || sMeta == NULL) {
        PcmDumpLogf("[pcm-dump] disabled: %lu B allocation failed (free was %lu)",
                   (unsigned long)(bufBytes + metaBytes), freeBefore);
        free(sCapturePcm);
        free(sMeta);
        sCapturePcm = NULL;
        sMeta = NULL;
        return;
    }

    sSecondsChosen = (uint32_t)chosen;
    sCaptureCapacityFrames = (uint32_t)chosen * PCMDUMP_SAMPLE_RATE;
    sMetaCapacity = (uint32_t)metaRows;
    LightLock_Init(&sLock);
    sWriterThread = threadCreate(PcmDumpWriterMain, NULL, PCMDUMP_WRITER_STACK, PCMDUMP_WRITER_PRIO,
                                 -1 /* no core affinity: let the OS place it */, false);
    if (sWriterThread == NULL) {
        PcmDumpLogf("[pcm-dump] disabled: writer thread create failed");
        free(sCapturePcm);
        free(sMeta);
        sCapturePcm = NULL;
        sMeta = NULL;
        return;
    }
    sArmed = 1;
    unsigned long freeAfter = PcmDumpFreeHeapBytes();
    PcmDumpLogf("[pcm-dump] armed: pcm_dump_s=%d chosen=%d s (%lu B buffer + %lu B meta, RAM-only, "
               "no SD I/O until the window ends), free_heap_before=%lu free_heap_after=%lu, "
               "starts at race-active",
               seconds, chosen, (unsigned long)bufBytes, (unsigned long)metaBytes, freeBefore,
               freeAfter);
}

void gdx_pcmdump_submit(const int16_t* pcm, uint32_t frames, uint32_t chunkIndex, uint32_t tMs) {
    if (!sArmed || frames == 0u || pcm == NULL) {
        return;
    }
    if (!sStartRequested) {
        if (gGdxRaceActive == 0) {
            return;
        }
        sStartRequested = 1; /* flag only -- no I/O on this (drain) thread */
    }
    if (frames > PCMDUMP_CHUNK_FRAMES) {
        return; /* a chunk size this module was not sized for: drop rather than overrun the guard */
    }
    LightLock_Lock(&sLock);
    if (sCaptureFull) {
        LightLock_Unlock(&sLock);
        return; /* window's RAM buffer is full; held until a flush trigger fires, not dropped */
    }
    uint32_t freeFrames = sCaptureCapacityFrames - sCaptureWritePos;
    if (frames > freeFrames) {
        /* Belt-and-suspenders only (see file header): should be unreachable, since submit()
         * always clamps to capacity below. Account for the lost span as silence instead of
         * disappearing it, same convention as round 5. */
        sDroppedChunks++;
        sPendingDropFrames += frames;
        sCaptureFull = 1;
        LightLock_Unlock(&sLock);
        return;
    }
    if (sPendingDropFrames > 0u && sMetaCount < sMetaCapacity) {
        sMeta[sMetaCount].chunkIndex = PCMDUMP_SILENCE_CHUNK_INDEX;
        sMeta[sMetaCount].tMs = tMs;
        sMeta[sMetaCount].frames = sPendingDropFrames;
        sMetaCount++;
        sPendingDropFrames = 0u;
    }
    memcpy(&sCapturePcm[(size_t)sCaptureWritePos * PCMDUMP_CHANNELS], pcm,
           (size_t)frames * PCMDUMP_BYTES_PER_FRAME);
    sCaptureWritePos += frames;
    if (sMetaCount < sMetaCapacity) {
        sMeta[sMetaCount].chunkIndex = chunkIndex;
        sMeta[sMetaCount].tMs = tMs;
        sMeta[sMetaCount].frames = frames;
        sMetaCount++;
    } else {
        sDroppedChunks++; /* meta table exhausted -- sized with headroom, should not happen */
    }
    if (sCaptureWritePos >= sCaptureCapacityFrames) {
        sCaptureFull = 1;
    }
    LightLock_Unlock(&sLock);
}

/* Any thread. HOME-menu pause: called from gdx3ds_audio_suspend() (gdx3ds_audio_ndsp.c), which
 * runs on the main thread inside the APTHOOK_ONSUSPEND transition. Just raises a flag -- the
 * actual (possibly multi-MB) flush happens on this module's own low-priority writer thread, so
 * a HOME press is never delayed by it. */
void gdx_pcmdump_notify_suspend(void) {
    if (!sArmed) {
        return;
    }
    sSuspendNotified = 1;
    __dmb();
}

void gdx_pcmdump_shutdown(void) {
    if (!sArmed) {
        return;
    }
    sStopRequested = 1;
    __dmb();
    if (sWriterThread != NULL) {
        if (R_FAILED(threadJoin(sWriterThread, PCMDUMP_WRITER_JOIN_NS))) {
            PcmDumpLogf("[pcm-dump] writer join TIMEOUT (%lld ns) -- leaking the thread",
                       (long long)PCMDUMP_WRITER_JOIN_NS);
        } else {
            threadFree(sWriterThread);
        }
        sWriterThread = NULL;
    }
    free(sCapturePcm);
    free(sMeta);
    sCapturePcm = NULL;
    sMeta = NULL;
    sArmed = 0;
}

void gdx_pcmdump_progress(uint32_t* capturedFrames, uint32_t* targetFrames, uint32_t* dropped) {
    if (capturedFrames != NULL) {
        *capturedFrames = sDone ? sCapturedWrittenFrames : sCaptureWritePos;
    }
    if (targetFrames != NULL) {
        *targetFrames = sCaptureCapacityFrames;
    }
    if (dropped != NULL) {
        *dropped = sDroppedChunks;
    }
}
