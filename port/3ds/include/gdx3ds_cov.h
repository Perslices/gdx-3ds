/* gdx3ds_cov.h — COVERAGE census: what each frame of a replay actually exercises.
 *
 * Why (NEXT.md B15/B43, docs/research/native/coverage2-progress.md): the parity gate samples
 * a handful of frames by `SHOT`, and what a SHOT does not sample, the gate cannot see. Five
 * separate tracks have now been bitten by the same hole (explosions fxceiling=3, geomloss's
 * machines truncation, dropsites' four forced sites, machfix's 4P material bug, and
 * menus-longtail@4145). Dense per-frame hashing is the brute-force answer and is not viable as
 * the standing gate. The viable answer is to put a small number of SHOTs where the content is,
 * which first requires knowing where the content is. That is this file.
 *
 * Model: the `[fx] xframe` receipt (gdx3ds_fx_sink.c) — per frame, say what was there.
 *
 * A "feature" is a (family, value) pair, packed into one uint32 key: family in the top 8 bits,
 * value in the low 24. Families are listed below. During a frame every hook calls
 * gdx3ds_cov_mark(key); at frame end gdx3ds_cov_frame() emits the DELTA against the previous
 * frame (feature sets are extremely temporally coherent, so the steady state costs no lines at
 * all) plus a run-end summary of every key with its first/last frame and frame count.
 *
 * INI: [debug] cov = 0 (default OFF; every hook is one int load + branch when off).
 *      [debug] cov_verbose = 0 (1 also logs a `key` line with detail on a key's first sighting)
 *
 * Receipts:
 *   [cov] init keys=<cap> frames=<cap>            — armed
 *   [cov] f=<frame> t=<tick> m=<mode> +<k>,<k> -<k>   — per-frame delta (frame == [fh] frame=)
 *   [cov] sum <k> first=<f> last=<f> n=<frames>   — one per distinct key at run end
 *   [cov] done keys=<n> frames=<n> marks=<n> drop=<n>
 *
 * A key prints as <fam>:<val> in lowercase hex, e.g. `04:0a13c7`. tools/dev/covsched.py parses
 * these lines, reconstructs the per-frame feature sets and derives a SHOT schedule from them.
 *
 * Threading: every hook and the frame flush run on whichever thread renders (main with
 * [debug] renderthread=0, core 2 otherwise) — the same thread the oracle's after-endframe hook
 * runs on, which is why the frame numbering agrees with [fh] by construction. Nothing here is
 * safe to call from two threads at once and nothing here needs to be: it is a diagnostic that
 * is off in every shipped configuration.
 */
#ifndef GDX3DS_COV_H
#define GDX3DS_COV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feature families. Keep the numbering stable: covsched.py names them from this list. */
#define GDX3DS_COV_FAM_MODE 0x01u /* value = game mode (gdx_input_game_mode)                */
#define GDX3DS_COV_FAM_FX   0x02u /* value = effect class id (gdx3ds_fx_record.h classes)   */
#define GDX3DS_COV_FAM_MACH 0x03u /* value = (lod << 8) | slot, machine mesh actually drawn */
#define GDX3DS_COV_FAM_CC   0x04u /* value = 24-bit fold of (shaderId0, shaderId1)          */
#define GDX3DS_COV_FAM_RM   0x05u /* value = render-mode bits, see GDX3DS_COV_RM_*          */
#define GDX3DS_COV_FAM_TEX  0x06u /* value = (unit << 20) | (wclass << 16) | (hclass << 12) | fmtsiz */
#define GDX3DS_COV_FAM_CLIP 0x07u /* value = event id, see GDX3DS_COV_CLIP_*                */
#define GDX3DS_COV_FAM_VP   0x08u /* value = viewport rect fold (distinct rects, not a count)*/
#define GDX3DS_COV_FAM_TLUT 0x09u /* value = (tmem << 8) | count class — a palette load     */
#define GDX3DS_COV_FAM_TC   0x0Au /* value = texture-cache event, see GDX3DS_COV_TC_*       */
#define GDX3DS_COV_FAM_FB   0x0Bu /* value = 0 screen target, 1 texture target, 2 right eye */
/* 0x0C is unused: the viewport COUNT of a frame is the number of distinct VP keys in it,
 * which covsched.py derives — a separate family would be the same fact twice. */

#define GDX3DS_COV_RM_ZTEST  0x01u
#define GDX3DS_COV_RM_ZWRITE 0x02u
#define GDX3DS_COV_RM_DECAL  0x04u
#define GDX3DS_COV_RM_ALPHA  0x08u
#define GDX3DS_COV_RM_FOG    0x10u
#define GDX3DS_COV_RM_PDEPTH 0x20u /* prim-depth-anchored draw (SHADER_OPT PRIM_DEPTH)      */
#define GDX3DS_COV_RM_TEDGE  0x40u
#define GDX3DS_COV_RM_ATHR   0x80u

#define GDX3DS_COV_CLIP_NEEDED 0u /* a triangle needed CPU clipping                          */
#define GDX3DS_COV_CLIP_FAN    1u /* the clip fan produced > 3 output vertices               */
#define GDX3DS_COV_CLIP_TRUNC  2u /* the fan hit its vertex cap and truncated                */
#define GDX3DS_COV_CLIP_REJECT 3u /* a reject-variant screen dropped a triangle              */
#define GDX3DS_COV_CLIP_ALLOUT 4u /* all_out: every vertex outside the same plane            */
#define GDX3DS_COV_CLIP_CULL   5u /* backface/degenerate cull                                */

#define GDX3DS_COV_TC_HIT    0u
#define GDX3DS_COV_TC_MISS   1u
#define GDX3DS_COV_TC_EVICT  2u
#define GDX3DS_COV_TC_UPLOAD 3u

#define GDX3DS_COV_KEY(fam, val) ((uint32_t)(((fam) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)))

/* Latched by gdx3ds_cov_init(); read-only afterwards. Every hook is guarded on it. */
extern int gdx3ds_cov_on;

/* Arms the census. `sink` takes one '\0'-terminated line and is the caller's business (on the
 * 3DS, svcOutputDebugString + the SD filelog); NULL disarms. Injected rather than linked so
 * this module drops into the host test and harness builds with nothing to stub. */
void gdx3ds_cov_arm(int on, int verbose, void (*sink)(const char* line));

/* Records `key` as present in the current frame. Returns 1 the FIRST time this key is seen in
 * the whole run (so a caller with detail worth logging can log it once), else 0. */
int gdx3ds_cov_mark_slow(uint32_t key);

static inline int gdx3ds_cov_mark(uint32_t key) {
    return gdx3ds_cov_on ? gdx3ds_cov_mark_slow(key) : 0;
}

/* Frame boundary. Called from gdx3ds_oracle_after_endframe() with the SAME (frame, tick, mode)
 * the [fh] receipt carries, so a [cov] frame number and an [fh] frame number are the same thing. */
void gdx3ds_cov_frame(unsigned long frame, unsigned long tick, int mode);

/* Run end: the per-key summary. Idempotent. */
void gdx3ds_cov_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_COV_H */
