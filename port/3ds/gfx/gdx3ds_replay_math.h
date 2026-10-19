/* port/3ds/gfx/gdx3ds_replay_math.h -- C6 pose-replay ("60 Hz motion"): the math and the PICA
 * command-list scan, host-compilable (port/3ds/test/gdx3ds_replay_math_host_tests.cpp).
 *
 * Conventions (every one is the interpreter's or the shader's own):
 *   - N64 matrices are ROW-VECTOR: clip = (x, y, z, 1) * MP, MP = MV * P (gdx_rsp.cpp SpMatrix).
 *   - The legacy `projection` uniform is column-vector: out = Proj * pos (rows = uniform vec4s),
 *     with pos = the packed clip vertex = clip * F, F = diag(xscale, 1, .) and z' = (z + w) / 2
 *     (GetClipParameters: zIsFrom0To1, no y flip; the x scale is AdjXForAspectRatio or the
 *     widescreen-stretch factor the vertex was packed with).
 *   - The gpuxf `xmvp` uniform rows are the COLUMNS of the row-vector matrix G the vertex is
 *     multiplied by: component i = dot(xmvp[i], (ob + xoff, 1)) (Gdx3dsXfDraw::mvp comment).
 *
 * A replayed frame B redraws frame A's vertices. For a draw whose vertices went through MP_A,
 * the vertex B would have produced is v * MP_B = (v * MP_A) * K with K = MP_A^-1 * MP_B, so
 *   legacy:  Proj_B = Proj_A * (F^-1 K F)^T      (rows)
 *   gpuxf:   U_B    = K^T * U_A                   (U = the four xmvp rows)
 * computed in double. Lighting uniforms are model-space directions c = normalize(M3 * l)
 * (CalculateNormalDir), so c_B = normalize(M3_B * M3_A^-1 * c_A), rescaled to |c_A|. */
#ifndef GDX3DS_REPLAY_MATH_H
#define GDX3DS_REPLAY_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace GdxReplayMath {

/* The N64 fixed-point Mtx (16 host-order words: 8 integer halves, 8 fraction halves) to float,
 * expression for expression gdx_rsp.cpp SpMatrix. */
inline void MtxFromWords(const int32_t w[16], float m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t intPart = w[i * 2 + j / 2];
            const uint32_t fracPart = static_cast<uint32_t>(w[8 + i * 2 + j / 2]);
            m[i][j] = static_cast<int32_t>((intPart & 0xffff0000) | (fracPart >> 16)) / 65536.0f;
            m[i][j + 1] = static_cast<int32_t>((intPart << 16) | (fracPart & 0xffff)) / 65536.0f;
        }
    }
}

/* gdx_rsp.cpp MatrixMul, same operation order (the float result must be bit-identical). */
inline void MulF(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

inline void ToDouble(const float a[4][4], double d[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            d[i][j] = a[i][j];
        }
    }
}

inline void MulD(double r[4][4], const double a[4][4], const double b[4][4]) {
    double t[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

inline void Transpose(double r[4][4], const double a[4][4]) {
    double t[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[j][i];
        }
    }
    memcpy(r, t, sizeof(t));
}

/* Gauss-Jordan with partial pivoting. false for a (numerically) singular matrix. */
inline bool Invert4(const double m[4][4], double inv[4][4]) {
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i][j];
            a[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    }
    double scale = 0.0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            scale = fabs(m[i][j]) > scale ? fabs(m[i][j]) : scale;
        }
    }
    if (scale == 0.0) {
        return false;
    }
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++) {
            if (fabs(a[r][c]) > fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (fabs(a[piv][c]) <= scale * 1e-12) {
            return false;
        }
        if (piv != c) {
            for (int j = 0; j < 8; j++) {
                const double t = a[c][j];
                a[c][j] = a[piv][j];
                a[piv][j] = t;
            }
        }
        const double d = a[c][c];
        for (int j = 0; j < 8; j++) {
            a[c][j] /= d;
        }
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0.0) {
                continue;
            }
            const double f = a[r][c];
            for (int j = 0; j < 8; j++) {
                a[r][j] -= f * a[c][j];
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            inv[i][j] = a[i][j + 4];
        }
    }
    return true;
}

/* K = MP_A^-1 * MP_B (row-vector convention: clip_B = clip_A * K). */
inline bool Correction(const float mpA[4][4], const float mpB[4][4], double k[4][4]) {
    double a[4][4], b[4][4], ai[4][4];
    ToDouble(mpA, a);
    ToDouble(mpB, b);
    if (!Invert4(a, ai)) {
        return false;
    }
    MulD(k, ai, b);
    return true;
}

/* The packed-vertex map F (row-vector): pos = clip * F. */
inline void PackMatrix(float xscale, double f[4][4]) {
    memset(f, 0, sizeof(double) * 16);
    f[0][0] = xscale;
    f[1][1] = 1.0;
    f[2][2] = 0.5; /* z' = (z + w) / 2 */
    f[3][2] = 0.5;
    f[3][3] = 1.0;
}

/* Legacy draw: Proj_B = Proj_A * (F^-1 K F)^T. `projA`/`projB` are the uniform's rows. */
inline bool PatchLegacy(const float projA[4][4], const double k[4][4], float xscale, float projB[4][4]) {
    double f[4][4], fi[4][4], t[4][4], kp[4][4], kpt[4][4], pa[4][4], pb[4][4];
    PackMatrix(xscale, f);
    if (!Invert4(f, fi)) {
        return false;
    }
    MulD(t, fi, k);
    MulD(kp, t, f);
    Transpose(kpt, kp);
    ToDouble(projA, pa);
    MulD(pb, pa, kpt);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            projB[i][j] = static_cast<float>(pb[i][j]);
        }
    }
    return true;
}

/* gpuxf draw: U_B = K^T * U_A, U = the four xmvp rows. */
inline void PatchXfMvp(const float uA[4][4], const double k[4][4], float uB[4][4]) {
    double kt[4][4], ua[4][4], ub[4][4];
    Transpose(kt, k);
    ToDouble(uA, ua);
    MulD(ub, kt, ua);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            uB[i][j] = static_cast<float>(ub[i][j]);
        }
    }
}

/* track_resident re-base: the resident vertices are world s16 and the shader adds `xoff`
 * (= -eye, integers) exactly; U_A was built for eye E_A = -xoff. Moving the origin to the
 * integer eye E_B changes only the translation column: with G the row-vector matrix
 * (U = G^T), G' = T(E_B - E_A) * G, i.e. U'[i][3] = U[i][3] + sum_k d_k U[i][k]. */
inline void RebaseXf(float u[4][4], const double d[3]) {
    for (int i = 0; i < 4; i++) {
        const double t = static_cast<double>(u[i][3]) + d[0] * u[i][0] + d[1] * u[i][1] + d[2] * u[i][2];
        u[i][3] = static_cast<float>(t);
    }
}

/* 3x3 linear part R = M3_B * M3_A^-1 (rows of the modelview's upper-left block, the product
 * TransposedMatrixMul applies to a light direction: c = M3 * l). */
inline bool LightRotation(const float mvA[3][3], const float mvB[3][3], double r[3][3]) {
    double a[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            a[i][j] = mvA[i][j];
        }
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabs(det) < 1e-20) {
        return false;
    }
    double inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = mvB[i][0] * inv[0][j] + mvB[i][1] * inv[1][j] + mvB[i][2] * inv[2][j];
        }
    }
    return true;
}

/* c_B = R * c_A, rescaled to |c_A| (the uniform carries a normalised direction / 127). */
inline void RotateDir(const double r[3][3], const float in[3], float out[3]) {
    const double x = r[0][0] * in[0] + r[0][1] * in[1] + r[0][2] * in[2];
    const double y = r[1][0] * in[0] + r[1][1] * in[1] + r[1][2] * in[2];
    const double z = r[2][0] * in[0] + r[2][1] * in[1] + r[2][2] * in[2];
    const double la = sqrt(static_cast<double>(in[0]) * in[0] + static_cast<double>(in[1]) * in[1] +
                           static_cast<double>(in[2]) * in[2]);
    const double lb = sqrt(x * x + y * y + z * z);
    if (la == 0.0 || lb == 0.0) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }
    const double s = la / lb;
    out[0] = static_cast<float>(x * s);
    out[1] = static_cast<float>(y * s);
    out[2] = static_cast<float>(z * s);
}

/* ---- the PICA command list: where did a float uniform's words land? ----------------------
 *
 * libctru GPUCMD_AddInternal writes a packet as [param0][header][param1..paramN-1][pad]:
 * header bits 0-15 = register, 20-27 = N-1, bit 31 = consecutive (param k -> register + k);
 * the packet is padded to an even word count. citro3d's C3D_UpdateUniforms writes
 * FLOATUNIFORM_CONFIG = 0x80000000 | index (f32 mode) and then the vec4 words, 4 per vec4 in
 * C3D_FVec storage order (c[0..3] = w, z, y, x), through FLOATUNIFORM_DATA (any of the eight
 * DATA registers is the same port; the index auto-increments). A packet holds at most 256
 * params, so a vec4 never straddles two packets; within one packet word 0 of the first vec4 is
 * param0 (before the header), every other word is contiguous. */
enum : uint32_t {
    kRegVshFloatConfig = 0x02C0,
    kRegVshFloatData0 = 0x02C1,
    kRegVshFloatData7 = 0x02C8,
};

struct UnifWord {
    uint32_t* ptr;  /* the word in the command list */
    uint8_t index;  /* float uniform register index */
    uint8_t comp;   /* 0..3 = C3D_FVec c[comp] (w, z, y, x) */
};

/* Calls fn(const UnifWord&) for every f32-mode vertex-shader float-uniform word in
 * [begin, end), which must start on a packet boundary. Returns false if a packet runs past
 * `end` (malformed or not a boundary). f24-mode writes (shader constants) advance the index
 * but are not reported. */
template <class Fn>
inline bool ScanUniformWords(uint32_t* begin, uint32_t* end, Fn&& fn) {
    uint32_t* w = begin;
    uint32_t cfgIndex = 0;
    bool cfgF32 = false;
    uint32_t pos = 0; /* words since the last CONFIG write */
    while (w < end) {
        if (w + 2 > end) {
            return false;
        }
        const uint32_t header = w[1];
        const uint32_t extra = (header >> 20) & 0xFFu;
        const uint32_t reg = header & 0xFFFFu;
        const bool consecutive = (header >> 31) != 0;
        const uint32_t total = 2u + extra + (extra & 1u);
        if (w + total > end) {
            return false;
        }
        for (uint32_t k = 0; k <= extra; k++) {
            uint32_t* p = (k == 0) ? w : w + 1 + k;
            const uint32_t target = consecutive ? reg + k : reg;
            if (target == kRegVshFloatConfig) {
                cfgIndex = *p & 0xFFu;
                cfgF32 = (*p >> 31) != 0;
                pos = 0;
            } else if (target >= kRegVshFloatData0 && target <= kRegVshFloatData7) {
                if (cfgF32) {
                    UnifWord u;
                    u.ptr = p;
                    u.index = static_cast<uint8_t>(cfgIndex + (pos >> 2));
                    u.comp = static_cast<uint8_t>(pos & 3u);
                    fn(u);
                }
                pos++;
            }
        }
        w += total;
    }
    return true;
}

inline uint32_t FloatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

inline float BitsFloat(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

} // namespace GdxReplayMath

#endif /* GDX3DS_REPLAY_MATH_H */
