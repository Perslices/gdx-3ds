/* port/3ds/gdx3ds_hudcustom_layout.c -- implementation. See the header for the contract; this
 * file is deliberately free of <3ds.h> and decomp types so it links into a host test binary. */
#include "gdx3ds_hudcustom_layout.h"

#include <stdio.h>

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

void gdx_hudcustom_clamp_rect(GdxHudCustomRect* r, int minW, int minH, int maxBandY0) {
    if (minW < 1) {
        minW = 1;
    }
    if (minH < 1) {
        minH = 1;
    }
    if (maxBandY0 < minH) {
        maxBandY0 = minH; /* degenerate caller input: still leave room for one widget */
    }

    if (r->w < minW) {
        r->w = minW;
    }
    if (r->w > GDX_HUDCUSTOM_SCREEN_W) {
        r->w = GDX_HUDCUSTOM_SCREEN_W;
    }
    if (r->h < minH) {
        r->h = minH;
    }
    if (r->h > maxBandY0) {
        r->h = maxBandY0;
    }

    r->x = ClampInt(r->x, 0, GDX_HUDCUSTOM_SCREEN_W - r->w);
    r->y = ClampInt(r->y, 0, maxBandY0 - r->h);
}

int gdx_hudcustom_snap(int v, int step) {
    int half;
    if (step < 1) {
        step = 1;
    }
    half = step / 2;
    if (v >= 0) {
        return ((v + half) / step) * step;
    }
    /* Round-half-away-from-zero for negatives too, so a caller that ever snaps a signed delta
     * (not just an absolute screen coordinate) doesn't get an asymmetric bias at zero. */
    return -(((-v + half) / step) * step);
}

GdxHudCustomHit gdx_hudcustom_hit_test(const GdxHudCustomRect* r, int resizable, int px, int py) {
    if (!r->enabled) {
        return GDX_HUDCUSTOM_HIT_NONE;
    }
    if (px < r->x || px >= r->x + r->w || py < r->y || py >= r->y + r->h) {
        return GDX_HUDCUSTOM_HIT_NONE;
    }
    if (px >= r->x + r->w - GDX_HUDCUSTOM_HANDLE && px < r->x + r->w - 1 && py >= r->y + 1 &&
        py < r->y + 1 + GDX_HUDCUSTOM_HANDLE) {
        return GDX_HUDCUSTOM_HIT_CLOSE;
    }
    if (resizable && px >= r->x + r->w - GDX_HUDCUSTOM_HANDLE && px < r->x + r->w &&
        py >= r->y + r->h - GDX_HUDCUSTOM_HANDLE && py < r->y + r->h) {
        return GDX_HUDCUSTOM_HIT_RESIZE;
    }
    return GDX_HUDCUSTOM_HIT_BODY;
}

int gdx_hudcustom_hit_test_all(const GdxHudCustomRect* rects, int count, unsigned resizableMask, int px, int py,
                                int* outIndex, GdxHudCustomHit* outHit) {
    int i;
    for (i = 0; i < count; i++) {
        const int resizable = (resizableMask >> (unsigned) i) & 1u;
        const GdxHudCustomHit hit = gdx_hudcustom_hit_test(&rects[i], resizable, px, py);
        if (hit != GDX_HUDCUSTOM_HIT_NONE) {
            *outIndex = i;
            *outHit = hit;
            return 1;
        }
    }
    return 0;
}

int gdx_hudcustom_format_line(char* out, int cap, const GdxHudCustomRect* r) {
    const int n = snprintf(out, (size_t) (cap > 0 ? cap : 0), "%d %d %d %d %d", r->x, r->y, r->w, r->h,
                            r->enabled ? 1 : 0);
    if (n < 0 || n >= cap) {
        return -1;
    }
    return n;
}

int gdx_hudcustom_parse_line(const char* value, int minW, int minH, int maxBandY0, GdxHudCustomRect* out) {
    GdxHudCustomRect r;
    int enabledRaw;
    if (sscanf(value, "%d %d %d %d %d", &r.x, &r.y, &r.w, &r.h, &enabledRaw) != 5) {
        return 0;
    }
    r.enabled = enabledRaw ? 1 : 0;
    gdx_hudcustom_clamp_rect(&r, minW, minH, maxBandY0);
    *out = r;
    return 1;
}
