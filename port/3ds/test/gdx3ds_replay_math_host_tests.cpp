/* port/3ds/test/gdx3ds_replay_math_host_tests.cpp -- C6 pose-replay math and the PICA
 * command-list scan (port/3ds/gfx/gdx3ds_replay_math.h).
 *
 * The replay's claim is: patching A's uniforms with K = MP_A^-1 * MP_B reproduces the vertex
 * B's own frame would have produced. These tests check exactly that claim numerically, for
 * each patch form, against a direct transform with MP_B:
 *   1. legacy: pos = pack(v * MP_A); Proj_B * pos == Proj_A * pack(v * MP_B)
 *   2. gpuxf:  U_B * (ob, 1) == U built from MP_B directly (incl. the rebased track form)
 *   3. resident re-base: moving xoff to B's eye leaves every clip position unchanged
 *   4. lights: R = M3_B * M3_A^-1 maps normalize(M3_A l) to normalize(M3_B l)
 *   5. the scan finds every f32 uniform word written through GPUCMD_Add's packet layout
 *      (param0 before the header, pad words, >256-param splits, incremental CONFIG writes,
 *      f24 constants in between), and reports the split vec4 that starts at param0. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "gdx3ds_replay_math.h"

using namespace GdxReplayMath;

static int sFails = 0;
static int sChecks = 0;

static void Check(bool ok, const char* what) {
    sChecks++;
    if (!ok) {
        sFails++;
        printf("FAIL: %s\n", what);
    }
}

/* A perspective camera (row-vector N64 style: clip = p * V * P). */
static void Camera(float eye[3], float yaw, float pitch, float out[4][4]) {
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    double f[3] = { cy * cp, sp, sy * cp };
    double up0[3] = { 0, 1, 0 };
    double r[3] = { f[1] * up0[2] - f[2] * up0[1], f[2] * up0[0] - f[0] * up0[2], f[0] * up0[1] - f[1] * up0[0] };
    double rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int i = 0; i < 3; i++) {
        r[i] /= rl;
    }
    double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    double v[4][4] = {};
    for (int i = 0; i < 3; i++) {
        v[i][0] = r[i];
        v[i][1] = u[i];
        v[i][2] = -f[i];
    }
    v[3][0] = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]);
    v[3][1] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v[3][2] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    v[3][3] = 1.0;
    const double n = 16.0, fa = 60000.0, t = 1.0 / tan(0.6);
    double p[4][4] = {};
    p[0][0] = t * 0.75;
    p[1][1] = t;
    p[2][2] = (fa + n) / (n - fa);
    p[2][3] = -1.0;
    p[3][2] = 2.0 * fa * n / (n - fa);
    double m[4][4];
    MulD(m, v, p);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i][j] = (float)m[i][j];
        }
    }
}

static void Model(float pos[3], float ang, float scale, float out[4][4]) {
    memset(out, 0, sizeof(float) * 16);
    const float c = cosf(ang), s = sinf(ang);
    out[0][0] = c * scale;
    out[0][2] = -s * scale;
    out[1][1] = scale;
    out[2][0] = s * scale;
    out[2][2] = c * scale;
    out[3][0] = pos[0];
    out[3][1] = pos[1];
    out[3][2] = pos[2];
    out[3][3] = 1.0f;
}

static void XformRow(const double v[4], const float m[4][4], double out[4]) {
    for (int j = 0; j < 4; j++) {
        out[j] = v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] + v[3] * m[3][j];
    }
}

static void ColMul(const float m[4][4], const double v[4], double out[4]) {
    for (int i = 0; i < 4; i++) {
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * v[3];
    }
}

static double RelNdc(const double a[4], const double b[4]) {
    double worst = 0.0;
    for (int i = 0; i < 3; i++) {
        const double d = fabs(a[i] / a[3] - b[i] / b[3]);
        worst = d > worst ? d : worst;
    }
    return worst;
}

static void TestLegacy() {
    float eyeA[3] = { 1200.0f, 80.0f, -3400.0f };
    float eyeB[3] = { 1230.0f, 82.0f, -3370.0f };
    float camA[4][4], camB[4][4], mA[4][4], mB[4][4], mpA[4][4], mpB[4][4];
    Camera(eyeA, 0.70f, -0.05f, camA);
    Camera(eyeB, 0.72f, -0.06f, camB);
    float pa[3] = { 1300.0f, 70.0f, -3300.0f }, pb[3] = { 1331.0f, 71.0f, -3272.0f };
    Model(pa, 0.3f, 1.5f, mA);
    Model(pb, 0.33f, 1.5f, mB);
    MulF(mpA, mA, camA);
    MulF(mpB, mB, camB);
    double k[4][4];
    Check(Correction(mpA, mpB, k), "legacy: MP_A invertible");
    /* a landscape fixup-like projection (rotation + depth remap) */
    float proj[4][4] = { { 0, 1, 0, 0 }, { -1, 0, 0, 0 }, { 0, 0, 1, -1 }, { 0, 0, 0, 1 } };
    const float xs = 0.8f;
    float projB[4][4];
    Check(PatchLegacy(proj, k, xs, projB), "legacy: patch");
    double worst = 0.0;
    for (int n = 0; n < 200; n++) {
        const double ob[4] = { (n % 7) * 13.0 - 40.0, (n % 5) * 9.0 - 20.0, (n % 11) * 11.0 - 60.0, 1.0 };
        double ca[4], cb[4], pa4[4], pb4[4], outA[4], outB[4];
        XformRow(ob, mpA, ca);
        XformRow(ob, mpB, cb);
        const double packA[4] = { ca[0] * xs, ca[1], (ca[2] + ca[3]) / 2.0, ca[3] };
        const double packB[4] = { cb[0] * xs, cb[1], (cb[2] + cb[3]) / 2.0, cb[3] };
        (void)pa4;
        (void)pb4;
        ColMul(projB, packA, outA);
        ColMul(proj, packB, outB);
        const double e = RelNdc(outA, outB);
        worst = e > worst ? e : worst;
    }
    printf("legacy: worst NDC error %.3g\n", worst);
    Check(worst < 1e-4, "legacy: patched projection reproduces B's vertices");
}

static void TestXf() {
    float eyeA[3] = { -800.0f, 300.0f, 9000.0f };
    float eyeB[3] = { -760.0f, 305.0f, 8950.0f };
    float mpA[4][4], mpB[4][4];
    Camera(eyeA, 2.0f, 0.1f, mpA);
    Camera(eyeB, 2.03f, 0.1f, mpB);
    /* rebased track form: G = T(E) * MP, vertex = ob - E (E = integer eye) */
    const double E[3] = { -800.0, 300.0, 9000.0 };
    float uA[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            uA[i][j] = mpA[j][i];
        }
        uA[i][3] = (float)(E[0] * mpA[0][i] + E[1] * mpA[1][i] + E[2] * mpA[2][i] + mpA[3][i]);
    }
    double k[4][4];
    Check(Correction(mpA, mpB, k), "xf: MP_A invertible");
    float uB[4][4];
    PatchXfMvp(uA, k, uB);
    double worst = 0.0;
    for (int n = 0; n < 200; n++) {
        const double world[3] = { -700.0 + n * 3.0, 280.0 + (n % 9), 8800.0 - n * 5.0 };
        const double rel[4] = { world[0] - E[0], world[1] - E[1], world[2] - E[2], 1.0 };
        double got[4], want[4];
        ColMul(uB, rel, got);
        const double w4[4] = { world[0], world[1], world[2], 1.0 };
        XformRow(w4, mpB, want);
        const double e = RelNdc(got, want);
        worst = e > worst ? e : worst;
    }
    printf("xf: worst NDC error %.3g\n", worst);
    Check(worst < 1e-4, "xf: patched xmvp reproduces B's vertices");

    /* resident re-base to B's integer eye: clip unchanged */
    double eyeBSolved[3] = { -760.0, 305.0, 8950.0 };
    double d[3] = { eyeBSolved[0] - E[0], eyeBSolved[1] - E[1], eyeBSolved[2] - E[2] };
    float uR[4][4];
    memcpy(uR, uB, sizeof(uR));
    RebaseXf(uR, d);
    double worstR = 0.0;
    for (int n = 0; n < 100; n++) {
        const double world[3] = { -900.0 + n * 7.0, 250.0 + n, 9100.0 - n * 3.0 };
        const double relA[4] = { world[0] - E[0], world[1] - E[1], world[2] - E[2], 1.0 };
        const double relB[4] = { world[0] - eyeBSolved[0], world[1] - eyeBSolved[1], world[2] - eyeBSolved[2], 1.0 };
        double a[4], b[4];
        ColMul(uB, relA, a);
        ColMul(uR, relB, b);
        const double e = RelNdc(a, b);
        worstR = e > worstR ? e : worstR;
    }
    printf("xf rebase: worst NDC error %.3g\n", worstR);
    Check(worstR < 1e-5, "xf: re-base to B's eye leaves clip positions unchanged");
}

static void TestLights() {
    float mA[3][3], mB[3][3];
    const float a = 0.4f, b = 0.47f, s = 1.3f;
    const float ra[3][3] = { { cosf(a), 0, -sinf(a) }, { 0, 1, 0 }, { sinf(a), 0, cosf(a) } };
    const float rb[3][3] = { { cosf(b), 0.1f, -sinf(b) }, { 0, 1, 0 }, { sinf(b), 0, cosf(b) } };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            mA[i][j] = ra[i][j] * s;
            mB[i][j] = rb[i][j] * s;
        }
    }
    double r[3][3];
    Check(LightRotation(mA, mB, r), "lights: M3_A invertible");
    const double l[3] = { 0.3, -0.8, 0.52 };
    float cA[3], cB[3], want[3];
    double t[3];
    for (int i = 0; i < 3; i++) {
        t[i] = mA[i][0] * l[0] + mA[i][1] * l[1] + mA[i][2] * l[2];
    }
    double tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (int i = 0; i < 3; i++) {
        cA[i] = (float)(t[i] / tl / 127.0);
    }
    for (int i = 0; i < 3; i++) {
        t[i] = mB[i][0] * l[0] + mB[i][1] * l[1] + mB[i][2] * l[2];
    }
    tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (int i = 0; i < 3; i++) {
        want[i] = (float)(t[i] / tl / 127.0);
    }
    RotateDir(r, cA, cB);
    double worst = 0.0;
    for (int i = 0; i < 3; i++) {
        worst = fabs(cB[i] - want[i]) > worst ? fabs(cB[i] - want[i]) : worst;
    }
    printf("lights: worst %.3g\n", worst);
    Check(worst < 1e-6, "lights: rotated coefficient = B's coefficient");
}

/* GPUCMD_AddInternal/GPUCMD_Add, verbatim shape (libctru gpu.c). */
struct CmdBuf {
    std::vector<uint32_t> w;
    void AddInternal(uint32_t header, const uint32_t* param, uint32_t n) {
        n--;
        header |= (n & 0xFF) << 20;
        w.push_back(param[0]);
        w.push_back(header);
        for (uint32_t i = 1; i <= n; i++) {
            w.push_back(param[i]);
        }
        if (n & 1) {
            w.push_back(0);
        }
    }
    void Add(uint32_t header, const uint32_t* param, uint32_t n) {
        while (n) {
            const uint32_t rem = n > 0x100 ? 0x100 : n;
            AddInternal(header, param, rem);
            param += rem;
            n -= rem;
            if (header & 0x80000000u) {
                header += rem;
            }
        }
    }
    void Write(uint32_t reg, uint32_t v) {
        Add(reg | 0x000F0000u, &v, 1);
    }
    void Writes(uint32_t reg, const uint32_t* v, uint32_t n) {
        Add(reg | 0x000F0000u, v, n);
    }
    void Incremental(uint32_t reg, const uint32_t* v, uint32_t n) {
        Add(reg | 0x000F0000u | 0x80000000u, v, n);
    }
};

static void TestScan() {
    CmdBuf cb;
    cb.Write(0x0041, 0x1234); /* some other register */
    const uint32_t f24[4] = { 0x00000050u, 1, 2, 3 }; /* an f24 constant at index 0x50 */
    cb.Incremental(kRegVshFloatConfig, f24, 4);
    /* 4 vec4s at index 0 (projection), then 70 vec4s at index 10 (a >256-word upload) */
    cb.Write(kRegVshFloatConfig, 0x80000000u | 0);
    std::vector<uint32_t> d(16);
    for (int i = 0; i < 16; i++) {
        d[i] = 1000u + i;
    }
    cb.Writes(kRegVshFloatData0, d.data(), 16);
    cb.Write(kRegVshFloatConfig, 0x80000000u | 10);
    std::vector<uint32_t> big(70 * 4);
    for (size_t i = 0; i < big.size(); i++) {
        big[i] = 5000u + (uint32_t)i;
    }
    cb.Writes(kRegVshFloatData0, big.data(), (uint32_t)big.size());
    cb.Write(0x0042, 7);
    int seen[256][4];
    memset(seen, -1, sizeof(seen));
    bool split0 = false;
    uint32_t* vec0 = nullptr;
    const bool ok = ScanUniformWords(cb.w.data(), cb.w.data() + cb.w.size(), [&](const UnifWord& u) {
        seen[u.index][u.comp] = (int)*u.ptr;
        if (u.index == 0 && u.comp == 0) {
            vec0 = u.ptr;
        }
        if (u.index == 0 && u.comp == 1) {
            split0 = u.ptr != vec0 + 1;
        }
    });
    Check(ok, "scan: well-formed list");
    bool good = true;
    for (int v = 0; v < 4; v++) {
        for (int c = 0; c < 4; c++) {
            good = good && seen[v][c] == (int)(1000 + v * 4 + c);
        }
    }
    Check(good, "scan: the 16-word projection upload");
    Check(split0, "scan: vec4 0 starts at param0 (split around the header)");
    good = true;
    for (int v = 0; v < 70; v++) {
        for (int c = 0; c < 4; c++) {
            good = good && seen[10 + v][c] == (int)(5000 + v * 4 + c);
        }
    }
    Check(good, "scan: 280 words across a 256-param packet split");
    Check(seen[0x50][0] == -1, "scan: f24 constants are not reported");
    /* truncated list */
    Check(!ScanUniformWords(cb.w.data(), cb.w.data() + cb.w.size() - 3, [](const UnifWord&) {}),
          "scan: a list cut mid-packet is refused");
}

static void TestMtxWords() {
    /* an N64 Mtx of 1.5 on the diagonal and a translation of -2.25, 3.0, 100.5 */
    float want[4][4] = { { 1.5f, 0, 0, 0 }, { 0, 1.5f, 0, 0 }, { 0, 0, 1.5f, 0 }, { -2.25f, 3.0f, 100.5f, 1.0f } };
    int32_t w[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t a = (int32_t)lround(want[i][j] * 65536.0);
            const int32_t b = (int32_t)lround(want[i][j + 1] * 65536.0);
            w[i * 2 + j / 2] = (int32_t)(((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b >> 16));
            w[8 + i * 2 + j / 2] = (int32_t)(((uint32_t)a << 16) | ((uint32_t)b & 0xFFFFu));
        }
    }
    float got[4][4];
    MtxFromWords(w, got);
    Check(memcmp(got, want, sizeof(got)) == 0, "mtx: fixed-point words decode exactly");
}

int main() {
    TestMtxWords();
    TestLegacy();
    TestXf();
    TestLights();
    TestScan();
    if (sFails == 0) {
        printf("=== ALL PASS (%d checks) ===\n", sChecks);
        return 0;
    }
    printf("=== %d FAILURES of %d checks ===\n", sFails, sChecks);
    return 1;
}
