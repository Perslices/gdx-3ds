/* gdx3ds_native_cfg.h — the [native] ini section (09-fx-integration-plan.md §6.1).
 *
 *   [native]
 *   all        = 0   ; master for the TIER-2 levers only (track, machines, fx_sink, fx_sprites,
 *                    ; fx_hud): 1 = each of those that is 0/absent reads as 1; an explicit
 *                    ; verify value (2) is kept. Never touches interp.
 *   interp     = 0   ; Tier 1 own interpreter (track: interp); always explicit
 *   machines   = 0   ; baked machine meshes (track: machines)
 *   track      = 0   ; course tessellator marker replay (track: track; 2 = verify)
 *   fx_sink    = 0   ; effects marker replay: per-class native batches (2 = verify)
 *   fx_sprites = 0   ; fold prim/env into the vertex colour across sprites where the shader
 *                    ; carries that input per vertex (needs fx_sink)
 *   fx_hud     = 0   ; native 2D batcher (HUD, stars, markers)
 *   ownfold    = 0   ; the SAME prim/env fold inside the OWNED interpreter's own batches
 *                    ; (fx_hud is inert under interp=1). Tier-2: `all` turns it on.
 *   ncensus    = 0   ; [ncensus] batch-split attribution on the owned path (raw, not Tier-2)
 *   machines_retained      = 0 ; C5 Stage 1: gpuxf meshes drawn from GPU-resident per-(slot,LOD)
 *                                ; buffers, FLX alpha in the shader (needs gpuxf=1; [rr] receipt)
 *   machines_retained_clip = 0 ; 1 = performance mode: the PICA clips edge-straddling meshes and
 *                                ; runs bind without the survivor screen (tolerance-gated)
 *   machines_texskip       = 0 ; 1 = skip covered machine texture-set lists (interp=1; exact),
 *                                ; 2 = verify (never skips) -- [texskip] receipt
 *   (none of the three is Tier-2: `all` never turns them on)
 *
 * Levers are read at first use, after the ini is loaded; every consumer latches its own copy
 * (a boot-time decision, like trectbatch/renderthread). Everything external is weak so this
 * TU also links into the host harnesses. Receipt: gdx3ds_native_boot_receipt() prints one
 * "[native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx=<bitmask>" line. */
#ifndef GDX3DS_NATIVE_CFG_H
#define GDX3DS_NATIVE_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 once the ini is loaded and the config accessors exist (0 in harnesses). */
int gdx3ds_native_ready(void);
/* The lever's value with the `all` master applied to the Tier-2 keys (fallback when not
 * ready); every other key (interp, ...) is returned as written. */
int gdx3ds_native_lever(const char* key, int fallback);

/* 1 when interp_verify != 0 and interp != 0, i.e. when the batch-for-batch oracle is running
 * and BOTH 2D folds -- fx_hud (LUS) and ownfold (owned) -- are forced off so it is comparing
 * like with like (verifymatrix-progress.md, audit-2026-09-24 finding 5). */
int gdx3ds_native_verify_forces_fold_off(void);
/* 1 for the keys the `all` master covers. */
int gdx3ds_native_is_tier2(const char* key);
/* Raw [native] key (no master), for receipts and the DBG rows. */
int gdx3ds_native_raw(const char* key, int fallback);
/* fx levers as the plan's bitmask: bit0 sink, bit1 sprites, bit2 hud. */
int gdx3ds_native_fx_mask(void);
/* One-shot boot receipt (no-op until ready; safe to call every frame). */
void gdx3ds_native_boot_receipt(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NATIVE_CFG_H */
