/* gdx3ds_track_retained_math.h — [native] track_retained: the geometry that decides what the
 * PICA may be handed (gdx3ds_track_retained.cpp), host-compilable so it can be tested
 * (port/3ds/test/gdx3ds_trkrr_math_host_tests.cpp): the pass origin (the eye, from the composed
 * matrix), the near-plane w, the near-plane clip of a triangle in rebased object space with
 * canonical edge points, and the visible-region float24 bound.
 *
 * Matrices are the interpreter's N64 layout: clip_i = sum_k ob_k * mp[k][i] + mp[3][i]. */
#ifndef GDX3DS_TRACK_RETAINED_MATH_H
#define GDX3DS_TRACK_RETAINED_MATH_H

#include <cmath>
#include <cstdint>
#include <cstring>

namespace GdxTrkRr {

/* The eye: the point whose clip x, y and w are all 0 (x, y and w are affine in the position,
 * and a perspective camera maps exactly its eye there). Cramer's rule in double. */
inline bool SolveEye(const float mp[4][4], double eye[3]) {
    const int rows[3] = { 0, 1, 3 };
    double a[3][3];
    double b[3];
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            a[r][k] = mp[k][rows[r]];
        }
        b[r] = -(double)mp[3][rows[r]];
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > 1e-30)) {
        return false;
    }
    for (int c = 0; c < 3; c++) {
        double m[3][3];
        memcpy(m, a, sizeof(m));
        for (int r = 0; r < 3; r++) {
            m[r][c] = b[r];
        }
        const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                         m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        eye[c] = d / det;
    }
    return std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]);
}

/* w on the near clip plane (z = -w): along the w gradient from the eye, where z + w = 0. The
 * plane is parallel to w = const for a perspective camera, so this is the plane's w. */
inline float NearPlaneW(const float mp[4][4], const double eye[3]) {
    double g[3];
    double gz[3];
    double zEye = mp[3][2];
    for (int k = 0; k < 3; k++) {
        g[k] = mp[k][3];
        gz[k] = mp[k][2];
        zEye += eye[k] * mp[k][2];
    }
    const double gg = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
    const double den = gg + (g[0] * gz[0] + g[1] * gz[1] + g[2] * gz[2]);
    if (!(std::fabs(den) > 1e-30)) {
        return 1.0f;
    }
    const double w = -zEye / den * gg;
    return (w > 1e-3 && std::isfinite(w)) ? (float)w : 1.0f;
}

/* ---- clipping in rebased object space --------------------------------------------------------
 *
 * The retained path hands the PICA only polygons it will not have to clip at the near plane (and,
 * when the backend's scissor cannot be trusted to bound a viewport, at the side planes either).
 * Clip points are computed from the lexicographically smaller endpoint POSITION and their clip
 * coordinates are recomputed from the position, so the two triangles sharing an edge produce the
 * same points bit for bit, plane after plane: the edges seal. */

/* The PICA's position transform in double: clip_i = sum_k p_k m[k][i] + m[3][i], x times xs. */
struct ClipXf {
    double m[4][4];
    double xs;
};

inline void ClipOf(const ClipXf& t, const double p[3], double c[4]) {
    for (int i = 0; i < 4; i++) {
        c[i] = p[0] * t.m[0][i] + p[1] * t.m[1][i] + p[2] * t.m[2][i] + t.m[3][i];
    }
    c[0] *= t.xs;
}

enum { PLANE_NEAR = 0, PLANE_LEFT = 1, PLANE_RIGHT = 2, PLANE_BOTTOM = 3, PLANE_TOP = 4, PLANE_COUNT = 5 };
constexpr uint32_t kPlaneNearBit = 1u << PLANE_NEAR;
constexpr uint32_t kPlaneSideBits = (1u << PLANE_LEFT) | (1u << PLANE_RIGHT) | (1u << PLANE_BOTTOM) | (1u << PLANE_TOP);

/* >= 0 = inside. NEAR is the CPU clipper's z = -w plane. */
inline double PlaneDist(const double c[4], int plane) {
    switch (plane) {
        case PLANE_NEAR: return c[2] + c[3];
        case PLANE_LEFT: return c[3] + c[0];
        case PLANE_RIGHT: return c[3] - c[0];
        case PLANE_BOTTOM: return c[3] + c[1];
        default: return c[3] - c[1];
    }
}

/* A polygon corner: rebased position, its clip coordinates, attributes. */
struct PolyV {
    double p[3];
    double c[4];
    double u, v;
    double cn[4];
    bool orig;
    int16_t rel[3]; /* orig only */
};

inline bool PosLess(const PolyV& x, const PolyV& y) {
    for (int k = 0; k < 3; k++) {
        if (x.p[k] != y.p[k]) {
            return x.p[k] < y.p[k];
        }
    }
    return false;
}

/* The position half of an edge point: where lo-hi (lo = the lexicographically smaller position)
 * meets `plane`, and its clip coordinates. A function of the two positions and the plane only;
 * returns the parameter s from lo toward hi. */
inline double EdgePos(const ClipXf& t, const PolyV& lo, const PolyV& hi, int plane, PolyV& o) {
    const double dl = PlaneDist(lo.c, plane);
    const double dh = PlaneDist(hi.c, plane);
    const double s = dl / (dl - dh);
    for (int k = 0; k < 3; k++) {
        o.p[k] = lo.p[k] + s * (hi.p[k] - lo.p[k]);
    }
    ClipOf(t, o.p, o.c); /* a function of the position only */
    return s;
}

/* The attribute half: interpolated from x toward y at tx. */
inline void EdgeAttr(const PolyV& x, const PolyV& y, double tx, PolyV& o) {
    o.u = x.u + tx * (y.u - x.u);
    o.v = x.v + tx * (y.v - x.v);
    for (int k = 0; k < 4; k++) {
        o.cn[k] = x.cn[k] + tx * (y.cn[k] - x.cn[k]);
    }
    o.orig = false;
    for (int k = 0; k < 3; k++) {
        o.rel[k] = 0;
    }
}

/* Where edge x-y meets `plane` (their distances have opposite signs). */
inline PolyV EdgePoint(const ClipXf& t, const PolyV& x, const PolyV& y, int plane) {
    const bool swap = PosLess(y, x);
    const PolyV& lo = swap ? y : x;
    const PolyV& hi = swap ? x : y;
    PolyV o;
    const double s = EdgePos(t, lo, hi, plane, o);
    EdgeAttr(x, y, swap ? 1.0 - s : s, o); /* attributes: from x toward y */
    return o;
}

/* Sutherland-Hodgman against one plane; `out` holds n + 1. */
inline int ClipPlane(const ClipXf& t, const PolyV* in, int n, PolyV* out, int plane) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const PolyV& cur = in[i];
        const PolyV& nxt = in[(i + 1) % n];
        const bool curIn = PlaneDist(cur.c, plane) >= 0.0;
        const bool nxtIn = PlaneDist(nxt.c, plane) >= 0.0;
        if (curIn) {
            out[m++] = cur;
        }
        if (curIn != nxtIn) {
            out[m++] = EdgePoint(t, cur, nxt, plane);
        }
    }
    return m;
}

constexpr int kClipPolyMax = 8; /* a triangle cut by five planes */

/* The triangle clipped by every plane in `mask`, in the fixed plane order; 0 = nothing left. */
inline int ClipPoly(const ClipXf& t, const PolyV in[3], PolyV out[kClipPolyMax], uint32_t mask) {
    PolyV a[kClipPolyMax + 1];
    PolyV b[kClipPolyMax + 1];
    int n = 3;
    for (int i = 0; i < 3; i++) {
        a[i] = in[i];
    }
    for (int plane = 0; plane < PLANE_COUNT && n >= 3; plane++) {
        if ((mask & (1u << plane)) == 0) {
            continue;
        }
        n = ClipPlane(t, a, n, b, plane);
        if (n > kClipPolyMax) {
            n = kClipPolyMax; /* unreachable for a convex polygon and five planes */
        }
        for (int i = 0; i < n; i++) {
            a[i] = b[i];
        }
    }
    if (n < 3) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        out[i] = a[i];
    }
    return n;
}

/* ---- the lean clipper ([native] track_nearclip=1, C5 Stage 3d) ------------------------------
 *
 * ClipPoly's result, bit for bit, with less work: the polygon is a list of pointers (no 112-byte
 * corner copies per plane), each corner's plane distance is computed once per plane (ClipPlane
 * computes it twice, as `cur` and as `nxt`: the same value), and the position half of an edge
 * point between two ORIGINAL corners is kept in a small cache, so the neighbouring triangle that
 * shares the edge (a strip's next triangle, which walks it the other way) reuses it instead of
 * recomputing it. The position half is a function of the two positions and the plane only
 * (EdgePos, from the lexicographically smaller end: that is what makes shared edges seal), so a
 * cached point IS the point EdgePos would compute; the attributes depend on the direction and are
 * recomputed (EdgeAttr) every time. Corners are named by `key`: a nonzero value that names one
 * position under one `tag` (the retained path passes the slot's position version and the pass). */
struct EdgeCache {
    static constexpr int kN = 16;
    struct Entry {
        uint32_t tag, lo, hi, stamp;
        int plane;
        double s;
        double p[3];
        double c[4];
    };
    Entry e[kN];
    int next;
    uint32_t stamps;
    uint32_t hits, misses;
};

inline int EdgeFind(const EdgeCache& ec, uint32_t tag, uint32_t lo, uint32_t hi, int plane) {
    for (int i = 0; i < EdgeCache::kN; i++) {
        const EdgeCache::Entry& en = ec.e[i];
        if (en.stamp != 0 && en.tag == tag && en.lo == lo && en.hi == hi && en.plane == plane) {
            return i;
        }
    }
    return -1;
}

/* EdgePoint(x, y, plane), through the cache when both ends are original corners; *entry = the
 * cache entry that holds the point's position half (-1 = not cached). */
inline PolyV EdgePointCached(const ClipXf& t, const PolyV& x, uint32_t kx, const PolyV& y, uint32_t ky, int plane,
                             uint32_t tag, EdgeCache* ec, int* entry) {
    const bool swap = PosLess(y, x);
    const PolyV& lo = swap ? y : x;
    const PolyV& hi = swap ? x : y;
    const uint32_t klo = swap ? ky : kx;
    const uint32_t khi = swap ? kx : ky;
    PolyV o;
    double s;
    *entry = -1;
    const int hit = (ec != nullptr && klo != 0 && khi != 0) ? EdgeFind(*ec, tag, klo, khi, plane) : -1;
    if (hit >= 0) {
        const EdgeCache::Entry& en = ec->e[hit];
        memcpy(o.p, en.p, sizeof(o.p));
        memcpy(o.c, en.c, sizeof(o.c));
        s = en.s;
        ec->hits++;
        *entry = hit;
    } else {
        s = EdgePos(t, lo, hi, plane, o);
        if (ec != nullptr && klo != 0 && khi != 0) {
            const int i = ec->next;
            ec->next = (ec->next + 1) % EdgeCache::kN;
            EdgeCache::Entry& en = ec->e[i];
            en.tag = tag;
            en.lo = klo;
            en.hi = khi;
            en.plane = plane;
            en.s = s;
            memcpy(en.p, o.p, sizeof(en.p));
            memcpy(en.c, o.c, sizeof(en.c));
            en.stamp = ++ec->stamps != 0 ? ec->stamps : ++ec->stamps;
            ec->misses++;
            *entry = i;
        }
    }
    EdgeAttr(x, y, swap ? 1.0 - s : s, o);
    return o;
}

/* ClipPoly(in, mask) as pointers into `in` and `pool` (poolCap corners); outKey = the corner's
 * key (0 for a clip point), outStamp = the stamp of the edge-cache entry holding its position
 * half (0 none: an entry is still that point while its stamp is unchanged). Returns the corner
 * count (0 = nothing left) or -1 when the pool is too small (the caller uses ClipPoly). */
inline int LeanClipPoly(const ClipXf& t, const PolyV in[3], const uint32_t key[3], uint32_t tag, EdgeCache* ec,
                        PolyV* pool, int poolCap, const PolyV** out, uint32_t* outKey, int* outEntry,
                        uint32_t* outStamp, uint32_t mask) {
    const PolyV* ping[2][kClipPolyMax + 1];
    uint32_t keys[2][kClipPolyMax + 1];
    int ents[2][kClipPolyMax + 1];
    uint32_t stamps[2][kClipPolyMax + 1];
    int cur = 0;
    int n = 3;
    int np = 0;
    for (int i = 0; i < 3; i++) {
        ping[0][i] = &in[i];
        keys[0][i] = key[i];
        ents[0][i] = -1;
        stamps[0][i] = 0;
    }
    for (int plane = 0; plane < PLANE_COUNT && n >= 3; plane++) {
        if ((mask & (1u << plane)) == 0) {
            continue;
        }
        bool inside[kClipPolyMax + 1];
        for (int i = 0; i < n; i++) {
            inside[i] = PlaneDist(ping[cur][i]->c, plane) >= 0.0;
        }
        const int nxt = 1 - cur;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            if (inside[i]) {
                ping[nxt][m] = ping[cur][i];
                keys[nxt][m] = keys[cur][i];
                ents[nxt][m] = ents[cur][i];
                stamps[nxt][m] = stamps[cur][i];
                m++;
            }
            if (inside[i] != inside[j]) {
                if (np >= poolCap) {
                    return -1;
                }
                int entry = -1;
                pool[np] = EdgePointCached(t, *ping[cur][i], keys[cur][i], *ping[cur][j], keys[cur][j], plane, tag, ec,
                                           &entry);
                ping[nxt][m] = &pool[np];
                keys[nxt][m] = 0;
                ents[nxt][m] = entry;
                stamps[nxt][m] = entry >= 0 ? ec->e[entry].stamp : 0u;
                np++;
                m++;
            }
        }
        n = m > kClipPolyMax ? kClipPolyMax : m; /* unreachable for a convex polygon and five planes */
        cur = nxt;
    }
    if (n < 3) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        out[i] = ping[cur][i];
        outKey[i] = keys[cur][i];
        outEntry[i] = ents[cur][i];
        outStamp[i] = stamps[cur][i];
    }
    return n;
}

/* ---- the visible-region bound -------------------------------------------------------------
 *
 * The per-vertex bound max(e / w) charges a triangle for corners the viewport never shows (a
 * corner far to the side of a near triangle). A rasterised point is the blend sum(l_i V_i) of
 * the corners, so its screen error is at most sum(l_i e_i) / sum(l_i w_i): a linear-fractional
 * function of the barycentrics, whose maximum over any convex region is taken at a vertex of
 * that region. Over the part of the triangle inside the four side planes (all the viewport can
 * show) that is the clipped polygon's corners, with e and w carried linearly. Rigorous, and only
 * evaluated for the few triangles the cheap bound refuses. */
struct BoundV {
    double x, y, w, e;
};

inline double SidePlane(const BoundV& v, int plane) {
    switch (plane) {
        case 0: return v.w - v.x;
        case 1: return v.w + v.x;
        case 2: return v.w - v.y;
        default: return v.w + v.y;
    }
}

inline int ClipSide(const BoundV* in, int n, BoundV* out, int plane) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const BoundV& cur = in[i];
        const BoundV& nxt = in[(i + 1) % n];
        const double dc = SidePlane(cur, plane);
        const double dn = SidePlane(nxt, plane);
        if (dc >= 0.0) {
            out[m++] = cur;
        }
        if ((dc >= 0.0) != (dn >= 0.0)) {
            const double t = dc / (dc - dn);
            out[m].x = cur.x + t * (nxt.x - cur.x);
            out[m].y = cur.y + t * (nxt.y - cur.y);
            out[m].w = cur.w + t * (nxt.w - cur.w);
            out[m].e = cur.e + t * (nxt.e - cur.e);
            m++;
        }
    }
    return m;
}

/* 1/1000 px over the visible part; *visible = false when the viewport shows none of it. */
inline uint32_t VisibleBoundMpx(const BoundV* corners, int n, bool* visible) {
    BoundV a[12];
    BoundV b[12];
    int m = n;
    for (int i = 0; i < n; i++) {
        a[i] = corners[i];
    }
    for (int plane = 0; plane < 4 && m > 0; plane++) {
        m = ClipSide(a, m, b, plane);
        for (int i = 0; i < m; i++) {
            a[i] = b[i];
        }
    }
    *visible = m >= 3;
    double worst = 0.0;
    for (int i = 0; i < m; i++) {
        if (!(a[i].w > 0.0)) {
            return 4000000000u;
        }
        const double r = a[i].e / a[i].w;
        worst = r > worst ? r : worst;
    }
    const double px = worst * 200.0 * 1000.0;
    return px > 4.0e9 ? 4000000000u : (uint32_t)px;
}

/* ---- [native] track_resident: one check per ring instead of per vertex (C5 Stage 3c) ---------
 *
 * A resident ring is drawn only if EVERY point of its bounding sphere (centre c relative to the
 * pass origin, radius r) is in front of the near plane, inside the 2w box when F3DEX2.Rej is
 * loaded, inside the side planes when the viewport is folded, and above the w the float24 bound
 * is divided by. Clip coordinates are affine in the position, so over the sphere each plane
 * function L ranges over L(c) +- r |grad L|: the norms below are those gradients' lengths (the
 * linear part only: they do not depend on the origin). */
struct RingNorms {
    double w;       /* |grad w| */
    double nearZ;   /* |grad (z + w)| */
    double side[4]; /* |grad (w -+ x)|, |grad (w -+ y)| */
    double rej[4];  /* |grad (2w -+ x)|, |grad (2w -+ y)| */
};

inline double Norm3(double a, double b, double c) {
    return std::sqrt(a * a + b * b + c * c);
}

constexpr double kRingSx[4] = { -1.0, 1.0, 0.0, 0.0 };
constexpr double kRingSy[4] = { 0.0, 0.0, -1.0, 1.0 };

inline void RingNormsOf(const ClipXf& t, RingNorms* n) {
    double gx[3];
    double gy[3];
    double gz[3];
    double gw[3];
    for (int k = 0; k < 3; k++) {
        gx[k] = t.m[k][0] * t.xs;
        gy[k] = t.m[k][1];
        gz[k] = t.m[k][2];
        gw[k] = t.m[k][3];
    }
    n->w = Norm3(gw[0], gw[1], gw[2]);
    n->nearZ = Norm3(gz[0] + gw[0], gz[1] + gw[1], gz[2] + gw[2]);
    for (int p = 0; p < 4; p++) {
        double a[3];
        double b[3];
        for (int k = 0; k < 3; k++) {
            a[k] = gw[k] + kRingSx[p] * gx[k] + kRingSy[p] * gy[k];
            b[k] = 2.0 * gw[k] + kRingSx[p] * gx[k] + kRingSy[p] * gy[k];
        }
        n->side[p] = Norm3(a[0], a[1], a[2]);
        n->rej[p] = Norm3(b[0], b[1], b[2]);
    }
}

enum { RING_OK = 0, RING_NEAR = 1, RING_REJ = 2, RING_SIDE = 3 };

/* The planes a ring's sphere must clear; *wMin = the smallest w over the sphere, *wCentre = w(c).
 * A small relative margin keeps the fp32 decisions of the per-frame path (which may round a
 * vertex on a plane either way) on the same side. */
inline int RingPlanes(const ClipXf& t, const RingNorms& n, const double c[3], double r, bool side, bool rej,
                      double* wMin, double* wCentre) {
    double q[4];
    ClipOf(t, c, q);
    const double margin = 1.0e-4 * (std::fabs(q[3]) + r * n.w) + 1.0e-3;
    *wCentre = q[3];
    *wMin = q[3] - r * n.w;
    if (!(r >= 0.0) || *wMin <= margin || q[2] + q[3] - r * n.nearZ <= margin) {
        return RING_NEAR;
    }
    for (int p = 0; p < 4 && rej; p++) {
        if (2.0 * q[3] + kRingSx[p] * q[0] + kRingSy[p] * q[1] - r * n.rej[p] <= margin) {
            return RING_REJ;
        }
    }
    for (int p = 0; p < 4 && side; p++) {
        if (q[3] + kRingSx[p] * q[0] + kRingSy[p] * q[1] - r * n.side[p] <= margin) {
            return RING_SIDE;
        }
    }
    return RING_OK;
}

/* gpuxf's float24 bound, 1/1000 px, of every point with |p_k| <= a[k] (relative to the origin)
 * and w >= wMin under the PICA's matrix m (rows 0-2 linear, row 3 translation): each row's
 * 2^-14 (sum |m_k| a_k + |m_3|) is monotone in the a_k, so it bounds every such vertex's own
 * per-vertex term; dividing by the smallest w bounds every e / w, hence (mediant) every
 * rasterised point of every triangle made of them. */
inline uint32_t RingBoundMpx(const float m[4][4], float xs, const float a[3], double wMin) {
    float e[4];
    for (int i = 0; i < 4; i++) {
        e[i] = (std::fabs(m[0][i]) * a[0] + std::fabs(m[1][i]) * a[1] + std::fabs(m[2][i]) * a[2] + std::fabs(m[3][i])) *
               (1.0f / 16384.0f);
    }
    const float ex = e[0] * xs;
    const float exy = (ex > e[1] ? ex : e[1]) + e[3];
    if (!(wMin > 0.0)) {
        return 4000000000u;
    }
    const double px = (double)exy / wMin * 200.0 * 1000.0;
    return px > 4.0e9 ? 4000000000u : (uint32_t)std::ceil(px);
}

/* The exact version of RingPlanes + RingBoundMpx over the ring's own vertices (integer positions
 * relative to the origin): a triangle of the ring is a convex combination of its corners, so the
 * corners clearing a plane is exactly the triangles clearing it. The near and side decisions use
 * the per-frame path's own double clip coordinates (ClipOf of the same integers), so a ring passes
 * iff none of its triangles would be clipped there; the 2w box keeps a small relative margin
 * against the CPU path's fp32 world-space values. *bound = the largest per-vertex e / w, 1/1000 px
 * (the per-frame path's per-corner bound, so every triangle's own bound is at most this). */
inline int RingPlanesVerts(const ClipXf& t, const float m[4][4], float xs, const int32_t (*rel)[3], int n, bool side,
                           bool rej, double* wMin, uint32_t* bound) {
    *wMin = 1.0e300;
    *bound = 0;
    for (int v = 0; v < n; v++) {
        const double p[3] = { (double)rel[v][0], (double)rel[v][1], (double)rel[v][2] };
        double q[4];
        ClipOf(t, p, q);
        *wMin = q[3] < *wMin ? q[3] : *wMin;
        if (!(q[3] > 0.0) || PlaneDist(q, PLANE_NEAR) < 0.0) {
            return RING_NEAR;
        }
        const double margin = 1.0e-4 * q[3];
        for (int pl = 0; pl < 4 && rej; pl++) {
            if (2.0 * q[3] + kRingSx[pl] * q[0] + kRingSy[pl] * q[1] <= margin) {
                return RING_REJ;
            }
        }
        for (int pl = PLANE_LEFT; pl < PLANE_COUNT && side; pl++) {
            if (PlaneDist(q, pl) < 0.0) {
                return RING_SIDE;
            }
        }
        const float a[3] = { std::fabs((float)rel[v][0]), std::fabs((float)rel[v][1]), std::fabs((float)rel[v][2]) };
        const uint32_t b = RingBoundMpx(m, xs, a, q[3]);
        *bound = b > *bound ? b : *bound;
    }
    return RING_OK;
}

} // namespace GdxTrkRr

#endif /* GDX3DS_TRACK_RETAINED_MATH_H */
