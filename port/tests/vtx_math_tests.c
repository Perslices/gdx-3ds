/*
 * vtx_math_tests.c -- test C of the machines harness: port/3ds/gfx/gdx3ds_vtx_math.h
 * against a REFERENCE transcription of libultraship/src/fast/interpreter.cpp GfxSpVertex /
 * GfxSpTri1 (kept in the interpreter's own shape: per-vertex locals, its clamp helpers, its
 * std::clamp/int conversions) over random inputs. Every comparison is bit-exact: a
 * tolerance would hide exactly the sub-ULP drift that flips a triangle across a plane.
 *
 * Also checks the [trifast] UV multiply-by-reciprocal against the legacy divide
 * expression (exact by construction for power-of-two extents) and the reject / cull /
 * clip-fan decisions on random clip-space triangles.
 *
 * Compile with -ffp-contract=off (the harness script does) so the host cannot fuse.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_vtx_math.h"

static int g_fail = 0;
static unsigned long g_checks = 0;

/* ---- reference: verbatim-shaped transcription --------------------------------------- */
typedef struct {
    float x, y, z, w, u, v;
    struct { uint8_t r, g, b, a; } color;
    uint8_t clip_rej, clip_out;
} RefVertex;

static float ref_clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static int ref_clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void RefVertexTransform(const Gdx3dsVtxMathState* s, const Gdx3dsVtxIn* vin, RefVertex* d) {
    const int16_t* ob = vin->ob;
    const int8_t* vn = (const int8_t*)vin->cn;
    float x = ob[0] * s->mp[0][0] + ob[1] * s->mp[1][0] + ob[2] * s->mp[2][0] + s->mp[3][0];
    float y = ob[0] * s->mp[0][1] + ob[1] * s->mp[1][1] + ob[2] * s->mp[2][1] + s->mp[3][1];
    float z = ob[0] * s->mp[0][2] + ob[1] * s->mp[1][2] + ob[2] * s->mp[2][2] + s->mp[3][2];
    float w = ob[0] * s->mp[0][3] + ob[1] * s->mp[1][3] + ob[2] * s->mp[2][3] + s->mp[3][3];
    short U, V;
    int hasF3dflxAlpha = 0;
    uint8_t f3dflxAlpha = 0;
    if (s->xMode == 2) {
        const float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
        x = x * kSafeAreaScale;
    } else if (s->xMode == 1) {
        x = x * (4.0f / 3.0f) / s->aspect;
    }
    U = vin->tc[0] * s->texScaleS >> 16;
    V = vin->tc[1] * s->texScaleT >> 16;
    if (s->lighting) {
        int r = s->ambient[0];
        int g = s->ambient[1];
        int b = s->ambient[2];
        int i;
        for (i = 0; i < s->numDir; i++) {
            float intensity = 0;
            intensity += vn[0] * s->coeffs[i][0];
            intensity += vn[1] * s->coeffs[i][1];
            intensity += vn[2] * s->coeffs[i][2];
            intensity /= 127.0f;
            if (intensity > 0.0f) {
                r += intensity * s->lightCol[i][0];
                g += intensity * s->lightCol[i][1];
                b += intensity * s->lightCol[i][2];
            }
        }
        d->color.r = r > 255 ? 255 : r;
        d->color.g = g > 255 ? 255 : g;
        d->color.b = b > 255 ? 255 : b;
        if (s->textureGen) {
            if (s->flxAlpha) {
                float alphaLight[3];
                alphaLight[0] = s->alphaLight[0];
                alphaLight[1] = s->alphaLight[1];
                alphaLight[2] = s->alphaLight[2];
                {
                    const float intensity = alphaLight[0] * vn[0] + alphaLight[1] * vn[1] + alphaLight[2] * vn[2];
                    const int lutIndex = ref_clampi((int)intensity, -128, 127);
                    const int dmemIndex = (int)s->dmaIoDmem + 128 + lutIndex;
                    if (dmemIndex >= 0 && dmemIndex < 4096) {
                        f3dflxAlpha = s->dmem[dmemIndex];
                        hasF3dflxAlpha = 1;
                    }
                }
            } else {
                float dotx = 0, doty = 0;
                dotx += vn[0] * s->lookat[0][0];
                dotx += vn[1] * s->lookat[0][1];
                dotx += vn[2] * s->lookat[0][2];
                doty += vn[0] * s->lookat[1][0];
                doty += vn[1] * s->lookat[1][1];
                doty += vn[2] * s->lookat[1][2];
                dotx /= 127.0f;
                doty /= 127.0f;
                dotx = ref_clamp(dotx, -1.0f, 1.0f);
                doty = ref_clamp(doty, -1.0f, 1.0f);
                if (s->textureGenLinear) {
                    dotx = acosf(-dotx) * 0.159155f;
                    doty = acosf(-doty) * 0.159155f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }
                U = (int32_t)(dotx * s->texScaleS);
                V = (int32_t)(doty * s->texScaleT);
            }
        }
    } else {
        d->color.r = vin->cn[0];
        d->color.g = vin->cn[1];
        d->color.b = vin->cn[2];
    }
    d->u = U;
    d->v = V;
    d->clip_rej = 0;
    if (x < -w) d->clip_rej |= 1;
    if (x > w) d->clip_rej |= 2;
    if (y < -w) d->clip_rej |= 4;
    if (y > w) d->clip_rej |= 8;
    if (z > w) d->clip_rej |= 32;
    {
        uint8_t clipOut = 0;
        if (w - 1.0e-5f < 0.0f) clipOut |= 1u << 0;
        if (x + w < 0.0f) clipOut |= 1u << 1;
        if (w - x < 0.0f) clipOut |= 1u << 2;
        if (y + w < 0.0f) clipOut |= 1u << 3;
        if (w - y < 0.0f) clipOut |= 1u << 4;
        if (z + w < 0.0f) clipOut |= 1u << 5;
        if (w - z < 0.0f) clipOut |= 1u << 6;
        d->clip_out = clipOut;
    }
    d->x = x;
    d->y = y;
    d->z = z;
    d->w = w;
    if (s->fog) {
        float fogRatio;
        float fog_z;
        if (w <= 0.001f) {
            fogRatio = -1.0f;
        } else {
            fogRatio = ref_clamp(z / w, -1.0f, 1.0f);
        }
        fog_z = fogRatio * s->fogMul + s->fogOffset;
        fog_z = ref_clamp(fog_z, 0.0f, 255.0f);
        d->color.a = fog_z;
    } else if (hasF3dflxAlpha) {
        d->color.a = f3dflxAlpha;
    } else {
        d->color.a = vin->cn[3];
    }
}

/* ---- random helpers ------------------------------------------------------------------ */
static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static float rndf(float lo, float hi) {
    return lo + (hi - lo) * ((rnd() & 0xFFFFFF) / 16777215.0f);
}

static void RandomState(Gdx3dsVtxMathState* s, uint8_t* dmem, float modelview[4][4]) {
    int i, j;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s->mp[i][j] = rndf(-2.0f, 2.0f);
            modelview[i][j] = rndf(-1.5f, 1.5f);
        }
    }
    s->mp[3][3] = rndf(50.0f, 4000.0f); /* keep w plausible */
    s->xMode = (int)(rnd() % 3);
    s->aspect = rndf(1.0f, 2.4f);
    s->lighting = (rnd() & 3) != 0;
    s->numDir = 1 + (int)(rnd() % 3);
    for (i = 0; i < s->numDir; i++) {
        int8_t dir[3] = { (int8_t)(rnd() & 0xFF), (int8_t)(rnd() & 0xFF), (int8_t)(rnd() & 0xFF) };
        if (dir[0] == 0 && dir[1] == 0 && dir[2] == 0) dir[0] = 69;
        gdx3ds_vtx_light_coeffs(dir, (const float (*)[4])modelview, s->coeffs[i]);
        for (j = 0; j < 3; j++) s->lightCol[i][j] = (uint8_t)(rnd() & 0xFF);
    }
    for (j = 0; j < 3; j++) s->ambient[j] = (uint8_t)(rnd() % 120);
    s->textureGen = (rnd() & 3) != 0;
    s->textureGenLinear = (rnd() & 1);
    s->flxAlpha = (rnd() & 3) != 0;
    {
        float al[3] = { rndf(-1, 1), rndf(-1, 1), rndf(-1, 1) };
        if (al[0] == 0 && al[1] == 0) al[0] = 0.5f;
        gdx3ds_vtx_alpha_light(al, (const float (*)[4])modelview, s->alphaLight);
    }
    for (i = 0; i < 4096; i++) dmem[i] = (uint8_t)(rnd() & 0xFF);
    s->dmem = dmem;
    s->dmaIoDmem = (rnd() & 7) == 0 ? 3900 : 0x8B0; /* sometimes out of range */
    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++) s->lookat[i][j] = rndf(-1, 1);
    s->fog = (rnd() & 7) == 0;
    s->fogMul = (int16_t)(rnd() & 0xFFFF);
    s->fogOffset = (int16_t)(rnd() & 0xFFFF);
    s->texScaleS = (rnd() & 1) ? 0xFFFF : (uint16_t)(rnd() & 0xFFFF);
    s->texScaleT = (rnd() & 1) ? 0xFFFF : (uint16_t)(rnd() & 0xFFFF);
}

static void RandomVtx(Gdx3dsVtxIn* v) {
    int i;
    for (i = 0; i < 3; i++) v->ob[i] = (int16_t)(rnd() % 1200) - 600;
    v->flag = 0;
    v->tc[0] = (int16_t)(rnd() & 0xFFFF);
    v->tc[1] = (int16_t)(rnd() & 0xFFFF);
    for (i = 0; i < 4; i++) v->cn[i] = (uint8_t)(rnd() & 0xFF);
}

static void check(int ok, const char* what, unsigned long i) {
    g_checks++;
    if (!ok) {
        if (g_fail < 20) {
            printf("[FAIL] %s at iteration %lu\n", what, i);
        }
        g_fail++;
    }
}

/* ---- reference cull / clip (GfxSpTri1 shape) ----------------------------------------- */
static int RefCull(const Gdx3dsLoadedVtx* v1, const Gdx3dsLoadedVtx* v2, const Gdx3dsLoadedVtx* v3, int mode, int inv) {
    float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
    float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
    float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
    float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
    float cross = dx1 * dy2 - dy1 * dx2;
    if (mode == 0) return 0;
    if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) cross = -cross;
    if (inv) cross = -cross;
    if (mode == 1) return cross <= 0;
    if (mode == 2) return cross >= 0;
    return 1;
}

int main(void) {
    static uint8_t dmem[4096];
    float modelview[4][4];
    Gdx3dsVtxMathState st;
    unsigned long i;
    unsigned long alphaLut = 0, texgen = 0, fogged = 0;
    printf("=== gdx3ds_vtx_math.h vs reference transcription ===\n");

    /* 1. vertex transform: 20k vertices across 200 random states */
    for (i = 0; i < 20000; i++) {
        Gdx3dsVtxIn v;
        Gdx3dsLoadedVtx h;
        RefVertex r;
        if (i % 100 == 0) RandomState(&st, dmem, modelview);
        RandomVtx(&v);
        memset(&h, 0, sizeof(h));
        memset(&r, 0, sizeof(r));
        gdx3ds_vtx_transform(&st, &v, &h);
        RefVertexTransform(&st, &v, &r);
        check(memcmp(&h.x, &r.x, 4 * sizeof(float)) == 0, "xyzw", i);
        check(h.u == r.u && h.v == r.v, "uv", i);
        check(h.r == r.color.r && h.g == r.color.g && h.b == r.color.b, "shade rgb", i);
        check(h.a == r.color.a, "alpha", i);
        check(h.clip_rej == r.clip_rej && h.clip_out == r.clip_out, "clip flags", i);
        if (st.lighting && st.textureGen && st.flxAlpha) alphaLut++;
        if (st.lighting && st.textureGen && !st.flxAlpha) texgen++;
        if (st.fog) fogged++;
    }
    printf("vertex transform: 20000 vertices (alphaLut=%lu texgen=%lu fog=%lu) bit-exact\n", alphaLut, texgen, fogged);

    /* 2. reject / all-out / cull / clip-fan on random clip-space triangles */
    {
        unsigned long rej = 0, out = 0, cull = 0, clipped = 0, fans = 0;
        for (i = 0; i < 20000; i++) {
            Gdx3dsLoadedVtx t[3];
            int k, mode = (int)(rnd() % 4), inv = (int)(rnd() & 1);
            for (k = 0; k < 3; k++) {
                memset(&t[k], 0, sizeof(t[k]));
                t[k].w = rndf(-10.0f, 200.0f);
                t[k].x = rndf(-3.0f, 3.0f) * fabsf(t[k].w);
                t[k].y = rndf(-3.0f, 3.0f) * fabsf(t[k].w);
                t[k].z = rndf(-2.0f, 2.0f) * fabsf(t[k].w);
                t[k].u = rndf(-1000, 1000);
                t[k].v = rndf(-1000, 1000);
                t[k].r = (uint8_t)rnd();
                t[k].g = (uint8_t)rnd();
                t[k].b = (uint8_t)rnd();
                t[k].a = (uint8_t)rnd();
                gdx3ds_vtx_update_clip_flags(&t[k]);
            }
            /* reject: reference inline */
            {
                int refRej = 0;
                for (k = 0; k < 3; k++) {
                    const float w2 = 2.0f * t[k].w;
                    if (t[k].w <= 0.0f || t[k].x < -w2 || t[k].x > w2 || t[k].y < -w2 || t[k].y > w2 || t[k].z < -t[k].w) refRej = 1;
                }
                check(gdx3ds_tri_rej_reject(&t[0], &t[1], &t[2]) == refRej, "rej reject", i);
                if (refRej) rej++;
            }
            check(gdx3ds_tri_all_out(&t[0], &t[1], &t[2]) == ((t[0].clip_rej & t[1].clip_rej & t[2].clip_rej) != 0), "all-out", i);
            if (gdx3ds_tri_all_out(&t[0], &t[1], &t[2])) out++;
            check(gdx3ds_tri_cull(&t[0], &t[1], &t[2], mode, inv) == RefCull(&t[0], &t[1], &t[2], mode, inv), "cull", i);
            if (gdx3ds_tri_cull(&t[0], &t[1], &t[2], mode, inv)) cull++;
            if ((t[0].clip_out | t[1].clip_out | t[2].clip_out) != 0) {
                /* reference clipper, structurally the interpreter's loop */
                Gdx3dsLoadedVtx A[16], B[16], H[16];
                Gdx3dsLoadedVtx* in = A;
                Gdx3dsLoadedVtx* outp = B;
                size_t n = 3, j;
                uint32_t plane;
                int hn;
                memset(A, 0, sizeof(A));
                memset(B, 0, sizeof(B));
                A[0] = t[0];
                A[1] = t[1];
                A[2] = t[2];
                for (plane = 0; plane < 7 && n >= 3; ++plane) {
                    size_t on = 0;
                    for (j = 0; j < n; ++j) {
                        const Gdx3dsLoadedVtx* cur = &in[j];
                        const Gdx3dsLoadedVtx* nxt = &in[(j + 1) % n];
                        float cd, nd;
                        switch (plane) {
                            case 0: cd = cur->w - 1.0e-5f; nd = nxt->w - 1.0e-5f; break;
                            case 1: cd = cur->x + cur->w; nd = nxt->x + nxt->w; break;
                            case 2: cd = cur->w - cur->x; nd = nxt->w - nxt->x; break;
                            case 3: cd = cur->y + cur->w; nd = nxt->y + nxt->w; break;
                            case 4: cd = cur->w - cur->y; nd = nxt->w - nxt->y; break;
                            case 5: cd = cur->z + cur->w; nd = nxt->z + nxt->w; break;
                            default: cd = cur->w - cur->z; nd = nxt->w - nxt->z; break;
                        }
                        if (cd >= 0.0f && on < 16) outp[on++] = *cur;
                        if ((cd >= 0.0f) != (nd >= 0.0f) && on < 16) {
                            const float den = cd - nd;
                            float tt = den != 0.0f ? cd / den : 0.0f;
                            Gdx3dsLoadedVtx res;
                            tt = ref_clamp(tt, 0.0f, 1.0f);
                            memset(&res, 0, sizeof(res));
                            res.x = cur->x + (nxt->x - cur->x) * tt;
                            res.y = cur->y + (nxt->y - cur->y) * tt;
                            res.z = cur->z + (nxt->z - cur->z) * tt;
                            res.w = cur->w + (nxt->w - cur->w) * tt;
                            res.u = cur->u + (nxt->u - cur->u) * tt;
                            res.v = cur->v + (nxt->v - cur->v) * tt;
                            res.r = (uint8_t)ref_clamp(cur->r + (nxt->r - cur->r) * tt, 0.0f, 255.0f);
                            res.g = (uint8_t)ref_clamp(cur->g + (nxt->g - cur->g) * tt, 0.0f, 255.0f);
                            res.b = (uint8_t)ref_clamp(cur->b + (nxt->b - cur->b) * tt, 0.0f, 255.0f);
                            res.a = (uint8_t)ref_clamp(cur->a + (nxt->a - cur->a) * tt, 0.0f, 255.0f);
                            outp[on++] = res;
                        }
                    }
                    n = on;
                    {
                        Gdx3dsLoadedVtx* tmp = in;
                        in = outp;
                        outp = tmp;
                    }
                }
                hn = gdx3ds_tri_clip(&t[0], &t[1], &t[2], H);
                check(hn == (int)n, "clip polygon count", i);
                if (hn == (int)n) {
                    for (j = 0; j < n; j++) {
                        check(memcmp(&H[j].x, &in[j].x, 6 * sizeof(float)) == 0 && H[j].r == in[j].r && H[j].g == in[j].g &&
                                  H[j].b == in[j].b && H[j].a == in[j].a,
                              "clip polygon vertex", i);
                    }
                }
                clipped++;
                if (n >= 3) fans += n - 2;
            }
        }
        printf("triangles: 20000 (rejected=%lu allOut=%lu culled=%lu clipped=%lu fanTris=%lu) decisions bit-exact\n", rej,
               out, cull, clipped, fans);
    }

    /* 3. UV: trifast prep/pack vs the legacy divide expression */
    {
        unsigned long pow2 = 0;
        for (i = 0; i < 40000; i++) {
            Gdx3dsUvPrep p;
            int shifts = (int)(rnd() % 16), shiftt = (int)(rnd() % 16);
            uint32_t uls = rnd() % 512, ult = rnd() % 512;
            uint32_t tw = (rnd() & 1) ? (1u << (rnd() % 8)) : 1 + rnd() % 300;
            uint32_t th = (rnd() & 1) ? (1u << (rnd() % 8)) : 1 + rnd() % 300;
            int bilerp = (int)(rnd() & 1), hasOff = (int)(rnd() & 1);
            float U = (float)(int16_t)(rnd() & 0xFFFF), V = (float)(int16_t)(rnd() & 0xFFFF);
            float uScale = 1.0f / (float)(1u << (rnd() % 4)), vScale = 1.0f / (float)(1u << (rnd() % 4));
            float cu = (rnd() & 1) ? 1e30f : rndf(0.0f, 1.0f), cv = (rnd() & 1) ? 1e30f : rndf(0.0f, 1.0f);
            float ou = rndf(0, 1), ov = rndf(0, 1);
            float hu, hv, lu, lv;
            /* legacy expression (GfxSpTri1 non-trifast loop) */
            float u = U / 32.0f, v = V / 32.0f;
            if (shifts != 0) {
                if (shifts <= 10) u /= 1 << shifts; else u *= 1 << (16 - shifts);
            }
            if (shiftt != 0) {
                if (shiftt <= 10) v /= 1 << shiftt; else v *= 1 << (16 - shiftt);
            }
            u -= uls / 4.0f;
            v -= ult / 4.0f;
            if (bilerp) {
                u += 0.5f;
                v += 0.5f;
            }
            if (hasOff) {
                lu = fminf((u / tw) * uScale, cu) + ou;
                lv = fminf((v / th) * vScale, cv) + ov;
            } else {
                lu = fminf((u / tw) * uScale, cu);
                lv = fminf((v / th) * vScale, cv);
            }
            gdx3ds_uv_prep(&p, shifts, shiftt, uls, ult, tw, th, bilerp);
            gdx3ds_uv_pack(&p, U, V, uScale, vScale, cu, cv, hasOff, ou, ov, &hu, &hv);
            check(hu == lu && hv == lv, "uv pack vs legacy", i);
            if (p.uMul) pow2++;
        }
        printf("uv: 40000 (pow2 U extents=%lu) bit-exact vs the legacy divide\n", pow2);
    }
    /* 4. byte->unit table equivalence */
    for (i = 0; i < 256; i++) {
        check(gdx3ds_byte_to_unit((uint8_t)i) == (float)((int)i / 255.0f), "byte to unit", i);
    }

    printf("checks=%lu failures=%d\n", g_checks, g_fail);
    if (g_fail != 0) {
        printf("FAILURES: %d\n", g_fail);
        return 1;
    }
    printf("ALL PASS (vtx math: transform, reject/cull/clip, uv)\n");
    return 0;
}
