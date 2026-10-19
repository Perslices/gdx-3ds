/* gdx3ds_track_bake_game.c — [native] track_resident (C5 Stage 3c): the game-data side of the
 * course bake (gdx3ds_track_resident.cpp does the rest).
 *
 * Compiled with the game's own headers, so it reads gSegmentChunks and the material table with
 * the decomp's types and runs the decomp's OWN ring setup functions (Course_SetupChunk*Vtx in
 * course.c) to produce every ring variant: nothing about a ring's positions or texture
 * coordinates is re-implemented here. Called on the game thread from Course_Init (after the
 * chunking) through gdxtrk_course_loaded(); the walk is not running then, and the two globals
 * the setup functions use (gCourseVtxPtr, D_800F8930) are saved and restored around each call. */
#include "global.h"
#include "fzx_course.h"

#include <stdint.h>

/* course.c's material descriptor (a TU-local typedef there; same layout, size 0x20). */
typedef struct GdxTrkMaterial {
    void* texture;
    f32 textureScale;
    s32 width;
    s32 tile;
    s32 unk_10;
    s16 textureCoordinateMask;
    s16 u[5]; /* unk_16..unk_1E: the D_800F8930 table the walk loads for this shape/type */
} GdxTrkMaterial;

extern SegmentChunk gSegmentChunks[];
extern SegmentChunk* sLastSegmentChunk;
extern Vtx* gCourseVtxPtr;
extern s16 D_800F8930[5];
extern GdxTrkMaterial* D_800CF8C8[];

void Course_SetupChunkRoadAirVtx(SegmentChunk* chunk, s32 arg1);
void Course_SetupChunkPipeTunnelVtx(SegmentChunk* chunk, s32 arg1);
void Course_SetupChunkCylinderVtx(SegmentChunk* chunk, s32 arg1);
void Course_SetupChunkHalfPipeVtx(SegmentChunk* chunk, s32 arg1);

/* Ring layouts: which setup function a walking shape's emitter runs on BOTH rings of its pair
 * (course.c sForward/sBackwardChunkGroupDrawFuncs). */
enum { GDXTRK_LAYOUT_ROAD = 0, GDXTRK_LAYOUT_PIPE = 1, GDXTRK_LAYOUT_CYL = 2, GDXTRK_LAYOUT_HALF = 3 };
static const int kLayoutOfShape[8] = {
    GDXTRK_LAYOUT_ROAD, /* ROAD */
    GDXTRK_LAYOUT_ROAD, /* WALLED_ROAD */
    GDXTRK_LAYOUT_PIPE, /* PIPE */
    GDXTRK_LAYOUT_CYL,  /* CYLINDER */
    GDXTRK_LAYOUT_HALF, /* HALF_PIPE */
    GDXTRK_LAYOUT_PIPE, /* TUNNEL */
    GDXTRK_LAYOUT_ROAD, /* AIR (Course_SetupChunkRoadAirVtx) */
    GDXTRK_LAYOUT_ROAD, /* BORDERLESS_ROAD */
};

int gdxtrk_game_chunk_count(void) {
    if (sLastSegmentChunk == NULL || sLastSegmentChunk <= gSegmentChunks) {
        return 0;
    }
    return (int) (sLastSegmentChunk - gSegmentChunks);
}

uint32_t gdxtrk_game_chunk_info(int i) {
    return (uint32_t) gSegmentChunks[i].trackSegmentInfo;
}

int gdxtrk_game_layout_of(uint32_t info) {
    return kLayoutOfShape[TRACK_SHAPE_INDEX(info & TRACK_SHAPE_MASK) & 7];
}

/* The U table the walk loads when chunk i is the walking chunk (course.c Course_Draw*ChunkGroup:
 * only for a type other than TRACK_TYPE_NONE; otherwise the previous table stays). */
int gdxtrk_game_chunk_table(int i, int16_t d[5]) {
    const uint32_t info = (uint32_t) gSegmentChunks[i].trackSegmentInfo;
    const u32 shape = TRACK_SHAPE_INDEX(info & TRACK_SHAPE_MASK);
    const s32 type = (s32) (info & TRACK_TYPE_MASK);
    const GdxTrkMaterial* m;
    int k;
    if (type == TRACK_TYPE_NONE || D_800CF8C8[shape] == NULL) {
        return 0;
    }
    m = &D_800CF8C8[shape][type];
    for (k = 0; k < 5; k++) {
        d[k] = m->u[k];
    }
    return 1;
}

/* One ring of chunk i exactly as the walk writes it into courseVtxBuffer: the decomp's setup
 * function for `layout`, with D_800F8930 = d and arg1 = TRACK_FLAG_80000000 when `corr` (the
 * texture-correction V) else 0. joinBits >= 0 runs it on a copy of the chunk whose
 * TRACK_JOIN_MASK bits are replaced (a diagnostic hook; the bake passes -1: the pipe/tunnel ST
 * patches test TRACK_JOIN_BOTH, which is the same 0x600 as the setup's TRACK_JOIN_MASK). `tmpl`
 * seeds the eight Vtx (the fields the setup does not write: flag, cn); `out` receives them. */
void gdxtrk_game_ring(int i, int layout, const int16_t d[5], int corr, int joinBits, const void* tmpl, void* out) {
    Vtx scratch[8];
    SegmentChunk copy;
    SegmentChunk* chunk = &gSegmentChunks[i];
    Vtx* const savedPtr = gCourseVtxPtr;
    s16 savedD[5];
    const s32 arg1 = corr ? (s32) TRACK_FLAG_80000000 : 0;
    int k;
    for (k = 0; k < 8; k++) {
        scratch[k] = *(const Vtx*) tmpl;
    }
    if (joinBits >= 0) {
        copy = *chunk;
        copy.trackSegmentInfo = (copy.trackSegmentInfo & ~TRACK_JOIN_MASK) | (joinBits & TRACK_JOIN_MASK);
        chunk = &copy;
    }
    for (k = 0; k < 5; k++) {
        savedD[k] = D_800F8930[k];
        D_800F8930[k] = d[k];
    }
    gCourseVtxPtr = scratch;
    switch (layout) {
        case GDXTRK_LAYOUT_PIPE:
            Course_SetupChunkPipeTunnelVtx(chunk, arg1);
            break;
        case GDXTRK_LAYOUT_CYL:
            Course_SetupChunkCylinderVtx(chunk, arg1);
            break;
        case GDXTRK_LAYOUT_HALF:
            Course_SetupChunkHalfPipeVtx(chunk, arg1);
            break;
        default:
            Course_SetupChunkRoadAirVtx(chunk, arg1);
            break;
    }
    gCourseVtxPtr = savedPtr;
    for (k = 0; k < 5; k++) {
        D_800F8930[k] = savedD[k];
    }
    for (k = 0; k < 8; k++) {
        ((Vtx*) out)[k] = scratch[k];
    }
}

extern Gfx* sCourseDisp;
void Course_SetupChunkPipeTunnelJoinVtx(SegmentChunk* chunk);

/* n60trk2 [native] nrr_trk_joins: the five tunnel-mouth join vertices of chunk i (slots 16-20 of
 * the join templates D_80143E0..D_80144D0) exactly as Course_SetupChunkPipeTunnelJoinVtx writes them:
 * the decomp's own function runs on a scratch vertex buffer, and the `gSPVertex(sCourseDisp - 1, ...)`
 * it rewrites lands in a scratch command. It writes only `ob`; the fields it leaves (flag, U/V, cn)
 * come from `tmpl` (the stock walk leaves whatever courseVtxBuffer held there: cn = Course_Init's
 * 0,0,0,255, U/V stale -- dead inputs, the join combiner reads no texel). Vertices 5-7 repeat
 * vertex 0 (no template reads them). */
void gdxtrk_game_join_ring(int i, const void* tmpl, void* out) {
    Vtx scratch[8];
    Gfx cmd[2];
    Vtx* const savedPtr = gCourseVtxPtr;
    Gfx* const savedDisp = sCourseDisp;
    int k;
    for (k = 0; k < 8; k++) {
        scratch[k] = *(const Vtx*) tmpl;
    }
    gCourseVtxPtr = scratch;
    sCourseDisp = &cmd[1];
    Course_SetupChunkPipeTunnelJoinVtx(&gSegmentChunks[i]);
    sCourseDisp = savedDisp;
    gCourseVtxPtr = savedPtr;
    for (k = 5; k < 8; k++) {
        scratch[k] = scratch[0];
    }
    for (k = 0; k < 8; k++) {
        ((Vtx*) out)[k] = scratch[k];
    }
}
