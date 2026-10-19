/* gdx3ds_ships.h -- N60SHIPS read-only receipts for the per-racer cost (n60ships-progress.md §6 rank 0).
 *
 * [debug] ships_census = 1 (default 0): Racer_Draw's visibility pass is counted after it ran (every
 *   machineLod / unk_2B2 / unk_2B3 / highlight / crashed / ghost of the call) and averaged per call:
 *   `[ships] frame=N calls= md= np= tot= | act= vis= l1= .. l6= | shd= boost= near= hl= crash= ghost=`
 *   (act = active racers, vis = machineLod != 0, shd = shadow drawn, boost = flames drawn
 *   (unk_2B3 != 0, not crashed / spinning), near = unk_2B3 == 2 (flame trail), hl = highlight pass).
 * [debug] logic_split = 1 (default 0): Race_Update (the GP / time-trial / practice update) is split
 *   into a flat timeline of buckets, ms per race tick:
 *   `[lsplit] frame=N ticks= | menus= efx= rpre= coll= ai= phys= rpost= cam= bg= crs= oth= | sum= reads=`
 *   menus Menus_Update, efx Effects_Update, rpre Racer_Update up to the pair-collision loop, coll the
 *   pair-collision loop, ai Cpu_GenerateInputs (CPU racers), phys Racer_UpdateFromControls (all
 *   racers), rpost the rest of Racer_Update (race positions, pair info, ghosts), cam Camera_Update,
 *   bg Background_Update, crs Course_Update, oth func_80074844.
 *
 * Both are diagnostic only and never write game state: counters and svcGetSystemTick reads, emitted
 * from the host loop (gdx3ds_mprof_frame_end), never from a game fiber. With the [c5cat] clock armed
 * (gputrace / mainprof / c5pmu) the lines share its window (same frame=); otherwise they keep their
 * own 64-iteration window. Off, every hook is one int load + branch. Decomp hooks:
 * decomp-3ds-ships-census.patch (racer.c, ovl_i2/race.c). */
#ifndef GDX3DS_SHIPS_H
#define GDX3DS_SHIPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_LSPLIT_NONE = -1,
    GDX3DS_LSPLIT_MENUS = 0,
    GDX3DS_LSPLIT_EFX,
    GDX3DS_LSPLIT_RPRE,
    GDX3DS_LSPLIT_COLL,
    GDX3DS_LSPLIT_AI,
    GDX3DS_LSPLIT_PHYS,
    GDX3DS_LSPLIT_RPOST,
    GDX3DS_LSPLIT_CAM,
    GDX3DS_LSPLIT_BG,
    GDX3DS_LSPLIT_CRS,
    GDX3DS_LSPLIT_OTH,
    GDX3DS_LSPLIT_COUNT
};

/* Census slots, one Racer_Draw call. */
enum {
    GDX3DS_SHIPS_ACT = 0,
    GDX3DS_SHIPS_VIS,
    GDX3DS_SHIPS_L1, /* L1 .. L6 consecutive */
    GDX3DS_SHIPS_L6 = GDX3DS_SHIPS_L1 + 5,
    GDX3DS_SHIPS_SHD,
    GDX3DS_SHIPS_BOOST,
    GDX3DS_SHIPS_NEAR,
    GDX3DS_SHIPS_HL,
    GDX3DS_SHIPS_CRASH,
    GDX3DS_SHIPS_GHOST,
    GDX3DS_SHIPS_COUNT
};

extern int gdx3ds_ships_census_on;
extern int gdx3ds_lsplit_on;

/* Time-sliced card A/B ([debug] nrr_ab = mask, nrr_ab_period = N host iterations, default 0 = off):
 * a lever named in the mask is OFF in even blocks of N iterations and ON in odd ones, so one ini
 * on one SD trip carries both legs. Mask bit 0: [native] nrr_mach (the BODY consume stub declines
 * in off blocks: the section builds the stock way, game state identical by construction); bit 1:
 * [native] nrr_speck_batch. The phase changes only between host iterations. Receipt
 * `[ab] frame=N phase= mask= period=` once per [c5cat] / own window. */
extern int gdx3ds_nrr_ab_mask;
extern int gdx3ds_nrr_ab_phase; /* 1 = the masked levers are on in this block */
static inline int gdx3ds_nrr_ab_on(int bit) {
    return (gdx3ds_nrr_ab_mask & bit) == 0 || gdx3ds_nrr_ab_phase != 0;
}

/* Game fiber (Racer_Draw): one call's counts, plus the mode it ran in. */
void gdx3ds_ships_census_add(const uint8_t* counts, int gameMode, int numPlayers, int totalRacers);

/* Game fiber (Race_Update): begin opens bucket MENUS, mark switches buckets, end closes. Marks
 * outside a begin/end pair (Racer_Update from the records / ending overlays) are ignored. */
void gdx3ds_lsplit_begin(void);
void gdx3ds_lsplit_mark_slow(int bucket);
void gdx3ds_lsplit_end(void);
#define GDX3DS_LSPLIT_MARK(b)               \
    do {                                    \
        if (gdx3ds_lsplit_on) {             \
            gdx3ds_lsplit_mark_slow(b);     \
        }                                   \
    } while (0)

/* Host loop (gdx3ds_mprof_frame_end): latch the levers once the config is in; with the [c5cat]
 * clock off, count the window and emit at 64. gdx3ds_ships_emit: the [c5cat] window's emit. */
void gdx3ds_ships_frame_end(unsigned long frame, int c5catArmed);
void gdx3ds_ships_emit(unsigned long frame);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_SHIPS_H */
