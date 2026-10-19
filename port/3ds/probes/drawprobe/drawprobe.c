/* port/3ds/probes/drawprobe/drawprobe.c -- PROBE, NOT PART OF THE GAME BUILD.
 *
 * o3ds60 design input: MEASURED costs of citro3d submission on the 3DS ARM11 and the PICA200,
 * so the retained-mode native race renderer can be budgeted for 60 fps on the Old 3DS.
 * Standalone: its own CMakeLists.txt, title ID 000400000FF3DA00, never linked into the game.
 *
 *   T0  empty frame (clear only): the baseline every GPU number is taken net of
 *   T1  N draws of one static 64-tri VBO mesh, no state change      -> CPU us per draw
 *   T2  + modelview 4x4 and 3 livery vec4 per draw, split three ways: setting the uniforms,
 *       flushing them (C3D_UpdateUniforms) and the draw itself       -> us per part
 *   T3  100 draws, each with a TexEnv constant change / a texture switch among 8 /
 *       an attr+buf switch among 4 VBOs / all three                  -> us per change
 *   T4  30 machine-like meshes (400 and 1500 tris, float and short formats) -> GPU ms,
 *       Mverts/s, Mtris/s; FILL: K full-screen alpha-blended textured quads -> ms per layer
 *   T5  CPU matrix math for 30 machines (hand-written and citro3d), and a streaming VBO
 *       (400 sprites x 4 verts written, cache-flushed and drawn every frame)
 *   T6  the largest command-buffer use seen
 *
 * Timing: CPU with svcGetSystemTick (SYSCLOCK_ARM11 = 268,111,856 Hz on every model), GPU
 * with C3D_GetDrawingTime/C3D_GetProcessingTime, averaged over MEASURE_FRAMES frames after
 * WARMUP_FRAMES. One C3D_FrameBegin/End per frame, top screen 400x240, stereo off.
 *
 * Output: the bottom-screen console and sdmc:/3ds/drawprobe/drawprobe.txt (appended), one
 * `[dp] ...` line per test. Touches nothing of the game's (save, ini, log). START aborts
 * or exits; HOME is refused for the whole run (it freezes the Old 3DS, NEXT.md C4j).
 *
 * DRAWPROBE_LANE_LOG (emulator build only): also writes every line plus a [watchdog]
 * heartbeat to sdmc:/3ds/gdiffuser/log.txt so `gdx-dev retro run` recognises a boot, and
 * quits by itself at the end with the lane's "[oracle] quit requested" receipt. */
#include <3ds.h>
#include <citro3d.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "drawprobe_shader_shbin.h"

#ifndef DRAWPROBE_BUILD_ID
#define DRAWPROBE_BUILD_ID "unknown"
#endif

#define DISPLAY_TRANSFER_FLAGS                                                                      \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TICKS_PER_US ((double)SYSCLOCK_ARM11 / 1000000.0)
#define TICKS_PER_MS ((double)SYSCLOCK_ARM11 / 1000.0)
#define CMDBUF_BYTES 0x80000
#define WARMUP_FRAMES 10
#define MEASURE_FRAMES 100
#define TEST_WALL_CAP_MS 5000
#define LANE_BEAT_FRAMES 60
#define SCREEN_W 400.0f
#define SCREEN_H 240.0f
#define CLEAR_COLOR 0x202830FF

#define MACHINE_COUNT 30
#define GRID_COLS 6
#define GRID_ROWS 5
#define MAX_DRAWS 300
#define TEX_COUNT 8
#define TEX_SIZE 32
#define FILL_TEX_SIZE 64
#define SWITCH_MESH_COUNT 4
#define SPRITE_COUNT 400
#define SPRITE_SIZE 8.0f
#define T3_DRAWS 100
#define SUB_TIMERS 4

#define SHORT_POS_ONE 4096.0f
#define BYTE_NRM_ONE 127.0f
#define SHORT_UV_ONE 1024.0f

/* ---- data --------------------------------------------------------------------------- */

typedef struct {
    float pos[3];
    float nrm[3];
    float uv[2];
} VertexF;

typedef struct {
    s16 pos[4];
    s8 nrm[4];
    s16 uv[2];
} VertexS;

typedef struct {
    void* vbo;
    u16* ibo;
    int vertexCount;
    int indexCount;
    bool shortFormat;
    C3D_BufInfo bufInfo;
} Mesh;

typedef struct {
    int projection;
    int modelView;
    int lightDir;
    int lightAmb;
    int scales;
    int livery;
} UniformLocs;

typedef struct {
    u64 sub[SUB_TIMERS];
    u64 submit;
    u64 framePeriods;
    int periodCount;
    double gpuDrawMs;
    double gpuProcMs;
    int frames;
    float cmdbufMax;
} Accum;

typedef struct {
    int frames;
    double subUs[SUB_TIMERS];
    double submitUs;
    double gpuMs;
    double procMs;
    double frameMs;
    float cmdbufMax;
} ProbeResult;

typedef enum {
    T3_TEXENV = 1,
    T3_TEXTURE = 2,
    T3_VBO = 4,
} T3Mode;

typedef struct TestSpec TestSpec;
typedef bool (*ReadyFn)(const TestSpec* spec);
typedef void (*SubmitFn)(const TestSpec* spec, Accum* acc, u32 frame);
typedef void (*ReportFn)(const TestSpec* spec, const ProbeResult* res);

struct TestSpec {
    const char* name;
    ReadyFn ready;
    SubmitFn submit;
    ReportFn report;
    int n;
    int mode;
    bool vsync;
};

static C3D_RenderTarget* sTop;
static DVLB_s* sShaderDvlb;
static shaderProgram_s sProgram;
static UniformLocs sLoc;
static C3D_AttrInfo sAttrFloat;
static C3D_AttrInfo sAttrShort;
static C3D_Mtx sProjection;
static C3D_Mtx sCenterMtx;
static C3D_Mtx sIdentityMtx;
static C3D_Mtx sDrawMtx[MAX_DRAWS];
static float sLivery[MAX_DRAWS][3][4];

static Mesh sSmallMesh;
static Mesh sSwitchMesh[SWITCH_MESH_COUNT];
static Mesh sMesh400;
static Mesh sMesh1500;
static Mesh sMesh1500Short;
static Mesh sQuadMesh;
static VertexF* sSpriteVbo[2];
static u16* sSpriteIbo;
static C3D_Tex sTex[TEX_COUNT];
static bool sTexOk[TEX_COUNT];
static C3D_Tex sFillTex;
static bool sFillTexOk;

/* T5 inputs, mutated every frame so the compiler cannot fold the math away. */
static float sHandView[16];
static float sHandModel[MACHINE_COUNT][16];
static C3D_Mtx sC3dView;
static C3D_Mtx sC3dModel[MACHINE_COUNT];

static FILE* sLog;
static FILE* sLaneLog;
static u64 sBootTick;
static u32 sGlobalFrame;
static u32 sLastBeatFrame;
static int sBeat;
static bool sAbort;

static double sBaselineGpuMs;
static double sT1PerDrawUs100 = -1.0;
static float sCmdbufMaxAll;
static const char* sCmdbufMaxTest = "none";

/* ---- log ---------------------------------------------------------------------------- */

static unsigned long msSince(u64 tick) {
    return (unsigned long)((svcGetSystemTick() - tick) / TICKS_PER_MS);
}

static void writeLine(FILE* file, const char* line) {
    if (file != NULL) {
        fprintf(file, "%s\n", line);
        fflush(file);
    }
}

static void logLine(const char* fmt, ...) {
    char line[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    printf("%s\n", line);
    writeLine(sLog, line);
    writeLine(sLaneLog, line);
}

static void openLogs(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/drawprobe", 0777);
    sLog = fopen("sdmc:/3ds/drawprobe/drawprobe.txt", "a");
#ifdef DRAWPROBE_LANE_LOG
    mkdir("sdmc:/3ds/gdiffuser", 0777);
    sLaneLog = fopen("sdmc:/3ds/gdiffuser/log.txt", "w");
#endif
}

static void closeLogs(void) {
    if (sLog != NULL) {
        fclose(sLog);
        sLog = NULL;
    }
    if (sLaneLog != NULL) {
        fclose(sLaneLog);
        sLaneLog = NULL;
    }
}

/* The retro lane's boot check wants an ADVANCING `[watchdog] frame=N(+d)` heartbeat. */
static void laneHeartbeat(void) {
    if (sLaneLog == NULL) {
        return;
    }
    if (sGlobalFrame - sLastBeatFrame < LANE_BEAT_FRAMES) {
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "[watchdog] beat=%d frame=%lu(+%lu)", ++sBeat,
             (unsigned long)sGlobalFrame, (unsigned long)(sGlobalFrame - sLastBeatFrame));
    writeLine(sLaneLog, line);
    sLastBeatFrame = sGlobalFrame;
}

/* ---- meshes --------------------------------------------------------------------------- */

static void freeMesh(Mesh* mesh) {
    if (mesh->vbo != NULL) {
        linearFree(mesh->vbo);
        mesh->vbo = NULL;
    }
    if (mesh->ibo != NULL) {
        linearFree(mesh->ibo);
        mesh->ibo = NULL;
    }
}

static bool meshReady(const Mesh* mesh) {
    return mesh->vbo != NULL && mesh->ibo != NULL;
}

static size_t meshStride(bool shortFormat) {
    return shortFormat ? sizeof(VertexS) : sizeof(VertexF);
}

static bool allocMesh(Mesh* mesh, int vertexCount, int indexCount, bool shortFormat) {
    memset(mesh, 0, sizeof(*mesh));
    mesh->vbo = linearAlloc((size_t)vertexCount * meshStride(shortFormat));
    mesh->ibo = (u16*)linearAlloc((size_t)indexCount * sizeof(u16));
    if (!meshReady(mesh)) {
        freeMesh(mesh);
        return false;
    }
    mesh->vertexCount = vertexCount;
    mesh->indexCount = indexCount;
    mesh->shortFormat = shortFormat;
    return true;
}

static void writeVertex(Mesh* mesh, int index, const float pos[3], const float nrm[3],
                        const float uv[2]) {
    if (mesh->shortFormat) {
        VertexS* v = &((VertexS*)mesh->vbo)[index];
        for (int c = 0; c < 3; c++) {
            v->pos[c] = (s16)lroundf(pos[c] * SHORT_POS_ONE);
            v->nrm[c] = (s8)lroundf(nrm[c] * BYTE_NRM_ONE);
        }
        v->pos[3] = 0;
        v->nrm[3] = 0;
        v->uv[0] = (s16)lroundf(uv[0] * SHORT_UV_ONE);
        v->uv[1] = (s16)lroundf(uv[1] * SHORT_UV_ONE);
    } else {
        VertexF* v = &((VertexF*)mesh->vbo)[index];
        memcpy(v->pos, pos, sizeof(v->pos));
        memcpy(v->nrm, nrm, sizeof(v->nrm));
        memcpy(v->uv, uv, sizeof(v->uv));
    }
}

static void finishMesh(Mesh* mesh) {
    GSPGPU_FlushDataCache(mesh->vbo, (u32)(mesh->vertexCount * meshStride(mesh->shortFormat)));
    GSPGPU_FlushDataCache(mesh->ibo, (u32)(mesh->indexCount * sizeof(u16)));
    BufInfo_Init(&mesh->bufInfo);
    BufInfo_Add(&mesh->bufInfo, mesh->vbo, (int)meshStride(mesh->shortFormat), 3, 0x210);
}

static void writeSphereVertices(Mesh* mesh, int stacks, int slices) {
    for (int st = 0; st <= stacks; st++) {
        const float theta = (float)M_PI * (float)st / (float)stacks;
        for (int sl = 0; sl <= slices; sl++) {
            const float phi = 2.0f * (float)M_PI * (float)sl / (float)slices;
            const float n[3] = {sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi)};
            const float uv[2] = {(float)sl / (float)slices, (float)st / (float)stacks};
            writeVertex(mesh, st * (slices + 1) + sl, n, n, uv);
        }
    }
}

static void writeSphereIndices(Mesh* mesh, int stacks, int slices) {
    int k = 0;
    for (int st = 0; st < stacks; st++) {
        for (int sl = 0; sl < slices; sl++) {
            const u16 a = (u16)(st * (slices + 1) + sl);
            const u16 b = (u16)(a + slices + 1);
            mesh->ibo[k++] = a;
            mesh->ibo[k++] = b;
            mesh->ibo[k++] = (u16)(a + 1);
            mesh->ibo[k++] = (u16)(a + 1);
            mesh->ibo[k++] = b;
            mesh->ibo[k++] = (u16)(b + 1);
        }
    }
}

/* A unit UV sphere of stacks*slices*2 triangles (the pole rows included). */
static bool buildSphereMesh(Mesh* mesh, int stacks, int slices, bool shortFormat) {
    const int vertexCount = (stacks + 1) * (slices + 1);
    if (!allocMesh(mesh, vertexCount, stacks * slices * 6, shortFormat)) {
        return false;
    }
    writeSphereVertices(mesh, stacks, slices);
    writeSphereIndices(mesh, stacks, slices);
    finishMesh(mesh);
    return true;
}

static bool buildFullscreenQuad(Mesh* mesh) {
    if (!allocMesh(mesh, 4, 6, false)) {
        return false;
    }
    const float nrm[3] = {0.0f, 0.0f, 1.0f};
    const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (int i = 0; i < 4; i++) {
        const float pos[3] = {corners[i][0] * SCREEN_W, corners[i][1] * SCREEN_H, 0.0f};
        writeVertex(mesh, i, pos, nrm, corners[i]);
    }
    const u16 indices[6] = {0, 1, 2, 2, 1, 3};
    memcpy(mesh->ibo, indices, sizeof(indices));
    finishMesh(mesh);
    return true;
}

static bool buildSpriteBuffers(void) {
    const size_t vboBytes = SPRITE_COUNT * 4 * sizeof(VertexF);
    sSpriteVbo[0] = (VertexF*)linearAlloc(vboBytes);
    sSpriteVbo[1] = (VertexF*)linearAlloc(vboBytes);
    sSpriteIbo = (u16*)linearAlloc(SPRITE_COUNT * 6 * sizeof(u16));
    if (sSpriteVbo[0] == NULL || sSpriteVbo[1] == NULL || sSpriteIbo == NULL) {
        return false;
    }
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const u16 base = (u16)(i * 4);
        const u16 quad[6] = {base, (u16)(base + 1), (u16)(base + 2),
                             (u16)(base + 2), (u16)(base + 1), (u16)(base + 3)};
        memcpy(&sSpriteIbo[i * 6], quad, sizeof(quad));
    }
    GSPGPU_FlushDataCache(sSpriteIbo, SPRITE_COUNT * 6 * sizeof(u16));
    return true;
}

static bool spritesReady(void) {
    return sSpriteVbo[0] != NULL && sSpriteVbo[1] != NULL && sSpriteIbo != NULL;
}

/* ---- textures ------------------------------------------------------------------------- */

static bool buildSolidTexture(C3D_Tex* tex, int size, u32 texel) {
    if (!C3D_TexInit(tex, (u16)size, (u16)size, GPU_RGBA8)) {
        return false;
    }
    u32* data = (u32*)tex->data;
    for (int i = 0; i < size * size; i++) {
        data[i] = ((i >> 3) & 1) ? texel : (texel | 0xFF000000u);
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexFlush(tex);
    return true;
}

static void buildTextures(void) {
    static const u32 kTexels[TEX_COUNT] = {0x3040C0FFu, 0x30C040FFu, 0x30C0C0FFu, 0x404040FFu,
                                           0x50A0F0FFu, 0x70F0A0FFu, 0x20F0F0FFu, 0x9060C0FFu};
    for (int i = 0; i < TEX_COUNT; i++) {
        sTexOk[i] = buildSolidTexture(&sTex[i], TEX_SIZE, kTexels[i]);
    }
    sFillTexOk = buildSolidTexture(&sFillTex, FILL_TEX_SIZE, 0x80808080u);
}

static bool texturesReady(void) {
    for (int i = 0; i < TEX_COUNT; i++) {
        if (!sTexOk[i]) {
            return false;
        }
    }
    return true;
}

/* ---- shader, matrices, uniforms ------------------------------------------------------ */

static bool loadShader(void) {
    sShaderDvlb = DVLB_ParseFile((u32*)drawprobe_shader_shbin, drawprobe_shader_shbin_size);
    if (sShaderDvlb == NULL) {
        return false;
    }
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sLoc.projection = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
    sLoc.modelView = shaderInstanceGetUniformLocation(sProgram.vertexShader, "modelView");
    sLoc.lightDir = shaderInstanceGetUniformLocation(sProgram.vertexShader, "lightDir");
    sLoc.lightAmb = shaderInstanceGetUniformLocation(sProgram.vertexShader, "lightAmb");
    sLoc.scales = shaderInstanceGetUniformLocation(sProgram.vertexShader, "scales");
    sLoc.livery = shaderInstanceGetUniformLocation(sProgram.vertexShader, "livery");
    return sLoc.projection >= 0 && sLoc.modelView >= 0 && sLoc.lightDir >= 0 &&
           sLoc.lightAmb >= 0 && sLoc.scales >= 0 && sLoc.livery >= 0;
}

static void buildAttrInfos(void) {
    AttrInfo_Init(&sAttrFloat);
    AttrInfo_AddLoader(&sAttrFloat, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sAttrFloat, 1, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&sAttrFloat, 2, GPU_FLOAT, 2);
    AttrInfo_Init(&sAttrShort);
    AttrInfo_AddLoader(&sAttrShort, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&sAttrShort, 1, GPU_BYTE, 4);
    AttrInfo_AddLoader(&sAttrShort, 2, GPU_SHORT, 2);
}

static void machineMatrix(C3D_Mtx* out, int slot, float radius, float angle) {
    const float cellW = SCREEN_W / GRID_COLS;
    const float cellH = SCREEN_H / GRID_ROWS;
    const int cell = slot % MACHINE_COUNT;
    Mtx_Identity(out);
    Mtx_Translate(out, cellW * ((float)(cell % GRID_COLS) + 0.5f),
                  cellH * ((float)(cell / GRID_COLS) + 0.5f), 0.0f, true);
    Mtx_RotateY(out, angle, true);
    Mtx_RotateX(out, angle * 0.5f, true);
    Mtx_Scale(out, radius, radius, radius);
}

static void buildDrawTables(void) {
    Mtx_OrthoTilt(&sProjection, 0.0f, SCREEN_W, 0.0f, SCREEN_H, -100.0f, 100.0f, true);
    Mtx_Identity(&sIdentityMtx);
    Mtx_Identity(&sCenterMtx);
    Mtx_Translate(&sCenterMtx, SCREEN_W * 0.5f, SCREEN_H * 0.5f, 0.0f, true);
    Mtx_Scale(&sCenterMtx, 10.0f, 10.0f, 10.0f);
    for (int i = 0; i < MAX_DRAWS; i++) {
        machineMatrix(&sDrawMtx[i], i, 12.0f, 0.1f * (float)i);
        for (int c = 0; c < 3; c++) {
            sLivery[i][c][0] = (float)((i * 3 + c * 5) % 7) / 7.0f;
            sLivery[i][c][1] = (float)((i * 5 + c * 3) % 11) / 11.0f;
            sLivery[i][c][2] = (float)((i * 7 + c) % 13) / 13.0f;
            sLivery[i][c][3] = 1.0f;
        }
    }
}

static void setScales(bool shortFormat) {
    if (shortFormat) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.scales, 1.0f / SHORT_POS_ONE, 1.0f / BYTE_NRM_ONE,
                      1.0f / SHORT_UV_ONE, 0.0f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.scales, 1.0f, 1.0f, 1.0f, 0.0f);
    }
}

static void setLivery(int slot) {
    for (int c = 0; c < 3; c++) {
        const float* col = sLivery[slot][c];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.livery + c, col[0], col[1], col[2], col[3]);
    }
}

static void setFlatLivery(float r, float g, float b, float a) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.livery, r, g, b, a);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.livery + 1, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.livery + 2, 0.0f, 0.0f, 0.0f, 0.0f);
}

/* The per-draw machine uniforms: one modelview 4x4 + three livery vec4. */
static void setDrawUniforms(int slot) {
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.modelView, &sDrawMtx[slot]);
    setLivery(slot);
}

static void bindMesh(const Mesh* mesh) {
    C3D_SetAttrInfo(mesh->shortFormat ? &sAttrShort : &sAttrFloat);
    C3D_SetBufInfo((C3D_BufInfo*)&mesh->bufInfo);
    setScales(mesh->shortFormat);
}

static void drawMesh(const Mesh* mesh) {
    C3D_DrawElements(GPU_TRIANGLES, mesh->indexCount, C3D_UNSIGNED_SHORT, mesh->ibo);
}

static void setBlendEnabled(bool enabled) {
    if (enabled) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
}

/* Stage 0: texture * vertex colour. Stage 1: previous * constant (what T3 changes). */
static void setBaseTexEnv(void) {
    C3D_TexEnv* env0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env0);
    C3D_TexEnvSrc(env0, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env0, C3D_Both, GPU_MODULATE);
    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, 0);
    C3D_TexEnvFunc(env1, C3D_Both, GPU_MODULATE);
    C3D_TexEnvColor(env1, 0xFFFFFFFFu);
}

/* Every frame starts from the same state, then one tiny draw flushes it all, so the timed
 * loops that follow measure steady-state submission, not the frame's first-draw flush. */
static void applyBaseState(void) {
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.projection, &sProjection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.modelView, &sCenterMtx);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.lightDir, 0.4f, 0.6f, 0.69f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.lightAmb, 0.3f, 0.3f, 0.3f, 0.0f);
    setFlatLivery(0.9f, 0.5f, 0.2f, 1.0f);
    setBaseTexEnv();
    C3D_TexBind(0, &sTex[0]);
    C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    setBlendEnabled(false);
    bindMesh(&sSmallMesh);
    C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, sSmallMesh.ibo);
}

/* ---- timing ---------------------------------------------------------------------------- */

static double ticksToUs(u64 ticks) {
    return (double)ticks / TICKS_PER_US;
}

static void finishResult(const Accum* acc, ProbeResult* res) {
    const double frames = acc->frames > 0 ? (double)acc->frames : 1.0;
    memset(res, 0, sizeof(*res));
    res->frames = acc->frames;
    for (int i = 0; i < SUB_TIMERS; i++) {
        res->subUs[i] = ticksToUs(acc->sub[i]) / frames;
    }
    res->submitUs = ticksToUs(acc->submit) / frames;
    res->gpuMs = acc->gpuDrawMs / frames;
    res->procMs = acc->gpuProcMs / frames;
    if (acc->periodCount > 0) {
        res->frameMs = (double)acc->framePeriods / TICKS_PER_MS / (double)acc->periodCount;
    }
    res->cmdbufMax = acc->cmdbufMax;
}

static double perItem(double us, int count) {
    return count > 0 ? us / (double)count : 0.0;
}

static double netGpuMs(const ProbeResult* res) {
    return res->gpuMs - sBaselineGpuMs;
}

/* ---- tests: readiness -------------------------------------------------------------------- */

static bool readySmall(const TestSpec* spec) {
    (void)spec;
    return meshReady(&sSmallMesh);
}

static bool readyT3(const TestSpec* spec) {
    if (!meshReady(&sSmallMesh)) {
        return false;
    }
    if ((spec->mode & T3_TEXTURE) && !texturesReady()) {
        return false;
    }
    if (spec->mode & T3_VBO) {
        for (int i = 0; i < SWITCH_MESH_COUNT; i++) {
            if (!meshReady(&sSwitchMesh[i])) {
                return false;
            }
        }
    }
    return true;
}

static const Mesh* t4Mesh(const TestSpec* spec) {
    switch (spec->mode) {
        case 0:
            return &sMesh400;
        case 1:
            return &sMesh1500;
        default:
            return &sMesh1500Short;
    }
}

static const char* t4MeshName(const TestSpec* spec) {
    switch (spec->mode) {
        case 0:
            return "400f";
        case 1:
            return "1500f";
        default:
            return "1500s";
    }
}

static bool readyT4(const TestSpec* spec) {
    return meshReady(t4Mesh(spec));
}

static bool readyFill(const TestSpec* spec) {
    (void)spec;
    return meshReady(&sQuadMesh) && sFillTexOk;
}

static bool readyAlways(const TestSpec* spec) {
    (void)spec;
    return true;
}

static bool readyStream(const TestSpec* spec) {
    (void)spec;
    return spritesReady();
}

/* ---- tests: submission -------------------------------------------------------------------- */

static void submitEmpty(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)spec;
    (void)acc;
    (void)frame;
}

static void submitT1(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)frame;
    const u64 t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        drawMesh(&sSmallMesh);
    }
    acc->sub[0] += svcGetSystemTick() - t0;
}

static void submitT2(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)frame;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        setDrawUniforms(i);
    }
    u64 t1 = svcGetSystemTick();
    acc->sub[0] += t1 - t0;

    t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        setDrawUniforms(i);
        C3D_UpdateUniforms(GPU_VERTEX_SHADER);
    }
    t1 = svcGetSystemTick();
    acc->sub[1] += t1 - t0;

    t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        setDrawUniforms(i);
        drawMesh(&sSmallMesh);
    }
    t1 = svcGetSystemTick();
    acc->sub[2] += t1 - t0;
}

static void applyT3Changes(int mode, int i) {
    static const u32 kConst[8] = {0xFFFFFFFFu, 0xFF8080FFu, 0x80FF80FFu, 0x8080FFFFu,
                                  0xFFFF80FFu, 0x80FFFFFFu, 0xFF80FFFFu, 0xC0C0C0FFu};
    if (mode & T3_TEXENV) {
        C3D_TexEnvColor(C3D_GetTexEnv(1), kConst[i & 7]);
    }
    if (mode & T3_TEXTURE) {
        C3D_TexBind(0, &sTex[i % TEX_COUNT]);
    }
    if (mode & T3_VBO) {
        const Mesh* mesh = &sSwitchMesh[i % SWITCH_MESH_COUNT];
        C3D_SetAttrInfo(&sAttrFloat);
        C3D_SetBufInfo((C3D_BufInfo*)&mesh->bufInfo);
    }
}

static const Mesh* t3DrawMesh(int mode, int i) {
    if (mode & T3_VBO) {
        return &sSwitchMesh[i % SWITCH_MESH_COUNT];
    }
    return &sSmallMesh;
}

static void submitT3(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)frame;
    const u64 t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        applyT3Changes(spec->mode, i);
        drawMesh(t3DrawMesh(spec->mode, i));
    }
    acc->sub[0] += svcGetSystemTick() - t0;
}

static void submitT4(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)frame;
    const Mesh* mesh = t4Mesh(spec);
    const u64 t0 = svcGetSystemTick();
    bindMesh(mesh);
    for (int i = 0; i < spec->n; i++) {
        setDrawUniforms(i);
        drawMesh(mesh);
    }
    acc->sub[0] += svcGetSystemTick() - t0;
}

static void submitFill(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)frame;
    const u64 t0 = svcGetSystemTick();
    bindMesh(&sQuadMesh);
    C3D_TexBind(0, &sFillTex);
    setBlendEnabled(true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.modelView, &sIdentityMtx);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sLoc.lightDir, 0.0f, 0.0f, 1.0f, 0.0f);
    setFlatLivery(1.0f, 1.0f, 1.0f, 0.5f);
    for (int i = 0; i < spec->n; i++) {
        drawMesh(&sQuadMesh);
    }
    acc->sub[0] += svcGetSystemTick() - t0;
}

/* Column-major (GL/N64-style) out = a * b. */
static void handMultiply(float* out, const float* a, const float* b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                                 a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/* Column-major -> the PICA's row vectors (C3D_FVec stores w,z,y,x). */
static void handTransposeToUniform(const float* m, int loc) {
    C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, loc, 4);
    for (int row = 0; row < 4; row++) {
        rows[row].x = m[0 * 4 + row];
        rows[row].y = m[1 * 4 + row];
        rows[row].z = m[2 * 4 + row];
        rows[row].w = m[3 * 4 + row];
    }
}

static void perturbMathInputs(u32 frame) {
    const float wobble = (float)(frame & 63) * 0.001f;
    sHandView[12] = wobble;
    sC3dView.r[0].w = wobble;
}

static void submitT5Math(const TestSpec* spec, Accum* acc, u32 frame) {
    perturbMathInputs(frame);
    float product[16];
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        handMultiply(product, sHandView, sHandModel[i]);
        handTransposeToUniform(product, sLoc.modelView);
    }
    u64 t1 = svcGetSystemTick();
    acc->sub[0] += t1 - t0;

    C3D_Mtx c3dProduct;
    t0 = svcGetSystemTick();
    for (int i = 0; i < spec->n; i++) {
        Mtx_Multiply(&c3dProduct, &sC3dView, &sC3dModel[i]);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.modelView, &c3dProduct);
    }
    t1 = svcGetSystemTick();
    acc->sub[1] += t1 - t0;
}

static void writeSpriteCorner(VertexF* v, float x, float y, float u, float t) {
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = 0.0f;
    v->nrm[0] = 0.0f;
    v->nrm[1] = 0.0f;
    v->nrm[2] = 1.0f;
    v->uv[0] = u;
    v->uv[1] = t;
}

static void writeSprites(VertexF* vbo, u32 frame) {
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const float x = (float)((i * 37 + (int)frame) % 392);
        const float y = (float)((i * 23 + (int)frame / 2) % 232);
        VertexF* quad = &vbo[i * 4];
        writeSpriteCorner(&quad[0], x, y, 0.0f, 0.0f);
        writeSpriteCorner(&quad[1], x + SPRITE_SIZE, y, 1.0f, 0.0f);
        writeSpriteCorner(&quad[2], x, y + SPRITE_SIZE, 0.0f, 1.0f);
        writeSpriteCorner(&quad[3], x + SPRITE_SIZE, y + SPRITE_SIZE, 1.0f, 1.0f);
    }
}

static void submitT5Stream(const TestSpec* spec, Accum* acc, u32 frame) {
    (void)spec;
    VertexF* vbo = sSpriteVbo[frame & 1];
    const u32 bytes = SPRITE_COUNT * 4 * sizeof(VertexF);

    u64 t0 = svcGetSystemTick();
    writeSprites(vbo, frame);
    u64 t1 = svcGetSystemTick();
    acc->sub[0] += t1 - t0;

    t0 = svcGetSystemTick();
    GSPGPU_FlushDataCache(vbo, bytes);
    t1 = svcGetSystemTick();
    acc->sub[1] += t1 - t0;

    t0 = svcGetSystemTick();
    C3D_BufInfo info;
    BufInfo_Init(&info);
    BufInfo_Add(&info, vbo, sizeof(VertexF), 3, 0x210);
    C3D_SetAttrInfo(&sAttrFloat);
    C3D_SetBufInfo(&info);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sLoc.modelView, &sIdentityMtx);
    C3D_DrawElements(GPU_TRIANGLES, SPRITE_COUNT * 6, C3D_UNSIGNED_SHORT, sSpriteIbo);
    t1 = svcGetSystemTick();
    acc->sub[2] += t1 - t0;
}

/* ---- tests: reports ---------------------------------------------------------------------- */

static void reportEmpty(const TestSpec* spec, const ProbeResult* r) {
    sBaselineGpuMs = r->gpuMs;
    logLine("[dp] T0 empty gpu_ms=%.3f gpu_proc_ms=%.3f frame_ms=%.2f cmdbuf=%.4f frames=%d vsync=%d",
            r->gpuMs, r->procMs, r->frameMs, r->cmdbufMax, r->frames, spec->vsync ? 1 : 0);
}

static void reportT1(const TestSpec* spec, const ProbeResult* r) {
    const double perDraw = perItem(r->subUs[0], spec->n);
    if (spec->n == T3_DRAWS && spec->vsync) {
        sT1PerDrawUs100 = perDraw;
    }
    logLine("[dp] %s draws=%d cpu_us_per_draw=%.2f loop_us=%.1f gpu_ms=%.3f gpu_net_ms=%.3f "
            "gpu_proc_ms=%.3f frame_ms=%.2f cmdbuf=%.4f frames=%d vsync=%d",
            spec->name, spec->n, perDraw, r->subUs[0], r->gpuMs, netGpuMs(r), r->procMs,
            r->frameMs, r->cmdbufMax, r->frames, spec->vsync ? 1 : 0);
}

static void reportT2(const TestSpec* spec, const ProbeResult* r) {
    const double setUs = perItem(r->subUs[0], spec->n);
    const double setFlushUs = perItem(r->subUs[1], spec->n);
    const double setDrawUs = perItem(r->subUs[2], spec->n);
    logLine("[dp] T2 draws=%d set_us=%.2f flush_us=%.2f draw_us=%.2f cpu_us_per_draw=%.2f "
            "gpu_ms=%.3f gpu_net_ms=%.3f frame_ms=%.2f cmdbuf=%.4f frames=%d",
            spec->n, setUs, setFlushUs - setUs, setDrawUs - setFlushUs, setDrawUs, r->gpuMs,
            netGpuMs(r), r->frameMs, r->cmdbufMax, r->frames);
}

static const char* t3ModeName(int mode) {
    switch (mode) {
        case T3_TEXENV:
            return "texenv";
        case T3_TEXTURE:
            return "texbind";
        case T3_VBO:
            return "vbo";
        default:
            return "all3";
    }
}

static void reportT3(const TestSpec* spec, const ProbeResult* r) {
    const double perDraw = perItem(r->subUs[0], spec->n);
    const double change = sT1PerDrawUs100 >= 0.0 ? perDraw - sT1PerDrawUs100 : -1.0;
    logLine("[dp] T3 change=%s draws=%d cpu_us_per_draw=%.2f change_us=%.2f (vs T1 draws=100 "
            "%.2f) gpu_ms=%.3f frame_ms=%.2f cmdbuf=%.4f frames=%d",
            t3ModeName(spec->mode), spec->n, perDraw, change, sT1PerDrawUs100, r->gpuMs,
            r->frameMs, r->cmdbufMax, r->frames);
}

static double ratePerUs(double count, double ms) {
    return ms > 0.01 ? count / (ms * 1000.0) : 0.0;
}

static void reportT4(const TestSpec* spec, const ProbeResult* r) {
    const Mesh* mesh = t4Mesh(spec);
    const double tris = (double)spec->n * mesh->indexCount / 3.0;
    const double verts = (double)spec->n * mesh->vertexCount;
    const double indices = (double)spec->n * mesh->indexCount;
    const double net = netGpuMs(r);
    logLine("[dp] %s mesh=%s draws=%d tris=%.0f verts=%.0f idx=%.0f gpu_ms=%.3f gpu_net_ms=%.3f "
            "mverts_s=%.2f midx_s=%.2f mtris_s=%.2f cpu_us_per_draw=%.2f frame_ms=%.2f "
            "cmdbuf=%.4f frames=%d vsync=%d",
            spec->name, t4MeshName(spec), spec->n, tris, verts, indices, r->gpuMs, net,
            ratePerUs(verts, net), ratePerUs(indices, net), ratePerUs(tris, net),
            perItem(r->subUs[0], spec->n), r->frameMs, r->cmdbufMax, r->frames,
            spec->vsync ? 1 : 0);
}

static void reportFill(const TestSpec* spec, const ProbeResult* r) {
    const double net = netGpuMs(r);
    logLine("[dp] %s layers=%d gpu_ms=%.3f gpu_net_ms=%.3f gpu_ms_per_layer=%.3f "
            "cpu_us=%.1f frame_ms=%.2f frames=%d vsync=%d",
            spec->name, spec->n, r->gpuMs, net, perItem(net, spec->n), r->subUs[0], r->frameMs,
            r->frames, spec->vsync ? 1 : 0);
}

static void reportT5Math(const TestSpec* spec, const ProbeResult* r) {
    logLine("[dp] T5 mtx machines=%d hand_us_per_machine=%.3f c3d_us_per_machine=%.3f "
            "hand_total_us=%.1f c3d_total_us=%.1f frames=%d",
            spec->n, perItem(r->subUs[0], spec->n), perItem(r->subUs[1], spec->n), r->subUs[0],
            r->subUs[1], r->frames);
}

static void reportT5Stream(const TestSpec* spec, const ProbeResult* r) {
    (void)spec;
    logLine("[dp] T5 stream sprites=%d verts=%d write_us=%.1f flush_us=%.1f draw_us=%.1f "
            "total_us=%.1f gpu_ms=%.3f gpu_net_ms=%.3f cmdbuf=%.4f frames=%d",
            SPRITE_COUNT, SPRITE_COUNT * 4, r->subUs[0], r->subUs[1], r->subUs[2],
            r->subUs[0] + r->subUs[1] + r->subUs[2], r->gpuMs, netGpuMs(r), r->cmdbufMax,
            r->frames);
}

/* ---- the test table ---------------------------------------------------------------------- */

static const TestSpec kTests[] = {
    {"T0", readyAlways, submitEmpty, reportEmpty, 0, 0, true},
    {"T1", readySmall, submitT1, reportT1, 1, 0, true},
    {"T1", readySmall, submitT1, reportT1, 30, 0, true},
    {"T1", readySmall, submitT1, reportT1, 100, 0, true},
    {"T1", readySmall, submitT1, reportT1, 300, 0, true},
    {"T2", readySmall, submitT2, reportT2, 30, 0, true},
    {"T2", readySmall, submitT2, reportT2, 100, 0, true},
    {"T2", readySmall, submitT2, reportT2, 300, 0, true},
    {"T3", readyT3, submitT3, reportT3, T3_DRAWS, T3_TEXENV, true},
    {"T3", readyT3, submitT3, reportT3, T3_DRAWS, T3_TEXTURE, true},
    {"T3", readyT3, submitT3, reportT3, T3_DRAWS, T3_VBO, true},
    {"T3", readyT3, submitT3, reportT3, T3_DRAWS, T3_TEXENV | T3_TEXTURE | T3_VBO, true},
    {"T4", readyT4, submitT4, reportT4, MACHINE_COUNT, 0, true},
    {"T4", readyT4, submitT4, reportT4, MACHINE_COUNT, 1, true},
    {"T4", readyT4, submitT4, reportT4, MACHINE_COUNT, 2, true},
    {"FILL", readyFill, submitFill, reportFill, 1, 0, true},
    {"FILL", readyFill, submitFill, reportFill, 2, 0, true},
    {"FILL", readyFill, submitFill, reportFill, 4, 0, true},
    {"FILL", readyFill, submitFill, reportFill, 8, 0, true},
    {"T5", readyAlways, submitT5Math, reportT5Math, MACHINE_COUNT, 0, true},
    {"T5", readyStream, submitT5Stream, reportT5Stream, SPRITE_COUNT, 0, true},
    {"T1nv", readySmall, submitT1, reportT1, 300, 0, false},
    {"T4nv", readyT4, submitT4, reportT4, MACHINE_COUNT, 1, false},
    {"FILLnv", readyFill, submitFill, reportFill, 4, 0, false},
};

/* ---- the runner ---------------------------------------------------------------------------- */

static void pollAbort(void) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) {
        sAbort = true;
    }
}

static void recordFrameStart(Accum* acc, u64 begin, u64* prevBegin) {
    acc->gpuDrawMs += C3D_GetDrawingTime();
    acc->gpuProcMs += C3D_GetProcessingTime();
    if (*prevBegin != 0) {
        acc->framePeriods += begin - *prevBegin;
        acc->periodCount++;
    }
}

static void runFrame(const TestSpec* spec, Accum* acc, bool measuring, u64* prevBegin) {
    C3D_FrameBegin(spec->vsync ? C3D_FRAME_SYNCDRAW : 0);
    const u64 begin = svcGetSystemTick();
    if (measuring) {
        recordFrameStart(acc, begin, prevBegin);
    }
    *prevBegin = begin;
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(sTop);
    applyBaseState();
    const u64 s0 = svcGetSystemTick();
    spec->submit(spec, acc, sGlobalFrame);
    acc->submit += svcGetSystemTick() - s0;
    const float usage = C3D_GetCmdBufUsage();
    if (usage > acc->cmdbufMax) {
        acc->cmdbufMax = usage;
    }
    C3D_FrameEnd(0);
    if (measuring) {
        acc->frames++;
    }
    sGlobalFrame++;
    laneHeartbeat();
}

static bool runTest(const TestSpec* spec, ProbeResult* res) {
    Accum acc;
    Accum warm;
    memset(&acc, 0, sizeof(acc));
    memset(&warm, 0, sizeof(warm));
    const u64 start = svcGetSystemTick();
    u64 prevBegin = 0;
    for (int frame = 0; frame < WARMUP_FRAMES + MEASURE_FRAMES; frame++) {
        if (!aptMainLoop()) {
            sAbort = true;
        }
        pollAbort();
        if (sAbort) {
            return false;
        }
        const bool measuring = frame >= WARMUP_FRAMES;
        runFrame(spec, measuring ? &acc : &warm, measuring, &prevBegin);
        if (msSince(start) > TEST_WALL_CAP_MS && acc.frames >= 10) {
            break;
        }
    }
    finishResult(&acc, res);
    return true;
}

static void noteCmdbuf(const TestSpec* spec, const ProbeResult* res) {
    if (res->cmdbufMax > sCmdbufMaxAll) {
        sCmdbufMaxAll = res->cmdbufMax;
        sCmdbufMaxTest = spec->name;
    }
}

static void runAllTests(void) {
    const int count = (int)(sizeof(kTests) / sizeof(kTests[0]));
    for (int i = 0; i < count && !sAbort; i++) {
        const TestSpec* spec = &kTests[i];
        if (!spec->ready(spec)) {
            logLine("[dp] %s n=%d mode=%d SKIP reason=resources_missing", spec->name, spec->n,
                    spec->mode);
            continue;
        }
        ProbeResult res;
        if (!runTest(spec, &res)) {
            break;
        }
        spec->report(spec, &res);
        noteCmdbuf(spec, &res);
    }
    logLine("[dp] T6 cmdbuf_max=%.4f at=%s cmdbuf_bytes=%u", sCmdbufMaxAll, sCmdbufMaxTest,
            (unsigned)CMDBUF_BYTES);
}

/* ---- setup ------------------------------------------------------------------------------- */

static void buildMathInputs(void) {
    Mtx_Identity(&sC3dView);
    Mtx_Translate(&sC3dView, 1.0f, 2.0f, -30.0f, true);
    for (int i = 0; i < 16; i++) {
        sHandView[i] = (i % 5 == 0) ? 1.0f : 0.01f * (float)i;
    }
    for (int m = 0; m < MACHINE_COUNT; m++) {
        machineMatrix(&sC3dModel[m], m, 1.0f, 0.2f * (float)m);
        for (int i = 0; i < 16; i++) {
            sHandModel[m][i] = (i % 5 == 0) ? 1.0f : 0.001f * (float)(i + m);
        }
    }
}

static void buildMeshes(void) {
    if (!buildSphereMesh(&sSmallMesh, 4, 8, false)) {
        logLine("[dp] WARN small mesh alloc failed");
    }
    for (int i = 0; i < SWITCH_MESH_COUNT; i++) {
        if (!buildSphereMesh(&sSwitchMesh[i], 4, 8, false)) {
            logLine("[dp] WARN switch mesh %d alloc failed", i);
        }
    }
    if (!buildSphereMesh(&sMesh400, 10, 20, false)) {
        logLine("[dp] WARN 400-tri mesh alloc failed");
    }
    if (!buildSphereMesh(&sMesh1500, 25, 30, false)) {
        logLine("[dp] WARN 1500-tri mesh alloc failed");
    }
    if (!buildSphereMesh(&sMesh1500Short, 25, 30, true)) {
        logLine("[dp] WARN 1500-tri short mesh alloc failed");
    }
    if (!buildFullscreenQuad(&sQuadMesh)) {
        logLine("[dp] WARN quad alloc failed");
    }
    if (!buildSpriteBuffers()) {
        logLine("[dp] WARN sprite buffers alloc failed");
    }
}

static void logHeader(void) {
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    logLine("[dp] HDR drawprobe build=%s is_new3ds=%d hb=%d ostime_ms=%llu cmdbuf_bytes=%u "
            "warmup=%d frames=%d linear_free_k=%lu",
            DRAWPROBE_BUILD_ID, isNew ? 1 : 0, envIsHomebrew() ? 1 : 0,
            (unsigned long long)osGetTime(), (unsigned)CMDBUF_BYTES, WARMUP_FRAMES,
            MEASURE_FRAMES, (unsigned long)(linearSpaceFree() / 1024u));
}

static bool initGraphics(void) {
    gfxInitDefault();
    gfxSet3D(false);
    consoleInit(GFX_BOTTOM, NULL);
    if (!C3D_Init(CMDBUF_BYTES)) {
        return false;
    }
    sTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sTop == NULL) {
        return false;
    }
    C3D_RenderTargetSetOutput(sTop, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    return true;
}

static void waitForStart(void) {
#ifdef DRAWPROBE_LANE_LOG
    logLine("[oracle] quit requested (drawprobe lane build: done)");
    svcSleepThread(1000000000LL);
#else
    printf("\nDONE - press START to exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }
#endif
}

int main(void) {
    sBootTick = svcGetSystemTick();
    aptSetHomeAllowed(false);
    const bool gfxOk = initGraphics();
    openLogs();
    logHeader();
    if (!gfxOk) {
        logLine("[dp] FATAL citro3d init failed");
    } else if (!loadShader()) {
        logLine("[dp] FATAL shader load or uniform lookup failed");
    } else {
        buildAttrInfos();
        buildDrawTables();
        buildMathInputs();
        buildTextures();
        buildMeshes();
        runAllTests();
    }
    logLine("[dp] END %s total_ms=%lu frames=%lu", sAbort ? "ABORTED" : "OK", msSince(sBootTick),
            (unsigned long)sGlobalFrame);
    waitForStart();
    closeLogs();
    if (gfxOk) {
        C3D_Fini();
    }
    gfxExit();
    return 0;
}
