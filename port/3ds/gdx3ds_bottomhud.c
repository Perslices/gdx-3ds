/* port/3ds/gdx3ds_bottomhud.c — nibottom Phase 2: the DASH bottom-screen race HUD.
 * Contract, takeover model and the digit-overlay scope gap: gdx3ds_bottomhud.h.
 *
 * REPAINT-RATE DISCIPLINE (the brief's "dirty-rect repaint only"), per element:
 *   baked backdrop+chrome : once per race (sWasValid's 0->1 edge) — gdx3ds_bottomhud_assets.h.
 *   map box               : throttled to [debug] bottom_race_hz (default 10), own timestamp,
 *                           independent of the MAP tab's always-60Hz box.
 *   top-5 colour tags      : only when top5Character[] differs from the last paint.
 *   energy bar fill + MODE dot : every tick — a few hundred pixels, cheaper to redraw than to
 *                           dirty-track.
 *   live numeric overlay  : every tick while valid (this is the "timer digits per frame"
 *                           element; the others piggyback on the same console-text call for
 *                           simplicity since one extra PaintBegin/PaintRow pair per tick is
 *                           cheap and they must not drift out of sync with the pixel backdrop).
 */
#include "gdx3ds_bottomhud.h"
#include "gdx3ds_bottomhud_assets.h"
#include "gdx3ds_bottomhud_glyphs.h"
#include "gdx3ds_bottomhud_idle_assets.h" /* tools/gen_nibottom_idle_assets.py -- the idle view's logo */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx_racemap.h"        /* GdxRaceHudSnapshot/gdx_racehud_snapshot + the MAP bitmap accessors */
#include "gdx_hud_placement.h" /* GDX_HUD_PLACEMENT_*, GDX_HUD_ELEM_*, gdx_hud_placement_name,
                                * gdx_hud_element_on_top, gdx_hud_set_custom_top_mask (native/
                                * nihudtop's real CUSTOM-mode API -- port/3ds/gdx3ds_hudcustom.h's
                                * local #ifndef stand-ins and its weak gdx3ds_hudcustom_stub.c are
                                * both gone as of the nihudtop merge; this is the one real header) */
#include "gdx3ds_hudcustom_layout.h"  /* pure clamp/snap/hit-test/ini-line logic, host-tested */

/* port/input_bridge.c. Weak: see the matching comment on the KEY_SELECT mode-cycle logic. */
extern int gdx_input_in_gameplay(void) __attribute__((weak));

/* ---- geometry: one data table, not scattered #defines -------------------------------- */
#define DASH_FB_W 320
#define DASH_FB_H 240

/* COORDINATOR FIX (custom-HUD groundwork, hardware feedback round): the coordinator wants a
 * FUTURE on-device "custom" HUD mode where each widget can be dragged, resized or hidden. That
 * needs widget geometry to be RUNTIME data, not a compile-time #define -- a #define can never be
 * written by a future drag gesture or a saved layout file. sDashWidgets below is that single
 * source of truth: one row per widget (id, rect, enabled). The DASH_<widget>_X0/Y0/W/H names
 * every paint function already used are kept (so none of this file's per-widget pixel math has
 * to change) but are now THIN ACCESSORS over this table instead of independent literals --
 * relocating a widget is one row edit here, not a hunt through the file. `paint` records which
 * function owns each widget's per-tick draw, for a future generic "for each enabled widget, call
 * its paint fn" dispatch loop; NOT wired to one yet (see the comment on sDashWidgets itself,
 * below the forward declarations, for exactly why that last step is deferred this round, not
 * done partway). `enabled` IS wired up today (every Dash*Paint* function below bails out first
 * thing when its own row is disabled) -- cheap, safe, and it is the one piece of "hide a widget"
 * a future mode would actually need working now rather than only scaffolded. */
typedef struct DashWidgetRect {
    const char* id;
    int x, y, w, h;
    int enabled;
    void (*paint)(uint16_t* fb, const GdxRaceHudSnapshot* snap);
} DashWidgetRect;

typedef enum DashWidgetId {
    DASHW_ENERGY_BAR = 0,
    DASHW_HEADER,
    DASHW_MODE_CHIP,
    DASHW_MENU_BUTTON,
    DASHW_PORTRAIT,
    DASHW_STANDINGS,
    DASHW_KO_STARS,
    DASHW_LAP,
    DASHW_TIMER,
    DASHW_GAP,
    DASHW_SPEED,
    DASHW_MAP_BOX,
    DASHW_COUNT
} DashWidgetId;

/* Forward declarations purely so sDashWidgets' own `paint` column can reference them -- every
 * function here already exists further down this file with this exact signature, OR (map_box,
 * portrait, standings) is wrapped by a same-signature shim right above the table so the column
 * has no holes. gdx3ds_bottomhud_tick still calls the real per-tick sequence directly, in its own
 * carefully ordered/throttled/dirty-checked shape -- see the note on sDashWidgets for why. */
static void DashPaintEnergyFooter(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintHeaderLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintModeLabel(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintBackButton(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintOurPosition(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintStandingsLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintKOStars(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnLap(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnTimer(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintRightColumnGap(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintSpeed(uint16_t* fb, const GdxRaceHudSnapshot* snap);
static void DashPaintMapBoxLive(uint16_t* fb, const GdxRaceHudSnapshot* snap);

/* COORDINATOR NOTE (why `paint` is populated but gdx3ds_bottomhud_tick does not loop over it
 * yet): several widgets here are not independent per-tick draws -- the map box is cost-throttled
 * to [debug] bottom_race_hz against its OWN timestamp; the standings portraits/rank tags/colour
 * swatches are dirty-checked against Top5Changed and must repaint together, in a fixed order,
 * relative to the baked backdrop; the MODE dot must be painted AFTER DashPaintModeLabel repaints
 * the chip's own interior; and the once-per-race baked backdrop (DashPaintBaked) is not a
 * per-tick call at all. Collapsing that into one uniform "call every enabled widget's paint fn"
 * loop is a real, measurable rewrite of the tick's control flow, not a rename -- and Gate 5 (the
 * nibottom track's own measured main-thread cost budget) and the DASH's documented REPAINT-RATE
 * DISCIPLINE table at the top of this file both depend on exactly this ordering/throttling being
 * right. Rather than risk that under this round's "same visual result, keep the gates green"
 * constraint, the geometry/enabled table above is wired up FULLY (every paint fn already reads
 * its own rect from here, and bails out on `enabled == 0`); the `paint` column is real data (not
 * a placeholder) for a future generic dispatch pass to consume once it re-derives the
 * ordering/throttle rules as per-widget metadata instead of tick()'s hand-written sequence --
 * that re-derivation, not the table itself, is the work left undone this round. */
static DashWidgetRect sDashWidgets[DASHW_COUNT] = {
    { "energy_bar",  0,   0,   320, 24,  1, DashPaintEnergyFooter },
    { "header",      0,   218, 320, 22,  1, DashPaintHeaderLive },
    { "mode_chip",   168, 221, 62,  16,  1, DashPaintModeLabel },
    { "menu_button", 84,  221, 70,  16,  1, DashPaintBackButton },
    { "portrait",    0,   26,  63,  42,  1, DashPaintOurPosition },
    { "standings",   0,   67,  64,  148, 1, DashPaintStandingsLive },
    { "ko_stars",    258, 26,  62,  22,  1, DashPaintKOStars },
    { "lap",         258, 52,  60,  28,  1, DashPaintRightColumnLap },
    { "timer",       258, 85,  60,  69,  1, DashPaintRightColumnTimer },
    { "gap",         258, 155, 62,  24,  1, DashPaintRightColumnGap },
    { "speed",       258, 195, 62,  23,  1, DashPaintSpeed },
    { "map_box",     77,  36,  168, 168, 1, DashPaintMapBoxLive },
};

#define DASH_ENERGY_X0 (sDashWidgets[DASHW_ENERGY_BAR].x)
#define DASH_ENERGY_Y0 (sDashWidgets[DASHW_ENERGY_BAR].y)
#define DASH_ENERGY_W  (sDashWidgets[DASHW_ENERGY_BAR].w)
#define DASH_ENERGY_H  (sDashWidgets[DASHW_ENERGY_BAR].h)

#define DASH_HEADER_X0 (sDashWidgets[DASHW_HEADER].x)
#define DASH_HEADER_Y0 (sDashWidgets[DASHW_HEADER].y)
#define DASH_HEADER_W  (sDashWidgets[DASHW_HEADER].w)
#define DASH_HEADER_H  (sDashWidgets[DASHW_HEADER].h)

#define DASH_MODE_X0 (sDashWidgets[DASHW_MODE_CHIP].x)
#define DASH_MODE_Y0 (sDashWidgets[DASHW_MODE_CHIP].y)
#define DASH_MODE_W  (sDashWidgets[DASHW_MODE_CHIP].w)
#define DASH_MODE_H  (sDashWidgets[DASHW_MODE_CHIP].h)

#define DASH_BACK_X0 (sDashWidgets[DASHW_MENU_BUTTON].x)
#define DASH_BACK_Y0 (sDashWidgets[DASHW_MENU_BUTTON].y)
#define DASH_BACK_W  (sDashWidgets[DASHW_MENU_BUTTON].w)
#define DASH_BACK_H  (sDashWidgets[DASHW_MENU_BUTTON].h)

#define DASH_PORTRAIT_X0 (sDashWidgets[DASHW_PORTRAIT].x)
#define DASH_PORTRAIT_Y0 (sDashWidgets[DASHW_PORTRAIT].y)
#define DASH_PORTRAIT_W  (sDashWidgets[DASHW_PORTRAIT].w)
#define DASH_PORTRAIT_H  (sDashWidgets[DASHW_PORTRAIT].h)

#define DASH_STANDINGS_X0 (sDashWidgets[DASHW_STANDINGS].x)
#define DASH_STANDINGS_Y0 (sDashWidgets[DASHW_STANDINGS].y)
#define DASH_STANDINGS_W  (sDashWidgets[DASHW_STANDINGS].w)
#define DASH_STANDINGS_H  (sDashWidgets[DASHW_STANDINGS].h)
#define DASH_STANDINGS_ROW_H (DASH_STANDINGS_H / GDX_RACEHUD_TOP5_MAX) /* 29 */

#define DASH_KO_X0 (sDashWidgets[DASHW_KO_STARS].x)
#define DASH_KO_Y0 (sDashWidgets[DASHW_KO_STARS].y)
#define DASH_KO_W  (sDashWidgets[DASHW_KO_STARS].w)
#define DASH_KO_H  (sDashWidgets[DASHW_KO_STARS].h)

#define DASH_LAP_X0 (sDashWidgets[DASHW_LAP].x)
#define DASH_LAP_Y0 (sDashWidgets[DASHW_LAP].y)

#define DASH_TIMER_X0 (sDashWidgets[DASHW_TIMER].x)
#define DASH_TIMER_Y0 (sDashWidgets[DASHW_TIMER].y)

#define DASH_GAP_X0 (sDashWidgets[DASHW_GAP].x)
#define DASH_GAP_Y0 (sDashWidgets[DASHW_GAP].y)

/* The speedometer (hw bug n60-3d7, item 4): fills the gap below GAP now that BOOST is gone (item
 * 6, hardware feedback round removed the BOOST box outright -- see sDashWidgets' own "ko_stars"
 * row for where its space went instead). No baked backdrop; chrome and value are both repainted
 * live, same style as the MODE/MENU/KO-star chips. Position itself is NOT duplicated here --
 * round 3 item 3 already put the big ordinal numeral on the portrait widget. */
#define DASH_SPEED_X0 (sDashWidgets[DASHW_SPEED].x)
#define DASH_SPEED_Y0 (sDashWidgets[DASHW_SPEED].y)
#define DASH_SPEED_W  (sDashWidgets[DASHW_SPEED].w)
#define DASH_SPEED_H  (sDashWidgets[DASHW_SPEED].h)

/* COORDINATOR FIX (hardware feedback round, item 4): "the minimap could afford to be a bit
 * smaller, uses a lot of prime real estate." Shrunk from the debug MAP tab's own 192x192 (scale
 * 3 on the 64x64 source bitmap) to 168x168 and re-centred in the middle column's free space
 * (x64..258, y24..216) instead of pinned to its old top-left corner, trading the freed ~14-24px
 * margin on every side for breathing room around it rather than growing any one neighbour (no
 * neighbour widget needed more width/height to read correctly; the ask was "give everything else
 * some room", not "make X bigger"). DashPaintMapBoxLive/DashPaintMapMarker below are written as a
 * destination-pixel-driven nearest-neighbour scale (not the old fixed-integer-replication one)
 * so this box's size is just DASH_MAP_PX (square) -- any future custom-HUD resize is a table edit,
 * not a rewrite of the blit loop. */
#define DASH_MAP_X0 (sDashWidgets[DASHW_MAP_BOX].x)
#define DASH_MAP_Y0 (sDashWidgets[DASHW_MAP_BOX].y)
#define DASH_MAP_PX (sDashWidgets[DASHW_MAP_BOX].w) /* square: w == h */
#define DASH_MAP_SRC_DIM 64

/* ---- the fixed per-tick paint ORDER for modes ALL_TOP/SPLIT/ALL_BOTTOM ----------------
 *
 * COORDINATOR FIX (custom-HUD groundwork, follow-up round): "convert tick() to loop the widget
 * table in a defined paint order, re-encoding the ordering/throttle couplings explicitly (keep
 * output identical in modes 1-3)." sDashWidgets' own `paint` column (above) already holds the
 * right function pointer per widget; the only thing tick() used to do BY HAND that a bare loop
 * over that table in DASHW_* storage order would get wrong is (a) the paint CALL order, which is
 * NOT the same as the table's own storage order (storage is grouped by widget "kind" for
 * readability; paint order follows the backdrop's own layer order -- header, then standings,
 * then the map box, etc.), and (b) the MODE dot, which has no row of its own in sDashWidgets at
 * all and must paint immediately after DashPaintModeLabel repaints the chip's interior (see that
 * function's own call-site comment, further down). kDashPaintOrder is that order, written out
 * explicitly instead of left implicit in a hand-written call sequence; HEADER and MENU_BUTTON are
 * deliberately absent (HEADER's own live half is this list's first entry via DASHW_HEADER --
 * wait, HEADER *is* in the list, see below; MENU_BUTTON is not: it is only ever painted once, by
 * DashPaintBaked, at backdrop-bake time, never per-tick -- matching tick()'s pre-refactor
 * behaviour exactly, where DashPaintBackButton was never one of the hand-written per-tick calls
 * either). The throttle/dirty-check couplings themselves (map_box's own [debug] bottom_race_hz
 * timestamp, standings' Top5Changed dirty check) already live INSIDE their respective paint
 * functions (DashPaintMapBoxLive/DashPaintStandingsLive) from the widget-geometry-table round, so
 * looping this list unconditionally every tick is exactly as cheap as the old hand-written
 * sequence was -- each function still decides for itself whether it actually has anything new to
 * draw this tick. */
static const DashWidgetId kDashPaintOrder[] = {
    DASHW_HEADER,     /* course name + "<n> RACERS", dirty-checked internally */
    DASHW_STANDINGS,  /* portraits + rank badges + colour tags, dirty-checked internally */
    DASHW_MAP_BOX,    /* throttled internally to [debug] bottom_race_hz */
    DASHW_ENERGY_BAR, /* every tick: a few hundred pixels, cheaper to redraw than to dirty-track */
    DASHW_PORTRAIT,   /* the big OUR-position ordinal numeral */
    DASHW_MODE_CHIP,  /* repaints the chip's own interior -- the MODE DOT (no table row) must
                        * follow it; see the special-case right after this loop runs, below */
    DASHW_KO_STARS,
    DASHW_LAP,
    DASHW_TIMER,
    DASHW_GAP,
    DASHW_SPEED,
};
#define DASH_PAINT_ORDER_COUNT ((int) (sizeof(kDashPaintOrder) / sizeof(kDashPaintOrder[0])))

/* ---- CUSTOM HUD mode: a 4th placement mode, draggable/resizable/closeable widgets ------
 *
 * GDX_HUD_PLACEMENT_CUSTOM (port/gdx_hud_placement.h) sits one past the 3 fixed modes. Unlike
 * ALL_TOP/SPLIT/ALL_BOTTOM, CUSTOM does not reuse the baked pixel art at all (a baked asset can't
 * stretch to an arbitrary user-chosen size) -- every editable widget draws as a simple flat
 * panel + border + short label + live value instead (DashPaintCustomWidget, further down), at
 * whatever rect sCustomWidgets currently holds for it. sCustomWidgets starts as a plain copy of
 * sDashWidgets' own default rects (DashCustomDefaults) and is then possibly overridden, per
 * widget, by a saved sdmc:/3ds/gdiffuser/hud.ini [custom] section (DashHudIniLoad); RESET and
 * DONE (DashCustomReset / the EDIT<->DONE toggle) are the only two things that ever write that
 * section back out -- never every frame, per the brief.
 *
 * HEADER/MODE_CHIP/MENU_BUTTON are NOT part of the editable set: they are DASH-only chrome (a
 * course-name header, the mode-cycle chip, the "back to debug tabs" button), not race-HUD
 * elements with a top-screen counterpart, so dragging/closing them would not mean anything.
 *
 * SHELVED (nibottom-progress.md §12): the user's own real-hardware pass on CUSTOM found it not
 * worth further investment right now ("the default HUDS are good enough") and reported concrete
 * bugs along the way -- resizing a widget does not clear the framebuffer (stale pixels from the
 * old size/position linger until something else repaints over them) and some widgets simply
 * never display at all. None of that code is deleted: it is gated, off by default, behind the
 * new [debug] hud_custom lever (HudCustomLeverEnabled, right below) so modes 0-2 (ALL_TOP/SPLIT/
 * ALL_BOTTOM -- already proven byte-identical across this whole track) are unaffected and CUSTOM
 * simply becomes unreachable through the normal MODE chip/SELECT cycle or the DBG tab until
 * someone opts back in to keep working on it. */
static DashWidgetRect sCustomWidgets[DASHW_COUNT];

/* SHELF: [debug] hud_custom (default 0). 0 = CUSTOM ("MODE Z") is unreachable: the MODE chip/
 * SELECT cycle only visits the 3 fixed modes, the DBG tab's "HUD EDIT" row is hidden, and a
 * saved hud.ini with a stale [hud] placement=3 from before this lever existed falls back to
 * ALL_BOTTOM at load (gdx3ds_bottomhud_init) instead of resurrecting CUSTOM silently. 1 restores
 * the exact pre-shelving behaviour (4-mode cycle, visible HUD EDIT row) for whoever picks this
 * back up -- no code path below is removed, only gated. */
static int HudCustomLeverEnabled(void) {
    return gdx3ds_config_get_bool("debug", "hud_custom", 0) ? 1 : 0;
}

/* Public wrapper (gdx3ds_bottomhud.h): gdx3ds_menu.c's DBG tab reads this to decide whether to
 * paint/accept taps on the "HUD EDIT" row at all, instead of duplicating the ini read. */
int gdx3ds_bottomhud_custom_enabled(void) {
    return HudCustomLeverEnabled();
}

static int DashWidgetEditable(int id) {
    return id != DASHW_HEADER && id != DASHW_MODE_CHIP && id != DASHW_MENU_BUTTON;
}

/* "Picture widgets like portraits/star icon may resize in fixed steps or not at all." PORTRAIT
 * (the big single OUR-position numeral) is a fixed-size element -- drag-only, never resized;
 * MAP_BOX and KO_STARS (icon-based: the minimap bitmap, the star glyph) resize in coarse 16px
 * steps so their contents stay legible at a handful of sizes rather than an arbitrary one;
 * everything else (plain text/number boxes) resizes freely in 8px steps, same grid the drag
 * snap below uses. */
static int DashWidgetResizable(int id) {
    return id != DASHW_PORTRAIT;
}

static int DashWidgetResizeStep(int id) {
    if (id == DASHW_MAP_BOX || id == DASHW_KO_STARS) {
        return 16;
    }
    return 8;
}

static int DashWidgetMinW(int id) {
    return (id == DASHW_MAP_BOX) ? 48 : 32;
}

static int DashWidgetMinH(int id) {
    return (id == DASHW_MAP_BOX) ? 48 : 16;
}

/* Which GDX_HUD_ELEM_* bit (port/gdx_hud_placement.h) each editable widget stands in for on the TOP
 * screen, in DashWidgetId order; -1 for a widget with no top-screen counterpart at all (the
 * brief's own examples: "GAP/BEST/standings list, just hide" -- BEST lives inside the TIMER
 * widget here, not as its own row, so TIMER maps to GDX_HUD_ELEM_TIME on the strength of its
 * current-lap-time half; its BEST sub-field has no top equivalent either, but the whole widget
 * is one close/open unit, same as every other row in this table). */
static const int kDashWidgetTopElem[DASHW_COUNT] = {
    GDX_HUD_ELEM_ENERGY,  /* DASHW_ENERGY_BAR */
    -1,                    /* DASHW_HEADER      -- fixed chrome, not editable */
    -1,                    /* DASHW_MODE_CHIP   -- fixed chrome, not editable */
    -1,                    /* DASHW_MENU_BUTTON -- fixed chrome, not editable */
    GDX_HUD_ELEM_RANK,     /* DASHW_PORTRAIT    -- the big position ordinal */
    -1,                    /* DASHW_STANDINGS   -- no top-screen counterpart: hide only */
    GDX_HUD_ELEM_KO_STARS, /* DASHW_KO_STARS */
    GDX_HUD_ELEM_LAP,      /* DASHW_LAP */
    GDX_HUD_ELEM_TIME,     /* DASHW_TIMER */
    -1,                    /* DASHW_GAP         -- no top-screen counterpart: hide only */
    GDX_HUD_ELEM_SPEED,    /* DASHW_SPEED */
    GDX_HUD_ELEM_MINIMAP,  /* DASHW_MAP_BOX */
};

/* ---- state -------------------------------------------------------------------------- */
static int sEnabled = 0;       /* [debug] bottom_race */
static int sPlacementMode = GDX_HUD_PLACEMENT_ALL_TOP; /* [hud] placement, RAM cache of the ini */
static int sHz = 10;           /* [debug] bottom_race_hz — map-box throttle */
static int sWasValid = 0;      /* backdrop once-per-race edge */
static int sLastTop5Count = -1;
static int sLastTop5[GDX_RACEHUD_TOP5_MAX];
static u64 sLastMapTick = 0;
/* COORDINATOR FIX (hardware feedback round, item 5): "<n> RACERS" was frozen baked-template text;
 * make it live. Dirty-checked (like top5), not every tick -- it only ever changes when a racer
 * retires/finishes, same cadence gCpuRacersRetired itself changes at. sLastHeaderTrackName is the
 * matching cache for the course-name half of the header (replaces the old sWasValid-edge-only
 * DashPaintCourseName call with a self-contained dirty check, so DashPaintHeaderLive can be one
 * widget-table paint fn instead of two separately-scheduled pieces). */
static int sLastHeaderRacersRemaining = -1;
static char sLastHeaderTrackName[24] = "";
static char sPendingShot[40];
static int sShotArmed = 0;
static int sPlacementSaveDirty = 0; /* [hud] placement changed mid-race; ini write deferred */
static int sPrevChipTouch = 0;      /* edge-detect for the MODE chip's own touch-down */
static int sPrevBackTouch = 0;      /* edge-detect for the DEBUG button's touch-down (nee "MENU") */
static int sDismissed = 0;          /* 1 once the DEBUG button hands the screen back to gdx3ds_menu.c */
/* COORDINATOR FIX (hw bug n60-3d7, item 5): "let the MENU/RACE toggle work from a key (SELECT
 * long-press or similar)" -- the debug menu's RACE tab (the only prior way back once dismissed)
 * could overflow off its 40-column tab bar; a key-based toggle is the fallback that never
 * depends on tab-bar geometry. Ticks (not wall time) so it's frame-rate independent like every
 * other DASH timer here; ~1.5s at 60Hz. Counts while KEY_SELECT is held, independent of the
 * short-tap mode-cycle below (selectDown fires once on press regardless -- a long hold therefore
 * both cycles the MODE chip once AND, after the threshold, dismisses; a minor, harmless overlap
 * rather than two conflicting edge-detectors). */
#define DASH_SELECT_LONGPRESS_TICKS 90
static int sSelectHoldTicks = 0;

/* IDLE VIEW state -- see gdx3ds_bottomhud.h's own "IDLE VIEW" doc comment for the full picture.
 * Deliberately separate from the race DASH's own sDismissed/sSelectHoldTicks/sPrevBackTouch/
 * sWasValid (not reused): the two takeovers are mutually exclusive in time (gdx3ds_menu.c never
 * runs both in the same tick) but conceptually distinct, and keeping their latches apart means
 * neither one's reset logic has to reason about the other's state. */
static int sIdleEnabled = 1;     /* [debug] bottom_idle, read once at init */
static int sIdleDismissed = 0;   /* 1 once the idle view's own DEBUG button hands off to the console */
static int sIdleBaked = 0;       /* idle backdrop painted this "session" (reset on every fresh entry) */
static int sIdlePrevBackTouch = 0;
static int sIdleSelectHoldTicks = 0;

/* CUSTOM HUD mode state. sCustomBackdropPainted mirrors sWasValid's own "once per race" latch,
 * but separately: switching between a baked mode (1-3) and CUSTOM mid-race must repaint the
 * right kind of backdrop for whichever one is now active, not assume the other one already ran.
 * sEditMode is the drag/resize/close/tray UI; sEditSelected is -1 (nothing) or a DashWidgetId.
 *
 * The brief offers two alternative gestures for entering edit mode ("long-press the MODE chip
 * while in CUSTOM, or an EDIT button"); this build uses the EDIT button alone (DashPaintCustomChrome/
 * DashCustomToggleEdit, below), not a chip long-press, specifically so the MODE chip's existing
 * touch-DOWN-fires-immediately short-tap-cycle behaviour (gdx3ds_bottomhud_tick's own MODE-chip
 * block, unchanged from before this round) never has to change for ANY mode, CUSTOM included --
 * a long-press gesture sharing that same rect would need the short tap to fire on RELEASE
 * instead, which is exactly the kind of touch-timing change step 1's "keep output identical in
 * modes 1-3" was written to rule out. */
static int sCustomBackdropPainted = 0;
static int sEditMode = 0;
static int sEditSelected = -1;
static int sEditDragging = 0;
static int sEditResizing = 0;
static int sEditDragOffX = 0;
static int sEditDragOffY = 0;

/* The CUSTOM-mode fixed chrome buttons: RESET and the EDIT<->DONE toggle, both in the header's
 * only remaining free gap (x0..83, left of the DEBUG button at DASH_BACK_X0=84 -- see that
 * macro's own widget-table row). Not part of sDashWidgets/sCustomWidgets: these are meta-UI for
 * the custom-layout editor itself, not race-HUD widgets a layout could ever hide or move. */
#define DASH_RESET_X0 2
#define DASH_RESET_Y0 (DASH_HEADER_Y0 + 3)
#define DASH_RESET_W 38
#define DASH_RESET_H 16
#define DASH_EDIT_X0 42
#define DASH_EDIT_Y0 (DASH_HEADER_Y0 + 3)
#define DASH_EDIT_W 38
#define DASH_EDIT_H 16

/* The hidden-widget tray: one 32x14 chip per currently-disabled editable widget, packed
 * left-to-right starting at x=2, drawn (and only drawn) along this fixed strip while sEditMode
 * is on. Tapping a chip restores that widget (DashCustomRestoreHidden, below) immediately -- no
 * drag needed to bring something back from the tray, unlike everything already on the canvas. */
#define DASH_TRAY_Y0 24
#define DASH_TRAY_CHIP_W 32
#define DASH_TRAY_CHIP_H 14
#define DASH_TRAY_CHIP_GAP 2

/* Self-timing, same convention as gdx3ds_racemap.c's [bmap] receipt (see its own comment on
   why the window closes before the SHOT write): Gate 5 (nibottom Phase 2) asks for a
   main-thread cost delta in ms, and this is the honest number to quote it from — run with
   [debug] shots=0 so a SHOT's ~50ms SD write never gets folded into max_us. */
static unsigned long sTicks = 0;
static u64 sTickTicksTotal = 0;
static u64 sTickTicksMax = 0;

static void DashLog(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int) sizeof(buf) - 1) {
            n = (int) sizeof(buf) - 1;
        }
        svcOutputDebugString(buf, (size_t) n);
        gdx3ds_filelog_write(buf, (size_t) n);
    }
}

/* DEBUG LEVER (coordinator hw bug n60-3d7, item 2: "boost-ready did NOT turn green on hardware,
 * in a real GP race on lap 2+"). Round 5 only ever proved boostReady's read path agrees with
 * hud.c's via the INIT-path grant (practice mode); it never actually exercised the
 * lap-COMPLETION set-site (racer.c's `racer->stateFlags |= RACER_STATE_CAN_BOOST` at the
 * finish-line wrap check), because no replay in the corpus ever reached lap>=2. [debug]
 * force_lap2=1 plus racer.c's matching lever (see its own comment) fast-forwards a GP race
 * through lap 1 so that exact code path can be exercised in seconds. Investigation with this
 * lever (plus temporary edge-triggered logging, since reverted) found hud.c's read and DASH's
 * read agreeing at the exact same raceTimeMs through the real lap-completion path too, and the
 * bottom-screen energy bar visibly switching to the green family at that moment -- no bug found
 * in the read/paint logic. Kept as a permanent lever (default off) so the real-hardware boost
 * state can be reached and re-verified in seconds without driving a full GP lap by hand; see
 * docs/research/native/nibottom-progress.md for the full writeup. */
int gdx_debug_force_lap2_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = gdx3ds_config_get_bool("debug", "force_lap2", 0) ? 1 : 0;
    }
    return cached;
}

/* ---- init / levers -------------------------------------------------------------------- */

/* COORDINATOR FIX (hw bug n60-3d7, item 7): "when the HUD mode was saved, the game REWROTE the
 * user's gdiffuser.ini. Comments were dropped, the [variant.T/P/S/V] section names were
 * lowercased... keys moved, and a [hud] section was added. Never rewrite the user's ini
 * wholesale. Persist the HUD mode in its own small file."
 *
 * Root cause: gdx3ds_config_save (gdx3ds_config.c) is the WHOLE config table's writer -- by
 * design, not a bug specific to this lever: it self-documents "rewritten by the in-game menu
 * (comments are not preserved)" and is the same function gdx3ds_menu.c's MenuSaveCfg uses for
 * deliberate settings edits from the paused debug menu, where a full rewrite is an accepted
 * trade-off. The HUD placement lever is different: gdx3ds_bottomhud_cycle_mode can fire from a
 * SELECT press or a chip tap mid-race (see its own comment below), so every tap was rewriting
 * the user's entire ini just to persist one int. Fix: route ONLY this one key through its own
 * tiny dedicated file instead, never touching gdiffuser.ini at all.
 *
 * Variant-loader note (the coordinator also asked to "confirm the variant loader still works
 * with lowercased names, or document that it doesn't"): it does. gdx3ds_config.c's loader
 * lowercases every section AND key name on parse (gdx3ds_cfg_copy_lower, ~line 119/140) --
 * this happens for ALL entries, unconditionally, even from a pristine original-cased file --
 * and every lookup (gdx3ds_config_get_*, gdx3ds_config_section_entry) lowercases its own query
 * the same way before comparing. gdx3ds_launch_core.c's VariantSection builds "variant.%s" from
 * the launch-held button's name as-is (e.g. "variant.T") and hands it straight to
 * gdx3ds_config_section_entry, which lowercases it to "variant.t" before matching -- the exact
 * same normalisation the stored entries already went through on load, whatever case the file on
 * disk used. So a prior accidental lowercase rewrite of [variant.T] -> [variant.t] does NOT
 * break variant selection; the whole config module is case-insensitive end to end by
 * construction, not only for this key. */
#define GDX3DS_HUD_MODE_PATH "sdmc:/3ds/gdiffuser/hud.ini"

/* Fills every row of `out` with sDashWidgets' own hard-coded rects (the exact ALL_BOTTOM
 * geometry), enabled=1 -- CUSTOM mode's "everything on the bottom" starting point, and what
 * RESET always restores. */
static void DashCustomDefaults(DashWidgetRect* out) {
    int i;
    for (i = 0; i < DASHW_COUNT; i++) {
        out[i] = sDashWidgets[i];
        out[i].enabled = 1;
    }
}

/* Tells the real top-screen gating (port/gdx_hud_placement.c's gdx_hud_set_custom_top_mask, as
 * of the native/nihudtop merge) which bottom-screen elements are currently hidden on the bottom
 * and so belong on top instead, and whether CUSTOM mode is the active placement mode at all.
 * Called on every CUSTOM layout/mode change (RESET, DONE, and the mode-cycle transition into/out
 * of CUSTOM), never every frame. */
static void DashSyncCustomTopMask(void) {
    unsigned mask = 0;
    int i;
    for (i = 0; i < DASHW_COUNT; i++) {
        if (kDashWidgetTopElem[i] < 0) {
            continue;
        }
        if (!sCustomWidgets[i].enabled) {
            mask |= (1u << (unsigned) kDashWidgetTopElem[i]);
        }
    }
    gdx_hud_set_custom_top_mask(mask, sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM);
}

/* Single-purpose writer for the WHOLE small hud.ini file -- both the [hud] placement int (hw
 * bug n60-3d7, item 7's own fix: never gdiffuser.ini) and, now, the [custom] section (one line
 * per editable widget: "id = x y w h visible"). Both sections are written together so neither
 * save path can ever silently drop the other one's last-known state. Called from the placement
 * lever's existing safe points (race-end / menu-close, unchanged) AND from DashCustomReset/the
 * EDIT->DONE transition -- per the brief, the custom section is written ONLY on DONE or RESET,
 * never every frame. */
static void DashHudIniSave(void) {
    FILE* fp = fopen(GDX3DS_HUD_MODE_PATH, "w");
    int i;
    if (fp == NULL) {
        DashLog("[dash] WARNING: hud.ini save failed (%s)", GDX3DS_HUD_MODE_PATH);
        return;
    }
    fprintf(fp, "; sdmc:/3ds/gdiffuser/hud.ini -- just the DASH placement lever and the CUSTOM\n");
    fprintf(fp, "; layout (hw bug n60-3d7, item 7): this file is rewritten whole every time\n");
    fprintf(fp, "; either one changes, but it never touches the main gdiffuser.ini, so nothing\n");
    fprintf(fp, "; else is ever at risk.\n");
    fprintf(fp, "[hud]\n");
    fprintf(fp, "placement = %d\n", sPlacementMode);
    fprintf(fp, "[custom]\n");
    for (i = 0; i < DASHW_COUNT; i++) {
        char line[64];
        GdxHudCustomRect rc;
        if (!DashWidgetEditable(i)) {
            continue;
        }
        /* An explicit field-by-field copy, NOT a pointer cast: DashWidgetRect's own layout
         * leads with `id` (a pointer), so x/y/w/h/enabled are NOT at the same offsets
         * gdx_hudcustom_layout.h's GdxHudCustomRect expects -- see that header's own comment on
         * the field-order assumption this copy satisfies honestly instead. */
        rc.x = sCustomWidgets[i].x;
        rc.y = sCustomWidgets[i].y;
        rc.w = sCustomWidgets[i].w;
        rc.h = sCustomWidgets[i].h;
        rc.enabled = sCustomWidgets[i].enabled;
        if (gdx_hudcustom_format_line(line, (int) sizeof(line), &rc) < 0) {
            continue; /* defensive only: DASHW_COUNT rects always fit comfortably in `line` */
        }
        fprintf(fp, "%s = %s\n", sCustomWidgets[i].id, line);
    }
    fclose(fp);
}

/* Trims leading/trailing whitespace from `s` in place, same spirit as the pre-existing
 * placement-only parser this replaces. Returns s for chaining. */
static char* DashTrim(char* s) {
    char* end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

/* Single-purpose reader for the whole file: tracks the current `[section]` and, inside it,
 * parses one "key = value" pair per line. [hud]'s only key is "placement"; [custom]'s keys are
 * the DASHW_* id strings (sDashWidgets[i].id), each matched against gdx_hudcustom_parse_line's
 * robust "x y w h v" parser. A garbage/short [custom] line, or a line for an id this build
 * doesn't recognise, leaves that widget's entry exactly as the caller already defaulted it
 * (DashCustomDefaults) -- the whole point of routing every field through
 * gdx_hudcustom_parse_line's own validation instead of a bare atoi(). Independent of the shared
 * gdx3ds_config_* table for the same reason the old placement-only reader was (see that
 * function's prior comment, now folded into this one): that table is a process-wide singleton
 * and cannot be safely reloaded here. */
static void DashHudIniLoad(int* outMode, DashWidgetRect* customInOut) {
    FILE* fp = fopen(GDX3DS_HUD_MODE_PATH, "r");
    char line[128];
    char section[16] = "";
    if (fp == NULL) {
        return; /* no file yet: first boot after this fix, or a fresh SD card */
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char* trimmed = DashTrim(line);
        char* eq;
        char* key;
        char* value;
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed[0] == '[') {
            char* close = strchr(trimmed, ']');
            size_t n;
            if (close == NULL) {
                continue;
            }
            n = (size_t) (close - (trimmed + 1));
            if (n >= sizeof(section)) {
                n = sizeof(section) - 1;
            }
            memcpy(section, trimmed + 1, n);
            section[n] = '\0';
            continue;
        }
        eq = strchr(trimmed, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = DashTrim(trimmed);
        value = DashTrim(eq + 1);
        if (strcmp(section, "hud") == 0) {
            if (strcmp(key, "placement") == 0) {
                *outMode = atoi(value);
            }
            continue;
        }
        if (strcmp(section, "custom") == 0 && customInOut != NULL) {
            int i;
            for (i = 0; i < DASHW_COUNT; i++) {
                GdxHudCustomRect parsed;
                if (!DashWidgetEditable(i) || strcmp(key, customInOut[i].id) != 0) {
                    continue;
                }
                if (gdx_hudcustom_parse_line(value, DashWidgetMinW(i), DashWidgetMinH(i), DASH_HEADER_Y0, &parsed)) {
                    customInOut[i].x = parsed.x;
                    customInOut[i].y = parsed.y;
                    customInOut[i].w = parsed.w;
                    customInOut[i].h = parsed.h;
                    customInOut[i].enabled = parsed.enabled;
                }
                break;
            }
            continue;
        }
    }
    fclose(fp);
}

void gdx3ds_bottomhud_init(void) {
    int loadedMode = -1;
    /* DEFAULT ON (n60lead): the DASH is now the accepted default bottom-screen HUD. An
     * explicit `bottom_race = 0` in the ini still wins -- gdx3ds_config_get_bool only ever
     * hands back the fallback (1, here) when the key is absent or unparsable. */
    sEnabled = gdx3ds_config_get_bool("debug", "bottom_race", 1) ? 1 : 0;
    /* IDLE VIEW (default ON, independent lever from bottom_race -- see gdx3ds_bottomhud.h's own
     * "IDLE VIEW" doc comment): [debug] bottom_idle=0 turns off just the idle-view polish and
     * falls back to the text-console debug UI outside races, same as it always was. */
    sIdleEnabled = gdx3ds_config_get_bool("debug", "bottom_idle", 1) ? 1 : 0;
    sIdleDismissed = 0;
    sIdleBaked = 0;
    sIdlePrevBackTouch = 0;
    sIdleSelectHoldTicks = 0;
    DashCustomDefaults(sCustomWidgets);
    /* hud.ini (if present) is the canonical source from here on; [hud] placement in the main
     * gdiffuser.ini is only a one-time migration fallback for a save made before this fix (or a
     * hand-edit) -- see the fix note above GDX3DS_HUD_MODE_PATH. DashHudIniLoad also fills in
     * any saved [custom] rows over the just-set defaults above. */
    DashHudIniLoad(&loadedMode, sCustomWidgets);
    if (loadedMode >= 0) {
        sPlacementMode = loadedMode;
    } else {
        sPlacementMode = gdx3ds_config_get_int("hud", "placement", GDX_HUD_PLACEMENT_ALL_TOP);
    }
    if (sPlacementMode < 0 || sPlacementMode > GDX_HUD_PLACEMENT_CUSTOM) {
        sPlacementMode = GDX_HUD_PLACEMENT_ALL_TOP;
    }
    /* SHELF: a hud.ini written before CUSTOM was shelved (or hand-edited) can still say
     * placement=3 with the lever off today -- fall back to ALL_BOTTOM rather than silently
     * resurrecting CUSTOM behind the user's back. */
    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM && !HudCustomLeverEnabled()) {
        DashLog("[dash] hud_custom=0: saved placement=CUSTOM unavailable, falling back to ALL_BOTTOM");
        sPlacementMode = GDX_HUD_PLACEMENT_ALL_BOTTOM;
    }
    sHz = gdx3ds_config_get_int("debug", "bottom_race_hz", 10);
    if (sHz < 1) {
        sHz = 1;
    } else if (sHz > 60) {
        sHz = 60;
    }
    sWasValid = 0;
    sCustomBackdropPainted = 0;
    sEditMode = 0;
    sEditSelected = -1;
    sLastTop5Count = -1;
    sLastHeaderRacersRemaining = -1;
    sLastHeaderTrackName[0] = '\0';
    /* VERIFICATION AID, not a user-facing feature: [debug] hud_edit_start=1 forces CUSTOM mode
     * with edit mode already active from the very first tick, so an emulator/hardware capture
     * can show the edit overlay (selection, close/resize handles, the hidden-widget tray)
     * without first driving a scripted touch sequence to reach it -- the brief's own named
     * fallback ("a debug ini lever that pre-loads a moved/hidden layout") for exactly this case.
     * Combine with a hand-staged hud.ini [custom] section (one widget moved, one hidden) to also
     * demonstrate a non-default saved layout rendering correctly. */
    if (gdx3ds_config_get_bool("debug", "hud_edit_start", 0)) {
        if (HudCustomLeverEnabled()) {
            sPlacementMode = GDX_HUD_PLACEMENT_CUSTOM;
            sEditMode = 1;
            DashLog("[dash] debug hud_edit_start=1: forcing CUSTOM+edit at boot");
        } else {
            DashLog("[dash] debug hud_edit_start=1 ignored: [debug] hud_custom=0 (CUSTOM shelved)");
        }
    }
    /* VERIFICATION AID, not a user-facing feature, same idea as hud_edit_start above: [debug]
     * idle_dismiss_start=1 forces the idle view's own "dismissed" state from the very first tick,
     * so a capture can show the console UI with its "DASH" return tab (TAB_RACE, repainted per
     * MenuTabRaceLabel) without needing a touch sequence to reach it -- there is no scriptable
     * touch/key-hold path into this harness (the autoinput/.gdxrec replay system drives the N64
     * virtual pad, not real 3DS HID, which is what gdx3ds_menu.c's touch/SELECT handling reads),
     * same reason hud_edit_start exists. */
    if (gdx3ds_config_get_bool("debug", "idle_dismiss_start", 0)) {
        sIdleDismissed = 1;
        DashLog("[dash] debug idle_dismiss_start=1: forcing idle-dismissed (console+DASH tab) at boot");
    }
    DashSyncCustomTopMask();
    /* hud_custom/modes is a direct, greppable receipt for the SHELF gate (nibottom-progress.md
     * §12): modes=3 with no "CUSTOM" in the cycle whenever hud_custom=0 (the default), modes=4
     * when someone has explicitly opted back in. */
    DashLog("[dash] init enabled=%d idle=%d placement=%s hz=%d hud_custom=%d modes=%d", sEnabled, sIdleEnabled,
            sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM ? "CUSTOM" : gdx_hud_placement_name(sPlacementMode), sHz,
            HudCustomLeverEnabled(),
            HudCustomLeverEnabled() ? (GDX_HUD_PLACEMENT_CUSTOM + 1) : GDX_HUD_PLACEMENT_MODE_COUNT);
}

int gdx3ds_bottomhud_enabled(void) {
    return sEnabled;
}

int gdx3ds_bottomhud_race_live(void) {
    GdxRaceHudSnapshot snap;
    if (!sEnabled) {
        return 0;
    }
    if (gdx_title_demo_active()) {
        return 0; /* attract demo: the idle view keeps the screen, not the race DASH */
    }
    gdx_racehud_snapshot(&snap);
    return snap.valid;
}

/* This is the function port/gdx_hud_placement.c's gdx_hud_placement_mode() actually calls
 * (gdx3ds_bottomhud.h's own comment on this function says so). GDX_HUD_PLACEMENT_CUSTOM is not
 * one of the 3 values gdx_hud_placement_mode()'s callers originally knew about, so this reports
 * ALL_BOTTOM whenever the real mode is CUSTOM -- a safety fallback now, not the primary CUSTOM
 * path: gdx_hud_top_element_mask() (port/gdx_hud_placement.c) checks the real
 * gdx_hud_set_custom_top_mask() override BEFORE it ever calls gdx_hud_placement_mode() at all
 * (DashSyncCustomTopMask below keeps that override's mask/customActive flag in sync on every
 * CUSTOM layout/mode change), so in the normal 1P-CUSTOM case this function's ALL_BOTTOM answer
 * is never even consulted. It only matters as the effective fallback for numPlayers>1, where the
 * override is deliberately ignored and FORCED ALL_TOP IN 2-4P already overrides any mode number
 * regardless, so ALL_BOTTOM-vs-anything-else makes no difference there either.
 * gdx3ds_bottomhud_tick/_cycle_mode/_placement_mode's own internal logic (the MODE chip's label,
 * CUSTOM's own paint/edit path) all read sPlacementMode directly instead, so they still see the
 * real value. */
int gdx3ds_bottomhud_placement_mode(void) {
    if (gdx_title_demo_active()) {
        return GDX_HUD_PLACEMENT_ALL_TOP; /* no DASH during a demo, so nothing may leave the top screen */
    }
    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM) {
        return GDX_HUD_PLACEMENT_ALL_BOTTOM;
    }
    return sPlacementMode;
}

/* ADVISOR (nibottom Phase 2 check-in): an SD write is fine from the MENU's other setters (always
 * a deliberate, paused-ish user edit), but this one can fire from a KEY_SELECT press or a chip
 * tap WHILE A RACE IS RUNNING, and a stall there is a dropped frame on Old 3DS.
 * gdx3ds_config_set_int (the in-memory table update that makes the mode take effect immediately,
 * and keeps the shared table's own "[hud] placement" mirror consistent for anything else that
 * might read it this session) stays unconditional; only the actual file write is deferred, to
 * the next safe point: the race's own valid 1->0 transition (gdx3ds_bottomhud_tick, below) or a
 * non-race "menu close" (gdx3ds_bottomhud_flush_pending_save, called from gdx3ds_menu.c's
 * MenuEnterOff). DashHudModeSave, not gdx3ds_config_save -- see that function's own fix note
 * (hw bug n60-3d7, item 7): this lever gets its own small file, never gdiffuser.ini. */
void gdx3ds_bottomhud_cycle_mode(void) {
    const int inRace = (&gdx_input_in_gameplay != NULL) && gdx_input_in_gameplay();
    /* SHELF: with [debug] hud_custom=0 (default) the chip/SELECT only cycles the 3 fixed modes
     * (ALL_TOP -> SPLIT -> ALL_BOTTOM -> ALL_TOP), exactly as before CUSTOM existed. hud_custom=1
     * restores the +1-beyond-CUSTOM cycle (ALL_TOP -> SPLIT -> ALL_BOTTOM -> CUSTOM -> ALL_TOP). */
    const int modeCount = HudCustomLeverEnabled() ? (GDX_HUD_PLACEMENT_CUSTOM + 1) : GDX_HUD_PLACEMENT_MODE_COUNT;
    sPlacementMode = (sPlacementMode + 1) % modeCount;
    sCustomBackdropPainted = 0; /* repaint whichever kind of backdrop this new mode needs */
    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        sEditMode = 0; /* leaving CUSTOM always leaves edit mode too -- see DONE's own comment */
    }
    DashSyncCustomTopMask();
    /* RAM-only; safe mid-race -- and, since hw bug n60-3d8 item 2, safe for gdiffuser.ini too:
     * this DOES mark "hud"/"placement" dirty in the shared gdx3ds_config table (that mirror is
     * the whole point -- see the comment above), and a LATER, unrelated MenuSaveCfg() in the
     * same session will see it dirty, but gdx3ds_config_save() now refuses to ever let a [hud]
     * key reach gdiffuser.ini, dirty or not, pre-existing or new. That save-side filter is the
     * actual fix; this call did not change. NOTE: gdx3ds_config_set_int clamps/accepts any int,
     * including CUSTOM's 3 -- nothing else in the shared table interprets this key, so there is
     * no cross-module risk in it momentarily holding a value port/input_bridge.c's own
     * GDX_HUD_PLACEMENT_* enum doesn't define; gdx3ds_bottomhud_placement_mode() is the only
     * thing that value is read back through on this build, and it never forwards a raw 3 (see
     * its own comment). */
    gdx3ds_config_set_int("hud", "placement", sPlacementMode);
    if (inRace) {
        sPlacementSaveDirty = 1;
    } else {
        DashHudIniSave();
    }
    DashLog("[dash] mode -> %s%s",
            sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM ? "CUSTOM" : gdx_hud_placement_name(sPlacementMode),
            inRace ? " (save deferred)" : "");
}

void gdx3ds_bottomhud_flush_pending_save(void) {
    if (!sPlacementSaveDirty) {
        return;
    }
    DashHudIniSave();
    sPlacementSaveDirty = 0;
    DashLog("[dash] mode save flushed");
}

void gdx3ds_bottomhud_note_shot(const char* label) {
    if (!sEnabled || label == NULL || label[0] == '\0') {
        return;
    }
    snprintf(sPendingShot, sizeof(sPendingShot), "%s", label);
    __atomic_store_n(&sShotArmed, 1, __ATOMIC_RELEASE);
}

/* COORDINATOR FIX (hardware bug report n60-3d7, item 1): "scattered glitchy black lines on some
 * widgets (energy, position)." Root cause: the bottom screen is single-buffered (consoleInit's
 * GFX_BOTTOM console disables double buffering internally so a plain console can write straight
 * into the displayed buffer) and gdx3ds_bottomhud_tick painted widgets directly into that live,
 * on-screen buffer with many separate DashFillRect/DashPixel/DashBlit* calls spread over the
 * tick's whole duration (the "REPAINT-RATE DISCIPLINE" dirty-rect scheme above) -- exactly the
 * "clear-then-draw into the front buffer" pattern the bug report names: the LCD's scanout can
 * (and on real hardware, demonstrably does) catch the screen mid-paint, between a widget's
 * clear and its redraw, and show the cleared (black) rect for one or more frames.
 *
 * Fix: every Dash* paint call below now targets this off-screen shadow buffer instead of the
 * live framebuffer; gdx3ds_bottomhud_tick does ONE memcpy of the complete shadow buffer into
 * the live buffer at the very end of each tick that painted anything. The live buffer therefore
 * only ever holds a FULLY complete frame (last tick's, or this tick's) -- never a half-drawn
 * widget -- and the window in which the scanout could possibly catch an inconsistent image
 * shrinks from "the whole tick's worth of scattered draw calls" down to the duration of one
 * contiguous 150 KiB memcpy (sub-millisecond on this hardware), which can only ever expose two
 * adjacent rows of two DIFFERENT complete frames, never a cleared one. Deliberately NOT
 * gfxSetDoubleBuffering(GFX_BOTTOM, true) + gfxSwapBuffers(): see gdx3ds_os_ctru.c's own
 * window_swap commentary for why a second swap path on top of the existing
 * C3D_FrameBegin(SYNCDRAW) top-screen pacing has already caused a real fps regression and a
 * black-screen regression on this exact hardware target; this fix stays entirely inside DASH's
 * own (cold, non-NRR-hot) code and touches no gfx/GSP swap semantics at all. */
static uint16_t sShadowFb[DASH_FB_W * DASH_FB_H];

/* ---- pixel helpers (same portrait/column-major convention as gdx3ds_racemap.c) -------- */

static inline void DashPixel(uint16_t* fb, int x, int y, uint16_t colour) {
    if (x < 0 || x >= DASH_FB_W || y < 0 || y >= DASH_FB_H) {
        return;
    }
    fb[(size_t) x * DASH_FB_H + (DASH_FB_H - 1 - y)] = colour;
}

static void DashFillRect(uint16_t* fb, int x0, int y0, int w, int h, uint16_t colour) {
    int x;
    for (x = x0; x < x0 + w; x++) {
        int y;
        for (y = y0; y < y0 + h; y++) {
            DashPixel(fb, x, y, colour);
        }
    }
}

static void DashStrokeRect(uint16_t* fb, int x0, int y0, int w, int h, uint16_t colour) {
    int x;
    int y;
    for (x = x0; x < x0 + w; x++) {
        DashPixel(fb, x, y0, colour);
        DashPixel(fb, x, y0 + h - 1, colour);
    }
    for (y = y0; y < y0 + h; y++) {
        DashPixel(fb, x0, y, colour);
        DashPixel(fb, x0 + w - 1, y, colour);
    }
}

/* RGBA5551 (r5 g5 b5 a1, MSB-first) -> RGB565, same conversion gdx3ds_racemap.c's marker
 * palette uses. a=0 is transparent (the caller skips the write, not this function). */
static inline uint16_t Rgba5551To565(unsigned short v) {
    const unsigned r = (v >> 11) & 31u;
    const unsigned g = (v >> 6) & 31u;
    const unsigned b = (v >> 1) & 31u;
    return (uint16_t) ((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

/* Darkens + lightly desaturates a baked RGB565 background texel in place, toward its own luma.
 * COORDINATOR FIX (second visual-feedback round, item 3): "the background is too bright... tone
 * down the pink checker: darker or more desaturated, lower contrast... keep it pink, but make it
 * a backdrop, not the focus." ~35% darker, blended 20% toward grey -- enough to read as a muted
 * backdrop instead of a foreground pattern, without losing the pink hue entirely. */
static inline uint16_t DashDimBackground(uint16_t c) {
    unsigned r = ((c >> 11) & 31u) * 255u / 31u;
    unsigned g = ((c >> 5) & 63u) * 255u / 63u;
    unsigned b = (c & 31u) * 255u / 31u;
    const unsigned luma = (r * 30u + g * 59u + b * 11u) / 100u;
    r = (r * 65u) / 100u;
    g = (g * 65u) / 100u;
    b = (b * 65u) / 100u;
    r = (r * 80u + luma * 20u) / 100u;
    g = (g * 80u + luma * 20u) / 100u;
    b = (b * 80u + luma * 20u) / 100u;
    return (uint16_t) (((r * 31u / 255u) << 11) | ((g * 63u / 255u) << 5) | (b * 31u / 255u));
}

/* COORDINATOR FIX (third visual-feedback round, item 1): "the hand-drawn 3x5 font breaks an
 * explicit user requirement... Replace it with the real glyph atlases, baked into the C headers
 * from the o2r... Scale them to size with integer nearest-neighbour." gdx3ds_bottomhud_glyphs.h
 * (tools/gen_nibottom_bottomhud_glyphs.py) is that bake: real RGBA5551 pixels sliced straight
 * from fzerox.o2r's own aTimerSymbolsTex/aLapCounterSymbolsTex/aPosition<N>Tex/
 * aPositionOrdinalSuffix<X>Tex/aFont1<X>Tex/aFont2<X>Tex. This file's own hand font is gone --
 * every blit below goes through DashBlitGlyphRatio (a single nearest-neighbour scaler, num/den
 * both >= 1, so it covers upscaling a tiny icon-sized texture AND downscaling a big one, e.g.
 * the 28x32 position digits at num=1,den=2 to fit two of them in the 64px-wide OUR-position box)
 * or the DashBlitGlyphTable* helpers that walk a whole string against one of the tables above. */

static void DashBlitGlyphRatio(uint16_t* fb, int x, int y, const DashGlyph* g, int num, int den) {
    int dy;
    const int dw = (g->w * num) / den;
    const int dh = (g->h * num) / den;
    for (dy = 0; dy < dh; dy++) {
        const int sy = (dy * den) / num;
        int dx;
        for (dx = 0; dx < dw; dx++) {
            const int sx = (dx * den) / num;
            const unsigned short p = g->pixels[sy * g->w + sx];
            if ((p & 1u) == 0u) {
                continue; /* alpha=0 */
            }
            DashPixel(fb, x + dx, y + dy, Rgba5551To565(p));
        }
    }
}

static const DashGlyph* DashFindGlyph(const DashGlyph* table, int count, const char* key) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(table[i].key, key) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* Width of one glyph (its real, proportional `adv`, not its raw texture width) at num/den. */
static int DashGlyphAdvanceRatio(const DashGlyph* g, int num, int den) {
    return (g->adv * num) / den;
}

/* Font1/Font2 are the only tables with multi-glyph TEXT walked via DashFindGlyph per character
 * (one-char keys, plus a no-glyph space advance); the other tables (timer/lap/position digits,
 * suffixes) are always looked up a single glyph at a time by their own call sites below, since
 * each one is a distinct symbol/digit/suffix composited at its own hand-placed position, not a
 * run of prose. `space` is DASH_FONT1_SPACE or DASH_FONT2_SPACE, `gap` a small extra kerning
 * pixel between glyphs (at num/den, like everything else here) so characters do not touch. */
static int DashFontTextWidthRatio(const DashGlyph* table, int count, const char* s, int num, int den,
                                   int space, int gap) {
    int w = 0;
    int n = 0;
    for (; *s != '\0'; s++) {
        if (*s == ' ') {
            w += (space * num) / den;
        } else {
            char key[2] = {*s, '\0'};
            const DashGlyph* g = DashFindGlyph(table, count, key);
            if (g != NULL) {
                w += DashGlyphAdvanceRatio(g, num, den);
            }
        }
        if (n > 0) {
            w += (gap * num) / den;
        }
        n++;
    }
    return w;
}

static void DashBlitFontText(uint16_t* fb, int x, int y, const DashGlyph* table, int count, const char* s,
                              int num, int den, int space, int gap) {
    int cx = x;
    int n = 0;
    for (; *s != '\0'; s++) {
        if (n > 0) {
            cx += (gap * num) / den;
        }
        if (*s == ' ') {
            cx += (space * num) / den;
        } else {
            char key[2] = {*s, '\0'};
            const DashGlyph* g = DashFindGlyph(table, count, key);
            if (g != NULL) {
                DashBlitGlyphRatio(fb, cx, y, g, num, den);
                cx += DashGlyphAdvanceRatio(g, num, den);
            }
        }
        n++;
    }
}

/* Largest num in {2,1} (den fixed at 1) whose rendered Font2 string fits maxW -- the MODE chip
 * and the right column's boxes are narrow and their content length varies tick to tick (a
 * >=1-minute clock value runs longer; GAP can show a sign). */
static int DashFitFont2Num(const char* s, int maxW, int gap) {
    int num;
    for (num = 2; num >= 1; num--) {
        if (DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, s, num, 1, DASH_FONT2_SPACE, gap) <= maxW) {
            return num;
        }
    }
    return 1;
}

/* The baked backdrop+chrome composite (gdx3ds_bottomhud_assets.h): the opaque background
 * first, then every other widget alpha-composited over it, in the generator's (== manifest's
 * own) order. Drawn ONCE PER RACE — see sWasValid's 0->1 edge in gdx3ds_bottomhud_tick.
 *
 * COORDINATOR FIX (nibottom Phase 2, post-mockup-shot review): "portrait" and "mode_button"
 * are baked FLATTENED PNGs of the template's own frozen example content -- "4TH"/"04th" baked
 * into the portrait ribbon, "MODE 1" baked across the whole mode_button box (see
 * tools/gen_nibottom_bottomhud_assets.py's own doc comment: a flattened export, not layered).
 * Both are also fields this file draws LIVE every tick (the player's real position, the real
 * placement mode name) immediately on top, so baking them too meant drawing the live value
 * literally on top of a different, frozen value in the same cells -- the "stray '04'/garbled
 * text over the our-position block" and "MODE chip over ALL TOP" the coordinator flagged from
 * nibottom-shots/mutecity-start_m01_3297_bottom.png. Skipping them here (the checker background
 * still shows through, same as any other unbaked area) and drawing only the live text is the
 * fix -- not a width tweak, since the baked content fills each box edge to edge. LAP/TIME/BEST/
 * GAP/BOOST keep their bake (label + frozen example number, e.g. "TIME 0'42"18""): the matching
 * fix there was the opposite one at the time -- stop drawing live text over them, since "per
 * frame" live digits was deferred past that pass in favour of a single consistent game-font
 * look. (Later rounds then DID wire up live digits -- DashPaintRightColumnLap/Timer/Gap, below --
 * and BOOST itself is gone outright as of the hardware feedback round; see DashPaintKOStars.) */
/* Per-cell skip test for the widgets that carry a LIVE field in among otherwise-static baked
 * content, so DashPaintBaked leaves those cells alone (checker shows through) and a live paint
 * draws the real value there every tick / once per race. Row ranges for lap/timer/gap found by
 * scanning kDashAsset_lap/timer/gap directly for their actual label-vs-value content bands
 * (label always starts at row 0).
 *
 * COORDINATOR FIX (third visual-feedback round, item 2): "the header says 'MUTE CITY !' on the
 * gp2-sector replay. It should be SECTOR... read the actual course, not a constant." The baked
 * "header" widget's course-name text occupies cols ~6-76 of its 320px width (confirmed by direct
 * decode, same scan that found the free MENU-button gap in the second round) -- col < 150 skips
 * that whole zone with a comfortable margin short of the MODE chip's own already-unbaked rect
 * (168+). DashPaintHeaderLive (below) draws the real name there with real Font1 glyphs.
 *
 * COORDINATOR FIX (hardware feedback round, item 5): "'30 RACERS' [is] currently baked/frozen
 * text; make it live." Direct decode of kDashAsset_header found its "<N> RACERS" glyph pixels at
 * cols 255-313 on a flat cols-230-319 panel (nothing else lives right of the MODE chip's own 230
 * right edge) -- col >= 230 skips that whole panel, DashPaintHeaderLive's racer-count half draws
 * the live "<N> RACERS" there.
 *
 * COORDINATOR FIX (hardware feedback round, item 1): "the energy bar... has 'overflow' cut off
 * on top." Root cause: kDashAsset_energy_bar is a FLATTENED EXAMPLE export (same "frozen PNG of
 * whatever was on screen" issue "portrait"/"mode_button" already had) -- its baked pixels past
 * the "ENERGY" label (cols 7-53) are a frozen fill-bar screenshot, not a reusable empty frame, and
 * DashPaintEnergyFooter's own live fill only ever repaints rows 4..19 of the 24-tall box, leaving
 * a few rows of that frozen fill peeking out above/below it every tick -- the "overflow". col >=
 * 54 skips the whole frame/fill zone (keeping just the "ENERGY" label baked); DashPaintEnergyFrame
 * (below) draws a flat procedural panel + border there instead, same DASH-chrome style as the
 * MODE/MENU/KO-star/speed chips, with no frozen content left to bleed through.
 *
 * COORDINATOR FIX (hardware feedback round, item 2): "standings portraits... don't update based
 * on the racers/position. They're static." The standings widget's own baked art only fills cols
 * 0..28 of its 64px width (portrait + its own tiny baked ordinal ribbon, confirmed by direct
 * decode) -- col <= 28 skips that whole zone (all 5 rows at once: they share the same column
 * range), DashPaintStandingsPortraits (below) draws the real, live, per-row character portrait. */
static int DashBakeSkipCell(const char* id, int row, int col) {
    if (strcmp(id, "lap") == 0) {
        return row >= 12;
    }
    if (strcmp(id, "timer") == 0) {
        return (row >= 18 && row < 38) || row >= 52; /* TIME value, then BEST value */
    }
    if (strcmp(id, "gap") == 0) {
        return row >= 12;
    }
    if (strcmp(id, "header") == 0) {
        return col < 150 || col >= 230;
    }
    if (strcmp(id, "energy_bar") == 0) {
        return col >= 54;
    }
    if (strcmp(id, "standings") == 0) {
        return col <= 28;
    }
    return 0;
}

/* COORDINATOR FIX (second round item 1 / third round item 1): "the rank text in the top-5
 * column... is too small" -> "replace [the hand font] with the real glyph atlases... the
 * position digits and ST/ND/RD/TH textures for the ranks." The standings widget's own baked art
 * only fills cols 0..28 of its 64px width (portrait + its own tiny baked ordinal ribbon,
 * confirmed by direct decode) -- cols 29..63 are blank in every row band, so the real
 * aPosition<N>Tex digit + aPositionOrdinalSuffix<X>Tex suffix go there (num=1,den=2: the 28x32 /
 * 20x20 native textures halved, 14x16 + 10x20 side by side, comfortably inside the 34x29 free
 * cell) without touching the baked art. Static per race (row->rank never changes), bakes once. */
static void DashPaintStandingsRanks(uint16_t* fb) {
    static const char* const kSuffixKey[GDX_RACEHUD_TOP5_MAX] = {"ST", "ND", "RD", "TH", "TH"};
    int i;
    if (!sDashWidgets[DASHW_STANDINGS].enabled) {
        return;
    }
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        char digitKey[2] = {(char) ('1' + i), '\0'};
        const DashGlyph* digit = DashFindGlyph(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, digitKey);
        const DashGlyph* suffix = DashFindGlyph(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, kSuffixKey[i]);
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H;
        const int freeX0 = DASH_STANDINGS_X0 + 29;
        int x = freeX0 + 1;
        if (digit == NULL || suffix == NULL) {
            continue;
        }
        DashBlitGlyphRatio(fb, x, y0 + (DASH_STANDINGS_ROW_H - digit->h / 2) / 2, digit, 1, 2);
        x += digit->w / 2 + 1;
        DashBlitGlyphRatio(fb, x, y0 + (DASH_STANDINGS_ROW_H - suffix->h / 2) / 2, suffix, 1, 2);
    }
}

/* COORDINATOR FIX (second round item 4 / third round item 1): the "return to the debug menu"
 * button, styled like the MODE chip (same panel fill, same pink border), labelled with the real
 * Font2 glyphs (not libctru console text, not the retired hand font). Static chrome, baked once;
 * gdx3ds_bottomhud_tick hit-tests the same DASH_BACK_* rect every tick independently of this.
 * Takes `snap` (unused) only to match sDashWidgets' uniform paint-fn signature -- see the
 * geometry-table comment at the top of this file for why gdx3ds_bottomhud_tick still calls this
 * directly (once, at backdrop-bake time) rather than through that table's own `paint` column.
 *
 * COORDINATOR FIX (idle-view round): renamed "MENU" -> "DEBUG" -- a clearer name for a button
 * that opens the text-console DEBUG UI, not a game menu. Shared, unchanged, by the race DASH
 * (gdx3ds_bottomhud_tick's own dismiss handling), CUSTOM mode (gdx3ds_bottomhud_tick's
 * once-per-entry CUSTOM backdrop), and the new idle view (DashPaintIdleBaked) -- the label change
 * and every behaviour below apply identically to all three; only the one literal changed. */
static void DashPaintBackButton(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    static const char kLabel[] = "DEBUG";
    const int num = 1;
    int tw;
    int x;
    int y;
    (void) snap;
    if (!sDashWidgets[DASHW_MENU_BUTTON].enabled) {
        return;
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kLabel, num, 1, DASH_FONT2_SPACE, 1);
    x = DASH_BACK_X0 + (DASH_BACK_W - tw) / 2;
    y = DASH_BACK_Y0 + (DASH_BACK_H - (8 * num)) / 2;
    DashFillRect(fb, DASH_BACK_X0, DASH_BACK_Y0, DASH_BACK_W, DASH_BACK_H, 0x3885u);
    DashStrokeRect(fb, DASH_BACK_X0, DASH_BACK_Y0, DASH_BACK_W, DASH_BACK_H, 0xFB76u);
    DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kLabel, num, 1, DASH_FONT2_SPACE, 1);
}

/* COORDINATOR FIX (hardware feedback round, item 1): the energy bar's own flat procedural panel
 * + border, replacing the frozen/overflowing baked fill-bar art DashBakeSkipCell's "energy_bar"
 * case (col >= 54) now skips entirely -- see that function's own comment for the root cause. Same
 * DASH-chrome look every other procedural widget here uses (fill 0x3885u, border 0xFB76u), baked
 * once per race; DashPaintEnergyFooter's live fill (every tick, unchanged) draws on top of this. */
static void DashPaintEnergyFrame(uint16_t* fb) {
    if (!sDashWidgets[DASHW_ENERGY_BAR].enabled) {
        return;
    }
    DashFillRect(fb, DASH_ENERGY_X0 + 54, DASH_ENERGY_Y0 + 1, DASH_ENERGY_W - 54 - 1, DASH_ENERGY_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_ENERGY_X0 + 54, DASH_ENERGY_Y0, DASH_ENERGY_W - 54, DASH_ENERGY_H, 0xFB76u);
}

/* COORDINATOR FIX (third round item 2 / hardware feedback round item 5): the header's two live
 * fields -- the course name (was baked, now reads the real track) and the "<N> RACERS" readout
 * (was frozen baked-template text, item 5: "make it live... current racers remaining... if
 * cheap") -- as ONE self-contained, dirty-checked widget function, so it can sit in sDashWidgets'
 * table as a single per-tick paint fn instead of a once-per-race side call. Cheap on every tick
 * that changes nothing: two cache compares, no drawing. num=1,den=2 for the course name (Font1 is
 * a 16x16 native cell; the original baked art's own text sat at roughly this scale); Font2 at the
 * best-fit num (2 or 1) for the racer count, right-aligned in the now-unbaked cols 230+. */
static void DashPaintHeaderLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    if (!sDashWidgets[DASHW_HEADER].enabled) {
        return;
    }
    if (strcmp(sLastHeaderTrackName, snap->trackName) != 0) {
        const int num = 1;
        const int den = 2;
        const int y = DASH_HEADER_Y0 + (DASH_HEADER_H - (16 * num) / den) / 2;
        snprintf(sLastHeaderTrackName, sizeof(sLastHeaderTrackName), "%s", snap->trackName);
        if (snap->trackName[0] != '\0') {
            DashBlitFontText(fb, 6, y, kDashGlyphFont1, KDASHGLYPHFONT1_COUNT, snap->trackName, num, den,
                              DASH_FONT1_SPACE, 1);
        }
    }
    if (snap->racersRemaining != sLastHeaderRacersRemaining) {
        char buf[16];
        const int maxW = DASH_HEADER_X0 + DASH_HEADER_W - 234; /* cols 230..316: DashBakeSkipCell's skip zone */
        int num;
        int tw;
        int x;
        int y;
        sLastHeaderRacersRemaining = snap->racersRemaining;
        snprintf(buf, sizeof(buf), "%d RACERS", snap->racersRemaining);
        num = DashFitFont2Num(buf, maxW, 1);
        tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
        x = DASH_HEADER_X0 + DASH_HEADER_W - 4 - tw;
        y = DASH_HEADER_Y0 + (DASH_HEADER_H - (8 * num)) / 2;
        DashFillRect(fb, 230, DASH_HEADER_Y0 + 1, DASH_HEADER_W - 230 - 1, DASH_HEADER_H - 2, 0x2043u);
        DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
    }
}

/* The dimmed checkerboard, alone -- factored out of DashPaintBaked so the idle view
 * (DashPaintIdleBaked, below) can reuse the SAME 150 KB background bake instead of baking a
 * second full-320x240 canvas of its own (nibottom-progress.md's own Old-3DS memory note: a
 * second full-screen bake would cost another 150 KB RAM, which this reuse avoids entirely). Pure
 * extraction, no behaviour change -- DashPaintBaked's own call site below is byte-identical to
 * its pre-refactor body. */
static void DashPaintBackdropBackground(uint16_t* fb) {
    int i;
    for (i = 0; i < 76800; i++) {
        const int x = i % DASH_FB_W;
        const int y = i / DASH_FB_W;
        DashPixel(fb, x, y, DashDimBackground(kDashAsset_background[i]));
    }
}

/* Composites one kDashAssetWidgets entry over whatever is already in `fb` (background or an
 * earlier widget), skipping its own live-text cells (DashBakeSkipCell) and transparent texels --
 * factored out of DashPaintBaked's own per-widget loop so DashPaintIdleHeaderChrome (below) can
 * composite just the "header" widget alone for the idle view, with the exact same skip-cell and
 * alpha rules the race DASH's full bake already uses. */
static void DashCompositeAssetWidget(uint16_t* fb, const DashAssetWidget* w) {
    int row;
    for (row = 0; row < w->h; row++) {
        int col;
        for (col = 0; col < w->w; col++) {
            const unsigned short p = w->pixels[row * w->w + col];
            if (DashBakeSkipCell(w->id, row, col)) {
                continue; /* live-text-only cell within an otherwise-baked widget */
            }
            if ((p & 1u) == 0u) {
                continue; /* alpha=0: transparent, leave the layer below as-is */
            }
            DashPixel(fb, w->x + col, w->y + row, Rgba5551To565(p));
        }
    }
}

/* The once-per-race backdrop+chrome composite -- full rationale in the big comment block above
 * DashBakeSkipCell (this function used to sit directly below it; DashPaintBackdropBackground/
 * DashCompositeAssetWidget, extracted just above for the idle view to share, now sit between). */
static void DashPaintBaked(uint16_t* fb) {
    int i;
    DashPaintBackdropBackground(fb);
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        const DashAssetWidget* w = &kDashAssetWidgets[i];
        if (strcmp(w->id, "portrait") == 0 || strcmp(w->id, "mode_button") == 0) {
            continue; /* live-text-only boxes -- see the comment above */
        }
        DashCompositeAssetWidget(fb, w);
    }
    DashPaintStandingsRanks(fb);
    DashPaintBackButton(fb, NULL);
    DashPaintEnergyFrame(fb);
    DashStrokeRect(fb, DASH_MODE_X0, DASH_MODE_Y0, DASH_MODE_W, DASH_MODE_H, 0xFB76u);
}

/* ---- IDLE VIEW: the bottom screen's default look outside a live race -------------------
 *
 * The user's own ask: "we shouldn't just have the bottom screen only show a polished experience
 * during race... have the checkerboard always there, with the bottom 'menu' option there and
 * maybe just a g-diffuser logo." Reuses the SAME checkerboard bake the race DASH uses
 * (DashPaintBackdropBackground -- no second 150 KB full-screen image), the SAME baked "header"
 * chrome panel (DashCompositeAssetWidget, header only -- no race-only lap/timer/gap/standings/
 * energy_bar chrome, which would bake frozen EXAMPLE race numbers that have no meaning outside a
 * race), the real G-DIFFUSER wordmark (tools/gen_nibottom_idle_assets.py's small bake, the same
 * logo file the desktop port's own title menu already uses), and the SAME DEBUG button chrome
 * (DashPaintBackButton -- see DashPaintBackButton's own kLabel for why "DEBUG" now serves both
 * this view and the race DASH). Composited ONCE per idle-view entry (gdx3ds_bottomhud_idle_tick's
 * own sIdleBaked latch) -- genuinely static, unlike the race DASH's own per-tick live fields. */

/* Just the "header" widget's baked chrome, skipping its own live-text zones exactly as the race
 * DASH does (DashBakeSkipCell) -- those zones are left as checkerboard here too, since the idle
 * view has no course name or racer count to show. */
static void DashPaintIdleHeaderChrome(uint16_t* fb) {
    int i;
    for (i = 0; i < DASH_ASSET_WIDGET_COUNT; i++) {
        const DashAssetWidget* w = &kDashAssetWidgets[i];
        if (strcmp(w->id, "header") != 0) {
            continue;
        }
        DashCompositeAssetWidget(fb, w);
        return;
    }
}

/* Centres the baked G-DIFFUSER wordmark (gdx3ds_bottomhud_idle_assets.h, DASH_IDLE_LOGO_W/H)
 * over the free area above the header band -- num=1,den=1 since the generator already bakes it
 * at its final on-screen size, so DashBlitGlyphRatio does a plain alpha-composited copy here. */
static void DashPaintIdleLogo(uint16_t* fb) {
    static const DashGlyph kIdleLogo = {"LOGO", DASH_IDLE_LOGO_W, DASH_IDLE_LOGO_H, 0, kDashIdleLogoPixels};
    const int x = (DASH_FB_W - DASH_IDLE_LOGO_W) / 2;
    const int y = (DASH_HEADER_Y0 - DASH_IDLE_LOGO_H) / 2;
    DashBlitGlyphRatio(fb, x, y, &kIdleLogo, 1, 1);
}

/* The idle view's own once-per-entry backdrop: background, header chrome, logo, DEBUG button --
 * in that paint order (background first, everything else composited on top), mirroring
 * DashPaintBaked's own layering. */
static void DashPaintIdleBaked(uint16_t* fb) {
    DashPaintBackdropBackground(fb);
    DashPaintIdleHeaderChrome(fb);
    DashPaintIdleLogo(fb);
    DashPaintBackButton(fb, NULL);
}

/* Small colour tag for a standings row, tinted by character id -- kept alongside the real
 * portrait (DashPaintStandingsPortraits, below) as a cheap at-a-glance colour cue; harmless and
 * already working, so there was no reason to remove it when the portrait itself went live. */
static uint16_t DashCharacterColour(int character) {
    static const uint16_t kPalette[8] = {
        0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u, 0xF81Fu, 0x07FFu, 0xFD20u, 0xFFFFu,
    };
    if (character < 0) {
        return 0x4208u;
    }
    return kPalette[(unsigned) character % 8u];
}

static void DashPaintTop5Tags(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    int i;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H + 2;
        const int haveEntry = i < snap->top5Count;
        const uint16_t colour = haveEntry ? DashCharacterColour(snap->top5Character[i]) : 0x0000u;
        DashFillRect(fb, DASH_STANDINGS_X0 + DASH_STANDINGS_W - 8, y0, 6, 6, colour);
        if (haveEntry && snap->position == (i + 1)) {
            DashStrokeRect(fb, DASH_STANDINGS_X0 + 1, DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H + 1,
                           DASH_STANDINGS_W - 2, DASH_STANDINGS_ROW_H - 2, 0xFFFFu);
        }
    }
    sLastTop5Count = snap->top5Count;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        sLastTop5[i] = (i < snap->top5Count) ? snap->top5Character[i] : -1;
    }
}

static int Top5Changed(const GdxRaceHudSnapshot* snap) {
    int i;
    if (snap->top5Count != sLastTop5Count) {
        return 1;
    }
    for (i = 0; i < snap->top5Count; i++) {
        if (snap->top5Character[i] != sLastTop5[i]) {
            return 1;
        }
    }
    return 0;
}

/* COORDINATOR FIX (hardware feedback round, item 2): "these portrait elements don't update based
 * on the racers/position... they're static and essentially useless... they don't follow the
 * sizing of the original HUD, where the top 2 racers have bigger portraits." Source of the
 * ORIGINAL per-racer portrait and its sizing rule: decomp/src/overlays/ovl_i3/hud.c's
 * gCharacterPortraitTextures[] (one real 32x32 texture per character) and Hud_DrawRacePortraits
 * (i<3 draws at scale 1.0/32x32, i>=3 at 0.75/24x24 on the source's own 6-row readout). Baked here
 * via tools/gen_nibottom_bottomhud_glyphs.py's kDashGlyphPortrait table (same real archive
 * textures, keyed by the Racer::character index as a decimal string, looked up with the same
 * DashFindGlyph every other glyph table here already uses) -- not a swatch, not frozen art, the
 * REAL portrait for whichever racer actually holds that position this tick. This box has 5 rows,
 * not 6; the split is 2 big / 3 small, the closest analogue a 5-row readout has to the source's
 * 3-big/3-small split, and matches the user's own framing ("the top 2 racers have bigger
 * portraits"). Drawn BEFORE DashPaintTop5Tags (above) so that function's own white
 * highlight-border stroke -- which runs the full row width, including this portrait's own column
 * -- stays on top and visible, not erased by a later portrait repaint. */
static void DashPaintStandingsPortraits(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const int cellW = 29; /* DashBakeSkipCell's "standings" col <= 28 skip zone */
    int i;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        const int y0 = DASH_STANDINGS_Y0 + i * DASH_STANDINGS_ROW_H;
        DashFillRect(fb, DASH_STANDINGS_X0, y0, cellW, DASH_STANDINGS_ROW_H, 0x1041u);
        if (i < snap->top5Count) {
            char key[4];
            const DashGlyph* portrait;
            const int num = (i < 2) ? 7 : 5; /* top 2 rows: 28x28; rows 3-5: 20x20 */
            const int den = 8;
            snprintf(key, sizeof(key), "%d", snap->top5Character[i]);
            portrait = DashFindGlyph(kDashGlyphPortrait, KDASHGLYPHPORTRAIT_COUNT, key);
            if (portrait != NULL) {
                const int w = (portrait->w * num) / den;
                const int h = (portrait->h * num) / den;
                const int x = DASH_STANDINGS_X0 + (cellW - w) / 2;
                const int y = y0 + (DASH_STANDINGS_ROW_H - h) / 2;
                DashBlitGlyphRatio(fb, x, y, portrait, num, den);
            }
        }
    }
}

/* The standings box's own per-tick paint fn (sDashWidgets' "standings" row): dirty-checked
 * against Top5Changed, same cadence the rank badges/colour swatches already used, now also
 * covering the live portraits above -- all three stay in sync since they only ever repaint
 * together, on the same edge. */
static void DashPaintStandingsLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    if (!sDashWidgets[DASHW_STANDINGS].enabled) {
        return;
    }
    if (!Top5Changed(snap)) {
        return;
    }
    DashPaintStandingsPortraits(fb, snap);
    DashPaintTop5Tags(fb, snap); /* also refreshes sLastTop5/sLastTop5Count */
}

/* COORDINATOR FIX (hw bug n60-3d7, item 6): "the player and rival markers are missing, or show
 * as black dots." Root cause: DashPaintMapBox already called gdx_racemap_snapshot() (for its
 * `.valid` gate) but never drew `.markers[]` -- the snapshot's marker list was sitting right
 * there, unused, exactly the "future work" gap this comment used to name. gdx3ds_racemap.c's own
 * debug MAP tab (BmapMarker + kMarkerRgba5551) already proves the marker DATA and COLOURS are
 * correct (kMarkerRgba5551 below is byte-identical to decomp's own MINIMAP_*_COLOR constants,
 * confirmed against decomp/src/overlays/ovl_i3/minimap.h) -- this was never a wrong-colour bug,
 * it was markers never being drawn on the DASH's own map_box at all, which is exactly what reads
 * as "missing". (MINIMAP_GHOST_COLOR is legitimately black in the real game -- a ghost racer's
 * own marker -- not a bug; it only ever appears in time-attack/time-trial mode.) */
static const unsigned short kDashMarkerRgba5551[6] = {
    (unsigned short) ((0u << 11) | (31u << 6) | (31u << 1) | 1u),  /* PLAYER1 cyan    */
    (unsigned short) ((31u << 11) | (31u << 6) | (0u << 1) | 1u),  /* PLAYER2 yellow  */
    (unsigned short) ((0u << 11) | (31u << 6) | (0u << 1) | 1u),   /* PLAYER3 green   */
    (unsigned short) ((31u << 11) | (15u << 6) | (31u << 1) | 1u), /* PLAYER4 magenta */
    (unsigned short) ((0u << 11) | (0u << 6) | (31u << 1) | 1u),   /* LEADER blue     */
    (unsigned short) ((0u << 11) | (0u << 6) | (0u << 1) | 1u),    /* GHOST black     */
};

/* Clamps v into [lo,hi] -- small single-purpose helper, used below to keep the map's
 * destination-pixel-driven scale (DashPaintMapMarker/DashPaintMapBox) safe at the box edges now
 * that the scale is a plain ratio (DASH_MAP_PX/DASH_MAP_SRC_DIM) rather than a fixed integer. */
static int DashClampInt(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    } else if (v > hi) {
        return hi;
    } else {
        return v;
    }
}

/* One 3x3-source-pixel marker, mapped to its own destination-pixel rect at the box's current
 * DASH_MAP_PX/DASH_MAP_SRC_DIM ratio -- same shape as gdx3ds_racemap.c's own BmapMarker, just
 * ratio-driven instead of a fixed-integer replication, so the hardware-feedback-round map shrink
 * (item 4, DASH_MAP_PX now 168, not an integer multiple of 64) scales markers correctly too; any
 * FUTURE custom-HUD resize of this box needs no change here either. Draw order: called AFTER the
 * track bitmap blit below, so markers sit on top of the track/background, matching the real
 * game's own draw order (Minimap_DrawCourseMinimap draws the track first, then stamps marker
 * fill-rects over it). */
static void DashPaintMapMarker(uint16_t* fb, int srcCol, int srcRow, uint16_t colour565) {
    const int c0 = DashClampInt(srcCol - 1, 0, DASH_MAP_SRC_DIM);
    const int c1 = DashClampInt(srcCol + 2, 0, DASH_MAP_SRC_DIM);
    const int r0 = DashClampInt(srcRow - 1, 0, DASH_MAP_SRC_DIM);
    const int r1 = DashClampInt(srcRow + 2, 0, DASH_MAP_SRC_DIM);
    const int x0 = DASH_MAP_X0 + (c0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int x1 = DASH_MAP_X0 + (c1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int y0 = DASH_MAP_Y0 + (r0 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    const int y1 = DASH_MAP_Y0 + (r1 * DASH_MAP_PX) / DASH_MAP_SRC_DIM;
    DashFillRect(fb, x0, y0, x1 - x0, y1 - y0, colour565);
}

/* The map box: nearest-neighbour scale of the shared 64x64 CI8 minimap bitmap into DASH_MAP_PX
 * square screen pixels, destination-pixel-driven (iterate the SCREEN pixel, divide back to the
 * source index) rather than the old fixed-integer-replication loop -- see DASH_MAP_PX's own
 * comment (hardware feedback round, item 4: "could probably afford to be a bit smaller") for why
 * the box is no longer an exact integer multiple of 64. Racer markers (hw bug n60-3d7 item 6) are
 * drawn on top of it, reusing the same snapshot this function already fetched for its `.valid`
 * gate. */
static void DashPaintMapBox(uint16_t* fb) {
    GdxRaceMapSnapshot mapSnap;
    const unsigned char* tex = NULL;
    unsigned short pal5551[4];
    uint16_t pal565[4];
    int dx;
    int i;

    gdx_racemap_snapshot(&mapSnap);
    if (!mapSnap.valid || !gdx_racemap_bitmap(&tex, pal5551)) {
        DashFillRect(fb, DASH_MAP_X0, DASH_MAP_Y0, DASH_MAP_PX, DASH_MAP_PX, 0x0841u);
        DashStrokeRect(fb, DASH_MAP_X0 - 1, DASH_MAP_Y0 - 1, DASH_MAP_PX + 2, DASH_MAP_PX + 2, 0xFFFFu);
        return;
    }
    pal565[0] = 0x0841u;
    for (i = 1; i < 4; i++) {
        pal565[i] = Rgba5551To565(pal5551[i]);
    }
    for (dx = 0; dx < DASH_MAP_PX; dx++) {
        const int srcCol = (dx * DASH_MAP_SRC_DIM) / DASH_MAP_PX;
        int dy;
        for (dy = 0; dy < DASH_MAP_PX; dy++) {
            const int srcRow = (dy * DASH_MAP_SRC_DIM) / DASH_MAP_PX;
            const uint16_t colour = pal565[tex[(size_t) srcRow * DASH_MAP_SRC_DIM + srcCol] & 3u];
            DashPixel(fb, DASH_MAP_X0 + dx, DASH_MAP_Y0 + dy, colour);
        }
    }
    for (i = 0; i < mapSnap.markerCount; i++) {
        const GdxRaceMapMarker* m = &mapSnap.markers[i];
        if (m->blink && !mapSnap.blinkOn) {
            continue; /* the game's own (gGameFrameCount % 16) < 8 blink */
        }
        if ((unsigned) m->kind < 6u) {
            DashPaintMapMarker(fb, m->col, m->row, Rgba5551To565(kDashMarkerRgba5551[m->kind]));
        }
    }
    DashStrokeRect(fb, DASH_MAP_X0 - 1, DASH_MAP_Y0 - 1, DASH_MAP_PX + 2, DASH_MAP_PX + 2, 0xFFFFu);
}

/* sDashWidgets' "map_box" per-tick paint fn: owns its own [debug] bottom_race_hz throttle
 * internally (moved here from gdx3ds_bottomhud_tick, which used to track sLastMapTick itself) so
 * this widget is fully self-contained, matching the custom-HUD-groundwork table's "each row is
 * one independent per-tick paint call" shape. `snap` is unused (DashPaintMapBox fetches its own
 * GdxRaceMapSnapshot, a deliberately separate contract -- see gdx_racemap.h's own module doc for
 * why map state is a sibling struct, not a growth of GdxRaceHudSnapshot). */
static void DashPaintMapBoxLive(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const u64 now = svcGetSystemTick();
    const u64 interval = (u64) (1000.0 * CPU_TICKS_PER_MSEC) / (u64) sHz;
    (void) snap;
    if (!sDashWidgets[DASHW_MAP_BOX].enabled) {
        return;
    }
    if ((now - sLastMapTick) < interval) {
        return;
    }
    DashPaintMapBox(fb);
    sLastMapTick = now;
}

/* COORDINATOR FIX (third round item 5): the baked kDashAsset_energy_bar (320x24) has its
 * "ENERGY" label at cols ~7-53 and the decorative frame at cols 54-314; filling from x=4 (the
 * old flat 4px bezel on both edges) painted straight over the label and made the frame's own
 * left cap look clipped. Start the fill inside the frame instead (x=57) and stop 4px short of
 * its right edge (314-4=310), so the fill sits inside the frame art instead of under the label. */
#define DASH_ENERGY_FILL_X0 (DASH_ENERGY_X0 + 57)
#define DASH_ENERGY_FILL_W ((DASH_ENERGY_X0 + 314 - 4) - DASH_ENERGY_FILL_X0)

/* COORDINATOR FIX (fourth round): "the energy bar is flat yellow, but it should follow the real
 * game's colour scheme exactly." Read hud.c's own Hud_DrawEnergyBar (decomp/src/overlays/
 * ovl_i3/hud.c:617-665) and sEnergyBarFillColors (:134-145): there is NO continuous energy-level
 * gradient and NO low-energy flashing anywhere in that function or file (grepped) -- the only
 * state it reads is RACER_STATE_CAN_BOOST (sEnergyBarFillColors[i+5] vs [i], i=0..4), the same
 * flag DASH's own `boostReady` already mirrors exactly (gdx_racemap.h:122, decomp_port.c:2063).
 * What the game DOES draw is a fixed 5-row vertical "glossy" shading, one solid colour per
 * scanline, switched as a whole from a red family (boost not yet available) to a green family
 * (boost available -- set once per completed lap, cleared on use) by that single flag. Row 3 of
 * the red family packs two different RGBA5551 values into one 32-bit N64 FillColor
 * (GPACK_RGBA5551(220,0,0,1)<<16 | GPACK_RGBA5551(230,0,0,1)), which the hardware fill path
 * renders as alternating pixels -- reproduced here as an x-parity dither. Values below are
 * hud.c's own sEnergyBarFillColors rows, each converted once (offline, by hand) through this
 * file's own Rgba5551To565 (the same 5->6 bit green widen every other baked-art pixel here goes
 * through). The bar is scaled to our 16px-tall fill (hud.c's 1-player bar is 5px tall) by
 * nearest-neighbour row mapping, same integer-ratio style as DashBlitGlyphRatio. */
static const uint16_t kEnergyRowRed[5] = {0x9800u, 0xd800u, 0xf800u, 0xd800u, 0x9800u};
static const uint16_t kEnergyRowRedDitherB = 0xe000u; /* row 3's alternate (odd-x) pixel colour */
static const uint16_t kEnergyRowGreen[5] = {0x07e0u, 0x07e2u, 0x0624u, 0x0526u, 0x0428u};

static void DashPaintEnergyFooter(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const int fillW = (int) (((long) DASH_ENERGY_FILL_W * snap->energyMilli) / 1000);
    const int top = DASH_ENERGY_Y0 + 4;
    const int fillH = DASH_ENERGY_H - 8;
    int dy;
    if (!sDashWidgets[DASHW_ENERGY_BAR].enabled) {
        return;
    }
    DashFillRect(fb, DASH_ENERGY_FILL_X0, top, DASH_ENERGY_FILL_W, fillH, 0x0000u);
    for (dy = 0; dy < fillH; dy++) {
        const int row = (dy * 5) / fillH;
        int dx;
        for (dx = 0; dx < fillW; dx++) {
            const int x = DASH_ENERGY_FILL_X0 + dx;
            uint16_t colour;
            if (snap->boostReady) {
                colour = kEnergyRowGreen[row];
            } else if (row == 3 && (x & 1)) {
                colour = kEnergyRowRedDitherB;
            } else {
                colour = kEnergyRowRed[row];
            }
            DashPixel(fb, x, top + dy, colour);
        }
    }
}

static void DashPaintModeDot(uint16_t* fb) {
    /* Sized GDX_HUD_PLACEMENT_CUSTOM+1 (4), not GDX_HUD_PLACEMENT_MODE_COUNT (3): CUSTOM is a
     * real, reachable value of sPlacementMode now, and this array is indexed by it directly. */
    static const uint16_t kModeColour[GDX_HUD_PLACEMENT_CUSTOM + 1] = {0x07E0u, 0xFFE0u, 0xF81Fu, 0x07FFu};
    if (!sDashWidgets[DASHW_MODE_CHIP].enabled) {
        return;
    }
    DashFillRect(fb, DASH_MODE_X0 + DASH_MODE_W - 10, DASH_MODE_Y0 + 2, 6, 6, kModeColour[sPlacementMode]);
}

/* ---- the live numeric overlay: real glyphs only, one paint fn per right-column widget ---- */

/* "0'42"18"" -- same separators hud.c's own Hud_DrawTimerScisThousandths draws (prime/double-
 * prime, not colon/dot; see tools/gen_nibottom_mockup_assets.py's own citation of that). ms is
 * assumed >= 0 (lapTimeMs/bestTimeMs never go negative); gapMs is signed and goes through
 * DashFormatInterval instead, which adds the sign itself. */
static void DashFormatClock(char* out, size_t cap, int ms) {
    const int totalCs = ms / 10;
    const int cs = totalCs % 100;
    const int totalSec = totalCs / 100;
    const int sec = totalSec % 60;
    const int min = totalSec / 60;
    snprintf(out, cap, "%d'%02d\"%02d", min, sec, cs);
}

static void DashFormatInterval(char* out, size_t cap, int ms) {
    char body[16];
    const int absMs = (ms < 0) ? -ms : ms;
    DashFormatClock(body, sizeof(body), absMs);
    snprintf(out, cap, "%s%s", (ms < 0) ? "-" : "", body);
}

/* Clears a value row to the same dark panel fill the header/MODE-chip/MENU-button widgets use
 * (not an attempt to reproduce the dimmed checker pixel-for-pixel underneath -- a flat panel
 * read as "behind this text" every tick, cheap, and consistent with the rest of the box's
 * chrome) before redrawing this tick's digits over it. */
static void DashClearValueRow(uint16_t* fb, int x0, int y0, int w, int h) {
    DashFillRect(fb, x0, y0, w, h, 0x1041u);
}

/* COORDINATOR FIX (second round item 2 / third round item 1): "the right-column text... is too
 * small" -> "replace [the hand font]... aTimerSymbolsTex (8x16 digits plus ' " for times)."
 * Each field clears its own unbaked value row (DashBakeSkipCell) and redraws with the real
 * digit-strip textures every tick, at their native num=1,den=1 scale (gap=0: a dense digit strip
 * reads fine edge-to-edge, same way the game's own HUD draws it, and it is the only scale that
 * fits LAP/TIME/BEST/GAP's narrow ~56-58px boxes without clipping -- confirmed by width
 * arithmetic against kFontCharWidths-less-but-uniform 8px timer/lap cells).
 *
 * Split into one function per sDashWidgets row (Lap/Timer/Gap) instead of one monolithic sweep,
 * matching the "one paint fn per widget" table shape; BOOST's own badge is gone outright (item 6,
 * hardware feedback round: "basically useless... I'd put the [KO] stars at the top of this right
 * side column instead" -- see DashPaintKOStars, below, and sDashWidgets' own "ko_stars" row for
 * where that box now lives). */
static void DashPaintRightColumnLap(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    int lapDisp;
    if (!sDashWidgets[DASHW_LAP].enabled) {
        return;
    }
    /* COORDINATOR FIX (hardware feedback round, item 7): "when the race ends, this goes up to
     * 4/3. Should be clamped." gRacers[0].lap keeps counting past gTotalLapCount once the race
     * finishes (the decomp's own post-finish bookkeeping, left alone here -- this is a DASH
     * display-only clamp, not a game-state change). */
    lapDisp = snap->lap;
    if (snap->totalLaps > 0 && lapDisp > snap->totalLaps) {
        lapDisp = snap->totalLaps;
    }
    snprintf(buf, sizeof(buf), "%d/%d", lapDisp, snap->totalLaps);
    DashClearValueRow(fb, DASH_LAP_X0 + 2, DASH_LAP_Y0 + 12, 56, 14);
    DashBlitFontText(fb, DASH_LAP_X0 + 2, DASH_LAP_Y0 + 13, kDashGlyphLap, KDASHGLYPHLAP_COUNT, buf, 1, 1, 0, 0);
}

static void DashPaintRightColumnTimer(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    if (!sDashWidgets[DASHW_TIMER].enabled) {
        return;
    }
    DashFormatClock(buf, sizeof(buf), snap->lapTimeMs);
    DashClearValueRow(fb, DASH_TIMER_X0 + 2, DASH_TIMER_Y0 + 18, 56, 20);
    DashBlitFontText(fb, DASH_TIMER_X0 + 1, DASH_TIMER_Y0 + 19, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, buf, 1, 1, 0, 0);

    DashFormatClock(buf, sizeof(buf), snap->bestTimeMs);
    DashClearValueRow(fb, DASH_TIMER_X0 + 2, DASH_TIMER_Y0 + 52, 56, 17);
    DashBlitFontText(fb, DASH_TIMER_X0 + 1, DASH_TIMER_Y0 + 53, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, buf, 1, 1, 0, 0);
}

static void DashPaintRightColumnGap(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    if (!sDashWidgets[DASHW_GAP].enabled) {
        return;
    }
    DashFormatInterval(buf, sizeof(buf), snap->gapMs);
    DashClearValueRow(fb, DASH_GAP_X0 + 2, DASH_GAP_Y0 + 12, 58, 12);
    DashBlitFontText(fb, DASH_GAP_X0 + 1, DASH_GAP_Y0 + 12, kDashGlyphTimer, KDASHGLYPHTIMER_COUNT, buf, 1, 1, 0, 0);
}

/* COORDINATOR FIX (hardware feedback round, item 6): "[BOOST is] basically useless... more
 * useful would be the 'knockout' stars which are not visible anywhere on the second screen
 * HUD. I'd put the stars at the top of this right side column." Source: decomp/src/overlays/
 * ovl_i3/hud.c's Hud_DrawKOStars (hud_gfx/aHudRacersKOdTex, one 8x8 star per gRacersKOd, a
 * global race-wide knockout counter -- gdx_racehud_snapshot's own `koCount` field, see
 * gdx_racemap.h). No baked backdrop (same reasoning DASH_SPEED_* used when it was added): chrome,
 * the real star icon and the count are all repainted live, same chip style as MODE/MENU/SPEED. A
 * single icon + "x<N>" numeral reads correctly at any count from 0 to the full 30-racer field,
 * unlike trying to fit a literal growing star GRID (what the N64 HUD draws, with much more
 * screen edge to grow into) inside this 62px-wide column. */
static void DashPaintKOStars(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[8];
    const DashGlyph* star;
    int tw;
    int iconW;
    int x;
    int y;
    if (!sDashWidgets[DASHW_KO_STARS].enabled) {
        return;
    }
    /* "X%d", not "x%d": kDashGlyphFont2 only has uppercase A-Z (FONT_CHAR_SPEC, the generator's
     * own 48-slot font.c scheme) -- a lowercase 'x' silently drops (DashFindGlyph finds nothing,
     * DashBlitFontText skips it with zero advance), which first-pass testing against a live
     * emulator capture caught: the box rendered a bare "0", no "x". */
    snprintf(buf, sizeof(buf), "X%d", snap->koCount);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, 1, 1, DASH_FONT2_SPACE, 1);
    star = DashFindGlyph(kDashGlyphKoStar, KDASHGLYPHKOSTAR_COUNT, "star");
    iconW = (star != NULL) ? star->w * 2 : 0; /* num=2,den=1: the native 8x8 icon shown at 16x16 */

    DashFillRect(fb, DASH_KO_X0 + 1, DASH_KO_Y0 + 1, DASH_KO_W - 2, DASH_KO_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_KO_X0, DASH_KO_Y0, DASH_KO_W, DASH_KO_H, 0xFB76u);
    x = DASH_KO_X0 + (DASH_KO_W - (iconW + 4 + tw)) / 2;
    y = DASH_KO_Y0 + (DASH_KO_H - 16) / 2;
    if (star != NULL) {
        DashBlitGlyphRatio(fb, x, y, star, 2, 1);
    }
    DashBlitFontText(fb, x + iconW + 4, DASH_KO_Y0 + (DASH_KO_H - 8) / 2, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT,
                      buf, 1, 1, DASH_FONT2_SPACE, 1);
}

/* COORDINATOR FIX (hw bug n60-3d7, item 4): the speed readout (see sDashWidgets' own "speed"
 * row). Unlike lap/timer/gap, this box has no baked archive texture to redraw a value row
 * into -- it didn't exist before the header/energy_bar swap opened this gap -- so chrome and
 * value are both repainted here, every tick, same style as DashPaintModeLabel's chip (panel
 * fill 0x3885u, border 0xFB76u) rather than the darker 0x1041u value-row fill the baked boxes
 * use, so it reads as "the same family as MODE/MENU", not a mismatched fifth look. Font2 (not
 * the digit-strip kDashGlyphTimer, which has no letters) so "237 KPH" is one string, sized to
 * fit the 62px box the same way DashPaintModeLabel fits its label.
 *
 * COORDINATOR FIX (hardware feedback round, "also"): "dash-speed-box is x=260 w=62 -> 2px past
 * the right edge (322); fix." Table-driven now (DASH_SPEED_X0/W read sDashWidgets' "speed" row,
 * x=258 w=62 -> right edge 320, the screen's own width) instead of inheriting BOOST's old x0. */
static void DashPaintSpeed(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char buf[16];
    int num;
    int tw;
    int x;
    int y;
    if (!sDashWidgets[DASHW_SPEED].enabled) {
        return;
    }

    snprintf(buf, sizeof(buf), "%d KPH", snap->speedKph);
    num = DashFitFont2Num(buf, DASH_SPEED_W - 6, 1);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
    x = DASH_SPEED_X0 + (DASH_SPEED_W - tw) / 2;
    y = DASH_SPEED_Y0 + (DASH_SPEED_H - (8 * num)) / 2;

    DashFillRect(fb, DASH_SPEED_X0 + 1, DASH_SPEED_Y0 + 1, DASH_SPEED_W - 2, DASH_SPEED_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_SPEED_X0, DASH_SPEED_Y0, DASH_SPEED_W, DASH_SPEED_H, 0xFB76u);
    DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, buf, num, 1, DASH_FONT2_SPACE, 1);
}

/* COORDINATOR FIX (third round item 3): "verify the player's position... If it's wrong, fix the
 * source... Then remove that 'P30/30' debug string entirely; the big place numeral already
 * covers it." This IS that big numeral: real aPosition<N>Tex digit(s) + an
 * aPositionOrdinalSuffix<X>Tex, replacing the small P<n>/<n> text in the now-fully-unbaked
 * "portrait" box. Single digit (positions 1-9) draws at native num=1,den=1 (28x32 digit + 20x20
 * suffix side by side = 48px, fits the box with room); two digits (10+) halve to num=1,den=2 so
 * both digits plus the suffix (14+14+10=38px) still fit.
 *
 * COORDINATOR FIX (hardware feedback round, item 3): "has a black background that overflows onto
 * the mini map." Root cause: this box's own fill rect used to be exactly 64px wide (x=0..63),
 * landing its right edge on column 63 -- the SAME column DashPaintMapBox's white border stroke
 * occupies for its full height (DASH_MAP_X0 - 1 == 63 before the hardware-feedback-round map
 * shrink). This widget's fill paints every tick (DashPaintMapBox's own throttled redraw does
 * not), so it was repeatedly overwriting one column of the map's own border with black. Fixed at
 * the source: sDashWidgets' own "portrait" row is now 63px wide, not 64 -- see that table entry's
 * own comment. (The map box no longer sits at that same column after its own resize/recentre
 * either, so this is now doubly clear of it; the width fix stays, since a future custom-HUD drag
 * could move the map box back adjacent to this one.) */
static void DashPaintOurPosition(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    char digits[4];
    const char* suffixKey;
    int n;
    int den;
    int x;
    const int y0 = DASH_PORTRAIT_Y0;
    int i;
    if (!sDashWidgets[DASHW_PORTRAIT].enabled) {
        return;
    }
    n = snprintf(digits, sizeof(digits), "%d", snap->position);
    den = (n >= 2) ? 2 : 1;
    x = DASH_PORTRAIT_X0 + 2;

    DashFillRect(fb, DASH_PORTRAIT_X0, DASH_PORTRAIT_Y0, DASH_PORTRAIT_W, DASH_PORTRAIT_H, 0x1041u);
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        char key[2] = {digits[i], '\0'};
        const DashGlyph* digit = DashFindGlyph(kDashGlyphPosDigit, KDASHGLYPHPOSDIGIT_COUNT, key);
        if (digit == NULL) {
            continue;
        }
        DashBlitGlyphRatio(fb, x, y0 + (DASH_PORTRAIT_H - digit->h / den) / 2, digit, 1, den);
        x += digit->w / den + 1;
    }
    switch (snap->position % 100) {
        case 11: case 12: case 13: suffixKey = "TH"; break;
        default:
            switch (snap->position % 10) {
                case 1: suffixKey = "ST"; break;
                case 2: suffixKey = "ND"; break;
                case 3: suffixKey = "RD"; break;
                default: suffixKey = "TH"; break;
            }
    }
    {
        const DashGlyph* suffix = DashFindGlyph(kDashGlyphPosSuffix, KDASHGLYPHPOSSUFFIX_COUNT, suffixKey);
        if (suffix != NULL) {
            DashBlitGlyphRatio(fb, x, y0 + (DASH_PORTRAIT_H - suffix->h / den) / 2, suffix, 1, den);
        }
    }
}

/* COORDINATOR FIX (third round item 4): "the MODE chip is clipped: only 'TOP' and a green
 * square are visible. Show the full mode name." Real Font2 glyphs, best-fit num (2 or 1) so
 * "MODE 1"/"MODE 2"/"MODE 3" (the same labels docs/research/native/nibottom-mockup.html's own
 * MODE_CHIP_LABELS draws) always reads in full inside the 62x16 chip. */
static const char* DashModeChipLabel(int mode) {
    switch (mode) {
        case GDX_HUD_PLACEMENT_SPLIT: return "MODE 2";
        case GDX_HUD_PLACEMENT_ALL_BOTTOM: return "MODE 3";
        case GDX_HUD_PLACEMENT_CUSTOM: return "MODE Z"; /* the brief's own suggested label */
        default: return "MODE 1";
    }
}

static void DashPaintModeLabel(uint16_t* fb, const GdxRaceHudSnapshot* snap) {
    const char* label;
    int maxW;
    int num;
    int tw;
    int x;
    int y;
    (void) snap; /* unused -- present only to match sDashWidgets' uniform paint-fn signature */
    if (!sDashWidgets[DASHW_MODE_CHIP].enabled) {
        return;
    }
    label = DashModeChipLabel(sPlacementMode);
    maxW = DASH_MODE_W - 12; /* leave room for the colour dot at the chip's right edge */
    num = DashFitFont2Num(label, maxW, 1);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
    x = DASH_MODE_X0 + 4;
    y = DASH_MODE_Y0 + (DASH_MODE_H - (8 * num)) / 2;
    (void) tw;
    DashFillRect(fb, DASH_MODE_X0 + 1, DASH_MODE_Y0 + 1, DASH_MODE_W - 2, DASH_MODE_H - 2, 0x3885u);
    DashBlitFontText(fb, x, y, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
}

/* ---- CUSTOM HUD mode: generic widget paint, sample data, reset/restore, touch handling --- */

/* "Show sample values if no race is live": built whenever CUSTOM/edit mode is entered without a
 * live race (gdx3ds_menu.c's "HUDX" debug tab is the only way that happens -- see
 * gdx3ds_bottomhud_enter_custom_edit). Plausible mid-race numbers, nothing more; this is display
 * data only, never fed back into any game state. */
static void DashBuildSampleSnapshot(GdxRaceHudSnapshot* snap) {
    int i;
    memset(snap, 0, sizeof(*snap));
    snap->valid = 1;
    snap->numPlayers = 1;
    snap->position = 3;
    snap->totalRacers = 30;
    snap->racersRemaining = 27;
    snap->speedKph = 184;
    snap->lap = 2;
    snap->totalLaps = 3;
    snap->character = 0;
    snap->energyMilli = 620;
    snap->boostReady = 0;
    snap->koCount = 3;
    snap->lapTimeMs = 42180;
    snap->bestTimeMs = 41530;
    snap->gapMs = 1250;
    snap->top5Count = GDX_RACEHUD_TOP5_MAX;
    for (i = 0; i < GDX_RACEHUD_TOP5_MAX; i++) {
        snap->top5Character[i] = i;
    }
    snprintf(snap->trackName, sizeof(snap->trackName), "SAMPLE TRACK");
}

static const char* DashWidgetShortLabel(int id) {
    switch (id) {
        case DASHW_ENERGY_BAR: return "ENERGY";
        case DASHW_PORTRAIT: return "RANK";
        case DASHW_STANDINGS: return "TOP5";
        case DASHW_KO_STARS: return "KO";
        case DASHW_LAP: return "LAP";
        case DASHW_TIMER: return "TIME";
        case DASHW_GAP: return "GAP";
        case DASHW_SPEED: return "SPEED";
        case DASHW_MAP_BOX: return "MAP";
        default: return "?";
    }
}

/* One short live-value string per editable widget, in the same format each widget's own real
 * (non-CUSTOM) paint function already uses -- DashFormatClock/DashFormatInterval, the lap
 * clamp, etc. -- just rendered through the generic Font2 box instead of that widget's own
 * digit-strip glyphs (which assume a fixed baked box size CUSTOM mode does not have). */
static void DashFormatCustomValue(char* out, size_t cap, int id, const GdxRaceHudSnapshot* snap) {
    switch (id) {
        case DASHW_ENERGY_BAR:
            snprintf(out, cap, "%d%%", snap->energyMilli / 10);
            break;
        case DASHW_PORTRAIT:
            snprintf(out, cap, "%d", snap->position);
            break;
        case DASHW_STANDINGS:
            snprintf(out, cap, "TOP %d", snap->top5Count);
            break;
        case DASHW_KO_STARS:
            snprintf(out, cap, "X%d", snap->koCount);
            break;
        case DASHW_LAP: {
            int lapDisp = snap->lap;
            if (snap->totalLaps > 0 && lapDisp > snap->totalLaps) {
                lapDisp = snap->totalLaps;
            }
            snprintf(out, cap, "%d/%d", lapDisp, snap->totalLaps);
            break;
        }
        case DASHW_TIMER: {
            char c[16];
            DashFormatClock(c, sizeof(c), snap->lapTimeMs);
            snprintf(out, cap, "%s", c);
            break;
        }
        case DASHW_GAP:
            DashFormatInterval(out, cap, snap->gapMs);
            break;
        case DASHW_SPEED:
            snprintf(out, cap, "%d KPH", snap->speedKph);
            break;
        default:
            if (cap > 0) {
                out[0] = '\0';
            }
    }
}

/* The generic box every editable widget draws as in CUSTOM mode: flat fill, border (brighter
 * white when `selected`, the usual chip pink otherwise), a short label centred at the top, and
 * (room permitting) a live value centred at the bottom. Replaces that widget's own fancy baked-
 * art paint function entirely in this mode -- see the block comment above sCustomWidgets. */
static void DashPaintCustomWidget(uint16_t* fb, int id, const GdxRaceHudSnapshot* snap, int selected) {
    const DashWidgetRect* r = &sCustomWidgets[id];
    const char* label = DashWidgetShortLabel(id);
    char valueBuf[16];
    int num;
    int tw;
    /* BUG FIX (found via this round's own verification capture -- see the report): fitting the
     * label and the value text EACH independently by width alone let a wide-but-short widget
     * (e.g. the default full-width ENERGY_BAR, h=24) pick num=2 for both, which are then 16px
     * tall each -- the label's top-anchored row and the value's bottom-anchored row overlapped
     * by more than half their height inside a 24px box, garbling both ("EN[glitch]RGY"). rowNum
     * caps whichever size DashFitFont2Num picks so two 8px-tall rows (label then value) plus a
     * 2px margin on every edge ALWAYS fit vertically before either one is ever asked to draw at
     * num=2; a short widget silently falls back to the small, always-safe num=1 for both. */
    const int rowNum = (r->h >= 2 * 16 + 6) ? 2 : 1;
    if (!r->enabled) {
        return;
    }
    DashFillRect(fb, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 0x3885u);
    DashStrokeRect(fb, r->x, r->y, r->w, r->h, selected ? 0xFFFFu : 0xFB76u);

    num = DashFitFont2Num(label, r->w - 4, 1);
    if (num > rowNum) {
        num = rowNum;
    }
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, r->x + (r->w - tw) / 2, r->y + 2, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, label, num, 1,
                      DASH_FONT2_SPACE, 1);

    DashFormatCustomValue(valueBuf, sizeof(valueBuf), id, snap);
    if (valueBuf[0] != '\0' && r->h >= 24) {
        int vnum = DashFitFont2Num(valueBuf, r->w - 4, 1);
        int vtw;
        if (vnum > rowNum) {
            vnum = rowNum;
        }
        vtw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, valueBuf, vnum, 1, DASH_FONT2_SPACE, 1);
        DashBlitFontText(fb, r->x + (r->w - vtw) / 2, r->y + r->h - 8 * vnum - 2, kDashGlyphFont2,
                          KDASHGLYPHFONT2_COUNT, valueBuf, vnum, 1, DASH_FONT2_SPACE, 1);
    }
}

/* Edit-mode-only overlay, drawn AFTER every widget box so its handles always sit on top: a red
 * close "X" at the top-right corner (every visible editable widget, not just the selected one --
 * closing something should never require selecting it first) and, for resizable widgets only, a
 * cyan resize handle at the bottom-right corner. */
static void DashPaintEditHandles(uint16_t* fb, int id) {
    const DashWidgetRect* r = &sCustomWidgets[id];
    if (!r->enabled) {
        return;
    }
    DashFillRect(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE - 1, r->y + 1, GDX_HUDCUSTOM_HANDLE, GDX_HUDCUSTOM_HANDLE,
                 0xF800u);
    DashBlitFontText(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE, r->y + 1, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, "X", 1,
                      1, 0, 0);
    if (DashWidgetResizable(id)) {
        DashFillRect(fb, r->x + r->w - GDX_HUDCUSTOM_HANDLE, r->y + r->h - GDX_HUDCUSTOM_HANDLE,
                     GDX_HUDCUSTOM_HANDLE, GDX_HUDCUSTOM_HANDLE, 0x07FFu);
    }
}

/* The hidden-widget tray: one chip per currently-disabled editable widget, left to right. Loop
 * shape (which widgets, which order, which x) is duplicated in DashCustomHandleTouch's own tray
 * hit-test on purpose -- both are short, both are commented, and keeping them as two small
 * mirrored loops is clearer here than threading a shared index lookup through both a paint call
 * and a touch call for a <= 9-item list. */
static void DashPaintTray(uint16_t* fb) {
    int i;
    int x = 2;
    for (i = 0; i < DASHW_COUNT; i++) {
        if (!DashWidgetEditable(i) || sCustomWidgets[i].enabled) {
            continue;
        }
        DashFillRect(fb, x, DASH_TRAY_Y0, DASH_TRAY_CHIP_W, DASH_TRAY_CHIP_H, 0x2104u);
        DashStrokeRect(fb, x, DASH_TRAY_Y0, DASH_TRAY_CHIP_W, DASH_TRAY_CHIP_H, 0x07E0u);
        DashBlitFontText(fb, x + 2, DASH_TRAY_Y0 + 3, kDashGlyphFont2, KDASHGLYPHFONT2_COUNT,
                          DashWidgetShortLabel(i), 1, 1, 0, 0);
        x += DASH_TRAY_CHIP_W + DASH_TRAY_CHIP_GAP;
    }
}

/* RESET + the EDIT<->DONE toggle, drawn every tick CUSTOM mode is active (cheap: two small
 * chips), in the header's one remaining free gap. */
static void DashPaintCustomChrome(uint16_t* fb) {
    static const char kReset[] = "RESET";
    const char* editLabel = sEditMode ? "DONE" : "EDIT";
    int tw;

    DashFillRect(fb, DASH_RESET_X0 + 1, DASH_RESET_Y0 + 1, DASH_RESET_W - 2, DASH_RESET_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_RESET_X0, DASH_RESET_Y0, DASH_RESET_W, DASH_RESET_H, 0xFB76u);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, kReset, 1, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, DASH_RESET_X0 + (DASH_RESET_W - tw) / 2, DASH_RESET_Y0 + 4, kDashGlyphFont2,
                      KDASHGLYPHFONT2_COUNT, kReset, 1, 1, DASH_FONT2_SPACE, 1);

    DashFillRect(fb, DASH_EDIT_X0 + 1, DASH_EDIT_Y0 + 1, DASH_EDIT_W - 2, DASH_EDIT_H - 2, 0x3885u);
    DashStrokeRect(fb, DASH_EDIT_X0, DASH_EDIT_Y0, DASH_EDIT_W, DASH_EDIT_H, sEditMode ? 0xFFFFu : 0xFB76u);
    tw = DashFontTextWidthRatio(kDashGlyphFont2, KDASHGLYPHFONT2_COUNT, editLabel, 1, 1, DASH_FONT2_SPACE, 1);
    DashBlitFontText(fb, DASH_EDIT_X0 + (DASH_EDIT_W - tw) / 2, DASH_EDIT_Y0 + 4, kDashGlyphFont2,
                      KDASHGLYPHFONT2_COUNT, editLabel, 1, 1, DASH_FONT2_SPACE, 1);
}

/* RESET: restores every editable widget to its default (ALL_BOTTOM) rect, all visible, clears
 * any selection/drag/resize in progress, and persists immediately -- the brief's own "RESET
 * restoring the default layout with everything on the bottom". */
static void DashCustomReset(void) {
    DashCustomDefaults(sCustomWidgets);
    sEditSelected = -1;
    sEditDragging = 0;
    sEditResizing = 0;
    DashHudIniSave();
    DashSyncCustomTopMask();
    DashLog("[dash] custom layout reset to defaults");
}

static void DashCustomToggleEdit(void) {
    sEditMode = !sEditMode;
    sEditDragging = 0;
    sEditResizing = 0;
    if (!sEditMode) {
        DashHudIniSave();
        DashSyncCustomTopMask();
        DashLog("[dash] custom edit -> DONE, layout saved");
    } else {
        sEditSelected = -1;
        DashLog("[dash] custom edit -> begin");
    }
}

/* All CUSTOM-mode touch handling for one tick: RESET/EDIT-DONE buttons (always live while in
 * CUSTOM, edit mode or not), then -- edit mode only -- the hidden-widget tray, each visible
 * widget's close "X" / resize handle / body (in that priority order, first match wins, same
 * rule gdx_hudcustom_hit_test_all documents), and finally drag/resize tracking while the touch
 * stays down. Called once per tick, BEFORE painting, so the same tick's paint already reflects
 * whatever just changed. */
static void DashCustomHandleTouch(int touching, int prevTouching, int touchPx, int touchPy) {
    if (touching && !prevTouching) {
        if (touchPx >= DASH_RESET_X0 && touchPx < DASH_RESET_X0 + DASH_RESET_W && touchPy >= DASH_RESET_Y0 &&
            touchPy < DASH_RESET_Y0 + DASH_RESET_H) {
            DashCustomReset();
            return;
        }
        if (touchPx >= DASH_EDIT_X0 && touchPx < DASH_EDIT_X0 + DASH_EDIT_W && touchPy >= DASH_EDIT_Y0 &&
            touchPy < DASH_EDIT_Y0 + DASH_EDIT_H) {
            DashCustomToggleEdit();
            return;
        }
        if (!sEditMode) {
            return; /* nothing else is interactive in CUSTOM's plain view mode */
        }
        {
            int i;
            int x = 2;
            for (i = 0; i < DASHW_COUNT; i++) {
                if (!DashWidgetEditable(i) || sCustomWidgets[i].enabled) {
                    continue;
                }
                if (touchPx >= x && touchPx < x + DASH_TRAY_CHIP_W && touchPy >= DASH_TRAY_Y0 &&
                    touchPy < DASH_TRAY_Y0 + DASH_TRAY_CHIP_H) {
                    sCustomWidgets[i].enabled = 1;
                    sEditSelected = i;
                    return;
                }
                x += DASH_TRAY_CHIP_W + DASH_TRAY_CHIP_GAP;
            }
        }
        {
            GdxHudCustomRect rects[DASHW_COUNT];
            unsigned resizableMask = 0;
            int i;
            int hitIndex = -1;
            GdxHudCustomHit hit = GDX_HUDCUSTOM_HIT_NONE;
            for (i = 0; i < DASHW_COUNT; i++) {
                rects[i].x = sCustomWidgets[i].x;
                rects[i].y = sCustomWidgets[i].y;
                rects[i].w = sCustomWidgets[i].w;
                rects[i].h = sCustomWidgets[i].h;
                rects[i].enabled = DashWidgetEditable(i) ? sCustomWidgets[i].enabled : 0;
                if (DashWidgetResizable(i)) {
                    resizableMask |= (1u << (unsigned) i);
                }
            }
            if (gdx_hudcustom_hit_test_all(rects, DASHW_COUNT, resizableMask, touchPx, touchPy, &hitIndex, &hit)) {
                DashWidgetRect* r = &sCustomWidgets[hitIndex];
                sEditSelected = hitIndex;
                sEditDragging = 0;
                sEditResizing = 0;
                switch (hit) {
                    case GDX_HUDCUSTOM_HIT_CLOSE:
                        r->enabled = 0;
                        sEditSelected = -1;
                        break;
                    case GDX_HUDCUSTOM_HIT_RESIZE:
                        sEditResizing = 1;
                        break;
                    case GDX_HUDCUSTOM_HIT_BODY:
                    default:
                        sEditDragging = 1;
                        sEditDragOffX = touchPx - r->x;
                        sEditDragOffY = touchPy - r->y;
                        break;
                }
                return;
            }
        }
        sEditSelected = -1; /* empty space: deselect */
        return;
    }

    if (!touching) {
        sEditDragging = 0;
        sEditResizing = 0;
        return;
    }

    if (sEditMode && sEditSelected >= 0 && DashWidgetEditable(sEditSelected)) {
        DashWidgetRect* r = &sCustomWidgets[sEditSelected];
        if (sEditDragging) {
            GdxHudCustomRect rc;
            rc.x = gdx_hudcustom_snap(touchPx - sEditDragOffX, 8);
            rc.y = gdx_hudcustom_snap(touchPy - sEditDragOffY, 8);
            rc.w = r->w;
            rc.h = r->h;
            rc.enabled = 1;
            gdx_hudcustom_clamp_rect(&rc, DashWidgetMinW(sEditSelected), DashWidgetMinH(sEditSelected),
                                      DASH_HEADER_Y0);
            r->x = rc.x;
            r->y = rc.y;
        } else if (sEditResizing) {
            GdxHudCustomRect rc;
            const int step = DashWidgetResizeStep(sEditSelected);
            rc.x = r->x;
            rc.y = r->y;
            rc.w = gdx_hudcustom_snap(touchPx - r->x, step);
            rc.h = gdx_hudcustom_snap(touchPy - r->y, step);
            rc.enabled = 1;
            gdx_hudcustom_clamp_rect(&rc, DashWidgetMinW(sEditSelected), DashWidgetMinH(sEditSelected),
                                      DASH_HEADER_Y0);
            r->w = rc.w;
            r->h = rc.h;
        }
    }
}

/* ---- SHOT (same format/writer shape as gdx3ds_racemap.c's own, independent instance) --- */

static long DashWriteShot(const char* path, const uint16_t* fb) {
    const unsigned rowBytes = (DASH_FB_W * 3u + 3u) & ~3u;
    const uint32_t imageBytes = rowBytes * DASH_FB_H;
    const uint32_t fileBytes = 54u + imageBytes;
    static unsigned char sWriteBuf[8u * 1024u];
    unsigned char header[54];
    unsigned char row[DASH_FB_W * 3u + 3u];
    FILE* f;
    unsigned y;

    mkdir("autotest", 0777);
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    setvbuf(f, (char*) sWriteBuf, _IOFBF, sizeof(sWriteBuf));
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char) (fileBytes);
    header[3] = (unsigned char) (fileBytes >> 8);
    header[4] = (unsigned char) (fileBytes >> 16);
    header[5] = (unsigned char) (fileBytes >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char) (DASH_FB_W & 0xFF);
    header[19] = (unsigned char) (DASH_FB_W >> 8);
    header[22] = (unsigned char) (DASH_FB_H & 0xFF);
    header[23] = (unsigned char) (DASH_FB_H >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (y = 0; y < DASH_FB_H; y++) {
        const unsigned sy = DASH_FB_H - 1u - y;
        unsigned x;
        memset(row, 0, rowBytes);
        for (x = 0; x < DASH_FB_W; x++) {
            const uint16_t p = fb[(size_t) x * DASH_FB_H + (DASH_FB_H - 1u - sy)];
            const unsigned r = (p >> 11) & 31u;
            const unsigned g = (p >> 5) & 63u;
            const unsigned b = p & 31u;
            row[x * 3u + 0] = (unsigned char) ((b << 3) | (b >> 2));
            row[x * 3u + 1] = (unsigned char) ((g << 2) | (g >> 4));
            row[x * 3u + 2] = (unsigned char) ((r << 3) | (r >> 2));
        }
        fwrite(row, 1, rowBytes, f);
    }
    fclose(f);
    return (long) fileBytes;
}

static void DashServiceShot(const uint16_t* fb) {
    char path[80];
    long bytes;
    if (!__atomic_exchange_n(&sShotArmed, 0, __ATOMIC_ACQ_REL)) {
        return;
    }
    snprintf(path, sizeof(path), "autotest/%s_R.bmp", sPendingShot);
    bytes = DashWriteShot(path, fb);
    if (bytes < 0) {
        DashLog("[dash] SHOT write failed: %s", path);
    } else {
        DashLog("[dash] SHOT captured -> %s bytes=%ld", path, bytes);
    }
}

/* ---- the one call site: touch + paint, every tick the takeover is active -------------- */

void gdx3ds_bottomhud_tick(unsigned short* fbIn, unsigned int held, unsigned int prevHeld, int touching,
                            int prevTouching, int touchPx, int touchPy) {
    uint16_t* const realFb = (uint16_t*) fbIn; /* the live, on-screen, single-buffered console fb */
    uint16_t* fb = sShadowFb;                  /* every Dash* paint call below targets this instead
                                                 * -- see sShadowFb's own comment (hw bug n60-3d7 #1) */
    GdxRaceHudSnapshot snap;
    const u64 now = svcGetSystemTick();

    if (realFb == NULL) {
        return;
    }
    gdx_racehud_snapshot(&snap);
    if (!snap.valid) {
        if (!sEditMode) {
            return; /* the caller already checked race_live (or edit_active); nothing to show */
        }
        /* "Show sample values if no race is live": gdx3ds_menu.c's "HUDX" debug tab is the only
         * way sEditMode can be 1 here without a live race -- see gdx3ds_bottomhud_enter_custom_edit
         * and gdx3ds_bottomhud_edit_active's own takeover-gate OR in gdx3ds_menu.c. */
        DashBuildSampleSnapshot(&snap);
    }

    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        if (!sWasValid) {
            DashPaintBaked(fb);
            sWasValid = 1;
            sLastTop5Count = -1;
            sLastMapTick = 0;
        }
    } else if (!sCustomBackdropPainted) {
        /* CUSTOM mode never reuses the baked pixel art (DashPaintBaked) -- widgets here can be
         * resized to any size, which baked art cannot stretch to -- so it gets its own flat,
         * once-per-entry backdrop plus the one piece of baked-style chrome it still needs: the
         * DEBUG button, painted once here exactly like DashPaintBaked paints it once per race. */
        DashFillRect(fb, 0, 0, DASH_FB_W, DASH_FB_H, 0x18C3u);
        DashPaintBackButton(fb, NULL);
        sCustomBackdropPainted = 1;
    }

    /* MODE chip: a touch-down inside its rect, or a fresh KEY_SELECT, both cycle the mode --
     * EXACTLY as before this round, for every mode including CUSTOM (see sEditMode's own
     * comment for why CUSTOM's edit-mode entry deliberately does NOT share this gesture). The
     * one addition is `&& !sEditMode`: while actively editing a CUSTOM layout, the chip (and
     * SELECT) do nothing, so an accidental tap/press can never strand an in-progress drag/resize
     * unsaved by cycling the mode out from under it -- DONE (DashPaintCustomChrome's EDIT/DONE
     * toggle) is the only way out of edit mode itself. */
    {
        const int insideChip = touching && touchPx >= DASH_MODE_X0 && touchPx < DASH_MODE_X0 + DASH_MODE_W &&
                                touchPy >= DASH_MODE_Y0 && touchPy < DASH_MODE_Y0 + DASH_MODE_H;
        const int chipDown = insideChip && !sPrevChipTouch;
        const int selectDown = (held & KEY_SELECT) && !(prevHeld & KEY_SELECT);
        if ((chipDown || selectDown) && !sEditMode) {
            gdx3ds_bottomhud_cycle_mode();
        }
        sPrevChipTouch = insideChip;
    }
    (void) prevTouching;

    /* SELECT long-press -> dismiss, the key-based twin of the DEBUG button just below. Tracked
     * here (not inside the DEBUG-button block) so it still counts even while the finger is on the
     * chip/elsewhere; reset on release, and pushed past the threshold the instant it fires so
     * holding SELECT through the dismiss can't immediately re-fire. */
    if (held & KEY_SELECT) {
        if (sSelectHoldTicks <= DASH_SELECT_LONGPRESS_TICKS) {
            sSelectHoldTicks++;
        }
    } else {
        sSelectHoldTicks = 0;
    }

    /* COORDINATOR FIX (item 4): the DEBUG button (originally labelled "MENU"; renamed in the
     * idle-view round -- see DashPaintBackButton) -- a touch-down inside its rect dismisses
     * the DASH takeover back to the ordinary debug tab UI (gdx3ds_menu.c's gate checks
     * gdx3ds_bottomhud_dismissed() every tick); the race keeps running underneath. Nothing else
     * this tick matters once that happens -- the ordinary debug tab UI owns the next tick's whole
     * repaint.
     * COORDINATOR FIX (item 5): a SELECT long-press does the exact same thing -- see
     * sSelectHoldTicks above. */
    {
        const int insideBack = touching && touchPx >= DASH_BACK_X0 && touchPx < DASH_BACK_X0 + DASH_BACK_W &&
                                touchPy >= DASH_BACK_Y0 && touchPy < DASH_BACK_Y0 + DASH_BACK_H;
        const int backDown = insideBack && !sPrevBackTouch;
        const int selectLongpress = sSelectHoldTicks == DASH_SELECT_LONGPRESS_TICKS;
        sPrevBackTouch = insideBack;
        if (backDown || selectLongpress) {
            sDismissed = 1;
            sEditMode = 0; /* no autosave on the way out -- see DONE/RESET's own comments */
            DashLog("[dash] dismissed -> debug tabs");
            /* Flush before bailing: the `!sWasValid` block above may have just painted the
             * once-per-race baked backdrop into the shadow buffer on this very tick (race start
             * and a DEBUG-button tap landing on the same tick) -- without this, that backdrop
             * would sit in sShadowFb forever and never reach the live screen, since sWasValid is
             * now 1 and DashPaintBaked never runs again this race. */
            memcpy(realFb, fb, sizeof(sShadowFb));
            return;
        }
    }

    if (sPlacementMode == GDX_HUD_PLACEMENT_CUSTOM) {
        /* CUSTOM mode's own touch handling + paint, entirely separate from the modes-1-3 path
         * below (see the block comment above sCustomWidgets for why: no baked art, generic
         * resizable boxes instead). Touch is handled BEFORE painting so this same tick's paint
         * already reflects whatever a drag/resize/close/tray tap just changed. */
        int i;
        DashCustomHandleTouch(touching, prevTouching, touchPx, touchPy);
        DashPaintHeaderLive(fb, &snap);
        DashPaintModeLabel(fb, &snap); /* repaints the chip's interior -- the dot must follow it */
        DashPaintModeDot(fb);
        DashPaintCustomChrome(fb);
        for (i = 0; i < DASHW_COUNT; i++) {
            if (!DashWidgetEditable(i)) {
                continue;
            }
            DashPaintCustomWidget(fb, i, &snap, sEditMode && sEditSelected == i);
        }
        if (sEditMode) {
            for (i = 0; i < DASHW_COUNT; i++) {
                if (!DashWidgetEditable(i)) {
                    continue;
                }
                DashPaintEditHandles(fb, i);
            }
            DashPaintTray(fb);
        }
    } else {
        /* COORDINATOR FIX (custom-HUD groundwork, follow-up round): "convert tick() to loop the
         * widget table in a defined paint order, re-encoding the ordering/throttle couplings
         * explicitly." kDashPaintOrder (declared near sDashWidgets, with the full rationale) is
         * that order; each widget's own paint fn already decides internally whether it has
         * anything new to draw (dirty-checking for standings, the [debug] bottom_race_hz
         * throttle for the map box) -- exactly as cheap as the old hand-written call sequence
         * this loop replaces, and calling the exact same functions in the exact same order
         * produces byte-identical output for modes 1-3. The one coupling with no row of its own
         * in the table -- the MODE dot, which must follow DashPaintModeLabel's own repaint of the
         * chip's interior -- is re-encoded explicitly right here, not hidden inside the loop. */
        unsigned oi;
        for (oi = 0; oi < (unsigned) DASH_PAINT_ORDER_COUNT; oi++) {
            const DashWidgetId id = kDashPaintOrder[oi];
            sDashWidgets[id].paint(fb, &snap);
            if (id == DASHW_MODE_CHIP) {
                DashPaintModeDot(fb);
            }
        }
    }

    /* The ONE write to the live, on-screen buffer this tick: the shadow buffer is now a
     * complete, consistent frame (every widget above painted into it, never into realFb
     * directly), so this single contiguous copy is the only moment the live buffer changes --
     * see sShadowFb's own comment (hw bug n60-3d7 #1) for why that's what kills the tearing.
     * Deliberately included inside the self-timing window below: it is real per-tick cost this
     * fix adds and the avg/max_us receipt should say so honestly. */
    memcpy(realFb, fb, sizeof(sShadowFb));

    /* Self-timing window CLOSES HERE, before the SHOT write — see the sTicks/sTickTicksTotal
       comment above. */
    {
        const u64 dt = svcGetSystemTick() - now;
        sTicks++;
        sTickTicksTotal += dt;
        if (dt > sTickTicksMax) {
            sTickTicksMax = dt;
        }
        if ((sTicks % 256u) == 0u) {
            const unsigned long ticksPerMs = (unsigned long) CPU_TICKS_PER_MSEC;
            const unsigned long avgUs = (unsigned long) (((sTickTicksTotal / 256u) * 1000u) / ticksPerMs);
            const unsigned long maxUs = (unsigned long) ((sTickTicksMax * 1000u) / ticksPerMs);
            DashLog("[dash] ticks=%lu avg_us=%lu max_us=%lu", sTicks, avgUs, maxUs);
            sTickTicksTotal = 0;
            sTickTicksMax = 0;
        }
    }

    DashServiceShot(fb);
}

/* Called once, from gdx3ds_menu_tick_body, right after a race ends while the DASH owned the
 * screen: clears the once-per-race latch and flushes a deferred mode-cycle save (the race's own
 * valid 1->0 transition is the safe point gdx3ds_bottomhud.h promises). gdx3ds_menu.c detects
 * the transition itself (gdx3ds_bottomhud_race_live going 0) and repaints its own UI from
 * scratch; this function only resets the DASH's latches so the NEXT race gets a fresh backdrop. */
void gdx3ds_bottomhud_on_race_ended(void) {
    sWasValid = 0;
    sCustomBackdropPainted = 0;
    sLastTop5Count = -1;
    sLastHeaderRacersRemaining = -1;
    sLastHeaderTrackName[0] = '\0';
    sDismissed = 0; /* the next race always starts in the DASH, not the debug tabs */
    /* The idle view becomes reachable again the instant this race ends (if not itself dismissed)
     * -- its own backdrop latch must repaint fresh rather than trust whatever pixels the just-
     * ended race DASH (or CUSTOM edit mode) left in the live framebuffer. sIdleDismissed is left
     * alone on purpose: a user who was already looking at the debug console before/during this
     * race stays there after it ends too, same as the race DASH's own TAB_RACE return path never
     * force-navigates the console UI either. */
    sIdleBaked = 0;
    gdx3ds_bottomhud_flush_pending_save();
}

int gdx3ds_bottomhud_dismissed(void) {
    return sDismissed;
}

void gdx3ds_bottomhud_resume(void) {
    sDismissed = 0;
}

int gdx3ds_bottomhud_edit_active(void) {
    return sEditMode;
}

/* ---- IDLE VIEW: public API -- see gdx3ds_bottomhud.h's own doc comment for the full picture. */

int gdx3ds_bottomhud_idle_enabled(void) {
    return sIdleEnabled;
}

/* gdx3ds_menu_tick_body's own takeover-gate predicate for the idle view, checked only once the
 * race-live/edit-active takeover does not apply. bottom_race=0 (sEnabled) disables DASH-style
 * rendering everywhere, not just mid-race -- see gdx3ds_bottomhud.h's own note on why idle
 * requires BOTH levers, not bottom_idle alone. */
int gdx3ds_bottomhud_idle_active(void) {
    return sEnabled && sIdleEnabled && !sEditMode && !sIdleDismissed && !gdx3ds_bottomhud_race_live();
}

int gdx3ds_bottomhud_idle_dismissed(void) {
    return sIdleDismissed;
}

void gdx3ds_bottomhud_idle_resume(void) {
    sIdleDismissed = 0;
    sIdleBaked = 0; /* force a fresh repaint -- the live buffer holds the console UI right now */
}

/* One call per tick while gdx3ds_bottomhud_idle_active() is 1 -- same calling convention as
 * gdx3ds_bottomhud_tick, same off-screen sShadowFb (the two never run in the same tick, so
 * sharing the one buffer costs nothing extra). Unlike the race DASH, the idle view has no live
 * fields at all: DashPaintIdleBaked runs exactly ONCE per entry (the `!sIdleBaked` edge below),
 * and the shadow-to-live copy runs only on that same tick -- every later tick while idle stays up
 * does nothing but the DEBUG-button touch test, no framebuffer writes at all. This is the
 * "redraw rarely (static image; only redraw on change)" requirement taken literally: there is
 * nothing to change until the DEBUG button is tapped, so nothing redraws until then. */
void gdx3ds_bottomhud_idle_tick(unsigned short* fbIn, unsigned int held, unsigned int prevHeld, int touching,
                                 int prevTouching, int touchPx, int touchPy) {
    uint16_t* const realFb = (uint16_t*) fbIn;
    uint16_t* const fb = sShadowFb;
    int insideBack;
    int backDown;
    int selectLongpress;
    (void) prevHeld;
    (void) prevTouching;
    if (realFb == NULL) {
        return;
    }
    if (!sIdleBaked) {
        DashPaintIdleBaked(fb);
        memcpy(realFb, fb, sizeof(sShadowFb));
        sIdleBaked = 1;
    }

    /* SELECT long-press, the idle view's own twin of the race DASH's DEBUG-button dismiss
     * gesture -- same threshold constant, same feel. */
    if (held & KEY_SELECT) {
        if (sIdleSelectHoldTicks <= DASH_SELECT_LONGPRESS_TICKS) {
            sIdleSelectHoldTicks++;
        }
    } else {
        sIdleSelectHoldTicks = 0;
    }

    insideBack = touching && touchPx >= DASH_BACK_X0 && touchPx < DASH_BACK_X0 + DASH_BACK_W &&
                 touchPy >= DASH_BACK_Y0 && touchPy < DASH_BACK_Y0 + DASH_BACK_H;
    backDown = insideBack && !sIdlePrevBackTouch;
    selectLongpress = sIdleSelectHoldTicks == DASH_SELECT_LONGPRESS_TICKS;
    sIdlePrevBackTouch = insideBack;
    if (backDown || selectLongpress) {
        sIdleDismissed = 1;
        DashLog("[dash] idle dismissed -> debug tabs");
        return; /* nothing left to paint -- the console UI owns the next tick's whole repaint */
    }

    DashServiceShot(fb); /* same armed-shot mechanism the race DASH uses, for idle-view captures */
}

void gdx3ds_bottomhud_enter_custom_edit(void) {
    /* SHELF defense in depth: gdx3ds_menu.c's DBG tab already hides/refuses the "HUD EDIT" row
     * whenever hud_custom=0 (gdx3ds_bottomhud_custom_enabled), so this should be unreachable with
     * the lever off -- but refuse here too rather than trust the caller alone. */
    if (!HudCustomLeverEnabled()) {
        DashLog("[dash] custom edit refused: [debug] hud_custom=0 (CUSTOM shelved)");
        return;
    }
    if (sPlacementMode != GDX_HUD_PLACEMENT_CUSTOM) {
        const int inRace = (&gdx_input_in_gameplay != NULL) && gdx_input_in_gameplay();
        sPlacementMode = GDX_HUD_PLACEMENT_CUSTOM;
        sCustomBackdropPainted = 0;
        gdx3ds_config_set_int("hud", "placement", sPlacementMode);
        DashSyncCustomTopMask();
        /* Same save-now-or-defer rule as gdx3ds_bottomhud_cycle_mode's own switch into CUSTOM --
         * see that function's comment. */
        if (inRace) {
            sPlacementSaveDirty = 1;
        } else {
            DashHudIniSave();
        }
    }
    sDismissed = 0; /* the takeover gate's edit_active() clause needs this 0 to actually run */
    sEditSelected = -1;
    sEditMode = 1;
    DashLog("[dash] custom edit -> begin (via debug menu)");
}
