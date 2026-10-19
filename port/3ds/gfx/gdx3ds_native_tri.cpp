/* gdx3ds_native_tri.cpp — shared exact/fast triangle replay (gdx3ds_native_tri.h).
 *
 * Extracted verbatim from gdx3ds_track_replay.cpp (native/track); see the header. The only
 * edits are the namespace, the removal of the track-specific record pointer from the
 * context, and (native/interphooks) the interpreter access going through GdxInterpSeam so
 * the one body serves both interpreters: register reads are the seam's live views, the exact
 * path is the seam's SpTri + PopAppended adoption. */
#include "gdx3ds_native_tri.h"

#include <cstring>

#include "gdx3ds_nativebatch.h"
#include "gdx3ds_replay.h"
#include "gdx3ds_stereo.h"

extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);

namespace GdxNativeTri {

namespace {

constexpr size_t kStride = GDX3DS_VBOPACK_STRIDE;

struct ByteToUnit {
    float v[256];
    ByteToUnit() {
        for (int k = 0; k < 256; k++) {
            v[k] = k / 255.0f;
        }
    }
};
const ByteToUnit sB2U;

/* newlib s_fminf.c semantics, as the interpreter's GdxFminExact. */
inline float FminExact(float x, float y) {
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x < y ? x : y;
}

/* Combiner-input evaluation, verbatim from GfxSpTri1's gdxInput lambda. */
struct PackInput {
    GdxSeamRgba c;
    bool isShade;
};
PackInput InputValue(const GdxSeamRegs& R, const uint8_t (*inputMap)[7], int j, int k, const GdxSeamVertex* v,
                     const GdxSeamVertex* v1) {
    GdxSeamRgba* color;
    GdxSeamRgba tmp = { 0, 0, 0, 0 };
    bool isShade = false;
    switch (inputMap[k][j]) {
        case GDXSEAM_CCMUX_PRIMITIVE:
            color = R.prim_color;
            break;
        case GDXSEAM_CCMUX_SHADE:
            color = const_cast<GdxSeamRgba*>(&v->color);
            isShade = true;
            break;
        case GDXSEAM_CCMUX_SHADE_ALPHA: {
            tmp.r = tmp.g = tmp.b = tmp.a = v->color.a;
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_ENVIRONMENT:
            color = R.env_color;
            break;
        case GDXSEAM_CCMUX_PRIMITIVE_ALPHA: {
            tmp.r = tmp.g = tmp.b = R.prim_color->a;
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_ENV_ALPHA: {
            tmp.r = tmp.g = tmp.b = R.env_color->a;
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_PRIM_LOD_FRAC: {
            tmp.r = tmp.g = tmp.b = *R.prim_lod_fraction;
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_LOD_FRACTION: {
            if (*R.other_mode_l & GDXSEAM_G_TL_LOD) {
                float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                if (distance_frac < 0.0f) {
                    distance_frac = 0.0f;
                }
                if (distance_frac > 1.0f) {
                    distance_frac = 1.0f;
                }
                tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
            } else {
                tmp.r = tmp.g = tmp.b = tmp.a = 255.0f;
            }
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_KEY_CENTER:
            color = R.key_center;
            break;
        case GDXSEAM_CCMUX_KEY_SCALE:
            color = R.key_scale;
            break;
        case GDXSEAM_CCMUX_CONVERT_K4: {
            tmp.r = tmp.g = tmp.b = R.convert_k[4];
            color = &tmp;
            break;
        }
        case GDXSEAM_CCMUX_CONVERT_K5: {
            tmp.r = tmp.g = tmp.b = R.convert_k[5];
            color = &tmp;
            break;
        }
        case GDXSEAM_ACMUX_PRIM_LOD_FRAC:
            tmp.a = *R.prim_lod_fraction;
            color = &tmp;
            break;
        default:
            color = &tmp;
            break;
    }
    return { *color, isShade };
}

enum TriResult { TRI_DONE, TRI_NEED_EXACT };

/* The fast path: GfxSpTri1's reject / clip pre-scan / cull decisions and the trifast packed
 * emission, expression for expression. Returns TRI_NEED_EXACT when the tri must take the
 * interpreter's path (clipper). */
TriResult FastTri(RunCtx& c, uint8_t a, uint8_t b, uint8_t d) {
    const GdxSeamRegs& R = c.I->R();
    const GdxSeamVertex* v1 = &R.vertices[a];
    const GdxSeamVertex* v2 = &R.vertices[b];
    const GdxSeamVertex* v3 = &R.vertices[d];
    const GdxSeamVertex* v_arr[3] = { v1, v2, v3 };
    const GdxTrackPackState& ps = c.ps;

    if (ps.rejectVariant) {
        for (const GdxSeamVertex* vertex : v_arr) {
            const float w2 = 2.0f * vertex->w;
            if (vertex->w <= 0.0f || vertex->x < -w2 || vertex->x > w2 || vertex->y < -w2 || vertex->y > w2 ||
                vertex->z < -vertex->w) {
                return TRI_DONE;
            }
        }
    }
    if ((v1->clip_out | v2->clip_out | v3->clip_out) != 0) {
        return TRI_NEED_EXACT;
    }
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        return TRI_DONE;
    }
    const uint32_t geometry_mode = *R.geometry_mode;
    if ((geometry_mode & ps.cullBoth) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            cross = -cross;
        }
        if ((*R.extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0) {
            cross = -cross;
        }
        const uint32_t cull_type = geometry_mode & ps.cullBoth;
        if (cull_type == ps.cullFront) {
            if (cross <= 0) {
                return TRI_DONE;
            }
        } else if (cull_type == ps.cullBack) {
            if (cross >= 0) {
                return TRI_DONE;
            }
        } else if (cull_type == ps.cullBoth) {
            return TRI_DONE;
        }
    }

    float* out0 = Gdx_NativeBatchVerts(3);
    if (out0 == nullptr) {
        return TRI_NEED_EXACT; /* pool exhausted: let the interpreter's own early-out decide */
    }
    const Gdx3dsVboPackParams& pp = *ps.params;
    const uint8_t(*inputMap)[7] = ps.inputMap;
    for (int i = 0; i < 3; i++) {
        float* out = out0 + i * kStride;
        const GdxSeamVertex* vv = v_arr[i];
        if (gdx3ds_replay_rec) {
            gdx3ds_replay_out_vtx(out, i == 0 ? a : (i == 1 ? b : d)); /* C6: the loaded slot */
        }
        float z = vv->z, w = vv->w;
        if (ps.clip.zIsFrom0To1) {
            z = (z + w) / 2.0f;
        }
        out[0] = vv->x;
        out[1] = ps.clip.invertY ? -vv->y : vv->y;
        out[2] = z;
        out[3] = w;
        for (int t = 0; t < 2; t++) {
            const int slot = (t == 0) ? 4 : 10;
            if (!ps.usedTextures[t]) {
                out[slot] = 0.0f;
                out[slot + 1] = 0.0f;
                continue;
            }
            float u = vv->u / 32.0f;
            float v = vv->v / 32.0f;
            u *= c.prep[t].uPre;
            v *= c.prep[t].vPre;
            u -= c.prep[t].uOff;
            v -= c.prep[t].vOff;
            if (c.halfTexel) {
                u += 0.5f;
                v += 0.5f;
            }
            const float nu = c.prep[t].uMul ? u * c.prep[t].uNormMul : u / c.prep[t].uNormDiv;
            const float nv = c.prep[t].vMul ? v * c.prep[t].vNormMul : v / c.prep[t].vNormDiv;
            const float pu = FminExact(nu * pp.uScale[t], c.clampU[t]);
            const float pv = FminExact(nv * pp.vScale[t], c.clampV[t]);
            if (pp.hasOff) {
                out[slot] = pu + pp.uOff[t];
                out[slot + 1] = pv + pp.vOff[t];
            } else {
                out[slot] = pu;
                out[slot + 1] = pv;
            }
        }
        if (c.ci >= 0) {
            if (c.colorIsShade) {
                out[6] = sB2U.v[vv->color.r];
                out[7] = sB2U.v[vv->color.g];
                out[8] = sB2U.v[vv->color.b];
            } else {
                const PackInput cv = InputValue(R, inputMap, c.ci, 0, vv, v1);
                out[6] = sB2U.v[cv.c.r];
                out[7] = sB2U.v[cv.c.g];
                out[8] = sB2U.v[cv.c.b];
            }
        } else {
            out[6] = out[7] = out[8] = 1.0f;
        }
        if (pp.fogFactorToAlpha) {
            out[9] = c.fogFactorConst ? c.fogFactorConstVal : sB2U.v[vv->color.a];
        } else if (pp.wantVtxAlpha) {
            if (c.alphaIsShade) {
                out[9] = c.shadeAlphaIsOne ? 1.0f : sB2U.v[vv->color.a];
            } else {
                const PackInput av = InputValue(R, inputMap, c.ai, 1, vv, v1);
                out[9] = (c.shadeAlphaIsOne && av.isShade) ? 1.0f : sB2U.v[av.c.a];
            }
        } else {
            out[9] = 1.0f;
        }
    }
    if (c.useFog) {
        if (c.useBlendColor) {
            gdx3ds_fog_note_params(0, 0, 0);
        } else {
            gdx3ds_fog_note_params(*R.fog_mul, *R.fog_offset, 1);
        }
    }
    c.stats_fast++;
    return TRI_DONE;
}

/* The exact path: the interpreter's triangle path resolves state and appends at the pool
 * cursor; adopt what it appended into the native batch. Returns false when the tri stayed in
 * the interpreter's batch (packing refused) — the fast path is then unavailable for this
 * segment. */
bool ExactTri(RunCtx& c, uint8_t a, uint8_t b, uint8_t d) {
    GdxInterpSeam* I = c.I;
    const size_t n0 = I->PendingTris();
    I->SpTri(a, b, d);
    const size_t n1 = I->PendingTris();
    if (n1 < n0) {
        /* A full-buffer Flush ran inside: the interpreter drew its batch; nothing to adopt. */
        c.stats_fallback++;
        return false;
    }
    const size_t appended = n1 - n0;
    if (appended == 0) {
        return true; /* rejected / culled before the state block: nothing changed */
    }
    I->PackState(&c.ps);
    if (!c.ps.packBatch || c.ps.packDst == nullptr || n0 != 0) {
        c.stats_fallback += (uint32_t)appended;
        return false;
    }
    if (!Gdx_NativeBatchOpen()) {
        if (!Gdx_NativeBatchBeginWithAux(c.ps.aux, GDX3DS_STEREO_SCENE)) {
            c.stats_fallback += (uint32_t)appended;
            return false;
        }
    }
    float* cur = Gdx_NativeBatchCursor();
    if (cur != c.ps.packDst) {
        c.stats_fallback += (uint32_t)appended;
        return false;
    }
    Gdx_NativeBatchCommit((uint32_t)appended * 3);
    I->PopAppended(n0, appended);
    c.stats_exact += (uint32_t)appended;
    c.needExact = false;
    c.prepValid = false;
    return true;
}

} // namespace

void Init(RunCtx& c, GdxInterpSeam* I, bool verify) {
    memset(&c, 0, sizeof(c));
    c.I = I;
    c.verify = verify;
    c.needExact = true;
    c.prepValid = false;
    c.verifyHash = GDX_NATIVEBATCH_HASH_SEED;
}

/* Derive the trifast per-batch prep from the state snapshot (GfxSpTri1's expressions). */
bool DerivePrep(RunCtx& c) {
    GdxInterpSeam* I = c.I;
    I->PackState(&c.ps);
    const GdxTrackPackState& ps = c.ps;
    if (!ps.packBatch || ps.params == nullptr || !ps.memoValid || ps.memoDirty || ps.inputMap == nullptr ||
        !ps.triFast) {
        return false;
    }
    const uint8_t(*inputMap)[7] = ps.inputMap;
    const GdxSeamRegs& R = I->R();
    const Gdx3dsVboPackParams& pp = *ps.params;
    const bool bilerp = (*R.other_mode_h & (3U << GDXSEAM_G_MDSFT_TEXTFILT)) == GDXSEAM_G_TF_BILERP;
    c.halfTexel = bilerp; /* !is_rect */
    for (int t = 0; t < 2; t++) {
        c.clampU[t] = 1e30f;
        c.clampV[t] = 1e30f;
        if (!ps.usedTextures[t]) {
            continue;
        }
        const uint32_t uv_tile = ps.effectiveTile[t];
        GdxSeamTileUv tile;
        I->TileUv(uv_tile, &tile);
        const int shifts = tile.shifts;
        const int shiftt = tile.shiftt;
        c.prep[t].uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / (float)(1 << shifts) : (float)(1 << (16 - shifts)));
        c.prep[t].vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / (float)(1 << shiftt) : (float)(1 << (16 - shiftt)));
        c.prep[t].uOff = tile.uls / 4.0f;
        c.prep[t].vOff = tile.ult / 4.0f;
        const uint32_t tw = ps.texWidth[t];
        const uint32_t th = ps.texHeight[t];
        c.prep[t].uMul = (tw & (tw - 1u)) == 0u;
        c.prep[t].vMul = (th & (th - 1u)) == 0u;
        c.prep[t].uNormDiv = (float)tw;
        c.prep[t].vNormDiv = (float)th;
        c.prep[t].uNormMul = c.prep[t].uMul ? 1.0f / (float)tw : 1.0f;
        c.prep[t].vNormMul = c.prep[t].vMul ? 1.0f / (float)th : 1.0f;
        /* Final clamp limits (GfxSpTri1: wrap clamp, shader-clamp axes from the tm bits). */
        c.clampU[t] = pp.wrapClampU[t];
        c.clampV[t] = pp.wrapClampV[t];
        if (pp.shaderClampGate) {
            if (ps.tm & (1u << (2 * t))) {
                c.clampU[t] = ((ps.texWidth2[t] - 0.5f) / ps.texWidth[t]) * pp.uScale[t];
            }
            if (ps.tm & (1u << (2 * t + 1))) {
                c.clampV[t] = ((ps.texHeight2[t] - 0.5f) / ps.texHeight[t]) * pp.vScale[t];
            }
        }
    }
    const uint8_t blend_src = (uint8_t)(*R.other_mode_l >> 30);
    c.useBlendColor = blend_src == GDXSEAM_G_BL_CLR_BL;
    c.useFog = blend_src == GDXSEAM_G_BL_CLR_FOG || c.useBlendColor;
    c.ci = pp.vtxColorInput - 1;
    c.colorIsShade = c.ci >= 0 && inputMap[0][c.ci] == GDXSEAM_CCMUX_SHADE;
    c.ai = pp.vtxAlphaInput - 1;
    c.alphaIsShade = c.ai >= 0 && inputMap[1][c.ai] == GDXSEAM_CCMUX_SHADE;
    c.fogFactorConst = pp.fogFactorToAlpha && c.useBlendColor;
    c.fogFactorConstVal = c.fogFactorConst ? sB2U.v[R.fog_color->a] : 0.0f;
    c.shadeAlphaIsOne = c.useFog && !c.useBlendColor;
    c.prepValid = true;
    return true;
}

/* [native] replay (C6): the triangle's matrix-state tag, from the vertices the seam loaded. */
uint16_t ReplaySlotTag(uint8_t a, uint8_t b, uint8_t d) {
    const uint16_t t = gdx3ds_replay_vtx_tag[a];
    if (gdx3ds_replay_vtx_tag[b] != t || gdx3ds_replay_vtx_tag[d] != t) {
        return GDX_REPLAY_TAG_UNKNOWN;
    }
    return t;
}

void Tri(RunCtx& c, uint8_t a, uint8_t b, uint8_t d) {
    c.stats_tris++;
    if (gdx3ds_replay_rec) {
        /* one pose per draw: a batch whose tag would change is drawn first */
        const uint16_t tag = ReplaySlotTag(a, b, d);
        if (Gdx_NativeBatchOpen() && tag != gdx3ds_replay_nb_tag) {
            FlushNative(c);
        }
        gdx3ds_replay_nb_tag = tag;
    }
    if (!c.needExact) {
        if (!c.prepValid && !DerivePrep(c)) {
            c.needExact = true;
        } else if (FastTri(c, a, b, d) == TRI_DONE) {
            return;
        }
    }
    ExactTri(c, a, b, d);
}

void FlushBatchKeepState(RunCtx& c) {
    if (Gdx_NativeBatchOpen()) {
        if (c.verify) {
            Gdx_NativeBatchDiscard(&c.verifyHash);
        } else if (Gdx_NativeBatchFlush() != 0) {
            c.stats_draws++;
        }
        Gdx_NativeBatchEnd();
    }
}

InputResult EvalInput(const GdxSeamRegs& R, const uint8_t (*inputMap)[7], int j, int k, const GdxSeamVertex* v,
                      const GdxSeamVertex* v1) {
    const PackInput p = InputValue(R, inputMap, j, k, v, v1);
    return { p.c, p.isShade };
}

void FlushNative(RunCtx& c) {
    if (Gdx_NativeBatchOpen()) {
        if (c.verify) {
            Gdx_NativeBatchDiscard(&c.verifyHash);
        } else if (Gdx_NativeBatchFlush() != 0) {
            c.stats_draws++;
        }
        Gdx_NativeBatchEnd();
    }
    c.needExact = true;
    c.prepValid = false;
}

} // namespace GdxNativeTri
