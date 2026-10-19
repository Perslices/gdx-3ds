/* gdx3ds_trkrr_math_host_tests.cpp — [native] track_retained geometry (gdx3ds_track_retained_math.h).
 *
 * What the retained course hands the PICA rests on four facts, each checked here on synthetic
 * cameras and triangles:
 *   1. the pass origin: SolveEye recovers the eye from the composed matrix alone, and NearPlaneW
 *      the w of the z = -w plane (the CPU clipper's near plane);
 *   2. watertight clipping: the point where an edge meets a plane is bit-identical whichever of
 *      the two triangles sharing the edge asks, and lies on the plane; through all five planes,
 *      two neighbours' polygons share every point they generate on their common edge;
 *   3. ClipPoly keeps the part in front (3 / 4 / 3 / 0 corners for 0 / 1 / 2 / 3 behind);
 *   4. the visible-region float24 bound is RIGOROUS: no point of the triangle inside the four
 *      side planes has a worse error ratio sum(l e) / sum(l w) than it reports, and it never
 *      exceeds the per-corner bound max(e / w).
 *   5. ([native] track_resident, C5 Stage 3c) the per-ring check is CONSERVATIVE: a ring whose
 *      bounding sphere RingPlanes accepts has no point behind the near plane, outside the 2w box
 *      or (folded viewport) outside the side planes, none below the reported smallest w, and
 *      RingBoundMpx is never below any point's own per-vertex float24 bound.
 *   6. ([native] track_nearclip=1, C5 Stage 3d) the lean clipper IS ClipPoly: on strips of
 *      triangles sharing edges (the walk's order, so the edge cache is hit), through the near
 *      plane alone and through all five planes, LeanClipPoly returns ClipPoly's corners bit for
 *      bit (position, clip coordinates, attributes), a cached edge point included; and the strip's
 *      neighbours still seal.
 * See docs/research/native/c5s3-progress.md §1, c5s3c-progress.md §2 and c5s3d-progress.md §3. */
#include "gdx3ds_track_retained_math.h"

#include <cstdio>
#include <cstdlib>

using namespace GdxTrkRr;

namespace {

int sFailures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        sFailures++;
        printf("FAIL: %s\n", what);
    }
}

/* xorshift: deterministic across hosts */
uint32_t sRng = 0x2545F491u;
double Rand01() {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return (sRng & 0xFFFFFFu) / 16777216.0;
}
double RandIn(double a, double b) {
    return a + (b - a) * Rand01();
}

struct Camera {
    double eye[3];
    double nearZ, farZ;
    float mp[4][4]; /* N64 layout: clip_i = sum_k ob_k mp[k][i] + mp[3][i] */
};

/* A look-at + OpenGL-style perspective, composed in double, stored as the interpreter's floats. */
Camera MakeCamera(double yawDeg, double pitchDeg, const double eye[3], double nearZ, double farZ) {
    Camera c;
    for (int k = 0; k < 3; k++) {
        c.eye[k] = eye[k];
    }
    c.nearZ = nearZ;
    c.farZ = farZ;
    const double yaw = yawDeg * M_PI / 180.0;
    const double pitch = pitchDeg * M_PI / 180.0;
    const double fwd[3] = { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
    const double worldUp[3] = { 0.0, 1.0, 0.0 };
    double right[3] = { fwd[1] * worldUp[2] - fwd[2] * worldUp[1], fwd[2] * worldUp[0] - fwd[0] * worldUp[2],
                        fwd[0] * worldUp[1] - fwd[1] * worldUp[0] };
    const double rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (double& v : right) {
        v /= rl;
    }
    const double up[3] = { right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                           right[0] * fwd[1] - right[1] * fwd[0] };
    /* eye space: x = right, y = up, z = -forward */
    double view[4][4] = {};
    for (int k = 0; k < 3; k++) {
        view[0][k] = right[k];
        view[1][k] = up[k];
        view[2][k] = -fwd[k];
    }
    for (int r = 0; r < 3; r++) {
        view[r][3] = -(view[r][0] * eye[0] + view[r][1] * eye[1] + view[r][2] * eye[2]);
    }
    view[3][3] = 1.0;
    const double cot = 1.0 / std::tan(30.0 * M_PI / 180.0);
    double proj[4][4] = {};
    proj[0][0] = cot / (400.0 / 240.0);
    proj[1][1] = cot;
    proj[2][2] = (farZ + nearZ) / (nearZ - farZ);
    proj[2][3] = 2.0 * farZ * nearZ / (nearZ - farZ);
    proj[3][2] = -1.0;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            double m = 0.0;
            for (int j = 0; j < 4; j++) {
                m += proj[i][j] * view[j][k];
            }
            c.mp[k][i] = (float)m;
        }
    }
    return c;
}

double Row(const Camera& c, const double p[3], int i) {
    return p[0] * c.mp[0][i] + p[1] * c.mp[1][i] + p[2] * c.mp[2][i] + c.mp[3][i];
}

ClipXf XfOf(const Camera& c) {
    ClipXf t;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 4; i++) {
            t.m[k][i] = c.mp[k][i];
        }
    }
    t.xs = 1.0;
    return t;
}

PolyV Corner(const Camera& c, double x, double y, double z) {
    PolyV v;
    memset(&v, 0, sizeof(v));
    v.p[0] = x;
    v.p[1] = y;
    v.p[2] = z;
    ClipOf(XfOf(c), v.p, v.c);
    v.u = RandIn(-500.0, 500.0);
    v.v = RandIn(-500.0, 500.0);
    for (double& k : v.cn) {
        k = RandIn(0.0, 255.0);
    }
    v.orig = true;
    return v;
}

/* A point in eye space (x right, y up, d forward) -> world. */
void EyeToWorld(const Camera& c, double yawDeg, double pitchDeg, double ex, double ey, double d, double out[3]) {
    const double yaw = yawDeg * M_PI / 180.0;
    const double pitch = pitchDeg * M_PI / 180.0;
    const double fwd[3] = { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
    double right[3] = { fwd[2], 0.0, -fwd[0] };
    const double rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    right[0] /= rl;
    right[2] /= rl;
    const double up[3] = { right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                           right[0] * fwd[1] - right[1] * fwd[0] };
    for (int k = 0; k < 3; k++) {
        out[k] = c.eye[k] + ex * right[k] + ey * up[k] + d * fwd[k];
    }
}

void TestEyeAndNearPlane() {
    int bad = 0;
    for (int t = 0; t < 200; t++) {
        const double eye[3] = { RandIn(-30000, 30000), RandIn(-3000, 3000), RandIn(-30000, 30000) };
        const double nearZ = RandIn(8.0, 32.0);
        const Camera c = MakeCamera(RandIn(-180, 180), RandIn(-60, 60), eye, nearZ, RandIn(8000, 20000));
        double got[3];
        if (!SolveEye(c.mp, got)) {
            bad++;
            continue;
        }
        double err = 0.0;
        for (int k = 0; k < 3; k++) {
            err = std::fmax(err, std::fabs(got[k] - eye[k]));
        }
        /* the matrix is fp32: |E| ~ 3e4 carries ~2e-3 of translation rounding */
        if (err > 0.05) {
            bad++;
            printf("  eye err %.4f at (%.0f %.0f %.0f)\n", err, eye[0], eye[1], eye[2]);
        }
        const float nw = NearPlaneW(c.mp, got);
        if (std::fabs(nw - nearZ) > 0.01 * nearZ) {
            bad++;
            printf("  nearW %.4f vs %.4f\n", nw, nearZ);
        }
    }
    Check(bad == 0, "SolveEye recovers the eye and NearPlaneW the near plane's w (200 cameras)");
}

double NearQ(const PolyV& v) {
    return PlaneDist(v.c, PLANE_NEAR);
}

void TestEdgePointIsCanonical() {
    int bad = 0;
    int offPlane = 0;
    const double eye[3] = { 1234.0, 50.0, -2345.0 };
    const Camera c = MakeCamera(37.0, -8.0, eye, 16.0, 12000.0);
    const ClipXf t = XfOf(c);
    for (int k = 0; k < 5000; k++) {
        /* one corner in front of the near plane, one behind it (integer positions, as the stream) */
        double pa[3];
        double pb[3];
        EyeToWorld(c, 37.0, -8.0, RandIn(-400, 400), RandIn(-200, 200), RandIn(20.0, 600.0), pa);
        EyeToWorld(c, 37.0, -8.0, RandIn(-400, 400), RandIn(-200, 200), RandIn(-600.0, 12.0), pb);
        PolyV a = Corner(c, std::floor(pa[0]), std::floor(pa[1]), std::floor(pa[2]));
        PolyV b = Corner(c, std::floor(pb[0]), std::floor(pb[1]), std::floor(pb[2]));
        if (!(NearQ(a) >= 0.0) || !(NearQ(b) < 0.0)) {
            continue;
        }
        const PolyV ab = EdgePoint(t, a, b, PLANE_NEAR);
        const PolyV ba = EdgePoint(t, b, a, PLANE_NEAR);
        if (memcmp(ab.p, ba.p, sizeof(ab.p)) != 0 || memcmp(ab.c, ba.c, sizeof(ab.c)) != 0) {
            bad++;
        }
        if (std::fabs(NearQ(ab)) > 1e-6 * (std::fabs(NearQ(a)) + std::fabs(NearQ(b)))) {
            offPlane++;
        }
    }
    Check(bad == 0, "EdgePoint: both triangles sharing an edge get the same point, bit for bit");
    Check(offPlane == 0, "EdgePoint: the point lies on the plane");
}

bool SamePoint(const PolyV& x, const PolyV& y) {
    return memcmp(x.p, y.p, sizeof(x.p)) == 0;
}

/* Two triangles share edge (s0, s1); both are clipped by all five planes. Every point either
 * polygon generates on the shared edge must appear, bit for bit, in the other. */
void TestNeighboursSeal() {
    int pairs = 0;
    int unmatched = 0;
    const double eye[3] = { -300.0, 80.0, 900.0 };
    const Camera c = MakeCamera(71.0, -14.0, eye, 16.0, 12000.0);
    const ClipXf t = XfOf(c);
    for (int k = 0; k < 4000; k++) {
        double p[4][3];
        for (int i = 0; i < 4; i++) {
            EyeToWorld(c, 71.0, -14.0, RandIn(-700, 700), RandIn(-400, 400), RandIn(-200.0, 500.0), p[i]);
            for (int j = 0; j < 3; j++) {
                p[i][j] = std::floor(p[i][j]);
            }
        }
        const PolyV s0 = Corner(c, p[0][0], p[0][1], p[0][2]);
        const PolyV s1 = Corner(c, p[1][0], p[1][1], p[1][2]);
        const PolyV l = Corner(c, p[2][0], p[2][1], p[2][2]);
        const PolyV rr = Corner(c, p[3][0], p[3][1], p[3][2]);
        const PolyV triA[3] = { s0, s1, l };
        const PolyV triB[3] = { s1, s0, rr }; /* the neighbour walks the edge the other way */
        PolyV outA[kClipPolyMax];
        PolyV outB[kClipPolyMax];
        const uint32_t all = kPlaneNearBit | kPlaneSideBits;
        const int na = ClipPoly(t, triA, outA, all);
        const int nb = ClipPoly(t, triB, outB, all);
        /* points generated on the shared edge: collinear with s0-s1 and between them */
        auto onEdge = [&](const PolyV& v) {
            if (v.orig) {
                return false;
            }
            double d[3];
            double e[3];
            for (int j = 0; j < 3; j++) {
                d[j] = v.p[j] - s0.p[j];
                e[j] = s1.p[j] - s0.p[j];
            }
            const double cx = d[1] * e[2] - d[2] * e[1];
            const double cy = d[2] * e[0] - d[0] * e[2];
            const double cz = d[0] * e[1] - d[1] * e[0];
            const double ee = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            return ee > 0.0 && (cx * cx + cy * cy + cz * cz) < 1e-12 * ee * ee;
        };
        for (int i = 0; i < na; i++) {
            if (!onEdge(outA[i])) {
                continue;
            }
            pairs++;
            bool found = false;
            for (int j = 0; j < nb; j++) {
                found = found || SamePoint(outA[i], outB[j]);
            }
            unmatched += found ? 0 : 1;
        }
    }
    printf("  seal: %d points generated on shared edges\n", pairs);
    Check(pairs > 500, "enough shared-edge clip points were generated");
    Check(unmatched == 0, "ClipPoly: every point generated on a shared edge exists bit for bit in the neighbour");
}

void TestClipNearCounts() {
    const double eye[3] = { 0.0, 0.0, 0.0 };
    const Camera c = MakeCamera(0.0, 0.0, eye, 16.0, 12000.0);
    const ClipXf t = XfOf(c);
    /* forward is +z here; the near plane is z = 16 in world space */
    const PolyV f1 = Corner(c, -50, 0, 100);
    const PolyV f2 = Corner(c, 50, 0, 100);
    const PolyV f3 = Corner(c, 0, 40, 200);
    const PolyV b1 = Corner(c, 0, -40, 5);
    const PolyV b2 = Corner(c, 30, -40, -50);
    const PolyV b3 = Corner(c, -30, -40, -80);
    PolyV out[kClipPolyMax];
    const PolyV all[3] = { f1, f2, f3 };
    const PolyV one[3] = { f1, f2, b1 };
    const PolyV two[3] = { f1, b1, b2 };
    const PolyV none[3] = { b1, b2, b3 };
    Check(ClipPoly(t, all, out, kPlaneNearBit) == 3, "ClipPoly: all in front -> the triangle");
    Check(ClipPoly(t, one, out, kPlaneNearBit) == 4, "ClipPoly: one corner behind -> a quad");
    const int n = ClipPoly(t, two, out, kPlaneNearBit);
    Check(n == 3, "ClipPoly: two corners behind -> a triangle");
    bool onOrFront = true;
    for (int i = 0; i < n; i++) {
        onOrFront = onOrFront && NearQ(out[i]) > -1e-9;
    }
    Check(onOrFront, "ClipPoly: every output corner is on or in front of the plane");
    Check(ClipPoly(t, none, out, kPlaneNearBit) == 0, "ClipPoly: all behind -> nothing (the CPU clipper emits nothing)");
}

BoundV BoundOfPoint(const Camera& c, const double p[3], double e) {
    BoundV b;
    b.x = Row(c, p, 0);
    b.y = Row(c, p, 1);
    b.w = Row(c, p, 3);
    b.e = e;
    return b;
}

void TestVisibleBoundIsRigorous() {
    const double eye[3] = { -800.0, 120.0, 4000.0 };
    const Camera c = MakeCamera(-120.0, -12.0, eye, 16.0, 12000.0);
    int checked = 0;
    int violations = 0;
    int overCheap = 0;
    int hidden = 0;
    for (int t = 0; t < 3000; t++) {
        double p[3][3];
        BoundV v[3];
        double cheap = 0.0;
        bool front = true;
        for (int i = 0; i < 3; i++) {
            /* corners in front of the near plane, many far outside the side planes */
            EyeToWorld(c, -120.0, -12.0, RandIn(-900, 900), RandIn(-600, 600), RandIn(17.0, 900.0), p[i]);
            const double e = RandIn(0.001, 0.2);
            v[i] = BoundOfPoint(c, p[i], e);
            front = front && v[i].w > 0.0;
            cheap = std::fmax(cheap, e / v[i].w * 200000.0);
        }
        if (!front) {
            continue;
        }
        bool visible = false;
        const uint32_t bound = VisibleBoundMpx(v, 3, &visible);
        if (!visible) {
            hidden++;
        }
        if ((double)bound > cheap + 1.0) {
            overCheap++;
        }
        for (int s = 0; s < 400; s++) {
            double l0 = Rand01();
            double l1 = Rand01();
            if (l0 + l1 > 1.0) {
                l0 = 1.0 - l0;
                l1 = 1.0 - l1;
            }
            const double l2 = 1.0 - l0 - l1;
            const double x = l0 * v[0].x + l1 * v[1].x + l2 * v[2].x;
            const double y = l0 * v[0].y + l1 * v[1].y + l2 * v[2].y;
            const double w = l0 * v[0].w + l1 * v[1].w + l2 * v[2].w;
            if (!(w > 0.0) || std::fabs(x) > w || std::fabs(y) > w) {
                continue; /* outside the viewport: not a visible point */
            }
            checked++;
            if (!visible) {
                violations++; /* a visible point in a triangle declared invisible */
                continue;
            }
            /* the worst any error assignment |d_i| <= e_i can do at this point */
            const double ratio = (l0 * v[0].e + l1 * v[1].e + l2 * v[2].e) / w * 200000.0;
            if (ratio > (double)bound + 1.0) {
                violations++;
            }
        }
    }
    printf("  visible-bound: %d visible points checked, %d triangles invisible\n", checked, hidden);
    Check(checked > 10000, "enough visible points were sampled");
    Check(violations == 0, "the visible-region bound covers every visible point (rigorous)");
    Check(overCheap == 0, "the visible-region bound never exceeds the per-corner bound");
}

void TestInvisibleTriangle() {
    BoundV v[3] = { { 20.0, 0.0, 10.0, 0.01 }, { 30.0, 5.0, 12.0, 0.01 }, { 25.0, -5.0, 11.0, 0.01 } };
    bool visible = true;
    (void)VisibleBoundMpx(v, 3, &visible);
    Check(!visible, "a triangle entirely outside x = w is invisible");
}

void TestRingCheckIsConservative() {
    const double eye[3] = { 1500.0, 300.0, -2500.0 };
    const Camera c = MakeCamera(35.0, -8.0, eye, 16.0, 12000.0);
    ClipXf t = XfOf(c);
    t.xs = 1.25; /* an aspect x scale, as the interpreter's X_ADJ applies */
    RingNorms n;
    RingNormsOf(t, &n);
    float m[4][4];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 4; i++) {
            m[k][i] = c.mp[k][i];
        }
    }
    int accepted = 0;
    int refused[4] = { 0, 0, 0, 0 };
    int planeViolations = 0;
    int boundViolations = 0;
    int samples = 0;
    for (int ring = 0; ring < 4000; ring++) {
        /* ring centres all over the view volume (and outside it), radii like a chunk's */
        double cw[3];
        EyeToWorld(c, 35.0, -8.0, RandIn(-2500, 2500), RandIn(-1500, 1500), RandIn(-200.0, 4000.0), cw);
        const double r = RandIn(5.0, 300.0);
        const bool side = (ring & 1) != 0;
        const bool rej = (ring & 2) != 0;
        double wMin = 0.0;
        double wC = 0.0;
        const int why = RingPlanes(t, n, cw, r, side, rej, &wMin, &wC);
        if (why != RING_OK) {
            refused[why]++;
            continue;
        }
        accepted++;
        /* the box the bound is taken over: the sphere's */
        const float a[3] = { (float)(std::fabs(cw[0]) + r), (float)(std::fabs(cw[1]) + r), (float)(std::fabs(cw[2]) + r) };
        const uint32_t bound = RingBoundMpx(m, (float)t.xs, a, wMin);
        for (int s = 0; s < 64; s++) {
            double d[3] = { RandIn(-1, 1), RandIn(-1, 1), RandIn(-1, 1) };
            const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > 1.0 || dl == 0.0) {
                continue;
            }
            const double sc = (s & 3) == 0 ? r / dl : r * Rand01(); /* a quarter on the surface */
            const double p[3] = { cw[0] + d[0] * sc, cw[1] + d[1] * sc, cw[2] + d[2] * sc };
            double q[4];
            ClipOf(t, p, q);
            samples++;
            bool ok = q[3] >= wMin - 1e-6 * std::fabs(wMin) && PlaneDist(q, PLANE_NEAR) > 0.0;
            if (rej) {
                ok = ok && std::fabs(q[0]) <= 2.0 * q[3] && std::fabs(q[1]) <= 2.0 * q[3];
            }
            if (side) {
                ok = ok && std::fabs(q[0]) <= q[3] && std::fabs(q[1]) <= q[3];
            }
            if (!ok) {
                planeViolations++;
            }
            /* the point's own per-vertex bound (the per-frame path's errXY / w) */
            float e[4];
            for (int i = 0; i < 4; i++) {
                e[i] = (std::fabs(m[0][i]) * (float)std::fabs(p[0]) + std::fabs(m[1][i]) * (float)std::fabs(p[1]) +
                        std::fabs(m[2][i]) * (float)std::fabs(p[2]) + std::fabs(m[3][i])) *
                       (1.0f / 16384.0f);
            }
            const float ex = e[0] * (float)t.xs;
            const double own = ((ex > e[1] ? ex : e[1]) + e[3]) / q[3] * 200000.0;
            if (own > (double)bound + 1e-6 * (double)bound + 0.5) {
                boundViolations++;
            }
        }
    }
    printf("  ring check: %d rings accepted (%d near, %d rej, %d side refused), %d points checked\n", accepted,
           refused[RING_NEAR], refused[RING_REJ], refused[RING_SIDE], samples);
    Check(accepted > 500 && refused[RING_NEAR] > 100 && refused[RING_SIDE] > 100, "the ring check was exercised both ways");
    Check(planeViolations == 0, "an accepted ring has no point past a plane it was checked against (conservative)");
    Check(boundViolations == 0, "the ring bound covers every point's own per-vertex float24 bound");
}

bool SameCorner(const PolyV& x, const PolyV& y) {
    return memcmp(x.p, y.p, sizeof(x.p)) == 0 && memcmp(x.c, y.c, sizeof(x.c)) == 0 &&
           memcmp(&x.u, &y.u, sizeof(double)) == 0 && memcmp(&x.v, &y.v, sizeof(double)) == 0 &&
           memcmp(x.cn, y.cn, sizeof(x.cn)) == 0 && x.orig == y.orig &&
           (!x.orig || memcmp(x.rel, y.rel, sizeof(x.rel)) == 0);
}

/* A strip of quads between two rings of integer points, walked as the templates walk it
 * (triangles (a0, b0, a1), (a1, b0, b1), ...), clipped both ways. */
void TestLeanClipIsClipPoly() {
    int tris = 0;
    int clipped = 0;
    int differ = 0;
    int sealed = 0;
    EdgeCache ec;
    memset(&ec, 0, sizeof(ec));
    uint32_t keyNext = 1;
    for (int cam = 0; cam < 40; cam++) {
        const double yaw = RandIn(-180.0, 180.0);
        const double pitch = RandIn(-25.0, 5.0);
        const double eye[3] = { RandIn(-3000, 3000), RandIn(0, 200), RandIn(-3000, 3000) };
        const Camera c = MakeCamera(yaw, pitch, eye, 16.0, 12000.0);
        const ClipXf t = XfOf(c);
        const uint32_t tag = (uint32_t)cam + 1u;
        for (int strip = 0; strip < 60; strip++) {
            constexpr int kRing = 8;
            PolyV ringA[kRing];
            PolyV ringB[kRing];
            uint32_t keyA[kRing];
            uint32_t keyB[kRing];
            const double depthA = RandIn(-80.0, 120.0);
            const double depthB = depthA + RandIn(10.0, 150.0);
            for (int i = 0; i < kRing; i++) {
                double pa[3];
                double pb[3];
                const double x = -350.0 + 100.0 * i;
                EyeToWorld(c, yaw, pitch, x + RandIn(-10, 10), RandIn(-60, -20), depthA + RandIn(-8, 8), pa);
                EyeToWorld(c, yaw, pitch, x + RandIn(-10, 10), RandIn(-60, -20), depthB + RandIn(-8, 8), pb);
                ringA[i] = Corner(c, std::floor(pa[0]), std::floor(pa[1]), std::floor(pa[2]));
                ringB[i] = Corner(c, std::floor(pb[0]), std::floor(pb[1]), std::floor(pb[2]));
                for (int k = 0; k < 3; k++) {
                    ringA[i].rel[k] = (int16_t)ringA[i].p[k];
                    ringB[i].rel[k] = (int16_t)ringB[i].p[k];
                }
                keyA[i] = keyNext++;
                keyB[i] = keyNext++;
            }
            const uint32_t mask = (strip & 1) != 0 ? (kPlaneNearBit | kPlaneSideBits) : kPlaneNearBit;
            PolyV prevOut[kClipPolyMax];
            int prevN = 0;
            for (int q = 0; q + 1 < kRing; q++) {
                for (int half = 0; half < 2; half++) {
                    const PolyV tri[3] = { half == 0 ? ringA[q] : ringA[q + 1], ringB[q], half == 0 ? ringA[q + 1] : ringB[q + 1] };
                    const uint32_t key[3] = { half == 0 ? keyA[q] : keyA[q + 1], keyB[q], half == 0 ? keyA[q + 1] : keyB[q + 1] };
                    PolyV stock[kClipPolyMax];
                    const int ns = ClipPoly(t, tri, stock, mask);
                    PolyV pool[16];
                    const PolyV* lean[kClipPolyMax];
                    uint32_t outKey[kClipPolyMax];
                    int outEntry[kClipPolyMax];
                    uint32_t outStamp[kClipPolyMax];
                    const int nl = LeanClipPoly(t, tri, key, tag, &ec, pool, 16, lean, outKey, outEntry, outStamp, mask);
                    tris++;
                    clipped += ns != 3 || !stock[0].orig || !stock[1].orig || !stock[2].orig ? 1 : 0;
                    if (nl != ns) {
                        differ++;
                        continue;
                    }
                    for (int i = 0; i < ns; i++) {
                        differ += SameCorner(stock[i], *lean[i]) ? 0 : 1;
                    }
                    /* the previous triangle of the strip shares edge (ringA[q or q+1], ringB[q]) */
                    for (int i = 0; i < ns && prevN > 0; i++) {
                        if (stock[i].orig) {
                            continue;
                        }
                        for (int j = 0; j < prevN; j++) {
                            if (!prevOut[j].orig && SamePoint(stock[i], prevOut[j])) {
                                sealed++;
                                break;
                            }
                        }
                    }
                    for (int i = 0; i < ns; i++) {
                        prevOut[i] = stock[i];
                    }
                    prevN = ns;
                }
            }
        }
    }
    printf("  lean clip: %d triangles (%d clipped), edge cache hits %u misses %u, %d shared points\n", tris, clipped,
           ec.hits, ec.misses, sealed);
    Check(clipped > 1000, "enough strip triangles reached a plane");
    Check(ec.hits > 500, "the edge cache was hit (neighbours reuse the shared edge's point)");
    Check(differ == 0, "LeanClipPoly == ClipPoly bit for bit (positions, clip coordinates, attributes)");
    Check(sealed > 500, "strip neighbours share their generated edge points bit for bit");
}

} // namespace

int main() {
    TestEyeAndNearPlane();
    TestEdgePointIsCanonical();
    TestNeighboursSeal();
    TestClipNearCounts();
    TestVisibleBoundIsRigorous();
    TestInvisibleTriangle();
    TestRingCheckIsConservative();
    TestLeanClipIsClipPoly();
    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
