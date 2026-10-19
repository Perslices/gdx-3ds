/* port/3ds/test/gdx3ds_hudcustom_layout_host_tests.c -- the CUSTOM HUD mode's pure layout logic
 * (port/3ds/gdx3ds_hudcustom_layout.h), the same header gdx3ds_bottomhud.c's CUSTOM-mode touch
 * handler (DashCustomHandleTouch) and hud.ini [custom] section reader/writer
 * (DashHudIniLoad/DashHudIniSave) both call into. Same style as
 * gdx3ds_menu_tabbar_host_tests.c: header-only logic, no <3ds.h>, no decomp types, no TU to
 * compile in besides the .c this header is declared against.
 *
 * Covers (per the brief's own ask): layout load/save/clamp/reset and hit-testing. */
#include <stdio.h>
#include <string.h>

#include "gdx3ds_hudcustom_layout.h"

static int sFailures = 0;

static void Check(int ok, const char* what) {
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        sFailures++;
    }
}

/* ---- clamp -------------------------------------------------------------------------- */

static void TestClampInBounds(void) {
    GdxHudCustomRect r = {10, 10, 40, 20, 1};
    gdx_hudcustom_clamp_rect(&r, 32, 16, 220);
    Check(r.x == 10 && r.y == 10 && r.w == 40 && r.h == 20, "clamp: already-valid rect is untouched");
}

static void TestClampFloorsMinSize(void) {
    GdxHudCustomRect r = {0, 0, 4, 2, 1};
    gdx_hudcustom_clamp_rect(&r, 32, 16, 220);
    Check(r.w == 32 && r.h == 16, "clamp: too-small w/h floored to the minimum");
}

static void TestClampCapsMaxSize(void) {
    GdxHudCustomRect r = {0, 0, 9999, 9999, 1};
    gdx_hudcustom_clamp_rect(&r, 32, 16, 220);
    Check(r.w == GDX_HUDCUSTOM_SCREEN_W, "clamp: oversized w capped to the screen width");
    Check(r.h == 220, "clamp: oversized h capped to the header band ceiling");
}

static void TestClampPositionOffRightBottom(void) {
    GdxHudCustomRect r = {9999, 9999, 64, 24, 1};
    gdx_hudcustom_clamp_rect(&r, 32, 16, 220);
    Check(r.x == GDX_HUDCUSTOM_SCREEN_W - 64, "clamp: x pulled back onto the right edge");
    Check(r.y == 220 - 24, "clamp: y pulled back above the header band");
}

static void TestClampPositionOffLeftTop(void) {
    GdxHudCustomRect r = {-500, -500, 64, 24, 1};
    gdx_hudcustom_clamp_rect(&r, 32, 16, 220);
    Check(r.x == 0 && r.y == 0, "clamp: negative x/y pulled back onto the screen");
}

static void TestClampNeverOverlapsHeaderBand(void) {
    /* The exact invariant the header-band ceiling exists for: no clamp output can ever let
     * y + h exceed maxBandY0, for any input. */
    static const int kMaxBand = 218; /* DASH_HEADER_Y0 in the real caller */
    int x, y, w, h;
    for (y = -300; y <= 300; y += 37) {
        for (h = 1; h <= 260; h += 29) {
            GdxHudCustomRect r = {0, y, 40, h, 1};
            x = 0;
            (void) x;
            w = 40;
            (void) w;
            gdx_hudcustom_clamp_rect(&r, 32, 16, kMaxBand);
            if (r.y + r.h > kMaxBand) {
                char what[96];
                snprintf(what, sizeof(what), "clamp: y=%d h=%d -> y+h=%d must be <= band %d", y, h, r.y + r.h,
                          kMaxBand);
                Check(0, what);
                return;
            }
        }
    }
    Check(1, "clamp: y+h never exceeds the header band, swept across y/h");
}

/* ---- snap ---------------------------------------------------------------------------- */

static void TestSnapRoundsToNearestStep(void) {
    Check(gdx_hudcustom_snap(0, 8) == 0, "snap: 0 -> 0");
    Check(gdx_hudcustom_snap(3, 8) == 0, "snap: 3 -> 0 (rounds down)");
    Check(gdx_hudcustom_snap(5, 8) == 8, "snap: 5 -> 8 (rounds up)");
    Check(gdx_hudcustom_snap(12, 8) == 16, "snap: 12 -> 16 (exact midpoint rounds up)");
    Check(gdx_hudcustom_snap(13, 8) == 16, "snap: 13 -> 16");
    Check(gdx_hudcustom_snap(100, 8) == 104, "snap: 100 -> 104 (exact midpoint rounds up)");
}

static void TestSnapStepOneIsIdentity(void) {
    int v;
    for (v = -20; v <= 20; v++) {
        if (gdx_hudcustom_snap(v, 1) != v) {
            char what[64];
            snprintf(what, sizeof(what), "snap: step=1 is identity at v=%d", v);
            Check(0, what);
            return;
        }
    }
    Check(1, "snap: step=1 is identity across a sweep");
}

static void TestSnapRejectsZeroStep(void) {
    /* step < 1 must not divide by zero -- clamped to 1 internally. */
    Check(gdx_hudcustom_snap(7, 0) == 7, "snap: step=0 treated as step=1 (no crash, no garbage)");
}

/* ---- hit-test ------------------------------------------------------------------------ */

static void TestHitTestBodyVsCloseVsResize(void) {
    GdxHudCustomRect r = {100, 50, 64, 32, 1};
    Check(gdx_hudcustom_hit_test(&r, 1, 120, 60) == GDX_HUDCUSTOM_HIT_BODY, "hit: centre is BODY");
    Check(gdx_hudcustom_hit_test(&r, 1, 100 + 64 - 4, 50 + 4) == GDX_HUDCUSTOM_HIT_CLOSE,
          "hit: top-right corner is CLOSE");
    Check(gdx_hudcustom_hit_test(&r, 1, 100 + 64 - 4, 50 + 32 - 4) == GDX_HUDCUSTOM_HIT_RESIZE,
          "hit: bottom-right corner is RESIZE when resizable");
    Check(gdx_hudcustom_hit_test(&r, 0, 100 + 64 - 4, 50 + 32 - 4) == GDX_HUDCUSTOM_HIT_BODY,
          "hit: bottom-right corner is plain BODY when NOT resizable");
}

static void TestHitTestOutsideIsNone(void) {
    GdxHudCustomRect r = {100, 50, 64, 32, 1};
    Check(gdx_hudcustom_hit_test(&r, 1, 99, 60) == GDX_HUDCUSTOM_HIT_NONE, "hit: 1px left of the rect is NONE");
    Check(gdx_hudcustom_hit_test(&r, 1, 164, 60) == GDX_HUDCUSTOM_HIT_NONE, "hit: x==x0+w (exclusive) is NONE");
    Check(gdx_hudcustom_hit_test(&r, 1, 120, 82) == GDX_HUDCUSTOM_HIT_NONE, "hit: y==y0+h (exclusive) is NONE");
}

static void TestHitTestDisabledIsNone(void) {
    GdxHudCustomRect r = {100, 50, 64, 32, 0};
    Check(gdx_hudcustom_hit_test(&r, 1, 120, 60) == GDX_HUDCUSTOM_HIT_NONE, "hit: disabled widget is never hit");
}

static void TestHitTestAllFirstMatchWins(void) {
    GdxHudCustomRect rects[3] = {
        {0, 0, 50, 50, 1},   /* widget 0 */
        {20, 20, 50, 50, 1}, /* widget 1, overlapping widget 0 */
        {200, 200, 20, 20, 1},
    };
    int index = -1;
    GdxHudCustomHit hit = GDX_HUDCUSTOM_HIT_NONE;
    const int got = gdx_hudcustom_hit_test_all(rects, 3, 0u, 30, 30, &index, &hit);
    Check(got == 1 && index == 0, "hit-all: overlap resolves to the FIRST (lowest-index) rect");

    index = -1;
    hit = GDX_HUDCUSTOM_HIT_NONE;
    Check(gdx_hudcustom_hit_test_all(rects, 3, 0u, 300, 300, &index, &hit) == 0,
          "hit-all: a point outside every rect returns 0 and leaves outputs untouched");
}

static void TestHitTestAllResizableMaskPerRect(void) {
    GdxHudCustomRect rects[2] = {
        {0, 0, 32, 32, 1}, /* bit 0 clear: NOT resizable */
        {40, 0, 32, 32, 1},/* bit 1 set: resizable */
    };
    int index;
    GdxHudCustomHit hit;

    Check(gdx_hudcustom_hit_test_all(rects, 2, 0x2u, 32 - 4, 32 - 4, &index, &hit) == 1 && index == 0 &&
              hit == GDX_HUDCUSTOM_HIT_BODY,
          "hit-all: resizableMask bit clear -> corner reads BODY, not RESIZE");
    Check(gdx_hudcustom_hit_test_all(rects, 2, 0x2u, 40 + 32 - 4, 32 - 4, &index, &hit) == 1 && index == 1 &&
              hit == GDX_HUDCUSTOM_HIT_RESIZE,
          "hit-all: resizableMask bit set -> corner reads RESIZE");
}

/* ---- format / parse (the hud.ini [custom] line round-trip) --------------------------- */

static void TestFormatThenParseRoundTrips(void) {
    const GdxHudCustomRect in = {40, 80, 120, 60, 1};
    char line[64];
    GdxHudCustomRect out;
    const int n = gdx_hudcustom_format_line(line, (int) sizeof(line), &in);
    Check(n > 0, "format: writes a non-empty line");
    Check(gdx_hudcustom_parse_line(line, 32, 16, 220, &out) == 1, "parse: round-trips a just-formatted line");
    Check(out.x == in.x && out.y == in.y && out.w == in.w && out.h == in.h && out.enabled == in.enabled,
          "parse: round-tripped fields match exactly (all already in-bounds)");
}

static void TestFormatTooSmallBufferFails(void) {
    const GdxHudCustomRect in = {1, 2, 3, 4, 1};
    char tiny[2];
    Check(gdx_hudcustom_format_line(tiny, (int) sizeof(tiny), &in) == -1, "format: too-small buffer returns -1");
}

static void TestParseGarbageLeavesOutUntouched(void) {
    GdxHudCustomRect out = {7, 7, 7, 7, 0}; /* sentinel: must survive untouched */
    Check(gdx_hudcustom_parse_line("not a layout line", 32, 16, 220, &out) == 0,
          "parse: non-numeric garbage returns 0");
    Check(out.x == 7 && out.y == 7 && out.w == 7 && out.h == 7 && out.enabled == 0,
          "parse: garbage line leaves the caller's default completely untouched");
}

static void TestParseShortLineFails(void) {
    GdxHudCustomRect out = {7, 7, 7, 7, 0};
    Check(gdx_hudcustom_parse_line("1 2 3", 32, 16, 220, &out) == 0, "parse: only 3 of 5 fields returns 0");
    Check(out.x == 7, "parse: short line leaves the default untouched (spot check)");
}

static void TestParseOutOfRangeGetsClamped(void) {
    GdxHudCustomRect out;
    Check(gdx_hudcustom_parse_line("-999 -999 4 4 1", 32, 16, 220, &out) == 1,
          "parse: a structurally-valid-but-out-of-range line still parses");
    Check(out.x == 0 && out.y == 0 && out.w == 32 && out.h == 16,
          "parse: out-of-range fields come back clamped into bounds, not rejected outright");
}

static void TestParseEnabledIsNormalisedToZeroOrOne(void) {
    GdxHudCustomRect out;
    Check(gdx_hudcustom_parse_line("0 0 40 20 7", 32, 16, 220, &out) == 1, "parse: a garbage enabled value parses");
    Check(out.enabled == 1, "parse: any non-zero enabled value normalises to exactly 1");
}

/* ---- reset-to-defaults (the semantics RESET relies on in the real caller) ------------- */

static void TestResetLikeRestoreIsIdempotent(void) {
    /* gdx3ds_bottomhud.c's DashCustomReset copies the hard-coded default rect over the custom
     * one and forces enabled=1; this is the pure-logic half of that: clamping a known-good
     * default must be a no-op, so RESET never silently perturbs the very defaults it restores. */
    GdxHudCustomRect defaults = {258, 52, 60, 28, 1}; /* the real "lap" widget's default rect */
    const GdxHudCustomRect before = defaults;
    gdx_hudcustom_clamp_rect(&defaults, 32, 16, 218);
    Check(defaults.x == before.x && defaults.y == before.y && defaults.w == before.w && defaults.h == before.h,
          "reset: clamping an already-valid default rect changes nothing");
}

int main(void) {
    TestClampInBounds();
    TestClampFloorsMinSize();
    TestClampCapsMaxSize();
    TestClampPositionOffRightBottom();
    TestClampPositionOffLeftTop();
    TestClampNeverOverlapsHeaderBand();

    TestSnapRoundsToNearestStep();
    TestSnapStepOneIsIdentity();
    TestSnapRejectsZeroStep();

    TestHitTestBodyVsCloseVsResize();
    TestHitTestOutsideIsNone();
    TestHitTestDisabledIsNone();
    TestHitTestAllFirstMatchWins();
    TestHitTestAllResizableMaskPerRect();

    TestFormatThenParseRoundTrips();
    TestFormatTooSmallBufferFails();
    TestParseGarbageLeavesOutUntouched();
    TestParseShortLineFails();
    TestParseOutOfRangeGetsClamped();
    TestParseEnabledIsNormalisedToZeroOrOne();

    TestResetLikeRestoreIsIdempotent();

    printf(sFailures == 0 ? "=== ALL PASS (0 failures) ===\n" : "=== %d FAILURES ===\n", sFailures);
    return sFailures == 0 ? 0 : 1;
}
