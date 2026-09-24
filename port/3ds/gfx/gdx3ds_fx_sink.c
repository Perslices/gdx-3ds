/* gdx3ds_fx_sink.c — [native] fx: the game-side recorder behind gdx3ds_fx_shim_begin.h.
 *
 * Modes ([native] fx_sink): 0 = pass-through (every shim macro emits the real GBI; with
 * [native] fxcensus=1 the sink still counts what the native path WOULD record — the M0 census),
 * 1 = native (VTX/TRI/PRIM/ENV/TEX ops are recorded, pipesyncs dropped, a G_NOOP marker
 * replaces them at every run boundary — a class setup list, a texture load, the bracket end —
 * and Interpreter::GdxFxMarker replays the run there), 2 = verify (real GBI AND the record;
 * START/END markers per run let the replay reproduce the run into a native batch, hash it,
 * discard it and compare against the stock draws).
 *
 * [native] fx_sprites (fold) is carried in the record: the replay folds prim/env value changes
 * into the per-vertex colour when the shader carries that register per vertex, instead of
 * splitting the batch the way GfxDpSetPrimColor/GfxDpSetEnvColor do on the stock path.
 *
 * Everything external is weak: this TU also links into the DL-test harness. */
#include "gdx3ds_fx_record.h"
#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)
#include "gdx3ds_fx_shim.h"
#include "gdx3ds_native_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern void gdx3ds_hud_fold_stats(uint32_t out[2]) __attribute__((weak));

/* The class setup lists (port/gen/AssetBindings.c, hud_gfx). Weak: absent in the harness. */
extern unsigned char D_4007FB8[] __attribute__((weak));
extern unsigned char D_4007FD8[] __attribute__((weak));
extern unsigned char D_4008000[] __attribute__((weak));
extern unsigned char D_4008028[] __attribute__((weak));
extern unsigned char D_4008130[] __attribute__((weak));
extern unsigned char aSetupPitForceFieldDL[] __attribute__((weak));
extern unsigned char aSetupFallExplosionDL[] __attribute__((weak));

#define GDXFX_G_NOOP 0x00u

static const char* const kClassName[GDXFX_CLASS_COUNT] = { "expl", "debris", "spark", "cspark",
                                                           "smoke", "pitff", "fallx", "other" };

int gdxfx_active = 0;

static int sMode = -1; /* -1 = config not read yet */
static int sFold = 0;
static int sCensus = 0;
static int sCeiling = 0;
static int sInitLogged = 0;
static GdxFxRecord sRec[2];
static GdxFxRecord* sCur = NULL;
static int sPool = 0;
static int sPlayer = 0;
static int sCls = GDXFX_CLS_OTHER;
static uint32_t sRunStart = 0; /* op index where the current run began */
static uint32_t sBeginCalls = 0;
static GdxFxReplayStats sLastRp;

/* Census: per class, what the bracket emitted this Racer_Draw. `groups` counts the state
 * changes that land on a non-empty batch (+1 per class with tris), i.e. the draws the stock
 * interpreter issues for the class under its split-on-value-change rule. */
typedef struct {
    uint32_t vtxOps, verts, tris, prim, env, tile, texload, pipesync, groups, fallback;
} GdxFxClassCensus;
static GdxFxClassCensus sC[GDXFX_CLASS_COUNT];
/* Run-wide aggregates (every Racer_Draw, not just the sampled ones): per-class sprite max
 * over one call, and totals since boot — the "per-frame counts" the sampled line cannot show
 * for effects that live a few frames. */
static uint32_t sMaxSprites[GDXFX_CLASS_COUNT];
static uint32_t sMaxGroups[GDXFX_CLASS_COUNT];
static uint32_t sTotSprites[GDXFX_CLASS_COUNT];
static uint32_t sTotGroups[GDXFX_CLASS_COUNT];
static uint32_t sMaxGroupsFrame; /* stock draw groups of the whole bracket, max over one call */
static GdxFxReplayStats sTotRp;  /* replay stats summed over every pool-frame since boot */
static uint32_t sMaxNativeDraws; /* native draws of one pool-frame, max since boot */
static uint32_t sTrisSinceState; /* tris emitted since the last state change (census) */
static uint8_t sPrim[4], sEnv[4], sLod;
static int sTile = -1;
static uint32_t sOpsTotal, sRunsTotal, sFallbackTotal;

static void FxLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void FxLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    if (&gdx_dbg_logf == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

static void FxReadConfig(void) {
    if (!gdx3ds_native_ready()) {
        return; /* stay unread; try again next call */
    }
    sMode = gdx3ds_native_lever("fx_sink", 0);
    if (sMode < 0 || sMode > 2) {
        sMode = 0;
    }
    sFold = gdx3ds_native_lever("fx_sprites", 0) != 0;
    sCensus = gdx3ds_native_raw("fxcensus", 0) != 0;
    sCeiling = gdx3ds_native_raw("fxceiling", 0);
    FxLog("[fx] init mode=%d fold=%d census=%d ceiling=%d ops=%u runs=%u classes=%d", sMode, sFold, sCensus,
          sCeiling, (unsigned)GDXFX_MAX_OPS, (unsigned)GDXFX_MAX_RUNS, (int)GDXFX_CLASS_COUNT);
}

static int FxClassOf(const void* dl) {
    if (dl == (const void*)D_4007FB8 && D_4007FB8 != NULL) {
        return GDXFX_CLS_EXPLOSION;
    }
    if (dl == (const void*)D_4007FD8 && D_4007FD8 != NULL) {
        return GDXFX_CLS_DEBRIS;
    }
    if (dl == (const void*)D_4008000 && D_4008000 != NULL) {
        return GDXFX_CLS_SPARK;
    }
    if (dl == (const void*)D_4008028 && D_4008028 != NULL) {
        return GDXFX_CLS_CSPARK;
    }
    if (dl == (const void*)D_4008130 && D_4008130 != NULL) {
        return GDXFX_CLS_SMOKE;
    }
    if (dl == (const void*)aSetupPitForceFieldDL && aSetupPitForceFieldDL != NULL) {
        return GDXFX_CLS_PITFF;
    }
    if (dl == (const void*)aSetupFallExplosionDL && aSetupFallExplosionDL != NULL) {
        return GDXFX_CLS_FALLX;
    }
    return GDXFX_CLS_OTHER;
}

/* ---- B5 census (fxcensus=1): why the explosion class splits, and what would merge ----
 *
 * The [fx] agg line says explosions split on env (split=e). That is only half the story: the
 * run boundary of this class is the per-sprite gDPLoadTextureBlock (racer.c:6537), so even a
 * foldable env would still draw once per sprite. These counters separate the two:
 *   texload     — gDPSetTextureImage calls inside the class
 *   texsame     — of those, the ones whose image pointer equals the previous load's (a
 *                 redundant re-bind: two sprites on the same animation frame)
 *   runsdedupe  — runs the class would have if a redundant load did not break the run
 *                 (= consecutive distinct-frame groups) — the ceiling that folding env buys
 * plus the distinct-value cardinalities (are prim/env a small per-class set?) and whether
 * prim and env move independently per sprite. */
#define GDXFX_XC_SET 64
typedef struct {
    uint32_t v[GDXFX_XC_SET];
    uint32_t n;
    uint32_t overflow;
} GdxFxSet;
static GdxFxSet sXcPrim[GDXFX_CLASS_COUNT];
static GdxFxSet sXcEnv[GDXFX_CLASS_COUNT];
static GdxFxSet sXcTex[GDXFX_CLASS_COUNT];
static uint32_t sXcTexSame[GDXFX_CLASS_COUNT];  /* redundant frame loads */
static uint32_t sXcTexload[GDXFX_CLASS_COUNT];  /* frame loads, run-wide */
static uint32_t sXcRunsDedupe[GDXFX_CLASS_COUNT]; /* runs if redundant loads did not split */
static uint32_t sXcPrimOnly[GDXFX_CLASS_COUNT]; /* per sprite: prim moved, env did not */
static uint32_t sXcEnvOnly[GDXFX_CLASS_COUNT];
static uint32_t sXcBoth[GDXFX_CLASS_COUNT];
static uint32_t sXcNeither[GDXFX_CLASS_COUNT];
static uint32_t sXcEnvPerBracketMax[GDXFX_CLASS_COUNT]; /* distinct env values in one bracket */
static uint32_t sXcTexPerBracketMax[GDXFX_CLASS_COUNT];
static GdxFxSet sXcEnvBracket[GDXFX_CLASS_COUNT];
static GdxFxSet sXcTexBracket[GDXFX_CLASS_COUNT];
static const void* sXcLastTex;   /* previous load's image pointer, per bracket+class */
static uint32_t sXcSpritePrim;   /* prim/env change flags of the sprite being assembled */
static uint32_t sXcSpriteEnv;
static uint32_t sXcSpriteOpen;
static uint32_t sTotDrawsByClass[GDXFX_CLASS_COUNT];
static uint32_t sDlDumped[GDXFX_CLASS_COUNT];

static void FxSetAdd(GdxFxSet* s, uint32_t key) {
    uint32_t i;
    for (i = 0; i < s->n; i++) {
        if (s->v[i] == key) {
            return;
        }
    }
    if (s->n < GDXFX_XC_SET) {
        s->v[s->n++] = key;
    } else {
        s->overflow++;
    }
}

/* One sprite's worth of prim/env motion is closed at its frame load (the class's run
 * boundary) and at the bracket end. */
static void FxCensusCloseSprite(void) {
    if (!sXcSpriteOpen) {
        return;
    }
    if (sXcSpritePrim && sXcSpriteEnv) {
        sXcBoth[sCls]++;
    } else if (sXcSpritePrim) {
        sXcPrimOnly[sCls]++;
    } else if (sXcSpriteEnv) {
        sXcEnvOnly[sCls]++;
    } else {
        sXcNeither[sCls]++;
    }
    sXcSpritePrim = 0;
    sXcSpriteEnv = 0;
    sXcSpriteOpen = 0;
}

static void FxCensusTexload(const void* img) {
    FxCensusCloseSprite();
    sXcTexload[sCls]++;
    FxSetAdd(&sXcTex[sCls], (uint32_t)(uintptr_t)img);
    FxSetAdd(&sXcTexBracket[sCls], (uint32_t)(uintptr_t)img);
    if (img == sXcLastTex) {
        sXcTexSame[sCls]++;
    } else {
        sXcRunsDedupe[sCls]++;
    }
    sXcLastTex = img;
}

/* The class setup display lists are binary ROM data (hud_gfx segment 4): dump the words once
 * so the combine mode behind each class is on the record. */
static void FxCensusDumpDl(const void* dl) {
    const uint32_t* w = (const uint32_t*)dl;
    char line[320];
    int n, k;
    if (dl == NULL || sCls >= GDXFX_CLASS_COUNT || sDlDumped[sCls]) {
        return;
    }
    sDlDumped[sCls] = 1;
    n = snprintf(line, sizeof(line), "[fx] dl %s %p", kClassName[sCls], dl);
    for (k = 0; k < 16 && n > 0 && n < (int)sizeof(line); k += 2) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x:%08x", (unsigned)w[k], (unsigned)w[k + 1]);
        if ((w[k] >> 24) == 0xB8u) { /* G_ENDDL */
            break;
        }
    }
    FxLog("%s", line);
}

/* Census: a state change with tris pending closes a stock draw group. */
static void FxCensusState(void) {
    if (sTrisSinceState != 0) {
        sC[sCls].groups++;
        sTrisSinceState = 0;
    }
}

static int FxFirstOpOfRun(void) {
    return sCur->nOps == sRunStart;
}

/* Write the END marker for the pending run into `slot`. */
static void FxWriteEndMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    sCur->runs[run].opStart = (uint16_t)sRunStart;
    sCur->runs[run].opEnd = (uint16_t)sCur->nOps;
    sCur->runs[run].cls = (uint8_t)sCls;
    sCur->nRuns++;
    gdxfx_gbi_marker(slot, (GDXFX_G_NOOP << 24) | ((uint32_t)GDXFX_MARKER_P_END << 16) | (run & 0xFFFFu), sCur);
    sRunStart = sCur->nOps;
    sRunsTotal++;
}

static void FxWriteStartMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    gdxfx_gbi_marker(slot, (GDXFX_G_NOOP << 24) | ((uint32_t)GDXFX_MARKER_P_START << 16) | (run & 0xFFFFu), sCur);
}

/* Room for one more op (two for a tri pair) AND its run's marker. */
static int FxCanRecord(int nOps) {
    return sCur->nOps + (uint32_t)nOps <= GDXFX_MAX_OPS && sCur->nRuns < GDXFX_MAX_RUNS - 1;
}

/* A recording-eligible op could not be recorded: the real command must go out, but only
 * after any pending run has its marker (order). Returns the shim code. */
static int FxFallbackReal(void* slot, int advanced) {
    sC[sCls].fallback++;
    sFallbackTotal++;
    if (gdxfx_pending()) {
        if (!advanced) {
            return 0; /* never happens in the bracket (every site advances) */
        }
        FxWriteEndMarker(slot);
        return 2;
    }
    return 0;
}

static void FxAppend(uint8_t kind, uint8_t a, uint8_t b, uint8_t c, uint32_t arg) {
    GdxFxOp* op = &sCur->ops[sCur->nOps++];
    op->kind = kind;
    op->a = a;
    op->b = b;
    op->c = c;
    op->arg = arg;
    sOpsTotal++;
}

/* Verify mode: the real command is emitted too. First op of a run takes a START marker in
 * the slot and moves the real command to a fresh slot (2); otherwise the slot is reused (0). */
static int FxVerifyReturn(void* slot, int firstOfRun) {
    if (firstOfRun) {
        FxWriteStartMarker(slot);
        return 2;
    }
    return 0;
}

static int FxRecordReturn(void* slot, int firstOfRun) {
    if (sMode == 2) {
        return FxVerifyReturn(slot, firstOfRun);
    }
    return 1;
}

/* COVERAGE: effect-class presence, on the REAL GBI path, so it reads the same with
 * [native] fx_sink off as on.
 *
 * The setup display list is NOT the presence signal, and assuming it was cost a whole
 * measurement pass: racer.c emits all seven class setup lists on every in-race frame whether
 * or not that class has a sprite, so a census keyed on the setup pointer reported every class
 * present for all 1517 in-race frames of the canonical replay -- and `fx 7/7 (100%)` for the
 * OLD 8-SHOT schedule, which explosions-progress.md proves is false (no SHOT lands on a frame
 * with an explosion; they live at 4036-4216 and the nearest SHOT is 4000). The setup list only
 * says which class the NEXT vertex op belongs to; the vertex op is the sprite. */
static int sCovCls = GDXFX_CLS_OTHER;

void gdxfx_cov_setup(const void* dl) {
    sCovCls = FxClassOf(dl);
}

void gdxfx_cov_vertex(void) {
    if (sCovCls != GDXFX_CLS_OTHER) {
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_FX, (uint32_t)sCovCls));
    }
}

void gdxfx_begin(int poolIndex, unsigned frame, int playerIndex) {
    if (sMode < 0) {
        FxReadConfig();
    }
    gdxfx_active = 0;
    sBeginCalls++;
    if (sMode < 0 || (sMode == 0 && !sCensus)) {
        return;
    }
    if (poolIndex < 0 || poolIndex > 1) {
        if (!sInitLogged) {
            sInitLogged = 1;
            FxLog("[fx] pool index %d out of range: native fx disabled for this call", poolIndex);
        }
        return;
    }
    sPool = poolIndex;
    sPlayer = playerIndex;
    sCur = &sRec[poolIndex];
    if (sCur->magic != GDXFX_MAGIC || sCur->frame != frame) {
        sLastRp = sCur->rp; /* the replay of this pool's previous frame is complete by now */
        sTotRp.markers += sLastRp.markers;
        sTotRp.draws += sLastRp.draws;
        sTotRp.tris += sLastRp.tris;
        sTotRp.fast += sLastRp.fast;
        sTotRp.exact += sLastRp.exact;
        sTotRp.fallback += sLastRp.fallback;
        sTotRp.foldPrim += sLastRp.foldPrim;
        sTotRp.foldEnv += sLastRp.foldEnv;
        sTotRp.splitPrim += sLastRp.splitPrim;
        sTotRp.splitEnv += sLastRp.splitEnv;
        sTotRp.splitTile += sLastRp.splitTile;
        sTotRp.verifyOk += sLastRp.verifyOk;
        sTotRp.verifyBad += sLastRp.verifyBad;
        if (sLastRp.draws > sMaxNativeDraws) {
            sMaxNativeDraws = sLastRp.draws;
        }
        {
            int ci;
            for (ci = 0; ci < GDXFX_CLASS_COUNT; ci++) {
                sTotDrawsByClass[ci] += sLastRp.drawsByClass[ci];
            }
        }
        memset(&sCur->rp, 0, sizeof(sCur->rp));
        sCur->magic = GDXFX_MAGIC;
        sCur->mode = (uint32_t)sMode;
        sCur->fold = (uint32_t)sFold;
        sCur->census = (uint32_t)sCensus;
        sCur->ceiling = (uint32_t)sCeiling;
        sCur->frame = frame;
        sCur->nOps = 0;
        sCur->nRuns = 0;
    }
    sRunStart = sCur->nOps;
    sCls = GDXFX_CLS_OTHER;
    sCovCls = GDXFX_CLS_OTHER;
    memset(sC, 0, sizeof(sC));
    sTrisSinceState = 0;
    sTile = -1;
    if (sCensus) {
        memset(sXcEnvBracket, 0, sizeof(sXcEnvBracket));
        memset(sXcTexBracket, 0, sizeof(sXcTexBracket));
        sXcLastTex = NULL;
        sXcSpritePrim = 0;
        sXcSpriteEnv = 0;
        sXcSpriteOpen = 0;
    }
    memset(sPrim, 0, sizeof(sPrim));
    memset(sEnv, 0, sizeof(sEnv));
    sLod = 0;
    sOpsTotal = 0;
    sRunsTotal = 0;
    sFallbackTotal = 0;
    gdxfx_active = 1;
}

void gdxfx_end(void) {
    if (!gdxfx_active) {
        return;
    }
    gdxfx_active = 0;
    FxCensusState();
    if (sCensus) {
        int k;
        FxCensusCloseSprite();
        if (sC[GDXFX_CLS_EXPLOSION].vtxOps != 0) {
            FxLog("[fx] xframe %u cam=%d expl=%u tris=%u groups=%u", sCur->frame, sPlayer,
                  sC[GDXFX_CLS_EXPLOSION].vtxOps, sC[GDXFX_CLS_EXPLOSION].tris,
                  sC[GDXFX_CLS_EXPLOSION].groups);
        }
        for (k = 0; k < GDXFX_CLASS_COUNT; k++) {
            if (sXcEnvBracket[k].n > sXcEnvPerBracketMax[k]) {
                sXcEnvPerBracketMax[k] = sXcEnvBracket[k].n;
            }
            if (sXcTexBracket[k].n > sXcTexPerBracketMax[k]) {
                sXcTexPerBracketMax[k] = sXcTexBracket[k].n;
            }
        }
    }
    {
        int k;
        uint32_t groupsFrame = 0;
        for (k = 0; k < GDXFX_CLASS_COUNT; k++) {
            if (sC[k].vtxOps > sMaxSprites[k]) {
                sMaxSprites[k] = sC[k].vtxOps;
            }
            if (sC[k].groups > sMaxGroups[k]) {
                sMaxGroups[k] = sC[k].groups;
            }
            sTotSprites[k] += sC[k].vtxOps;
            sTotGroups[k] += sC[k].groups;
            groupsFrame += sC[k].groups;
        }
        if (groupsFrame > sMaxGroupsFrame) {
            sMaxGroupsFrame = groupsFrame;
        }
    }
    /* Verify mode (fx_sink=2) always emits the coverage receipt ([fx] agg2 vok/vbad): a verify
     * run without fxcensus=1 must not be a silent pass (same shape as the track fix 28f588e). */
    if ((sCensus || sMode == 2) && (sBeginCalls & 63u) == 1u) {
        if (sCensus) {
            char line[320];
            int n = snprintf(line, sizeof(line), "[fx] cam=%d mode=%d fold=%d pool=%d frame=%u", sPlayer, sMode, sFold,
                             sPool, sCur->frame);
            int k;
            for (k = 0; k < GDXFX_CLASS_COUNT - 1 && n > 0 && n < (int)sizeof(line); k++) {
                const GdxFxClassCensus* c = &sC[k];
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%u/%u/%u", kClassName[k], c->vtxOps, c->tris,
                              c->groups);
            }
            if (n > 0 && n < (int)sizeof(line)) {
                uint32_t prim = 0, env = 0, tile = 0, texload = 0, sync = 0, fb = 0;
                for (k = 0; k < GDXFX_CLASS_COUNT; k++) {
                    prim += sC[k].prim;
                    env += sC[k].env;
                    tile += sC[k].tile;
                    texload += sC[k].texload;
                    sync += sC[k].pipesync;
                    fb += sC[k].fallback;
                }
                snprintf(line + n, sizeof(line) - (size_t)n, " chg=p%u/e%u/t%u/l%u sync=%u fb=%u ops=%u/%u", prim, env,
                         tile, texload, sync, fb, sOpsTotal, sRunsTotal);
            }
            FxLog("%s", line);
            n = snprintf(line, sizeof(line), "[fx] agg calls=%u maxg=%u", sBeginCalls, sMaxGroupsFrame);
            for (k = 0; k < GDXFX_CLASS_COUNT - 1 && n > 0 && n < (int)sizeof(line); k++) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%u/%u/%u/%u", kClassName[k], sMaxSprites[k],
                              sMaxGroups[k], sTotSprites[k], sTotGroups[k]);
            }
            FxLog("%s", line); /* per class: max sprites per call / max stock draws per call / total sprites / total stock draws */
            /* B5: per class — sprites / stock draws / native draws / frame loads / redundant
             * frame loads / runs if redundant loads did not split / distinct prim values /
             * distinct env values / max distinct env in one bracket / max distinct frames in
             * one bracket / sprites where (prim only, env only, both, neither) moved. */
            for (k = 0; k < GDXFX_CLASS_COUNT - 1; k++) {
                if (sTotSprites[k] == 0) {
                    continue;
                }
                FxLog("[fx] xcens %s spr=%u stock=%u native=%u texload=%u texsame=%u runsdedupe=%u "
                      "prims=%u%s envs=%u%s envmax=%u texmax=%u mv=p%u/e%u/b%u/n%u",
                      kClassName[k], sTotSprites[k], sTotGroups[k], sTotDrawsByClass[k], sXcTexload[k],
                      sXcTexSame[k], sXcRunsDedupe[k], sXcPrim[k].n, sXcPrim[k].overflow ? "+" : "",
                      sXcEnv[k].n, sXcEnv[k].overflow ? "+" : "", sXcEnvPerBracketMax[k],
                      sXcTexPerBracketMax[k], sXcPrimOnly[k], sXcEnvOnly[k], sXcBoth[k], sXcNeither[k]);
            }
        }
        {
            uint32_t totGroups = 0;
            int k;
            for (k = 0; k < GDXFX_CLASS_COUNT; k++) {
                totGroups += sTotGroups[k];
            }
            /* Whole-run replay totals (every pool-frame): native draws vs the stock draw groups the
             * same brackets counted, max native draws in one pool-frame, folds/splits, verify. */
            FxLog("[fx] agg2 stockdraws=%u nativedraws=%u maxnative=%u markers=%u tris=%u fast=%u exact=%u fb=%u "
                  "fold=p%u/e%u split=p%u/e%u/t%u vok=%u vbad=%u",
                  totGroups, sTotRp.draws, sMaxNativeDraws, sTotRp.markers, sTotRp.tris, sTotRp.fast, sTotRp.exact,
                  sTotRp.fallback, sTotRp.foldPrim, sTotRp.foldEnv, sTotRp.splitPrim, sTotRp.splitEnv,
                  sTotRp.splitTile, sTotRp.verifyOk, sTotRp.verifyBad);
        }
        if (&gdx3ds_hud_fold_stats != NULL) {
            uint32_t hf[2];
            gdx3ds_hud_fold_stats(hf);
            FxLog("[fx] hud fold=p%u/e%u (interpreter batches kept across prim/env changes, [native] fx_hud)", hf[0],
                  hf[1]);
        }
        if (sMode != 0) {
            FxLog("[fx] replay(prev) markers=%u runs=%u vtx=%u verts=%u tris=%u fast=%u exact=%u draws=%u "
                  "fold=p%u/e%u split=p%u/e%u/t%u fb=%u vok=%u vbad=%u cls=%u/%u/%u/%u/%u/%u/%u",
                  sLastRp.markers, sLastRp.runs, sLastRp.vtxOps, sLastRp.verts, sLastRp.tris, sLastRp.fast,
                  sLastRp.exact, sLastRp.draws, sLastRp.foldPrim, sLastRp.foldEnv, sLastRp.splitPrim,
                  sLastRp.splitEnv, sLastRp.splitTile, sLastRp.fallback, sLastRp.verifyOk, sLastRp.verifyBad,
                  sLastRp.drawsByClass[0], sLastRp.drawsByClass[1], sLastRp.drawsByClass[2],
                  sLastRp.drawsByClass[3], sLastRp.drawsByClass[4], sLastRp.drawsByClass[5],
                  sLastRp.drawsByClass[6]);
        }
    }
}

int gdxfx_pending(void) {
    return sMode > 0 && sCur != NULL && sCur->nOps > sRunStart;
}

int gdxfx_marker(void* slot) {
    if (!gdxfx_pending()) {
        return 0;
    }
    FxWriteEndMarker(slot);
    return 1;
}

int gdxfx_setup(void* slot, const void* dl) {
    int r = 0;
    FxCensusState();
    if (sCensus) {
        FxCensusCloseSprite();
    }
    if (sMode != 0 && gdxfx_marker(slot)) {
        r = 2;
    }
    sCls = FxClassOf(dl);
    sTrisSinceState = 0;
    sTile = -1; /* the setup list re-asserts the tile state */
    if (sCensus) {
        sXcLastTex = NULL; /* a new class starts a new frame-load chain */
        sXcRunsDedupe[sCls]++; /* the setup list itself is a run boundary */
        FxCensusDumpDl(dl);
    }
    return r;
}

int gdxfx_texload(void* slot, const void* img) {
    sC[sCls].texload++;
    FxCensusState();
    if (sCensus) {
        FxCensusTexload(img);
    }
    if ((sCeiling & 2) != 0 && sCls == GDXFX_CLS_EXPLOSION) {
        return 0; /* ceiling measurement: keep the run open across the frame load */
    }
    if (sMode != 0 && gdxfx_marker(slot)) {
        return 2;
    }
    return 0;
}

int gdxfx_vertex(void* slot, const void* vtx, int n, int v0, int advanced) {
    int firstOfRun;
    sC[sCls].vtxOps++;
    sC[sCls].verts += (uint32_t)n;
    if (sMode == 0) {
        return 0;
    }
    if (n <= 0 || n > 32 || v0 < 0 || v0 + n > 32 || !FxCanRecord(1)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_tri(void* slot, int a, int b, int c, int advanced) {
    int firstOfRun;
    sC[sCls].tris++;
    sTrisSinceState++;
    if (sMode == 0) {
        return 0;
    }
    if (!FxCanRecord(1)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_TRI, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0);
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_tri2(void* slot, int a, int b, int c, int d, int e, int f, int advanced) {
    int firstOfRun;
    sC[sCls].tris += 2;
    sTrisSinceState += 2;
    if (sMode == 0) {
        return 0;
    }
    if (!FxCanRecord(2)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_TRI, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0);
    FxAppend(GDXFX_OP_TRI, (uint8_t)d, (uint8_t)e, (uint8_t)f, 0);
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_prim(void* slot, int m, int l, int r, int g, int b, int a, int advanced) {
    int firstOfRun;
    (void)m;
    if (sPrim[0] != (uint8_t)r || sPrim[1] != (uint8_t)g || sPrim[2] != (uint8_t)b || sPrim[3] != (uint8_t)a ||
        sLod != (uint8_t)l) {
        sC[sCls].prim++;
        FxCensusState();
        if (sCensus) {
            FxSetAdd(&sXcPrim[sCls], ((uint32_t)(r & 0xFF) << 24) | ((uint32_t)(g & 0xFF) << 16) |
                                         ((uint32_t)(b & 0xFF) << 8) | (uint32_t)(a & 0xFF));
            sXcSpritePrim = 1;
            sXcSpriteOpen = 1;
        }
        sPrim[0] = (uint8_t)r;
        sPrim[1] = (uint8_t)g;
        sPrim[2] = (uint8_t)b;
        sPrim[3] = (uint8_t)a;
        sLod = (uint8_t)l;
    }
    if (sMode == 0) {
        return 0;
    }
    if (!FxCanRecord(1)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_PRIM, (uint8_t)r, (uint8_t)g, (uint8_t)b, ((uint32_t)(l & 0xFF) << 8) | (uint32_t)(a & 0xFF));
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_env(void* slot, int r, int g, int b, int a, int advanced) {
    int firstOfRun;
    if (sEnv[0] != (uint8_t)r || sEnv[1] != (uint8_t)g || sEnv[2] != (uint8_t)b || sEnv[3] != (uint8_t)a) {
        sC[sCls].env++;
        FxCensusState();
        if (sCensus) {
            uint32_t key = ((uint32_t)(r & 0xFF) << 24) | ((uint32_t)(g & 0xFF) << 16) |
                           ((uint32_t)(b & 0xFF) << 8) | (uint32_t)(a & 0xFF);
            FxSetAdd(&sXcEnv[sCls], key);
            FxSetAdd(&sXcEnvBracket[sCls], key);
            sXcSpriteEnv = 1;
            sXcSpriteOpen = 1;
        }
        sEnv[0] = (uint8_t)r;
        sEnv[1] = (uint8_t)g;
        sEnv[2] = (uint8_t)b;
        sEnv[3] = (uint8_t)a;
    }
    if (sMode == 0) {
        return 0;
    }
    if (!FxCanRecord(1)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_ENV, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint32_t)(a & 0xFF));
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_texture(void* slot, int sc, int tc, int level, int tile, int on, int advanced) {
    int firstOfRun;
    if (sTile != tile) {
        sC[sCls].tile++;
        FxCensusState();
        sTile = tile;
    }
    if (sMode == 0) {
        return 0;
    }
    if (!FxCanRecord(1)) {
        return FxFallbackReal(slot, advanced);
    }
    firstOfRun = FxFirstOpOfRun();
    FxAppend(GDXFX_OP_TEX, (uint8_t)tile, (uint8_t)level, (uint8_t)on,
             ((uint32_t)(sc & 0xFFFF) << 16) | (uint32_t)(tc & 0xFFFF));
    return FxRecordReturn(slot, firstOfRun);
}

int gdxfx_pipesync(void* slot, int advanced) {
    (void)slot;
    (void)advanced;
    sC[sCls].pipesync++;
    if (sMode == 1) {
        return 1; /* dropped: the 3DS pipesync handler is a no-op (gfx_rdp_pipe_sync_handler_rdp) */
    }
    return 0; /* pass-through and verify keep the stock stream exact */
}
