/* port/3ds/gdx3ds_preset_n60.h — built-in lever presets for the config's preset layer.
 *
 * `[perf] preset = n60` (alias `n3ds60`) re-defaults the n60 race stack's levers (NRR, stereo
 * replay, end-mode pacing, race0 frame skip, ...) for the New 3DS; see gdx3ds_preset_n60.c for
 * the table and its rationale. Registered with gdx3ds_config_set_preset_resolver before the first
 * gdx3ds_config_load (main_3ds.cpp). Any other preset name (auto, off, o3ds, n3ds) resolves to no
 * table, so every lookup is exactly the shipped one -- EXCEPT the internal "o3ds-auto" name
 * below, which main_3ds.cpp feeds through gdx3ds_config_set_auto_preset_name, never typed by a
 * player.
 *
 * n60lead: a no-ini player must not be left on the untuned shipped defaults ("[perf] preset"
 * default is "auto"). gdx3ds_perf_preset.c's OWN auto (the [perf] LOD-only o3ds/n60 split) is
 * unchanged; THIS file additionally carries the n60/NRR/host-loop lever table auto needs on
 * each model:
 *   - New 3DS auto -> "n60" (gdx3ds_preset_resolve("n60", ...) == kN60Preset, identical to an
 *     explicit `preset = n60`).
 *   - Old 3DS auto -> "o3ds-auto" (kO3dsAutoPreset below): n60's own table is already correct
 *     for almost every lever (its header says n60's values came from an Old 3DS card), so this
 *     is n60's table minus the New-3DS-only tex_keep_kb headroom, plus the three Old-3DS-only
 *     levers the hardware-verified n60-3d15 card ini carries that n60 does not: audio_stream,
 *     tex_upload_kb (memory-fit; n60's own header names both as "the Old 3DS memory-fit levers
 *     ... dropped" when it built the New 3DS table) and syscore_pct_safe (the OS-starvation
 *     safety clamp). Everything else n60-3d15 sets beyond that is either identical to n60's
 *     table already, identical to its OWN compiled-in default already (so the key is
 *     redundant), diagnostic/telemetry, or a user preference -- none of that belongs in a
 *     performance preset; see the commit message for the full key-by-key classification.
 */
#ifndef GDX3DS_PRESET_N60_H
#define GDX3DS_PRESET_N60_H

#include "gdx3ds_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gdx3dsCfgPresetResolver: the table for `name` ("n60"/"n3ds60" or the internal "o3ds-auto"),
 * or NULL (count 0) for anything else, including "auto", "o3ds", "off" and NULL. */
const Gdx3dsCfgPresetEntry* gdx3ds_preset_resolve(const char* name, int* count);
/* 1 when `name` selects the n60 preset ("n60" / "n3ds60", case-insensitive). */
int gdx3ds_preset_n60_name(const char* name);
/* 1 when `name` selects the Old 3DS auto table ("o3ds-auto", case-insensitive; internal --
 * main_3ds.cpp feeds this through gdx3ds_config_set_auto_preset_name, it is not a documented
 * ini value, though typing it by hand resolves the same table). */
int gdx3ds_preset_o3ds_auto_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PRESET_N60_H */
