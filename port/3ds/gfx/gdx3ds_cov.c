/* port/3ds/gfx/gdx3ds_cov.c — COVERAGE census. Contract in port/3ds/include/gdx3ds_cov.h. */
#include "gdx3ds_cov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gdx3ds_cov_on = 0;

/* The log sink is INJECTED (gdx3ds_cov_arm) rather than linked: this TU lives in gdx3ds_gfx,
 * which the standalone gdx3ds-dl-tests harness and tools/ci-3ds.sh's host build of the owned
 * interpreter both link, and neither has an ini or an SD log. Nothing to stub, nothing weak,
 * and the census simply never arms where nobody arms it. */
static void (*sSink)(const char* line) = NULL;

/* Capacities. Sized from the corpus: the busiest frame measured is the 30-machine GP pack
 * (30 slots x up to 5 LODs = 150 MACH keys) plus ~60 CC, ~200 TEX and the small families, so
 * 1024 per frame has ~3x headroom. Fixed arrays: this runs on the render thread and must never
 * touch the heap (PROTOCOL: the operator-new failure path). ~100 KiB of .bss, diagnostic-only. */
#define COV_FRAME_CAP 1024u
#define COV_DICT_CAP 8192u
#define COV_DICT_SLOTS 16384u /* power of two, >= 2x COV_DICT_CAP: open addressing */
#define COV_SEEN_SLOTS 4096u  /* power of two, >= 2x COV_FRAME_CAP */

static int sVerbose = 0;
static int sFinished = 0;

/* Per-frame membership: an open-addressed set stamped with the frame ordinal, so a new frame
 * costs no clear. A slot is free when sSeenStamp != sStamp.
 *
 * sStamp starts at 1, NOT 0, and never takes the value 0 again: a zeroed sSeenStamp with
 * sStamp == 0 makes every slot look OCCUPIED, and the probe loop below -- which is only
 * bounded by finding a free slot -- spins forever. That cost the first smoke run: the render
 * thread wedged at frame 2 and the log showed `[rt] STALL: frame waiting done=1 target=2`
 * with `[cov] init` as the only [cov] line. */
static uint32_t sSeenKey[COV_SEEN_SLOTS];
static uint32_t sSeenStamp[COV_SEEN_SLOTS];
static uint32_t sStamp = 1;

static uint32_t sCur[COV_FRAME_CAP];
static uint32_t sCurN = 0;
static uint32_t sPrev[COV_FRAME_CAP];
static uint32_t sPrevN = 0;

/* Run dictionary: every key ever seen, with its first/last frame and how many frames carried it. */
static struct {
    uint32_t key;
    uint32_t first;
    uint32_t last;
    uint32_t frames;
} sDict[COV_DICT_CAP];
static uint32_t sDictN = 0;
static uint32_t sDictSlot[COV_DICT_SLOTS]; /* index+1 into sDict, 0 = empty */

static unsigned long sMarks = 0;
static unsigned long sDropFrame = 0; /* marks lost to COV_FRAME_CAP */
static unsigned long sDropDict = 0;  /* keys lost to COV_DICT_CAP */
static unsigned long sFrames = 0;

static void CovLog(const char* line) {
    if (sSink != NULL) {
        sSink(line);
    }
}

static inline uint32_t CovHash(uint32_t k) {
    /* 32-bit finalizer (murmur3 fmix): the keys are dense small integers in several families,
     * and the low bits alone collide badly under a power-of-two mask. */
    k ^= k >> 16;
    k *= 0x85EBCA6Bu;
    k ^= k >> 13;
    k *= 0xC2B2AE35u;
    k ^= k >> 16;
    return k;
}

void gdx3ds_cov_arm(int on, int verbose, void (*sink)(const char* line)) {
    char line[128];
    sSink = sink;
    gdx3ds_cov_on = on ? 1 : 0;
    sVerbose = verbose ? 1 : 0;
    if (!gdx3ds_cov_on) {
        return;
    }
    memset(sSeenStamp, 0, sizeof(sSeenStamp));
    memset(sDictSlot, 0, sizeof(sDictSlot));
    sStamp = 1;
    sDictN = 0;
    sCurN = 0;
    sPrevN = 0;
    snprintf(line, sizeof(line), "[cov] init keys=%u frame_cap=%u verbose=%d", (unsigned)COV_DICT_CAP,
             (unsigned)COV_FRAME_CAP, sVerbose);
    CovLog(line);
}

/* Dictionary insert/lookup. Returns 1 when the key is new to the run. */
static int DictTouch(uint32_t key, uint32_t frame) {
    uint32_t slot = CovHash(key) & (COV_DICT_SLOTS - 1u);
    for (;;) {
        const uint32_t idx = sDictSlot[slot];
        if (idx == 0) {
            if (sDictN >= COV_DICT_CAP) {
                sDropDict++;
                return 0;
            }
            sDict[sDictN].key = key;
            sDict[sDictN].first = frame;
            sDict[sDictN].last = frame;
            sDict[sDictN].frames = 1;
            sDictN++;
            sDictSlot[slot] = sDictN; /* index + 1 */
            return 1;
        }
        if (sDict[idx - 1].key == key) {
            sDict[idx - 1].last = frame;
            sDict[idx - 1].frames++;
            return 0;
        }
        slot = (slot + 1u) & (COV_DICT_SLOTS - 1u);
    }
}

int gdx3ds_cov_mark_slow(uint32_t key) {
    uint32_t slot = CovHash(key) & (COV_SEEN_SLOTS - 1u);
    uint32_t probes = COV_SEEN_SLOTS;
    sMarks++;
    while (probes-- != 0) {
        if (sSeenStamp[slot] != sStamp) {
            /* free slot: claim it for this frame */
            if (sCurN >= COV_FRAME_CAP) {
                sDropFrame++;
                return 0;
            }
            sSeenStamp[slot] = sStamp;
            sSeenKey[slot] = key;
            sCur[sCurN++] = key;
            /* `new to the run` is answered by the dictionary, which is only updated at frame
             * end (a key present in 4000 frames must not cost 4000 dictionary probes). The
             * first-sighting answer is therefore "is this key absent from the dictionary". */
            {
                uint32_t p = CovHash(key) & (COV_DICT_SLOTS - 1u);
                for (;;) {
                    const uint32_t idx = sDictSlot[p];
                    if (idx == 0) {
                        return 1;
                    }
                    if (sDict[idx - 1].key == key) {
                        return 0;
                    }
                    p = (p + 1u) & (COV_DICT_SLOTS - 1u);
                }
            }
        }
        if (sSeenKey[slot] == key) {
            return 0; /* already counted this frame */
        }
        slot = (slot + 1u) & (COV_SEEN_SLOTS - 1u);
    }
    /* Unreachable while COV_SEEN_SLOTS > COV_FRAME_CAP, which is a compile-time fact. Kept
     * because this runs on the render thread and a spin here is a hung run, not a bad number. */
    sDropFrame++;
    return 0;
}

static int CmpU32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Appends "<fam>:<val>" to `buf`; flushes and restarts the line when it would overflow. */
typedef struct {
    char buf[220];
    size_t n;
    char head[64];
    int any;
} CovLine;

static void LineStart(CovLine* l, const char* head) {
    snprintf(l->head, sizeof(l->head), "%s", head);
    l->n = (size_t)snprintf(l->buf, sizeof(l->buf), "%s", head);
    l->any = 0;
}

static void LineFlush(CovLine* l) {
    if (l->any) {
        CovLog(l->buf);
    }
    l->n = (size_t)snprintf(l->buf, sizeof(l->buf), "%s", l->head);
    l->any = 0;
}

static void LineAdd(CovLine* l, char sign, uint32_t key) {
    char tok[20];
    const int len = snprintf(tok, sizeof(tok), " %c%02x:%06x", sign, (unsigned)(key >> 24),
                             (unsigned)(key & 0x00FFFFFFu));
    if (l->n + (size_t)len + 1u >= sizeof(l->buf)) {
        LineFlush(l);
    }
    memcpy(l->buf + l->n, tok, (size_t)len + 1u);
    l->n += (size_t)len;
    l->any = 1;
}

void gdx3ds_cov_frame(unsigned long frame, unsigned long tick, int mode) {
    CovLine line;
    char head[80];
    uint32_t i, j;
    if (!gdx3ds_cov_on) {
        return;
    }
    gdx3ds_cov_mark_slow(GDX3DS_COV_KEY(GDX3DS_COV_FAM_MODE, (uint32_t)(mode & 0xFF)));
    qsort(sCur, sCurN, sizeof(sCur[0]), CmpU32);
    snprintf(head, sizeof(head), "[cov] f=%lu t=%lu m=%d", frame, tick, mode);
    LineStart(&line, head);
    /* Merge-diff the two sorted key lists. */
    i = 0;
    j = 0;
    while (i < sCurN || j < sPrevN) {
        if (j >= sPrevN || (i < sCurN && sCur[i] < sPrev[j])) {
            LineAdd(&line, '+', sCur[i]);
            i++;
        } else if (i >= sCurN || sPrev[j] < sCur[i]) {
            LineAdd(&line, '-', sPrev[j]);
            j++;
        } else {
            i++;
            j++;
        }
    }
    LineFlush(&line);
    for (i = 0; i < sCurN; i++) {
        DictTouch(sCur[i], (uint32_t)frame);
    }
    memcpy(sPrev, sCur, sCurN * sizeof(sCur[0]));
    sPrevN = sCurN;
    sCurN = 0;
    sStamp++;
    if (sStamp == 0) { /* wrapped: the stamp trick needs a real clear exactly once per 4G frames */
        memset(sSeenStamp, 0, sizeof(sSeenStamp));
        sStamp = 1;
    }
    sFrames++;
}

void gdx3ds_cov_finish(void) {
    char line[128];
    uint32_t i;
    if (!gdx3ds_cov_on || sFinished) {
        return;
    }
    sFinished = 1;
    for (i = 0; i < sDictN; i++) {
        snprintf(line, sizeof(line), "[cov] sum %02x:%06x first=%lu last=%lu n=%lu",
                 (unsigned)(sDict[i].key >> 24), (unsigned)(sDict[i].key & 0x00FFFFFFu),
                 (unsigned long)sDict[i].first, (unsigned long)sDict[i].last,
                 (unsigned long)sDict[i].frames);
        CovLog(line);
    }
    snprintf(line, sizeof(line), "[cov] done keys=%lu frames=%lu marks=%lu dropframe=%lu dropdict=%lu",
             (unsigned long)sDictN, (unsigned long)sFrames, sMarks, sDropFrame, sDropDict);
    CovLog(line);
}
