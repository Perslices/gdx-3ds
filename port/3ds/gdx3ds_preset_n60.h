/* port/3ds/gdx3ds_preset_n60.h — built-in lever presets for the config's preset layer.
 *
 * `[perf] preset = n60` (alias `n3ds60`) re-defaults the n60 race stack's levers (NRR, stereo
 * replay, end-mode pacing, race0 frame skip, ...) for the New 3DS; see gdx3ds_preset_n60.c for
 * the table and its rationale. Registered with gdx3ds_config_set_preset_resolver before the first
 * gdx3ds_config_load (main_3ds.cpp). Any other preset name (auto, off, o3ds, n3ds) resolves to no
 * table, so every lookup is exactly the shipped one.
 */
#ifndef GDX3DS_PRESET_N60_H
#define GDX3DS_PRESET_N60_H

#include "gdx3ds_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gdx3dsCfgPresetResolver: the table for `name`, or NULL (count 0). */
const Gdx3dsCfgPresetEntry* gdx3ds_preset_resolve(const char* name, int* count);
/* 1 when `name` selects the n60 preset ("n60" / "n3ds60", case-insensitive). */
int gdx3ds_preset_n60_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PRESET_N60_H */
