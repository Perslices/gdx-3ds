/* gdx3ds_hud_record.h -- [native] hud_batch (C5 Stage 2): the per-GfxPool record shared by the
 * game-side HUD capture (port/3ds/game/gdx3ds_hud_capture.c, writer) and the owned
 * interpreter's replay (port/3ds/gfx/n64/gdx_hud_replay.cpp, reader).
 *
 * The capture does not redefine a single GBI macro. The HUD draw functions (Menus_Draw, the
 * finish screens, Racer_Draw's marker/countdown tail) run exactly as before and write their
 * display list as always; afterwards gdxhud_capture() COMPACTS the words they wrote, in place:
 * every maximal run of "RDP state + texture + rectangle" commands (the recordable set, see
 * gdx3ds_hud_capture.c) is copied into this record and replaced by ONE G_NOOP marker; every
 * other command (G_DL, matrices, vertices, triangles, other markers) stays where it was. The
 * walker then meets [..., DL, marker, DL, marker, ...] and the replay executes each run at its
 * marker through the interpreter's own handlers -- the same commands, in the same order, at
 * the same position, so the frame is the stock frame by construction. What the replay saves
 * is the walker's per-command cost and, per texture load, the source resolution and the
 * immutable-texture content hash (memoised, gdx_hud_replay.cpp).
 *
 * Same double-buffer contract as the fx/track records: the game fills pool N while the render
 * side may replay pool N^1 (renderthread=1); on the Old 3DS both happen in one iteration. */
#ifndef GDX3DS_HUD_RECORD_H
#define GDX3DS_HUD_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXHUD_MAGIC 0x48554431u /* "HUD1" */
/* Race steady ~350 recordable words per frame, the results screens ~1,500 (250 rects);
 * 4096 leaves room for the 4-player HUD. Overflow aborts the capture of that region
 * (the region stays stock) and is counted. */
#define GDXHUD_MAX_WORDS 4096
#define GDXHUD_MAX_RUNS 1024

/* G_NOOP marker: w0 = (G_NOOP<<24) | (p<<16) | runIndex, w1 = GdxHudRecord*. Track owns
 * 0x5A/0x5B, fx 0x5C/0x5D, the c5cat scene markers 0x61; the design reserved 0x5E for the
 * HUD and 0x5F for the shadows (o3ds60-design.md section 5). The boost-flame region rides 0x5E
 * too: the run carries its kind. */
#define GDXHUD_MARKER_P 0x5E
#define GDXHUD_MARKER_P_SHADOW 0x5F

/* The three captured regions, one lever each ([native] hud_batch / shadow_batch / fx_batch). */
enum {
    GDXHUD_KIND_HUD = 0,    /* Racer_Draw's marker/countdown tail, Menus_Player1SpecialDraw, Menus_Draw */
    GDXHUD_KIND_SHADOW = 1, /* Racer_Draw's shadow decals (D_303A758 .. the machine pass) */
    GDXHUD_KIND_FX = 2,     /* Racer_Draw's boost flames (aSetupBoosterDL .. the fx shim bracket) */
    GDXHUD_KIND_MENU = 3,   /* nimenu [native] ni_menu: a non-race mode's whole draw (gdxmenu_capture,
                               gdx3ds_hud_capture.c -- folded in from its own TU in W5) */
    GDXHUD_KIND_COUNT = 4
};

/* One display-list word exactly as the 32-bit PORT lays out a Gfx (16 bytes: w0, pad, the
 * 64-bit w1 whose high half is the host-pointer tag) -- the owned walker's "wide" list. */
typedef struct GdxHudWord {
    uint32_t w0;
    uint32_t pad;
    uint32_t w1;
    uint32_t w1hi;
} GdxHudWord;

typedef struct GdxHudRun {
    uint16_t start;
    uint16_t end;
    uint8_t kind; /* GDXHUD_KIND_* */
    uint8_t mode; /* that region's lever: 1 on, 2 verify */
    uint8_t group; /* n60results hud_group: on a marker's run, the runs its marker replays (1 = itself); 0 on
                    * the runs after it in the group (no marker of their own) */
    uint8_t pad;
} GdxHudRun;

/* Written by the replay (render side), read back by the capture on that pool's next frame
 * for the [hud] receipt. */
typedef struct GdxHudReplayStats {
    uint32_t markers;   /* runs replayed */
    uint32_t words;     /* commands executed by the replay */
    uint32_t rects;     /* TEXRECT + FILLRECT among them */
    uint32_t loads;     /* texture loads (LOADBLOCK/LOADTILE) */
    uint32_t srcHit;    /* loads whose source resolution came from the memo */
    uint32_t srcMiss;   /* immutable loads resolved (and memoised) */
    uint32_t srcInel;   /* loads the memo does not cover (RDRAM/dynamic sources) */
    uint32_t hashHit;   /* immutable imports whose content hash came from the memo */
    uint32_t hashMiss;
    uint32_t verifyOk;  /* hud_batch=2: memo answers checked against a fresh computation */
    uint32_t verifyBad;
    uint32_t badRecord; /* markers whose record/run failed validation (nothing replayed) */
    uint32_t fastTris;  /* triangles (rect or not) that took the tri memo (no ResolveDrawState) */
} GdxHudReplayStats;

typedef struct GdxHudRecord {
    uint32_t magic;
    uint32_t frame; /* gGameFrameCount the record belongs to */
    uint32_t nWords;
    uint32_t nRuns;
    GdxHudWord words[GDXHUD_MAX_WORDS];
    GdxHudRun runs[GDXHUD_MAX_RUNS];
    GdxHudReplayStats rp[GDXHUD_KIND_COUNT];
} GdxHudRecord;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HUD_RECORD_H */
