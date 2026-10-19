/* gdx3ds_xf_census.c — [debug] xf_census (gdx3ds_xf_census.h). */
#include "gdx3ds_xf_census.h"

#ifdef __3DS__
#include <3ds.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __3DS__
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
#endif

int gdx3ds_xf_census_on = 0;
int gdx3ds_xf_census_path = GDX3DS_XFC_DL;

enum { kBuckets = 5 }; /* px error < 1/64, < 1/8, < 1, < 4, >= 4 */

typedef struct XfcPath {
    unsigned n, in, bad, cancel;
    unsigned hist[kBuckets];
    float maxPx;
    float maxWppm;
    float maxRatio; /* over ALL vertices: sum|w-row terms| / |w| (float24 w error ~ 2^-17 x this) */
} XfcPath;

static XfcPath sPath[GDX3DS_XFC_PATHS];
static int sLatched = 0;
static unsigned long sFrame = 0;

/* Round to the PICA's float24 mantissa (16 bits kept of 23), nearest-even; zero/denormal
 * inputs flush to zero as float24 does. The exponent range (bias 63) is not the binding
 * limit for any value a game vertex produces, so it is not modelled. */
static float F24(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    if ((u & 0x7F800000u) == 0) {
        return 0.0f;
    }
    u = (u + 0x3Fu + ((u >> 7) & 1u)) & ~0x7Fu;
    memcpy(&v, &u, sizeof(v));
    return v;
}

/* The shader's dp4 of one output row: float24 operands, products and partial sums. */
static float Dp4F24f(const float ob[3], const float mp[4][4], int row, float scale) {
    float acc = F24(F24(mp[0][row] * scale) * F24(ob[0]));
    acc = F24(acc + F24(F24(mp[1][row] * scale) * F24(ob[1])));
    acc = F24(acc + F24(F24(mp[2][row] * scale) * F24(ob[2])));
    return F24(acc + F24(mp[3][row] * scale));
}

static int Bucket(float px) {
    if (px < 1.0f / 64.0f) {
        return 0;
    }
    if (px < 0.125f) {
        return 1;
    }
    if (px < 1.0f) {
        return 2;
    }
    return px < 4.0f ? 3 : 4;
}

void gdx3ds_xf_census_note_f(const float ob[3], const float mp[4][4], float xScale, float x, float y, float w) {
    const unsigned path = (unsigned)gdx3ds_xf_census_path;
    XfcPath* p = &sPath[path < GDX3DS_XFC_PATHS ? path : 0u];
    p->n++;
    {
        const float terms = fabsf(mp[0][3] * ob[0]) + fabsf(mp[1][3] * ob[1]) + fabsf(mp[2][3] * ob[2]) + fabsf(mp[3][3]);
        const float aw = fabsf(w);
        const float ratio = aw > 1.0e-6f ? terms / aw : 1.0e9f;
        if (ratio > p->maxRatio) {
            p->maxRatio = ratio;
        }
        if (ratio > 1024.0f) {
            p->cancel++; /* float24 w off by more than ~1% of itself: perspective weights wrong */
        }
    }
    if (!(w > 1.0e-3f) || fabsf(x) > 2.0f * w || fabsf(y) > 2.0f * w) {
        return;
    }
    p->in++;
    const float x24 = Dp4F24f(ob, mp, 0, xScale);
    const float y24 = Dp4F24f(ob, mp, 1, 1.0f);
    const float w24 = Dp4F24f(ob, mp, 3, 1.0f);
    float px = 1.0e9f;
    if (w24 > 0.0f) {
        const float ex = fabsf(x24 / w24 - x / w) * 120.0f;
        const float ey = fabsf(y24 / w24 - y / w) * 200.0f;
        px = ex > ey ? ex : ey;
    }
    const float wppm = fabsf(w24 - w) / w * 1.0e6f;
    p->hist[Bucket(px)]++;
    if (px > 0.125f) {
        p->bad++;
    }
    if (px > p->maxPx) {
        p->maxPx = px;
    }
    if (wppm > p->maxWppm) {
        p->maxWppm = wppm;
    }
}

/* The edge's clip point: float24 endpoints, distances, parameter and blend vs the exact point. */
static void ClipRow4(const float ob[3], const float mp[4][4], float xScale, float c[4]) {
    c[0] = Dp4F24f(ob, mp, 0, xScale);
    c[1] = Dp4F24f(ob, mp, 1, 1.0f);
    c[2] = Dp4F24f(ob, mp, 2, 1.0f);
    c[3] = Dp4F24f(ob, mp, 3, 1.0f);
}

static void ClipRow4d(const float ob[3], const float mp[4][4], float xScale, double c[4]) {
    for (int r = 0; r < 4; r++) {
        c[r] = (double)ob[0] * mp[0][r] + (double)ob[1] * mp[1][r] + (double)ob[2] * mp[2][r] + mp[3][r];
    }
    c[0] *= xScale;
}

void gdx3ds_xf_census_note_clip(const float a[3], const float b[3], const float mp[4][4], float xScale) {
    XfcPath* p = &sPath[GDX3DS_XFC_NEAR];
    double ad[4];
    double bd[4];
    ClipRow4d(a, mp, xScale, ad);
    ClipRow4d(b, mp, xScale, bd);
    const double da = ad[2] + ad[3];
    const double db = bd[2] + bd[3];
    p->n++;
    if ((da >= 0.0) == (db >= 0.0)) {
        return;
    }
    const double t = da / (da - db);
    double pd[4];
    for (int r = 0; r < 4; r++) {
        pd[r] = ad[r] + t * (bd[r] - ad[r]);
    }
    if (!(pd[3] > 1.0e-3) || fabs(pd[0]) > 2.0 * pd[3] || fabs(pd[1]) > 2.0 * pd[3]) {
        return; /* off screen: the side clippers remove it */
    }
    float af[4];
    float bf[4];
    ClipRow4(a, mp, xScale, af);
    ClipRow4(b, mp, xScale, bf);
    const float fa = F24(af[2] + af[3]);
    const float fb = F24(bf[2] + bf[3]);
    const float tf = F24(fa / F24(fa - fb));
    float pf[4];
    for (int r = 0; r < 4; r++) {
        pf[r] = F24(af[r] + F24(tf * F24(bf[r] - af[r])));
    }
    p->in++;
    float px = 1.0e9f;
    if (pf[3] > 0.0f) {
        const float ex = fabsf((float)((double)pf[0] / pf[3] - pd[0] / pd[3])) * 120.0f;
        const float ey = fabsf((float)((double)pf[1] / pf[3] - pd[1] / pd[3])) * 200.0f;
        px = ex > ey ? ex : ey;
    }
    const float wppm = (float)(fabs(pf[3] - pd[3]) / pd[3] * 1.0e6);
    p->hist[Bucket(px)]++;
    if (px > 0.125f) {
        p->bad++;
    }
    if (px > p->maxPx) {
        p->maxPx = px;
    }
    if (wppm > p->maxWppm) {
        p->maxWppm = wppm;
    }
}

void gdx3ds_xf_census_note(const int16_t ob[3], const float mp[4][4], float xScale, float x, float y, float w) {
    const float f[3] = { (float)ob[0], (float)ob[1], (float)ob[2] }; /* s16 -> float24 is exact */
    gdx3ds_xf_census_note_f(f, mp, xScale, x, y, w);
}

/* Receipt + latch: 3DS only (the host interpreter tests link this file for the note hook and
 * never turn the census on). */
#ifdef __3DS__
static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, (s32)len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static void Latch(void) {
    if (sLatched || &gdx3ds_config_get_int == NULL) {
        return;
    }
    if (&gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return;
    }
    sLatched = 1;
    gdx3ds_xf_census_on = gdx3ds_config_get_int("debug", "xf_census", 0) != 0 ? 1 : 0;
    if (gdx3ds_xf_census_on) {
        Log("[xfcensus] init: float24-simulated shader dp4 vs the fp32 CPU transform, per path "
            "(dl / seam / mesh / trk), every 64 frames");
    }
}
#else
static void Log(const char* fmt, ...) {
    (void)fmt;
}

static void Latch(void) {
    sLatched = 1;
}
#endif

void gdx3ds_xf_census_frame(void) {
    static const char* const kName[GDX3DS_XFC_PATHS] = { "dl", "seam", "mesh", "trk", "near" };
    Latch();
    sFrame++;
    if (!gdx3ds_xf_census_on || (sFrame & 63) != 0) {
        return;
    }
    char line[1024];
    int off = snprintf(line, sizeof(line), "[xfcensus] frame=%lu", sFrame);
    for (int i = 0; i < GDX3DS_XFC_PATHS && off > 0 && off < (int)sizeof(line); i++) {
        const XfcPath* p = &sPath[i];
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        " | %s n=%u in=%u bad=%u px<1/64:%u <1/8:%u <1:%u <4:%u >=4:%u maxpx=%.3f maxw=%.0fppm cancel=%u maxratio=%.0f",
                        kName[i], p->n, p->in, p->bad, p->hist[0], p->hist[1], p->hist[2], p->hist[3], p->hist[4],
                        (double)p->maxPx, (double)p->maxWppm, p->cancel, (double)p->maxRatio);
    }
    Log("%s", line);
    memset(sPath, 0, sizeof(sPath));
}
