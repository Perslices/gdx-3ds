/* gdx3ds_mesh_walk.c — see gdx3ds_mesh_walk.h. */
#include "gdx3ds_mesh_walk.h"

#include <string.h>

/* F3DEX2 opcodes (decomp/include/PR/gbi.h, F3DEX_GBI_2). */
#define OP_VTX 0x01
#define OP_TRI1 0x05
#define OP_TRI2 0x06
#define OP_TEXTURE 0xD7
#define OP_ENDDL 0xDF
#define OP_RDPPIPESYNC 0xE7
#define OP_SETPRIMCOLOR 0xFA
#define OP_SETENVCOLOR 0xFB
#define OP_SETCOMBINE 0xFC

typedef struct WalkState {
    Gdx3dsMeshWalkOut* out;
    /* live RSP/RDP state as the emitter left it */
    Gdx3dsMeshRun cur;
    int stateDirty; /* a state command ran since the current run's first tri */
    uint8_t slotChunk[GDX3DS_MESH_SLOTS];
    uint8_t slotLocal[GDX3DS_MESH_SLOTS];
} WalkState;

const char* gdx3ds_mesh_walk_strerror(int status) {
    switch (status) {
        case GDX3DS_MESH_WALK_OK: return "ok";
        case GDX3DS_MESH_WALK_E_OPCODE: return "unexpected opcode";
        case GDX3DS_MESH_WALK_E_NOEND: return "no G_ENDDL";
        case GDX3DS_MESH_WALK_E_VTXRANGE: return "G_VTX slot range";
        case GDX3DS_MESH_WALK_E_VTXPTR: return "vertex pointer refused";
        case GDX3DS_MESH_WALK_E_SLOT: return "unloaded vertex slot";
        case GDX3DS_MESH_WALK_E_CAPACITY: return "capacity";
        case GDX3DS_MESH_WALK_E_NULL: return "null list";
        case GDX3DS_MESH_WALK_E_TEXOFF: return "G_TEXTURE off";
        default: return "?";
    }
}

static int Fail(WalkState* s, int status, size_t cmd, uint8_t op) {
    s->out->status = status;
    s->out->errCmd = (uint16_t)cmd;
    s->out->errOpcode = op;
    return status;
}

static int SameState(const Gdx3dsMeshRun* a, const Gdx3dsMeshRun* b) {
    return a->combineW0 == b->combineW0 && a->combineW1 == b->combineW1 && a->primRgba == b->primRgba &&
           a->envRgba == b->envRgba && a->texS == b->texS && a->texT == b->texT && a->tile == b->tile &&
           a->texOn == b->texOn && a->primKind == b->primKind && a->envKind == b->envKind &&
           a->primMin == b->primMin && a->primLod == b->primLod && a->combineSet == b->combineSet &&
           a->texSet == b->texSet;
}

static int AddTri(WalkState* s, unsigned a, unsigned b, unsigned c, size_t cmd, uint8_t op) {
    Gdx3dsMeshWalkOut* out = s->out;
    unsigned idx[3] = { a, b, c };
    int i;
    if (a >= GDX3DS_MESH_SLOTS || b >= GDX3DS_MESH_SLOTS || c >= GDX3DS_MESH_SLOTS) {
        return Fail(s, GDX3DS_MESH_WALK_E_SLOT, cmd, op);
    }
    if (out->nTris >= GDX3DS_MESH_MAX_TRIS) {
        return Fail(s, GDX3DS_MESH_WALK_E_CAPACITY, cmd, op);
    }
    if (out->nRuns == 0 || s->stateDirty) {
        /* A state command that re-set the SAME state does not split the interpreter's batch
         * (same tile -> textures_changed stays clear; same prim/env -> no flush), so merge. */
        if (out->nRuns == 0 || !SameState(&out->runs[out->nRuns - 1], &s->cur)) {
            if (out->nRuns >= GDX3DS_MESH_MAX_RUNS) {
                return Fail(s, GDX3DS_MESH_WALK_E_CAPACITY, cmd, op);
            }
            s->cur.firstTri = out->nTris;
            s->cur.triCount = 0;
            out->runs[out->nRuns++] = s->cur;
        }
        s->stateDirty = 0;
    }
    for (i = 0; i < 3; i++) {
        const uint8_t ch = s->slotChunk[idx[i]];
        if (ch == 0xFF) {
            return Fail(s, GDX3DS_MESH_WALK_E_SLOT, cmd, op);
        }
        out->tris[out->nTris * 3 + i] = (uint16_t)(out->chunks[ch].baseVertex + s->slotLocal[idx[i]]);
    }
    out->nTris++;
    out->runs[out->nRuns - 1].triCount++;
    return 0;
}

int gdx3ds_mesh_walk(const void* gfx, size_t maxCmds, Gdx3dsMeshResolveFn resolve, void* user,
                     Gdx3dsMeshWalkOut* out) {
    WalkState st;
    size_t i;
    memset(out, 0, sizeof(*out));
    memset(&st, 0, sizeof(st));
    st.out = out;
    memset(st.slotChunk, 0xFF, sizeof(st.slotChunk));
    st.cur.primKind = GDX3DS_MESH_COLOR_INHERIT;
    st.cur.envKind = GDX3DS_MESH_COLOR_INHERIT;
    if (gfx == NULL) {
        return Fail(&st, GDX3DS_MESH_WALK_E_NULL, 0, 0);
    }
    for (i = 0; i < maxCmds; i++) {
        const uint32_t w0 = gdx3ds_mesh_gfx_w0(gfx, i);
        const uint64_t w1 = gdx3ds_mesh_gfx_w1(gfx, i);
        const uint8_t op = (uint8_t)(w0 >> 24);
        out->opcodeMask[op >> 5] |= 1u << (op & 31);
        out->nCmds = (uint16_t)(i + 1);
        switch (op) {
            case OP_ENDDL:
                /* State set after the last triangle still changes what the list leaves behind:
                 * record it as a zero-triangle run the device applies in order. */
                if (st.stateDirty && out->nRuns < GDX3DS_MESH_MAX_RUNS) {
                    st.cur.firstTri = out->nTris;
                    st.cur.triCount = 0;
                    out->runs[out->nRuns++] = st.cur;
                }
                return out->status = GDX3DS_MESH_WALK_OK;
            case OP_RDPPIPESYNC:
                break;
            case OP_TEXTURE:
                st.cur.texS = (uint16_t)((w1 >> 16) & 0xFFFF);
                st.cur.texT = (uint16_t)(w1 & 0xFFFF);
                st.cur.tile = (uint8_t)((w0 >> 8) & 7);
                st.cur.texOn = (uint8_t)((w0 >> 1) & 0x7F);
                st.cur.texSet = 1;
                /* G_OFF is emitted before untextured (shade/prim-only) runs; the interpreter's
                 * GfxSpTexture ignores the on bit, the combiner decides texture use. */
                st.stateDirty = 1;
                break;
            case OP_SETCOMBINE:
                st.cur.combineW0 = w0 & 0x00FFFFFFu;
                st.cur.combineW1 = (uint32_t)(w1 & 0xFFFFFFFFu);
                st.cur.combineSet = 1;
                st.stateDirty = 1;
                break;
            case OP_SETPRIMCOLOR:
                st.cur.primMin = (uint8_t)((w0 >> 8) & 0xFF);
                st.cur.primLod = (uint8_t)(w0 & 0xFF);
                st.cur.primRgba = (uint32_t)(w1 & 0xFFFFFFFFu);
                st.cur.primKind = GDX3DS_MESH_COLOR_CONST;
                st.stateDirty = 1;
                break;
            case OP_SETENVCOLOR:
                st.cur.envRgba = (uint32_t)(w1 & 0xFFFFFFFFu);
                st.cur.envKind = GDX3DS_MESH_COLOR_CONST;
                st.stateDirty = 1;
                break;
            case OP_VTX: {
                const unsigned n = (w0 >> 12) & 0xFF;
                const unsigned v0 = (((w0 >> 1) & 0x7F) >= n) ? (((w0 >> 1) & 0x7F) - n) : 0xFFFF;
                const void* src;
                unsigned ch, k;
                out->nVtxCmds++;
                if (n == 0 || v0 == 0xFFFF || v0 + n > GDX3DS_MESH_SLOTS) {
                    return Fail(&st, GDX3DS_MESH_WALK_E_VTXRANGE, i, op);
                }
                if (v0 != 0) {
                    out->v0NonZero = 1;
                }
                src = resolve(w1, (size_t)n * 16u, user);
                if (src == NULL) {
                    return Fail(&st, GDX3DS_MESH_WALK_E_VTXPTR, i, op);
                }
                for (ch = 0; ch < out->nChunks; ch++) {
                    if (out->chunks[ch].src == src && out->chunks[ch].count == n && out->chunks[ch].texS == st.cur.texS &&
                        out->chunks[ch].texT == st.cur.texT) {
                        break;
                    }
                }
                if (ch == out->nChunks) {
                    if (out->nChunks >= GDX3DS_MESH_MAX_CHUNKS || out->nVerts + n > GDX3DS_MESH_MAX_VERTS) {
                        return Fail(&st, GDX3DS_MESH_WALK_E_CAPACITY, i, op);
                    }
                    out->chunks[ch].srcW1 = w1;
                    out->chunks[ch].src = src;
                    out->chunks[ch].count = (uint16_t)n;
                    out->chunks[ch].texS = st.cur.texS;
                    out->chunks[ch].texT = st.cur.texT;
                    out->chunks[ch].baseVertex = out->nVerts;
                    out->nVerts = (uint16_t)(out->nVerts + n);
                    out->nChunks++;
                }
                for (k = 0; k < n; k++) {
                    st.slotChunk[v0 + k] = (uint8_t)ch;
                    st.slotLocal[v0 + k] = (uint8_t)k;
                }
                break;
            }
            case OP_TRI1: {
                const int r = AddTri(&st, ((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2, i, op);
                if (r != 0) {
                    return r;
                }
                break;
            }
            case OP_TRI2: {
                const uint32_t lo = (uint32_t)(w1 & 0xFFFFFFFFu);
                int r = AddTri(&st, ((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2, i, op);
                if (r != 0) {
                    return r;
                }
                r = AddTri(&st, ((lo >> 16) & 0xFF) / 2, ((lo >> 8) & 0xFF) / 2, (lo & 0xFF) / 2, i, op);
                if (r != 0) {
                    return r;
                }
                break;
            }
            default:
                return Fail(&st, GDX3DS_MESH_WALK_E_OPCODE, i, op);
        }
    }
    return Fail(&st, GDX3DS_MESH_WALK_E_NOEND, maxCmds, 0);
}
