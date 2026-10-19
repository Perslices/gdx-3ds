/* gdx3ds_gad_replay.cpp -- [native] gadget_batch (C5 Stage 3b, gdx3ds_dress.h): the G_NOOP
 * p=0x6D marker replay of a gadget run.
 *
 * The gadget pass's recorded ops (gdx3ds_dress_sink.c) replay here, where the stock commands
 * stood. Vertex loads go through the interpreter's own SpVertex, ST patches through
 * SpModifyVertex, gSPTexture through SpTexture; triangles take the shared exact-first /
 * fast-rest path into a Gdx_NativeBatch (gdx3ds_native_tri.h), which flushes as ONE draw per
 * material run in emission order. The run's texture state was resolved once, by its first
 * triangle; every later triangle of the same state only transforms and packs. A gSPTexture that
 * changes the tile splits the batch (the next triangle resolves the new tile's texture through
 * the interpreter's triangle path), exactly as the [native] fx replay does.
 *
 * Verify (gadget_batch=2): the stock GBI runs too; the START marker snapshots the vertex cache
 * and the texture registers and arms the backend's stock-draw hash; the END marker flushes the
 * stock batch, restores the snapshot, replays the run into a native batch, hashes and DISCARDS
 * it, and compares. Bit-identical VBO bytes per run is the in-process parity proof. */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gdx3ds_dress.h"
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_native_tri.h"
#include "gdx3ds_nativebatch.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

using GdxNativeTri::RunCtx;

namespace {

struct GadSnapshot {
    GdxSeamVertex verts[GDXSEAM_VERTEX_CACHE_COUNT];
    uint8_t firstTile;
    uint16_t scaleS, scaleT;
    bool texChanged[2];
    bool valid;
};
GadSnapshot sSnap;
uint32_t sVerifyLogged = 0;
uint32_t sErrLogged = 0;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

void Snapshot(const GdxSeamRegs& R) {
    memcpy(sSnap.verts, R.vertices, sizeof(sSnap.verts));
    sSnap.firstTile = *R.first_tile_index;
    sSnap.scaleS = *R.tex_scale_s;
    sSnap.scaleT = *R.tex_scale_t;
    sSnap.texChanged[0] = R.textures_changed[0];
    sSnap.texChanged[1] = R.textures_changed[1];
    sSnap.valid = true;
}

void Restore(const GdxSeamRegs& R) {
    memcpy(R.vertices, sSnap.verts, sizeof(sSnap.verts));
    *R.first_tile_index = sSnap.firstTile;
    *R.tex_scale_s = sSnap.scaleS;
    *R.tex_scale_t = sSnap.scaleT;
    R.textures_changed[0] = sSnap.texChanged[0];
    R.textures_changed[1] = sSnap.texChanged[1];
    sSnap.valid = false;
}

void ReplayOp(RunCtx& c, GdxInterpSeam& I, GdxGadRecord* rec, const GdxGadOp& op) {
    const GdxSeamRegs& R = I.R();
    switch (op.kind) {
        case GDXGAD_OP_VTX:
            rec->rp.vtxOps++;
            rec->rp.verts += op.a;
            I.SpVertex(op.a, op.b, reinterpret_cast<const void*>(static_cast<uintptr_t>(op.arg)));
            break;
        case GDXGAD_OP_TRI:
            GdxNativeTri::Tri(c, op.a, op.b, op.c);
            break;
        case GDXGAD_OP_MOD:
            rec->rp.mods++;
            I.SpModifyVertex(op.a, op.b, op.arg);
            break;
        case GDXGAD_OP_TEX:
            if (*R.first_tile_index != op.a && Gdx_NativeBatchOpen()) {
                GdxNativeTri::FlushNative(c);
                rec->rp.splitTile++;
            }
            I.SpTexture(static_cast<uint16_t>(op.arg >> 16), static_cast<uint16_t>(op.arg & 0xFFFFu), op.b, op.a,
                        op.c);
            break;
        default:
            break;
    }
}

void NoteVerify(GdxGadRecord* rec, uint32_t run, const RunCtx& c, uint32_t stockTris, uint32_t stockHash) {
    const uint32_t nativeTris = c.stats_fast + c.stats_exact;
    const bool ok = (c.stats_fallback == 0) && (nativeTris == stockTris) && (c.verifyHash == stockHash);
    if (ok) {
        rec->rp.verifyOk++;
        return;
    }
    rec->rp.verifyBad++;
    if (sVerifyLogged++ < 12) {
        Log("[gad] verify MISMATCH frame=%u run=%u stock=(%u tris,%08x) native=(%u tris,%08x) fast=%u exact=%u fb=%u",
            rec->frame, run, stockTris, stockHash, nativeTris, c.verifyHash, c.stats_fast, c.stats_exact,
            c.stats_fallback);
    }
}

} // namespace

void gdx3ds_gad_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1) {
    GdxGadRecord* rec = reinterpret_cast<GdxGadRecord*>(w1);
    const uint32_t p = (w0 >> 16) & 0xFFu;
    const uint32_t run = w0 & 0xFFFFu;
    if (rec == nullptr || rec->magic != GDXGAD_MAGIC) {
        if (sErrLogged++ < 4) {
            Log("[gad] marker with bad record %p (w0=%08x)", static_cast<void*>(rec), (unsigned)w0);
        }
        return;
    }
    const bool verify = rec->mode == 2;
    const GdxSeamRegs& R = I.R();
    if (p == GDXGAD_MARKER_P_START) {
        if (verify) {
            I.Flush(); /* whatever preceded the run is drawn now; the stock hash covers the run only */
            Gdx_NativeBatchStockHash(nullptr, 1);
            Gdx_NativeBatchStockHashArm(1);
            Snapshot(R);
        }
        return;
    }
    if (p != GDXGAD_MARKER_P_END || run >= rec->nRuns) {
        return;
    }
    rec->rp.markers++;
    uint32_t stockTris = 0;
    uint32_t stockHash = 0;
    if (verify) {
        I.Flush();
        stockHash = Gdx_NativeBatchStockHash(&stockTris, 1);
        Gdx_NativeBatchStockHashArm(0);
        if (sSnap.valid) {
            Restore(R);
        } else {
            stockTris = 0xFFFFFFFFu; /* forces a mismatch receipt below */
        }
    } else if (I.PendingTris() != 0) {
        I.Flush(); /* the native batch must start at the pool cursor with nothing pending */
    }
    RunCtx c;
    GdxNativeTri::Init(c, &I, verify);
    const GdxGadRun& r = rec->runs[run];
    for (uint32_t k = r.opStart; k < r.opEnd && k < rec->nOps; k++) {
        ReplayOp(c, I, rec, rec->ops[k]);
    }
    GdxNativeTri::FlushNative(c);
    rec->rp.tris += c.stats_tris;
    rec->rp.fast += c.stats_fast;
    rec->rp.exact += c.stats_exact;
    rec->rp.fallback += c.stats_fallback;
    rec->rp.draws += c.stats_draws;
    if (verify) {
        NoteVerify(rec, run, c, stockTris, stockHash);
    }
}
