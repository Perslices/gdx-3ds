/* port/3ds/gfx/n64/gdx_texskip.cpp -- [native] machines_texskip: skip a machine's texture-set
 * list when the stream proves nothing can observe it (C5 Stage 1, docs/research/native/
 * c5s1-progress.md; the design is o3ds60-design.md section 3 "Textures resident per slot").
 *
 * WHAT IT SKIPS. Racer_Draw (racer.c) emits, per drawn machine,
 *     G_MTX  G_DL D_800CDD38[slot]  G_SETENVCOLOR  G_DL D_800CDDB0[slot*6+lod-1]
 * The body list is a native retained mesh (gdx3ds_mesh.cpp); the texture-set list in front of
 * it is up to 8 x (SETTIMG, SETTILE 7, LOADBLOCK, SETTILE, SETTILESIZE) plus syncs, walked
 * command by command every frame for every drawn machine: the largest per-racer interpreter
 * cost the design measured on hardware. Every machine's list uses its own TMEM layout (packed
 * from word 16 by texture size), so "the next list covers this one" is a per-pair question.
 *
 * WHY IT IS EXACT. A texture-set list writes tile descriptor fields (SETTILE: format, line,
 * TMEM address, clamp/mirror/mask/shift; SETTILESIZE: the extent), TMEM, the span store and
 * texture_to_load, and arms textures_changed. Its readers are (a) its own body, (b) LOD-6 far
 * specks that inherit the body's last combiner and tile, and (c) whatever comes after. A list
 * is skipped only when, within its command group (texture-state-neutral commands, set lists,
 * machine bodies and specks -- TexStateNeutral / TexSkipBuildPlan), the lists that really RUN
 * after it rewrite every TMEM word and every tile field it writes (TexSkipCovered). The span
 * store retires whole overlapping spans, so once every word a skipped list wrote is rewritten
 * the span map, TMEM and descriptors are exactly the stock ones; dead-base stamps are never
 * read. Which later lists run is the same rule applied backwards from the group's last list,
 * and a list the rule expects to skip that runs after all only adds coverage. A later body
 * must not sample a skipped list's tile as a tile its own list leaves alone (it would see the
 * skipped list's value in the stock stream); the plan probes every later body's tiles for that.
 *
 * For (a) and (b) the "window" serves the record of the list's own effect: the tile fields are
 * swapped in, LoadedTexAt returns the recorded span views, and textures_changed is armed as the
 * list arms it. The imports themselves run unchanged: every tile the list sets that the body
 * samples must be an `immutable` (o2r asset) source, whose cache key is hashed from, and whose
 * decode reads, the SOURCE bytes the recorded span view points at -- never TMEM -- so each
 * import is the stock import, hit or miss. Tiles the list does not set (the race setup's tile
 * 0) stay live, which is stock because the list's loads never write the TMEM they view
 * (TexSkipLiveTilesClear).
 *
 * WHAT CAN GO WRONG IS COUNTED. The tripwire (TexSkipTrip) fires on any texture-state writer
 * inside a window, an import of a non-immutable set tile, an unexpected G_DL, or a task that ends with
 * a window open; `[native] machines_texskip = 2` (verify) never skips and instead compares the
 * record against the live state at every site where a skip would have been taken.
 *
 * Host-compilable: the machine registry is reached only through the injected TexSkipHooks. */
#include "gdx_interp.h"

#include <new>
#include <string.h>

#include "gdx_log.h"
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

constexpr unsigned kScanMaxCmds = 128; /* a texture-set list is ~50 commands */
constexpr unsigned kChainMaxCmds = 256; /* a whole Racer_Draw machine loop (30 x ~4 commands) */
constexpr uint8_t kImFmtCi = 2;
constexpr uint32_t kReceiptTasks = 64;

/* A G_DL's target as the game wrote it: the machine lists (sets, bodies, the speck's
 * placeholder) are host pointers in wide packets tagged kHostTag, and the walker resolves a
 * host-built list to that same pointer. Anything else (a segment address) is not ours and
 * ends a scan -- without paying DlSrc::ResolveList, which is the expensive part of a G_DL. */
const void* HostOperand(const Cmd& c) {
    if (c.w1hi != kHostTag) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(c.w1));
}

/* The descriptor fields G_SETTILE writes, and the four G_SETTILESIZE writes. A set list SETTILEs
 * its load tile (7) but never sizes it, so tile 7's extent is whatever an earlier list left: only
 * the fields a list actually writes are recorded, compared and swapped in. */
bool TileSetEq(const Tile& a, const Tile& b) {
    return a.fmt == b.fmt && a.siz == b.siz && a.cms == b.cms && a.cmt == b.cmt && a.masks == b.masks &&
           a.maskt == b.maskt && a.shifts == b.shifts && a.shiftt == b.shiftt && a.tmem == b.tmem &&
           a.line_size_bytes == b.line_size_bytes && a.palette == b.palette && a.tmem_index == b.tmem_index;
}

bool TileSizeEq(const Tile& a, const Tile& b) {
    return a.uls == b.uls && a.ult == b.ult && a.lrs == b.lrs && a.lrt == b.lrt;
}

void CopyTileSet(Tile& dst, const Tile& src) {
    dst.fmt = src.fmt;
    dst.siz = src.siz;
    dst.cms = src.cms;
    dst.cmt = src.cmt;
    dst.masks = src.masks;
    dst.maskt = src.maskt;
    dst.shifts = src.shifts;
    dst.shiftt = src.shiftt;
    dst.tmem = src.tmem;
    dst.line_size_bytes = src.line_size_bytes;
    dst.palette = src.palette;
    dst.tmem_index = src.tmem_index;
}

void CopyTileSize(Tile& dst, const Tile& src) {
    dst.uls = src.uls;
    dst.ult = src.ult;
    dst.lrs = src.lrs;
    dst.lrt = src.lrt;
}

bool LoadedEq(const LoadedTexture& a, const LoadedTexture& b) {
    return a.addr == b.addr && a.orig_size_bytes == b.orig_size_bytes && a.size_bytes == b.size_bytes &&
           a.full_image_line_size_bytes == b.full_image_line_size_bytes && a.line_size_bytes == b.line_size_bytes &&
           a.tmem_start == b.tmem_start && a.tmem_word_count == b.tmem_word_count && a.immutable == b.immutable &&
           a.native_rgba16 == b.native_rgba16;
}

} // namespace

void Interp::SetTexSkip(int level, const TexSkipHooks& hooks) {
    mTexHooks = hooks;
    mTexSkip = level < 0 ? 0 : (level > 2 ? 2 : level);
    if (mTexHooks.setSlot == nullptr || mTexHooks.isSpeck == nullptr || mTexHooks.isBody == nullptr ||
        mTexHooks.bodyProbe == nullptr || mTexHooks.generation == nullptr) {
        mTexSkip = 0;
    }
    if (mTexSkip != 0 && mTexRecs == nullptr) {
        mTexRecs = new (std::nothrow) TexSetRec[kTexSkipSlots];
        if (mTexRecs == nullptr) {
            mTexSkip = 0;
        } else {
            memset(static_cast<void*>(mTexRecs), 0, sizeof(TexSetRec) * kTexSkipSlots);
        }
    }
    GDXN_LOG("[texskip] init machines_texskip=%d (%s)", mTexSkip,
             mTexSkip == 1 ? "skip" : (mTexSkip == 2 ? "verify: never skips" : "off"));
}

/* The list's shape, and where its loads leave spans. Only the commands a machine texture set is
 * made of are accepted; anything else (a TLUT, a CI format, a strided SETTIMG, a load before any
 * SETTIMG) makes the slot permanently unskippable at this generation. */
bool Interp::TexSkipScanList(const ListRef& list, TexSetRec& rec) {
    rec.setTileMask = 0;
    rec.sizeTileMask = 0;
    rec.nSpans = 0;
    if (list.isF3d) {
        return false;
    }
    uint16_t tmemOf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    bool haveTimg = false;
    for (size_t i = 0; i < list.limit && i < kScanMaxCmds; i++) {
        const Cmd c = ReadCmd(list, i);
        switch (Opcode(c.w0)) {
            case OP_SETTIMG:
                if (C0(c.w0, 0, 12) != 0 || C0(c.w0, 21, 3) == kImFmtCi) {
                    return false; /* width != 1 would make LOADBLOCK read the render tiles */
                }
                haveTimg = true;
                break;
            case OP_SETTILE: {
                const uint32_t tile = C1(c.w1, 24, 3);
                if (C0(c.w0, 21, 3) == kImFmtCi) {
                    return false;
                }
                tmemOf[tile] = static_cast<uint16_t>(C0(c.w0, 0, 9));
                rec.setTileMask |= static_cast<uint8_t>(1u << tile);
                break;
            }
            case OP_LOADBLOCK: {
                if (!haveTimg || rec.nSpans >= 8) {
                    return false;
                }
                rec.spanBase[rec.nSpans] = tmemOf[C1(c.w1, 24, 3)];
                rec.spanWords[rec.nSpans] = 0; /* filled at capture, from the span store */
                rec.nSpans++;
                break;
            }
            case OP_SETTILESIZE:
                rec.sizeTileMask |= static_cast<uint8_t>(1u << C1(c.w1, 24, 3));
                break;
            case OP_RDPLOADSYNC:
            case OP_RDPPIPESYNC:
            case OP_RDPTILESYNC:
                break;
            case OP_ENDDL:
                return rec.nSpans > 0;
            default:
                return false;
        }
    }
    return false;
}

/* The two commands after the set: SETENVCOLOR, then a pushed G_DL (the body). */
bool Interp::TexSkipPeekBody(const Frame& f, const void** body) {
    if (f.list.isF3d || f.idx + 1 >= f.list.limit) {
        return false;
    }
    const Cmd env = ReadCmd(f.list, f.idx);
    const Cmd dl = ReadCmd(f.list, f.idx + 1);
    if (Opcode(env.w0) != OP_SETENVCOLOR || Opcode(dl.w0) != OP_DL || C0(dl.w0, 16, 1) != 0) {
        return false;
    }
    *body = HostOperand(dl);
    return *body != nullptr;
}

/* Commands that neither read nor write the texture state a set list leaves (tile descriptors,
 * TMEM, spans, texture_to_load): matrices, lights/viewport, geometry/other modes, colour
 * registers, the combiner, G_TEXTURE (tile select + scale; it applies identically before the
 * covering set in both streams), syncs. Anything that draws or loads is excluded. */
static bool TexStateNeutral(uint8_t op) {
    switch (op) {
        case OP_MTX:
        case OP_POPMTX:
        case OP_MOVEMEM:
        case OP_MOVEWORD:
        case OP_GEOMETRYMODE:
        case OP_SETOTHERMODE_L:
        case OP_SETOTHERMODE_H:
        case OP_RDPPIPESYNC:
        case OP_RDPTILESYNC:
        case OP_RDPLOADSYNC:
        case OP_SETSCISSOR:
        case OP_SETPRIMDEPTH:
        case OP_SETFOGCOLOR:
        case OP_SETBLENDCOLOR:
        case OP_SETPRIMCOLOR:
        case OP_SETENVCOLOR:
        case OP_SETCOMBINE:
        case OP_TEXTURE:
            return true;
        default:
            return false;
    }
}

/* The texture-set lists that follow this one in the same command group, in stream order, up to
 * the first command that could observe texture state outside a window (anything that is not
 * texture-state neutral, a set list, a registered machine body or a far speck). Returns the
 * count (0 = this set is the last of its group).
 *
 * The group is scanned ONCE per task, at its first set, into a plan (the G_DL index and slot of
 * every set in it); the later sets of the group read their successors from the plan. Scanning
 * per set re-resolved every later G_DL for every set -- quadratic in the group, and in the
 * emulator it cost more than the skips saved. */
int Interp::TexSkipChain(const Frame& f, int* chain, uint8_t* chainTiles, int cap) {
    const size_t self = f.idx - 1; /* this set's own G_DL */
    int pos = -1;
    if (mTexPlan.task == mTexSkipTaskSeq && mTexPlan.base == f.list.base) {
        for (int p = 0; p < mTexPlan.n; p++) {
            if (mTexPlan.idx[p] == self) {
                pos = p;
                break;
            }
        }
    }
    if (pos < 0) {
        TexSkipBuildPlan(f);
        pos = 0;
    }
    int n = 0;
    for (int p = pos + 1; p < mTexPlan.n && n < cap; p++) {
        chain[n] = mTexPlan.slot[p];
        chainTiles[n] = mTexPlan.bodyTiles[p];
        n++;
    }
    return n;
}

/* The tiles the body after the set at `setIdx` can sample, with its inherited tile and texel
 * use unknown (so a run that relies on them counts every tile): 0 when no body follows (a far
 * speck samples a tile its own group's set always rewrites, or the race setup's tile 0). */
uint8_t Interp::TexSkipPlanBodyTiles(const ListRef& list, size_t setIdx) {
    if (setIdx + 2 >= list.limit) {
        return 0;
    }
    const Cmd env = ReadCmd(list, setIdx + 1);
    const Cmd dl = ReadCmd(list, setIdx + 2);
    const void* body = Opcode(dl.w0) == OP_DL ? HostOperand(dl) : nullptr;
    if (Opcode(env.w0) != OP_SETENVCOLOR || body == nullptr || mTexHooks.isBody(body) == 0) {
        return 0;
    }
    int slot = -1;
    uint8_t tiles = 0xFF;
    uint8_t worst = 0xFF;
    for (uint8_t inh = 0; inh < 8; inh++) {
        if (mTexHooks.bodyProbe(mTexHooks.user, body, inh, 7u, &slot, &tiles) == 0) {
            return 0xFF;
        }
        worst = inh == 0 ? tiles : static_cast<uint8_t>(worst | tiles);
    }
    return worst;
}

/* The plan: this set and every set after it in the group (see TexSkipChain). */
void Interp::TexSkipBuildPlan(const Frame& f) {
    const uint64_t msT0 = mMsOn ? static_cast<uint64_t>(gdx_ninterp_now()) : 0; /* [debug] machsplit */
    TexSkipBuildPlanBody(f);
    if (mMsOn) {
        mMsWin.planTicks += static_cast<uint64_t>(gdx_ninterp_now()) - msT0;
        mMsWin.plans++;
    }
}

void Interp::TexSkipBuildPlanBody(const Frame& f) {
    mTexPlan.task = mTexSkipTaskSeq;
    mTexPlan.base = f.list.base;
    mTexPlan.n = 0;
    mTexPlan.idx[0] = static_cast<uint32_t>(f.idx - 1);
    mTexPlan.slot[0] = -1;
    mTexPlan.bodyTiles[0] = 0xFF;
    mTexPlan.n = 1;
    mTexSkipStats.plans++;
    for (size_t i = f.idx; i < f.list.limit && i < f.idx + kChainMaxCmds; i++) {
        const Cmd c = ReadCmd(f.list, i);
        const uint8_t op = Opcode(c.w0);
        if (TexStateNeutral(op)) {
            continue;
        }
        const void* target = (op == OP_DL && C0(c.w0, 16, 1) == 0) ? HostOperand(c) : nullptr;
        if (target == nullptr) {
            break;
        }
        const int slot = mTexHooks.setSlot(target);
        if (slot >= 0) {
            if (mTexPlan.n >= kChainMaxSets) {
                break;
            }
            mTexPlan.idx[mTexPlan.n] = static_cast<uint32_t>(i);
            mTexPlan.slot[mTexPlan.n] = slot;
            mTexPlan.bodyTiles[mTexPlan.n] = TexSkipPlanBodyTiles(f.list, i);
            mTexPlan.n++;
            continue;
        }
        if (mTexHooks.isSpeck(target) == 0 && mTexHooks.isBody(target) == 0) {
            break;
        }
    }
}


/* The skip rule. The state a set leaves is recoverable after its group only if every TMEM word
 * it writes is rewritten, and every tile field it writes is written again, by lists that really
 * RUN later in the group. Which later lists run is itself this rule applied to them, so it is
 * evaluated backwards from the group's last list (which always runs: it has no successor). A
 * later list assumed skipped may still run (its own body may disqualify it), which only adds
 * coverage, so the answer for this list can only be conservative. And no later body may sample,
 * as a tile its OWN set does not write, a tile this list writes -- it would read this list's
 * value in the stock stream. */
bool Interp::TexSkipCovered(const TexSetRec& rec, const int* chain, const uint8_t* chainTiles, int n, uint32_t gen) {
    uint64_t cov[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t tilesCov = 0, sizesCov = 0;
    for (int j = n - 1; j >= 0; j--) {
        const TexSetRec& r = mTexRecs[chain[j]];
        const bool known = r.shapeOk && r.captured && r.gen == gen;
        if (!known) {
            return false; /* a set in the group we cannot reason about */
        }
        const uint8_t liveSet = static_cast<uint8_t>(chainTiles[j] & ~r.setTileMask);
        const uint8_t liveSize = static_cast<uint8_t>(chainTiles[j] & ~r.sizeTileMask);
        if ((liveSet & rec.setTileMask) != 0 || (liveSize & rec.sizeTileMask) != 0) {
            return false;
        }
        bool skipJ = false;
        if (known && j != n - 1 && (r.setTileMask & ~tilesCov) == 0 && (r.sizeTileMask & ~sizesCov) == 0) {
            skipJ = true;
            for (int w = 0; w < 8 && skipJ; w++) {
                skipJ = (r.wordMask[w] & ~cov[w]) == 0;
            }
        }
        if (!skipJ && known) {
            for (int w = 0; w < 8; w++) {
                cov[w] |= r.wordMask[w];
            }
            tilesCov |= r.setTileMask;
            sizesCov |= r.sizeTileMask;
        }
    }
    if ((rec.setTileMask & ~tilesCov) != 0 || (rec.sizeTileMask & ~sizesCov) != 0) {
        return false;
    }
    for (int w = 0; w < 8; w++) {
        if ((rec.wordMask[w] & ~cov[w]) != 0) {
            return false;
        }
    }
    return true;
}

bool Interp::TexSkipCanSkip(const TexSetRec& rec, const int* chain, const uint8_t* chainTiles, int n, uint8_t bodyTiles,
                            uint32_t gen) {
    if (!rec.shapeOk) {
        mTexSkipStats.unsup++;
        return false;
    }
    if (rec.gen != gen || !rec.captured) {
        mTexSkipStats.noRec++;
        return false;
    }
    /* Tiles the list sets are served from the record and must be immutable sources (keyed and
     * decoded from the source bytes, never TMEM). Tiles it does not set (the race setup's tile
     * 0) are read live, which is the stock state only if the list's loads never write the TMEM
     * any of them views -- checked for every such tile, used by this body or not, because the
     * bodies later in the group read them too. */
    const uint8_t own = static_cast<uint8_t>(bodyTiles & rec.setTileMask);
    bool ownOk = true;
    for (int t = 0; t < 8; t++) {
        if (((own >> t) & 1u) != 0 && (!rec.lt[t].immutable || rec.lt[t].addr == nullptr)) {
            ownOk = false;
        }
    }
    if (!ownOk || !TexSkipLiveTilesClear(rec, static_cast<uint8_t>(~rec.setTileMask))) {
        mTexSkipStats.noTiles++;
        return false;
    }
    if (n == 0) {
        mTexSkipStats.noNext++;
        return false;
    }
    if (!TexSkipCovered(rec, chain, chainTiles, n, gen)) {
        mTexSkipStats.noCover++;
        return false;
    }
    return true;
}

/* The texel use of the combiner a body inherits (the live G_SETCOMBINE, packed by
 * DpSetCombineMode: rgb1 a|b<<4|c<<8|d<<13, alpha1 << 16, rgb2 << 28, alpha2 << 44):
 * bit0 = cycle-1 TEXEL0, bit1 = cycle-1 TEXEL1, bit2 = any texel input in cycle 2. */
uint8_t Interp::TexSkipInheritedUse() const {
    const uint64_t cm = mRdp.combine_mode;
    bool t0 = false, t1 = false, c2 = false;
    const auto note = [](uint32_t v, bool rgbC, bool& a, bool& b) {
        if (v == 1u || (rgbC && v == 8u)) {
            a = true;
        }
        if (v == 2u || (rgbC && v == 9u)) {
            b = true;
        }
    };
    const auto cycle = [&note](uint64_t rgb, uint64_t alpha, bool& a, bool& b) {
        note(static_cast<uint32_t>(rgb & 0xFu), false, a, b);
        note(static_cast<uint32_t>((rgb >> 4) & 0xFu), false, a, b);
        note(static_cast<uint32_t>((rgb >> 8) & 0x1Fu), true, a, b);
        note(static_cast<uint32_t>((rgb >> 13) & 0x7u), false, a, b);
        note(static_cast<uint32_t>(alpha & 0x7u), false, a, b);
        note(static_cast<uint32_t>((alpha >> 3) & 0x7u), false, a, b);
        note(static_cast<uint32_t>((alpha >> 6) & 0x7u), false, a, b);
        note(static_cast<uint32_t>((alpha >> 9) & 0x7u), false, a, b);
    };
    cycle(cm & 0xFFFFu, (cm >> 16) & 0xFFFu, t0, t1);
    bool c2a = false, c2b = false;
    cycle((cm >> 28) & 0xFFFFu, (cm >> 44) & 0xFFFu, c2a, c2b);
    c2 = c2a || c2b;
    return static_cast<uint8_t>((t0 ? 1u : 0u) | (t1 ? 2u : 0u) | (c2 ? 4u : 0u));
}

/* The TMEM a live tile samples (its span view's extent) is disjoint from every span the list
 * writes. */
bool Interp::TexSkipLiveTilesClear(const TexSetRec& rec, uint8_t live) {
    for (int t = 0; t < 8; t++) {
        if (((live >> t) & 1u) == 0) {
            continue;
        }
        const uint16_t start = mRdp.texture_tile[t].tmem_index;
        const LoadedTexture lt = LoadedTexAt(start);
        const uint32_t words = lt.size_bytes > 8u ? (lt.size_bytes + 7u) / 8u : 1u;
        for (unsigned i = 0; i < rec.nSpans; i++) {
            const uint32_t b = rec.spanBase[i];
            const uint32_t e = b + (rec.spanWords[i] != 0 ? rec.spanWords[i] : 1u);
            if (start < e && b < start + words) {
                return false;
            }
        }
    }
    return true;
}

/* What the list leaves behind, read right after it ran (only SETENVCOLOR has run since). */
void Interp::TexSkipCapture(TexSetRec& rec) {
    for (int t = 0; t < 8; t++) {
        if (((rec.setTileMask >> t) & 1u) == 0) {
            continue;
        }
        const Tile& live = mRdp.texture_tile[t];
        rec.tile[t] = live;
        rec.lt[t] = LoadedTexAt(live.tmem_index);
    }
    memset(rec.wordMask, 0, sizeof(rec.wordMask));
    for (unsigned i = 0; i < rec.nSpans; i++) {
        const uint16_t base = rec.spanBase[i];
        rec.spanWords[i] = (base < 512u && mRdp.tmem_span_base[base] == static_cast<uint16_t>(base + 1u))
                               ? mRdp.tmem_span_words[base]
                               : 0;
        /* A load whose span did not survive its own list is not recoverable: mark the whole
         * base so no later list can be taken to cover it. */
        const uint32_t words = rec.spanWords[i] != 0 ? rec.spanWords[i] : 512u;
        for (uint32_t w = base; w < base + words && w < 512u; w++) {
            rec.wordMask[w >> 6] |= 1ull << (w & 63u);
        }
    }
    rec.captured = 1;
    mTexSkipStats.recs++;
    gdx3ds_hitch_add(GDX_HITCH_EV_TEXSKIP_REC, 1u); /* HITCH */
}

/* verify: the record against the live state at a site where a skip would have been taken. */
void Interp::TexSkipVerifyAt(const TexSetRec& rec) {
    mTexSkipStats.vfyChecks++;
    for (int t = 0; t < 8; t++) {
        if (((rec.setTileMask >> t) & 1u) == 0) {
            continue;
        }
        const Tile& live = mRdp.texture_tile[t];
        if (!TileSetEq(rec.tile[t], live) ||
            (((rec.sizeTileMask >> t) & 1u) != 0 && !TileSizeEq(rec.tile[t], live))) {
            mTexSkipStats.vfyTileMis++;
        }
        if (!LoadedEq(rec.lt[t], LoadedTexAt(live.tmem_index))) {
            mTexSkipStats.vfyLtMis++;
        }
    }
    for (unsigned i = 0; i < rec.nSpans; i++) {
        const uint16_t base = rec.spanBase[i];
        const uint16_t words = (base < 512u && mRdp.tmem_span_base[base] == static_cast<uint16_t>(base + 1u))
                                   ? mRdp.tmem_span_words[base]
                                   : 0;
        if (words != rec.spanWords[i]) {
            mTexSkipStats.vfyLtMis++;
        }
    }
}

/* Open the window: the list's tile descriptors in, textures_changed armed as its LOADBLOCKs arm
 * it, the record served to LoadedTexAt / ImportTexture until the covering list closes it. */
void Interp::TexSkipOpen(const TexSetRec& rec, const void* body) {
    mTexOvMask = rec.setTileMask;
    for (int t = 0; t < 8; t++) {
        const bool set = ((rec.setTileMask >> t) & 1u) != 0;
        const bool size = ((rec.sizeTileMask >> t) & 1u) != 0;
        mTexOvSaved[t] = mRdp.texture_tile[t];
        if (set) {
            CopyTileSet(mRdp.texture_tile[t], rec.tile[t]);
        }
        if (size) {
            CopyTileSize(mRdp.texture_tile[t], rec.tile[t]);
        }
    }
    mRdp.textures_changed[0] = true;
    mRdp.textures_changed[1] = true;
    mTexOv = &rec;
    mTexOvBody = body;
}

/* Close it: the live descriptors back. The list that closes the window overwrites every tile
 * the record swapped in (setTileMask coverage), so what it leaves is the stock state. */
void Interp::TexSkipClose() {
    for (int t = 0; t < 8; t++) {
        mRdp.texture_tile[t] = mTexOvSaved[t]; /* untouched tiles restore to themselves */
    }
    mTexOv = nullptr;
    mTexOvMask = 0;
    mTexOvBody = nullptr;
}

/* The far speck arrives resolved to its loaded asset image; the G_DL that named it (the
 * command just read from the top frame) still carries the placeholder operand. */
bool Interp::TexSkipDlIsSpeck(const void* listPtr) {
    if (mTexHooks.isSpeck(listPtr) != 0) {
        return true;
    }
    if (mSp <= 0) {
        return false;
    }
    const Frame& f = mStack[mSp - 1];
    if (f.idx == 0) {
        return false;
    }
    const Cmd dl = ReadCmd(f.list, f.idx - 1);
    const void* operand = Opcode(dl.w0) == OP_DL ? HostOperand(dl) : nullptr;
    return operand != nullptr && mTexHooks.isSpeck(operand) != 0;
}

int Interp::TexSkipOnDl(const void* listPtr) {
    if (mTexSkip == 0) {
        return 0;
    }
    if (mMsOn == 0) {
        return TexSkipOnDlBody(listPtr);
    }
    const uint64_t t0 = static_cast<uint64_t>(gdx_ninterp_now()); /* [debug] machsplit */
    const int r = TexSkipOnDlBody(listPtr);
    mMsWin.texskipTicks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
    mMsWin.texskipCalls++;
    return r;
}

int Interp::TexSkipOnDlBody(const void* listPtr) {
    if (mHud != nullptr) {
        /* A G_DL inside a hud/fx/shadow_batch replayed run (gdx_hud_replay.cpp): its commands
         * come from a record, not the stack top, so the peek/plan below would read the wrong
         * stream. Never skip there; a set list met there is stock state, and one met inside an
         * open window is the tripwire's business, as anywhere else. */
        if (mTexOv != nullptr && mTexHooks.setSlot(listPtr) >= 0) {
            TexSkipClose();
        }
        return 0;
    }
    const int slot = mTexHooks.setSlot(listPtr);
    if (mTexOv != nullptr) {
        /* An open window admits its own body and far specks; the next set closes it. */
        if (slot >= 0) {
            TexSkipClose();
        } else if (listPtr == mTexOvBody || TexSkipDlIsSpeck(listPtr)) {
            return 0;
        } else {
            TexSkipTrip(3, 0);
            TexSkipClose();
            return 0;
        }
    }
    if (slot < 0) {
        /* The body a real set armed: capture (or, in verify mode, compare) what the set left,
         * and tap the body's imports. Anything else in between disarms. */
        if (mTexRecArm != nullptr && listPtr == mTexRecArmBody) {
            if (mTexVfyArm) {
                TexSkipVerifyAt(*mTexRecArm);
            } else {
                TexSkipCapture(*mTexRecArm);
            }
        }
        mTexRecArm = nullptr;
        mTexVfyArm = false;
        return 0;
    }
    mTexSkipStats.lists++;
    mTexRecArm = nullptr;
    mTexVfyArm = false;
    if (slot >= kTexSkipSlots || mSp <= 0) {
        mTexSkipStats.real++;
        return 0;
    }
    const Frame& f = mStack[mSp - 1]; /* the frame holding this G_DL, idx already past it */
    const void* body = nullptr;
    int bodySlot = -1;
    uint8_t bodyTiles = 0xFF;
    if (!TexSkipPeekBody(f, &body) ||
        mTexHooks.bodyProbe(mTexHooks.user, body, mRdp.first_tile_index, TexSkipInheritedUse(), &bodySlot,
                            &bodyTiles) == 0 ||
        bodySlot != slot) {
        mTexSkipStats.noBody++;
        mTexSkipStats.real++;
        return 0;
    }
    TexSetRec& rec = mTexRecs[slot];
    const uint32_t gen = mTexHooks.generation();
    if (rec.gen != gen) {
        /* A new generation of this slot's lists: re-scan the set, forget every recorded import. */
        memset(static_cast<void*>(&rec), 0, sizeof(rec));
        rec.gen = gen;
        ListRef self;
        const Cmd dl = ReadCmd(f.list, f.idx - 1);
        rec.shapeOk = (mSrc.ResolveList(dl, self) && TexSkipScanList(self, rec)) ? 1 : 0;
    }
    int chain[kChainMaxSets];
    uint8_t chainTiles[kChainMaxSets];
    const int n = TexSkipChain(f, chain, chainTiles, kChainMaxSets);
    const bool can = TexSkipCanSkip(rec, chain, chainTiles, n, bodyTiles, gen);
    if (can && mTexSkip == 1) {
        TexSkipOpen(rec, body);
        mTexSkipStats.skips++;
        return 1;
    }
    mTexSkipStats.real++;
    if (rec.shapeOk) {
        mTexRecArm = &rec;
        mTexRecArmBody = body;
        mTexVfyArm = can; /* verify mode: compare here instead of re-recording */
        mTexSkipStats.vfyWould += can ? 1u : 0u;
    }
    return 0;
}

void Interp::TexSkipAfterDl(const void* listPtr) {
    (void)listPtr; /* reserved: nothing is armed across the body draw any more */
}

void Interp::TexSkipTaskEnd() {
    if (mTexSkip == 0) {
        return;
    }
    if (mTexOv != nullptr) {
        TexSkipTrip(4, 0);
        TexSkipClose();
    }
    mTexRecArm = nullptr;
    mTexVfyArm = false;
    mTexSkipTaskSeq++; /* every plan belongs to one task */
    if (++mTexSkipTasks >= kReceiptTasks) {
        mTexSkipTasks = 0;
        TexSkipReceipt();
    }
}

/* [texskip] every 64 tasks: lists = texture-set lists met, skips = skipped, real = walked;
 * why a set was walked: noBody (not SETENV + native body next), unsup (list shape), noRec (no
 * capture at this generation yet), noTiles (a set tile the body samples is not an immutable
 * source, or a live tile's TMEM overlaps the set's loads), noNext (no covering set next in the
 * loop), noCover (the next set does not cover this one's tiles/spans); recs = captures;
 * ovImports = imports of set tiles inside windows; vfy* = verify-mode comparisons (checks /
 * tile and span-view mismatches / would-have-skipped); trips = the tripwire, split writer /
 * non-immutable import / unexpected G_DL / task end. vfy*Mis and trips must stay 0. */
void Interp::TexSkipReceipt() {
    const TexSkipCounters& s = mTexSkipStats;
    GDXN_LOG("[texskip] level=%d lists=%lu skips=%lu real=%lu | noBody=%lu unsup=%lu noRec=%lu noTiles=%lu "
             "noNext=%lu noCover=%lu | recs=%lu plans=%lu ovImports=%lu | vfy checks=%lu tileMis=%lu ltMis=%lu "
             "would=%lu | trips=%lu (writer=%lu import=%lu dl=%lu taskEnd=%lu)",
             mTexSkip, (unsigned long)s.lists, (unsigned long)s.skips, (unsigned long)s.real,
             (unsigned long)s.noBody, (unsigned long)s.unsup, (unsigned long)s.noRec, (unsigned long)s.noTiles,
             (unsigned long)s.noNext, (unsigned long)s.noCover, (unsigned long)s.recs, (unsigned long)s.plans,
             (unsigned long)s.ovImports, (unsigned long)s.vfyChecks, (unsigned long)s.vfyTileMis,
             (unsigned long)s.vfyLtMis, (unsigned long)s.vfyWould,
             (unsigned long)s.trips, (unsigned long)s.tripWriter, (unsigned long)s.tripImport,
             (unsigned long)s.tripDl, (unsigned long)s.tripTaskEnd);
    TexSkipResetStats();
}

} // namespace gdxn
