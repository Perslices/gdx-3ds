/* port/3ds/gdx3ds_hudcustom_layout.h -- pure logic for the CUSTOM HUD mode's editable widget
 * layout: clamp/snap arithmetic, hit-testing, and the hud.ini [custom] section's per-line
 * format. No <3ds.h>, no decomp types -- host-testable, same separation
 * gdx3ds_menu_tabbar_layout.h already uses for the debug tab bar's own column math (see
 * port/3ds/test/gdx3ds_menu_tabbar_host_tests.c for that precedent, and
 * port/3ds/test/gdx3ds_hudcustom_layout_host_tests.c for this header's own tests).
 *
 * port/3ds/gdx3ds_bottomhud.c is the only caller: it owns touch coordinates, the framebuffer
 * paint and the actual SD file I/O (fopen/fprintf on GDX3DS_HUD_MODE_PATH); this module owns
 * every decision that can be expressed as plain ints/structs in and out, so that decision logic
 * can be verified on a host build instead of only on real/emulated 3DS hardware. */
#ifndef GDX3DS_HUDCUSTOM_LAYOUT_H
#define GDX3DS_HUDCUSTOM_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_HUDCUSTOM_SCREEN_W 320
#define GDX_HUDCUSTOM_SCREEN_H 240
#define GDX_HUDCUSTOM_HANDLE 8 /* close/resize hit-box side, in screen pixels */

/* A deliberately separate, smaller struct from gdx3ds_bottomhud.c's own DashWidgetRect (which
 * also carries a widget id string and a paint-function pointer, both irrelevant to this pure
 * logic). The caller converts between the two with a plain per-field copy -- NOT a pointer cast:
 * DashWidgetRect's own first member is `id` (a pointer), so its x/y/w/h/enabled are not at the
 * same offsets this struct's are. */
typedef struct GdxHudCustomRect {
    int x;
    int y;
    int w;
    int h;
    int enabled;
} GdxHudCustomRect;

typedef enum GdxHudCustomHit {
    GDX_HUDCUSTOM_HIT_NONE = 0,
    GDX_HUDCUSTOM_HIT_CLOSE,
    GDX_HUDCUSTOM_HIT_RESIZE,
    GDX_HUDCUSTOM_HIT_BODY
} GdxHudCustomHit;

/* Clamps `r` fully on-screen: width/height are floored to (minW, minH) and capped so the rect
 * never runs off the right/bottom edge of a GDX_HUDCUSTOM_SCREEN_W x GDX_HUDCUSTOM_SCREEN_H
 * canvas, and y+h is additionally capped at maxBandY0 (the DASH's fixed header band always sits
 * at the bottom of the screen; a widget must never be able to slide under it). Width/height are
 * clamped BEFORE position, so a too-small or too-large save can never leave the rect partially
 * off-screen. `enabled` is left as given (0/1 only -- the caller normalises that separately). */
void gdx_hudcustom_clamp_rect(GdxHudCustomRect* r, int minW, int minH, int maxBandY0);

/* Rounds v to the nearest multiple of step (step clamped to >= 1 internally, so a caller mistake
 * can never divide by zero). */
int gdx_hudcustom_snap(int v, int step);

/* Where does screen point (px,py) land inside rect r? NONE if r is disabled or the point is
 * outside it entirely. Otherwise: the CLOSE hit-box is a GDX_HUDCUSTOM_HANDLE square inset 1px
 * from the top-right corner; the RESIZE hit-box is the same-sized square at the bottom-right
 * corner, but only reachable when `resizable` is non-zero (a non-resizable widget's bottom-right
 * corner is plain BODY, same as the rest of the rect); everything else inside r is BODY. */
GdxHudCustomHit gdx_hudcustom_hit_test(const GdxHudCustomRect* r, int resizable, int px, int py);

/* Scans rects[0..count-1] IN ORDER for the first ENABLED rect (px,py) lands in at all (any hit
 * kind), fills outIndex/outHit and returns 1. Returns 0 (both outputs untouched) if nothing
 * was hit. `resizableMask` bit i set == rects[i] is resizable, passed straight to
 * gdx_hudcustom_hit_test per-rect. This is the one function the caller's touch-down handler
 * needs -- it does not have to re-implement the scan-order/first-match rule itself. */
int gdx_hudcustom_hit_test_all(const GdxHudCustomRect* rects, int count, unsigned resizableMask, int px, int py,
                                int* outIndex, GdxHudCustomHit* outHit);

/* Formats one hud.ini [custom] line's VALUE half as "x y w h v" (no trailing newline, NUL
 * terminated) -- the caller prefixes its own "id = " key. Returns the number of characters
 * written (excluding the NUL) on success, -1 if `cap` was too small to hold the whole line. */
int gdx_hudcustom_format_line(char* out, int cap, const GdxHudCustomRect* r);

/* Parses a hud.ini [custom] line's VALUE half ("x y w h v", any amount of whitespace between
 * fields, leading/trailing whitespace tolerated). On a successful parse of all 5 integer fields,
 * fills *out (clamped via gdx_hudcustom_clamp_rect with the given minW/minH/maxBandY0, `enabled`
 * forced to exactly 0 or 1) and returns 1. On a short/garbage line, returns 0 and leaves *out
 * completely untouched -- the caller's own already-populated default for that widget survives,
 * so one corrupt line can never take the rest of a saved layout down with it. */
int gdx_hudcustom_parse_line(const char* value, int minW, int minH, int maxBandY0, GdxHudCustomRect* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HUDCUSTOM_LAYOUT_H */
