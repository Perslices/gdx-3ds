/* port/3ds/gfx/n64/gdx_hudsky_emit.inc -- [native] hudsky_emit: the compact emitter for retained
 * HUD runs (hudsky, docs/research/native/hudsky-progress.md). Included at the end of
 * gdx_hud_retain.cpp: it reads the retained record (HudRetRec, RetEv, the register groups),
 * whose types live in that file's anonymous namespace.
 *
 * WHY. On the Old 3DS the HUD costs ~5 ms per rendered frame for ~284k instructions at CPI 4.7,
 * 48% of the cycles stalled on instruction fetch (hwprof §10.1). hud_retain removed most of the
 * HUD's work items and saved nothing on the console, because a retained hit still walked a lot of
 * distinct code per run: the general fingerprint (a 46-way register switch, the same-content
 * plan), the replay's event switch issuing one virtual backend call per recorded call, the
 * 46-way register apply, and for every drained batch DrawTriangles' 11.5 KB.
 *
 * WHAT. Once a record is complete and proven (hud_retain's own fingerprint and verify), its
 * steady-state hit compiles once into a plan:
 *   - the register compare as a list of (live field, recorded field, bytes) ranges, plus the few
 *     groups that are not plain memory (the backend-held mRs, the open batch, the tri memo, the
 *     texture-to-load operand, cimg==zbuf), which keep their stock compare;
 *   - the register write-back as the same kind of range list (the same exceptions keep theirs);
 *   - the backend calls as ONE program (gdx3ds_hsx.h): the recorded setters, batch begins,
 *     float slices and draws, in the recorded order, handed to the backend in one call that runs
 *     them in one loop and drains each batch through a lean packed-draw tail.
 * A hit is then: one compact compare loop, the loads and LRU touches (unchanged), one backend
 * call, one compact apply loop.
 *
 * EXACTNESS. The plan is a re-encoding of the record, not a new decision: the compare ranges
 * test the same fields byte for byte (the stock compares are bytewise or field-wise on padding-free
 * fields), the program is the recorded call list (EV_REFRESH dropped -- the stock replay skips it
 * too; the atlas arm moved to the end -- the stock replay applies only its last value there), the
 * apply ranges write the same fields. Variant hits (only rectangle geometry moved) still go through
 * HudRetainVariant, which rewrites the record's floats in place; the program indexes the floats,
 * so it needs no recompile. Anything the plan does not cover -- a run with foreign spans, a
 * backend that cannot run programs, a diagnostic armed in the backend -- takes the stock replay.
 *
 * VERIFY ([native] hudsky_emit=2). Every exact hit runs both fingerprints and compares the
 * answers; the stock replay draws; the plan's program is decoded against the record's call list
 * op by op; after the stock register apply the compiled apply is run on top and every group must
 * be unchanged. `[hsx] vfy ... bad=0` is the in-process proof; the cross-leg proof is the [fh] /
 * SHOT / [gstate] gate plus `[hsxg]`, the hash of every draw's GPU command words. */

namespace {

struct HsxField {
    int32_t live; /* byte offset of the live field from the Interp */
    uint16_t rec; /* byte offset of the recorded field in HudRegs */
    uint16_t n;   /* bytes */
};

constexpr int kHsxMaxFieldsPerGroup = 8;

/* Fixed-size copies for the sizes the field table produces (no libc call on the hit path). */
inline void HsxCopy(uint8_t* d, const uint8_t* s, uint32_t n) {
    switch (n) {
        case 1: d[0] = s[0]; return;
        case 2: memcpy(d, s, 2); return;
        case 4: memcpy(d, s, 4); return;
        case 8: memcpy(d, s, 8); return;
        case 12: memcpy(d, s, 12); return;
        case 16: memcpy(d, s, 16); return;
        default: memcpy(d, s, n); return;
    }
}

inline bool HsxSame(const uint8_t* a, const uint8_t* b, uint32_t n) {
    switch (n) {
        case 1: return a[0] == b[0];
        case 2: {
            uint16_t x, y;
            memcpy(&x, a, 2);
            memcpy(&y, b, 2);
            return x == y;
        }
        case 4: {
            uint32_t x, y;
            memcpy(&x, a, 4);
            memcpy(&y, b, 4);
            return x == y;
        }
        case 8: {
            uint32_t x[2], y[2];
            memcpy(x, a, 8);
            memcpy(y, b, 8);
            return x[0] == y[0] && x[1] == y[1];
        }
        case 12: {
            uint32_t x[3], y[3];
            memcpy(x, a, 12);
            memcpy(y, b, 12);
            return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
        }
        case 16: {
            uint32_t x[4], y[4];
            memcpy(x, a, 16);
            memcpy(y, b, 16);
            return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
        }
        default: return memcmp(a, b, n) == 0;
    }
}

/* One record's compiled plan (HudRetRec::hsx). */
struct HsxPlan {
    PodVec<HsxField> cmp;
    PodVec<HsxField> apply;
    PodVec<Gdx3dsHsxOp> ops;
    PodVec<uint16_t> liveOps; /* ops whose value is taken from a live register at the hit */
    uint64_t cmpSlow;         /* groups compared by HudRegSameLive */
    uint64_t applySlow;       /* groups applied by HudRegApply */
    bool anyImm;              /* an immutable load: the same-content plan must be asked */
    bool tailBegun;           /* C6: the batch the run leaves open was begun by it */
};

} // namespace

struct Interp::HsxStats {
    uint32_t hits, compiles, compileFail, stockFp, stockReplay, refused, fpMiss;
    uint32_t vfyRuns, vfyOk, vfyBad, vfyFp, vfyOps, vfyApply;
    uint32_t sesVfyBad, sesVfyRuns;
    uint64_t ticksHit;
    uint32_t ops, draws;
    /* hudclock: whole HUD runs (HudReplayRun), by path: 0 executed, 1 replayed, 2 variant */
    uint64_t runTicks[3];
    uint32_t runN[3];
    /* hudclock=2: the phases of an emitter hit run (HsxMark), charged when the run was a hit */
    uint64_t phLast, phCur[8], phSum[8];
    uint32_t phRuns, phReads;
    double readUs; /* one clock read, measured at first use */
};

/* The fields of one register group, as (live, recorded, bytes) ranges; -1 = not plain memory
 * (the stock compare / apply keeps it). */
GDX_COLD int Interp::HsxGroupFields(int g, void* outv) const {
    HsxField* out = static_cast<HsxField*>(outv);
    const uint8_t* const self = reinterpret_cast<const uint8_t*>(this);
    int k = 0;
    const auto add = [&](const void* live, size_t recOff, size_t n) {
        out[k].live = static_cast<int32_t>(static_cast<const uint8_t*>(live) - self);
        out[k].rec = static_cast<uint16_t>(recOff);
        out[k].n = static_cast<uint16_t>(n);
        k++;
    };
#define HSX_F(liveField, recField)                                                                      \
    do {                                                                                                \
        static_assert(sizeof(liveField) == sizeof(HudRegs::recField), "field size"); \
        add(&(liveField), offsetof(HudRegs, recField), sizeof(liveField));                              \
    } while (0)
    if (g >= RG_TILES0) {
        const int t = g - RG_TILES0;
        const Tile& tl = mRdp.texture_tile[t];
        const size_t base = offsetof(HudRegs, tiles) + static_cast<size_t>(t) * sizeof(Tile);
        add(&tl.uls, base + offsetof(Tile, uls), 4u * sizeof(float));
        return k;
    }
    if (g >= RG_TILEF0) {
        const int t = g - RG_TILEF0;
        const Tile& tl = mRdp.texture_tile[t];
        const size_t base = offsetof(HudRegs, tiles) + static_cast<size_t>(t) * sizeof(Tile);
        add(&tl.fmt, base + offsetof(Tile, fmt), 8); /* fmt siz cms cmt masks maskt shifts shiftt */
        add(&tl.tmem, base + offsetof(Tile, tmem), sizeof(tl.tmem));
        add(&tl.line_size_bytes, base + offsetof(Tile, line_size_bytes), sizeof(tl.line_size_bytes));
        add(&tl.palette, base + offsetof(Tile, palette), sizeof(tl.palette));
        add(&tl.tmem_index, base + offsetof(Tile, tmem_index), sizeof(tl.tmem_index));
        return k;
    }
    switch (g) {
        case RG_OM:
            HSX_F(mRdp.other_mode_l, omL);
            HSX_F(mRdp.other_mode_h, omH);
            return k;
        case RG_CC: HSX_F(mRdp.combine_mode, cc); return k;
        case RG_PRIM:
            HSX_F(mRdp.prim_color, prim);
            HSX_F(mRdp.prim_lod_fraction, primLod);
            return k;
        case RG_ENV: HSX_F(mRdp.env_color, env); return k;
        case RG_FOG: HSX_F(mRdp.fog_color, fog); return k;
        case RG_BLEND: HSX_F(mRdp.blend_color, blend); return k;
        case RG_FILL: HSX_F(mRdp.fill_color, fill); return k;
        case RG_PDEPTH: HSX_F(mRdp.prim_depth, pdepth); return k;
        case RG_KEY:
            HSX_F(mRdp.key_center, keyC);
            HSX_F(mRdp.key_scale, keyS);
            return k;
        case RG_CONV: HSX_F(mRdp.convert_k, conv); return k;
        case RG_SCIS: HSX_F(mRdp.scissor, scis); return k;
        case RG_VP: HSX_F(mRdp.viewport, vp); return k;
        case RG_VSC: HSX_F(mRdp.viewport_or_scissor_changed, vsc); return k;
        case RG_GRAY:
            HSX_F(mRdp.grayscale, gray);
            HSX_F(mRdp.grayscale_color, grayCol);
            return k;
        case RG_FTI: HSX_F(mRdp.first_tile_index, fti); return k;
        case RG_TXC: HSX_F(mRdp.textures_changed, txc); return k;
        case RG_GEOM: HSX_F(mRsp.geometry_mode, geom); return k;
        case RG_XGEOM: HSX_F(mRsp.extra_geometry_mode, xgeom); return k;
        case RG_TSCALE:
            HSX_F(mRsp.texture_scaling_factor.s, tsS);
            HSX_F(mRsp.texture_scaling_factor.t, tsT);
            return k;
        case RG_FOGMW:
            HSX_F(mRsp.fog_mul, fogMul);
            HSX_F(mRsp.fog_offset, fogOff);
            return k;
        case RG_LVXY:
        case RG_LVUV:
        case RG_LVCOL:
        case RG_LVCLIP:
            for (int i = 0; i < 4; i++) {
                const LoadedVertex& v = mRsp.loaded_vertices[kMaxVertices + i];
                const size_t base = offsetof(HudRegs, lv) + static_cast<size_t>(i) * sizeof(LoadedVertex);
                if (g == RG_LVXY) {
                    add(&v.x, base + offsetof(LoadedVertex, x), 4u * sizeof(float));
                } else if (g == RG_LVUV) {
                    add(&v.u, base + offsetof(LoadedVertex, u), 2u * sizeof(float));
                } else if (g == RG_LVCOL) {
                    add(&v.color, base + offsetof(LoadedVertex, color), sizeof(Rgba));
                } else {
                    add(&v.clip_rej, base + offsetof(LoadedVertex, clip_rej), 2);
                }
            }
            return k;
        default: return -1; /* ttl, zci, rs0, rs1, pack, trimemo */
    }
#undef HSX_F
}

/* Compile the plan of a valid record. False when it cannot be expressed (the stock path stays). */
GDX_COLD bool Interp::HsxCompile(HudRetRec& rec) {
    HsxPlan* P = static_cast<HsxPlan*>(rec.hsx);
    if (P == nullptr) {
        P = static_cast<HsxPlan*>(calloc(1, sizeof(HsxPlan)));
        if (P == nullptr) {
            return false;
        }
        rec.hsx = P;
    }
    P->cmp.n = P->apply.n = P->ops.n = P->liveOps.n = 0;
    P->cmpSlow = P->applySlow = 0;
    P->anyImm = false;
    HsxField f[kHsxMaxFieldsPerGroup];
    for (uint64_t m = rec.cmpMask; m != 0; m &= m - 1u) {
        const int g = __builtin_ctzll(m);
        const int nf = HsxGroupFields(g, f);
        if (nf < 0) {
            P->cmpSlow |= Bit(g);
            continue;
        }
        for (int i = 0; i < nf; i++) {
            if (!P->cmp.Push(f[i])) {
                return false;
            }
        }
    }
    for (uint64_t m = rec.writeMask; m != 0; m &= m - 1u) {
        const int g = __builtin_ctzll(m);
        if (g == RG_PACK || g == RG_ZCI) {
            continue; /* the pack state is applied with the live cursor; zci is never written */
        }
        const int nf = HsxGroupFields(g, f);
        if (nf < 0) {
            P->applySlow |= Bit(g);
            continue;
        }
        for (int i = 0; i < nf; i++) {
            if (!P->apply.Push(f[i])) {
                return false;
            }
        }
    }
    for (uint32_t k = 0; k < rec.loads.n; k++) {
        P->anyImm = P->anyImm || rec.loads.d[k].imm != 0;
    }
    int armLast = -1;
    bool begun = false; /* C6: the open batch was begun by this run (HudRetainReplay's begunHere) */
    for (uint32_t k = 0; k < rec.evs.n; k++) {
        const RetEv& e = rec.evs.d[k];
        Gdx3dsHsxOp o;
        memset(&o, 0, sizeof(o));
        o.a = e.a;
        o.b = e.b;
        o.c = e.c;
        memcpy(o.i, e.i, sizeof(o.i));
        o.f = e.f;
        o.p = e.p;
        switch (e.op) {
            case EV_UNLOAD: o.op = GDX3DS_HSX_UNLOAD; break;
            case EV_LOAD: o.op = GDX3DS_HSX_LOAD; break;
            case EV_SELTEX: o.op = GDX3DS_HSX_SELTEX; break;
            case EV_SAMPLER: o.op = GDX3DS_HSX_SAMPLER; break;
            case EV_DEPTH: o.op = GDX3DS_HSX_DEPTH; break;
            case EV_DECAL: o.op = GDX3DS_HSX_DECAL; break;
            case EV_VIEWPORT: o.op = GDX3DS_HSX_VIEWPORT; break;
            case EV_SCISSOR: o.op = GDX3DS_HSX_SCISSOR; break;
            case EV_ALPHA: o.op = GDX3DS_HSX_ALPHA; break;
            case EV_PRIMDEPTH: o.op = GDX3DS_HSX_PRIMDEPTH; break;
            case EV_ALPHATHR: o.op = GDX3DS_HSX_ALPHATHR; break;
            case EV_FOGNOTE: o.op = GDX3DS_HSX_FOGNOTE; break;
            case EV_CLEARDEPTH: o.op = GDX3DS_HSX_CLEARDEPTH; break;
            case EV_BEGIN: o.op = GDX3DS_HSX_BEGIN; break;
            case EV_FLOATS: o.op = GDX3DS_HSX_FLOATS; break;
            case EV_DRAW: o.op = GDX3DS_HSX_DRAW; break;
            case EV_ATLASARM: armLast = static_cast<int>(e.a); continue;
            case EV_REFRESH: continue;
            default: return false;
        }
        if ((e.op == EV_PRIMDEPTH || e.op == EV_ALPHATHR) && e.c != 0) {
            if (!P->liveOps.Push(static_cast<uint16_t>(P->ops.n))) {
                return false;
            }
        }
        if (e.op == EV_BEGIN) {
            begun = true;
        } else if (e.op == EV_DRAW) {
            o.screen = begun ? 1 : 0;
            begun = false;
        }
        if (!P->ops.Push(o)) {
            return false;
        }
    }
    P->tailBegun = begun;
    if (armLast >= 0) {
        Gdx3dsHsxOp o;
        memset(&o, 0, sizeof(o));
        o.op = GDX3DS_HSX_ATLASARM;
        o.a = static_cast<uint8_t>(armLast);
        if (!P->ops.Push(o)) {
            return false;
        }
    }
    rec.hsxValid = true;
    mHsx->compiles++;
    return true;
}

/* HudRetainFingerprint for an exact-words hit, with the register compare compiled. Same checks,
 * same order, same miss reasons. */
bool Interp::HsxFingerprint(HudRetRec& rec, int* why) {
    HudRetain& R = *mHudR;
    const HsxPlan& P = *static_cast<const HsxPlan*>(rec.hsx);
    const uint8_t* const self = reinterpret_cast<const uint8_t*>(this);
    const uint8_t* const entry = reinterpret_cast<const uint8_t*>(&rec.entry);
    for (uint32_t k = 0; k < P.cmp.n; k++) {
        const HsxField& f = P.cmp.d[k];
        if (!HsxSame(self + f.live, entry + f.rec, f.n)) {
            R.lastGroup = 0xFE; /* a compiled range (the group is not named on this path) */
            *why = RM_REGS;
            return false;
        }
    }
    for (uint64_t m = P.cmpSlow; m != 0; m &= m - 1u) {
        const int g = __builtin_ctzll(m);
        if (!HudRegSameLive(rec.entry, g)) {
            R.wRegMiss[g]++;
            R.lastGroup = static_cast<uint8_t>(g);
            *why = RM_REGS;
            return false;
        }
    }
    if (rec.timgEpoch != mSrc.TimgClassEpoch() || rec.atlasEpoch != mBackend->AtlasEpoch() ||
        rec.texRoom != (mTexCount + 1 < kTexCacheSize) || memcmp(&rec.cfg, &mCfg, sizeof(mCfg)) != 0) {
        *why = RM_EXT;
        return false;
    }
    for (int u = 0; u < 2; u++) {
        const int16_t s = mRs.textures[u];
        const uint32_t id = s >= 0 ? mTex[s].texture_id : 0xFFFFFFFFu;
        if (((rec.cmpMask >> (u == 0 ? RG_RS0 : RG_RS1)) & 1u) != 0 && id != rec.entryTexId[u]) {
            *why = RM_TEX;
            return false;
        }
    }
    for (uint32_t k = 0; k < rec.imps.n; k++) {
        const RetImp& im = rec.imps.d[k];
        const TexEntry& e = mTex[im.slot];
        if (!e.used || !e.uploaded || e.texture_id != im.texId || memcmp(&e.key, &im.key, sizeof(TexKey)) != 0) {
            *why = RM_TEX;
            return false;
        }
    }
    uint64_t now[2];
    gdx_gfx_tmem_source_stamp(now);
    if (P.anyImm && !HudRetainSameContentPlan(rec)) {
        *why = RM_TEX;
        return false;
    }
    for (uint32_t k = 0; k < rec.preds.n; k++) {
        const RetPred& pr = rec.preds.d[k];
        const GdxHudWord& w = rec.words.d[pr.word];
        if (HudColourSame(pr.op, w.w0, w.w1) != (pr.equal != 0)) {
            R.wRegMiss[RG_PRIM]++;
            R.lastGroup = 0xFF;
            *why = RM_REGS;
            return false;
        }
    }
    for (uint32_t k = 0; k < rec.srcs.n; k++) {
        const RetSrc& sr = rec.srcs.d[k];
        if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(sr.addr), sr.n, sr.cls, rec.srcStamp) == 0) {
            *why = RM_SRC;
            return false;
        }
    }
    const size_t needFloats = (static_cast<size_t>(rec.tris) + static_cast<size_t>(kMaxTriBuffer)) * kPackTriFloats;
    if (mBackend->VboFreeFloats() < needFloats) {
        *why = RM_HEAD;
        return false;
    }
    rec.srcStamp[0] = now[0];
    rec.srcStamp[1] = now[1];
    *why = RM_NONE;
    return true;
}

/* The compiled register write-back: the plain groups by range, the rest by the stock apply. */
void Interp::HsxApply(const HudRetRec& rec) {
    const HsxPlan& P = *static_cast<const HsxPlan*>(rec.hsx);
    uint8_t* const self = reinterpret_cast<uint8_t*>(this);
    const uint8_t* const exit = reinterpret_cast<const uint8_t*>(&rec.exit);
    for (uint32_t k = 0; k < P.apply.n; k++) {
        const HsxField& f = P.apply.d[k];
        HsxCopy(self + f.live, exit + f.rec, f.n);
    }
    for (uint64_t m = P.applySlow; m != 0; m &= m - 1u) {
        HudRegApply(rec.exit, __builtin_ctzll(m));
    }
}

/* The program through the Backend's virtual setters, one call per op: what HudRetainReplay's
 * event loop issues. Used when the backend cannot run programs (the host mock) or refuses
 * (a backend diagnostic is armed). */
GDX_COLD void Interp::HsxRunGeneric(Gdx3dsHsxProgram* pr) {
    float* cursor = pr->cursor;
    for (uint32_t k = 0; k < pr->nOps; k++) {
        const Gdx3dsHsxOp& o = pr->ops[k];
        switch (o.op) {
            case GDX3DS_HSX_UNLOAD: mBackend->UnloadShader(const_cast<void*>(o.p)); break;
            case GDX3DS_HSX_LOAD: mBackend->LoadShader(const_cast<void*>(o.p)); break;
            case GDX3DS_HSX_SELTEX: mBackend->SelectTexture(o.i[0], static_cast<uint32_t>(o.i[1])); break;
            case GDX3DS_HSX_SAMPLER:
                mBackend->SetSamplerParameters(o.i[0], o.a != 0, static_cast<uint32_t>(o.i[1]),
                                               static_cast<uint32_t>(o.i[2]));
                break;
            case GDX3DS_HSX_DEPTH: mBackend->SetDepthTestAndMask(o.a != 0, o.b != 0); break;
            case GDX3DS_HSX_DECAL: mBackend->SetZmodeDecal(o.a != 0); break;
            case GDX3DS_HSX_VIEWPORT: mBackend->SetViewport(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_SCISSOR: mBackend->SetScissor(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_ALPHA: mBackend->SetUseAlpha(o.a != 0); break;
            case GDX3DS_HSX_PRIMDEPTH: mBackend->SetCurrentPrimDepth(o.f); break;
            case GDX3DS_HSX_ALPHATHR: mBackend->SetCurrentAlphaCompareThreshold(o.f); break;
            case GDX3DS_HSX_FOGNOTE: mBackend->FogNoteParams(o.i[0], o.i[1], o.i[2]); break;
            case GDX3DS_HSX_CLEARDEPTH: mBackend->ClearDepthRegion(o.i[0], o.i[1], o.i[2], o.i[3]); break;
            case GDX3DS_HSX_ATLASARM: mBackend->AtlasArm(o.a != 0); break;
            case GDX3DS_HSX_BEGIN: {
                Gdx3dsVboPackParams scratch;
                uint32_t cap = 0;
                cursor = mBackend->VboPackBegin(&scratch, &cap);
                pr->begins++;
                break;
            }
            case GDX3DS_HSX_FLOATS:
                if (cursor != nullptr) {
                    memcpy(cursor + static_cast<size_t>(o.i[0]) * kPackTriFloats, pr->floats + o.i[2],
                           static_cast<size_t>(o.i[1]) * kPackTriFloats * sizeof(float));
                }
                break;
            case GDX3DS_HSX_DRAW:
                if (gdx3ds_replay_interp != nullptr) {
                    gdx3ds_replay_interp->setDrawTag(o.screen != 0 ? static_cast<uint16_t>(GDX_REPLAY_TAG_SCREEN)
                                                                   : pr->liveTag);
                }
                mBackend->DrawPackedBatch(static_cast<size_t>(o.i[0]), &pr->auxs[o.i[1]]);
                pr->draws++;
                cursor = nullptr;
                break;
            default: break;
        }
    }
    pr->cursor = cursor;
}

/* A hit (exact or variant) through the plan: the loads and LRU touches (unchanged), ONE backend
 * program, the compiled apply, the pack state, the [hudr] hit counters. */
void Interp::HsxReplay(HudRetRec& rec) {
    HudRetain& R = *mHudR;
    HsxPlan& P = *static_cast<HsxPlan*>(rec.hsx);
    const bool clk = mHudClock != 0 || mHudRetClock != 0;
    const uint64_t t0 = clk ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    for (uint32_t k = 0; k < P.liveOps.n; k++) {
        Gdx3dsHsxOp& o = P.ops.d[P.liveOps.d[k]];
        /* == Flush's expressions; the stock replay passes the live register the same way */
        o.f = o.op == GDX3DS_HSX_PRIMDEPTH ? static_cast<float>(mRdp.prim_depth) / 32767.0f
                                           : static_cast<float>(mRdp.blend_color.a) / 255.0f;
    }
    for (uint32_t k = 0; k < rec.loads.n; k++) {
        HudDirectApplyLoad(rec.loads.d[k], false);
    }
    for (uint32_t k = 0; k < rec.imps.n; k++) {
        TexCacheTouch(rec.imps.d[k].slot);
    }
    HsxMark(3);
    Gdx3dsHsxProgram prog;
    prog.ops = P.ops.d;
    prog.nOps = P.ops.n;
    prog.floats = rec.floats.d;
    prog.auxs = rec.auxs.d;
    prog.cursor = mPackDst;
    prog.liveTag = mReplayBatchTag;
    prog.begins = 0;
    prog.draws = 0;
    if (!mBackend->HsxRun(&prog)) {
        mHsx->refused++;
        prog.cursor = mPackDst;
        prog.begins = 0;
        prog.draws = 0;
        HsxRunGeneric(&prog);
    }
    HsxMark(4);
    mStats.triBegin += prog.begins;
    mStats.batches += prog.draws;
    HsxApply(rec);
    if (((rec.writeMask >> RG_PACK) & 1u) != 0) {
        mBufNumTris = rec.exit.bufNumTris;
        mPackBatch = rec.exit.packBatch;
        mPackCapTris = rec.exit.packCap;
        mPackComb = rec.exit.packComb;
        mPackFolded = rec.exit.packFolded;
        if (rec.paramsWritten) {
            mPackParams = rec.exit.params;
        }
        if (rec.auxWritten) {
            mPackAux = rec.exit.aux;
        }
        mPackDst = mBufNumTris > 0 ? prog.cursor : nullptr;
        if (P.tailBegun) {
            mReplayBatchTag = GDX_REPLAY_TAG_SCREEN; /* C6: the batch the run left open is its own */
        }
    }
    HsxMark(5);
    if (clk) {
        mHsx->ticksHit += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
    }
    mHsx->hits++;
    mHsx->ops += prog.nOps;
    mHsx->draws += prog.draws;
    R.wHit++;
    R.wHitRects += rec.rects;
    R.wHitLoads += rec.loads.n;
    R.wHitImps += rec.imps.n;
    R.wHitDraws += rec.draws;
    R.wHitTris += rec.tris;
}

/* Verify: the plan against the record, for a hit the stock path is about to replay. */
GDX_COLD void Interp::HsxVerifyPlan(HudRetRec& rec, bool stockHit, bool hsxHit) {
    HsxStats& S = *mHsx;
    const HsxPlan& P = *static_cast<const HsxPlan*>(rec.hsx);
    bool bad = false;
    if (stockHit != hsxHit) {
        S.vfyFp++;
        bad = true;
    }
    /* the program, decoded against the call list */
    uint32_t j = 0;
    int armLast = -1;
    bool opsOk = true;
    bool vBegun = false;
    for (uint32_t k = 0; k < rec.evs.n && opsOk; k++) {
        const RetEv& e = rec.evs.d[k];
        if (e.op == EV_REFRESH) {
            continue;
        }
        if (e.op == EV_ATLASARM) {
            armLast = e.a;
            continue;
        }
        if (j >= P.ops.n) {
            opsOk = false;
            break;
        }
        const Gdx3dsHsxOp& o = P.ops.d[j++];
        static const uint8_t kMap[EV_COUNT] = { GDX3DS_HSX_UNLOAD,    GDX3DS_HSX_LOAD,      GDX3DS_HSX_SELTEX,
                                                GDX3DS_HSX_SAMPLER,   GDX3DS_HSX_DEPTH,     GDX3DS_HSX_DECAL,
                                                GDX3DS_HSX_VIEWPORT,  GDX3DS_HSX_SCISSOR,   GDX3DS_HSX_ALPHA,
                                                GDX3DS_HSX_PRIMDEPTH, GDX3DS_HSX_ALPHATHR,  GDX3DS_HSX_FOGNOTE,
                                                GDX3DS_HSX_ATLASARM,  0xFF,                 GDX3DS_HSX_BEGIN,
                                                GDX3DS_HSX_FLOATS,    GDX3DS_HSX_DRAW,      GDX3DS_HSX_CLEARDEPTH };
        const bool live = (e.op == EV_PRIMDEPTH || e.op == EV_ALPHATHR) && e.c != 0;
        if (e.op == EV_BEGIN) {
            vBegun = true;
        }
        const uint8_t wantScreen = e.op == EV_DRAW ? (vBegun ? 1 : 0) : 0;
        if (e.op == EV_DRAW) {
            vBegun = false;
        }
        opsOk = o.op == kMap[e.op] && o.a == e.a && o.b == e.b && o.c == e.c && memcmp(o.i, e.i, sizeof(o.i)) == 0 &&
                o.p == e.p && (live || memcmp(&o.f, &e.f, sizeof(float)) == 0) && o.screen == wantScreen;
    }
    if (opsOk && armLast >= 0) {
        opsOk = j < P.ops.n && P.ops.d[j].op == GDX3DS_HSX_ATLASARM && P.ops.d[j].a == armLast;
        j++;
    }
    opsOk = opsOk && j == P.ops.n;
    if (!opsOk) {
        S.vfyOps++;
        bad = true;
    }
    S.vfyRuns++;
    S.sesVfyRuns++;
    if (bad) {
        S.vfyBad++;
        S.sesVfyBad++;
    } else {
        S.vfyOk++;
    }
}

/* Verify, after the stock apply: the compiled apply on top must change nothing. */
GDX_COLD void Interp::HsxVerifyApply(const HudRetRec& rec) {
    HsxStats& S = *mHsx;
    HudRegs* before = static_cast<HudRegs*>(malloc(sizeof(HudRegs)));
    HudRegs* after = static_cast<HudRegs*>(malloc(sizeof(HudRegs)));
    if (before == nullptr || after == nullptr) {
        free(before);
        free(after);
        return;
    }
    HudRegCapture(*before);
    HsxApply(rec);
    HudRegCapture(*after);
    bool same = true;
    for (int g = 0; g < RG_COUNT && same; g++) {
        same = HudRegSame(*before, *after, g);
    }
    if (!same) {
        S.vfyApply++;
        S.vfyBad++;
        S.sesVfyBad++;
        if (S.vfyOk > 0) {
            S.vfyOk--;
        }
    }
    free(before);
    free(after);
}

bool Interp::HsxEnsure() {
    if (mHsx == nullptr) {
        mHsx = static_cast<HsxStats*>(calloc(1, sizeof(HsxStats)));
    }
    return mHsx != nullptr;
}

/* Ready the plan of a record (compiled once per recording). False = the stock path keeps it. */
bool Interp::HsxPlanReady(HudRetRec& rec) {
    if (!HsxEnsure() || rec.foreign.n != 0) {
        return false; /* foreign spans: the stock fingerprint's span checks are not compiled */
    }
    if (!rec.hsxValid && !HsxCompile(rec)) {
        mHsx->compileFail++;
        return false;
    }
    return true;
}

/* The exact-words hit decision of HudRetainBegin under [native] hudsky_emit (hud_retain=1).
 * Returns -1 when the plan does not cover the record (the stock fingerprint decides); otherwise
 * the hit answer in *hit (level 2: the STOCK answer, after both were computed and compared). */
int Interp::HsxExactFingerprint(HudRetRec& rec, const void* runWords, uint32_t n, int* why, bool* hit) {
    if (!HsxPlanReady(rec)) {
        return -1;
    }
    if (mHudSkyEmit == 2) {
        /* both fingerprints; the stock one runs last, so its source-stamp update is the one that stands */
        int whyH = RM_NONE;
        const uint64_t s0 = rec.srcStamp[0], s1 = rec.srcStamp[1];
        const bool hsxHit = HsxFingerprint(rec, &whyH);
        rec.srcStamp[0] = s0;
        rec.srcStamp[1] = s1;
        *hit = HudRetainFingerprint(rec, runWords, n, false, why);
        HsxVerifyPlan(rec, *hit, hsxHit);
        return 2;
    }
    HsxMark(1);
    *hit = HsxFingerprint(rec, why);
    HsxMark(2);
    if (!*hit) {
        mHsx->fpMiss++;
    }
    return 1;
}

/* hudclock=2: phase clock of the emitter's hit runs. Phase k gets the time since the previous
 * mark; HsxPhaseEnd(true) banks the run (a hit), false drops it. */
GDX_COLD void Interp::HsxMarkSlow(int phase) {
    if (mHudClock < 2 || mHudSkyEmit == 0 || !HsxEnsure()) {
        return;
    }
    HsxStats& S = *mHsx;
    const uint64_t now = static_cast<uint64_t>(gdx_ninterp_now());
    if (phase < 0) {
        memset(S.phCur, 0, sizeof(S.phCur));
        S.phReads = 1;
    } else if (phase < 8) {
        S.phCur[phase] += now - S.phLast;
        S.phReads++;
    }
    S.phLast = now;
}

GDX_COLD void Interp::HsxPhaseEnd(bool retained) {
    if (mHudClock < 2 || mHudSkyEmit == 0 || mHsx == nullptr || mHudR == nullptr) {
        return;
    }
    const bool hit = retained && mHudR->lastPath == 1;
    HsxMark(6);
    HsxStats& S = *mHsx;
    if (S.readUs == 0.0) {
        const uint64_t a = static_cast<uint64_t>(gdx_ninterp_now());
        const uint64_t b = static_cast<uint64_t>(gdx_ninterp_now());
        S.readUs = static_cast<double>(b - a) / 268.111856;
    }
    if (hit) {
        for (int k = 0; k < 8; k++) {
            S.phSum[k] += S.phCur[k];
        }
        S.phRuns++;
    }
}

/* hudclock: one HUD run's whole time (HudReplayRun), charged to the path it took. */
GDX_COLD void Interp::HsxNoteRun(bool retained, uint64_t ticks) {
    if (!HsxEnsure() || mHudR == nullptr) {
        return;
    }
    const int path = !retained ? 0 : (mHudR->lastPath == 2 ? 2 : 1);
    mHsx->runTicks[path] += ticks;
    mHsx->runN[path]++;
}

/* [hsx] receipt: per HUD task, how many runs the compact emitter served, the program size and
 * draws it drained, the per-hit time (with hudclock), and under verify the comparison. */
GDX_COLD int Interp::HsxFormat(char* buf, size_t cap, uint32_t tasks) {
    if ((mHudSkyEmit == 0 && mHudClock == 0) || cap == 0 || tasks == 0 || !HsxEnsure()) {
        return 0;
    }
    HsxStats& S = *mHsx;
    const double inv = 1.0 / static_cast<double>(tasks);
    int n = snprintf(buf, cap,
                     "[hsx] tasks=%u mode=%d hits=%.2f ops=%.1f draws=%.2f compiles=%u fail=%u fpmiss=%.2f refused=%u",
                     static_cast<unsigned>(tasks), mHudSkyEmit, S.hits * inv, S.ops * inv, S.draws * inv, S.compiles,
                     S.compileFail, S.fpMiss * inv, S.refused);
    if ((mHudClock != 0 || mHudRetClock != 0) && S.hits != 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | hitms=%.3f us/hit=%.2f",
                      static_cast<double>(S.ticksHit) / 268111.856 * inv,
                      static_cast<double>(S.ticksHit) / 268.111856 / static_cast<double>(S.hits));
    }
    if (static_cast<size_t>(n) < cap) { /* the sky half (gdx_hudsky_tri.inc) */
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | sky words=%.1f tris=%.1f stock=%.1f", mHsxSkyCalls * inv,
                      mHsxSkyTris * inv, mHsxSkyStock * inv);
        if (mHudSkyEmit == 2 && static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " vfy pack ok=%u bad=%u ses_bad=%u", mHsxSkyVfyOk,
                          mHsxSkyVfyBad, mHsxSkyVfyBadSes);
        }
        mHsxSkyCalls = mHsxSkyTris = mHsxSkyStock = 0;
        mHsxSkyVfyOk = mHsxSkyVfyBad = 0;
    }
    if (mHudClock != 0 && static_cast<size_t>(n) < cap) {
        double us[3];
        for (int k = 0; k < 3; k++) {
            us[k] = S.runN[k] != 0 ? static_cast<double>(S.runTicks[k]) / 268.111856 / S.runN[k] : 0.0;
        }
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | run us exec=%.1f/%.2f hit=%.1f/%.2f var=%.1f/%.2f", us[0], S.runN[0] * inv, us[1],
                      S.runN[1] * inv, us[2], S.runN[2] * inv);
    }
    if (mHudClock >= 2 && S.phRuns != 0 && static_cast<size_t>(n) < cap) {
        const double k = 1.0 / 268.111856 / S.phRuns;
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | hit ph us pre=%.1f look=%.1f fp=%.1f loads=%.1f prog=%.1f apply=%.1f post=%.1f (read=%.2f)",
                      S.phSum[0] * k, S.phSum[1] * k, S.phSum[2] * k, S.phSum[3] * k, S.phSum[4] * k, S.phSum[5] * k,
                      S.phSum[6] * k, S.readUs);
    }
    if (mHudSkyEmit == 2 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | vfy runs=%u ok=%u bad=%u (fp=%u ops=%u apply=%u) ses=%u/%u",
                      S.vfyRuns, S.vfyOk, S.vfyBad, S.vfyFp, S.vfyOps, S.vfyApply, S.sesVfyBad, S.sesVfyRuns);
    }
    S.hits = S.compiles = S.compileFail = S.stockFp = S.stockReplay = S.refused = S.fpMiss = 0;
    S.vfyRuns = S.vfyOk = S.vfyBad = S.vfyFp = S.vfyOps = S.vfyApply = 0;
    S.ticksHit = 0;
    S.ops = S.draws = 0;
    memset(S.runTicks, 0, sizeof(S.runTicks));
    memset(S.phSum, 0, sizeof(S.phSum));
    S.phRuns = 0;
    memset(S.runN, 0, sizeof(S.runN));
    return n < static_cast<int>(cap) ? n : static_cast<int>(cap) - 1;
}
