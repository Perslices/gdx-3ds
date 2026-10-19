/* port/gdx_hud_placement.h -- the HUD placement lever (nibottom Phase 2).
 *
 * Three positions, cycled by the bottom-screen MODE chip + a free key (KEY_SELECT on 3DS):
 *
 *   ALL_TOP     (default) -- stock top-screen HUD, bottom screen whatever bottom_race/bottom_map
 *                             draw there. Byte-identical to a build with the lever compiled out.
 *   SPLIT                 -- Hud_DrawHud + Hud_DrawRacePortraits suppressed on the top screen
 *                             (they move to the bottom DASH); the minimap stays on top.
 *   ALL_BOTTOM             -- the two Minimap_DrawCourseMinimap calls ALSO suppressed; the whole
 *                             race HUD lives on the bottom screen.
 *
 * FORCED ALL_TOP IN 2-4P: menus.c's Hud_DrawHud/Hud_DrawRacePortraits/minimap call sites are
 * inside a `for (i = 0; i < gNumPlayers; i++)` loop that draws every player's own HUD in its own
 * viewport quadrant; there is no single "the" HUD to relocate, and FIELD standings (every racer,
 * not just player 1) do not fit the 1-player DASH layout this phase ships. gdx_hud_top_suppressed/
 * gdx_hud_minimap_suppressed both force the 1P predicate's answer to "not suppressed" whenever
 * numPlayers > 1, so this header's mode accessors are the ONLY thing 2-4P code needs to check --
 * callers never have to re-derive the numPlayers>1 override themselves.
 *
 * Cross-platform (included by port/input_bridge.c, which both the PC and 3DS CMakeLists compile):
 * off/ALL_TOP everywhere except the 3DS build, where port/3ds/gdx3ds_bottomhud.c owns the actual
 * ini-backed lever. Plain ints, no decomp types -- the decomp-side call site in menus.c declares
 * the two suppression predicates inline (`extern int gdx_hud_top_suppressed(int); // port/
 * input_bridge.c`) rather than #including this header, per the project's existing #ifdef PORT
 * inline-extern idiom; this header exists for the port-side C files that share the enum.
 */
#ifndef GDX_HUD_PLACEMENT_H
#define GDX_HUD_PLACEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_HUD_PLACEMENT_ALL_TOP = 0,
    GDX_HUD_PLACEMENT_SPLIT = 1,
    GDX_HUD_PLACEMENT_ALL_BOTTOM = 2,
    GDX_HUD_PLACEMENT_MODE_COUNT = 3,
    /* nihudtop Phase 3: a 4th position, BEYOND the chip's 0..MODE_COUNT-1 cycle on purpose --
     * MODE_COUNT must stay 3 (gdx3ds_bottomhud.c's chip cycle/colour table is sized and indexed
     * by it) until the chip itself grows a 4th notch, which is a later change. Reaching CUSTOM
     * today is NOT through gdx_hud_placement_mode(); it is the separate
     * gdx_hud_set_custom_top_mask() override below (driven for now only by a dev ini lever --
     * see port/3ds/gdx3ds_hudtop_custom.c), which gdx_hud_element_on_top consults before it ever
     * looks at the mode number. GDX_HUD_PLACEMENT_CUSTOM exists so callers/tests have a symbolic
     * name for "the mask decides," not because any mode accessor returns it yet. */
    GDX_HUD_PLACEMENT_CUSTOM = 3
};

/* The individual top-screen HUD pieces Hud_DrawHud/Hud_DrawRacePortraits/Hud_DrawPosition/
 * Hud_DrawPlayerSpeed/Minimap_DrawCourseMinimap draw for 1P (docs/research/native/
 * nihudtop-progress.md has the full call-site table). ALL_TOP/SPLIT/ALL_BOTTOM answer the same
 * way for every one of these except MINIMAP (today's all-or-nothing suppression); CUSTOM is the
 * first mode where they can differ from each other. */
enum {
    GDX_HUD_ELEM_TIME = 0,    /* race/lap clock digits + death-race timer+best (hud.c Hud_DrawHud
                                * case 1: Hud_DrawTimeRectangle, Hud_UpdatePlayerHudInfo's digit
                                * draw via Hud_DrawPlayerTimer, Hud_DrawDeathRaceBest/Timer/
                                * BestTime) */
    GDX_HUD_ELEM_LAP,         /* "LAP x/y" icon + counter (Hud_DrawLapRectangle,
                                * Hud_DrawLapCounter) */
    GDX_HUD_ELEM_INTERVAL,    /* gap-to-rival + practice best-lap/delta (Hud_UpdateRaceIntervalInfo,
                                * Hud_DrawPracticeBestLap, Hud_DrawPracticeLapDelta) */
    GDX_HUD_ELEM_ENERGY,      /* energy meter: outline + fill bar, red/green boost-ready tint
                                * (Hud_DrawEnergyOutlineRectangle, Hud_DrawEnergyBar) */
    GDX_HUD_ELEM_RANK,        /* position ordinal, e.g. "1ST" (menus.c Hud_DrawPosition call) */
    GDX_HUD_ELEM_SPEED,       /* kph speedometer (menus.c Hud_DrawPlayerSpeed call) */
    GDX_HUD_ELEM_KO_STARS,    /* elimination star counter (Hud_DrawKOStars) */
    GDX_HUD_ELEM_LIVES,       /* Death Race remaining-lives readout (Hud_DrawPlayerLives) */
    GDX_HUD_ELEM_REVERSE,     /* reverse-gear icon (Hud_DrawReverse) */
    GDX_HUD_ELEM_PORTRAITS,   /* standings portrait strip (menus.c Hud_DrawRacePortraits call) */
    GDX_HUD_ELEM_MINIMAP,     /* course minimap (menus.c Minimap_DrawCourseMinimap calls; already
                                * independently gated pre-Phase-3 via gdx_hud_minimap_suppressed,
                                * now just this bit) */
    GDX_HUD_ELEM_COUNT
};

/* The raw configured mode (0..2), NOT forced for player count -- for the MODE chip's own label
 * and for gdx3ds_bottomhud_paint, which needs to know what was actually asked for even in 2-4P
 * (where it still draws the FIELD standings lever-independently). */
int gdx_hud_placement_mode(void);

/* 1 if Hud_DrawHud + Hud_DrawRacePortraits (menus.c:5650/5662) must be skipped this frame. */
int gdx_hud_top_suppressed(int numPlayers);

/* 1 if both Minimap_DrawCourseMinimap calls (menus.c:5677/5689) must be skipped this frame. */
int gdx_hud_minimap_suppressed(int numPlayers);

/* 1 if GDX_HUD_ELEM_* `element` may draw on the TOP screen this frame for `numPlayers`. The one
 * predicate every per-element call site checks. For modes 0-2 this reproduces today's
 * all-or-nothing answers exactly: every element but MINIMAP shares gdx_hud_top_suppressed's
 * answer, MINIMAP alone follows gdx_hud_minimap_suppressed (both of those are implemented on top
 * of this function -- see port/input_bridge.c). Numbers>1 always force "every element on top"
 * (gdx_hud_placement.h's FORCED ALL_TOP IN 2-4P rule), CUSTOM mask or no. */
int gdx_hud_element_on_top(int element, int numPlayers);

/* Hot-path helper: every GDX_HUD_ELEM_* this frame's `numPlayers` allows on top, bit i == element
 * i, computed once and then tested with a plain `(mask >> element) & 1u` at each call site --
 * gdx_hud_element_on_top is defined in terms of this and exists for callers (host tests, the two
 * legacy predicates) that want a single yes/no without managing the mask themselves. */
unsigned gdx_hud_top_element_mask(int numPlayers);

/* Dev-only override hook: while `customActive` is nonzero (and numPlayers<=1 -- CUSTOM has no
 * multiplayer layout, see FORCED ALL_TOP IN 2-4P), gdx_hud_top_element_mask returns `mask`
 * verbatim instead of deriving it from gdx_hud_placement_mode(). Nothing calls this yet except a
 * dev ini lever (port/3ds/gdx3ds_hudtop_custom.c, [debug] hud_custom_top_mask/hud_custom_active);
 * the real CUSTOM mode (a 4th MODE-chip notch feeding a per-element UI) is future work. */
void gdx_hud_set_custom_top_mask(unsigned mask, int customActive);

/* "ALL_TOP" / "SPLIT" / "ALL_BOTTOM", for the MODE chip label. NULL/out-of-range -> "ALL_TOP". */
const char* gdx_hud_placement_name(int mode);

#ifdef __cplusplus
}
#endif

#endif /* GDX_HUD_PLACEMENT_H */
