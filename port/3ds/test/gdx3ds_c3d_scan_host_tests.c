/* port/3ds/test/gdx3ds_c3d_scan_host_tests.c -- c5submit: the citro3d diet's dirty-flag scan
 * (port/3ds/vendor/citro3d/gdx/c3d_gdx_scan.h) against stock citro3d 1.7.1's byte loop.
 *
 * C3DGDX_DIET_UNIFSCAN is byte-identical only if C3Dgdx_NextDirty / C3Dgdx_RunEnd find exactly
 * the runs stock C3D_UpdateUniforms finds. This drives both over the same 96-flag arrays --
 * every single flag, every run of every length at every offset, and 200,000 random arrays
 * (sparse, dense, and arbitrary non-0/1 bytes) -- and compares the (start, end) run lists, then
 * the flag arrays both leave behind. The in-game verify (`[native] c3d_diet_verify`) proves the
 * same on real draws; this pins the helper on patterns a replay may never produce. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "c3d_gdx_scan.h"

#define FLAGS 96
#define MAX_RUNS 64

typedef struct {
    int count;
    int start[MAX_RUNS];
    int end[MAX_RUNS];
} RunList;

static unsigned long sChecks = 0;
static unsigned long sFailures = 0;

/* Stock C3D_UpdateUniforms' float loop (uniforms.c), recording runs instead of emitting. */
static void StockRuns(uint8_t* dirty, RunList* out) {
    int i = 0;
    out->count = 0;
    while (i < FLAGS) {
        if (!dirty[i]) {
            i++;
            continue;
        }
        int j;
        for (j = i; j < FLAGS && dirty[j]; j++) {
        }
        out->start[out->count] = i;
        out->end[out->count] = j;
        out->count++;
        for (int k = i; k < j; k++) {
            dirty[k] = 0;
        }
        i = j;
    }
}

/* The diet's loop (uniforms.c C3Di_GdxUpdateUniforms). */
static void DietRuns(uint8_t* dirty, RunList* out) {
    out->count = 0;
    int i = C3Dgdx_NextDirty(dirty, 0, FLAGS);
    while (i < FLAGS) {
        int j = C3Dgdx_RunEnd(dirty, i, FLAGS);
        out->start[out->count] = i;
        out->end[out->count] = j;
        out->count++;
        memset(&dirty[i], 0, (size_t)(j - i));
        i = C3Dgdx_NextDirty(dirty, j, FLAGS);
    }
}

static void Check(const uint8_t* pattern, const char* what) {
    _Alignas(4) uint8_t a[FLAGS];
    _Alignas(4) uint8_t b[FLAGS];
    RunList ra;
    RunList rb;
    memcpy(a, pattern, FLAGS);
    memcpy(b, pattern, FLAGS);
    StockRuns(a, &ra);
    DietRuns(b, &rb);
    sChecks++;
    int same = ra.count == rb.count && memcmp(a, b, FLAGS) == 0;
    for (int r = 0; same && r < ra.count; r++) {
        same = ra.start[r] == rb.start[r] && ra.end[r] == rb.end[r];
    }
    if (!same) {
        sFailures++;
        if (sFailures <= 5) {
            printf("FAIL %s: stock %d runs, diet %d runs\n", what, ra.count, rb.count);
        }
    }
}

static uint32_t sRng = 0x12345678u;

static uint32_t NextRandom(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static void CheckEveryRun(void) {
    uint8_t p[FLAGS];
    for (int start = 0; start < FLAGS; start++) {
        for (int end = start + 1; end <= FLAGS; end++) {
            memset(p, 0, sizeof(p));
            memset(&p[start], 1, (size_t)(end - start));
            Check(p, "single run");
        }
    }
}

static void CheckRandom(int n, uint32_t densityPct, int anyByte) {
    uint8_t p[FLAGS];
    for (int t = 0; t < n; t++) {
        for (int i = 0; i < FLAGS; i++) {
            const int on = (NextRandom() % 100u) < densityPct;
            p[i] = on ? (anyByte ? (uint8_t)(1u + NextRandom() % 255u) : 1u) : 0u;
        }
        Check(p, anyByte ? "random bytes" : "random flags");
    }
}

static void CheckLaneHelpers(void) {
    /* Every byte value in every lane: non-zero lanes are exactly the non-zero bytes. */
    for (int lane = 0; lane < 4; lane++) {
        for (uint32_t v = 0; v < 256u; v++) {
            const uint32_t w = v << (lane * 8);
            const uint32_t expect = v ? (0x80u << (lane * 8)) : 0u;
            sChecks++;
            if (C3Dgdx_NonZeroLanes(w) != expect) {
                sFailures++;
                printf("FAIL lanes lane=%d v=%u\n", lane, v);
            }
        }
    }
}

int main(void) {
    uint8_t p[FLAGS];
    memset(p, 0, sizeof(p));
    Check(p, "all clean");
    memset(p, 1, sizeof(p));
    Check(p, "all dirty");
    CheckEveryRun();
    CheckLaneHelpers();
    CheckRandom(50000, 5, 0);
    CheckRandom(50000, 50, 0);
    CheckRandom(50000, 95, 0);
    CheckRandom(50000, 50, 1);
    if (sFailures) {
        printf("c3d_scan_tests: %lu checks, %lu failures\n", sChecks, sFailures);
        return 1;
    }
    printf("c3d_scan_tests: %lu checks, 0 failures\n", sChecks);
    return 0;
}
