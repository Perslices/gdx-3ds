/* gdx3ds_track_sink.c — [native] track: the game-side recorder behind gdx3ds_track_shim.h.
 *
 * Modes ([native] track): 0 = pass-through (every shim macro emits the real GBI; with
 * [native] trkcensus=1 the sink still counts what the native path WOULD record — the M0
 * census), 1 = native (VTX/MOD/template-DL ops are recorded, a G_NOOP marker replaces them at
 * every material-state boundary, and Interpreter::GdxTrackMarker replays the run there),
 * 2 = verify (real GBI AND the record; START/END markers per run let the replay reproduce the
 * run into a native batch, hash it, discard it and compare against the stock draws).
 *
 * Everything external is weak: this TU also links into the DL-test harness. */
#include "gdx3ds_track_record.h"
#include "gdx3ds_track_shim.h"
#include "gdx3ds_track_templates.h"
#include "gdx3ds_native_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_track_templates_raw.inc"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

/* The asset-bound template arrays (port/gen/AssetBindings.c). Weak: absent in the harness. */
extern unsigned char D_80140F0[] __attribute__((weak));
extern unsigned char D_8014138[] __attribute__((weak));
extern unsigned char D_8014180[] __attribute__((weak));
extern unsigned char D_80141C8[] __attribute__((weak));
extern unsigned char D_8014210[] __attribute__((weak));
extern unsigned char D_8014268[] __attribute__((weak));
extern unsigned char D_80142C0[] __attribute__((weak));
extern unsigned char D_8014308[] __attribute__((weak));
extern unsigned char D_8014350[] __attribute__((weak));
extern unsigned char D_8014398[] __attribute__((weak));
extern unsigned char D_80143E0[] __attribute__((weak));
extern unsigned char D_8014430[] __attribute__((weak));
extern unsigned char D_8014480[] __attribute__((weak));
extern unsigned char D_80144D0[] __attribute__((weak));
extern unsigned char D_8014520[] __attribute__((weak));
extern unsigned char D_8014580[] __attribute__((weak));
extern unsigned char D_80145E0[] __attribute__((weak));
extern unsigned char D_8014640[] __attribute__((weak));

/* The bound arrays above are identity placeholders: the bytes live in the segment-8 RDRAM
 * carve (decomp sys_gfx.c PORT block, warmed at boot by the bridge) at the symbol's segment
 * offset, in N64 word order. Weak: absent in the harness. */
extern unsigned char* gdx_rdram __attribute__((weak));
extern uintptr_t gSegment16C8A0VramStart __attribute__((weak));
static const uint32_t kGdxTrkSegOffset[GDXTRK_TEMPLATE_COUNT] = {
    0x140F0, 0x14138, 0x14180, 0x141C8, 0x14210, 0x14268, 0x142C0, 0x14308, 0x14350,
    0x14398, 0x143E0, 0x14430, 0x14480, 0x144D0, 0x14520, 0x14580, 0x145E0, 0x14640,
};

#define GDXTRK_G_NOOP 0x00u
#define GDXTRK_G_MWO_POINT_ST 0x14

int gdxtrk_active = 0;

static int sMode = -1; /* -1 = config not read yet */
static int sCensus = 0;
static int sTemplatesOk = 0;
static int sInitLogged = 0;
static const unsigned char* sTemplatePtr[GDXTRK_TEMPLATE_COUNT];
static GdxTrkRecord sRec[2];
static GdxTrkRecord* sCur = NULL;
static int sPool = 0;
static int sCam = 0;
static uint32_t sRunStart = 0;    /* op index where the current run began */
static int sLastWasVtxRecord = 0; /* the previous shim emission was a recorded VTX op */
static uint32_t sPendingCensus = 0;
static uint32_t sBeginCalls = 0;
static GdxTrkReplayStats sLastRp;

static struct {
    uint32_t vtxOps, verts, dlOps, tris, modOps, runs, realDl, ovrReplace, ovrNoop, fallback;
    uint32_t state[4];
} sC;

static void TrkLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void TrkLog(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    if (gdx_dbg_logf == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

static void TrkInitTemplates(void) {
    unsigned char* const syms[GDXTRK_TEMPLATE_COUNT] = {
        D_80140F0, D_8014138, D_8014180, D_80141C8, D_8014210, D_8014268, D_80142C0, D_8014308, D_8014350,
        D_8014398, D_80143E0, D_8014430, D_8014480, D_80144D0, D_8014520, D_8014580, D_80145E0, D_8014640,
    };
    int ok = 0;
    int okArr = 0;
    int i;
    const unsigned char* seg8 = NULL;
    if (&gdx_rdram != NULL && &gSegment16C8A0VramStart != NULL && gdx_rdram != NULL && gSegment16C8A0VramStart != 0) {
        seg8 = gdx_rdram + gSegment16C8A0VramStart;
    }
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        GdxTrkTemplate t;
        int rc = -1;
        sTemplatePtr[i] = syms[i];
        if (seg8 != NULL) {
            rc = gdxtrk_decode_template(seg8 + kGdxTrkSegOffset[i], kGdxTrkRawTable[i].len, &t);
            if (rc == GDXTRK_DECODE_OK && gdxtrk_template_equal(&t, &kGdxTrkGolden[i])) {
                ok++;
            } else {
                const unsigned char* b = seg8 + kGdxTrkSegOffset[i];
                TrkLog("[trk] template %s: carve decode rc=%d != golden (%02X%02X%02X%02X %02X%02X%02X%02X)",
                       kGdxTrkTemplateNames[i], rc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            }
        }
        /* The bound array is normally an untouched placeholder; count it only as a receipt. */
        if (syms[i] != NULL && gdxtrk_decode_template(syms[i], kGdxTrkRawTable[i].len, &t) == GDXTRK_DECODE_OK &&
            gdxtrk_template_equal(&t, &kGdxTrkGolden[i])) {
            okArr++;
        }
    }
    sTemplatesOk = (ok == GDXTRK_TEMPLATE_COUNT);
    TrkLog("[trk] init mode=%d census=%d templates(carve)=%d/%d templates(arrays)=%d seg8=%p ops=%u runs=%u", sMode,
           sCensus, ok, GDXTRK_TEMPLATE_COUNT, okArr, (const void*)seg8, (unsigned)GDXTRK_MAX_OPS,
           (unsigned)GDXTRK_MAX_RUNS);
}

static void TrkReadConfig(void) {
    if (gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return; /* stay unread; try again next call */
    }
    sMode = 0;
    sCensus = 0;
    if (gdx3ds_config_get_int != NULL) {
        sMode = gdx3ds_native_lever("track", 0); /* [native] all=1 turns it on */
        sCensus = gdx3ds_native_raw("trkcensus", 0) != 0;
    }
    if (sMode < 0 || sMode > 2) {
        sMode = 0;
    }
    TrkInitTemplates();
    if (sMode != 0 && !sTemplatesOk) {
        TrkLog("[trk] templates failed verification: forcing [native] track=0");
        sMode = 0;
    }
}

static int TrkFirstOpOfRun(void) {
    return sCur->nOps == sRunStart;
}

/* Write the END marker for the pending run into `slot`. */
static void TrkWriteEndMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    sCur->runs[run].opStart = (uint16_t)sRunStart;
    sCur->runs[run].opEnd = (uint16_t)sCur->nOps;
    sCur->nRuns++;
    gdxtrk_gbi_marker(slot, (GDXTRK_G_NOOP << 24) | ((uint32_t)GDXTRK_MARKER_P_END << 16) | (run & 0xFFFFu), sCur);
    sRunStart = sCur->nOps;
    sC.runs++;
}

static void TrkWriteStartMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    gdxtrk_gbi_marker(slot, (GDXTRK_G_NOOP << 24) | ((uint32_t)GDXTRK_MARKER_P_START << 16) | (run & 0xFFFFu), sCur);
}

/* Room for one more op AND its run's marker. */
static int TrkCanRecord(void) {
    return sCur->nOps < GDXTRK_MAX_OPS && sCur->nRuns < GDXTRK_MAX_RUNS - 1;
}

/* A recording-eligible op could not be recorded: the real command must go out, but only
 * after any pending run has its marker (order). Returns the shim code. */
static int TrkFallbackReal(void* slot, int advanced) {
    sC.fallback++;
    sLastWasVtxRecord = 0;
    if (gdxtrk_pending()) {
        if (!advanced) {
            /* Overwrite site with a pending run: cannot both mark and overwrite; drop the
               overwrite semantics (never observed: the overwrite always follows a VTX). */
            return 0;
        }
        TrkWriteEndMarker(slot);
        return 2;
    }
    return 0;
}

static int TrkAppend(uint8_t kind, uint8_t n, uint8_t slot, uint8_t tmpl, uint32_t arg) {
    GdxTrkOp* op = &sCur->ops[sCur->nOps++];
    op->kind = kind;
    op->n = n;
    op->slot = slot;
    op->tmpl = tmpl;
    op->arg = arg;
    return 1;
}

/* Verify mode: the real command is emitted too. First op of a run takes a START marker in
 * the slot and moves the real command to a fresh slot (2); otherwise the slot is reused (0). */
static int TrkVerifyReturn(void* slot, int firstOfRun, int advanced) {
    if (firstOfRun) {
        TrkWriteStartMarker(slot);
        return 2;
    }
    (void)advanced;
    return 0;
}

void gdxtrk_begin(int poolIndex, unsigned frame, int cameraIndex) {
    if (sMode < 0) {
        TrkReadConfig();
    }
    gdxtrk_active = 0;
    sBeginCalls++;
    if (sMode < 0 || (sMode == 0 && !sCensus)) {
        return;
    }
    if (poolIndex < 0 || poolIndex > 1) {
        if (!sInitLogged) {
            sInitLogged = 1;
            TrkLog("[trk] pool index %d out of range: native track disabled for this call", poolIndex);
        }
        return;
    }
    sPool = poolIndex;
    sCam = cameraIndex;
    sCur = &sRec[poolIndex];
    if (sCur->magic != GDXTRK_MAGIC || sCur->frame != frame) {
        sLastRp = sCur->rp; /* the replay of this pool's previous frame is complete by now */
        memset(&sCur->rp, 0, sizeof(sCur->rp));
        sCur->magic = GDXTRK_MAGIC;
        sCur->mode = (uint32_t)sMode;
        sCur->frame = frame;
        sCur->nOps = 0;
        sCur->nRuns = 0;
    }
    sRunStart = sCur->nOps;
    sLastWasVtxRecord = 0;
    sPendingCensus = 0;
    memset(&sC, 0, sizeof(sC));
    gdxtrk_active = 1;
}

void gdxtrk_end(void) {
    if (!gdxtrk_active) {
        return;
    }
    gdxtrk_active = 0;
    /* Verify mode (track=2) always emits the coverage receipt (vok/vbad): a verify run
     * without trkcensus=1 must not be a silent pass. */
    if ((sCensus || sMode == 2) && (sBeginCalls & 63u) == 1u) {
        if (sCensus) {
            TrkLog("[trk] cam=%d mode=%d pool=%d frame=%u vtx=%u verts=%u dl=%u tris=%u mod=%u runs=%u "
                   "state=%u/%u/%u/%u realdl=%u ovr=%u/%u fb=%u ops=%u/%u",
                   sCam, sMode, sPool, sCur->frame, sC.vtxOps, sC.verts, sC.dlOps, sC.tris, sC.modOps, sC.runs,
                   sC.state[0], sC.state[1], sC.state[2], sC.state[3], sC.realDl, sC.ovrReplace, sC.ovrNoop,
                   sC.fallback, sCur->nOps, sCur->nRuns);
        }
        if (sMode != 0) {
            TrkLog("[trk] replay(prev) markers=%u runs=%u vtx=%u verts=%u tris=%u fast=%u exact=%u draws=%u "
                   "comb=%u fb=%u vok=%u vbad=%u",
                   sLastRp.markers, sLastRp.runs, sLastRp.vtxOps, sLastRp.verts, sLastRp.tris, sLastRp.fast,
                   sLastRp.exact, sLastRp.draws, sLastRp.combines, sLastRp.fallback, sLastRp.verifyOk,
                   sLastRp.verifyBad);
        }
    }
}

int gdxtrk_pending(void) {
    return sMode > 0 && sCur != NULL && sCur->nOps > sRunStart;
}

int gdxtrk_marker(void* slot) {
    if (!gdxtrk_pending()) {
        return 0;
    }
    TrkWriteEndMarker(slot);
    sLastWasVtxRecord = 0;
    return 1;
}

int gdxtrk_state(void* slot, int kind) {
    if (kind >= 0 && kind < 4) {
        sC.state[kind]++;
    }
    sLastWasVtxRecord = 0;
    if (sMode == 0) {
        if (sPendingCensus != 0) {
            sC.runs++;
            sPendingCensus = 0;
        }
        return 0;
    }
    return gdxtrk_marker(slot);
}

int gdxtrk_vertex(void* slot, const void* vtx, int n, int v0, int advanced) {
    int firstOfRun;
    sC.vtxOps++;
    sC.verts += (uint32_t)n;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (n <= 0 || n > 32 || v0 < 0 || v0 + n > 32) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    if (!advanced) {
        /* Course_SetupChunkPipeTunnelJoinVtx: `gSPVertex(sCourseDisp - 1, …, 5, 16)` overwrites
           the previous slot — the 8-vertex load just recorded (replace the op), or a
           `gSPTexture ON` the stock game thereby DROPS (NOOP the real slot). */
        if (sLastWasVtxRecord && sCur->nOps > sRunStart && sCur->ops[sCur->nOps - 1].kind == GDXTRK_OP_VTX) {
            GdxTrkOp* op = &sCur->ops[sCur->nOps - 1];
            op->n = (uint8_t)n;
            op->slot = (uint8_t)v0;
            op->arg = (uint32_t)(uintptr_t)vtx;
            sC.ovrReplace++;
            return sMode == 2 ? 0 : 1;
        }
        if (!TrkCanRecord()) {
            return TrkFallbackReal(slot, advanced);
        }
        sC.ovrNoop++;
        if (sMode == 2) {
            TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
            sLastWasVtxRecord = 1;
            if (firstOfRun) {
                TrkWriteStartMarker(slot);
            } else {
                gdxtrk_gbi_noop(slot);
            }
            return 2;
        }
        gdxtrk_gbi_noop(slot);
        TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
        sLastWasVtxRecord = 1;
        return 1;
    }
    if (!TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
    sLastWasVtxRecord = 1;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}

int gdxtrk_dl(void* slot, const void* dl, int advanced) {
    int t;
    int firstOfRun;
    for (t = 0; t < GDXTRK_TEMPLATE_COUNT; t++) {
        if (sTemplatePtr[t] == (const unsigned char*)dl) {
            break;
        }
    }
    if (t == GDXTRK_TEMPLATE_COUNT) {
        /* Setup lists (D_8014008 / D_8014078): real, after any pending run's marker. */
        sC.realDl++;
        sLastWasVtxRecord = 0;
        if (sMode == 0) {
            if (sPendingCensus != 0) {
                sC.runs++;
                sPendingCensus = 0;
            }
            return 0;
        }
        return gdxtrk_marker(slot) ? 2 : 0;
    }
    sC.dlOps++;
    sC.tris += kGdxTrkGolden[t].ntris;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (!TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    TrkAppend(GDXTRK_OP_DL, 0, 0, (uint8_t)t, 0);
    sLastWasVtxRecord = 0;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}

int gdxtrk_modify(void* slot, int vtx, int where, uint32_t val, int advanced) {
    int firstOfRun;
    sC.modOps++;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (where != GDXTRK_G_MWO_POINT_ST || vtx < 0 || vtx >= 32 || !TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    TrkAppend(GDXTRK_OP_MOD, 0, (uint8_t)vtx, 0, val);
    sLastWasVtxRecord = 0;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}
