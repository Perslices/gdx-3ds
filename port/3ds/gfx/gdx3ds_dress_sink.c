/* gdx3ds_dress_sink.c -- C5 Stage 3b (gdx3ds_dress.h): the three levers, the direct-load scope
 * markers and the game-side gadget recorder behind gdx3ds_gad_shim_begin.h.
 *
 * The gadget recorder is the [native] fx sink's pattern (gdx3ds_fx_sink.c) on a smaller op set:
 * inside the bracket of Course_GadgetsDraw, gSPVertex / gSP1Triangle / gSP2Triangles /
 * gSPModifyVertex / gSPTexture are recorded into a per-GfxPool op list; the class setup lists
 * (gSPDisplayList) and the jump's texture load (gDPSetTextureImage, the head of
 * gDPLoadTextureBlock) stay real GBI, preceded by a G_NOOP p=0x6D marker whenever ops are pending
 * (the run boundary). The render side replays each run at its marker
 * (gdx3ds_gad_replay.cpp). With gadget_batch=2 (verify) the real GBI goes out as well, bracketed
 * by START/END markers, and the replay compares its native batch with the stock one.
 *
 * Shim return codes (as the fx/track shims): 0 = emit the real command into the slot, 1 =
 * recorded (the macro rewinds its cursor if it advanced), 2 = a marker was written into the slot,
 * emit the real command into a fresh slot.
 *
 * Everything external is weak: the TU also links into the DL-test harness. */
#include "gdx3ds_dress.h"
#include "gdx3ds_gad_shim.h"
#include "gdx3ds_native_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));

#define GDXGAD_G_NOOP 0x00u

static const char* const kScopeLever[GDXDRESS_SCOPE_COUNT] = { "", "gadget_batch", "dress_retained", "fxshd_direct" };
static int sLevel[GDXDRESS_SCOPE_COUNT] = { 0, -1, -1, -1 };

static void DressLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void DressLog(const char* fmt, ...) {
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

static int DressReadLevel(int scope) {
    int level;
    if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
        return -1;
    }
    level = gdx3ds_native_raw(kScopeLever[scope], 0);
    if (level < 0 || level > 2) {
        level = 0;
    }
    if (level != 0 && gdx3ds_config_get_int("native", "interp", 0) == 0) {
        DressLog("[dress] %s=%d needs [native] interp=1 (the owned walker replays it): off", kScopeLever[scope], level);
        level = 0;
    } else if (level != 0 && gdx3ds_native_verify_forces_fold_off()) {
        DressLog("[dress] %s=%d is off under interp_verify (the LUS pass cannot replay its markers)",
                 kScopeLever[scope], level);
        level = 0;
    } else if (level != 0 && scope == GDXDRESS_SCOPE_DECO && gdx3ds_native_lever("machines", 0) == 0) {
        DressLog("[dress] dress_retained=%d needs [native] machines=1 (the decorations bake into its registry): off",
                 level);
        level = 0;
    }
    /* dress_retained=2: the decoration meshes draw as at 1 (the mesh path is gated by frame
     * parity; [native] machines_verify=1 compares its emission with the list's), and the scope's
     * direct loads run in verify mode -- one ledger, so every verify leg keeps all levels at 2. */
    DressLog("[dress] init %s=%d (scope %d, marker p=0x%02X%s)", kScopeLever[scope], level, scope,
             GDXDRESS_MARKER_P_SCOPE, scope == GDXDRESS_SCOPE_GADGET ? ", runs p=0x6D/0x6E" : "");
    return level;
}

int gdx_dress_level(int scope) {
    if (scope <= GDXDRESS_SCOPE_NONE || scope >= GDXDRESS_SCOPE_COUNT) {
        return 0;
    }
    if (sLevel[scope] < 0) {
        sLevel[scope] = DressReadLevel(scope);
    }
    return sLevel[scope] > 0 ? sLevel[scope] : 0;
}

int gdx_dress_scope_marker(void* slot, int scope, int open) {
    if (slot == NULL || gdx_dress_level(scope) == 0) {
        return 0;
    }
    /* A G_NOOP draws nothing; the record pointer is NULL (the hook reads only w0). */
    gdxgad_gbi_marker(slot, (GDXGAD_G_NOOP << 24) | ((uint32_t)GDXDRESS_MARKER_P_SCOPE << 16) |
                                (uint32_t)(open ? scope : GDXDRESS_SCOPE_NONE),
                      NULL);
    return 1;
}

/* n60lap [native] finish_direct (0 off, default / 1 on): the FINISH banner (Menus_DrawFinish's
 * aFinishDL, a 3D model drawn in the HUD section after the finish line, ~380 frames per race)
 * reloads its two textures twelve times a frame through the walker's stock LOADBLOCK -- the TMEM
 * copy and the import's TMEM hash every time. Armed, the list is bracketed by the effects
 * section's direct-load scope markers (GDXDRESS_SCOPE_FX), so those loads take that scope's
 * exact path: resolved once, TMEM left as debt on the one ledger, the import's hash memoised per
 * source. The level is fxshd_direct's (2 = verify, the stock loads run and are compared); with
 * fxshd_direct off the lever does nothing. Latched at first call. */
static int sFinishDirect = -1;

int gdx_finish_scope_marker(void* slot, int open) {
    if (sFinishDirect < 0) {
        if (!gdx3ds_native_ready()) {
            return 0;
        }
        sFinishDirect = gdx3ds_native_raw("finish_direct", 0) == 1 ? 1 : 0;
        DressLog("[dress] init finish_direct=%d (rides fxshd_direct=%d: the FINISH banner's loads in scope %d)",
                 sFinishDirect, gdx_dress_level(GDXDRESS_SCOPE_FX), GDXDRESS_SCOPE_FX);
    }
    if (sFinishDirect == 0) {
        return 0;
    }
    return gdx_dress_scope_marker(slot, GDXDRESS_SCOPE_FX, open);
}

/* n60finish [native] finish_mesh (0 off, default / 1 on): a G_NOOP p=0x6A right before the FINISH
 * banner's G_DL. The owned walker (gdx_rt_glue.cpp) hands the next G_DL to the mesh registry, which
 * draws aFinishDL as a retained GPU-transform mesh (gdx3ds_mesh.cpp, "finish_mesh"). Needs the owned
 * walker; off under interp_verify (the LUS pass would walk the list either way). Latched at the
 * first call. */
static int sFinishMesh = -1;

int gdx_finish_mesh_marker(void* slot) {
    if (sFinishMesh < 0) {
        if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
            return 0;
        }
        sFinishMesh = gdx3ds_native_raw("finish_mesh", 0) == 1 ? 1 : 0;
        if (sFinishMesh != 0 && gdx3ds_config_get_int("native", "interp", 0) == 0) {
            sFinishMesh = 0;
        } else if (sFinishMesh != 0 && gdx3ds_native_verify_forces_fold_off()) {
            sFinishMesh = 0;
        }
        DressLog("[dress] init finish_mesh=%d (marker p=0x%02X before the FINISH banner's G_DL)", sFinishMesh,
                 GDXFINISH_MARKER_P);
    }
    if (sFinishMesh == 0 || slot == NULL) {
        return 0;
    }
    gdxgad_gbi_marker(slot, (GDXGAD_G_NOOP << 24) | ((uint32_t)GDXFINISH_MARKER_P << 16) | 1u, NULL);
    return 1;
}

/* ---- the gadget recorder ---------------------------------------------------------------- */

int gdxgad_active = 0;

static GdxGadRecord sRec[2];
static GdxGadRecord* sCur = NULL;
static int sMode = 0;
static uint32_t sRunStart = 0;
static uint32_t sBegins = 0;
/* Receipt window: replay stats of the pool-frames completed since the last receipt, and the
 * recorder's own counts. */
static GdxGadReplayStats sWinRp;
static uint32_t sWinFrames, sWinOps, sWinRuns, sWinFull, sWinSetups;

static void GadAddRp(GdxGadReplayStats* a, const GdxGadReplayStats* b) {
    a->markers += b->markers;
    a->vtxOps += b->vtxOps;
    a->verts += b->verts;
    a->mods += b->mods;
    a->tris += b->tris;
    a->fast += b->fast;
    a->exact += b->exact;
    a->fallback += b->fallback;
    a->draws += b->draws;
    a->splitTile += b->splitTile;
    a->verifyOk += b->verifyOk;
    a->verifyBad += b->verifyBad;
}

static void GadMaybeEmit(void) {
    const double inv = sWinFrames != 0 ? 1.0 / (double)sWinFrames : 0.0;
    if (sWinFrames < 64) {
        return;
    }
    DressLog("[gad] frames=%u mode=%d | rec ops=%.1f runs=%.2f setups=%.2f full=%u | rp markers=%.2f vtx=%.1f "
             "verts=%.1f mods=%.1f tris=%.1f fast=%.1f exact=%.1f fb=%u draws=%.2f split=%.2f vok=%u vbad=%u",
             sWinFrames, sMode, sWinOps * inv, sWinRuns * inv, sWinSetups * inv, sWinFull, sWinRp.markers * inv,
             sWinRp.vtxOps * inv, sWinRp.verts * inv, sWinRp.mods * inv, sWinRp.tris * inv, sWinRp.fast * inv,
             sWinRp.exact * inv, sWinRp.fallback, sWinRp.draws * inv, sWinRp.splitTile * inv, sWinRp.verifyOk,
             sWinRp.verifyBad);
    memset(&sWinRp, 0, sizeof(sWinRp));
    sWinFrames = 0;
    sWinOps = 0;
    sWinRuns = 0;
    sWinFull = 0;
    sWinSetups = 0;
}

void gdxgad_begin(int poolIndex, unsigned frame, int enabled) {
    gdxgad_active = 0;
    sMode = gdx_dress_level(GDXDRESS_SCOPE_GADGET);
    if (sMode == 0 || !enabled || poolIndex < 0 || poolIndex > 1) {
        return;
    }
    sBegins++;
    sCur = &sRec[poolIndex];
    if (sCur->magic != GDXGAD_MAGIC || sCur->frame != frame) {
        if (sCur->magic == GDXGAD_MAGIC) {
            GadAddRp(&sWinRp, &sCur->rp); /* that pool's previous frame has been replayed */
            sWinFrames++;
            GadMaybeEmit();
        }
        memset(&sCur->rp, 0, sizeof(sCur->rp));
        sCur->magic = GDXGAD_MAGIC;
        sCur->mode = (uint32_t)sMode;
        sCur->frame = frame;
        sCur->nOps = 0;
        sCur->nRuns = 0;
    }
    sRunStart = sCur->nOps;
    gdxgad_active = 1;
}

void gdxgad_end(void) {
    if (!gdxgad_active) {
        return;
    }
    gdxgad_active = 0;
    sWinOps += sCur->nOps;
    sWinRuns += sCur->nRuns;
}

int gdxgad_pending(void) {
    return sMode > 0 && sCur != NULL && sCur->nOps > sRunStart;
}

static void GadWriteEndMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    sCur->runs[run].opStart = (uint16_t)sRunStart;
    sCur->runs[run].opEnd = (uint16_t)sCur->nOps;
    sCur->nRuns++;
    gdxgad_gbi_marker(slot, (GDXGAD_G_NOOP << 24) | ((uint32_t)GDXGAD_MARKER_P_END << 16) | (run & 0xFFFFu), sCur);
    sRunStart = sCur->nOps;
}

int gdxgad_marker(void* slot) {
    if (!gdxgad_pending()) {
        return 0;
    }
    GadWriteEndMarker(slot);
    return 1;
}

/* A real command that is a run boundary (a setup list, a texture load): the pending run gets its
 * marker first. */
int gdxgad_boundary(void* slot, int isSetup) {
    if (isSetup) {
        sWinSetups++;
    }
    return gdxgad_marker(slot) ? 2 : 0;
}

static int GadCanRecord(int nOps) {
    return sCur->nOps + (uint32_t)nOps <= GDXGAD_MAX_OPS && sCur->nRuns < GDXGAD_MAX_RUNS - 1;
}

/* Could not record: the real command goes out after the pending run's marker (order). */
static int GadFallbackReal(void* slot, int advanced) {
    sWinFull++;
    if (gdxgad_pending() && advanced) {
        GadWriteEndMarker(slot);
        return 2;
    }
    return 0;
}

static void GadAppend(uint8_t kind, uint8_t a, uint8_t b, uint8_t c, uint32_t arg) {
    GdxGadOp* op = &sCur->ops[sCur->nOps++];
    op->kind = kind;
    op->a = a;
    op->b = b;
    op->c = c;
    op->arg = arg;
}

/* Verify: the real command goes out too; the first op of a run takes a START marker. */
static int GadRecordReturn(void* slot, int firstOfRun) {
    if (sMode != 2) {
        return 1;
    }
    if (firstOfRun) {
        gdxgad_gbi_marker(slot, (GDXGAD_G_NOOP << 24) | ((uint32_t)GDXGAD_MARKER_P_START << 16) | (sCur->nRuns & 0xFFFFu),
                          sCur);
        return 2;
    }
    return 0;
}

int gdxgad_vertex(void* slot, const void* vtx, int n, int v0, int advanced) {
    int first;
    if (!GadCanRecord(1) || n <= 0 || n > 64 || v0 < 0 || v0 + n > 64) {
        return GadFallbackReal(slot, advanced);
    }
    first = sCur->nOps == sRunStart;
    GadAppend(GDXGAD_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
    return GadRecordReturn(slot, first);
}

int gdxgad_tri(void* slot, int a, int b, int c, int advanced) {
    int first;
    if (!GadCanRecord(1)) {
        return GadFallbackReal(slot, advanced);
    }
    first = sCur->nOps == sRunStart;
    GadAppend(GDXGAD_OP_TRI, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0);
    return GadRecordReturn(slot, first);
}

int gdxgad_tri2(void* slot, int a, int b, int c, int d, int e, int f, int advanced) {
    int first;
    if (!GadCanRecord(2)) {
        return GadFallbackReal(slot, advanced);
    }
    first = sCur->nOps == sRunStart;
    GadAppend(GDXGAD_OP_TRI, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0);
    GadAppend(GDXGAD_OP_TRI, (uint8_t)d, (uint8_t)e, (uint8_t)f, 0);
    return GadRecordReturn(slot, first);
}

int gdxgad_modify(void* slot, int vtx, int where, uint32_t val, int advanced) {
    int first;
    if (!GadCanRecord(1) || vtx < 0 || vtx > 255) {
        return GadFallbackReal(slot, advanced);
    }
    first = sCur->nOps == sRunStart;
    GadAppend(GDXGAD_OP_MOD, (uint8_t)vtx, (uint8_t)where, 0, val);
    return GadRecordReturn(slot, first);
}

int gdxgad_texture(void* slot, int sc, int tc, int level, int tile, int on, int advanced) {
    int first;
    if (!GadCanRecord(1)) {
        return GadFallbackReal(slot, advanced);
    }
    first = sCur->nOps == sRunStart;
    GadAppend(GDXGAD_OP_TEX, (uint8_t)tile, (uint8_t)level, (uint8_t)on,
              ((uint32_t)(sc & 0xFFFF) << 16) | (uint32_t)(tc & 0xFFFF));
    return GadRecordReturn(slot, first);
}
