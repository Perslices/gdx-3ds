/* port/3ds/gfx/n64/gdx_hud_direct.cpp -- [native] hud_direct: the HUD's per-frame texture
 * loads and rectangle state work, removed (C5 Stage 2b, docs/research/native/
 * c5s2b-progress.md; o3ds60-design.md section 5; c5s2-progress.md section 4).
 *
 * WHERE IT RUNS. Only inside a replayed HUD run (gdx_hud_replay.cpp HudReplayRun, kind HUD):
 * the game's HUD functions still run in full on the game thread and still write their display
 * list; c5s2's capture moves the recordable commands into the per-GfxPool record; the walker
 * reaches the 0x5E marker; HudReplayRun executes the run. With [native] hud_direct=1 three
 * things change inside that run, and nothing outside it:
 *
 * 1. TMEM DEBT. A G_LOADBLOCK does everything the stock load does -- the SETTIMG resolution
 *    (c5s2's memo), the descriptor, the same-content test, the span store, the source stamp,
 *    textures_changed -- EXCEPT copy the source bytes into mRdp.tmem. The copy is recorded as a
 *    debt entry instead: "TMEM bytes [off, end) are the mirror of `src` (byte-swapped when
 *    native), as of generation stamp S", with the source class the stamp was taken under
 *    (gdx_gfx_tmem_source_class / _stamp, the tmemblock seam). Entries are disjoint: a new
 *    debt, or a REAL copy into TMEM (MirrorToTmem's hook), supersedes the part it overwrites.
 *    Only two things in the owned interpreter ever read TMEM bytes -- ImportTexture's content
 *    hash for the TMEM-hashed classes, and DecodeRgba16's TMEM path on a cache miss -- and both
 *    settle the debt first (HudDebtHash, HudDebtPayAll). The task end settles the rest
 *    (HudDirectTaskEnd), so TMEM is byte-exact again before anything else can run. A payment
 *    copies from the source only after gdx_gfx_tmem_source_unchanged proves the bytes did not
 *    move since the stamp; a payment it cannot prove is counted (`stale=`), never hidden. A
 *    source whose class has no staleness test (UNKNOWN) never becomes debt: it is copied.
 *
 * 2. THE TEXTURE, RESOLVED ONCE. The texture cache key of a TMEM-hashed import is FNV over the
 *    TMEM span, i.e. over the mirror of the debt's source. When one debt entry covers the whole
 *    span, the hash is a pure function of (source pointer, span, swap) and the source bytes;
 *    it is memoised per source (HashE) and re-used while gdx_gfx_tmem_source_unchanged says
 *    the bytes have not moved since it was computed -- the DMA generation for the HUD's
 *    segment-4 carve, the native-RGBA16 generation, or "stable" for a registered ROM range.
 *    A memo miss (first sight, or the generation moved: `inval=`) pays the span and hashes it.
 *    The key then finds the same texture-cache entry the stock path finds -- the [trectbatch]
 *    atlas VIEW the HUD's texture was placed in when it was first decoded -- so a HUD frame
 *    does one key build and one hash-table lookup per texture, and no copy and no hash.
 *
 * 3. RECTANGLES EMITTED DIRECTLY. A TEXRECT does DpTextureRectangle's and DrawRectangle's
 *    arithmetic (same expressions, same order), resolves the draw state exactly as the first
 *    of its two SpTri calls would (c5s2's HudRectState memo, else ResolveDrawState -- the
 *    [trectbatch] atlas protocol, the sampler, the shader), then writes both triangles into the
 *    open packed batch with SpTri's emission expressions (HudEmitRectTri). The second SpTri's
 *    state block is not re-run: nothing it reads changed since the first (HudRectState's
 *    argument; the tri memo is still valid), so its answer is the first one. A rectangle whose
 *    first triangle fills the batch takes the stock SpTri for its second; a COPY-cycle
 *    rectangle takes the stock handler.
 *
 * Draw order, blending, batching and every backend call are the stock path's: the rectangles
 * are emitted where the stock rectangles were, into the same packed batches, with the same
 * flushes. The frame is therefore the stock frame by construction, and the gate is exact
 * parity (fh + SHOT BMPs), not a tolerance.
 *
 * VERIFY ([native] hud_direct=2). The run executes the STOCK path (every load copies, every
 * rectangle goes through SpTri), and at each point the direct path would have acted it
 * cross-checks instead: the debt ledger is kept as a SHADOW and every entry is compared with
 * the real TMEM bytes wherever it would have been paid; the hash memo's answer is compared with
 * the stock hash; every stock rectangle's two emitted triangles (and the aux record when it
 * began the batch) are compared float-for-float with HudEmitRectTri's prediction from the
 * same state. `vfy ... bad=0` on [hudd] is the in-process proof. */
#include "gdx_interp.h"
#include "gdx3ds_replay.h" // [native] replay (C6)

#include <new>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_hud_record.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx3ds_hitch.h" // HITCH: per-frame event counters
#include "gdx3ds_hotcold.h"

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

constexpr uint32_t kDebtMax = 24;    /* live entries; the race HUD needs 1-3 */
constexpr unsigned kHashSets = 128; /* power of two; 2 ways each (the race HUD has ~20 textures) */
constexpr uint32_t kTmemBytes = 4096u;
constexpr size_t kTriFloats = 3u * GDX3DS_VBOPACK_STRIDE;

/* == gdx_tri.cpp FminExact */
inline float FminExact(float x, float y) {
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x < y ? x : y;
}

/* FNV-1a over TMEM, word then tail: ImportTexture's TMEM-hash loop, expression for expression. */
uint32_t TmemFnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    uint32_t b = 0;
    for (; b + 4 <= n; b += 4) {
        uint32_t word;
        memcpy(&word, p + b, 4);
        h = (h ^ word) * 16777619u;
    }
    for (; b < n; b++) {
        h = (h ^ p[b]) * 16777619u;
    }
    return h;
}

uint32_t HashSetOf(const uint8_t* src, uint32_t span) {
    return ((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(src)) * 2654435761u) ^ (span * 40503u)) >> 25 &
           (kHashSets - 1u);
}

/* Byte i of MirrorToTmem's output for a copy of n bytes from src: a native-RGBA16 copy swaps
 * every byte pair counted from the copy's START, and an odd tail byte is copied as is. */
inline uint8_t MirrorByte(const uint8_t* src, uint32_t n, bool native, uint32_t i) {
    if (!native || ((n & 1u) != 0 && i == n - 1u)) {
        return src[i];
    }
    return src[i ^ 1u];
}

/* The PackInput of SpTri's gdxInput lambda (gdx_tri.cpp), for vertex `vv` of a triangle whose
 * first vertex is `v1`. */
struct PackIn {
    Rgba c;
    bool isShade;
};

} // namespace

/* ---- the debt ledger (TU-local; the Interp holds one inside HudDirect) ------------------- */

namespace {

/* TMEM bytes [off, end) == MirrorToTmem(src0, n0, native) written at tmem offset off0,
 * restricted to that range, as of `stamp` (sampled before the bytes would have been read)
 * under source class `cls`. */
struct Debt {
    uint32_t off, end;
    uint32_t off0, n0;
    const uint8_t* src0;
    uint64_t stamp[2];
    uint8_t native, cls;
};

/* [hudd] receipt window (reset by HudDirectFormat). */
struct HudWin {
    uint32_t runs, rects, rectStock;
    uint32_t debtNotes, debtBytes, debtUnknown, trimBytes, pays, payBytes, stale, endBytes, overflow, payAll, ends;
    uint32_t hashHit, hashMiss, hashInval, hashPartial;
    uint32_t loadHit, loadMiss, loadStock, vfyLoadOk, vfyLoadBad;
    uint32_t vfyDebtOk, vfyDebtBad, vfyHashOk, vfyHashBad, vfyRectOk, vfyRectBad, vfyRectSkip;
    uint64_t ticks[4 + FLUSH_COUNT]; /* [native] hudclock: runs, rectangles, loads, the rectangles'
                                         state blocks (ResolveDrawState: imports, flushes); then the
                                         batches drained inside direct runs by FlushReason
                                         (needs [native] ncensus=1 to be counted) */
};

struct Ledger {
    Debt e[kDebtMax];
    uint32_t n;
    bool shadow; /* verify: TMEM already holds the real bytes; an entry is a prediction */
    uint8_t* tmem;
    HudWin* w;
    uint32_t* sesStale;
    uint32_t* sesBad;
};

/* hotcold: the shadow (verify) compare of DebtSettleBytes, out of line. */
GDX_COLD void DebtShadowCompare(Ledger& L, const Debt& d, const uint8_t* dst, const uint8_t* src, uint32_t i,
                                uint32_t end) {
        bool same = true;
        for (; i < end && same; i++) {
            same = dst[i] == MirrorByte(src, d.n0, d.native != 0, i);
        }
        if (same) {
            L.w->vfyDebtOk++;
        } else {
            L.w->vfyDebtBad++;
            (*L.sesBad)++;
        }
}

/* Write (or, in shadow mode, compare) the bytes [lo, hi) of entry `d`. */
void DebtSettleBytes(Ledger& L, const Debt& d, uint32_t lo, uint32_t hi) {
    if (lo >= hi) {
        return;
    }
    uint8_t* dst = L.tmem + d.off0;
    const uint8_t* src = d.src0;
    uint32_t i = lo - d.off0;
    const uint32_t end = hi - d.off0;
    if (GDX_UNLIKELY(L.shadow)) {
        DebtShadowCompare(L, d, dst, src, i, end);
        return;
    }
    L.w->pays++;
    L.w->payBytes += hi - lo;
    if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(d.src0), d.n0, d.cls, d.stamp) == 0) {
        L.w->stale++; /* the tripwire: the source moved between the load and this payment */
        (*L.sesStale)++;
    }
    if (d.native == 0) {
        memcpy(dst + i, src + i, end - i);
        return;
    }
    /* MirrorToTmem's pair swap, from the ORIGINAL copy's start: bytes up to the next word
     * boundary one at a time, then whole words (REV16 shape), then the tail (the odd last
     * byte of an odd-length copy is not swapped -- MirrorByte). */
    const uint32_t pairsEnd = d.n0 & ~1u;
    for (; i < end && (i & 3u) != 0; i++) {
        dst[i] = MirrorByte(src, d.n0, true, i);
    }
    const uint32_t wordEnd = (end < pairsEnd ? end : pairsEnd) & ~3u;
    for (; i + 4u <= wordEnd; i += 4u) {
        uint32_t w;
        memcpy(&w, src + i, sizeof(w));
        w = ((w & 0xFF00FF00u) >> 8) | ((w & 0x00FF00FFu) << 8);
        memcpy(dst + i, &w, sizeof(w));
    }
    for (; i < end; i++) {
        dst[i] = MirrorByte(src, d.n0, true, i);
    }
}

void DebtRemoveAt(Ledger& L, uint32_t i) {
    L.e[i] = L.e[--L.n];
}

/* Take [a, b) out of the ledger. `settle` = the bytes are needed now (pay / compare them);
 * otherwise they are SUPERSEDED (something is about to write them). Returns the bytes removed.
 * A cut strictly inside an entry splits it; with no free slot the right part is settled now,
 * which is always correct (the bytes it owes are the bytes TMEM must end up holding). */
uint32_t DebtCut(Ledger& L, uint32_t a, uint32_t b, bool settle) {
    uint32_t removed = 0;
    uint32_t i = 0;
    while (i < L.n) {
        Debt& d = L.e[i];
        if (d.end <= a || d.off >= b) {
            i++;
            continue;
        }
        const uint32_t lo = d.off > a ? d.off : a;
        const uint32_t hi = d.end < b ? d.end : b;
        removed += hi - lo;
        if (settle) {
            DebtSettleBytes(L, d, lo, hi);
        }
        if (lo == d.off && hi == d.end) {
            DebtRemoveAt(L, i);
            continue; /* the slot now holds another entry */
        }
        if (lo == d.off) {
            d.off = hi;
        } else if (hi == d.end) {
            d.end = lo;
        } else if (L.n < kDebtMax) {
            Debt right = d;
            right.off = hi;
            d.end = lo;
            L.e[L.n++] = right;
        } else {
            L.w->overflow++;
            DebtSettleBytes(L, d, hi, d.end);
            d.end = lo;
        }
        i++;
    }
    return removed;
}

void DebtAdd(Ledger& L, uint32_t off, uint32_t n, const uint8_t* src, bool native, unsigned cls,
             const uint64_t stamp[2]) {
    DebtCut(L, off, off + n, false); /* this copy supersedes whatever it overwrites */
    if (L.n >= kDebtMax) {
        L.w->overflow++;
        const Debt oldest = L.e[0];
        DebtSettleBytes(L, oldest, oldest.off, oldest.end);
        DebtRemoveAt(L, 0);
    }
    Debt& d = L.e[L.n++];
    d.off = off;
    d.end = off + n;
    d.off0 = off;
    d.n0 = n;
    d.src0 = src;
    d.stamp[0] = stamp[0];
    d.stamp[1] = stamp[1];
    d.native = native ? 1 : 0;
    d.cls = static_cast<uint8_t>(cls);
}

/* The one entry that covers [a, b) whole, or -1. */
int DebtCover(const Ledger& L, uint32_t a, uint32_t b, bool* any) {
    *any = false;
    for (uint32_t i = 0; i < L.n; i++) {
        const Debt& d = L.e[i];
        if (d.end <= a || d.off >= b) {
            continue;
        }
        *any = true;
        if (d.off <= a && d.end >= b) {
            return static_cast<int>(i);
        }
        return -1;
    }
    return -1;
}

struct HashE {
    const uint8_t* src;
    uint32_t span, hash;
    uint64_t stamp[2];
    uint8_t native, cls, filled;
};

/* A LOADBLOCK's resolved form ("the texture, resolved once"): everything DpLoadBlock derives
 * before it copies -- the SETTIMG resolution and class, the LoadedTexture descriptor, the bytes
 * the copy would move -- keyed on the SETTIMG operand, the LOADBLOCK words, the load tile's
 * TMEM address and the bridge's classification epoch (the [native] timgmemo freshness term).
 * Only for a SETTIMG width of 1 (G_LOADBLOCK's own form), where DpLoadBlock's line-size search
 * over the other tiles never runs, so nothing else can enter the descriptor. */
constexpr unsigned kLoadSets = 64; /* power of two, 2 ways each; the race HUD loads ~20 textures */
struct LoadE {
    uint32_t tw0, tw1, tw1hi; /* SETTIMG */
    uint32_t lw0, lw1;        /* LOADBLOCK */
    uint32_t epoch;
    uint16_t tmemIndex;
    uint8_t filled, imm, native, cls;
    const uint8_t* tlAddr;
    LoadedTexture loaded;
    uint32_t copyBytes;
};

uint32_t LoadSetOf(uint32_t tw1, uint32_t lw1) {
    return ((tw1 * 2654435761u) ^ (lw1 * 40503u)) >> 26 & (kLoadSets - 1u);
}

} // namespace

struct Interp::HudDirect {
    Ledger led;
    LoadE load[kLoadSets][2];
    uint8_t loadMru[kLoadSets];
    /* the last debt note (HudDebtMirror): what the load being memoised really copied */
    const uint8_t* noteSrc;
    uint32_t noteN;
    uint8_t noteCls;
    bool noted;
    HashE hash[kHashSets][2];
    uint8_t hashMru[kHashSets]; /* the way used last: a miss replaces the other one */
    HudWin w;
    uint32_t sesStale, sesVfyBad; /* never reset: the session tripwires */
    uint32_t sesRuns;
    /* [native] hud_retain (gdx_hud_retain.cpp): the last direct load's resolved form (valid
     * only when it stored a span and left its copy as debt), and whether the last TMEM-hashed
     * key came from one debt entry covering the whole span (a function of that source span). */
    HudLoadForm last;
    bool lastHashCovered;
    const uint8_t* lastHashSrc; /* the covering debt's source for the hashed span */
    uint8_t lastHashNative, lastHashCls;
};

bool Interp::HudDirectEnsure() {
    if (mHudD != nullptr) {
        return true;
    }
    mHudD = new (std::nothrow) HudDirect;
    if (mHudD == nullptr) {
        mHudDirect = 0;
        return false;
    }
    memset(static_cast<void*>(mHudD), 0, sizeof(HudDirect));
    mHudD->led.tmem = mRdp.tmem;
    mHudD->led.w = &mHudD->w;
    mHudD->led.sesStale = &mHudD->sesStale;
    mHudD->led.sesBad = &mHudD->sesVfyBad;
    return true;
}

bool Interp::HudDirectBegin() {
    mHudD->led.shadow = mHudDirect == 2;
    mHudD->w.runs++;
    return mHudD->led.shadow;
}

/* MirrorToTmem's hook: called for every TMEM copy while a debt is live or a direct load runs.
 * Returns true when the copy became debt (the caller then copies nothing). */
bool Interp::HudDebtMirror(uint8_t* dst, const uint8_t* src, uint32_t n, bool nativeRgba16) {
    if (mHudD == nullptr || dst < mRdp.tmem || dst + n > mRdp.tmem + kTmemBytes) {
        return false;
    }
    Ledger& L = mHudD->led;
    const uint32_t off = static_cast<uint32_t>(dst - mRdp.tmem);
    bool virtualized = false;
    if (mHudVirtualLoad) {
        const unsigned cls = gdx_gfx_tmem_source_class(reinterpret_cast<uintptr_t>(src), n);
        if (cls == GDX_TMEMSRC_UNKNOWN) {
            mHudD->w.debtUnknown++; /* no staleness test for this source: copy it now */
            mHudD->w.trimBytes += DebtCut(L, off, off + n, false);
        } else {
            uint64_t stamp[2];
            gdx_gfx_tmem_source_stamp(stamp); /* before the bytes are (or would be) read */
            DebtAdd(L, off, n, src, nativeRgba16, cls, stamp);
            mHudD->noteSrc = src;
            mHudD->noteN = n;
            mHudD->noteCls = static_cast<uint8_t>(cls);
            mHudD->noted = true;
            mHudD->w.debtNotes++;
            mHudD->w.debtBytes += n;
            virtualized = !L.shadow;
        }
    } else {
        mHudD->w.trimBytes += DebtCut(L, off, off + n, false); /* a real copy supersedes the debt */
    }
    mHudDebtN = L.n;
    return virtualized;
}

/* ImportTexture's TMEM-hash hook (only while a debt is live). false = no debt over the span,
 * the stock hash runs; true = *out is the stock hash of TMEM [off, off+span), from the memo or
 * computed over the now-settled bytes. */
bool Interp::HudDebtHash(uint32_t off, uint32_t span, uint32_t* out) {
    if (mHudD == nullptr || span == 0 || off + span > kTmemBytes) {
        return false;
    }
    HudDirect& H = *mHudD;
    Ledger& L = H.led;
    H.lastHashCovered = false;
    bool any = false;
    const int ci = DebtCover(L, off, off + span, &any);
    if (!any) {
        return false;
    }
    /* A span inside ONE entry, pair-aligned with its copy and clear of an odd tail byte, holds
     * the mirror of exactly [src, src + span): its hash is a function of that source span. */
    HashE* he = nullptr;
    bool hit = false;
    const Debt* d = ci >= 0 ? &L.e[ci] : nullptr;
    const uint32_t rel = d != nullptr ? off - d->off0 : 0;
    if (d != nullptr && (d->native == 0 || ((rel & 1u) == 0 && off + span <= d->off0 + (d->n0 & ~1u)))) {
        H.lastHashCovered = true;
        H.lastHashSrc = d->src0 + rel;
        H.lastHashNative = d->native;
        H.lastHashCls = d->cls;
        const uint8_t* src = d->src0 + rel;
        const uint32_t set = HashSetOf(src, span);
        HashE* ways = H.hash[set];
        int way = -1;
        for (int k = 0; k < 2 && way < 0; k++) {
            if (ways[k].filled != 0 && ways[k].src == src && ways[k].span == span && ways[k].native == d->native &&
                ways[k].cls == d->cls) {
                way = k;
            }
        }
        hit = way >= 0;
        if (way < 0) {
            way = ways[0].filled == 0 ? 0 : (ways[1].filled == 0 ? 1 : (H.hashMru[set] ^ 1));
        }
        H.hashMru[set] = static_cast<uint8_t>(way);
        he = &ways[way];
        if (hit) {
            uint64_t now[2];
            gdx_gfx_tmem_source_stamp(now);
            if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(src), span, he->cls, he->stamp) != 0) {
                he->stamp[0] = now[0]; /* unchanged through `now`: later checks scan from here */
                he->stamp[1] = now[1];
            } else {
                hit = false;
                H.w.hashInval++;
            }
        }
        if (hit && !L.shadow) {
            H.w.hashHit++;
            *out = he->hash;
            return true;
        }
    } else {
        H.w.hashPartial++;
    }
    /* Make the bytes real (verify: check the prediction against them), then hash them. */
    uint64_t stamp[2] = { 0, 0 };
    uint8_t cls = 0, native = 0;
    const uint8_t* src = nullptr;
    if (he != nullptr) {
        stamp[0] = d->stamp[0];
        stamp[1] = d->stamp[1];
        cls = d->cls;
        native = d->native;
        src = d->src0 + rel;
    }
    const uint32_t staleBefore = H.sesStale;
    if (hit) {
        /* verify, memo hit: the direct path would NOT settle here -- the debt stays owed. Check
           the prediction against the real bytes and leave the shadow ledger as it would be. */
        DebtSettleBytes(L, *d, off, off + span);
    } else {
        DebtCut(L, off, off + span, true);
        mHudDebtN = L.n;
    }
    const uint32_t h = TmemFnv(mRdp.tmem + off, span);
    if (he != nullptr) {
        if (hit) {
            if (he->hash == h) {
                H.w.vfyHashOk++;
            } else {
                H.w.vfyHashBad++;
                H.sesVfyBad++;
            }
        } else {
            H.w.hashMiss++;
        }
        if (H.sesStale == staleBefore) {
            he->src = src;
            he->span = span;
            he->hash = h;
            he->stamp[0] = stamp[0];
            he->stamp[1] = stamp[1];
            he->native = native;
            he->cls = cls;
            he->filled = 1;
        }
    }
    *out = h;
    return true;
}

void Interp::HudDirectTicks(int what, uint64_t ticks) {
    if (mHudD != nullptr && what >= 0 && what < 4 + FLUSH_COUNT) {
        mHudD->w.ticks[what] += ticks;
    }
}

void Interp::HudDebtPayAll() {
    if (mHudD == nullptr) {
        return;
    }
    if (mHudD->led.n != 0) {
        mHudD->w.payAll++;
    }
    DebtCut(mHudD->led, 0, kTmemBytes, true);
    mHudDebtN = mHudD->led.n;
}

void Interp::HudDirectTaskEnd() {
    if (mHudD == nullptr || mHudDebtN == 0) {
        return;
    }
    Ledger& L = mHudD->led;
    mHudD->w.ends++;
    for (uint32_t i = 0; i < L.n; i++) {
        mHudD->w.endBytes += L.e[i].end - L.e[i].off;
    }
    HudDebtPayAll();
}

void Interp::HudDirectClear() {
    if (mHudD != nullptr) {
        memset(mHudD->hash, 0, sizeof(mHudD->hash));
        memset(mHudD->load, 0, sizeof(mHudD->load));
    }
}

/* ---- the load ----------------------------------------------------------------------------- */

namespace {

inline bool LoadedSame(const LoadedTexture& a, const LoadedTexture& b) {
    return a.addr == b.addr && a.orig_size_bytes == b.orig_size_bytes && a.size_bytes == b.size_bytes &&
           a.full_image_line_size_bytes == b.full_image_line_size_bytes && a.line_size_bytes == b.line_size_bytes &&
           a.immutable == b.immutable && a.native_rgba16 == b.native_rgba16;
}

} // namespace

/* A LOADBLOCK of a direct run. With its resolved form memoised, it is DpLoadBlock from the
 * same-content test on (the copy a debt, as for every direct load): the span store, the kind,
 * the stamp, textures_changed -- the resolution, the class and the descriptor come from the
 * memo. Otherwise (and under verify) the stock handler runs and the memo is filled from what
 * it did. */
void Interp::HudDirectLoadBlock(const Cmd& c, Frame& f) {
    HudDirect& H = *mHudD;
    auto& tl = mRdp.texture_to_load;
    const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    const uint16_t tmemIndex = mRdp.texture_tile[tile].tmem_index;
    H.last.valid = 0;
    const bool eligible = tl.width <= 1 && mDspCensus == 0 && mTmemSkip == 0 && mTmemCls == 0;
    if (!eligible) {
        H.w.loadStock++;
        mHudVirtualLoad = true;
        (void)Exec(c, f, 0);
        mHudVirtualLoad = false;
        return;
    }
    const uint32_t epoch = mSrc.TimgClassEpoch();
    const uint32_t set = LoadSetOf(tl.cmd.w1, c.w1);
    int way = -1;
    for (int k = 0; k < 2 && way < 0; k++) {
        const LoadE& q = H.load[set][k];
        if (q.filled != 0 && q.tw0 == tl.cmd.w0 && q.tw1 == tl.cmd.w1 && q.tw1hi == tl.cmd.w1hi && q.lw0 == c.w0 &&
            q.lw1 == c.w1 && q.tmemIndex == tmemIndex) {
            way = k;
        }
    }
    if (way < 0) {
        way = H.load[set][0].filled == 0 ? 0 : (H.load[set][1].filled == 0 ? 1 : (H.loadMru[set] ^ 1));
    }
    H.loadMru[set] = static_cast<uint8_t>(way);
    LoadE& e = H.load[set][way];
    const bool hit = e.filled != 0 && e.epoch == epoch && e.tw0 == tl.cmd.w0 && e.tw1 == tl.cmd.w1 &&
                     e.tw1hi == tl.cmd.w1hi && e.lw0 == c.w0 && e.lw1 == c.w1 && e.tmemIndex == tmemIndex;
    if (hit && !H.led.shadow) {
        H.w.loadHit++;
        TexSkipNoteWriter(0xF3); /* == DpLoadBlock's first statement ([native] machines_texskip tripwire) */
        mStats.tmemLoads++;
        tl.addr = e.tlAddr;
        tl.immutable = e.imm;
        tl.native_rgba16 = e.native;
        const LoadedTexture loaded = e.loaded;
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
        uint32_t testBytes = 0;
        if (tmemByteOffset < kTmemBytes) {
            testBytes = loaded.size_bytes < kTmemBytes - tmemByteOffset ? loaded.size_bytes
                                                                        : kTmemBytes - tmemByteOffset;
        }
        uint64_t srcStamp[2];
        unsigned srcCls = GDX_TMEMSRC_UNKNOWN;
        HudLoadForm form;
        memset(&form, 0, sizeof(form));
        form.loaded = loaded;
        form.tlAddr = e.tlAddr;
        form.copyBytes = e.copyBytes;
        form.tmemIndex = tmemIndex;
        form.cls = e.cls;
        form.native = e.native;
        form.imm = e.imm;
        if (TmemLoadIsRedundant(tmemIndex, 1, loaded, testBytes, srcStamp, &srcCls)) {
            mStats.tmemSkips++;
            form.skipped = 1;
            form.valid = 1; /* tmemskip/tmemcls are 0 here: the skip is SameContentLoad's */
            H.last = form;
            return;
        }
        form.valid = srcCls == GDX_TMEMSRC_UNKNOWN && srcStamp[0] == 0 && srcStamp[1] == 0 ? 1 : 0;
        HudDirectApplySpan(form, srcStamp, srcCls);
        H.last = form;
        return;
    }
    /* The stock load (a debt all the same), then its resolved form, read back. */
    H.w.loadMiss++;
    gdx3ds_hitch_add(GDX_HITCH_EV_HUD_MISS, 1u); /* HITCH */
    H.noted = false;
    mHudVirtualLoad = true;
    (void)Exec(c, f, 0);
    mHudVirtualLoad = false;
    const bool live = tmemIndex < 512u && mRdp.tmem_span_base[tmemIndex] == static_cast<uint16_t>(tmemIndex + 1u) &&
                      mRdp.tmem_span_load_kind[tmemIndex] == 1u;
    if (!live || !H.noted || tl.addr == nullptr) {
        return; /* nothing copied (unresolved, skipped, unreadable): not memoised */
    }
    LoadedTexture got = mRdp.loaded_texture[tmemIndex];
    got.tmem_start = 0;
    got.tmem_word_count = 0;
    if (hit) {
        const bool same = LoadedSame(got, e.loaded) && e.tlAddr == tl.addr && e.imm == tl.immutable &&
                          e.native == tl.native_rgba16 && e.copyBytes == H.noteN && e.cls == H.noteCls &&
                          H.noteSrc == got.addr;
        if (same) {
            H.w.vfyLoadOk++;
        } else {
            H.w.vfyLoadBad++;
            H.sesVfyBad++;
        }
    }
    if (H.noteSrc != got.addr) {
        return;
    }
    e.tw0 = tl.cmd.w0;
    e.tw1 = tl.cmd.w1;
    e.tw1hi = tl.cmd.w1hi;
    e.lw0 = c.w0;
    e.lw1 = c.w1;
    e.epoch = epoch;
    e.tmemIndex = tmemIndex;
    e.tlAddr = tl.addr;
    e.imm = tl.immutable;
    e.native = tl.native_rgba16;
    e.loaded = got;
    e.copyBytes = H.noteN;
    e.cls = H.noteCls;
    e.filled = 1;
    /* The stock load just ran (with a debt): its form is the memo entry just filled, applied
     * with the stamp the stock path recorded. */
    if (mTmemSkip == 0 && mTmemCls == 0) {
        H.last.skipped = 0;
        H.last.loaded = e.loaded;
        H.last.tlAddr = e.tlAddr;
        H.last.copyBytes = e.copyBytes;
        H.last.tmemIndex = tmemIndex;
        H.last.cls = e.cls;
        H.last.native = e.native;
        H.last.imm = e.imm;
        H.last.valid = 1;
    }
}

/* The hit path's tail: the debt, the span store, the kind, the stamp, textures_changed. */
void Interp::HudDirectApplySpan(const HudLoadForm& form, const uint64_t srcStamp[2], unsigned srcCls) {
    HudDirect& H = *mHudD;
    const uint16_t tmemIndex = form.tmemIndex;
    const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
    uint32_t tmemWriteEndByte = 0;
    if (form.loaded.addr != nullptr && tmemByteOffset < kTmemBytes && form.copyBytes != 0) {
        uint64_t stamp[2];
        gdx_gfx_tmem_source_stamp(stamp); /* before the bytes would have been read */
        DebtAdd(H.led, tmemByteOffset, form.copyBytes, form.loaded.addr, form.loaded.native_rgba16 != 0, form.cls,
                stamp);
        mHudDebtN = H.led.n;
        H.w.debtNotes++;
        H.w.debtBytes += form.copyBytes;
        mRdp.tmem_generation++;
        tmemWriteEndByte = tmemByteOffset + form.copyBytes;
    }
    StoreLoadedTexture(tmemIndex, form.loaded);
    NoteTmemLoadKind(tmemIndex, 1, tmemWriteEndByte);
    RecordTmemSrcStamp(tmemIndex, srcStamp, srcCls);
    ArmTexturesChanged();
}

/* [native] hud_retain: a retained run's load, replayed. With tmemskip and tmemcls off (the only
 * forms HudDirectLastLoad reports valid) DpLoadBlock's same-content test is SameContentLoad
 * (false for a non-immutable source; for an immutable one the retention fingerprint has
 * checked that it gives the recorded answer) and its stamp is {0, 0} under UNKNOWN. */
void Interp::HudDirectApplyLoad(const HudLoadForm& form, bool countLoad) {
    if (countLoad) {
        mStats.tmemLoads++;
    }
    TexSkipNoteWriter(0xF3);
    if (form.skipped != 0) {
        mStats.tmemSkips++;
        return;
    }
    const uint64_t zero[2] = { 0, 0 };
    HudDirectApplySpan(form, zero, GDX_TMEMSRC_UNKNOWN);
}

const Interp::HudLoadForm* Interp::HudDirectLastLoad() const {
    return mHudD != nullptr ? &mHudD->last : nullptr;
}

bool Interp::HudDirectLastHashCovered() const {
    return mHudD != nullptr && mHudD->lastHashCovered;
}

/* The covering debt of the last covered TMEM hash (HudDebtHash's memo condition). */
void Interp::HudDirectLastHashSource(const uint8_t** src, uint8_t* native, uint8_t* cls) const {
    *src = mHudD != nullptr ? mHudD->lastHashSrc : nullptr;
    *native = mHudD != nullptr ? mHudD->lastHashNative : 0;
    *cls = mHudD != nullptr ? mHudD->lastHashCls : 0;
}

/* Is TMEM [off, off + span) still owed, whole, by one debt whose bytes there mirror `src`
 * (HudDebtHash's memo condition)? Then the span's hash is a function of those source bytes. */
bool Interp::HudDebtCoverSame(uint32_t off, uint32_t span, const uint8_t* src, uint8_t native, uint8_t cls) const {
    if (mHudD == nullptr || span == 0 || off + span > kTmemBytes) {
        return false;
    }
    const Ledger& L = mHudD->led;
    bool any = false;
    const int ci = DebtCover(L, off, off + span, &any);
    if (ci < 0) {
        return false;
    }
    const Debt& d = L.e[ci];
    const uint32_t rel = off - d.off0;
    const bool aligned = d.native == 0 || ((rel & 1u) == 0 && off + span <= d.off0 + (d.n0 & ~1u));
    return aligned && d.src0 + rel == src && d.native == native && d.cls == cls;
}

/* The ledger as an opaque snapshot, and TMEM as it will be once every debt is paid (the
 * retention verify compares these, not the ledger's internal layout). */
size_t Interp::HudDebtStateBytes() const {
    return sizeof(Ledger);
}

void Interp::HudDebtStateSave(void* out) const {
    if (mHudD != nullptr) {
        memcpy(out, &mHudD->led, sizeof(Ledger));
    }
}

void Interp::HudDebtStateLoad(const void* in) {
    if (mHudD != nullptr) {
        memcpy(&mHudD->led, in, sizeof(Ledger));
        mHudDebtN = mHudD->led.n;
    }
}

void Interp::HudDebtMaterialize(uint8_t* out) const {
    memcpy(out, mRdp.tmem, kTmemBytes);
    if (mHudD == nullptr) {
        return;
    }
    const Ledger& L = mHudD->led;
    if (L.shadow) {
        return; /* verify ledger: TMEM already holds the real bytes */
    }
    for (uint32_t i = 0; i < L.n; i++) {
        const Debt& d = L.e[i];
        for (uint32_t b = d.off; b < d.end; b++) {
            out[b] = MirrorByte(d.src0, d.n0, d.native != 0, b - d.off0);
        }
    }
}

/* ---- C5 Stage 3b: the same load, in a direct-load scope outside the HUD ----------------------
 *
 * (port/3ds/gfx/gdx3ds_dress.h.) A walker G_LOADBLOCK inside a scope the decomp opened (the
 * gadget pass, the decoration passes, the effects section), or a LOADBLOCK of a shadow / boost-
 * flame replay run under [native] fxshd_direct, runs HudDirectLoadBlock above: the resolved form
 * from the load memo (so each texture is resolved once per course), the TMEM bytes left as debt
 * on the SAME ledger the HUD uses (so every TMEM reader and the task end still settle it), the
 * texture key's hash from the per-source memo when the import meets it. Nothing else of the
 * interpreter changes: the span store, the stamp, textures_changed run as in the stock load.
 * The level decides the ledger's mode for this load: 2 = verify (the stock load runs, the memo's
 * descriptor is compared, the debt is a shadow compared with TMEM wherever it would be paid).
 * The HUD's own [hudd] load counters are left as they were; this path counts in [dressd]. */
void Interp::DirectScopeLoadBlock(const Cmd& c, Frame& f, int kind, int level) {
    mDirectInLoad = true;
    if (!HudDirectEnsure()) {
        (void)Exec(c, f, 0);
        mDirectInLoad = false;
        return;
    }
    HudDirect& H = *mHudD;
    H.led.shadow = level == 2;
    const uint32_t hit0 = H.w.loadHit, miss0 = H.w.loadMiss, stock0 = H.w.loadStock;
    const uint32_t vok0 = H.w.vfyLoadOk, vbad0 = H.w.vfyLoadBad;
    HudDirectLoadBlock(c, f);
    const int k = (kind >= 0 && kind < 4) ? kind : 0;
    mDirectWin.hit[k] += H.w.loadHit - hit0;
    mDirectWin.miss[k] += H.w.loadMiss - miss0;
    mDirectWin.stock[k] += H.w.loadStock - stock0;
    mDirectWin.vfyOk += H.w.vfyLoadOk - vok0;
    mDirectWin.vfyBad += H.w.vfyLoadBad - vbad0;
    H.w.loadHit = hit0;
    H.w.loadMiss = miss0;
    H.w.loadStock = stock0;
    H.w.vfyLoadOk = vok0;
    H.w.vfyLoadBad = vbad0;
    mDirectInLoad = false;
}

/* [dressd] per 64 tasks, per task: loads served from the memo / resolved stock and memoised / not
 * eligible, by scope (shd = the shadow + flame replay runs, gad, deco, fx), and verify compares. */
GDX_COLD int Interp::DirectFormat(char* buf, size_t cap, uint32_t tasks) {
    const DirectWin& w = mDirectWin;
    uint32_t any = w.vfyOk + w.vfyBad;
    for (int k = 0; k < 4; k++) {
        any += w.hit[k] + w.miss[k] + w.stock[k];
    }
    if (any == 0 || tasks == 0 || cap == 0) {
        return 0;
    }
    static const char* const kName[4] = { "shd", "gad", "deco", "fx" };
    const double inv = 1.0 / static_cast<double>(tasks);
    int n = snprintf(buf, cap, "[dressd] tasks=%u levels=%d/%d/%d/%d", static_cast<unsigned>(tasks), mDirectLevels[0],
                     mDirectLevels[1], mDirectLevels[2], mDirectLevels[3]);
    for (int k = 0; k < 4 && n > 0 && static_cast<size_t>(n) < cap; k++) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | %s hit=%.2f miss=%.2f stock=%.2f", kName[k],
                      w.hit[k] * inv, w.miss[k] * inv, w.stock[k] * inv);
    }
    if (n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | vfy load=%u/%u ses_bad=%u stale_ses=%u", w.vfyOk,
                      w.vfyBad, mHudD != nullptr ? mHudD->sesVfyBad : 0u, mHudD != nullptr ? mHudD->sesStale : 0u);
    }
    memset(&mDirectWin, 0, sizeof(mDirectWin));
    return n;
}

/* ---- the rectangle ------------------------------------------------------------------------ */

namespace {

/* == gdx_rect.cpp TrectRectEligible */
bool TrectEligible(const Rdp& rdp, uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt) {
    const Tile& tt = rdp.texture_tile[tile];
    const bool clampS = ((tt.cms & TX_CLAMP) != 0 || tt.masks == 0) && !((tt.cms & TX_MIRROR) != 0 && tt.masks != 0);
    const bool clampT = ((tt.cmt & TX_CLAMP) != 0 || tt.maskt == 0) && !((tt.cmt & TX_MIRROR) != 0 && tt.maskt != 0);
    if (!clampS || !clampT) {
        return false;
    }
    if (tt.shifts != 0 || tt.shiftt != 0) {
        return false;
    }
    const float s0 = static_cast<float>(uls) / 32.0f, s1 = lrs / 32.0f;
    const float t0 = static_cast<float>(ult) / 32.0f, t1 = lrt / 32.0f;
    const float sMin = s0 < s1 ? s0 : s1, sMax = s0 < s1 ? s1 : s0;
    const float tMin = t0 < t1 ? t0 : t1, tMax = t0 < t1 ? t1 : t0;
    return sMin >= tt.uls / 4.0f - 0.01f && sMax <= tt.lrs / 4.0f + 1.01f && tMin >= tt.ult / 4.0f - 0.01f &&
           tMax <= tt.lrt / 4.0f + 1.01f;
}

/* A TEXRECT's operands, as Exec decodes them. */
struct RectArgs {
    int32_t ulx, uly, lrx, lry;
    uint8_t tile;
    int16_t uls, ult, dsdx, dtdy;
    bool flip;
};

RectArgs DecodeRect(const Cmd& c, const Cmd& h1, const Cmd& h2) {
    RectArgs r;
    r.lrx = static_cast<int32_t>(C0(c.w0, 12, 12));
    r.lry = static_cast<int32_t>(C0(c.w0, 0, 12));
    r.tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    r.ulx = static_cast<int32_t>(C1(c.w1, 12, 12));
    r.uly = static_cast<int32_t>(C1(c.w1, 0, 12));
    r.uls = static_cast<int16_t>(C1(h1.w1, 16, 16));
    r.ult = static_cast<int16_t>(C1(h1.w1, 0, 16));
    r.dsdx = static_cast<int16_t>(C1(h2.w1, 16, 16));
    r.dtdy = static_cast<int16_t>(C1(h2.w1, 0, 16));
    r.flip = Opcode(c.w0) == OP_TEXRECTFLIP;
    return r;
}

} // namespace

/* [native] hud_retain: TrectEligible for the live tile (the retention variant installs the
 * recorded tile before it asks). */
bool Interp::HudTrectEligible(uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt) const {
    return TrectEligible(mRdp, tile, uls, ult, lrs, lrt);
}

/* == DpTextureRectangle's u/v and DrawRectangle's positions (the COPY cycle's widened extent
 * included), into
 * v[0..3] = ul, ll, lr, ur (x, y, z, w, u, v only: DrawRectangle never writes the colour or the
 * clip flags of these four slots). Same expressions, same order, same types. */
bool Interp::HudRectGeometry(const Cmd& c, const Cmd& h1, const Cmd& h2, LoadedVertex v[4], float* lrsOut,
                             float* lrtOut) const {
    const RectArgs r = DecodeRect(c, h1, h2);
    int16_t dsdx = r.dsdx;
    int16_t dtdy = r.dtdy;
    const int16_t uls = r.uls;
    const int16_t ult = r.ult;
    int32_t lrx = r.lrx, lry = r.lry;
    const int32_t ulx = r.ulx, uly = r.uly;
    if ((mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_COPY) {
        if (dsdx > 0x0400 || dsdx < -0x0400) {
            dsdx = static_cast<int16_t>(dsdx >> 2);
        }
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    if (r.flip) {
        dsdx = static_cast<int16_t>(-dsdx);
        dtdy = static_cast<int16_t>(-dtdy);
    }
    const int16_t width = static_cast<int16_t>(!r.flip ? lrx - ulx : lry - uly);
    const int16_t height = static_cast<int16_t>(!r.flip ? lry - uly : lrx - ulx);
    const float lrs = static_cast<float>(((uls << 7) + dsdx * width) >> 7);
    const float lrt = static_cast<float>(((ult << 7) + dtdy * height) >> 7);
    LoadedVertex* ul = &v[0];
    LoadedVertex* ll = &v[1];
    LoadedVertex* lr = &v[2];
    LoadedVertex* ur = &v[3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!r.flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    *lrsOut = lrs;
    *lrtOut = lrt;
    HudRectPositions(ulx, uly, lrx, lry, v);
    return r.flip;
}

/* == DrawRectangle's four corner positions (x, y, z, w) into v[0..3] = ul, ll, lr, ur. */
void Interp::HudRectPositions(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, LoadedVertex v[4]) const {
    LoadedVertex* ul = &v[0];
    LoadedVertex* ll = &v[1];
    LoadedVertex* lr = &v[2];
    LoadedVertex* ur = &v[3];
    const float halfW = static_cast<float>(mCfg.nativeW / 2);
    const float halfH = static_cast<float>(mCfg.nativeH / 2);
    float ulxf = static_cast<float>(ulx);
    float ulyf = static_cast<float>(uly);
    float lrxf = static_cast<float>(lrx);
    float lryf = static_cast<float>(lry);
    ulxf = ulxf / (4.0f * halfW) - 1.0f;
    ulyf = -(ulyf / (4.0f * halfH)) + 1.0f;
    lrxf = lrxf / (4.0f * halfW) - 1.0f;
    lryf = -(lryf / (4.0f * halfH)) + 1.0f;
    {
        const float currentAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
        const float aspectScale = (4.0f / 3.0f) / currentAspect;
        const uint32_t widescreenMode = mRsp.extra_geometry_mode & (0x04u | 0x08u | 0x10u | 0x20u);
        const bool widescreenFrameActive = currentAspect > (4.0f / 3.0f) && mCfg.widescreen && !mCfg.forceFixedAspect;
        const bool stretchActive = widescreenFrameActive && (widescreenMode & 0x04u) != 0;
        const bool enhancedWidescreenUi = widescreenFrameActive && widescreenMode != 0 && mCfg.widescreenUi;
        if (stretchActive) {
            constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
            ulxf *= kSafeAreaScale;
            lrxf *= kSafeAreaScale;
        } else if (enhancedWidescreenUi && (widescreenMode & 0x20u) != 0) {
            const float center = (ulxf + lrxf) * 0.5f;
            const float halfWidth = (lrxf - ulxf) * 0.5f * aspectScale;
            ulxf = center - halfWidth;
            lrxf = center + halfWidth;
        } else {
            ulxf = AdjXForAspectRatio(ulxf);
            lrxf = AdjXForAspectRatio(lrxf);
            if (enhancedWidescreenUi) {
                const bool anchorLeft = (widescreenMode & 0x08u) != 0;
                const bool anchorRight = (widescreenMode & 0x10u) != 0;
                if (anchorLeft != anchorRight) {
                    const float uiAspect = currentAspect < mCfg.hudMaxAspect ? currentAspect : mCfg.hudMaxAspect;
                    float offset = (anchorRight ? 1.0f : -1.0f) * ((uiAspect - (4.0f / 3.0f)) / currentAspect);
                    if (mCfg.removeBorders) {
                        offset += (anchorRight ? 1.0f : -1.0f) * (0.1f / currentAspect);
                    }
                    ulxf += offset;
                    lrxf += offset;
                }
            }
        }
    }
    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;
    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;
    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;
    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;
}

namespace {

/* == SpTri's gdxInput lambda (gdx_tri.cpp): combiner input j of channel k at vertex vi, for a
 * triangle whose first vertex is v1. */
PackIn GdxInput(const Rdp& rdp, const Combiner* comb, int j, int k, const LoadedVertex* vi, const LoadedVertex* v1) {
    Rgba tmp = { 0, 0, 0, 0 };
    bool isShade = false;
    switch (comb->shader_input_mapping[k][j]) {
        case CCMUX_PRIMITIVE: tmp = rdp.prim_color; break;
        case CCMUX_SHADE: tmp = vi->color; isShade = true; break;
        case CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = vi->color.a; break;
        case CCMUX_ENVIRONMENT: tmp = rdp.env_color; break;
        case CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = rdp.prim_color.a; break;
        case CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = rdp.env_color.a; break;
        case CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = rdp.prim_lod_fraction; break;
        case CCMUX_LOD_FRACTION: {
            if (rdp.other_mode_l & (1u << MDSFT_TEXTLOD)) {
                float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                if (distance_frac < 0.0f) {
                    distance_frac = 0.0f;
                }
                if (distance_frac > 1.0f) {
                    distance_frac = 1.0f;
                }
                tmp.r = tmp.g = tmp.b = tmp.a = static_cast<uint8_t>(distance_frac * 255.0f);
            } else {
                tmp.r = tmp.g = tmp.b = tmp.a = 255;
            }
            break;
        }
        case CCMUX_KEY_CENTER: tmp = rdp.key_center; break;
        case CCMUX_KEY_SCALE: tmp = rdp.key_scale; break;
        case CCMUX_CONVERT_K4: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(rdp.convert_k[4]); break;
        case CCMUX_CONVERT_K5: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(rdp.convert_k[5]); break;
        default: break;
    }
    if (k == 1 && comb->shader_input_mapping[k][j] == ACMUX_PRIM_LOD_FRAC) {
        tmp = Rgba{ 0, 0, 0, rdp.prim_lod_fraction };
        isShade = false;
    }
    return PackIn{ tmp, isShade };
}

} // namespace

/* == SpTri's packed emission (gdx_tri.cpp, from the clamp limits to the last vertex) for `n`
 * vertices of a triangle whose first vertex is `v1`, minus the batch bookkeeping: writes
 * n x GDX3DS_VBOPACK_STRIDE floats at `out`. A vertex's floats depend on the triangle only
 * through v1->w (CCMUX_LOD_FRACTION), which is 1.0 for all four corners of a rectangle, so a
 * rectangle packs its four corners once and writes them as its two triangles. Reads the state
 * and mPackParams only, so the verify mode predicts a stock rectangle with the same code. */
void Interp::HudPackVerts(const DrawState& ds, const LoadedVertex* const* v, int n, const LoadedVertex* v1,
                          float* out, bool halfTexel) const {
    const Combiner* comb = ds.comb;
    const uint32_t tm = ds.tm;
    const bool use_fog = ds.use_fog;
    const bool use_blend_color = ds.use_blend_color;

    float clampU[2] = { 1e30f, 1e30f };
    float clampV[2] = { 1e30f, 1e30f };
    for (int t = 0; t < 2; t++) {
        if (!ds.usedTextures[t]) {
            continue;
        }
        clampU[t] = mPackParams.wrapClampU[t];
        clampV[t] = mPackParams.wrapClampV[t];
        if (mPackParams.shaderClampGate) {
            if (tm & (1u << (2 * t))) {
                clampU[t] = ((ds.tex_width2[t] - 0.5f) / ds.tex_width[t]) * mPackParams.uScale[t];
            }
            if (tm & (1u << (2 * t + 1))) {
                clampV[t] = ((ds.tex_height2[t] - 0.5f) / ds.tex_height[t]) * mPackParams.vScale[t];
            }
        }
    }

    /* halfTexel = bilerp && !is_rect: never for a rectangle; the sky's compact triangle path
     * ([native] hudsky_emit, gdx_hudsky_tri.inc) passes it for its triangles. */
    struct UvPrep {
        float uOff, vOff, uPre, vPre, uNormMul, vNormMul, uNormDiv, vNormDiv;
        bool uMul, vMul;
    } prep[2];
    for (int t = 0; t < 2; t++) {
        if (!ds.usedTextures[t]) {
            continue;
        }
        const Tile& ut = mRdp.texture_tile[ds.effective_tile[t]];
        const int shifts = ut.shifts, shiftt = ut.shiftt;
        prep[t].uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / static_cast<float>(1 << shifts) : static_cast<float>(1 << (16 - shifts)));
        prep[t].vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / static_cast<float>(1 << shiftt) : static_cast<float>(1 << (16 - shiftt)));
        prep[t].uOff = ut.uls / 4.0f;
        prep[t].vOff = ut.ult / 4.0f;
        const uint32_t tw = ds.tex_width[t], th = ds.tex_height[t];
        prep[t].uMul = (tw & (tw - 1u)) == 0u;
        prep[t].vMul = (th & (th - 1u)) == 0u;
        prep[t].uNormDiv = static_cast<float>(tw);
        prep[t].vNormDiv = static_cast<float>(th);
        prep[t].uNormMul = prep[t].uMul ? 1.0f / static_cast<float>(tw) : 1.0f;
        prep[t].vNormMul = prep[t].vMul ? 1.0f / static_cast<float>(th) : 1.0f;
    }
    const int ci = mPackParams.vtxColorInput - 1;
    const bool colorIsShade = ci >= 0 && comb->shader_input_mapping[0][ci] == CCMUX_SHADE;
    const int ai = mPackParams.vtxAlphaInput - 1;
    const bool alphaIsShade = ai >= 0 && comb->shader_input_mapping[1][ai] == CCMUX_SHADE;
    const bool fogFactorConst = mPackParams.fogFactorToAlpha && use_blend_color;
    const float fogFactorConstVal = fogFactorConst ? mByteToUnit[mRdp.fog_color.a] : 0.0f;
    const bool shadeAlphaIsOne = use_fog && !use_blend_color;
    for (int i = 0; i < n; i++) {
        float* o = out + i * GDX3DS_VBOPACK_STRIDE;
        const LoadedVertex* vv = v[i];
        float z = vv->z, w = vv->w;
        if (ds.clip.zIsFrom0To1) {
            z = (z + w) / 2.0f;
        }
        o[0] = vv->x;
        o[1] = ds.clip.invertY ? -vv->y : vv->y;
        o[2] = z;
        o[3] = w;
        for (int t = 0; t < 2; t++) {
            const int slot = (t == 0) ? 4 : 10;
            if (!ds.usedTextures[t]) {
                o[slot] = 0.0f;
                o[slot + 1] = 0.0f;
                continue;
            }
            float u = vv->u / 32.0f;
            float vt = vv->v / 32.0f;
            u *= prep[t].uPre;
            vt *= prep[t].vPre;
            u -= prep[t].uOff;
            vt -= prep[t].vOff;
            if (halfTexel) {
                u += 0.5f;
                vt += 0.5f;
            }
            const float nu = prep[t].uMul ? u * prep[t].uNormMul : u / prep[t].uNormDiv;
            const float nv = prep[t].vMul ? vt * prep[t].vNormMul : vt / prep[t].vNormDiv;
            const float pu = FminExact(nu * mPackParams.uScale[t], clampU[t]);
            const float pv = FminExact(nv * mPackParams.vScale[t], clampV[t]);
            if (mPackParams.hasOff) {
                o[slot] = pu + mPackParams.uOff[t];
                o[slot + 1] = pv + mPackParams.vOff[t];
            } else {
                o[slot] = pu;
                o[slot + 1] = pv;
            }
        }
        if (ci >= 0) {
            if (colorIsShade) {
                o[6] = mByteToUnit[vv->color.r];
                o[7] = mByteToUnit[vv->color.g];
                o[8] = mByteToUnit[vv->color.b];
            } else {
                const PackIn cv = GdxInput(mRdp, comb, ci, 0, vv, v1);
                o[6] = mByteToUnit[cv.c.r];
                o[7] = mByteToUnit[cv.c.g];
                o[8] = mByteToUnit[cv.c.b];
            }
        } else {
            o[6] = o[7] = o[8] = 1.0f;
        }
        if (mPackParams.fogFactorToAlpha) {
            o[9] = fogFactorConst ? fogFactorConstVal : mByteToUnit[vv->color.a];
        } else if (mPackParams.wantVtxAlpha) {
            if (alphaIsShade) {
                o[9] = shadeAlphaIsOne ? 1.0f : mByteToUnit[vv->color.a];
            } else {
                const PackIn av = GdxInput(mRdp, comb, ai, 1, vv, v1);
                o[9] = (shadeAlphaIsOne && av.isShade) ? 1.0f : mByteToUnit[av.c.a];
            }
        } else {
            o[9] = 1.0f;
        }
    }
}

/* == SpTri's vertex-0 batch record, written at a batch's first triangle (first vertex v0). */
void Interp::HudPackAux(const DrawState& ds, const LoadedVertex* v0, Gdx3dsVboPackAux* aux) const {
    const Combiner* comb = ds.comb;
    const bool use_alpha = ds.use_alpha;
    const bool use_fog = ds.use_fog;
    const bool use_blend_color = ds.use_blend_color;
    aux->numInputs = static_cast<uint8_t>(ds.numInputs < 7 ? ds.numInputs : 7);
    for (int j = 0; j < aux->numInputs; j++) {
        const PackIn cv = GdxInput(mRdp, comb, j, 0, v0, v0);
        aux->inputRgba[j][0] = cv.c.r / 255.0f;
        aux->inputRgba[j][1] = cv.c.g / 255.0f;
        aux->inputRgba[j][2] = cv.c.b / 255.0f;
        if (use_alpha) {
            const PackIn av = GdxInput(mRdp, comb, j, 1, v0, v0);
            aux->inputRgba[j][3] = (use_fog && !use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
        } else {
            aux->inputRgba[j][3] = 1.0f;
        }
    }
    aux->hasFog = use_fog ? 1 : 0;
    if (use_fog) {
        const Rgba& fogSrc = use_blend_color ? mRdp.blend_color : mRdp.fog_color;
        aux->fogRgb[0] = fogSrc.r / 255.0f;
        aux->fogRgb[1] = fogSrc.g / 255.0f;
        aux->fogRgb[2] = fogSrc.b / 255.0f;
        aux->fogFactor0 = use_blend_color ? mRdp.fog_color.a / 255.0f : v0->color.a / 255.0f;
    } else {
        aux->fogRgb[0] = aux->fogRgb[1] = aux->fogRgb[2] = 0.0f;
        aux->fogFactor0 = 0.0f;
    }
}

/* One of a rectangle's two SpTri calls: the reject screen, the state block (or, for the second
 * call, the first one's answer -- `haveState`: nothing the state block reads has changed since,
 * so it would return `ds` again and bind nothing; the tri memo is left as that call leaves it),
 * then SpTri's batch bookkeeping around the emission: begin (GEOMLOSS cap), the vertex-0 record
 * at a batch's first triangle, the fog note, the counters, the FULL flush. `corners` holds the
 * rectangle's four packed corners for the batch they were packed under (`*packed`). */
bool Interp::HudDirectTri(const int idx[3], DrawState& ds, bool haveState, float* corners, bool* packed) {
    LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    mStats.triCalls++;
    if (mClipFanDepth == 0) {
        mStats.triTopCalls++;
        if (mDxOn) {
            ++mTriSampleSeq; /* TriClock's sample counter (receipt cadence only) */
        }
    }
    mStats.triRectCalls++;
    if (lv[idx[0]].clip_rej & lv[idx[1]].clip_rej & lv[idx[2]].clip_rej) {
        mStats.triTrivialRej++;
        return false;
    }
    if (haveState) {
        mTriMemo = ds;
        mTriMemoValid = true;
    } else {
        const uint64_t t0 = mHudClock >= 3 ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        const bool ok = (mHudRectFast && HudRectState(true, ds)) || ResolveDrawState(true, ds);
        if (GDX_UNLIKELY(mHudClock >= 3)) {
            HudDirectTicks(3, static_cast<uint64_t>(gdx_ninterp_now()) - t0);
        }
        if (!ok) {
            return false;
        }
    }
    if (gdx3ds_replay_interp != nullptr) {
        ReplayBatchTag(GDX_REPLAY_TAG_SCREEN); /* [native] replay (C6): HUD rectangles are screen space */
    }
    const bool begins = mBufNumTris == 0;
    if (begins) {
        uint32_t capTris = 0;
        mStats.triBegin++;
        mPackDst = mBackend->VboPackBegin(&mPackParams, &capTris);
        mPackBatch = (mPackDst != nullptr);
        mPackComb = ds.comb;
        *packed = false; /* the batch parameters were just (re)written */
        if (!mPackBatch) {
            mStats.packRefused++;
            mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
        } else {
            mPackCapTris = capTris < static_cast<uint32_t>(kMaxTriBuffer) ? static_cast<size_t>(capTris)
                                                                         : static_cast<size_t>(kMaxTriBuffer);
            if (mPackCapTris < static_cast<size_t>(kMaxTriBuffer)) {
                mStats.packSplit++;
            }
        }
    }
    if (!mPackBatch) {
        return true;
    }
    if (begins) {
        HudPackAux(ds, &lv[idx[0]], &mPackAux);
    }
    if (!*packed) {
        const LoadedVertex* const quad[4] = { &lv[0], &lv[1], &lv[2], &lv[3] };
        HudPackVerts(ds, quad, 4, &lv[idx[0]], corners);
        *packed = true;
    }
    float* dst = mPackDst + mBufNumTris * (3 * GDX3DS_VBOPACK_STRIDE);
    for (int k = 0; k < 3; k++) {
        memcpy(dst + k * GDX3DS_VBOPACK_STRIDE, corners + idx[k] * GDX3DS_VBOPACK_STRIDE,
               GDX3DS_VBOPACK_STRIDE * sizeof(float));
    }
    if (ds.use_fog) {
        if (ds.use_blend_color) {
            mBackend->FogNoteParams(0, 0, 0);
        } else {
            mBackend->FogNoteParams(mRsp.fog_mul, mRsp.fog_offset, 1);
        }
    }
    mStats.triEmitAll++;
    if (++mBufNumTris >= mPackCapTris) {
        Flush(FLUSH_FULL);
    }
    return true;
}

/* A TEXRECT of a direct HUD run: DpTextureRectangle + DrawRectangle with the two SpTri calls
 * replaced by HudDirectTri. */
void Interp::HudDirectTexRect(const Cmd& c, Frame& f) {
    HudWin& w = mHudD->w;
    if (f.idx + 2 > f.list.limit) {
        w.rectStock++;
        (void)Exec(c, f, 0); /* a malformed tail keeps the stock handler */
        return;
    }
    const bool copyCycle = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_COPY;
    const Cmd h1 = ReadCmd(f.list, f.idx);
    const Cmd h2 = ReadCmd(f.list, f.idx + 1);
    f.idx += 2;
    mStats.op[Opcode(h1.w0)]++;
    mStats.op[Opcode(h2.w0)]++;
    mStats.cmds += 2;
    w.rects++;

    /* == DpTextureRectangle */
    mStats.rects++;
    const uint64_t saved_combine_mode = mRdp.combine_mode;
    if (copyCycle) {
        /* DpSetCombineMode(ColorComb(0, 0, 0, CCMUX_TEXEL0), AlphaComb(0, 0, 0, ACMUX_TEXEL0), 0, 0) */
        DpSetCombineMode((CCMUX_TEXEL0 & 7u) << 13, (ACMUX_TEXEL0 & 7u) << 9, 0, 0);
    }
    LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    float lrs = 0.0f, lrt = 0.0f;
    (void)HudRectGeometry(c, h1, h2, lv, &lrs, &lrt);
    const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    const uint8_t saved_tile = mRdp.first_tile_index;
    if (saved_tile != tile) {
        ArmTexturesChanged();
    }
    mRdp.first_tile_index = tile;
    mTrectRectAtlasOk = mTrectBatchOn && TrectEligible(mRdp, tile, static_cast<int16_t>(C1(h1.w1, 16, 16)),
                                                       static_cast<int16_t>(C1(h1.w1, 0, 16)), lrs, lrt);
    mHudLastTrectOk = mTrectRectAtlasOk; /* [native] hud_retain records it with the rectangle */

    /* == DrawRectangle (the positions were written by HudRectGeometry) */
    const uint32_t saved_other_mode_h = mRdp.other_mode_h;
    if (copyCycle) {
        mRdp.other_mode_h = (mRdp.other_mode_h & ~(3u << MDSFT_TEXTFILT)) | TF_POINT;
    }
    Rect default_viewport;
    default_viewport.x = 0;
    default_viewport.y = static_cast<int16_t>(mCfg.nativeH);
    default_viewport.width = mCfg.nativeW;
    default_viewport.height = mCfg.nativeH;
    AdjustViewportOrScissor(&default_viewport);
    const Rect viewport_saved = mRdp.viewport;
    const uint32_t geometry_mode_saved = mRsp.geometry_mode;
    mRdp.viewport = default_viewport;
    mRdp.viewport_or_scissor_changed = true;
    mRsp.geometry_mode = 0;
    {
        ProfScope prof(mBackend, 3);
        static const int kTriA[3] = { 0, 1, 3 }; /* SpTri(ul, ll, ur) */
        static const int kTriB[3] = { 1, 2, 3 }; /* SpTri(ll, lr, ur) */
        float corners[4 * GDX3DS_VBOPACK_STRIDE];
        bool packed = false;
        DrawState ds;
        const bool resolved = HudDirectTri(kTriA, ds, false, corners, &packed);
        (void)HudDirectTri(kTriB, ds, resolved, corners, &packed);
    }
    mRsp.geometry_mode = geometry_mode_saved;
    mRdp.viewport = viewport_saved;
    mRdp.viewport_or_scissor_changed = true;
    if (copyCycle) {
        mRdp.other_mode_h = saved_other_mode_h;
    }

    mTrectRectAtlasOk = false;
    if (saved_tile != tile) {
        ArmTexturesChanged();
    }
    mRdp.first_tile_index = saved_tile;
    mRdp.combine_mode = saved_combine_mode;
}

/* hud_direct=2: the stock rectangle runs, then its two triangles are predicted from the same
 * state with the direct path's own geometry and emission and compared bit for bit. */
GDX_COLD void Interp::HudVerifyTexRect(const Cmd& c, Frame& f) {
    HudWin& w = mHudD->w;
    const bool whole = f.idx + 2 <= f.list.limit;
    Cmd h1 = {}, h2 = {};
    if (whole) {
        h1 = ReadCmd(f.list, f.idx);
        h2 = ReadCmd(f.list, f.idx + 1);
    }
    const uint32_t emitted0 = mStats.triEmitAll;
    (void)Exec(c, f, 0);
    if (!whole || mStats.triEmitAll - emitted0 != 2u || !mPackBatch || mBufNumTris < 2 ||
        !mTriMemoValid || mPackDst == nullptr) {
        w.vfyRectSkip++;
        return;
    }
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    LoadedVertex pv[4];
    for (int i = 0; i < 4; i++) {
        pv[i] = lv[i]; /* colour and clip flags: DrawRectangle leaves them as they were */
    }
    float lrs = 0.0f, lrt = 0.0f;
    (void)HudRectGeometry(c, h1, h2, pv, &lrs, &lrt);
    float predict[2 * kTriFloats];
    Gdx3dsVboPackAux aux;
    memset(&aux, 0, sizeof(aux));
    const bool began = mBufNumTris == 2;
    float corners[4 * GDX3DS_VBOPACK_STRIDE];
    const LoadedVertex* const quad[4] = { &pv[0], &pv[1], &pv[2], &pv[3] };
    HudPackVerts(mTriMemo, quad, 4, &pv[0], corners);
    static const int kOrder[6] = { 0, 1, 3, 1, 2, 3 }; /* SpTri(ul, ll, ur), SpTri(ll, lr, ur) */
    for (int k = 0; k < 6; k++) {
        memcpy(predict + k * GDX3DS_VBOPACK_STRIDE, corners + kOrder[k] * GDX3DS_VBOPACK_STRIDE,
               GDX3DS_VBOPACK_STRIDE * sizeof(float));
    }
    if (began) {
        HudPackAux(mTriMemo, &pv[0], &aux);
    }
    const float* got = mPackDst + (mBufNumTris - 2) * kTriFloats;
    bool same = memcmp(predict, got, sizeof(predict)) == 0;
    if (same && began) {
        same = aux.numInputs == mPackAux.numInputs && aux.hasFog == mPackAux.hasFog &&
               memcmp(aux.fogRgb, mPackAux.fogRgb, sizeof(aux.fogRgb)) == 0 &&
               memcmp(&aux.fogFactor0, &mPackAux.fogFactor0, sizeof(float)) == 0 &&
               memcmp(aux.inputRgba, mPackAux.inputRgba, sizeof(aux.inputRgba[0]) * aux.numInputs) == 0;
    }
    if (same) {
        w.vfyRectOk++;
    } else {
        w.vfyRectBad++;
        mHudD->sesVfyBad++;
    }
}

/* ---- the receipt -------------------------------------------------------------------------- */

/* [hudd] per rendered task (64-task window): direct runs; rects emitted direct / stock;
 * direct-run LOADBLOCKs; debt noted (count, bytes; unk = sources with no staleness test,
 * copied), superseded bytes, payments (count, bytes, stale = source moved: MUST be 0, the
 * session total rides along), bytes left for the task end, ledger overflows; the hash memo
 * (hit = a HUD texture whose key needed no hash, miss = resolved (hashed) this frame,
 * inval = the source generation moved, part = span not inside one debt); verify ok/bad for
 * the ledger, the memo and the rectangles (skip = a rect the check could not see). */
GDX_COLD int Interp::HudDirectFormat(char* buf, size_t cap, uint32_t tasks) {
    if (mHudD == nullptr || cap == 0 || tasks == 0) {
        return 0;
    }
    HudWin& w = mHudD->w;
    if (w.runs == 0 && w.pays == 0 && w.trimBytes == 0) {
        return 0;
    }
    const double inv = 1.0 / static_cast<double>(tasks);
    const int n = snprintf(
        buf, cap,
        "[hudd] tasks=%u mode=%d runs=%.2f rects=%.1f stock=%.2f | debt notes=%.1f kb=%.2f unk=%u "
        "trim=%.0f pays=%.2f paid=%.0f all=%.2f stale=%u ses=%u end=%.0f/%.2f ovf=%u | hash hit=%.1f miss=%.2f inval=%u part=%u | "
        "load hit=%.1f miss=%.2f stock=%.2f | vfy debt=%u/%u hash=%u/%u load=%u/%u rect=%u/%u skip=%u ses_bad=%u",
        static_cast<unsigned>(tasks), mHudDirect, w.runs * inv, w.rects * inv, w.rectStock * inv,
        w.debtNotes * inv, w.debtBytes * inv / 1024.0, w.debtUnknown, w.trimBytes * inv, w.pays * inv,
        w.payBytes * inv, (w.payAll - w.ends) * inv, w.stale, mHudD->sesStale, w.endBytes * inv, w.ends * inv, w.overflow, w.hashHit * inv, w.hashMiss * inv,
        w.hashInval, w.hashPartial, w.loadHit * inv, w.loadMiss * inv, w.loadStock * inv, w.vfyDebtOk, w.vfyDebtBad,
        w.vfyHashOk, w.vfyHashBad, w.vfyLoadOk, w.vfyLoadBad, w.vfyRectOk, w.vfyRectBad,
        w.vfyRectSkip, mHudD->sesVfyBad);
    int m = n;
    if (mHudClock != 0 && m > 0 && static_cast<size_t>(m) < cap) {
        const double toMs = 1.0 / 268111.856 / static_cast<double>(tasks); /* SYSCLOCK_ARM11 ticks per ms */
        m += snprintf(buf + m, cap - static_cast<size_t>(m), " | clock runms=%.3f rectms=%.3f loadms=%.3f stms=%.3f",
                      static_cast<double>(w.ticks[0]) * toMs, static_cast<double>(w.ticks[1]) * toMs,
                      static_cast<double>(w.ticks[2]) * toMs, static_cast<double>(w.ticks[3]) * toMs);
        m += snprintf(buf + m, cap > static_cast<size_t>(m) ? cap - static_cast<size_t>(m) : 0, " drains");
        for (int k = 0; k < FLUSH_COUNT && static_cast<size_t>(m) < cap; k++) {
            if (w.ticks[4 + k] != 0) {
                m += snprintf(buf + m, cap - static_cast<size_t>(m), " %d=%.2f", k,
                              static_cast<double>(w.ticks[4 + k]) / static_cast<double>(tasks));
            }
        }
    }
    memset(&w, 0, sizeof(w));
    return m;
}

/* [native] deadsetup verify (gdx_dlmemo.cpp, c5dlmemo): the ledger's live entries, field by
 * field (no padding bytes), so a window can be run twice from one start state and the two
 * ledgers compared. dst == nullptr returns the capacity needed. */
namespace {
constexpr size_t kDebtWireBytes = 4u * 4u + 8u + 16u + 2u;
} // namespace

size_t Interp::DebtStateSave(void* dst, size_t cap) const {
    const size_t need = sizeof(uint32_t) + kDebtWireBytes * kDebtMax;
    if (dst == nullptr) {
        return need;
    }
    uint8_t* o = static_cast<uint8_t*>(dst);
    const uint32_t n = (mHudD != nullptr) ? mHudD->led.n : 0u;
    if (cap < need) {
        return 0;
    }
    memcpy(o, &n, 4);
    size_t at = 4;
    for (uint32_t i = 0; i < n; i++) {
        const Debt& d = mHudD->led.e[i];
        const uint64_t src = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.src0));
        memcpy(o + at, &d.off, 4);
        memcpy(o + at + 4, &d.end, 4);
        memcpy(o + at + 8, &d.off0, 4);
        memcpy(o + at + 12, &d.n0, 4);
        memcpy(o + at + 16, &src, 8);
        memcpy(o + at + 24, d.stamp, 16);
        o[at + 40] = d.native;
        o[at + 41] = d.cls;
        at += kDebtWireBytes;
    }
    return at;
}

void Interp::DebtStateLoad(const void* src, size_t bytes) {
    if (mHudD == nullptr || bytes < 4) {
        return;
    }
    const uint8_t* in = static_cast<const uint8_t*>(src);
    uint32_t n = 0;
    memcpy(&n, in, 4);
    if (n > kDebtMax || bytes < 4 + n * kDebtWireBytes) {
        return;
    }
    size_t at = 4;
    for (uint32_t i = 0; i < n; i++) {
        Debt& d = mHudD->led.e[i];
        uint64_t p = 0;
        memcpy(&d.off, in + at, 4);
        memcpy(&d.end, in + at + 4, 4);
        memcpy(&d.off0, in + at + 8, 4);
        memcpy(&d.n0, in + at + 12, 4);
        memcpy(&p, in + at + 16, 8);
        d.src0 = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(p));
        memcpy(d.stamp, in + at + 24, 16);
        d.native = in[at + 40];
        d.cls = in[at + 41];
        at += kDebtWireBytes;
    }
    mHudD->led.n = n;
    mHudDebtN = n;
}

} // namespace gdxn
