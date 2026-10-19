/* port/3ds/gdx3ds_preset_n60.c — `[perf] preset = n60`: the n60 race stack as built-in defaults.
 *
 * Contract: gdx3ds_preset_n60.h. Rationale and gates: docs/research/native/n3ds-progress.md.
 *
 * The Old 3DS reached a 60 fps race (2D and 3D) with the n60 stack: the NRR native race renderer,
 * stereo frame replay with interleave, end-mode swap pacing, race0 frame skip, the HUD/track/draw
 * levers, finish_mesh and prewarm. Every one of those is an ini lever that defaults to 0, and the
 * Old 3DS card switched them on with ~110 ini keys (the config table holds 128). This file is the
 * same lever set as ONE ini key, for the New 3DS (the default, non-GDX_3DS_O3DS build):
 *
 *   [perf]
 *   preset = n60
 *
 * The values are the Old 3DS card's (n60-3d5 MAIN, gate 21) with three classes of change:
 *   - dropped: everything diagnostic (receipts, census, PMU, hitch, spike, launch variants) and the
 *     Old 3DS memory-fit levers (audio_stream, tex_upload_kb: the New 3DS has the RAM);
 *   - added: renderthread = 0. The whole n60 stack (NRR snapshot/marker path, race0 frame skip,
 *     overlap kicks, stereo replay/interleave, end-mode pacing) is the sequential-host-loop
 *     architecture (rtMode 0); the New 3DS default is the core-2 render thread, which those levers
 *     refuse. At 804 MHz with the L2 the sequential race fits a 16.7 ms frame with room to spare,
 *     and core 2 keeps the audio threads;
 *   - New 3DS headroom: tex_keep_kb 4096 (2x the Old 3DS budget, more linear heap), the Old 3DS
 *     audio model defaults (audio_simd, audio_ctx_narrow) on, full rival detail and draw distance
 *     (the [perf] LOD levers stay stock: the o3ds LOD preset is NOT applied), [stereo] enabled = 1
 *     (the 3D slider works; slider 0 renders byte-identical mono).
 *
 * An explicit ini key always wins over a value here (gdx3ds_config.h, PRESET DEFAULTS).
 */
#include "gdx3ds_preset_n60.h"

#include <stddef.h>
#include <strings.h>

static const Gdx3dsCfgPresetEntry kN60Preset[] = {
    /* host loop / pacing / frame skip */
    { "debug", "renderthread", "0" },
    { "debug", "frameskip", "race0" },
    { "debug", "race0_arm", "1" },
    { "debug", "race0_dwell", "600" },
    { "debug", "race0_heavy_mode", "steady" },
    { "debug", "pace_swapgate", "1" },
    { "debug", "pace_swapgate_3d", "2" },
    { "debug", "dl_hollow", "1" },
    { "debug", "overlap_kick", "1" },
    { "debug", "swp_lean", "4" },
    { "debug", "mirror_lazy", "1" },
    { "debug", "flush_svc", "1" },
    /* textures */
    { "debug", "tex_fast", "1" },
    { "debug", "tex_keep", "1" },
    { "debug", "tex_keep_kb", "4096" },
    { "debug", "tex_keep_floor_kb", "3072" },
    /* audio + save */
    { "debug", "audio_io_unlock", "1" },
    { "debug", "audio_ctx_narrow", "1" },
    { "debug", "audio_simd", "1" },
    { "debug", "audio_rs_align", "1" },
    { "debug", "sram_async", "1" },
    { "debug", "filelog_race_quiet", "1" },
    /* the native renderer: owned interpreter + C5 retained paths */
    { "native", "all", "1" },
    { "native", "interp", "1" },
    { "native", "gpuxf", "1" },
    { "native", "track_retained", "1" },
    { "native", "track_resident", "1" },
    { "native", "track_mattab", "1" },
    { "native", "track_nearclip", "1" },
    { "native", "track_opfold", "1" },
    { "native", "hud_batch", "1" },
    { "native", "hud_direct", "1" },
    { "native", "hud_retain", "1" },
    { "native", "shadow_batch", "1" },
    { "native", "fx_batch", "1" },
    { "native", "fxshd_direct", "1" },
    { "native", "gadget_batch", "1" },
    { "native", "sky_retain", "1" },
    { "native", "machines_retained", "1" },
    { "native", "machines_texskip", "1" },
    { "native", "machines_mattab", "1" },
    { "native", "machines_loadmemo", "1" },
    { "native", "machines_walkfast", "1" },
    { "native", "machines_mattab_live", "1" },
    { "native", "c3d_diet", "1" },
    { "native", "dlmemo", "1" },
    { "native", "deadsetup", "1" },
    { "native", "replay", "0" },
    { "native", "hudsky_emit", "1" },
    /* NRR */
    { "native", "nrr", "1" },
    { "native", "nrr_hud", "1" },
    { "native", "nrr_trk", "1" },
    { "native", "nrr_mach", "1" },
    { "native", "nrr_sky", "1" },
    { "native", "nrr_shd", "1" },
    { "native", "nrr_fx", "1" },
    { "native", "nrr_dress", "1" },
    { "native", "nrr_deco", "1" },
    { "native", "nrr_direct", "1" },
    { "native", "nrr_hud_memo", "1" },
    { "native", "nrr_trk_joins", "1" },
    { "native", "nrr_hud_late", "7" },
    { "native", "nrr_trk_merge", "1" },
    { "native", "nrr_draw", "15" },
    { "native", "nrr_hud_kept", "1" },
    { "native", "nrr_hud_rr", "3" },
    { "native", "finish_mesh", "1" },
    { "native", "finish_direct", "0" },
    { "native", "prewarm", "7" },
    { "native", "prewarm_draws", "256" },
    { "native", "zclear_elide", "2" },
    { "native", "lcd_vram", "0" },
    /* stereo */
    { "native", "stereo_interleave", "1" },
    { "native", "stereo_noreflush", "1" },
    { "stereo", "enabled", "1" },
    /* bottom screen */
    { "menu", "race_quiet", "1" },
};

int gdx3ds_preset_n60_name(const char* name) {
    if (name == NULL) {
        return 0;
    }
    return strcasecmp(name, "n60") == 0 || strcasecmp(name, "n3ds60") == 0;
}

const Gdx3dsCfgPresetEntry* gdx3ds_preset_resolve(const char* name, int* count) {
    if (!gdx3ds_preset_n60_name(name)) {
        *count = 0;
        return NULL;
    }
    *count = (int)(sizeof(kN60Preset) / sizeof(kN60Preset[0]));
    return kN60Preset;
}
