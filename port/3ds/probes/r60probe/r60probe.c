/* port/3ds/probes/r60probe/r60probe.c -- PROBE, NOT PART OF THE GAME BUILD (r60, researchers A + F).
 *
 * Why is the Old 3DS ARM11 so much slower than the emulator on the render path, and what would
 * claw it back? Every test prints one `[r60] ...` line. Sections A..H are CPU-side (researcher A):
 * the draw tests write into a scratch command buffer and REWIND it afterwards, so they time
 * exactly the CPU work of building commands (what the game pays), nothing else. Sections
 * FLUSH..T8 (researcher F) render real frames on the top screen and run LAST (riskier, longer).
 *
 *   A  C3D per-draw cost vs a raw PICA writer (byte-checked against C3D's own output), the
 *      C3D parts (C3Di_UpdateContext clean, C3D_UpdateUniforms clean, one GPUCMD_Add), and the
 *      replay of a prebuilt per-draw command block (memcpy + patch)
 *   C  memory: pointer-chase latency (one load per 32-byte line, random order) over working
 *      sets 4 KB .. 4 MB in heap / linear / VRAM; read/write/copy bandwidth, with and without PLD
 *   D  I-cache: straight-line code of 4 .. 64 KB, cycles per instruction
 *   E  VFP11 op latency (dependent chains), throughput (4 chains), f64, div/sqrt, int<->float;
 *      4x4 multiply scalar vs citro3d vs VFP short-vector mode (LEN=4); vertex transform
 *      float vs ARMv6 SMUAD/SMLAD fixed point
 *   F  svcGetSystemTick, LightLock, svc mutex, GSPGPU_FlushDataCache vs svcFlushProcessDataCache
 *   L  hwprof: whole-linear-heap flush vs the heap working set (eviction), linear read-back after
 *      a flush, small memcpy heap->linear vs heap->heap (warm/cold dst), integer division
 *      (EABI helpers), float<->double conversion and f64 latency
 *   X  (opt-in at the end: Y, then X) svcGetThreadContext on retail, and a user-mode CCNT read
 *      that crashes if the perfmon is privileged-only -- the two questions a PC sampler depends on
 *   P  CP15 performance counters (svcControlPerformanceCounter) over A/C/D/E workloads:
 *      I-cache misses, instructions (CPI), D-cache read/write misses, stall causes, TLB, branches
 *   H  core 1: APT_SetAppCpuTimeLimit 30/50/80 -> measured share, and GSP latency seen from core 0
 *   R  render setup for the rest (DrawProbe's machine shader, 8 solid 8x8 textures, a 16-tri disc,
 *      400x240 RGBA8 target on the top screen, framebuffer CRC via display-transfer readback)
 *   FLUSH   C3D_FrameEnd(0) vs C3D_FrameEnd(GX_CMDLIST_FLUSH) CPU cost; whole-linear-heap
 *           GSPGPU_FlushDataCache alone (clean, and after dirtying 256 KB); 64 KiB range flush
 *   RESYNC  (B-gpu 8) C3D draw, 50 raw draws (raw uniforms + raw draw packet), then the force-dirty
 *           calls + one C3D draw with a new texture/texenv: us of the resync, CRC vs all-C3D
 *   T7      (D-radical 2.7) frame A = 60 draws with per-draw uniforms + 8 texture binds via C3D;
 *           frame B (new matrices) (a) re-issued from a record array via C3D, (b) memcpy of A's
 *           command span + patch of the modelView words at parsed offsets; CRC + word compare
 *           against a directly drawn frame with the same matrices
 *   T8      (D-radical 2.7) cadence: alternate A (22 ms spin + 60 draws) and B (the T7 cmdbuf
 *           replay) for 600 presents per mode (syncdraw / free / paced); per present the vblank
 *           it becomes visible on (PPF callback + C3D_FrameCounter), missed/duplicated vblanks
 *
 * Timing: svcGetSystemTick = the 268,111,856 Hz ARM11 clock on the Old 3DS, i.e. CPU cycles.
 * Output: bottom-screen console + sdmc:/3ds/r60probe/r60probe.txt (appended). START exits at
 * the end (START during T8 skips the rest of T8). HOME is refused for the whole run (it freezes
 * the Old 3DS, NEXT.md C4j). A hang shows as the log ending after the previous test's line. */
#include <3ds.h>
#include <citro3d.h>

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "r60probe_shader_shbin.h"

#ifndef R60PROBE_BUILD_ID
#define R60PROBE_BUILD_ID "unknown"
#endif

/* citro3d internal, exported from libcitro3d.a (the per-draw dirty-state walk). */
extern void C3Di_UpdateContext(void);

#define CMDBUF_BYTES 0x80000
#define DRAWS 300
#define DRAW_REPS 20
#define MESH_VERTS 64
#define MESH_INDICES 192
#define UNIF_MODELVIEW 0
#define UNIF_LIVERY 4
#define UNIF_VEC4S 7
#define RAW_DRAW_WORDS 30
#define RAW_UNIF_WORDS (4 + UNIF_VEC4S * 4)
#define BLOCK_WORDS (RAW_UNIF_WORDS + RAW_DRAW_WORDS)
#define CHASE_MAX_BYTES (4u << 20)
#define CHASE_STEPS 200000
#define BW_BYTES (1u << 20)
#define BW_REPS 8
#define XF_VERTS 1024
#define MTX_REPS 1000
#define SPIN_MS 500

/* ---- log ------------------------------------------------------------------------------ */

static FILE* sLog;

static void logLine(const char* fmt, ...) {
    char line[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    printf("%s\n", line);
    if (sLog != NULL) {
        fprintf(sLog, "%s\n", line);
        fflush(sLog);
    }
}

static void openLog(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/r60probe", 0777);
    sLog = fopen("sdmc:/3ds/r60probe/r60probe.txt", "a");
}

static inline u64 nowTicks(void) {
    return svcGetSystemTick();
}

static double ticksToUs(u64 ticks) {
    return (double)ticks * 1000000.0 / (double)SYSCLOCK_ARM11;
}

static double perUnit(u64 ticks, double units) {
    return units > 0.0 ? (double)ticks / units : 0.0;
}

static u32 sLcg = 0x12345678u;

static u32 nextRandom(void) {
    sLcg = sLcg * 1664525u + 1013904223u;
    return sLcg >> 8;
}

static volatile u32 sSink;

/* ---- command-buffer discard ------------------------------------------------------------ */

static u32 cmdMark(void) {
    return gpuCmdBufOffset;
}

static void cmdRewind(u32 mark) {
    gpuCmdBufOffset = mark;
}

/* ---- A: draw submission ------------------------------------------------------------------ */

typedef struct {
    float pos[3];
    float nrm[3];
    float uv[2];
} VertexF;

static VertexF* sVbo;
static u16* sIbo;
static C3D_BufInfo sBuf;
static C3D_AttrInfo sAttr;
static u32 sIdxOffset;
static C3D_Mtx sDrawMtx[DRAWS];
static C3D_FVec sLivery[DRAWS][3];
static u32 sBlock[BLOCK_WORDS];

static bool buildMesh(void) {
    sVbo = (VertexF*)linearAlloc(MESH_VERTS * sizeof(VertexF));
    sIbo = (u16*)linearAlloc(MESH_INDICES * sizeof(u16));
    if (sVbo == NULL || sIbo == NULL) {
        return false;
    }
    for (int i = 0; i < MESH_VERTS; i++) {
        const float a = (float)i * 0.1f;
        const VertexF v = {{cosf(a), sinf(a), 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 0.5f}};
        sVbo[i] = v;
    }
    for (int i = 0; i < MESH_INDICES; i++) {
        sIbo[i] = (u16)(nextRandom() % MESH_VERTS);
    }
    GSPGPU_FlushDataCache(sVbo, MESH_VERTS * sizeof(VertexF));
    GSPGPU_FlushDataCache(sIbo, MESH_INDICES * sizeof(u16));
    BufInfo_Init(&sBuf);
    BufInfo_Add(&sBuf, sVbo, sizeof(VertexF), 3, 0x210);
    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sAttr, 1, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sAttr, 2, GPU_FLOAT, 2);
    C3D_SetAttrInfo(&sAttr);
    C3D_SetBufInfo(&sBuf);
    sIdxOffset = osConvertVirtToPhys(sIbo) - sBuf.base_paddr;
    for (int i = 0; i < DRAWS; i++) {
        Mtx_Identity(&sDrawMtx[i]);
        Mtx_Translate(&sDrawMtx[i], (float)i, 2.0f, -30.0f, true);
        Mtx_RotateY(&sDrawMtx[i], 0.01f * (float)i, true);
        for (int c = 0; c < 3; c++) {
            sLivery[i][c].x = (float)((i + c) % 7) / 7.0f;
            sLivery[i][c].y = 0.5f;
            sLivery[i][c].z = 0.25f;
            sLivery[i][c].w = 1.0f;
        }
    }
    return true;
}

static void c3dDraw(void) {
    C3D_DrawElements(GPU_TRIANGLES, MESH_INDICES, C3D_UNSIGNED_SHORT, sIbo);
}

static void c3dSetUniforms(int i) {
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, UNIF_MODELVIEW, &sDrawMtx[i]);
    for (int c = 0; c < 3; c++) {
        const C3D_FVec* l = &sLivery[i][c];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, UNIF_LIVERY + c, l->x, l->y, l->z, l->w);
    }
}

/* Exactly what C3D_DrawElements(GPU_TRIANGLES, ..., C3D_UNSIGNED_SHORT) emits when nothing is
 * dirty (libcitro3d 1.7.1 disassembly), as 15 (param, header) pairs. */
#define RAW_PAIR(w, n, reg, mask, val)                     \
    do {                                                   \
        (w)[(n)++] = (val);                                \
        (w)[(n)++] = GPUCMD_HEADER(0, (mask), (reg));      \
    } while (0)

static inline u32 rawDraw(u32* w, u32 idxOffset, u32 count) {
    u32 n = 0;
    RAW_PAIR(w, n, GPUREG_PRIMITIVE_CONFIG, 0x2, GPU_GEOMETRY_PRIM);
    RAW_PAIR(w, n, GPUREG_RESTART_PRIMITIVE, 0xF, 1);
    RAW_PAIR(w, n, GPUREG_INDEXBUFFER_CONFIG, 0xF, idxOffset | (1u << 31));
    RAW_PAIR(w, n, GPUREG_NUMVERTICES, 0xF, count);
    RAW_PAIR(w, n, GPUREG_VERTEX_OFFSET, 0xF, 0);
    RAW_PAIR(w, n, GPUREG_GEOSTAGE_CONFIG, 0x2, 0x100);
    RAW_PAIR(w, n, GPUREG_GEOSTAGE_CONFIG2, 0x2, 0x100);
    RAW_PAIR(w, n, GPUREG_START_DRAW_FUNC0, 0x1, 0);
    RAW_PAIR(w, n, GPUREG_DRAWELEMENTS, 0xF, 1);
    RAW_PAIR(w, n, GPUREG_START_DRAW_FUNC0, 0x1, 1);
    RAW_PAIR(w, n, GPUREG_GEOSTAGE_CONFIG, 0x2, 0);
    RAW_PAIR(w, n, GPUREG_GEOSTAGE_CONFIG2, 0x2, 0);
    RAW_PAIR(w, n, GPUREG_VTX_FUNC, 0xF, 1);
    RAW_PAIR(w, n, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    RAW_PAIR(w, n, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    return n;
}

/* One contiguous float-uniform upload, the way C3D_UpdateUniforms packs a dirty run:
 * CONFIG write, then DATA with (count*4 - 1) extra words, padded to an even length. */
static inline u32 rawUniforms(u32* w, u32 loc, const C3D_FVec* v, u32 count) {
    const u32* src = (const u32*)v;
    const u32 words = count * 4;
    w[0] = 0x80000000u | loc;
    w[1] = GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_CONFIG);
    w[2] = src[0];
    w[3] = GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA) | ((words - 1) << 20);
    memcpy(&w[4], &src[1], (words - 1) * 4);
    w[3 + words] = 0;
    return 4 + words;
}

static inline u32* cmdReserve(u32 words) {
    if (gpuCmdBufOffset + words > gpuCmdBufSize) {
        return NULL;
    }
    u32* w = gpuCmdBuf + gpuCmdBufOffset;
    gpuCmdBufOffset += words;
    return w;
}

static void rawEmitDraw(void) {
    u32* w = cmdReserve(RAW_DRAW_WORDS);
    if (w != NULL) {
        rawDraw(w, sIdxOffset, MESH_INDICES);
    }
}

static void rawEmitUniforms(int i) {
    C3D_FVec block[UNIF_VEC4S];
    memcpy(&block[0], sDrawMtx[i].r, 4 * sizeof(C3D_FVec));
    memcpy(&block[4], sLivery[i], 3 * sizeof(C3D_FVec));
    u32* w = cmdReserve(RAW_UNIF_WORDS);
    if (w != NULL) {
        rawUniforms(w, UNIF_MODELVIEW, block, UNIF_VEC4S);
    }
}

static void buildBlock(void) {
    C3D_FVec zero[UNIF_VEC4S];
    memset(zero, 0, sizeof(zero));
    rawUniforms(sBlock, UNIF_MODELVIEW, zero, UNIF_VEC4S);
    rawDraw(&sBlock[RAW_UNIF_WORDS], sIdxOffset, MESH_INDICES);
}

/* Replay: copy the prebuilt block and patch the 28 uniform words (param 0 lives at [2]). */
static void replayBlock(int i) {
    u32* w = cmdReserve(BLOCK_WORDS);
    if (w == NULL) {
        return;
    }
    memcpy(w, sBlock, sizeof(sBlock));
    const u32* m = (const u32*)sDrawMtx[i].r;
    const u32* l = (const u32*)sLivery[i];
    w[2] = m[0];
    memcpy(&w[4], &m[1], 15 * 4);
    memcpy(&w[19], l, 12 * 4);
}

typedef void (*DrawLoopFn)(void);

static void loopC3dDraw(void) {
    for (int i = 0; i < DRAWS; i++) {
        c3dDraw();
    }
}

static void loopC3dDrawUnif(void) {
    for (int i = 0; i < DRAWS; i++) {
        c3dSetUniforms(i);
        c3dDraw();
    }
}

static void loopRawDraw(void) {
    for (int i = 0; i < DRAWS; i++) {
        rawEmitDraw();
    }
}

static void loopRawDrawUnif(void) {
    for (int i = 0; i < DRAWS; i++) {
        rawEmitUniforms(i);
        rawEmitDraw();
    }
}

static void loopReplay(void) {
    for (int i = 0; i < DRAWS; i++) {
        replayBlock(i);
    }
}

/* Runs a draw loop DRAW_REPS times, discarding its commands; returns cycles per draw. */
static double timeDrawLoop(DrawLoopFn fn) {
    u64 total = 0;
    for (int r = 0; r < DRAW_REPS; r++) {
        const u32 mark = cmdMark();
        const u64 t0 = nowTicks();
        fn();
        total += nowTicks() - t0;
        cmdRewind(mark);
    }
    return perUnit(total, (double)DRAW_REPS * DRAWS);
}

/* Byte-check: C3D's words for one clean draw (and one uniform block) vs the raw writer's. */
static bool checkRawDraw(void) {
    const u32 mark = cmdMark();
    c3dDraw();
    const u32 c3dWords = cmdMark() - mark;
    u32 raw[RAW_DRAW_WORDS];
    rawDraw(raw, sIdxOffset, MESH_INDICES);
    const bool same = c3dWords == RAW_DRAW_WORDS && memcmp(gpuCmdBuf + mark, raw, sizeof(raw)) == 0;
    cmdRewind(mark);
    return same;
}

static bool checkRawUniforms(void) {
    const u32 mark = cmdMark();
    c3dSetUniforms(5);
    C3D_UpdateUniforms(GPU_VERTEX_SHADER);
    const u32 c3dWords = cmdMark() - mark;
    u32 raw[RAW_UNIF_WORDS];
    C3D_FVec block[UNIF_VEC4S];
    memcpy(&block[0], sDrawMtx[5].r, 4 * sizeof(C3D_FVec));
    memcpy(&block[4], sLivery[5], 3 * sizeof(C3D_FVec));
    rawUniforms(raw, UNIF_MODELVIEW, block, UNIF_VEC4S);
    const bool same = c3dWords == RAW_UNIF_WORDS && memcmp(gpuCmdBuf + mark, raw, sizeof(raw)) == 0;
    cmdRewind(mark);
    return same;
}

static double timeC3dParts(int which) {
    const int n = 10000;
    const u32 mark = cmdMark();
    const u64 t0 = nowTicks();
    for (int i = 0; i < n; i++) {
        if (which == 0) {
            C3Di_UpdateContext();
        } else if (which == 1) {
            C3D_UpdateUniforms(GPU_VERTEX_SHADER);
        } else if (which == 2) {
            GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
        } else {
            sSink += osConvertVirtToPhys(sIbo);
        }
        if ((i & 255) == 255) {
            cmdRewind(mark);
        }
    }
    const u64 dt = nowTicks() - t0;
    cmdRewind(mark);
    return perUnit(dt, n);
}

static void testDraws(void) {
    const u32 mark = cmdMark();
    c3dDraw(); /* flush every dirty flag C3D_Init left, then discard */
    cmdRewind(mark);
    buildBlock();
    const bool drawOk = checkRawDraw();
    const bool unifOk = checkRawUniforms();
    const double c3d = timeDrawLoop(loopC3dDraw);
    const double c3dU = timeDrawLoop(loopC3dDrawUnif);
    const double raw = timeDrawLoop(loopRawDraw);
    const double rawU = timeDrawLoop(loopRawDrawUnif);
    const double rep = timeDrawLoop(loopReplay);
    logLine("[r60] A draw cyc/draw: c3d=%.0f c3d+unif=%.0f raw=%.0f raw+unif=%.0f replay+patch=%.0f "
            "(us: %.2f %.2f %.2f %.2f %.2f) match draw=%d unif=%d",
            c3d, c3dU, raw, rawU, rep, c3d / 268.1, c3dU / 268.1, raw / 268.1, rawU / 268.1,
            rep / 268.1, drawOk ? 1 : 0, unifOk ? 1 : 0);
    logLine("[r60] A c3d parts cyc: UpdateContext_clean=%.0f UpdateUniforms_vs_clean=%.0f "
            "GPUCMD_AddWrite=%.0f osConvertVirtToPhys=%.0f",
            timeC3dParts(0), timeC3dParts(1), timeC3dParts(2), timeC3dParts(3));
}

/* ---- C: memory ---------------------------------------------------------------------------- */

static u32* sHeapMem;
static u32* sLinearMem;
static u32* sVramMem;

/* One node per 32-byte line, visited in one random cycle. */
static void buildChase(u32* mem, u32 bytes) {
    const u32 nodes = bytes / 32;
    u32* order = (u32*)malloc(nodes * sizeof(u32));
    if (order == NULL) {
        return;
    }
    for (u32 i = 0; i < nodes; i++) {
        order[i] = i;
    }
    for (u32 i = nodes - 1; i > 0; i--) {
        const u32 j = nextRandom() % (i + 1);
        const u32 t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (u32 i = 0; i < nodes; i++) {
        mem[order[i] * 8] = order[(i + 1) % nodes] * 8;
    }
    free(order);
}

static u32 chase(const u32* mem, u32 steps) {
    u32 p = 0;
    for (u32 s = 0; s < steps; s++) {
        p = mem[p];
    }
    return p;
}

static double chaseCycles(u32* mem, u32 bytes) {
    buildChase(mem, bytes);
    sSink += chase(mem, bytes / 32); /* warm: one full lap */
    const u64 t0 = nowTicks();
    sSink += chase(mem, CHASE_STEPS);
    return perUnit(nowTicks() - t0, CHASE_STEPS);
}

static void chaseRow(const char* name, u32* mem, const u32* sizesKb, int count) {
    char line[256];
    int n = snprintf(line, sizeof(line), "[r60] C chase %s cyc/load:", name);
    for (int i = 0; i < count && n < (int)sizeof(line); i++) {
        n += snprintf(line + n, sizeof(line) - n, " %luK=%.1f", (unsigned long)sizesKb[i],
                      chaseCycles(mem, sizesKb[i] * 1024u));
    }
    logLine("%s", line);
}

static u32 readSum(const u32* p, u32 bytes) {
    u32 s = 0;
    for (u32 i = 0; i < bytes / 4; i += 8) {
        s += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return s;
}

static u32 readSumPld(const u32* p, u32 bytes) {
    u32 s = 0;
    for (u32 i = 0; i < bytes / 4; i += 8) {
        __asm__ volatile("pld [%0, #128]" : : "r"(&p[i]));
        s += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    }
    return s;
}

static double mbPerS(u64 ticks, u32 bytes) {
    const double us = ticksToUs(ticks);
    return us > 0.0 ? (double)bytes * BW_REPS / us : 0.0; /* bytes/us == MB/s */
}

static double bwRead(const u32* p, bool pld) {
    const u64 t0 = nowTicks();
    for (int r = 0; r < BW_REPS; r++) {
        sSink += pld ? readSumPld(p, BW_BYTES) : readSum(p, BW_BYTES);
    }
    return mbPerS(nowTicks() - t0, BW_BYTES);
}

static double bwWrite(u32* p) {
    const u64 t0 = nowTicks();
    for (int r = 0; r < BW_REPS; r++) {
        memset(p, r, BW_BYTES);
    }
    return mbPerS(nowTicks() - t0, BW_BYTES);
}

static double bwCopy(u32* dst, const u32* src) {
    const u64 t0 = nowTicks();
    for (int r = 0; r < BW_REPS; r++) {
        memcpy(dst, src, BW_BYTES);
    }
    return mbPerS(nowTicks() - t0, BW_BYTES);
}

static double lineReadCycles(const u32* p, u32 bytes);

static void testMemory(void) {
    static const u32 kHeapKb[] = {4, 8, 16, 32, 64, 128, 256, 1024, 4096};
    static const u32 kLinearKb[] = {16, 64, 1024, 4096};
    static const u32 kVramKb[] = {16, 64, 1024};
    chaseRow("heap", sHeapMem, kHeapKb, (int)(sizeof(kHeapKb) / sizeof(kHeapKb[0])));
    chaseRow("linear", sLinearMem, kLinearKb, (int)(sizeof(kLinearKb) / sizeof(kLinearKb[0])));
    /* VRAM is READ-ONLY for this process: the game's rsf (which the CIA reuses) maps
     * 1f000000-1f5fffff with `:r`. buildChase WROTE its pointer table into VRAM, a permission data
     * abort that killed the first hardware run at this line (hwprof2). Read-only line sweep instead. */
    if (sVramMem != NULL) {
        char line[160];
        int n = snprintf(line, sizeof(line), "[r60] C vram read-only sweep cyc/line:");
        for (size_t i = 0; i < sizeof(kVramKb) / sizeof(kVramKb[0]) && n < (int)sizeof(line); i++) {
            n += snprintf(line + n, sizeof(line) - n, " %luK=%.1f", (unsigned long)kVramKb[i],
                          lineReadCycles(sVramMem, kVramKb[i] * 1024u));
        }
        logLine("%s", line);
    }
    logLine("[r60] C bw MB/s 1MB: heap read=%.0f read+pld=%.0f write=%.0f | linear read=%.0f "
            "read+pld=%.0f write=%.0f | copy heap>heap=%.0f heap>linear=%.0f linear>heap=%.0f",
            bwRead(sHeapMem, false), bwRead(sHeapMem, true), bwWrite(sHeapMem),
            bwRead(sLinearMem, false), bwRead(sLinearMem, true), bwWrite(sLinearMem),
            bwCopy(sHeapMem, sHeapMem + (2u << 18)), bwCopy(sLinearMem, sHeapMem),
            bwCopy(sHeapMem, sLinearMem));
}

/* ---- D: I-cache --------------------------------------------------------------------------- */

#define ICODE(name, insns)                                                         \
    __attribute__((naked, noinline)) static void name(void) {                     \
        __asm__(".rept " #insns "\n\tadd r0, r0, #1\n\t.endr\n\tbx lr\n");         \
    }

ICODE(icode4k, 1023)
ICODE(icode8k, 2047)
ICODE(icode12k, 3071)
ICODE(icode16k, 4095)
ICODE(icode24k, 6143)
ICODE(icode32k, 8191)
ICODE(icode64k, 16383)

typedef struct {
    const char* name;
    void (*fn)(void);
    u32 insns;
} ICodeSpec;

static const ICodeSpec kICode[] = {
    {"4K", icode4k, 1024},   {"8K", icode8k, 2048},   {"12K", icode12k, 3072}, {"16K", icode16k, 4096},
    {"24K", icode24k, 6144}, {"32K", icode32k, 8192}, {"64K", icode64k, 16384},
};

static double icodeCycles(const ICodeSpec* spec) {
    const int calls = 200;
    spec->fn();
    const u64 t0 = nowTicks();
    for (int i = 0; i < calls; i++) {
        spec->fn();
    }
    return perUnit(nowTicks() - t0, (double)calls * spec->insns);
}

static void testICache(void) {
    char line[256];
    int n = snprintf(line, sizeof(line), "[r60] D icache straight-line cyc/insn:");
    for (size_t i = 0; i < sizeof(kICode) / sizeof(kICode[0]); i++) {
        const double cpi = icodeCycles(&kICode[i]);
        n += snprintf(line + n, sizeof(line) - n, " %s=%.2f", kICode[i].name, cpi);
    }
    logLine("%s (miss cost/32B line ~= (cpi-1)*8)", line);
}

/* ---- E: VFP / SIMD ------------------------------------------------------------------------ */

static const float kOnes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
static const double kOnesD[4] = {1.0, 1.0, 1.0, 1.0};

#define VFP_S_CLOBBER "s0", "s1", "s2", "s3", "s4", "s5", "memory"

__attribute__((noinline)) static void vfpMlaDep(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 64\nvmla.f32 s0, s1, s2\n.endr\n" : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpMla4(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 16\nvmla.f32 s0, s1, s2\nvmla.f32 s3, s1, s2\n"
                     "vmla.f32 s4, s1, s2\nvmla.f32 s5, s1, s2\n.endr\n"
                     : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpMulDep(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 64\nvmul.f32 s0, s0, s1\n.endr\n" : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpAddDep(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 64\nvadd.f32 s0, s0, s1\n.endr\n" : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpDivDep(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 64\nvdiv.f32 s0, s0, s1\n.endr\n" : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpSqrtDep(void) {
    __asm__ volatile("vldmia %0, {s0-s5}\n.rept 64\nvsqrt.f32 s0, s0\n.endr\n" : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpMlaDepF64(void) {
    __asm__ volatile("vldmia %0, {d0-d2}\n.rept 64\nvmla.f64 d0, d1, d2\n.endr\n" : : "r"(kOnesD) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpDivDepF64(void) {
    __asm__ volatile("vldmia %0, {d0-d2}\n.rept 64\nvdiv.f64 d0, d0, d1\n.endr\n" : : "r"(kOnesD) : VFP_S_CLOBBER);
}

/* int -> float -> int through the core registers: 64 = 16 round trips x 4 instructions. */
__attribute__((noinline)) static void vfpCvtTrip(void) {
    __asm__ volatile("mov r1, #7\n.rept 16\nvmov s0, r1\nvcvt.f32.s32 s0, s0\nvcvt.s32.f32 s0, s0\n"
                     "vmov r1, s0\n.endr\n" : : : "r1", "s0", "memory");
}

static double vfpCycles(void (*fn)(void), double opsPerCall) {
    const int calls = 2000;
    fn();
    const u64 t0 = nowTicks();
    for (int i = 0; i < calls; i++) {
        fn();
    }
    return perUnit(nowTicks() - t0, calls * opsPerCall);
}

/* Column-major (GL/N64-style) out = a * b, scalar VFP (what drawprobe T5 timed). */
__attribute__((noinline)) static void mtxHand(float* out, const float* a, const float* b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                                 a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/* The same product in VFP short-vector mode: FPSCR LEN=4, STRIDE=1. a's columns live in the
 * vector banks s8-s23, each b column is loaded as 4 scalars into bank 0 (s0-s3), and each
 * output column is 1 VMUL + 3 VMLA of length 4 into s24-s27. Same operation order as mtxHand,
 * so the result is bit-identical (VFPv2 VMLA is not fused). FPSCR is restored before exit. */
__attribute__((noinline)) static void mtxShortVector(float* out, const float* a, const float* b) {
    __asm__ volatile(
        "vmrs r12, fpscr\n"
        "bic r3, r12, #0x00370000\n"
        "orr r3, r3, #0x00030000\n"
        "vldmia %[a], {s8-s23}\n"
        "vmsr fpscr, r3\n"
        ".rept 4\n"
        "vldmia %[b]!, {s0-s3}\n"
        "vmul.f32 s24, s8, s0\n"
        "vmla.f32 s24, s12, s1\n"
        "vmla.f32 s24, s16, s2\n"
        "vmla.f32 s24, s20, s3\n"
        "vstmia %[o]!, {s24-s27}\n"
        ".endr\n"
        "vmsr fpscr, r12\n"
        : [o] "+r"(out), [b] "+r"(b)
        : [a] "r"(a)
        : "r3", "r12", "memory", "s0", "s1", "s2", "s3", "s8", "s9", "s10", "s11", "s12", "s13", "s14",
          "s15", "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27");
}

static float sMtxA[16];
static float sMtxB[16];
static float sMtxOut[16];
static C3D_Mtx sC3dA;
static C3D_Mtx sC3dB;
static C3D_Mtx sC3dOut;

static void buildMtxInputs(void) {
    for (int i = 0; i < 16; i++) {
        sMtxA[i] = (i % 5 == 0) ? 1.0f : 0.01f * (float)(i + 1);
        sMtxB[i] = (i % 5 == 0) ? 0.5f : 0.003f * (float)(i + 7);
    }
    Mtx_Identity(&sC3dA);
    Mtx_Translate(&sC3dA, 1.0f, 2.0f, -30.0f, true);
    Mtx_Identity(&sC3dB);
    Mtx_RotateY(&sC3dB, 0.3f, false);
}

typedef enum { MTX_HAND, MTX_C3D, MTX_VEC } MtxKind;

static double mtxCycles(MtxKind kind) {
    const u64 t0 = nowTicks();
    for (int i = 0; i < MTX_REPS; i++) {
        if (kind == MTX_HAND) {
            mtxHand(sMtxOut, sMtxA, sMtxB);
        } else if (kind == MTX_C3D) {
            Mtx_Multiply(&sC3dOut, &sC3dA, &sC3dB);
        } else {
            mtxShortVector(sMtxOut, sMtxA, sMtxB);
        }
        sMtxA[3] += 1.0e-7f; /* keep the compiler from hoisting */
    }
    return perUnit(nowTicks() - t0, MTX_REPS);
}

static bool mtxVectorMatches(void) {
    float hand[16];
    float vec[16];
    mtxHand(hand, sMtxA, sMtxB);
    mtxShortVector(vec, sMtxA, sMtxB);
    return memcmp(hand, vec, sizeof(hand)) == 0;
}

/* Vertex transform: N64 Vtx-style s16 x,y,z (+pad) by a 4x4, four outputs per vertex. */
static s16 sXfIn[XF_VERTS * 4];
static float sXfOutF[XF_VERTS * 4];
static s32 sXfOutQ[XF_VERTS * 4];
static float sXfM[4][4];
static u32 sXfQ01[4];
static u32 sXfQ2t[4];

static inline u32 packPair(s32 lo, s32 hi) {
    return ((u32)lo & 0xFFFFu) | ((u32)hi << 16);
}

static inline s32 smuad(u32 a, u32 b) {
    s32 r;
    __asm__("smuad %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static inline s32 smlad(u32 a, u32 b, s32 acc) {
    s32 r;
    __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc));
    return r;
}

static void buildXfInputs(void) {
    for (int i = 0; i < XF_VERTS * 4; i++) {
        sXfIn[i] = (s16)((int)(nextRandom() % 8192) - 4096);
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            sXfM[r][c] = (r == c) ? 0.9f : 0.05f * (float)(r - c);
        }
    }
    for (int k = 0; k < 4; k++) {
        sXfQ01[k] = packPair((s32)(sXfM[0][k] * 4096.0f), (s32)(sXfM[1][k] * 4096.0f));
        sXfQ2t[k] = packPair((s32)(sXfM[2][k] * 4096.0f), (s32)(sXfM[3][k] * 64.0f));
    }
}

__attribute__((noinline)) static void xformFloat(void) {
    const float(*m)[4] = sXfM;
    for (int i = 0; i < XF_VERTS; i++) {
        const s16* v = &sXfIn[i * 4];
        float* o = &sXfOutF[i * 4];
        for (int k = 0; k < 4; k++) {
            o[k] = v[0] * m[0][k] + v[1] * m[1][k] + v[2] * m[2][k] + m[3][k];
        }
    }
}

/* x,y already sit packed in one word (s16 pair); z gets a constant 1 in its top half so the
 * translation rides the second SMLAD. Q12 matrix, 32-bit result. */
__attribute__((noinline)) static void xformSmlad(void) {
    for (int i = 0; i < XF_VERTS; i++) {
        const u32* v = (const u32*)&sXfIn[i * 4];
        const u32 xy = v[0];
        const u32 z1 = (v[1] & 0xFFFFu) | 0x10000u;
        s32* o = &sXfOutQ[i * 4];
        for (int k = 0; k < 4; k++) {
            o[k] = smlad(sXfQ2t[k], z1, smuad(sXfQ01[k], xy));
        }
    }
}

static double xfCycles(void (*fn)(void)) {
    fn();
    const u64 t0 = nowTicks();
    for (int r = 0; r < 10; r++) {
        fn();
    }
    return perUnit(nowTicks() - t0, 10.0 * XF_VERTS);
}

static void testMath(void) {
    logLine("[r60] E vfp cyc/op: mla_dep=%.1f mla_4chain=%.2f mul_dep=%.1f add_dep=%.1f div_dep=%.1f "
            "sqrt_dep=%.1f mla64_dep=%.1f div64_dep=%.1f cvt_roundtrip=%.1f",
            vfpCycles(vfpMlaDep, 64), vfpCycles(vfpMla4, 64), vfpCycles(vfpMulDep, 64),
            vfpCycles(vfpAddDep, 64), vfpCycles(vfpDivDep, 64), vfpCycles(vfpSqrtDep, 64),
            vfpCycles(vfpMlaDepF64, 64), vfpCycles(vfpDivDepF64, 64), vfpCycles(vfpCvtTrip, 16));
    logLine("[r60] E mtx4x4 cyc: scalar=%.0f c3d_Mtx_Multiply=%.0f shortvec=%.0f shortvec_match=%d",
            mtxCycles(MTX_HAND), mtxCycles(MTX_C3D), mtxCycles(MTX_VEC), mtxVectorMatches() ? 1 : 0);
    logLine("[r60] E xform s16xyz->4 cyc/vertex: float=%.1f smlad_q12=%.1f", xfCycles(xformFloat),
            xfCycles(xformSmlad));
}

/* ---- F: svc / sync / cache flush ------------------------------------------------------------- */

static double flushCycles(bool viaGsp, u32 bytes) {
    const int reps = 20;
    u64 total = 0;
    for (int r = 0; r < reps; r++) {
        memset(sLinearMem, r, bytes);
        const u64 t0 = nowTicks();
        if (viaGsp) {
            GSPGPU_FlushDataCache(sLinearMem, bytes);
        } else {
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)sLinearMem, bytes);
        }
        total += nowTicks() - t0;
    }
    return perUnit(total, reps);
}

static void testSvc(void) {
    const int n = 10000;
    u64 t0 = nowTicks();
    for (int i = 0; i < n; i++) {
        sSink += (u32)svcGetSystemTick();
    }
    const double tick = perUnit(nowTicks() - t0, n);

    LightLock lock;
    LightLock_Init(&lock);
    t0 = nowTicks();
    for (int i = 0; i < n; i++) {
        LightLock_Lock(&lock);
        LightLock_Unlock(&lock);
    }
    const double light = perUnit(nowTicks() - t0, n);

    Handle mutex = 0;
    double svcMutex = -1.0;
    if (R_SUCCEEDED(svcCreateMutex(&mutex, false))) {
        t0 = nowTicks();
        for (int i = 0; i < n; i++) {
            svcWaitSynchronization(mutex, U64_MAX);
            svcReleaseMutex(mutex);
        }
        svcMutex = perUnit(nowTicks() - t0, n);
        svcCloseHandle(mutex);
    }
    logLine("[r60] F cyc: svcGetSystemTick=%.0f LightLock_pair=%.0f svc_mutex_pair=%.0f", tick, light,
            svcMutex);
    logLine("[r60] F flush us: gsp 32B=%.1f 4K=%.1f 64K=%.1f 256K=%.1f | svcFlushProcessDataCache 32B=%.1f "
            "4K=%.1f 64K=%.1f 256K=%.1f",
            flushCycles(true, 32) / 268.1, flushCycles(true, 4096) / 268.1, flushCycles(true, 65536) / 268.1,
            flushCycles(true, 262144) / 268.1, flushCycles(false, 32) / 268.1, flushCycles(false, 4096) / 268.1,
            flushCycles(false, 65536) / 268.1, flushCycles(false, 262144) / 268.1);
}

/* ---- L: hwprof (docs/research/native/hwprof-progress.md) --------------------------------------
 * The costs the emulator does not charge and the game pays around its retained/replay paths:
 * what a whole-linear-heap flush does to the CPU's OTHER working set, reading back linear memory
 * the CPU wrote and flushed, small copies into linear memory (streamed-pool floats), integer
 * division (no hardware divider on the ARM11) and float<->double conversion. */

#define L_SET_BYTES (8u * 1024u)
#define L_LINES (L_SET_BYTES / 32u)

/* One load per 32-byte line over `bytes`, cycles per line. */
static double lineReadCycles(const u32* p, u32 bytes) {
    const u64 t0 = nowTicks();
    u32 s = 0;
    for (u32 i = 0; i < bytes / 4; i += 8) {
        s += p[i];
    }
    const u64 t1 = nowTicks();
    sSink += s;
    return perUnit(t1 - t0, (double)(bytes / 32));
}

static void touch(const u32* p, u32 bytes) {
    sSink += readSum(p, bytes);
}

/* L flush-evict: does a whole-linear-heap GSPGPU_FlushDataCache (C3D_FrameEnd(0), an [overlap]
 * kick) also throw away the HEAP working set? warm = re-read of 8 KB of heap just read; the other
 * columns re-read the same 8 KB after one operation in between. If after_heapflush ~= evicted, every
 * heap flush costs the next ~16 KB of D-cache refills on top of its own time. */
static void testFlushEvict(void) {
    extern u32 __ctru_linear_heap;
    extern u32 __ctru_linear_heap_size;
    const u32* set = sHeapMem;
    const u32* other = sHeapMem + (1u << 18); /* 1 MB further on */
    double warm = 0.0;
    double afterHeap = 0.0;
    double afterRange = 0.0;
    double afterSvcRange = 0.0;
    double evicted = 0.0;
    double heapUs = 0.0;
    const int reps = 8;
    for (int r = 0; r < reps; r++) {
        touch(set, L_SET_BYTES);
        warm += lineReadCycles(set, L_SET_BYTES);

        touch(set, L_SET_BYTES);
        const u64 h0 = nowTicks();
        GSPGPU_FlushDataCache((const void*)__ctru_linear_heap, __ctru_linear_heap_size);
        heapUs += ticksToUs(nowTicks() - h0);
        afterHeap += lineReadCycles(set, L_SET_BYTES);

        touch(set, L_SET_BYTES);
        GSPGPU_FlushDataCache(sLinearMem, 64u * 1024u);
        afterRange += lineReadCycles(set, L_SET_BYTES);

        touch(set, L_SET_BYTES);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)sLinearMem, 64u * 1024u);
        afterSvcRange += lineReadCycles(set, L_SET_BYTES);

        touch(set, L_SET_BYTES);
        touch(other, 64u * 1024u); /* control: a 64 KB sweep evicts the 16 KB L1D for sure */
        evicted += lineReadCycles(set, L_SET_BYTES);
    }
    logLine("[r60] L flush-evict heap8K cyc/line: warm=%.1f after_heapflush=%.1f after_gsp64K=%.1f "
            "after_svc64K=%.1f evicted=%.1f | heapflush_us=%.0f (heap=%luKB)",
            warm / reps, afterHeap / reps, afterRange / reps, afterSvcRange / reps, evicted / reps, heapUs / reps,
            (unsigned long)(__ctru_linear_heap_size / 1024u));
}

/* L readback: the CPU re-reads linear memory it wrote (a retained record, a command list, a VBO
 * it scans) -- warm, after a range flush (clean+invalidate: every line refills from FCRAM), and
 * after a 64 KB sweep. Also the same in VRAM (uncached for the CPU). */
static void testReadback(void) {
    const u32 bytes = 16u * 1024u;
    double warm = 0.0;
    double flushed = 0.0;
    double swept = 0.0;
    const int reps = 8;
    for (int r = 0; r < reps; r++) {
        memset(sLinearMem, r, bytes);
        warm += lineReadCycles(sLinearMem, bytes);
        memset(sLinearMem, r + 1, bytes);
        GSPGPU_FlushDataCache(sLinearMem, bytes);
        flushed += lineReadCycles(sLinearMem, bytes);
        memset(sLinearMem, r + 2, bytes);
        touch(sHeapMem + (1u << 18), 64u * 1024u);
        swept += lineReadCycles(sLinearMem, bytes);
    }
    double vram = -1.0;
    if (sVramMem != NULL) {
        vram = lineReadCycles(sVramMem, bytes);
        vram = lineReadCycles(sVramMem, bytes); /* second pass: is it ever cached? */
    }
    logLine("[r60] L readback linear16K cyc/line: warm=%.1f after_flush=%.1f after_sweep=%.1f | vram=%.1f",
            warm / reps, flushed / reps, swept / reps, vram);
}

/* L copy: small memcpy into linear memory vs heap, the shape of a retained replay writing its
 * recorded floats into the streamed pool. cold dst = the destination lines were swept out. */
static double copyCycles(u32* dst, const u32* src, u32 bytes, bool coldDst) {
    const int calls = 64;
    u64 total = 0;
    for (int c = 0; c < calls; c++) {
        u32* d = dst + (u32)c * (bytes / 4);
        if (coldDst) {
            touch(sHeapMem + (1u << 18), 32u * 1024u);
        } else {
            memset(d, 0, bytes);
        }
        touch(src, bytes);
        const u64 t0 = nowTicks();
        memcpy(d, src, bytes);
        total += nowTicks() - t0;
    }
    sSink += dst[0];
    return perUnit(total, calls);
}

static void testSmallCopy(void) {
    static const u32 kSizes[] = {64, 256, 1024, 4096};
    char line[384];
    int n = snprintf(line, sizeof(line), "[r60] L copy cyc/call heap>linear warm/cold | heap>heap warm/cold:");
    for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]) && n < (int)sizeof(line); i++) {
        const u32 b = kSizes[i];
        n += snprintf(line + n, sizeof(line) - n, " %luB=%.0f/%.0f|%.0f/%.0f", (unsigned long)b,
                      copyCycles(sLinearMem, sHeapMem + (2u << 18), b, false),
                      copyCycles(sLinearMem, sHeapMem + (2u << 18), b, true),
                      copyCycles(sHeapMem + (3u << 18), sHeapMem + (2u << 18), b, false),
                      copyCycles(sHeapMem + (3u << 18), sHeapMem + (2u << 18), b, true));
    }
    logLine("%s", line);
}

/* L div: dependent chains through the EABI helpers (the ARM11 has no divide instruction). */
static volatile u32 sDivU = 7u;
static volatile s32 sDivS = -7;
static volatile u64 sDivU64 = 7ull;

__attribute__((noinline)) static u32 chainUdiv(u32 x, u32 d, int n) {
    for (int i = 0; i < n; i++) {
        x = x / d + 0x10000000u;
    }
    return x;
}

__attribute__((noinline)) static s32 chainSdiv(s32 x, s32 d, int n) {
    for (int i = 0; i < n; i++) {
        x = x / d + 0x10000000;
    }
    return x;
}

__attribute__((noinline)) static u32 chainUmod(u32 x, u32 d, int n) {
    for (int i = 0; i < n; i++) {
        x = (x % d) * 0x9E3779B1u + 0x7F4A7C15u;
    }
    return x;
}

__attribute__((noinline)) static u64 chainUdiv64(u64 x, u64 d, int n) {
    for (int i = 0; i < n; i++) {
        x = x / d + 0x1000000000000000ull;
    }
    return x;
}

__attribute__((noinline)) static u32 chainMul(u32 x, u32 d, int n) {
    for (int i = 0; i < n; i++) {
        x = x * d + 0x10000000u;
    }
    return x;
}

static void testDiv(void) {
    const int n = 20000;
    u64 t0 = nowTicks();
    sSink += chainMul(0xFFFFFFFFu, sDivU, n);
    const double mul = perUnit(nowTicks() - t0, n);
    t0 = nowTicks();
    sSink += chainUdiv(0xFFFFFFFFu, sDivU, n);
    const double udiv = perUnit(nowTicks() - t0, n);
    t0 = nowTicks();
    sSink += (u32)chainSdiv(0x7FFFFFFF, sDivS, n);
    const double sdiv = perUnit(nowTicks() - t0, n);
    t0 = nowTicks();
    sSink += chainUmod(0xFFFFFFFFu, sDivU + 1000u, n);
    const double umod = perUnit(nowTicks() - t0, n);
    t0 = nowTicks();
    sSink += (u32)chainUdiv64(0xFFFFFFFFFFFFFFFFull, sDivU64, n);
    const double udiv64 = perUnit(nowTicks() - t0, n);
    logLine("[r60] L div cyc/op (dependent, incl. loop): mul=%.1f udiv32=%.1f sdiv32=%.1f umod32=%.1f udiv64=%.1f",
            mul, udiv, sdiv, umod, udiv64);
}

/* L f64: float<->double round trips (what a stray double literal costs in float code) and
 * double add/mul latency next to the f32 ones of section E. */
__attribute__((noinline)) static void vfpCvt64Trip(void) {
    __asm__ volatile("vldmia %0, {s0-s1}\n.rept 32\nvcvt.f64.f32 d1, s0\nvcvt.f32.f64 s0, d1\n.endr\n"
                     : : "r"(kOnes) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpAddDepF64(void) {
    __asm__ volatile("vldmia %0, {d0-d2}\n.rept 64\nvadd.f64 d0, d0, d1\n.endr\n" : : "r"(kOnesD) : VFP_S_CLOBBER);
}

__attribute__((noinline)) static void vfpMulDepF64(void) {
    __asm__ volatile("vldmia %0, {d0-d2}\n.rept 64\nvmul.f64 d0, d0, d1\n.endr\n" : : "r"(kOnesD) : VFP_S_CLOBBER);
}

static void testF64(void) {
    logLine("[r60] L f64 cyc/op: cvt_f32_f64_roundtrip=%.1f add64_dep=%.1f mul64_dep=%.1f (f32: see E add_dep/mul_dep)",
            vfpCycles(vfpCvt64Trip, 32), vfpCycles(vfpAddDepF64, 64), vfpCycles(vfpMulDepF64, 64));
}

static void testHwprof(void) {
    testFlushEvict();
    testReadback();
    testSmallCopy();
    testDiv();
    testF64();
}

/* ---- P: CP15 performance counters -------------------------------------------------------------- */

static bool sPmuOk;

static const PerfCounterEvent kPmuEvents[8] = {
    PERFCOUNTEREVT_CORE_INST_CACHE_MISS,   PERFCOUNTEREVT_CORE_INST_EXECUTED,
    PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS,
    PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD,
    PERFCOUNTEREVT_CORE_MAIN_TLB_MISS,     PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED,
};

static u32 pmuRead(PerfCounterRegister reg) {
    u64 value = 0;
    svcControlPerformanceCounter(&value, PERFCOUNTEROP_GET_VALUE, reg, 0);
    return (u32)value;
}

static bool pmuSelect(int pair) {
    u64 out = 0;
    const Result r0 = svcControlPerformanceCounter(&out, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0,
                                                   kPmuEvents[pair * 2]);
    const Result r1 = svcControlPerformanceCounter(&out, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1,
                                                   kPmuEvents[pair * 2 + 1]);
    return R_SUCCEEDED(r0) && R_SUCCEEDED(r1);
}

static void pmuInit(void) {
    u64 out = 0;
    const Result rc = svcControlPerformanceCounter(&out, PERFCOUNTEROP_ENABLE, 0, 0);
    sPmuOk = R_SUCCEEDED(rc) && pmuSelect(0);
    logLine("[r60] P pmu enable rc=0x%08lX ok=%d", (unsigned long)rc, sPmuOk ? 1 : 0);
}

static void pmuFini(void) {
    if (sPmuOk) {
        u64 out = 0;
        svcControlPerformanceCounter(&out, PERFCOUNTEROP_DISABLE, 0, 0);
    }
}

/* Runs `work` once per counter pair (warmed first) and prints events per unit. Counters are
 * per core and count kernel + other core-0 threads too: read the ratios, not the last digit. */
static void pmuProfile(const char* name, void (*work)(void), double units) {
    if (!sPmuOk) {
        return;
    }
    double ev[8];
    u64 cycles = 0;
    for (int pair = 0; pair < 4; pair++) {
        pmuSelect(pair);
        work();
        const u32 a0 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
        const u32 a1 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_1);
        const u64 t0 = nowTicks();
        work();
        cycles += nowTicks() - t0;
        const u32 b0 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
        const u32 b1 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_1);
        ev[pair * 2] = (double)(u32)(b0 - a0) / units;
        ev[pair * 2 + 1] = (double)(u32)(b1 - a1) / units;
    }
    const double cyc = (double)cycles / 4.0 / units;
    logLine("[r60] P %s per unit: cyc=%.1f inst=%.1f cpi=%.2f imiss=%.2f drmiss=%.2f dwmiss=%.2f "
            "stall_ifetch=%.1f stall_dhazard=%.1f tlbmiss=%.2f brmiss=%.2f",
            name, cyc, ev[1], ev[1] > 0.0 ? cyc / ev[1] : 0.0, ev[0], ev[2], ev[3], ev[4], ev[5], ev[6],
            ev[7]);
}

static void workC3dDraws(void) {
    const u32 mark = cmdMark();
    loopC3dDrawUnif();
    cmdRewind(mark);
}

static void workRawDraws(void) {
    const u32 mark = cmdMark();
    loopRawDrawUnif();
    cmdRewind(mark);
}

static void workICode8k(void) {
    for (int i = 0; i < 50; i++) {
        icode8k();
    }
}

static void workICode32k(void) {
    for (int i = 0; i < 50; i++) {
        icode32k();
    }
}

static void workChase(void) {
    sSink += chase(sHeapMem, CHASE_STEPS);
}

static void workMtxHand(void) {
    for (int i = 0; i < MTX_REPS; i++) {
        mtxHand(sMtxOut, sMtxA, sMtxB);
    }
}

static void workXformFloat(void) {
    xformFloat();
}

/* What one counter read costs: the unit of the game's [c5pmu] overhead (two reads per bucket switch). */
static void pmuReadCost(void) {
    if (!sPmuOk) {
        return;
    }
    const int n = 2000;
    const u64 t0 = nowTicks();
    for (int i = 0; i < n; i++) {
        sSink += pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
    }
    const double getValue = perUnit(nowTicks() - t0, n);
    pmuSelect(0); /* inst, imiss */
    const u32 i0 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
    const u32 m0 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_1);
    for (int i = 0; i < 100; i++) {
        sSink += pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
    }
    const u32 i1 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_0);
    const u32 m1 = pmuRead(PERFCOUNTERREG_CORE_COUNT_REG_1);
    logLine("[r60] P read cyc: get_value=%.0f | per read: inst=%.0f imiss=%.1f (the game's [c5pmu] pays 2 per "
            "bucket switch)", getValue, (double)(u32)(i1 - i0) / 101.0, (double)(u32)(m1 - m0) / 101.0);
}

static void testPmu(void) {
    pmuInit();
    pmuReadCost();
    pmuProfile("c3d_draw+unif (per draw)", workC3dDraws, DRAWS);
    pmuProfile("raw_draw+unif (per draw)", workRawDraws, DRAWS);
    pmuProfile("icode8k (per insn)", workICode8k, 50.0 * 2048);
    pmuProfile("icode32k (per insn)", workICode32k, 50.0 * 8192);
    buildChase(sHeapMem, 16 * 1024);
    pmuProfile("chase_heap16K (per load)", workChase, CHASE_STEPS);
    buildChase(sHeapMem, 1024 * 1024);
    pmuProfile("chase_heap1M (per load)", workChase, CHASE_STEPS);
    pmuProfile("mtx_scalar (per 4x4)", workMtxHand, MTX_REPS);
    pmuProfile("xform_float (per vertex)", workXformFloat, XF_VERTS);
    pmuFini();
}

/* ---- H: core 1 ----------------------------------------------------------------------------- */

static volatile u32 sSpinCount;
static volatile bool sSpinStop;

static void spinMain(void* arg) {
    (void)arg;
    while (!sSpinStop) {
        sSpinCount++;
    }
}

/* Spins a thread on `core` for SPIN_MS while main times 20 GSP flushes of 64 KB (GSP is a
 * sysmodule on core 1). Returns the spin count, -1 if the thread could not be created. */
static s64 spinOn(int core, double* flushUs) {
    sSpinCount = 0;
    sSpinStop = false;
    Thread t = threadCreate(spinMain, NULL, 4096, 0x30, core, false);
    if (t == NULL) {
        return -1;
    }
    const u64 start = nowTicks();
    *flushUs = flushCycles(true, 65536) / 268.1;
    const u64 spent = nowTicks() - start;
    const s64 left = (s64)SPIN_MS * 1000000LL - (s64)(ticksToUs(spent) * 1000.0);
    if (left > 0) {
        svcSleepThread(left);
    }
    sSpinStop = true;
    threadJoin(t, U64_MAX);
    threadFree(t);
    return (s64)sSpinCount;
}

static void testCore1(void) {
    const double idleFlush = flushCycles(true, 65536) / 268.1;
    double flush0 = 0.0;
    const s64 base = spinOn(0, &flush0);
    logLine("[r60] H core0 spin=%lld per %dms (reference) flush64K_us idle=%.1f with_core0_spinner=%.1f",
            (long long)base, SPIN_MS, idleFlush, flush0);
    static const u32 kPercents[] = {30, 50, 80};
    for (size_t i = 0; i < sizeof(kPercents) / sizeof(kPercents[0]); i++) {
        const Result rc = APT_SetAppCpuTimeLimit(kPercents[i]);
        u32 got = 0;
        APT_GetAppCpuTimeLimit(&got);
        double flush1 = 0.0;
        const s64 c1 = spinOn(1, &flush1);
        logLine("[r60] H syscore set=%lu rc=0x%08lX got=%lu core1_spin=%lld share=%.1f%% "
                "flush64K_us_while_core1_busy=%.1f",
                (unsigned long)kPercents[i], (unsigned long)rc, (unsigned long)got, (long long)c1,
                (base > 0 && c1 >= 0) ? 100.0 * (double)c1 / (double)base : -1.0, flush1);
    }
    APT_SetAppCpuTimeLimit(30);
}

/* ==== R: rendered tests (researcher F): FLUSH, RESYNC, T7 REPLAY, T8 CADENCE =============== */

/* citro3d / libctru internals, exported from the static libraries. */
extern void C3Di_DirtyUniforms(GPU_SHADER_TYPE type);
extern void C3Di_RenderQueueWaitDone(void);
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

#define R_DISPLAY_FLAGS                                                                          \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |             \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define R_READBACK_FLAGS                                                                          \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |              \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define R_TARGET_W 240
#define R_TARGET_H 400
#define R_READBACK_BYTES (R_TARGET_W * R_TARGET_H * 4)
#define R_CLEAR_COLOR 0x202830FF
#define R_TEX_COUNT 8
#define R_TEX_SIZE 8
#define R_RING 16
#define R_MESH_VERTS (R_RING + 1)
#define R_MESH_INDICES (R_RING * 3)
#define R_SCENE_DRAWS 60
#define R_BIND_EVERY 8 /* binds at draws 0, 8, .., 56: 8 binds per 60-draw frame */
#define R_GRID_COLS 10
#define R_CELL 40.0f
#define SPAN_MAX_WORDS 16384
#define FLUSH_WARMUP 4
#define FLUSH_FRAMES 30
#define FLUSH_REPS 10
#define RESYNC_BLOCKS 4
#define RESYNC_RAW_DRAWS 50
#define RESYNC_WARMUP 2
#define RESYNC_FRAMES 20
#define T7_REPS 10
#define T8_FRAMES 600
#define T8_SPIN_US 22000
#define T8_SEQ_PER_LINE 100

typedef struct {
    double sum;
    double max;
    int n;
} UsStat;

static void statAdd(UsStat* s, double us) {
    s->sum += us;
    if (us > s->max) {
        s->max = us;
    }
    s->n++;
}

static double statMean(const UsStat* s) {
    return s->n > 0 ? s->sum / s->n : 0.0;
}

static double usSince(u64 t0) {
    return ticksToUs(nowTicks() - t0);
}

static C3D_RenderTarget* sTop;
static DVLB_s* sDvlb;
static shaderProgram_s sProgram;
static int sLocProj;
static int sLocMv;
static int sLocLightDir;
static int sLocLightAmb;
static int sLocScales;
static int sLocLivery;
static C3D_Tex sRTex[R_TEX_COUNT];
static VertexF* sRVerts;
static u16* sRIdx;
static C3D_BufInfo sRBuf;
static C3D_AttrInfo sRAttr;
static u32 sRIdxOffset;
static C3D_Mtx sRProj;
static C3D_Mtx sRMtx[2][R_SCENE_DRAWS]; /* set 0 = frame A, set 1 = frame B / direct reference */
static u32* sReadback;
static u32 sCrcTable[256];
static bool sRenderOk;

/* ---- R: setup --------------------------------------------------------------------------- */

static void buildCrcTable(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        sCrcTable[n] = c;
    }
}

static u32 crc32Bytes(const u8* p, u32 bytes) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < bytes; i++) {
        c = sCrcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static bool loadRenderShader(void) {
    sDvlb = DVLB_ParseFile((u32*)r60probe_shader_shbin, r60probe_shader_shbin_size);
    if (sDvlb == NULL) {
        return false;
    }
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sLocProj = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
    sLocMv = shaderInstanceGetUniformLocation(sProgram.vertexShader, "modelView");
    sLocLightDir = shaderInstanceGetUniformLocation(sProgram.vertexShader, "lightDir");
    sLocLightAmb = shaderInstanceGetUniformLocation(sProgram.vertexShader, "lightAmb");
    sLocScales = shaderInstanceGetUniformLocation(sProgram.vertexShader, "scales");
    sLocLivery = shaderInstanceGetUniformLocation(sProgram.vertexShader, "livery");
    return sLocProj >= 0 && sLocMv >= 0 && sLocLightDir >= 0 && sLocLightAmb >= 0 && sLocScales >= 0 &&
           sLocLivery >= 0;
}

static bool buildRenderTextures(void) {
    static const u32 kColors[R_TEX_COUNT] = {
        0xFF4040FF, 0x40FF40FF, 0x4040FFFF, 0xFFFF40FF, 0xFF40FFFF, 0x40FFFFFF, 0xC08040FF, 0x8040C0FF,
    };
    for (int t = 0; t < R_TEX_COUNT; t++) {
        if (!C3D_TexInit(&sRTex[t], R_TEX_SIZE, R_TEX_SIZE, GPU_RGBA8)) {
            return false;
        }
        u32* texels = (u32*)sRTex[t].data;
        for (int i = 0; i < R_TEX_SIZE * R_TEX_SIZE; i++) {
            texels[i] = kColors[t];
        }
        C3D_TexSetFilter(&sRTex[t], GPU_NEAREST, GPU_NEAREST);
        C3D_TexFlush(&sRTex[t]);
    }
    return true;
}

/* A 16-triangle disc; vertices and indices share one linear block so indices sit above base. */
static bool buildRenderMesh(void) {
    const u32 vertBytes = (u32)((R_MESH_VERTS * sizeof(VertexF) + 15u) & ~15u);
    u8* block = (u8*)linearAlloc(vertBytes + R_MESH_INDICES * sizeof(u16));
    if (block == NULL) {
        return false;
    }
    sRVerts = (VertexF*)block;
    sRIdx = (u16*)(block + vertBytes);
    const VertexF center = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 0.5f}};
    sRVerts[0] = center;
    for (int i = 0; i < R_RING; i++) {
        const float a = (float)i * 6.2831853f / (float)R_RING;
        const VertexF v = {{cosf(a), sinf(a), 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 0.5f}};
        sRVerts[1 + i] = v;
        sRIdx[i * 3 + 0] = 0;
        sRIdx[i * 3 + 1] = (u16)(1 + i);
        sRIdx[i * 3 + 2] = (u16)(1 + (i + 1) % R_RING);
    }
    GSPGPU_FlushDataCache(block, vertBytes + R_MESH_INDICES * sizeof(u16));
    BufInfo_Init(&sRBuf);
    BufInfo_Add(&sRBuf, sRVerts, sizeof(VertexF), 3, 0x210);
    AttrInfo_Init(&sRAttr);
    AttrInfo_AddLoader(&sRAttr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sRAttr, 1, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sRAttr, 2, GPU_FLOAT, 2);
    sRIdxOffset = osConvertVirtToPhys(sRIdx) - sRBuf.base_paddr;
    return true;
}

static void buildRenderMatrices(void) {
    Mtx_OrthoTilt(&sRProj, 0.0f, 400.0f, 0.0f, 240.0f, -100.0f, 100.0f, true);
    for (int set = 0; set < 2; set++) {
        for (int i = 0; i < R_SCENE_DRAWS; i++) {
            C3D_Mtx* m = &sRMtx[set][i];
            Mtx_Identity(m);
            Mtx_Translate(m, R_CELL * ((float)(i % R_GRID_COLS) + 0.5f), R_CELL * ((float)(i / R_GRID_COLS) + 0.5f),
                          0.0f, true);
            Mtx_RotateZ(m, 0.1f * (float)i + 0.35f * (float)set, true);
            Mtx_Scale(m, 16.0f, 16.0f - 3.0f * (float)set, 16.0f);
        }
    }
}

static bool renderInit(void) {
    sTop = C3D_RenderTargetCreate(R_TARGET_W, R_TARGET_H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sTop == NULL) {
        return false;
    }
    C3D_RenderTargetSetOutput(sTop, GFX_TOP, GFX_LEFT, R_DISPLAY_FLAGS);
    sReadback = (u32*)linearAlloc(R_READBACK_BYTES);
    if (sReadback == NULL || !loadRenderShader() || !buildRenderTextures() || !buildRenderMesh()) {
        return false;
    }
    buildRenderMatrices();
    buildCrcTable();
    return true;
}

/* ---- R: frame helpers ------------------------------------------------------------------- */

static void setRenderTexEnv(bool alt) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, alt ? GPU_ADD : GPU_MODULATE);
}

static void setRenderLivery(int i) {
    for (int c = 0; c < 3; c++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocLivery + c, (float)((i * 3 + c * 5) % 7) / 7.0f,
                      (float)((i * 5 + c * 3) % 11) / 11.0f, (float)((i * 7 + c) % 13) / 13.0f, 1.0f);
    }
}

/* Marks every piece of state the scene uses dirty, so the next C3D draw emits all of it: this
 * is B's resync set plus uniforms/texenv buffer/effect, and it makes each frame self-contained. */
static void forceDirtyAll(void) {
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    for (int i = 1; i < 6; i++) {
        C3D_GetTexEnv(i);
    }
    C3D_TexEnvBufColor(0);
    C3D_GetAttrInfo();
    C3D_GetBufInfo();
}

static void applyRenderBase(void) {
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocProj, &sRProj);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocLightDir, 0.4f, 0.6f, 0.69f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocLightAmb, 0.3f, 0.3f, 0.3f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLocScales, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_SetAttrInfo(&sRAttr);
    C3D_SetBufInfo(&sRBuf);
    C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    setRenderTexEnv(false);
}

static bool beginRenderFrame(u8 flags, bool fullState) {
    if (!C3D_FrameBegin(flags)) {
        return false;
    }
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, R_CLEAR_COLOR, 0);
    C3D_FrameDrawOn(sTop);
    if (fullState) {
        forceDirtyAll();
        applyRenderBase();
    }
    return true;
}

static void renderDrawC3d(void) {
    C3D_DrawElements(GPU_TRIANGLES, R_MESH_INDICES, C3D_UNSIGNED_SHORT, sRIdx);
}

static void bindSceneTexture(int i) {
    if (i % R_BIND_EVERY == 0) {
        C3D_TexBind(0, &sRTex[(i / R_BIND_EVERY) % R_TEX_COUNT]);
    }
}

static void drawSceneC3d(int set) {
    for (int i = 0; i < R_SCENE_DRAWS; i++) {
        bindSceneTexture(i);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, &sRMtx[set][i]);
        setRenderLivery(i);
        renderDrawC3d();
    }
}

/* Waits for the queued frame, reads the tiled colour buffer back as linear RGBA8, CRCs it.
 * Only call between frames. */
static u32 readbackCrc(void) {
    GSPGPU_InvalidateDataCache(sReadback, R_READBACK_BYTES);
    C3D_SyncDisplayTransfer((u32*)sTop->frameBuf.colorBuf, GX_BUFFER_DIM(R_TARGET_W, R_TARGET_H), sReadback,
                            GX_BUFFER_DIM(R_TARGET_W, R_TARGET_H), R_READBACK_FLAGS);
    C3Di_RenderQueueWaitDone();
    GSPGPU_InvalidateDataCache(sReadback, R_READBACK_BYTES);
    return crc32Bytes((const u8*)sReadback, R_READBACK_BYTES);
}

/* ---- FLUSH ---------------------------------------------------------------------------------- */

static double timeHeapFlush(u32 dirtyBytes) {
    if (dirtyBytes > 0) {
        memset(sReadback, (int)dirtyBytes, dirtyBytes);
    }
    const u64 t0 = nowTicks();
    GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
    return usSince(t0);
}

static void testFlush(void) {
    UsStat end0 = {0};
    UsStat endFlush = {0};
    for (int f = 0; f < FLUSH_WARMUP + 2 * FLUSH_FRAMES; f++) {
        const bool cmdlistFlush = (f & 1) != 0;
        if (!beginRenderFrame(0, true)) {
            logLine("[r60] FLUSH FAIL frame_begin f=%d", f);
            return;
        }
        drawSceneC3d(0);
        const u64 t0 = nowTicks();
        C3D_FrameEnd(cmdlistFlush ? GX_CMDLIST_FLUSH : 0);
        const double us = usSince(t0);
        if (f >= FLUSH_WARMUP) {
            statAdd(cmdlistFlush ? &endFlush : &end0, us);
        }
    }
    C3Di_RenderQueueWaitDone();
    UsStat heapClean = {0};
    UsStat heapDirty = {0};
    UsStat range64 = {0};
    for (int r = 0; r < FLUSH_REPS; r++) {
        statAdd(&heapClean, timeHeapFlush(0));
        statAdd(&heapDirty, timeHeapFlush(256u * 1024u));
        memset(sReadback, r, 65536);
        const u64 t0 = nowTicks();
        GSPGPU_FlushDataCache(sReadback, 65536);
        statAdd(&range64, usSince(t0));
    }
    logLine("[r60] FLUSH us: frameend0 mean=%.1f max=%.1f | frameend_cmdlistflush mean=%.1f max=%.1f | "
            "heapflush(%.1fMB) clean mean=%.1f max=%.1f dirty256K mean=%.1f | range64K mean=%.1f max=%.1f "
            "(draws/frame=%d)",
            statMean(&end0), end0.max, statMean(&endFlush), endFlush.max,
            (double)__ctru_linear_heap_size / (1024.0 * 1024.0), statMean(&heapClean), heapClean.max,
            statMean(&heapDirty), statMean(&range64), range64.max, R_SCENE_DRAWS);
}

/* ---- RESYNC --------------------------------------------------------------------------------- */

static const C3D_Mtx* resyncMatrix(int j) {
    return &sRMtx[(j / R_SCENE_DRAWS) % 2][j % R_SCENE_DRAWS];
}

static void rawEmitMvDraw(const C3D_Mtx* m) {
    u32* w = cmdReserve(4 + 16 + RAW_DRAW_WORDS);
    if (w != NULL) {
        const u32 n = rawUniforms(w, (u32)sLocMv, m->r, 4);
        rawDraw(w + n, sRIdxOffset, R_MESH_INDICES);
    }
}

typedef struct {
    UsStat resyncCalls;
    UsStat resyncDraw;
    UsStat blockPerDraw;
} ResyncStats;

/* One frame of RESYNC_BLOCKS x [C3D lead draw, 50 draws (raw or C3D), state change + C3D draw]. */
static bool resyncFrame(bool raw, ResyncStats* st) {
    if (!beginRenderFrame(0, true)) {
        return false;
    }
    int j = 0;
    for (int b = 0; b < RESYNC_BLOCKS; b++) {
        C3D_TexBind(0, &sRTex[0]);
        setRenderTexEnv(false);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, resyncMatrix(j++));
        setRenderLivery(b);
        renderDrawC3d();
        const u64 tb = nowTicks();
        for (int k = 0; k < RESYNC_RAW_DRAWS; k++) {
            if (raw) {
                rawEmitMvDraw(resyncMatrix(j++));
            } else {
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, resyncMatrix(j++));
                renderDrawC3d();
            }
        }
        const double blockUs = usSince(tb);
        const u64 t0 = nowTicks();
        C3D_TexBind(0, &sRTex[1 + b % (R_TEX_COUNT - 1)]);
        setRenderTexEnv(true);
        if (raw) {
            C3D_GetAttrInfo();
            C3D_GetBufInfo();
        }
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, resyncMatrix(j++));
        const u64 t1 = nowTicks();
        renderDrawC3d();
        const u64 t2 = nowTicks();
        if (st != NULL) {
            statAdd(&st->resyncCalls, ticksToUs(t1 - t0));
            statAdd(&st->resyncDraw, ticksToUs(t2 - t1));
            statAdd(&st->blockPerDraw, blockUs / RESYNC_RAW_DRAWS);
        }
    }
    C3D_FrameEnd(0);
    return true;
}

static void testResync(void) {
    ResyncStats mix;
    ResyncStats ref;
    memset(&mix, 0, sizeof(mix));
    memset(&ref, 0, sizeof(ref));
    for (int f = 0; f < RESYNC_WARMUP + RESYNC_FRAMES; f++) {
        const bool measuring = f >= RESYNC_WARMUP;
        if (!resyncFrame(true, measuring ? &mix : NULL) || !resyncFrame(false, measuring ? &ref : NULL)) {
            logLine("[r60] RESYNC FAIL frame_begin f=%d", f);
            return;
        }
    }
    C3Di_RenderQueueWaitDone();
    u32 crcMix = 0;
    u32 crcRef = 0;
    if (resyncFrame(true, NULL)) {
        crcMix = readbackCrc();
    }
    if (resyncFrame(false, NULL)) {
        crcRef = readbackCrc();
    }
    const double mixTotal = statMean(&mix.resyncCalls) + statMean(&mix.resyncDraw);
    const double refTotal = statMean(&ref.resyncCalls) + statMean(&ref.resyncDraw);
    logLine("[r60] RESYNC us samples=%d: resync_calls=%.2f resync_draw=%.2f resync_total=%.2f max_draw=%.2f | "
            "ref_c3d_statechange_total=%.2f extra=%.2f | per draw in block: raw+unif=%.2f c3d+unif=%.2f | "
            "crc_mix=%08lX crc_ref=%08lX crc_eq=%d",
            mix.resyncDraw.n, statMean(&mix.resyncCalls), statMean(&mix.resyncDraw), mixTotal,
            mix.resyncDraw.max, refTotal, mixTotal - refTotal, statMean(&mix.blockPerDraw),
            statMean(&ref.blockPerDraw), (unsigned long)crcMix, (unsigned long)crcRef,
            (crcMix == crcRef && crcMix != 0) ? 1 : 0);
}

/* ---- T7 REPLAY ------------------------------------------------------------------------------ */

typedef struct {
    s8 tex;
    u8 livery;
} ReplayRec;

typedef struct {
    u16 pos[R_SCENE_DRAWS][16]; /* span-relative word index of modelView word k of draw d */
    int draws;
} MvPatchMap;

static ReplayRec sRec[R_SCENE_DRAWS];
static u32 sSpanA[SPAN_MAX_WORDS];
static u32 sSpanD[SPAN_MAX_WORDS];
static u32 sSpanWords;
static u32 sSpanDWords;
static MvPatchMap sMvMap;
static bool sSpanReady;

/* Walks a PICA command span ([param0, header] + extra params, padded to 8 bytes) and records,
 * for every float-uniform upload that covers modelView, where each of its 16 words sits. A new
 * draw starts at each upload of modelView word 0. Handles C3D's merged runs (the first draw
 * uploads projection..livery as one run) and GPUCMD_Add's 256-word splits. */
static bool mapModelViewWords(const u32* span, u32 words, MvPatchMap* map) {
    int runLoc = -1;
    u32 runWord = 0;
    int cur = -1;
    u32 i = 0;
    map->draws = 0;
    while (i + 1 < words) {
        const u32 hdr = span[i + 1];
        const u32 reg = hdr & 0x3FF;
        const u32 extra = (hdr >> 20) & 0xFF;
        const bool incremental = (hdr >> 31) != 0;
        const u32 size = (2 + extra + 1) & ~1u;
        if (reg == GPUREG_VSH_FLOATUNIFORM_CONFIG) {
            if (extra == 0 && (span[i] & 0x80000000u) != 0) {
                runLoc = (int)(span[i] & 0xFF);
                runWord = 0;
            } else {
                runLoc = -1;
            }
        } else if (reg == GPUREG_VSH_FLOATUNIFORM_DATA && !incremental && runLoc >= 0) {
            for (u32 n = 0; n <= extra; n++) {
                const u32 at = (n == 0) ? i : i + 1 + n;
                const int vec = runLoc + (int)(runWord / 4);
                const int comp = (int)(runWord % 4);
                if (vec >= sLocMv && vec < sLocMv + 4) {
                    if (vec == sLocMv && comp == 0) {
                        cur = map->draws++;
                        if (cur >= R_SCENE_DRAWS) {
                            return false;
                        }
                    }
                    if (cur >= 0) {
                        map->pos[cur][(vec - sLocMv) * 4 + comp] = (u16)at;
                    }
                }
                runWord++;
            }
        }
        i += size;
    }
    return map->draws == R_SCENE_DRAWS && i == words;
}

/* Frame B, flavour C: copy A's span into this frame's command buffer, patch modelView from
 * matrix set `set`, and close with the framebuffer flush C3D would have emitted (its DrawUsed
 * flag is not set by raw words). Returns the copied span inside the command buffer. */
static u32* replaySpan(int set, double* copyUs, double* patchUs) {
    const u64 t0 = nowTicks();
    u32* w = cmdReserve(sSpanWords + 6);
    if (w == NULL) {
        return NULL;
    }
    memcpy(w, sSpanA, sSpanWords * 4);
    const u64 t1 = nowTicks();
    for (int d = 0; d < sMvMap.draws; d++) {
        const u32* src = (const u32*)sRMtx[set][d].m;
        const u16* pos = sMvMap.pos[d];
        for (int k = 0; k < 16; k++) {
            w[pos[k]] = src[k];
        }
    }
    u32* tail = w + sSpanWords;
    tail[0] = 1;
    tail[1] = GPUCMD_HEADER(0, 0xF, GPUREG_FRAMEBUFFER_FLUSH);
    tail[2] = 1;
    tail[3] = GPUCMD_HEADER(0, 0xF, GPUREG_FRAMEBUFFER_INVALIDATE);
    tail[4] = 1;
    tail[5] = GPUCMD_HEADER(0, 0xF, GPUREG_EARLYDEPTH_CLEAR);
    const u64 t2 = nowTicks();
    if (copyUs != NULL) {
        *copyUs = ticksToUs(t1 - t0);
    }
    if (patchUs != NULL) {
        *patchUs = ticksToUs(t2 - t1);
    }
    return w;
}

typedef struct {
    UsStat directA;
    UsStat saveA;
    UsStat directD;
    UsStat reissue;
    UsStat cmdbuf;
    UsStat copy;
    UsStat patch;
    int crcReissueEq;
    int crcCmdbufEq;
    int wordsEq;
    int crcADiffer;
    int reps;
    u32 crcD;
} T7Stats;

/* Frame A: C3D draws with matrix set 0, recording the re-issue array; saves the span. */
static bool t7FrameA(T7Stats* st) {
    if (!beginRenderFrame(0, true)) {
        return false;
    }
    const u32 start = gpuCmdBufOffset;
    const u64 t0 = nowTicks();
    for (int i = 0; i < R_SCENE_DRAWS; i++) {
        sRec[i].tex = (s8)((i % R_BIND_EVERY == 0) ? (i / R_BIND_EVERY) % R_TEX_COUNT : -1);
        sRec[i].livery = (u8)i;
        bindSceneTexture(i);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, &sRMtx[0][i]);
        setRenderLivery(i);
        renderDrawC3d();
    }
    statAdd(&st->directA, usSince(t0));
    const u32 words = gpuCmdBufOffset - start;
    bool ok = words <= SPAN_MAX_WORDS;
    if (ok) {
        const u64 ts = nowTicks();
        memcpy(sSpanA, gpuCmdBuf + start, words * 4);
        statAdd(&st->saveA, usSince(ts));
        sSpanWords = words;
    }
    C3D_FrameEnd(0);
    return ok;
}

/* Direct reference: C3D draws with matrix set 1; saves its span for the word compare. */
static bool t7FrameDirect(T7Stats* st) {
    if (!beginRenderFrame(0, true)) {
        return false;
    }
    const u32 start = gpuCmdBufOffset;
    const u64 t0 = nowTicks();
    drawSceneC3d(1);
    statAdd(&st->directD, usSince(t0));
    const u32 words = gpuCmdBufOffset - start;
    const bool ok = words <= SPAN_MAX_WORDS;
    if (ok) {
        memcpy(sSpanD, gpuCmdBuf + start, words * 4);
        sSpanDWords = words;
    }
    C3D_FrameEnd(0);
    return ok;
}

/* Frame B, flavour R: re-issue the recorded array through citro3d with matrix set 1. */
static bool t7FrameReissue(T7Stats* st) {
    if (!beginRenderFrame(0, true)) {
        return false;
    }
    const u64 t0 = nowTicks();
    for (int i = 0; i < R_SCENE_DRAWS; i++) {
        if (sRec[i].tex >= 0) {
            C3D_TexBind(0, &sRTex[(int)sRec[i].tex]);
        }
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, &sRMtx[1][i]);
        setRenderLivery(sRec[i].livery);
        renderDrawC3d();
    }
    statAdd(&st->reissue, usSince(t0));
    C3D_FrameEnd(0);
    return true;
}

static bool t7FrameCmdbuf(T7Stats* st) {
    if (!beginRenderFrame(0, false)) {
        return false;
    }
    const u64 t0 = nowTicks();
    double copyUs = 0.0;
    double patchUs = 0.0;
    const u32* w = replaySpan(1, &copyUs, &patchUs);
    const double us = usSince(t0);
    if (w != NULL) {
        statAdd(&st->cmdbuf, us);
        statAdd(&st->copy, copyUs);
        statAdd(&st->patch, patchUs);
        if (sSpanDWords == sSpanWords && memcmp(w, sSpanD, sSpanWords * 4) == 0) {
            st->wordsEq++;
        }
    }
    C3D_FrameEnd(0);
    return w != NULL;
}

static bool t7Rep(T7Stats* st) {
    if (!t7FrameA(st)) {
        return false;
    }
    const u32 crcA = readbackCrc();
    if (!mapModelViewWords(sSpanA, sSpanWords, &sMvMap)) {
        return false;
    }
    sSpanReady = true;
    if (!t7FrameDirect(st)) {
        return false;
    }
    const u32 crcD = readbackCrc();
    if (!t7FrameReissue(st)) {
        return false;
    }
    const u32 crcReissue = readbackCrc();
    if (!t7FrameCmdbuf(st)) {
        return false;
    }
    const u32 crcCmdbuf = readbackCrc();
    st->crcD = crcD;
    st->crcReissueEq += (crcReissue == crcD) ? 1 : 0;
    st->crcCmdbufEq += (crcCmdbuf == crcD) ? 1 : 0;
    st->crcADiffer += (crcA != crcD) ? 1 : 0;
    st->reps++;
    return true;
}

static void testReplay(void) {
    T7Stats st;
    memset(&st, 0, sizeof(st));
    for (int r = 0; r < T7_REPS; r++) {
        if (!t7Rep(&st)) {
            logLine("[r60] T7 FAIL rep=%d span_words=%lu mv_draws_mapped=%d (span>%d or parse mismatch or "
                    "frame_begin)",
                    r, (unsigned long)sSpanWords, sMvMap.draws, SPAN_MAX_WORDS);
            sSpanReady = sSpanReady && sMvMap.draws == R_SCENE_DRAWS;
            return;
        }
    }
    const double per80 = 80.0 / R_SCENE_DRAWS / 1000.0;
    logLine("[r60] T7 replay us draws=%d binds=%d span_words=%lu reps=%d: p_direct=%.1f (A=%.1f) "
            "p_reissue_us=%.1f p_cmdbuf_us=%.1f (copy=%.1f patch=%.1f) saveA=%.1f | ms@80: reissue=%.3f "
            "cmdbuf=%.3f | crc_reissue_eq=%d/%d crc_cmdbuf_eq=%d/%d words_eq=%d/%d crcA_ne_D=%d/%d crcD=%08lX",
            R_SCENE_DRAWS, R_SCENE_DRAWS / R_BIND_EVERY + 1, (unsigned long)sSpanWords, st.reps,
            statMean(&st.directD), statMean(&st.directA), statMean(&st.reissue), statMean(&st.cmdbuf),
            statMean(&st.copy), statMean(&st.patch), statMean(&st.saveA), statMean(&st.reissue) * per80,
            statMean(&st.cmdbuf) * per80, st.crcReissueEq, st.reps, st.crcCmdbufEq, st.reps, st.wordsEq,
            st.reps, st.crcADiffer, st.reps, (unsigned long)st.crcD);
}

/* ---- T8 CADENCE ----------------------------------------------------------------------------- */

static volatile u32 sPpfCount;
static u32 sPpfVbl[T8_FRAMES];

/* GSP event thread: the frame's display transfer finished; citro3d's queue-finish handler has
 * already requested the swap, which the display latches at the next vblank. */
static void onPresentTransferDone(void* arg) {
    (void)arg;
    const u32 n = sPpfCount;
    if (n < T8_FRAMES) {
        sPpfVbl[n] = C3D_FrameCounter(0);
        sPpfCount = n + 1;
    }
}

typedef enum { CAD_SYNCDRAW, CAD_FREE, CAD_PACED } CadenceMode;

static const char* const kCadenceNames[] = {"syncdraw", "free", "paced"};

static void spinUntil(u64 deadline) {
    while (nowTicks() < deadline) {
    }
}

/* Paced mode: hold B's submission until A (present `index`) has been latched by a vblank, so
 * B's display transfer cannot land in the buffer still being scanned out. FrameBegin's queue
 * wait can return before the PPF callback ran, hence the first wait (bounded, 100 ms). */
static void waitForFlipAfterPresent(u32 index) {
    const u64 deadline = nowTicks() + (u64)100 * (SYSCLOCK_ARM11 / 1000u);
    while (sPpfCount <= index && nowTicks() < deadline) {
        svcSleepThread(100000LL);
    }
    if (sPpfCount <= index) {
        return;
    }
    const u32 presentedAt = sPpfVbl[index];
    while (C3D_FrameCounter(0) == presentedAt && nowTicks() < deadline) {
        svcSleepThread(200000LL);
    }
}

static bool cadenceFrameA(u8 flags, double* cpuUs) {
    if (!C3D_FrameBegin(flags)) {
        return false;
    }
    const u64 t0 = nowTicks();
    spinUntil(t0 + (u64)T8_SPIN_US * (SYSCLOCK_ARM11 / 1000000u));
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, R_CLEAR_COLOR, 0);
    C3D_FrameDrawOn(sTop);
    forceDirtyAll();
    applyRenderBase();
    drawSceneC3d(0);
    *cpuUs = usSince(t0);
    C3D_FrameEnd(0);
    return true;
}

static bool cadenceFrameB(u8 flags, bool paced, u32 frameIndex, double* cpuUs) {
    if (!beginRenderFrame(flags, !sSpanReady)) {
        return false;
    }
    const u64 t0 = nowTicks();
    if (sSpanReady) {
        replaySpan(1, NULL, NULL);
    } else {
        for (int i = 0; i < 4; i++) {
            bindSceneTexture(i * R_BIND_EVERY);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLocMv, &sRMtx[1][i]);
            renderDrawC3d();
        }
    }
    *cpuUs = usSince(t0);
    if (paced && frameIndex > 0) {
        waitForFlipAfterPresent(frameIndex - 1);
    }
    C3D_FrameEnd(0);
    return true;
}

static void logCadenceSequence(CadenceMode mode, u32 presents) {
    char digits[T8_SEQ_PER_LINE + 1];
    for (u32 from = 1; from < presents; from += T8_SEQ_PER_LINE) {
        u32 n = 0;
        for (u32 k = from; k < presents && n < T8_SEQ_PER_LINE; k++) {
            const u32 d = sPpfVbl[k] - sPpfVbl[k - 1];
            digits[n++] = (char)('0' + (d > 9 ? 9 : d));
        }
        digits[n] = '\0';
        logLine("[r60] T8 seq mode=%s from=%lu %s", kCadenceNames[mode], (unsigned long)from, digits);
    }
}

static bool runCadence(CadenceMode mode) {
    const u8 flags = (mode == CAD_SYNCDRAW) ? C3D_FRAME_SYNCDRAW : 0;
    UsStat aCpu = {0};
    UsStat bCpu = {0};
    C3Di_RenderQueueWaitDone();
    sPpfCount = 0;
    gspSetEventCallback(GSPGPU_EVENT_PPF, onPresentTransferDone, NULL, false);
    const u64 start = nowTicks();
    int frames = 0;
    bool aborted = false;
    bool failed = false;
    for (; frames < T8_FRAMES; frames++) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            aborted = true;
            break;
        }
        double us = 0.0;
        if ((frames & 1) == 0) {
            failed = !cadenceFrameA(flags, &us);
            statAdd(&aCpu, us);
        } else {
            failed = !cadenceFrameB(flags, mode == CAD_PACED, (u32)frames, &us);
            statAdd(&bCpu, us);
        }
        if (failed) {
            break;
        }
    }
    C3Di_RenderQueueWaitDone();
    svcSleepThread(50000000LL); /* let the last PPF callback run */
    gspSetEventCallback(GSPGPU_EVENT_PPF, NULL, NULL, false);
    const double wallMs = usSince(start) / 1000.0;
    const u32 presents = sPpfCount;
    int hist[4] = {0, 0, 0, 0};
    u32 repeated = 0;
    for (u32 k = 1; k < presents; k++) {
        const u32 d = sPpfVbl[k] - sPpfVbl[k - 1];
        hist[d > 3 ? 3 : d]++;
        if (d > 1) {
            repeated += d - 1;
        }
    }
    const u32 spanVbl = presents > 1 ? sPpfVbl[presents - 1] - sPpfVbl[0] : 0;
    logLine("[r60] T8 cadence mode=%s frames=%d presents=%lu span_vbl=%lu present_hz=%.2f wall_ms=%.0f | "
            "delta0(dropped/overwritten)=%d delta1=%d delta2=%d delta3+=%d repeated_vbl=%lu | a_cpu_ms=%.2f "
            "b_cpu_us=%.1f b_kind=%s%s%s",
            kCadenceNames[mode], frames, (unsigned long)presents, (unsigned long)spanVbl,
            spanVbl > 0 ? 59.83 * (double)(presents - 1) / (double)spanVbl : 0.0, wallMs, hist[0], hist[1],
            hist[2], hist[3], (unsigned long)repeated, statMean(&aCpu) / 1000.0, statMean(&bCpu),
            sSpanReady ? "cmdbuf_replay60" : "c3d_draws4", aborted ? " ABORTED" : "",
            failed ? " FAIL_frame_begin" : "");
    logCadenceSequence(mode, presents);
    return !aborted && !failed;
}

static void testCadence(void) {
    static const CadenceMode kModes[] = {CAD_SYNCDRAW, CAD_FREE, CAD_PACED};
    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++) {
        if (!runCadence(kModes[i])) {
            break;
        }
    }
}

static void runRenderTests(void) {
    sRenderOk = renderInit();
    logLine("[r60] R init ok=%d linear_heap=0x%08lX size_mb=%.1f linear_free_k=%lu mv_loc=%d livery_loc=%d",
            sRenderOk ? 1 : 0, (unsigned long)__ctru_linear_heap,
            (double)__ctru_linear_heap_size / (1024.0 * 1024.0), (unsigned long)(linearSpaceFree() / 1024u),
            sLocMv, sLocLivery);
    if (!sRenderOk) {
        logLine("[r60] FLUSH SKIP reason=render_init");
        logLine("[r60] RESYNC SKIP reason=render_init");
        logLine("[r60] T7 SKIP reason=render_init");
        logLine("[r60] T8 SKIP reason=render_init");
        return;
    }
    testFlush();
    testResync();
    testReplay();
    testCadence();
    C3Di_RenderQueueWaitDone();
}

/* ---- main ------------------------------------------------------------------------------------- */

static u32* sCpuCmdBuf;

/* Sections A and P build commands outside any frame. citro3d only binds its command buffer in
 * C3D_FrameBegin, and GPUCMD_Add* svcBreak()s on a NULL gpuCmdBuf, so bind a scratch buffer
 * of our own for them (researcher F; the first committed version panicked at the first A draw). */
static bool allocMemory(void) {
    sHeapMem = (u32*)memalign(32, CHASE_MAX_BYTES);
    sLinearMem = (u32*)linearAlloc(CHASE_MAX_BYTES);
    sVramMem = (u32*)vramAlloc(1u << 20);
    sCpuCmdBuf = (u32*)linearAlloc(CMDBUF_BYTES);
    if (sCpuCmdBuf != NULL) {
        GPUCMD_SetBuffer(sCpuCmdBuf, CMDBUF_BYTES / 4, 0);
    }
    return sHeapMem != NULL && sLinearMem != NULL && sCpuCmdBuf != NULL;
}

/* ---- X: what a PC sampler would need (hwprof; opt-in, after everything is logged) -------------
 * X1 svcGetThreadContext (svc 0x3B) on the main thread: the only non-debug way to read another
 * thread's PC. 3dbrew lists it as a stub on retail kernels; the rc says. Harmless.
 * X2 a user-mode read of the CP15 cycle counter (MRC p15,0,Rd,c15,c12,1). If the ARM11 MPCore's
 * performance monitor is privileged-only (DDI0360: no user-access bit on this core), this is an
 * UNDEFINED INSTRUCTION: Luma's exception screen appears and the probe dies. The line before it
 * is in the log either way, so the log answers the question. Run it last, on purpose. */
static void testThreadContext(void) {
    u32 ctx[64];
    memset(ctx, 0, sizeof(ctx));
    Handle self = 0;
    u32 rc = 0;
    if (R_SUCCEEDED(svcDuplicateHandle(&self, CUR_THREAD_HANDLE))) {
        register u32 r0 __asm__("r0") = (u32)ctx;
        register u32 r1 __asm__("r1") = (u32)self;
        __asm__ volatile("svc 0x3B" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
        rc = r0;
        svcCloseHandle(self);
    }
    logLine("[r60] X1 svcGetThreadContext(self) rc=0x%08lX (0xF8C007F4 = not implemented: no PC sampler without a "
            "debugger)", (unsigned long)rc);
}

static void testUserCcnt(void) {
    logLine("[r60] X2 attempting a user-mode MRC p15 c15 c12 1 (CCNT). If the next X2 line is missing and Luma "
            "showed an exception screen: the ARM11 perfmon is privileged-only on this console.");
    u32 a = 0;
    u32 b = 0;
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 1" : "=r"(a));
    sSink += readSum(sHeapMem, 4096);
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 1" : "=r"(b));
    logLine("[r60] X2 user-mode CCNT readable: a=%lu b=%lu delta=%lu", (unsigned long)a, (unsigned long)b,
            (unsigned long)(b - a));
}

static void waitForStart(void) {
    printf("\nDONE - press START to exit\n");
    printf("Optional (hwprof): Y = svcGetThreadContext test,\n  then X = user-mode CCNT read (MAY CRASH)\n");
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (down & KEY_Y) {
            testThreadContext();
        }
        if (down & KEY_X) {
            testUserCcnt();
        }
        gspWaitForVBlank();
    }
}

int main(void) {
    const u64 boot = nowTicks();
    aptSetHomeAllowed(false);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    openLog();
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    u32 pct = 0;
    APT_GetAppCpuTimeLimit(&pct);
    u32 fpscr = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    logLine("[r60] HDR r60probe build=%s is_new3ds=%d hb=%d fpscr=0x%08lX syscore_at_boot=%lu%%", R60PROBE_BUILD_ID,
            isNew ? 1 : 0, envIsHomebrew() ? 1 : 0, (unsigned long)fpscr, (unsigned long)pct);
    const bool c3dOk = C3D_Init(CMDBUF_BYTES);
    if (!c3dOk || !allocMemory() || !buildMesh()) {
        logLine("[r60] FATAL init c3d=%d heap=%p linear=%p cmdbuf=%p", c3dOk ? 1 : 0, (void*)sHeapMem,
                (void*)sLinearMem, (void*)sCpuCmdBuf);
    } else {
        buildMtxInputs();
        buildXfInputs();
        testDraws();
        testICache();
        testMath();
        testSvc();
        testMemory();
        testHwprof();
        testPmu();
        GPUCMD_SetBuffer(NULL, 0, 0);
        testCore1();
        runRenderTests();
    }
    logLine("[r60] END total_ms=%.0f", ticksToUs(nowTicks() - boot) / 1000.0);
    waitForStart();
    if (sLog != NULL) {
        fclose(sLog);
    }
    if (c3dOk) {
        C3D_Fini();
    }
    gfxExit();
    return 0;
}
