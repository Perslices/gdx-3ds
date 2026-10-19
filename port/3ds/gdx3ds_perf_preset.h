/* port/3ds/gdx3ds_perf_preset.h — [perf] preset: one key that re-defaults the scene-density
 * levers for a console class (NEXT.md C4p, docs/research/native/o3dslod-progress.md).
 *
 * The Old 3DS runs a 30-machine GP race at ~68 ms/frame and a 1-machine time trial at
 * ~40 ms (hw-c4run2-0929), and every [perf] default was tuned on the New 3DS. A preset does
 * NOT add a code path: it changes the DEFAULT of existing [perf] keys, and an explicit key
 * in gdiffuser.ini always wins over it. So `rival_detail = 0` next to `preset = o3ds` means
 * "the O3DS preset, but full rival detail", and every preset lever can be A/B'd by writing
 * its key.
 *
 *   [perf] preset = auto   (DEFAULT) -> o3ds on an Old 3DS / 2DS, off on a New 3DS
 *                  off | n3ds        -> every [perf] default is the shipped New 3DS one
 *                  o3ds              -> the Old 3DS preset whatever the console
 *                  n60 | n3ds60      -> every [perf] LOD default stock (full detail); the n60 race
 *                                       stack's levers come from the config preset layer
 *                                       (gdx3ds_preset_n60.h, n3ds-progress.md)
 *
 * Every lever in the preset is VISUAL ONLY (which LOD a racer is drawn with, how far the
 * track is drawn): no game state reads any of them. Receipt, once at first use:
 *   [perf] preset=o3ds (auto: model=old3ds) rival_detail=.. rival_tier_max=.. hi=.. lo=..
 *          draw_distance=.. lod6_skip=..
 */
#ifndef GDX3DS_PERF_PRESET_H
#define GDX3DS_PERF_PRESET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Latch [perf] preset and the console model, print the receipt. Idempotent; every getter
 * calls it lazily, so it needs no call site of its own, but it must run after
 * gdx3ds_config_load. */
void gdx3ds_perf_preset_init(void);

/* 1 when the Old 3DS preset is in force. */
int gdx3ds_perf_preset_o3ds(void);

/* [perf] <key> with a preset-aware default: the ini value if the key is present, else the
 * preset's value for it (when a preset is active and carries the key), else stockDefault. */
int gdx3ds_perf_get_int(const char* key, int stockDefault);

/* Track draw distance in percent of the stock far-cull distance, for the decomp hook
 * (decomp-port-o3ds-scene-lod.patch, Course_Draw). 100 = stock; clamped 40..100. */
int gdx_track_draw_distance_pct(void);

/* LOD-6 emission for the decomp hook (decomp-port-o3ds-scene-lod.patch), [perf] lod6_skip,
 * default 0 = stock. 1 = drop the texture-load list in front of a machine at LOD 6 (the far
 * speck) and keep the speck; 2 = drop the speck too. */
int gdx_rival_lod6_skip(void);

#ifdef GDX3DS_PERF_PRESET_HOST_TEST
/* Host-test seam: forget the latch so the next getter re-reads config and model. */
void gdx3ds_perf_preset_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PERF_PRESET_H */
