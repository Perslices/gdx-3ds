/* gdx3ds_fx_replay.cpp — [native] fx: the G_NOOP marker replay (gdx3ds_fx_marker_seam).
 *
 * The effects bracket's recorded ops (gdx3ds_fx_sink.c) are replayed here, at the G_NOOP
 * marker the shim wrote where the stock commands used to be. Vertex loads go through the
 * interpreter's own SpVertex (transform, aspect adjust, clip flags, fog factor, texture
 * scaling are its code), gSPTexture through SpTexture, prim/env through the RDP registers.
 * Triangles: exact-first / fast-rest into a Gdx_NativeBatch (gdx3ds_native_tri.h). The batch
 * flushes as ONE draw per material run, in emission order — the classes are translucent and
 * z-tested, so append order is the game's order and nothing is ever sorted.
 *
 * Where the stock path splits its batch on every prim/env VALUE change (the citro3d backend
 * binds those registers as TexEnv stage constants from vertex 0), the replay asks the backend
 * which combiner inputs actually ride as stage constants in the current shader
 * (Gdx_NativeBatchConstInputs) and, with [native] fx_sprites=1, keeps appending when the changed
 * register is carried per vertex instead (the fast path evaluates the register at append time,
 * exactly as the stock per-vertex pack does). Otherwise the batch splits and the reason is
 * counted (split=prim/env/tile). A gSPTexture tile change always splits (the next tri resolves
 * the tile's texture through the interpreter's triangle path).
 *
 * Interpreter access goes through GdxInterpSeam (gdx3ds_interp_seam.h): the same body runs
 * under the vendored LUS interpreter and under the owned walker.
 *
 * Verify ([native] fx_sink=2): the stock GBI runs too; the START marker snapshots the vertex
 * cache and the registers the run can touch and arms the backend's stock-draw hash, the END
 * marker flushes the stock batch, restores the snapshot, replays the run into a native batch,
 * hashes and DISCARDS it, and compares. Bit-identical VBO bytes per run is the parity proof. */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gdx3ds_fx_record.h"
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_native_tri.h"
#include "gdx3ds_nativebatch.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

using GdxNativeTri::RunCtx;

namespace {

struct RegSnapshot {
    GdxSeamVertex verts[GDXSEAM_VERTEX_CACHE_COUNT];
    GdxSeamRgba prim, env;
    uint8_t lodFrac;
    uint8_t firstTile;
    uint16_t scaleS, scaleT;
    bool texChanged[2];
    bool valid;
};
RegSnapshot sSnap;
uint32_t sVerifyLogged = 0;
uint32_t sErrLogged = 0;
/* B5 census: the combiner routing of each class, logged once. The whole fold question is
 * "which combiner input rides GPU_PRIMARY_COLOR" — everything else is a TexEnv stage constant
 * refreshed from vertex 0, so a value change of it must split the batch. */
uint32_t sRoutingLogged = 0;

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
    sSnap.prim = *R.prim_color;
    sSnap.env = *R.env_color;
    sSnap.lodFrac = *R.prim_lod_fraction;
    sSnap.firstTile = *R.first_tile_index;
    sSnap.scaleS = *R.tex_scale_s;
    sSnap.scaleT = *R.tex_scale_t;
    sSnap.texChanged[0] = R.textures_changed[0];
    sSnap.texChanged[1] = R.textures_changed[1];
    sSnap.valid = true;
}

void Restore(const GdxSeamRegs& R) {
    memcpy(R.vertices, sSnap.verts, sizeof(sSnap.verts));
    *R.prim_color = sSnap.prim;
    *R.env_color = sSnap.env;
    *R.prim_lod_fraction = sSnap.lodFrac;
    *R.first_tile_index = sSnap.firstTile;
    *R.tex_scale_s = sSnap.scaleS;
    *R.tex_scale_t = sSnap.scaleT;
    R.textures_changed[0] = sSnap.texChanged[0];
    R.textures_changed[1] = sSnap.texChanged[1];
    sSnap.valid = false;
}

enum Reg { REG_ENV = 0, REG_PRIM = 1 };

bool MuxIsReg(uint8_t mux, Reg reg, bool alphaChannel) {
    if (reg == REG_PRIM) {
        if (alphaChannel) {
            return mux == GDXSEAM_ACMUX_PRIMITIVE || mux == GDXSEAM_ACMUX_PRIM_LOD_FRAC;
        }
        return mux == GDXSEAM_CCMUX_PRIMITIVE || mux == GDXSEAM_CCMUX_PRIMITIVE_ALPHA ||
               mux == GDXSEAM_CCMUX_PRIM_LOD_FRAC;
    }
    if (alphaChannel) {
        return mux == GDXSEAM_ACMUX_ENVIRONMENT;
    }
    return mux == GDXSEAM_CCMUX_ENVIRONMENT || mux == GDXSEAM_CCMUX_ENV_ALPHA;
}

const char* MuxName(uint8_t mux, bool alphaChannel) {
    if (alphaChannel) {
        switch (mux) {
            case GDXSEAM_ACMUX_PRIMITIVE: return "PRIM.a";
            case GDXSEAM_ACMUX_ENVIRONMENT: return "ENV.a";
            case GDXSEAM_ACMUX_PRIM_LOD_FRAC: return "LODF";
            default: break;
        }
        return "?a";
    }
    switch (mux) {
        case GDXSEAM_CCMUX_PRIMITIVE: return "PRIM";
        case GDXSEAM_CCMUX_SHADE: return "SHADE";
        case GDXSEAM_CCMUX_SHADE_ALPHA: return "SHADE.a";
        case GDXSEAM_CCMUX_ENVIRONMENT: return "ENV";
        case GDXSEAM_CCMUX_PRIMITIVE_ALPHA: return "PRIM.a";
        case GDXSEAM_CCMUX_ENV_ALPHA: return "ENV.a";
        case GDXSEAM_CCMUX_PRIM_LOD_FRAC: return "LODF";
        default: break;
    }
    return "?";
}

/* One line per class: which input the vertex colour carries, which inputs are stage constants,
 * and the mux each input slot maps back to. This is the evidence for the B5 option choice. */
void LogRouting(RunCtx& c, uint8_t cls) {
    if ((sRoutingLogged >> cls) & 1u) {
        return;
    }
    if (!c.prepValid && !GdxNativeTri::DerivePrep(c)) {
        return;
    }
    uint32_t rgbMask = 0, alphaMask = 0;
    if (!Gdx_NativeBatchConstInputs(&rgbMask, &alphaMask)) {
        return;
    }
    sRoutingLogged |= 1u << cls;
    const Gdx3dsVboPackParams& pp = *c.ps.params;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[fx] routing cls=%u vtxRgb=%d vtxAlpha=%d constRgb=%02x constA=%02x "
                                       "wantVtxAlpha=%u fogToAlpha=%u inputs=",
                     (unsigned)cls, (int)pp.vtxColorInput, (int)pp.vtxAlphaInput, (unsigned)rgbMask,
                     (unsigned)alphaMask, (unsigned)pp.wantVtxAlpha, (unsigned)pp.fogFactorToAlpha);
    const uint8_t(*inputMap)[7] = c.ps.inputMap;
    for (int j = 0; j < 7 && n > 0 && n < (int)sizeof(buf); j++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %d:%s/%s", j + 1, MuxName(inputMap[0][j], false),
                      MuxName(inputMap[1][j], true));
    }
    Log("%s", buf);
}

/* True when a value change of `reg` needs no batch split: the open batch has a valid fast-path
 * state and no TexEnv stage constant of the current shader reads that register. */
bool RegisterFoldable(RunCtx& c, Reg reg) {
    if (!Gdx_NativeBatchOpen()) {
        return true; /* nothing pending: the change costs nothing either way */
    }
    if (!c.prepValid && !GdxNativeTri::DerivePrep(c)) {
        return false;
    }
    uint32_t rgbMask = 0, alphaMask = 0;
    if (!Gdx_NativeBatchConstInputs(&rgbMask, &alphaMask)) {
        return false;
    }
    const uint8_t(*inputMap)[7] = c.ps.inputMap;
    for (int j = 0; j < 7; j++) {
        const bool isConstRgb = (rgbMask >> j) & 1u;
        const bool isConstA = (alphaMask >> j) & 1u;
        if (isConstRgb && MuxIsReg(inputMap[0][j], reg, false)) {
            return false;
        }
        if (isConstA && MuxIsReg(inputMap[1][j], reg, true)) {
            return false;
        }
    }
    /* The register must reach the vertex only through the fast-path inputs the prep knows. */
    const Gdx3dsVboPackParams& pp = *c.ps.params;
    if (c.ai >= 0 && MuxIsReg(inputMap[1][c.ai], reg, true) && (!pp.wantVtxAlpha || pp.fogFactorToAlpha)) {
        return false;
    }
    return true;
}

} // namespace

void gdx3ds_fx_marker_seam(GdxInterpSeam& I, uint32_t w0, uintptr_t w1) {
    GdxFxRecord* rec = reinterpret_cast<GdxFxRecord*>(w1);
    const uint32_t p = (w0 >> 16) & 0xFFu;
    const uint32_t run = w0 & 0xFFFFu;
    if (rec == nullptr || rec->magic != GDXFX_MAGIC) {
        if (sErrLogged++ < 4) {
            Log("[fx] marker with bad record %p (w0=%08x)", static_cast<void*>(rec), (unsigned)w0);
        }
        return;
    }
    const bool verify = rec->mode == 2;
    const bool fold = rec->fold != 0;
    const bool foldAll = (rec->ceiling & 1u) != 0; /* census only: see GdxFxRecord.ceiling */
    const GdxSeamRegs& R = I.R();
    if (p == GDXFX_MARKER_P_START) {
        if (verify) {
            I.Flush(); /* whatever preceded the run is drawn now; the stock hash covers the run only */
            Gdx_NativeBatchStockHash(nullptr, 1);
            Gdx_NativeBatchStockHashArm(1);
            Snapshot(R);
        }
        return;
    }
    if (p != GDXFX_MARKER_P_END || run >= rec->nRuns) {
        return;
    }
    rec->rp.markers++;
    rec->rp.runs++;

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
    uint32_t foldPrim = 0, foldEnv = 0, splitPrim = 0, splitEnv = 0, splitTile = 0;

    const GdxFxRun& r = rec->runs[run];
    const bool census = rec->census != 0 && r.cls < GDXFX_CLASS_COUNT;
    for (uint32_t k = r.opStart; k < r.opEnd && k < rec->nOps; k++) {
        const GdxFxOp& op = rec->ops[k];
        switch (op.kind) {
            case GDXFX_OP_VTX:
                rec->rp.vtxOps++;
                rec->rp.verts += op.a;
                I.SpVertex(op.a, op.b, reinterpret_cast<const void*>(static_cast<uintptr_t>(op.arg)));
                break;
            case GDXFX_OP_TRI:
                GdxNativeTri::Tri(c, op.a, op.b, op.c);
                break;
            case GDXFX_OP_PRIM: {
                const uint8_t l = (uint8_t)(op.arg >> 8);
                const uint8_t a = (uint8_t)op.arg;
                GdxSeamRgba& pc = *R.prim_color;
                if (pc.r != op.a || pc.g != op.b || pc.b != op.c || pc.a != a || *R.prim_lod_fraction != l) {
                    if (census && Gdx_NativeBatchOpen()) {
                        LogRouting(c, r.cls);
                    }
                    if (foldAll || (fold && RegisterFoldable(c, REG_PRIM))) {
                        foldPrim += Gdx_NativeBatchOpen() ? 1 : 0;
                    } else if (Gdx_NativeBatchOpen()) {
                        GdxNativeTri::FlushNative(c);
                        splitPrim++;
                    }
                    pc.r = op.a;
                    pc.g = op.b;
                    pc.b = op.c;
                    pc.a = a;
                    *R.prim_lod_fraction = l;
                }
                break;
            }
            case GDXFX_OP_ENV: {
                const uint8_t a = (uint8_t)op.arg;
                GdxSeamRgba& ec = *R.env_color;
                if (ec.r != op.a || ec.g != op.b || ec.b != op.c || ec.a != a) {
                    if (census && Gdx_NativeBatchOpen()) {
                        LogRouting(c, r.cls);
                    }
                    if (foldAll || (fold && RegisterFoldable(c, REG_ENV))) {
                        foldEnv += Gdx_NativeBatchOpen() ? 1 : 0;
                    } else if (Gdx_NativeBatchOpen()) {
                        GdxNativeTri::FlushNative(c);
                        splitEnv++;
                    }
                    ec.r = op.a;
                    ec.g = op.b;
                    ec.b = op.c;
                    ec.a = a;
                }
                break;
            }
            case GDXFX_OP_TEX: {
                if (*R.first_tile_index != op.a && Gdx_NativeBatchOpen()) {
                    GdxNativeTri::FlushNative(c);
                    splitTile++;
                }
                I.SpTexture((uint16_t)(op.arg >> 16), (uint16_t)(op.arg & 0xFFFFu), op.b, op.a, op.c);
                break;
            }
            default:
                break;
        }
    }
    GdxNativeTri::FlushNative(c);

    rec->rp.tris += c.stats_tris;
    rec->rp.fast += c.stats_fast;
    rec->rp.exact += c.stats_exact;
    rec->rp.fallback += c.stats_fallback;
    rec->rp.draws += c.stats_draws;
    rec->rp.foldPrim += foldPrim;
    rec->rp.foldEnv += foldEnv;
    rec->rp.splitPrim += splitPrim;
    rec->rp.splitEnv += splitEnv;
    rec->rp.splitTile += splitTile;
    if (r.cls < GDXFX_CLASS_COUNT) {
        rec->rp.drawsByClass[r.cls] += c.stats_draws;
    }
    if (verify) {
        const uint32_t nativeTris = c.stats_fast + c.stats_exact;
        const bool ok = (c.stats_fallback == 0) && (nativeTris == stockTris) && (c.verifyHash == stockHash);
        if (ok) {
            rec->rp.verifyOk++;
        } else {
            rec->rp.verifyBad++;
            if (sVerifyLogged++ < 12) {
                Log("[fx] verify MISMATCH frame=%u run=%u cls=%u stock=(%u tris,%08x) native=(%u tris,%08x) fast=%u exact=%u fb=%u",
                    rec->frame, run, (unsigned)r.cls, stockTris, stockHash, nativeTris, c.verifyHash, c.stats_fast,
                    c.stats_exact, c.stats_fallback);
            }
        }
        /* The replay left the registers and the cache exactly as the stock run did. */
    }
}
