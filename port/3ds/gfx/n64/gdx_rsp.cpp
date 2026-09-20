/* port/3ds/gfx/n64/gdx_rsp.cpp -- RSP: matrices (11-deep), lights (33) + lookat, viewport,
 * G_VTX transform/light/texgen/fog/clip flags, MODIFYVTX, DMA_IO (F3DFLX alpha LUT), LINE3D.
 * Bodies mirror GfxSpMatrix / GfxSpPopMatrix / GfxSpVertex / GfxSpMovememF3dex2 / GfxSpDmaIo /
 * GfxSpLine3DGdx / CalculateNormalDir (interpreter.cpp 3226-3842, 5620-5800) in float32,
 * same op order (plan §3.5). Vertex/matrix operands from a raw big-endian list are read through
 * a byte-swapping accessor (replaces MakePersistentVtxCopy / MakePersistentMtxCopy). */
#include "gdx_interp.h"

#include <math.h>
#include <string.h>

namespace gdxn {

static constexpr float kMinClipW = 1.0e-5f;

static inline float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void NormalizeVector(float v[3]) {
    const float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void TransposedMatrixMul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void MatrixMul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

void Interp::CalculateNormalDir(const Light* light, float coeffs[3]) {
    float light_dir[3] = { light->dir[0] / 127.0f, light->dir[1] / 127.0f, light->dir[2] / 127.0f };
    TransposedMatrixMul(coeffs, light_dir, mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1]);
    NormalizeVector(coeffs);
}

float Interp::AdjXForAspectRatio(float x) const {
    if (!mCfg.widescreen || mCfg.forceFixedAspect) {
        return x;
    }
    return x * (4.0f / 3.0f) / (static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH));
}

void Interp::SpMatrix(uint8_t parameters, const Cmd& c, const ListRef& list) {
    ProfScope prof(mBackend, 6);
    const uintptr_t p = mSrc.ResolveData(c, 64, /*preferPhysical=*/true, reinterpret_cast<uintptr_t>(list.base));
    if (p == 0 || (p & 7u) != 0 || DlSrc::ReadableBytes(p) < 64u) {
        mStats.mtxUnresolved++;
        return; /* the bridge drops the command ([mtx-dropped]) */
    }
    int32_t words[16];
    memcpy(words, reinterpret_cast<const void*>(p), sizeof(words));
    if (list.isBig) {
        for (int i = 0; i < 16; i++) {
            words[i] = static_cast<int32_t>(Bswap32(static_cast<uint32_t>(words[i])));
        }
    }
    const int32_t* addr = words;
    float matrix[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t int_part = addr[i * 2 + j / 2];
            const uint32_t frac_part = static_cast<uint32_t>(addr[8 + i * 2 + j / 2]);
            matrix[i][j] = static_cast<int32_t>((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = static_cast<int32_t>((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
    if (parameters & MTX_PROJECTION) {
        if (parameters & MTX_LOAD) {
            memcpy(mRsp.P_matrix, matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp.P_matrix, matrix, mRsp.P_matrix);
        }
    } else {
        if ((parameters & MTX_PUSH) && mRsp.modelview_matrix_stack_size < 11) {
            ++mRsp.modelview_matrix_stack_size;
            memcpy(mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1],
                   mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & MTX_LOAD) {
            if (mRsp.modelview_matrix_stack_size == 0) ++mRsp.modelview_matrix_stack_size;
            memcpy(mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1], matrix,
                      mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1]);
        }
        mRsp.lights_changed = true;
    }
    MatrixMul(mRsp.MP_matrix, mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1], mRsp.P_matrix);
}

void Interp::SpPopMatrix(uint32_t count) {
    while (count--) {
        if (mRsp.modelview_matrix_stack_size > 0) {
            --mRsp.modelview_matrix_stack_size;
            if (mRsp.modelview_matrix_stack_size > 0) {
                MatrixMul(mRsp.MP_matrix, mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1], mRsp.P_matrix);
            }
        }
    }
    mRsp.lights_changed = true;
}

static const uint8_t kFallbackVtx[16 * 64] = {};

void Interp::SpVertex(size_t n_vertices, size_t dest_index, const Cmd& c, const ListRef& list) {
    ProfScope prof(mBackend, 2);
    mStats.vtx += static_cast<uint32_t>(n_vertices);
    if (n_vertices == 0 || dest_index + n_vertices > static_cast<size_t>(kMaxVertices)) {
        return;
    }
    const size_t need = n_vertices * 16u;
    uintptr_t p = mSrc.ResolveData(c, need, /*preferPhysical=*/true, reinterpret_cast<uintptr_t>(list.base));
    if (p == 0 || DlSrc::ReadableBytes(p) < need) {
        /* Never hand the T&L a short buffer: the bridge substitutes zeroed vertices. */
        mStats.vtxUnresolved++;
        p = reinterpret_cast<uintptr_t>(kFallbackVtx);
    }
    TransformVertices(reinterpret_cast<const uint8_t*>(p), n_vertices, dest_index, list.isBig);
}

/* The transform/light/texgen/fog/clip-flag body of GfxSpVertex, from a resolved source. Also
 * the seam's SpVertex (marker replays, machine walks): host-endian Vtx, no resolution. */
void Interp::TransformVertices(const uint8_t* src, size_t n_vertices, size_t dest_index, bool isBig) {
    if (n_vertices == 0 || dest_index + n_vertices > static_cast<size_t>(kMaxVertices)) {
        return;
    }
    float mp[4][4];
    memcpy(mp, mRsp.MP_matrix, sizeof(mp));

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        Vtx vv;
        memcpy(&vv, src + i * 16u, sizeof(vv));
        if (isBig) {
            for (int k = 0; k < 3; k++) vv.ob[k] = static_cast<int16_t>(Bswap16(static_cast<uint16_t>(vv.ob[k])));
            vv.flag = Bswap16(vv.flag);
            vv.tc[0] = static_cast<int16_t>(Bswap16(static_cast<uint16_t>(vv.tc[0])));
            vv.tc[1] = static_cast<int16_t>(Bswap16(static_cast<uint16_t>(vv.tc[1])));
        }
        const Vtx* v = &vv;
        const int8_t* vn = reinterpret_cast<const int8_t*>(vv.cn);
        LoadedVertex* d = &mRsp.loaded_vertices[dest_index];

        float x = v->ob[0] * mp[0][0] + v->ob[1] * mp[1][0] + v->ob[2] * mp[2][0] + mp[3][0];
        float y = v->ob[0] * mp[0][1] + v->ob[1] * mp[1][1] + v->ob[2] * mp[2][1] + mp[3][1];
        float z = v->ob[0] * mp[0][2] + v->ob[1] * mp[1][2] + v->ob[2] * mp[2][2] + mp[3][2];
        float w = v->ob[0] * mp[0][3] + v->ob[1] * mp[1][3] + v->ob[2] * mp[2][3] + mp[3][3];

        float world_pos[3] = { 0.0f, 0.0f, 0.0f };
        if (mRsp.geometry_mode & GM_LIGHTING_POSITIONAL) {
            float(*mtx)[4] = mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1];
            world_pos[0] = v->ob[0] * mtx[0][0] + v->ob[1] * mtx[1][0] + v->ob[2] * mtx[2][0] + mtx[3][0];
            world_pos[1] = v->ob[0] * mtx[0][1] + v->ob[1] * mtx[1][1] + v->ob[2] * mtx[2][1] + mtx[3][1];
            world_pos[2] = v->ob[0] * mtx[0][2] + v->ob[1] * mtx[1][2] + v->ob[2] * mtx[2][2] + mtx[3][2];
        }

        if ((mRsp.extra_geometry_mode & 0x00000004u) != 0) { /* G_EX_WIDESCREEN_STRETCH */
            const bool widescreenFrameActive =
                (static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH)) > (4.0f / 3.0f) && mCfg.widescreen &&
                !mCfg.forceFixedAspect;
            if (widescreenFrameActive) {
                constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
                x = x * kSafeAreaScale;
            } else {
                x = AdjXForAspectRatio(x);
            }
        } else {
            x = AdjXForAspectRatio(x);
        }

        short U = static_cast<short>(v->tc[0] * mRsp.texture_scaling_factor.s >> 16);
        short V = static_cast<short>(v->tc[1] * mRsp.texture_scaling_factor.t >> 16);
        bool hasF3dflxAlpha = false;
        uint8_t f3dflxAlpha = 0;

        if (mRsp.geometry_mode & GM_LIGHTING) {
            if (mRsp.lights_changed) {
                for (int li = 0; li < mRsp.current_num_lights - 1; li++) {
                    CalculateNormalDir(&mRsp.current_lights[li].l, mRsp.current_lights_coeffs[li]);
                }
                CalculateNormalDir(&mRsp.lookat[0], mRsp.current_lookat_coeffs[0]);
                CalculateNormalDir(&mRsp.lookat[1], mRsp.current_lookat_coeffs[1]);
                mRsp.lights_changed = false;
            }
            int r = mRsp.current_lights[mRsp.current_num_lights - 1].l.col[0];
            int g = mRsp.current_lights[mRsp.current_num_lights - 1].l.col[1];
            int b = mRsp.current_lights[mRsp.current_num_lights - 1].l.col[2];

            for (int li = 0; li < mRsp.current_num_lights - 1; li++) {
                float intensity = 0;
                if ((mRsp.geometry_mode & GM_LIGHTING_POSITIONAL) && (mRsp.current_lights[li].p.unk3 != 0)) {
                    float dist_vec[3] = { mRsp.current_lights[li].p.pos[0] - world_pos[0],
                                          mRsp.current_lights[li].p.pos[1] - world_pos[1],
                                          mRsp.current_lights[li].p.pos[2] - world_pos[2] };
                    float dist_sq = dist_vec[0] * dist_vec[0] + dist_vec[1] * dist_vec[1] + dist_vec[2] * dist_vec[2] * 2;
                    float dist = sqrt(dist_sq);
                    float light_model[3];
                    TransposedMatrixMul(light_model, dist_vec,
                                        mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1]);
                    float light_intensity[3];
                    for (int k = 0; k < 3; k++) {
                        light_intensity[k] = 4.0f * light_model[k] / dist_sq;
                        light_intensity[k] = Clampf(light_intensity[k], -1.0f, 1.0f);
                    }
                    float total_intensity =
                        light_intensity[0] * vn[0] + light_intensity[1] * vn[1] + light_intensity[2] * vn[2];
                    total_intensity = Clampf(total_intensity, -1.0f, 1.0f);
                    float distf = floorf(dist);
                    float attenuation = (distf * mRsp.current_lights[li].p.unk7 * 2.0f +
                                         distf * distf * mRsp.current_lights[li].p.unkE / 8.0f) /
                                            static_cast<float>(0xFFFF) +
                                        1.0f;
                    intensity = total_intensity / attenuation;
                } else {
                    intensity += vn[0] * mRsp.current_lights_coeffs[li][0];
                    intensity += vn[1] * mRsp.current_lights_coeffs[li][1];
                    intensity += vn[2] * mRsp.current_lights_coeffs[li][2];
                    intensity /= 127.0f;
                }
                if (intensity > 0.0f) {
                    r += intensity * mRsp.current_lights[li].l.col[0];
                    g += intensity * mRsp.current_lights[li].l.col[1];
                    b += intensity * mRsp.current_lights[li].l.col[2];
                }
            }
            d->color.r = static_cast<uint8_t>(r > 255 ? 255 : r);
            d->color.g = static_cast<uint8_t>(g > 255 ? 255 : g);
            d->color.b = static_cast<uint8_t>(b > 255 ? 255 : b);

            if (mRsp.geometry_mode & GM_TEXTURE_GEN) {
                if (mVariant == 2 && mRsp.f3dflx_alpha_light_valid) {
                    float alphaLight[3];
                    TransposedMatrixMul(alphaLight, mRsp.f3dflx_alpha_light,
                                        mRsp.modelview_matrix_stack[mRsp.modelview_matrix_stack_size - 1]);
                    NormalizeVector(alphaLight);
                    const float intensity = alphaLight[0] * vn[0] + alphaLight[1] * vn[1] + alphaLight[2] * vn[2];
                    int lutIndex = static_cast<int>(intensity);
                    if (lutIndex < -128) lutIndex = -128;
                    if (lutIndex > 127) lutIndex = 127;
                    const int dmemIndex = static_cast<int>(mRsp.dma_io_dmem) + 128 + lutIndex;
                    if (dmemIndex >= 0 && dmemIndex < static_cast<int>(sizeof(mRsp.dmem))) {
                        f3dflxAlpha = mRsp.dmem[dmemIndex];
                        hasF3dflxAlpha = true;
                    }
                } else {
                    float dotx = 0, doty = 0;
                    dotx += vn[0] * mRsp.current_lookat_coeffs[0][0];
                    dotx += vn[1] * mRsp.current_lookat_coeffs[0][1];
                    dotx += vn[2] * mRsp.current_lookat_coeffs[0][2];
                    doty += vn[0] * mRsp.current_lookat_coeffs[1][0];
                    doty += vn[1] * mRsp.current_lookat_coeffs[1][1];
                    doty += vn[2] * mRsp.current_lookat_coeffs[1][2];
                    dotx /= 127.0f;
                    doty /= 127.0f;
                    dotx = Clampf(dotx, -1.0f, 1.0f);
                    doty = Clampf(doty, -1.0f, 1.0f);
                    if (mRsp.geometry_mode & GM_TEXTURE_GEN_LINEAR) {
                        dotx = acosf(-dotx) * 0.159155f;
                        doty = acosf(-doty) * 0.159155f;
                    } else {
                        dotx = (dotx + 1.0f) / 4.0f;
                        doty = (doty + 1.0f) / 4.0f;
                    }
                    U = static_cast<short>(static_cast<int32_t>(dotx * mRsp.texture_scaling_factor.s));
                    V = static_cast<short>(static_cast<int32_t>(doty * mRsp.texture_scaling_factor.t));
                }
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
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
            if (w - kMinClipW < 0.0f) clipOut |= 1u << 0;
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

        if (mRsp.geometry_mode & GM_FOG) {
            float fogRatio;
            if (w <= 0.001f) {
                fogRatio = -1.0f;
            } else {
                fogRatio = Clampf(z / w, -1.0f, 1.0f);
            }
            float fog_z = fogRatio * mRsp.fog_mul + mRsp.fog_offset;
            fog_z = Clampf(fog_z, 0.0f, 255.0f);
            d->color.a = static_cast<uint8_t>(fog_z);
        } else if (hasF3dflxAlpha) {
            d->color.a = f3dflxAlpha;
        } else {
            d->color.a = v->cn[3];
        }
    }
}

void Interp::SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val) {
    if (where != MWO_POINT_ST || idx >= kMaxVertices) {
        return;
    }
    LoadedVertex* v = &mRsp.loaded_vertices[idx];
    v->u = static_cast<int16_t>(val >> 16);
    v->v = static_cast<int16_t>(val);
}

void Interp::SpMovemem(uint8_t index, uint8_t offset, const Cmd& c, size_t xferSize, const ListRef& list) {
    const size_t need = xferSize != 0 ? xferSize : 1;
    const uintptr_t p = mSrc.ResolveData(c, need);
    if (p == 0) {
        mStats.droppedOps++;
        return;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(p);
    switch (index) {
        case MV_VIEWPORT: {
            int16_t vp[8];
            memcpy(vp, data, sizeof(vp));
            if (list.isBig) {
                for (int k = 0; k < 8; k++) vp[k] = static_cast<int16_t>(Bswap16(static_cast<uint16_t>(vp[k])));
            }
            CalcAndSetViewport(vp);
            break;
        }
        case MV_LIGHT: {
            if (mVariant == 2 && offset == 24 && mRsp.dma_io_loaded) {
                int16_t dir[3];
                memcpy(dir, data + 8, sizeof(dir));
                mRsp.f3dflx_alpha_light[0] = dir[0] / 256.0f;
                mRsp.f3dflx_alpha_light[1] = dir[1] / 256.0f;
                mRsp.f3dflx_alpha_light[2] = dir[2] / 256.0f;
                NormalizeVector(mRsp.f3dflx_alpha_light);
                mRsp.f3dflx_alpha_light_valid = true;
                break;
            }
            const int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= kMaxLights) {
                LightU lu;
                memcpy(&lu, data, sizeof(lu));
                if (list.isBig) {
                    for (int k = 0; k < 3; k++) lu.p.pos[k] = static_cast<int16_t>(Bswap16(static_cast<uint16_t>(lu.p.pos[k])));
                }
                mRsp.current_lights[lightidx] = lu;
            } else if (lightidx < 0) {
                memcpy(&mRsp.lookat[offset / 24], data, sizeof(Light));
            }
            break;
        }
        default:
            break;
    }
}

void Interp::SpDmaIo(const Cmd& c) {
    /* gsSPDma_io: w0 = [D6, flag<<23 | dmem<<13-ish | size]; LUS: write = C0(23,1),
       dmem = C0(13,10)*8, size = C0(0,12)+1, data = SegAddr(w1). */
    const bool write = C0(c.w0, 23, 1) != 0;
    const uint16_t dmem = static_cast<uint16_t>(C0(c.w0, 13, 10) * 8);
    const size_t size = static_cast<size_t>(C0(c.w0, 0, 12)) + 1;
    if (dmem >= sizeof(mRsp.dmem)) return;
    const uintptr_t p = mSrc.ResolveData(c, size);
    if (p == 0) {
        mStats.droppedOps++;
        return;
    }
    size_t copySize = sizeof(mRsp.dmem) - dmem;
    if (size < copySize) copySize = size;
    if (write) {
        /* RSP -> RDRAM: the game never reads DMEM back on this port; keep memory untouched. */
        return;
    }
    memcpy(mRsp.dmem + dmem, reinterpret_cast<const void*>(p), copySize);
    mRsp.dma_io_dmem = dmem;
    mRsp.dma_io_loaded = true;
}

static inline uint8_t ComputeClipOut(const LoadedVertex& v) {
    uint8_t m = 0;
    if (v.w - kMinClipW < 0.0f) m |= 1u << 0;
    if (v.x + v.w < 0.0f) m |= 1u << 1;
    if (v.w - v.x < 0.0f) m |= 1u << 2;
    if (v.y + v.w < 0.0f) m |= 1u << 3;
    if (v.w - v.y < 0.0f) m |= 1u << 4;
    if (v.z + v.w < 0.0f) m |= 1u << 5;
    if (v.w - v.z < 0.0f) m |= 1u << 6;
    return m;
}

void Interp::SpLine3d(uint8_t vtx1Idx, uint8_t vtx2Idx, uint8_t halfWidth) {
    if (vtx1Idx >= kMaxVertices || vtx2Idx >= kMaxVertices) return;
    const LoadedVertex a = mRsp.loaded_vertices[vtx1Idx];
    const LoadedVertex b = mRsp.loaded_vertices[vtx2Idx];
    if (a.w <= 0.0f || b.w <= 0.0f) return;
    const float ax = a.x / a.w, ay = a.y / a.w;
    const float bx = b.x / b.w, by = b.y / b.w;
    const float pixelHalfW = static_cast<float>(mCfg.curW) * 0.5f;
    const float pixelHalfH = static_cast<float>(mCfg.curH) * 0.5f;
    const float dx = (bx - ax) * pixelHalfW;
    const float dy = (by - ay) * pixelHalfH;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    const float nativeWidthPx = 1.5f + static_cast<float>(halfWidth) * 0.5f;
    const float outputScale = static_cast<float>(mCfg.curH) / 240.0f;
    const float halfPx = nativeWidthPx * 0.5f * (outputScale > 0.0f ? outputScale : 1.0f);
    const float perpX = (-dy / len) * halfPx / pixelHalfW;
    const float perpY = (dx / len) * halfPx / pixelHalfH;
    LoadedVertex* quad = &mRsp.loaded_vertices[kMaxVertices + 0];
    quad[0] = a;
    quad[1] = a;
    quad[2] = b;
    quad[3] = b;
    quad[0].x = (ax + perpX) * a.w;
    quad[0].y = (ay + perpY) * a.w;
    quad[1].x = (ax - perpX) * a.w;
    quad[1].y = (ay - perpY) * a.w;
    quad[2].x = (bx + perpX) * b.w;
    quad[2].y = (by + perpY) * b.w;
    quad[3].x = (bx - perpX) * b.w;
    quad[3].y = (by - perpY) * b.w;
    for (int k = 0; k < 4; k++) {
        quad[k].clip_rej = 0;
        quad[k].clip_out = ComputeClipOut(quad[k]);
    }
    const uint32_t savedGeometryMode = mRsp.geometry_mode;
    mRsp.geometry_mode &= ~GM_CULL_BOTH;
    SpTri(kMaxVertices + 0, kMaxVertices + 1, kMaxVertices + 2, false);
    SpTri(kMaxVertices + 1, kMaxVertices + 3, kMaxVertices + 2, false);
    mRsp.geometry_mode = savedGeometryMode;
}

} // namespace gdxn
