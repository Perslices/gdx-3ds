/* gdx3ds_vtx_math.h — CPU T&L shared by the native machine-mesh path, transcribed
 * VERBATIM (same expressions, same float operation order, same integer truncations)
 * from libultraship/src/fast/interpreter.cpp:
 *   GfxSpVertex           -> gdx3ds_vtx_transform   (position, AdjXForAspectRatio,
 *                            lighting, FLX alpha LUT / texgen UVs, fog, clip flags)
 *   CalculateNormalDir    -> gdx3ds_vtx_light_coeffs
 *   GfxSpTri1 (pre-draw)  -> gdx3ds_tri_rej_reject, gdx3ds_tri_all_out, gdx3ds_tri_cull,
 *                            gdx3ds_tri_clip (+ gdx3ds_vtx_update_clip_flags)
 *   GfxSpTri1 [trifast]   -> gdx3ds_uv_prep / gdx3ds_uv_pack (UV normalisation)
 *
 * The interpreter is NOT rewired to this header yet (the LUS patch on this fork is kept
 * to the one-line G_DL hook); tools/native/machines_harness.sh test C checks the header
 * against a reference transcription on the host, and device parity ([native] machines
 * 0 vs 1 SHOT byte-compare / [fh]) is the end-to-end guard. Any change to the
 * interpreter's vertex math must be mirrored here.
 *
 * Pure C, header-only, float only (no double promotion anywhere: the 3DS VFPv2 and the
 * host both run strict IEEE single; compile test C with -ffp-contract=off so the host
 * cannot fuse a*b+c). */
#ifndef GDX3DS_VTX_MATH_H
#define GDX3DS_VTX_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX3DS_VTX_MAX_DIR_LIGHTS 8
#define GDX3DS_VTX_MIN_CLIP_W 1.0e-5f /* interpreter kGdxMinClipW */
#define GDX3DS_VTX_DMEM_SIZE 4096

/* 16-byte host-endian Vtx as the loaded segment image holds it (AssetBindings.c kind-3
 * fixup): position, flag, texcoords, then colour (or s8 normal x3 + alpha). */
typedef struct Gdx3dsVtxIn {
    int16_t ob[3];
    int16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
} Gdx3dsVtxIn;

/* interpreter LoadedVertex (u/v hold the short U/V as floats, exactly as d->u = U does). */
typedef struct Gdx3dsLoadedVtx {
    float x, y, z, w;
    float u, v;
    uint8_t r, g, b, a;
    uint8_t clip_rej;
    uint8_t clip_out;
    uint8_t pad[2];
} Gdx3dsLoadedVtx;

enum { GDX3DS_VTX_X_NONE = 0, GDX3DS_VTX_X_ADJ = 1, GDX3DS_VTX_X_STRETCH = 2 };

/* Everything GfxSpVertex reads from the RSP, latched once per draw. */
typedef struct Gdx3dsVtxMathState {
    float mp[4][4];
    int xMode;    /* GDX3DS_VTX_X_*: ADJ = x * (4/3) / aspect, STRETCH = x * safeAreaScale */
    float aspect; /* (float)width / (float)height */
    int lighting;
    int numDir;                                   /* current_num_lights - 1 */
    float coeffs[GDX3DS_VTX_MAX_DIR_LIGHTS][3];   /* current_lights_coeffs */
    uint8_t lightCol[GDX3DS_VTX_MAX_DIR_LIGHTS][3];
    uint8_t ambient[3];                           /* current_lights[numDir].l.col */
    int textureGen;
    int textureGenLinear;
    int flxAlpha;         /* FZeroFlxReject && f3dflx_alpha_light_valid */
    float alphaLight[3];  /* TransposedMatrixMul(f3dflx_alpha_light, modelview) then normalised */
    const uint8_t* dmem;  /* mRsp->dmem */
    int dmaIoDmem;        /* mRsp->dma_io_dmem */
    float lookat[2][3];   /* current_lookat_coeffs */
    int fog;
    int16_t fogMul, fogOffset;
    uint16_t texScaleS, texScaleT;
} Gdx3dsVtxMathState;

static inline void gdx3ds_vtx_normalize(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static inline void gdx3ds_vtx_transposed_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

/* CalculateNormalDir: light dir (s8 x3) -> model-space coefficients. */
static inline void gdx3ds_vtx_light_coeffs(const int8_t dir[3], const float modelview[4][4], float coeffs[3]) {
    float light_dir[3] = { dir[0] / 127.0f, dir[1] / 127.0f, dir[2] / 127.0f };
    gdx3ds_vtx_transposed_mul(coeffs, light_dir, modelview);
    gdx3ds_vtx_normalize(coeffs);
}

/* FLX alpha light: interpreter recomputes this per vertex from the same inputs, so once
 * per draw is bit-identical. */
static inline void gdx3ds_vtx_alpha_light(const float alphaLightRsp[3], const float modelview[4][4], float out[3]) {
    gdx3ds_vtx_transposed_mul(out, alphaLightRsp, modelview);
    gdx3ds_vtx_normalize(out);
}

static inline float gdx3ds_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t gdx3ds_vtx_clip_out_mask(const Gdx3dsLoadedVtx* v) {
    uint8_t mask = 0;
    if (v->w - GDX3DS_VTX_MIN_CLIP_W < 0.0f) mask |= 1u << 0;
    if (v->x + v->w < 0.0f) mask |= 1u << 1;
    if (v->w - v->x < 0.0f) mask |= 1u << 2;
    if (v->y + v->w < 0.0f) mask |= 1u << 3;
    if (v->w - v->y < 0.0f) mask |= 1u << 4;
    if (v->z + v->w < 0.0f) mask |= 1u << 5;
    if (v->w - v->z < 0.0f) mask |= 1u << 6;
    return mask;
}

/* GfxSpVertex, one vertex. */
static inline void gdx3ds_vtx_transform(const Gdx3dsVtxMathState* s, const Gdx3dsVtxIn* v, Gdx3dsLoadedVtx* d) {
    const int8_t* n = (const int8_t*)v->cn;
    float x = v->ob[0] * s->mp[0][0] + v->ob[1] * s->mp[1][0] + v->ob[2] * s->mp[2][0] + s->mp[3][0];
    float y = v->ob[0] * s->mp[0][1] + v->ob[1] * s->mp[1][1] + v->ob[2] * s->mp[2][1] + s->mp[3][1];
    float z = v->ob[0] * s->mp[0][2] + v->ob[1] * s->mp[1][2] + v->ob[2] * s->mp[2][2] + s->mp[3][2];
    float w = v->ob[0] * s->mp[0][3] + v->ob[1] * s->mp[1][3] + v->ob[2] * s->mp[2][3] + s->mp[3][3];
    int16_t U, V;
    int hasF3dflxAlpha = 0;
    uint8_t f3dflxAlpha = 0;

    if (s->xMode == GDX3DS_VTX_X_STRETCH) {
        const float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
        x = x * kSafeAreaScale;
    } else if (s->xMode == GDX3DS_VTX_X_ADJ) {
        x = x * (4.0f / 3.0f) / s->aspect;
    }

    U = (int16_t)(((int)v->tc[0] * (int)s->texScaleS) >> 16);
    V = (int16_t)(((int)v->tc[1] * (int)s->texScaleT) >> 16);

    if (s->lighting) {
        int r = s->ambient[0];
        int g = s->ambient[1];
        int b = s->ambient[2];
        int i;
        for (i = 0; i < s->numDir; i++) {
            float intensity = 0;
            intensity += n[0] * s->coeffs[i][0];
            intensity += n[1] * s->coeffs[i][1];
            intensity += n[2] * s->coeffs[i][2];
            intensity /= 127.0f;
            if (intensity > 0.0f) {
                r = (int)((float)r + intensity * (float)s->lightCol[i][0]);
                g = (int)((float)g + intensity * (float)s->lightCol[i][1]);
                b = (int)((float)b + intensity * (float)s->lightCol[i][2]);
            }
        }
        d->r = (uint8_t)(r > 255 ? 255 : r);
        d->g = (uint8_t)(g > 255 ? 255 : g);
        d->b = (uint8_t)(b > 255 ? 255 : b);

        if (s->textureGen) {
            if (s->flxAlpha) {
                const float intensity = s->alphaLight[0] * n[0] + s->alphaLight[1] * n[1] + s->alphaLight[2] * n[2];
                int lutIndex = (int)intensity;
                int dmemIndex;
                if (lutIndex < -128) lutIndex = -128;
                if (lutIndex > 127) lutIndex = 127;
                dmemIndex = s->dmaIoDmem + 128 + lutIndex;
                if (dmemIndex >= 0 && dmemIndex < GDX3DS_VTX_DMEM_SIZE) {
                    f3dflxAlpha = s->dmem[dmemIndex];
                    hasF3dflxAlpha = 1;
                }
            } else {
                float dotx = 0, doty = 0;
                dotx += n[0] * s->lookat[0][0];
                dotx += n[1] * s->lookat[0][1];
                dotx += n[2] * s->lookat[0][2];
                doty += n[0] * s->lookat[1][0];
                doty += n[1] * s->lookat[1][1];
                doty += n[2] * s->lookat[1][2];
                dotx /= 127.0f;
                doty /= 127.0f;
                dotx = gdx3ds_clampf(dotx, -1.0f, 1.0f);
                doty = gdx3ds_clampf(doty, -1.0f, 1.0f);
                if (s->textureGenLinear) {
                    dotx = acosf(-dotx) * 0.159155f;
                    doty = acosf(-doty) * 0.159155f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }
                U = (int16_t)(int32_t)(dotx * s->texScaleS);
                V = (int16_t)(int32_t)(doty * s->texScaleT);
            }
        }
    } else {
        d->r = v->cn[0];
        d->g = v->cn[1];
        d->b = v->cn[2];
    }

    d->u = U;
    d->v = V;

    d->clip_rej = 0;
    if (x < -w) d->clip_rej |= 1;
    if (x > w) d->clip_rej |= 2;
    if (y < -w) d->clip_rej |= 4;
    if (y > w) d->clip_rej |= 8;
    if (z > w) d->clip_rej |= 32;

    d->x = x;
    d->y = y;
    d->z = z;
    d->w = w;
    d->clip_out = gdx3ds_vtx_clip_out_mask(d);

    if (s->fog) {
        float fogRatio;
        float fog_z;
        if (w <= 0.001f) {
            fogRatio = -1.0f;
        } else {
            fogRatio = gdx3ds_clampf(z / w, -1.0f, 1.0f);
        }
        fog_z = fogRatio * s->fogMul + s->fogOffset;
        fog_z = gdx3ds_clampf(fog_z, 0.0f, 255.0f);
        d->a = (uint8_t)fog_z;
    } else if (hasF3dflxAlpha) {
        d->a = f3dflxAlpha;
    } else {
        d->a = v->cn[3];
    }
}

/* ---- GfxSpTri1 pre-draw stages ------------------------------------------------------ */

/* Reject-variant box test: 1 = drop the triangle. */
static inline int gdx3ds_tri_rej_reject(const Gdx3dsLoadedVtx* v1, const Gdx3dsLoadedVtx* v2, const Gdx3dsLoadedVtx* v3) {
    const Gdx3dsLoadedVtx* v_arr[3];
    int i;
    v_arr[0] = v1;
    v_arr[1] = v2;
    v_arr[2] = v3;
    for (i = 0; i < 3; i++) {
        const Gdx3dsLoadedVtx* vertex = v_arr[i];
        const float w2 = 2.0f * vertex->w;
        if (vertex->w <= 0.0f || vertex->x < -w2 || vertex->x > w2 || vertex->y < -w2 || vertex->y > w2 ||
            vertex->z < -vertex->w) {
            return 1;
        }
    }
    return 0;
}

static inline int gdx3ds_tri_all_out(const Gdx3dsLoadedVtx* v1, const Gdx3dsLoadedVtx* v2, const Gdx3dsLoadedVtx* v3) {
    return (v1->clip_rej & v2->clip_rej & v3->clip_rej) != 0;
}

enum { GDX3DS_CULL_NONE = 0, GDX3DS_CULL_FRONT = 1, GDX3DS_CULL_BACK = 2, GDX3DS_CULL_BOTH = 3 };

/* CPU backface cull: 1 = drop. cullMode from geometry_mode & G_CULL_BOTH (F3DEX2:
 * FRONT 0x200 -> 1, BACK 0x400 -> 2, BOTH -> 3); invert = G_EX_INVERT_CULLING. */
static inline int gdx3ds_tri_cull(const Gdx3dsLoadedVtx* v1, const Gdx3dsLoadedVtx* v2, const Gdx3dsLoadedVtx* v3,
                                  int cullMode, int invert) {
    float dx1, dy1, dx2, dy2, cross;
    if (cullMode == GDX3DS_CULL_NONE) {
        return 0;
    }
    dx1 = v1->x / (v1->w) - v2->x / (v2->w);
    dy1 = v1->y / (v1->w) - v2->y / (v2->w);
    dx2 = v3->x / (v3->w) - v2->x / (v2->w);
    dy2 = v3->y / (v3->w) - v2->y / (v2->w);
    cross = dx1 * dy2 - dy1 * dx2;
    if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
        cross = -cross;
    }
    if (invert) {
        cross = -cross;
    }
    if (cullMode == GDX3DS_CULL_FRONT) {
        return cross <= 0;
    } else if (cullMode == GDX3DS_CULL_BACK) {
        return cross >= 0;
    }
    return 1; /* BOTH */
}

static inline float gdx3ds_tri_clip_distance(const Gdx3dsLoadedVtx* v, uint32_t plane) {
    switch (plane) {
        case 0: return v->w - GDX3DS_VTX_MIN_CLIP_W;
        case 1: return v->x + v->w;
        case 2: return v->w - v->x;
        case 3: return v->y + v->w;
        case 4: return v->w - v->y;
        case 5: return v->z + v->w;
        default: return v->w - v->z;
    }
}

static inline uint8_t gdx3ds_lerp_byte(uint8_t a, uint8_t b, float t) {
    const float r = gdx3ds_clampf(a + (b - a) * t, 0.0f, 255.0f);
    return (uint8_t)r;
}

static inline void gdx3ds_vtx_interpolate(const Gdx3dsLoadedVtx* a, const Gdx3dsLoadedVtx* b, float t, Gdx3dsLoadedVtx* r) {
    memset(r, 0, sizeof(*r));
    r->x = a->x + (b->x - a->x) * t;
    r->y = a->y + (b->y - a->y) * t;
    r->z = a->z + (b->z - a->z) * t;
    r->w = a->w + (b->w - a->w) * t;
    r->u = a->u + (b->u - a->u) * t;
    r->v = a->v + (b->v - a->v) * t;
    r->r = gdx3ds_lerp_byte(a->r, b->r, t);
    r->g = gdx3ds_lerp_byte(a->g, b->g, t);
    r->b = gdx3ds_lerp_byte(a->b, b->b, t);
    r->a = gdx3ds_lerp_byte(a->a, b->a, t);
}

/* UpdateLoadedVertexClipFlags (fan vertices, after the w clamp). */
static inline void gdx3ds_vtx_update_clip_flags(Gdx3dsLoadedVtx* v) {
    v->clip_out = gdx3ds_vtx_clip_out_mask(v);
    v->clip_rej = 0;
    if (v->x < -v->w) v->clip_rej |= 1;
    if (v->x > v->w) v->clip_rej |= 2;
    if (v->y < -v->w) v->clip_rej |= 4;
    if (v->y > v->w) v->clip_rej |= 8;
    if (v->z < -v->w) v->clip_rej |= 16;
    if (v->z > v->w) v->clip_rej |= 32;
}

#define GDX3DS_CLIP_POLY_MAX 16

/* DROPSITES site 3, mesh half. The cap below is the LUS/owned-interpreter cap, kept identical
 * on purpose (gdx_tri.cpp carries the same reasoning): clipping a CONVEX polygon by a half-
 * space adds at most one vertex, so from a triangle the output is bounded by 3 + 7 = 10 and
 * the 16-slot buffer has slack it cannot use. The cap is counted rather than raised -- raising
 * it would diverge from the path this clipper exists to match. One copy per translation unit
 * (the mesh walker's is the live one; the host tests get their own and ignore it). */
static uint32_t gdx3ds_clip_poly_hw __attribute__((unused)) = 0;
static uint32_t gdx3ds_clip_poly_trunc __attribute__((unused)) = 0;

/* Homogeneous 7-plane clip of one triangle (requiresClipping = OR of clip_out != 0).
 * Writes the polygon to out (<= 16), returns its vertex count (< 3 = fully clipped).
 * The caller fans (0, i, i+1) for i in 1..n-2, clamps each fan vertex's w to
 * GDX3DS_VTX_MIN_CLIP_W and refreshes its clip flags (gdx3ds_vtx_update_clip_flags),
 * then applies gdx3ds_tri_all_out + gdx3ds_tri_cull exactly as GfxSpTri1's
 * sClipFanDepth > 0 re-entry does. */
static inline int gdx3ds_tri_clip(const Gdx3dsLoadedVtx* v1, const Gdx3dsLoadedVtx* v2, const Gdx3dsLoadedVtx* v3,
                                  Gdx3dsLoadedVtx* out) {
    Gdx3dsLoadedVtx polygonA[GDX3DS_CLIP_POLY_MAX];
    Gdx3dsLoadedVtx polygonB[GDX3DS_CLIP_POLY_MAX];
    Gdx3dsLoadedVtx* input = polygonA;
    Gdx3dsLoadedVtx* output = polygonB;
    size_t inputCount = 3;
    uint32_t plane;
    size_t i;
    /* B26: two 512-byte zero-fills stood here. They were dead stores on the per-clipped-
     * triangle owned path -- no lever, so paid in the shipped build.
     *
     * They are NOT removed because "only polygonA[0..2] are read". That was the filed
     * premise and it is WRONG: the loop below ping-pongs, so polygonA is the INPUT buffer on
     * planes 0/2/4/6 and is read at [0..inputCount-1], up to the cap, not just [0..2].
     *
     * They are removed because no slot of EITHER buffer is ever read before this function
     * writes it. outputCount is incremented only on the two arms that write the slot they
     * consume (the struct copy at the `currentInside` arm, and the interpolate), never on
     * the truncation arms -- so on exit from a plane, output[0..outputCount-1] is exactly
     * the set of slots written. `inputCount = outputCount` plus the swap then makes the next
     * plane's read range exactly that prefix, and induction from polygonA[0..2] = v1,v2,v3
     * below carries it to every plane and to the final copy into `out`. Both buffers are
     * covered symmetrically, which is why both memsets go and the dead store was 1024 B per
     * clipped triangle rather than 512.
     *
     * Escape routes closed in docs/research/native/vtxmath-progress.md B26 §2-§3: aliasing /
     * escaped pointers, callers reading past the returned count, partially-written structs
     * (gdx3ds_vtx_interpolate does its OWN memset, so interpolated vertices are unaffected),
     * a byte-wise read of the named pad[2] member (read nowhere in the tree), and the clip
     * flags (both call sites recompute them via gdx3ds_vtx_update_clip_flags). The argument
     * is also EXECUTED: docs/research/native/vtxmath-evidence/ poisons 64 KiB of stack with
     * 0xCD before every call and compares 198683 clipped triangles against a zero-initialised
     * reference, pad[2] and clip flags included -- 610969 vertex checks, 0 failures, with a
     * control proving the poison reaches this frame.
     *
     * Size, measured, so nobody re-finds this expecting a win: hardware session
     * hw-crash-0923 averages 8.5 clipped triangles per frame, 18.6 over frames that clip at
     * all, peak window 135. At ~250-500 ARM11 cycles per 1024 B (estimate, not a
     * measurement) that is ~0.003-0.005 ms/frame typical and ~0.04-0.08 ms/frame at the worst
     * window of a whole session -- correctness tidying, not a perf lever. */
    polygonA[0] = *v1;
    polygonA[1] = *v2;
    polygonA[2] = *v3;
    for (plane = 0; plane < 7 && inputCount >= 3; ++plane) {
        size_t outputCount = 0;
        int truncated = 0;
        for (i = 0; i < inputCount; ++i) {
            const Gdx3dsLoadedVtx* current = &input[i];
            const Gdx3dsLoadedVtx* next = &input[(i + 1) % inputCount];
            const float currentDistance = gdx3ds_tri_clip_distance(current, plane);
            const float nextDistance = gdx3ds_tri_clip_distance(next, plane);
            const int currentInside = currentDistance >= 0.0f;
            const int nextInside = nextDistance >= 0.0f;
            if (currentInside) {
                if (outputCount < GDX3DS_CLIP_POLY_MAX) {
                    output[outputCount++] = *current;
                } else {
                    truncated = 1;
                }
            }
            if (currentInside != nextInside && outputCount >= GDX3DS_CLIP_POLY_MAX) {
                truncated = 1;
            }
            if (currentInside != nextInside && outputCount < GDX3DS_CLIP_POLY_MAX) {
                const float denominator = currentDistance - nextDistance;
                const float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
                gdx3ds_vtx_interpolate(current, next, gdx3ds_clampf(t, 0.0f, 1.0f), &output[outputCount++]);
            }
        }
        if (outputCount > (size_t)gdx3ds_clip_poly_hw) {
            gdx3ds_clip_poly_hw = (uint32_t)outputCount;
        }
        if (truncated) {
            gdx3ds_clip_poly_trunc++;
        }
        inputCount = outputCount;
        {
            Gdx3dsLoadedVtx* tmp = input;
            input = output;
            output = tmp;
        }
    }
    for (i = 0; i < inputCount; i++) {
        out[i] = input[i];
    }
    return (int)inputCount;
}

/* ---- [trifast] UV normalisation (GfxSpTri1 packed emission) ------------------------- */
typedef struct Gdx3dsUvPrep {
    float uOff, vOff;         /* uls/4, ult/4 */
    float uPre, vPre;         /* 2^-shift for shifts 1..10 (exact), else 2^(16-shift) */
    float uNormMul, vNormMul; /* exact reciprocal when the extent is a power of two */
    float uNormDiv, vNormDiv;
    int uMul, vMul;
    int halfTexel;            /* G_TF_BILERP */
} Gdx3dsUvPrep;

static inline void gdx3ds_uv_prep(Gdx3dsUvPrep* p, int shifts, int shiftt, uint32_t uls, uint32_t ult, uint32_t texW,
                                  uint32_t texH, int bilerp) {
    p->uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / (float)(1 << shifts) : (float)(1 << (16 - shifts)));
    p->vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / (float)(1 << shiftt) : (float)(1 << (16 - shiftt)));
    p->uOff = uls / 4.0f;
    p->vOff = ult / 4.0f;
    p->uMul = (texW & (texW - 1u)) == 0u;
    p->vMul = (texH & (texH - 1u)) == 0u;
    p->uNormDiv = (float)texW;
    p->vNormDiv = (float)texH;
    p->uNormMul = p->uMul ? 1.0f / (float)texW : 1.0f;
    p->vNormMul = p->vMul ? 1.0f / (float)texH : 1.0f;
    p->halfTexel = bilerp;
}

/* newlib s_fminf.c semantics (the interpreter's GdxFminExact). */
static inline float gdx3ds_fmin_exact(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

/* LoadedVertex u/v -> padded, clamped, (atlas-offset) PICA UV: out[slot], out[slot+1]. */
static inline void gdx3ds_uv_pack(const Gdx3dsUvPrep* p, float vu, float vv, float uScale, float vScale, float clampU,
                                  float clampV, int hasOff, float uOff, float vOff, float* outU, float* outV) {
    float u = vu / 32.0f;
    float v = vv / 32.0f;
    float nu, nv, pu, pv;
    u *= p->uPre;
    v *= p->vPre;
    u -= p->uOff;
    v -= p->vOff;
    if (p->halfTexel) {
        u += 0.5f;
        v += 0.5f;
    }
    nu = p->uMul ? u * p->uNormMul : u / p->uNormDiv;
    nv = p->vMul ? v * p->vNormMul : v / p->vNormDiv;
    pu = gdx3ds_fmin_exact(nu * uScale, clampU);
    pv = gdx3ds_fmin_exact(nv * vScale, clampV);
    if (hasOff) {
        *outU = pu + uOff;
        *outV = pv + vOff;
    } else {
        *outU = pu;
        *outV = pv;
    }
}

/* uint8 -> [0,1] float, bit-identical to (int)b / 255.0f (interpreter sGdxByteToUnit). */
static inline float gdx3ds_byte_to_unit(uint8_t b) {
    return (int)b / 255.0f;
}

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_VTX_MATH_H */
