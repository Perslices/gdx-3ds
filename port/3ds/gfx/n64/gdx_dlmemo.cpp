/* port/3ds/gfx/n64/gdx_dlmemo.cpp -- [native] dlmemo and [native] deadsetup (C5, agent
 * c5dlmemo; docs/research/native/c5dlmemo-progress.md). Both act on the owned walker's G_DL.
 *
 * DLMEMO. A G_DL resolve (DlSrc::ResolveList -> the bridge's NativeResolveList) costs ~37 us on
 * the Old 3DS: the segment/placeholder resolve, the readable-limit probe and the list-facts scan.
 * The bridge memoises all three, but per task (its keys fold gSegments[1], the double-buffered
 * GfxPool, or live on the per-task adapter), so every setup list pays it on every frame. This
 * memo is cross-task. Its answer (base, limit, stride, isBig, isF3d) is keyed on
 *   - the operand words (w1, w1hi): the raw token, and the GDX1 host decision, which is a pure
 *     function of them plus gHostRanges (the resolve-tables version);
 *   - a GENERATION = every mutable input of the resolve that is not the list itself: all 16
 *     slots of the running thread's segment view, the resolve-tables version, the segment-reload
 *     epoch (must be even) and the mode segment-9 state (gdx_gfx_job_list_memo_key). The bridge's
 *     own gResolveMemo accepts exactly this set for TryResolveAddressUncached; here nothing is
 *     hashed -- a generation is matched by comparing every word;
 *   - the list's own OPCODE BYTES up to its terminator, re-read and compared on every hit: the
 *     list-facts derivation (validation, the F3D terminator, the scan bound) reads nothing else
 *     of the list (FactsScan reads the opcode byte of each command and stops at the terminator);
 *   - the task's root range, excluded per entry (KnownCommandLimit's only per-task term).
 * An answer is filled only when the bridge says it has no other input (no alignment fallback,
 * not the raw big-endian class) and the generation did not move while it was computed (a
 * first-claim segment write or an asset-image publish inside the resolve). Failures are never
 * memoised. The one input not keyed is the OS memory map behind the readable-limit clamp; see
 * the progress doc §1.
 *
 * THREADS. The memo is a member of the one Interp, which is only ever driven by the thread
 * that walks (the render thread under renderthread, else main): single owner, no sharing, no
 * tearing (the factsrace class cannot occur). Every key input it reads is either job-private
 * (the segment view) or an atomic the bridge already publishes for cross-thread readers.
 *
 * DEADSETUP. Racer_Draw emits every effect class's setup list every frame, draws or not; with
 * no instance the lists run back to back and most of what they set is overwritten by the next
 * list before anything reads it. At a G_DL the planner looks ahead in the parent for a WINDOW:
 * consecutive G_DLs (separated only by syncs and the c5cat/c5sub clock markers) whose targets
 * are pure state lists (geometry/other modes, combiner, prim/env, G_TEXTURE, SETTIMG, SETTILE,
 * SETTILESIZE, LOADBLOCK, syncs). A backward liveness pass over the flattened commands, with
 * everything live at the window's end, marks each command dead when nothing it writes can be
 * read before it is rewritten. Dead commands are not executed; a list with no live command is
 * not entered (its G_DL hook still runs). The game functions and the emitted display list are
 * untouched: this skips interpreter work, not game code. Model and proof: progress doc §2. */
#include "gdx_interp.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "gdx_dlsrc_bridge.h"
#include "gdx_log.h"
#include "gdx3ds_hotcold.h" // hotcold: GDX_COLD / GDX_UNLIKELY (per-function attributes only)

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

constexpr unsigned kDlMemoSets = 64; /* power of two */
constexpr unsigned kDlMemoWays = 4;  /* 256 entries: two views x ~35 lists per 1P race task, and 4P */
constexpr unsigned kDlMemoMaxOps = 64;
constexpr unsigned kDlMemoWords = 80; /* see Entry::words */
constexpr unsigned kDlMemoGens = 4; /* the GfxPool alternates two views; room for a transition */

bool KeyEq(const GdxDlMemoKey& a, const GdxDlMemoKey& b) {
    return a.tablesVersion == b.tablesVersion && a.epoch == b.epoch && a.seg9State == b.seg9State &&
           memcmp(a.seg, b.seg, sizeof(a.seg)) == 0;
}

/* FactsScan's opcode byte: +0 in a big-endian narrow list, +3 (host-endian w0) otherwise. */
inline uint8_t OpByteAt(const uint8_t* base, size_t i, uint8_t stride, bool isBig) {
    return base[i * stride + ((stride == 8 && isBig) ? 0u : 3u)];
}

inline bool IsTerminator(uint8_t op) {
    return op == OP_ENDDL || op == 0xB8u; /* F3DEX2 / F3D G_ENDDL, as FactsScan */
}

bool InRoot(const uint8_t* base, const GdxDlMemoKey& k) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(base);
    return k.rootBegin != 0 && p >= k.rootBegin && p < k.rootEnd;
}

bool SameRef(const ListRef& a, const ListRef& b) {
    return a.base == b.base && a.limit == b.limit && a.stride == b.stride && a.isBig == b.isBig && a.isF3d == b.isF3d;
}

} // namespace

struct Interp::DlMemo {
    struct Gen {
        GdxDlMemoKey key;
        uint32_t serial; /* 0 = empty */
        uint32_t lastUse;
    };
    struct Entry {
        uint32_t w1, w1hi;
        uint32_t serial; /* the generation it was filled under */
        const uint8_t* base;
        size_t limit;
        uint32_t lastUse;
        uint32_t stamp; /* bumped at every fill: a plan built on this entry names it (deadsetup) */
        uint8_t stride, isBig, isF3d;
        uint8_t nFull; /* commands through the terminator, held whole (w0, w1) */
        uint8_t nW0;   /* commands whose w0 is held (>= nFull: the raw class's sniff extent) */
        /* words[0 .. 2*nFull): (w0, w1) of each command through the terminator -- a superset of
         * the opcode bytes the facts derivation reads, and the whole list deadsetup plans on;
         * then w0 of commands [nFull, nW0) -- the rest of the big-endian sniff's read set. */
        uint32_t words[kDlMemoWords];
    };
    struct Win {
        uint32_t lookups, hits, miss, fills, noAdm, noTerm, odd, raced, gens, root, ops, vfyOk, vfyBad;
    };
    Gen gen[kDlMemoGens];
    Entry e[kDlMemoSets][kDlMemoWays];
    Entry scratch; /* a capture before it is known to fit */
    uint32_t serial;
    uint32_t clock;
    int cur;
    Win w;
    uint32_t sesVfyBad, sesHits, noAdmLogs;
    uint32_t stamps;

    /* The generation matching `k`, or -1. */
    int Find(const GdxDlMemoKey& k) {
        if (cur >= 0 && gen[cur].serial != 0 && KeyEq(gen[cur].key, k)) {
            gen[cur].lastUse = ++clock;
            return cur;
        }
        for (unsigned i = 0; i < kDlMemoGens; i++) {
            if (gen[i].serial != 0 && KeyEq(gen[i].key, k)) {
                cur = static_cast<int>(i);
                gen[i].lastUse = ++clock;
                return cur;
            }
        }
        return -1;
    }
    /* A new generation in the empty or least recently used slot; its serial retires every entry
     * the slot's previous generation filled. */
    int Add(const GdxDlMemoKey& k) {
        unsigned victim = 0;
        for (unsigned i = 0; i < kDlMemoGens; i++) {
            if (gen[i].serial == 0) {
                victim = i;
                break;
            }
            if (gen[i].lastUse < gen[victim].lastUse) {
                victim = i;
            }
        }
        if (++serial == 0) {
            serial = 1;
        }
        gen[victim].key = k;
        gen[victim].serial = serial;
        gen[victim].lastUse = ++clock;
        cur = static_cast<int>(victim);
        w.gens++;
        return cur;
    }
    Entry* Set(const Cmd& c, uint32_t genSerial) {
        const uint32_t h = (c.w1 * 2654435761u) ^ (c.w1hi * 40503u) ^ (genSerial * 0x9E3779B9u);
        return e[(h >> 20) & (kDlMemoSets - 1u)];
    }
    /* The way holding (operand, generation), or nullptr. */
    Entry* Probe(const Cmd& c, uint32_t genSerial) {
        Entry* s = Set(c, genSerial);
        for (unsigned k = 0; k < kDlMemoWays; k++) {
            if (s[k].serial == genSerial && s[k].w1 == c.w1 && s[k].w1hi == c.w1hi) {
                s[k].lastUse = ++clock;
                return &s[k];
            }
        }
        return nullptr;
    }
    /* The way to fill: an empty or stale one, else the least recently used. */
    Entry& Victim(const Cmd& c, uint32_t genSerial) {
        Entry* s = Set(c, genSerial);
        unsigned lru = 0;
        for (unsigned k = 0; k < kDlMemoWays; k++) {
            if (s[k].serial == 0 || !Live(s[k].serial)) {
                return s[k];
            }
            if (s[k].lastUse < s[lru].lastUse) {
                lru = k;
            }
        }
        return s[lru];
    }
    bool Live(uint32_t serial) const {
        for (unsigned i = 0; i < kDlMemoGens; i++) {
            if (gen[i].serial == serial) {
                return true;
            }
        }
        return false;
    }
    /* The two raw words of command i (w0 at +0 and the low w1 at +4 in a narrow packet, +8 in
     * a wide one), as stored -- no byte swap: the compare is on memory. */
    static uint32_t WordAt(const uint8_t* base, size_t i, uint8_t stride, unsigned which) {
        uint32_t w;
        memcpy(&w, base + i * stride + (which == 0 ? 0u : (stride == 16 ? 8u : 4u)), sizeof(w));
        return w;
    }
    /* Everything the resolve read is still there: the w0 of every command it read (a superset
     * of the opcode bytes and of the sniff's bytes). withW1: also every w1 through the
     * terminator, i.e. the whole list is the list the entry holds (deadsetup's plans read it). */
    static bool WordsStill(const Entry& en, bool withW1) {
        for (unsigned i = 0; i < en.nFull; i++) {
            if (WordAt(en.base, i, en.stride, 0) != en.words[2 * i]) {
                return false;
            }
        }
        if (withW1) {
            for (unsigned i = 0; i < en.nFull; i++) {
                if (WordAt(en.base, i, en.stride, 1) != en.words[2 * i + 1]) {
                    return false;
                }
            }
        }
        const uint32_t* tail = en.words + 2 * en.nFull;
        for (unsigned i = en.nFull; i < en.nW0; i++) {
            if (WordAt(en.base, i, en.stride, 0) != tail[i - en.nFull]) {
                return false;
            }
        }
        return true;
    }
    /* Capture through the first terminator (whole commands) and, for the raw class, the w0 of
     * the sniff's commands past it. false when the terminator is not inside the cap or the read
     * set does not fit (such a list is not memoised). */
    static bool Capture(Entry& en, const ListRef& r, bool rawClass, uint32_t sniffCmds) {
        const size_t n = r.limit < kDlMemoMaxOps ? r.limit : kDlMemoMaxOps;
        size_t full = 0;
        for (size_t i = 0; i < n; i++) {
            if (IsTerminator(OpByteAt(r.base, i, r.stride, r.isBig))) {
                full = i + 1;
                break;
            }
        }
        if (full == 0) {
            return false;
        }
        size_t w0n = full;
        if (rawClass) {
            if (r.limit < kDlMemoMaxOps || sniffCmds > kDlMemoMaxOps) {
                return false;
            }
            w0n = sniffCmds > full ? sniffCmds : full;
        }
        if (2 * full + (w0n - full) > kDlMemoWords) {
            return false;
        }
        for (size_t i = 0; i < full; i++) {
            en.words[2 * i] = WordAt(r.base, i, r.stride, 0);
            en.words[2 * i + 1] = WordAt(r.base, i, r.stride, 1);
        }
        for (size_t i = full; i < w0n; i++) {
            en.words[2 * full + (i - full)] = WordAt(r.base, i, r.stride, 0);
        }
        en.nFull = static_cast<uint8_t>(full);
        en.nW0 = static_cast<uint8_t>(w0n);
        return true;
    }
};

GDX_COLD void Interp::SetDlMemo(int level) {
    mDlMemoLevel = (level < 0 || level > 2) ? 0 : level;
    if (mDlMemoLevel != 0 && mDlMemo == nullptr) {
        mDlMemo = new (std::nothrow) DlMemo;
        if (mDlMemo == nullptr) {
            mDlMemoLevel = 0;
        } else {
            memset(static_cast<void*>(mDlMemo), 0, sizeof(DlMemo));
            mDlMemo->cur = -1;
        }
    }
    GDXN_LOG("[dlmemo] init dlmemo=%d (%s) entries=%u gens=%u bytes=%u", mDlMemoLevel,
             mDlMemoLevel == 1 ? "memo" : (mDlMemoLevel == 2 ? "verify: always resolves" : "off"), kDlMemoSets * kDlMemoWays,
             kDlMemoGens, static_cast<unsigned>(sizeof(DlMemo)));
}

/* The G_DL resolve through the memo. hitOnly: answer only from the memo (no bridge call, so no
 * side effect of a resolve can happen early) -- deadsetup's look-ahead. */
bool Interp::ResolveListMemo(const Cmd& c, ListRef& out, bool hitOnly) {
    return ResolveListMemoKeyed(c, out, hitOnly, nullptr, nullptr);
}

/* keyIn: a key the caller sampled with nothing executed since (deadsetup's look-ahead reads one
 * key for the whole window), else nullptr to sample it here. */
bool Interp::ResolveListMemoKeyed(const Cmd& c, ListRef& out, bool hitOnly, const GdxDlMemoKey* keyIn,
                                  const void** entryOut) {
    if (entryOut != nullptr) {
        *entryOut = nullptr;
    }
    DlMemo* M = mDlMemo;
    if (M == nullptr || mDlMemoLevel == 0) {
        return hitOnly ? false : mSrc.ResolveList(c, out);
    }
    M->w.lookups++;
    GdxDlMemoKey k;
    if (keyIn != nullptr) {
        k = *keyIn;
    } else {
        mSrc.MemoKey(&k);
    }
    if ((k.epoch & 1u) != 0) {
        M->w.odd++; /* a segment reload is in flight: the resolve's own guard decides */
        return hitOnly ? false : mSrc.ResolveList(c, out);
    }
    int g = M->Find(k);
    DlMemo::Entry* hit = nullptr;
    if (g >= 0) {
        DlMemo::Entry* en = M->Probe(c, M->gen[g].serial);
        if (en != nullptr) {
            if (InRoot(en->base, k)) {
                M->w.root++;
            } else if (!DlMemo::WordsStill(*en, hitOnly)) {
                M->w.ops++;
            } else {
                hit = en;
            }
        }
    }
    if (hit != nullptr && (mDlMemoLevel == 1 || hitOnly)) {
        M->w.hits++;
        M->sesHits++;
        if (entryOut != nullptr) {
            *entryOut = hit;
        }
        out.base = hit->base;
        out.limit = hit->limit;
        out.stride = hit->stride;
        out.isBig = hit->isBig != 0;
        out.isF3d = hit->isF3d != 0;
        return true;
    }
    if (hitOnly) {
        return false;
    }
    int memoOk = 0;
    uint32_t sniffCmds = 0;
    ListRef fresh;
    const bool ok = mSrc.ResolveListMemoOk(c, fresh, &memoOk, &sniffCmds);
    if (GDX_UNLIKELY(hit != nullptr)) {
        /* hotcold: dlmemo=2 verify only (a level-1 hit returned above), out of line and cold. */
        [M, hit, ok, &fresh, &c]() GDX_COLD {
        ListRef memo;
        memo.base = hit->base;
        memo.limit = hit->limit;
        memo.stride = hit->stride;
        memo.isBig = hit->isBig != 0;
        memo.isF3d = hit->isF3d != 0;
        if (ok && SameRef(memo, fresh)) {
            M->w.vfyOk++;
        } else {
            M->w.vfyBad++;
            M->sesVfyBad++;
            if (M->sesVfyBad <= 8) {
                GDXN_LOG("[dlmemo] VERIFY MISMATCH w1=%08X w1hi=%08X memo=%p/%u/%u/%d/%d fresh=%s%p/%u/%u/%d/%d", c.w1,
                         c.w1hi, static_cast<const void*>(memo.base), static_cast<unsigned>(memo.limit), memo.stride,
                         memo.isBig ? 1 : 0, memo.isF3d ? 1 : 0, ok ? "" : "(failed) ",
                         static_cast<const void*>(fresh.base), static_cast<unsigned>(fresh.limit), fresh.stride,
                         fresh.isBig ? 1 : 0, fresh.isF3d ? 1 : 0);
            }
        }
        }();
    }
    if (!ok) {
        return false; /* failures are never memoised */
    }
    out = fresh;
    if (hit != nullptr) {
        return true;
    }
    M->w.miss++;
    if (memoOk == 0) {
        M->w.noAdm++;
        if (M->noAdmLogs < 24) {
            M->noAdmLogs++;
            GDXN_LOG("[dlmemo] not admissible w1=%08X w1hi=%08X -> %p stride=%u big=%d f3d=%d inroot=%d", c.w1, c.w1hi,
                     static_cast<const void*>(fresh.base), fresh.stride, fresh.isBig ? 1 : 0, fresh.isF3d ? 1 : 0,
                     InRoot(fresh.base, k) ? 1 : 0);
        }
        return true;
    }
    GdxDlMemoKey k2;
    mSrc.MemoKey(&k2);
    if (!KeyEq(k, k2)) {
        M->w.raced++; /* the resolve moved the generation (a first claim / publish), or a reload */
        return true;
    }
    /* Capture into the scratch entry first: a list the memo cannot hold must not evict one it
       does, nor retire a generation. */
    DlMemo::Entry& cap = M->scratch;
    if (!DlMemo::Capture(cap, fresh, memoOk == 2, sniffCmds)) {
        M->w.noTerm++;
        if (M->noAdmLogs < 48) {
            M->noAdmLogs++;
            GDXN_LOG("[dlmemo] not captured w1=%08X -> %p stride=%u limit=%u class=%d sniff=%u", c.w1,
                     static_cast<const void*>(fresh.base), fresh.stride, static_cast<unsigned>(fresh.limit), memoOk,
                     sniffCmds);
        }
        return true;
    }
    if (g < 0) {
        g = M->Add(k);
    }
    DlMemo::Entry* old = M->Probe(c, M->gen[g].serial); /* a stale copy of this operand (root/words) */
    DlMemo::Entry& en = (old != nullptr) ? *old : M->Victim(c, M->gen[g].serial);
    en.nFull = cap.nFull;
    en.nW0 = cap.nW0;
    memcpy(en.words, cap.words, sizeof(uint32_t) * (2u * cap.nFull + (cap.nW0 - cap.nFull)));
    en.w1 = c.w1;
    en.w1hi = c.w1hi;
    en.base = fresh.base;
    en.limit = fresh.limit;
    en.stride = fresh.stride;
    en.isBig = fresh.isBig ? 1 : 0;
    en.isF3d = fresh.isF3d ? 1 : 0;
    en.serial = M->gen[g].serial;
    en.lastUse = ++M->clock;
    en.stamp = ++M->stamps;
    M->w.fills++;
    return true;
}

GDX_COLD int Interp::DlMemoFormat(char* buf, size_t cap, uint32_t tasks) {
    if (mDlMemo == nullptr || tasks == 0 || cap == 0) {
        return 0;
    }
    DlMemo::Win& w = mDlMemo->w;
    const double inv = 1.0 / static_cast<double>(tasks);
    const int n = snprintf(buf, cap,
                           "[dlmemo] tasks=%u level=%d /task look=%.1f hit=%.1f miss=%.1f fill=%.2f noadm=%.2f "
                           "noterm=%.2f odd=%.2f raced=%.2f root=%.2f ops=%.2f | gens=%u | vfy ok=%u bad=%u ses_bad=%u",
                           static_cast<unsigned>(tasks), mDlMemoLevel, w.lookups * inv, w.hits * inv, w.miss * inv,
                           w.fills * inv, w.noAdm * inv, w.noTerm * inv, w.odd * inv, w.raced * inv, w.root * inv,
                           w.ops * inv, w.gens, w.vfyOk, w.vfyBad, mDlMemo->sesVfyBad);
    memset(&w, 0, sizeof(w));
    return n;
}

/* ===========================================================================================
 * [native] deadsetup
 * =========================================================================================== */

namespace {

constexpr unsigned kDsMaxLists = 8;
constexpr unsigned kDsMaxCmds = 64;
constexpr unsigned kDsMaxParent = 32; /* parent commands a cached window spans */
constexpr unsigned kDsPlans = 32;     /* cached windows (per window: one per GfxPool half; 4P has 4) */
constexpr uint32_t kDsDeclineTasks = 240; /* a declined window is looked at again after this */

/* State atoms of the liveness model. Geometry mode and the other modes are per bit (a command
 * writes exactly the bits of its masks); the rest are whole registers. */
enum : uint32_t {
    A_COMB = 1u << 0,      /* combine_mode */
    A_PRIM = 1u << 1,      /* prim_color + prim_lod_fraction */
    A_ENV = 1u << 2,       /* env_color */
    A_TEXSCALE = 1u << 3,  /* texture_scaling_factor */
    A_FIRSTTILE = 1u << 4, /* first_tile_index */
    A_TIMG = 1u << 5,      /* texture_to_load: fmt, siz, width, cmd */
    A_TIMGRES = 1u << 6,   /* texture_to_load: addr, immutable, native_rgba16 (the load's resolution) */
    A_TEXCHG = 1u << 7,    /* textures_changed[0..1] (always armed together) */
    A_TILESET0 = 1u << 8,  /* texture_tile[t] SETTILE fields, t = 0..7 -> bits 8..15 */
    A_TILESIZE0 = 1u << 16 /* texture_tile[t] SETTILESIZE fields -> bits 16..23 */
};
constexpr uint32_t kAllTileSet = 0xFFu << 8;

struct Live {
    uint32_t gm;
    uint64_t om;
    uint32_t misc;
};

/* == gdx_tmem.cpp BytesForTexels */
uint32_t DsBytesForTexels(uint64_t texels, uint32_t siz) {
    uint64_t bytes = 0;
    switch (siz) {
        case 0: bytes = (texels + 1u) / 2u; break;
        case 1: bytes = texels; break;
        case 2: bytes = texels * 2u; break;
        case 3: bytes = texels * 4u; break;
        default: break;
    }
    return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes);
}

/* The bits SpGeometryMode / SpSetOtherMode write for this command, exactly as Exec decodes
 * it: the mask bits AND the mode bits outside it (Exec ORs the whole word in). false = a shape
 * the model refuses (a shift Exec would take out of range). */
uint32_t GeometryBits(const Cmd& c) {
    const uint32_t clear = ~C0(c.w0, 0, 24);
    return clear | c.w1;
}

bool OtherModeBits(const Cmd& c, uint64_t* bits) {
    const uint32_t a = C0(c.w0, 8, 8), b = C0(c.w0, 0, 8);
    const bool high = Opcode(c.w0) == OP_SETOTHERMODE_H;
    const uint32_t top = high ? 63u : 31u;
    if (a + b > top) {
        return false;
    }
    const uint32_t shift = top - a - b;
    const uint32_t numBits = b + 1u;
    if (numBits >= 64u) {
        return false;
    }
    const uint64_t mask = ((static_cast<uint64_t>(1) << numBits) - 1u) << shift;
    const uint64_t mode = high ? (static_cast<uint64_t>(c.w1) << 32) : static_cast<uint64_t>(c.w1);
    *bits = mask | mode;
    return true;
}

bool ModelledOp(uint8_t op) {
    switch (op) {
        case OP_RDPLOADSYNC:
        case OP_RDPPIPESYNC:
        case OP_RDPTILESYNC:
        case OP_RDPFULLSYNC:
        case OP_GEOMETRYMODE:
        case OP_SETOTHERMODE_L:
        case OP_SETOTHERMODE_H:
        case OP_RDPSETOTHERMODE:
        case OP_SETCOMBINE:
        case OP_SETPRIMCOLOR:
        case OP_SETENVCOLOR:
        case OP_TEXTURE:
        case OP_SETTIMG:
        case OP_SETTILE:
        case OP_SETTILESIZE:
        case OP_LOADBLOCK:
            return true;
        default:
            return false;
    }
}

bool NeutralOp(uint8_t op) {
    return op == OP_RDPLOADSYNC || op == OP_RDPPIPESYNC || op == OP_RDPTILESYNC || op == OP_RDPFULLSYNC;
}

/* A parent command the window may step over: syncs, and the [c5cat] / [c5sub] clock markers
 * (their hook reads no interpreter state; the walker still executes them in place). */
bool InertParentOp(const Cmd& c) {
    const uint8_t op = Opcode(c.w0);
    if (NeutralOp(op)) {
        return true;
    }
    if (op == OP_NOOP) {
        const uint32_t p = (c.w0 >> 16) & 0xFFu;
        return p == 0x61u || p == 0x62u;
    }
    return false;
}

} // namespace

struct Interp::DeadSet {
    struct PlanList {
        ListRef ref;
        size_t parentIdx;
        uint8_t n;
        uint64_t effect, live, covers;
        const void* entry; /* the memo entry the list resolved from (it holds the list's words) */
        uint32_t stamp;    /* that entry's fill stamp */
    };
    /* A built window, reusable wherever the same parent words appear with the same pending flag
     * and every list resolves from the same memo entry with the same fill (the same words) --
     * the liveness passes read nothing else, so the position in the parent does not matter (the
     * four 4P effect sections share one plan). Found by the first G_DL's operand. */
    struct Plan {
        uint32_t keyW1, keyW1hi; /* the first G_DL's operand */
        uint8_t nParent; /* parent commands [start, start + nParent) through the last G_DL */
        bool pending;
        bool valid;
        bool declined;     /* no window worth running here: the stock walk, re-examined later */
        uint32_t madeAt;   /* taskSeq at which it was built or declined */
        uint32_t meshGen;  /* the machine registry's generation at build (DeadSetMachineList) */
        uint8_t nLists;
        uint32_t lastUse;
        Cmd parent[kDsMaxParent];
        PlanList l[kDsMaxLists];
    };
    struct List {
        ListRef ref;
        size_t parentIdx;
        uint8_t n;       /* commands before ENDDL */
        uint64_t effect; /* commands that are not syncs */
        uint64_t live;
        uint64_t covers; /* live LOADBLOCKs that stand in for a dead one (their copy is checked) */
        Cmd cmd[kDsMaxCmds];
    };
    struct Win {
        uint32_t tries, plans, reused, declined, rNone, rParent, rList, lists, skipped, partial, full, cmds, dead, deadLoads, trips, shortLoads, vfyOk,
            vfyBad, vfyGroup[8];
        uint64_t ticks;
    };
    bool active;
    bool consumed; /* verify: the window already ran (stock, inline); its G_DLs are no-ops */
    Frame* frame;
    int depth;
    uint8_t nLists, cur;
    List l[kDsMaxLists];
    /* per flattened command: the load's TMEM range (base word, bytes), 0xFFFF = unknown */
    uint16_t loadBase[kDsMaxLists * kDsMaxCmds];
    uint32_t loadBytes[kDsMaxLists * kDsMaxCmds];
    uint8_t coverFlat[kDsMaxLists * kDsMaxCmds]; /* 1 = a live load that stands in for a dead one */
    Win w;
    Plan plans[kDsPlans];
    uint32_t planClock;
    uint32_t taskSeq;
    uint32_t deadLoadsBuilt; /* the dead LOADBLOCKs of the window being built */
    const void* entry[kDsMaxLists]; /* per list of the window being built */
    uint32_t sesVfyBad, sesTrips, sesPlans, sesSkipped, sesDead, sesVfyOk, sesReused;
    uint32_t planLogs, breakLogs;
    uint8_t stopOp;
    /* verify scratch */
    Rdp* s0;
    Rdp* s1;
    uint8_t* led0;
    uint8_t* led1;
    size_t ledCap;
};

GDX_COLD void Interp::SetDeadSetup(int level) {
    mDeadSetLevel = (level < 0 || level > 2) ? 0 : level;
    if (mDeadSetLevel != 0 && mDlMemoLevel == 0) {
        mDeadSetLevel = 0; /* the look-ahead resolves only from the memo */
    }
    if (mDeadSetLevel != 0 && mDs == nullptr) {
        mDs = new (std::nothrow) DeadSet;
        if (mDs == nullptr) {
            mDeadSetLevel = 0;
        } else {
            memset(static_cast<void*>(mDs), 0, sizeof(DeadSet));
        }
    }
    if (mDeadSetLevel == 2 && mDs != nullptr && mDs->s0 == nullptr) {
        mDs->s0 = new (std::nothrow) Rdp;
        mDs->s1 = new (std::nothrow) Rdp;
        mDs->ledCap = DebtStateSave(nullptr, 0);
        mDs->led0 = new (std::nothrow) uint8_t[mDs->ledCap + 1];
        mDs->led1 = new (std::nothrow) uint8_t[mDs->ledCap + 1];
        if (mDs->s0 == nullptr || mDs->s1 == nullptr || mDs->led0 == nullptr || mDs->led1 == nullptr) {
            mDeadSetLevel = 0;
        }
    }
    GDXN_LOG("[deadset] init deadsetup=%d (%s)%s", mDeadSetLevel,
             mDeadSetLevel == 1 ? "skip" : (mDeadSetLevel == 2 ? "verify: runs stock, compares" : "off"),
             (level != 0 && mDlMemoLevel == 0) ? " (inert: needs [native] dlmemo)" : "");
}

void Interp::DeadSetFree() {
    delete mDlMemo;
    mDlMemo = nullptr;
    if (mDs != nullptr) {
        delete mDs->s0;
        delete mDs->s1;
        delete[] mDs->led0;
        delete[] mDs->led1;
        delete mDs;
        mDs = nullptr;
    }
}

bool Interp::DeadSetEligible(const Cmd& c, const Frame& f) const {
    return mSp > 0 && &f == &mStack[mSp - 1] && !f.list.isF3d && C0(c.w0, 16, 1) == 0 && mHud == nullptr && mTexOv == nullptr && mTexRecArm == nullptr && mTmemSkip == 0 && !mVerifyActive &&
           mDspCensus == 0 && mMsOn == 0;
}

namespace {

/* The list's commands, when every one is modelled: no F3D dialect, a terminator inside the cap. */
template <typename L>
bool DsParse(L& d) {
    if (d.ref.isF3d) {
        return false;
    }
    d.effect = 0;
    for (size_t i = 0; i < d.ref.limit && i < kDsMaxCmds; i++) {
        const Cmd c = ReadCmd(d.ref, i);
        const uint8_t op = Opcode(c.w0);
        if (op == OP_ENDDL) {
            d.n = static_cast<uint8_t>(i);
            return true;
        }
        if (!ModelledOp(op)) {
            return false;
        }
        if (op == OP_SETOTHERMODE_L || op == OP_SETOTHERMODE_H) {
            uint64_t bits = 0;
            if (!OtherModeBits(c, &bits)) {
                return false;
            }
        }
        d.cmd[i] = c;
        if (!NeutralOp(op)) {
            d.effect |= static_cast<uint64_t>(1) << i;
        }
    }
    return false;
}

} // namespace

/* The first parent command after this G_DL that is not an inert one is another pushed G_DL. */
bool Interp::DeadSetNextIsDl(const Frame& f) const {
    for (size_t j = f.idx; j < f.list.limit; j++) {
        const Cmd pc = ReadCmd(f.list, j);
        const uint8_t op = Opcode(pc.w0);
        if (op == OP_DL) {
            return C0(pc.w0, 16, 1) == 0;
        }
        if (!InertParentOp(pc)) {
            return false;
        }
    }
    return false;
}

/* Build the window starting at this G_DL. Only memo hits are used for the look-ahead: a
 * resolve that misses might have a side effect (a first claim), which must not run early. */
bool Interp::DeadSetBuild(const Cmd& c, Frame& f, const GdxDlMemoKey& key) {
    DeadSet& D = *mDs;
    const size_t start = f.idx - 1;
    D.w.tries++;
    uint8_t n = 0;
    bool memoMiss = false;
    for (size_t j = start; j < f.list.limit && n < kDsMaxLists; j++) {
        const Cmd pc = (j == start) ? c : ReadCmd(f.list, j);
        const uint8_t op = Opcode(pc.w0);
        if (op == OP_DL && C0(pc.w0, 16, 1) == 0) {
            DeadSet::List& L = D.l[n];
            if (!ResolveListMemoKeyed(pc, L.ref, true, &key, &D.entry[n])) {
                memoMiss = true; /* not warm yet: try again next time, do not decline */
                break;
            }
            if (DeadSetMachineList(L.ref.base)) {
                break;
            }
            if (!DsParse(L)) {
                break;
            }
            L.parentIdx = j;
            n++;
            continue;
        }
        if (!InertParentOp(pc)) {
            break;
        }
    }
    D.stopOp = 0xFF;
    if (n < kDsMaxLists) {
        size_t j = start;
        for (uint8_t k = 0; k < n; k++) {
            j = D.l[k].parentIdx + 1;
        }
        D.stopOp = (j < f.list.limit) ? Opcode(ReadCmd(f.list, j).w0) : 0xFE;
    }
    if (n < 2) {
        if (D.breakLogs < 24) {
            D.breakLogs++;
            GDXN_LOG("[deadset] no window at %p+%u: %u list(s), stopped at parent op %02X", static_cast<const void*>(f.list.base),
                     static_cast<unsigned>(start), static_cast<unsigned>(n), D.stopOp);
        }
        if (!memoMiss) {
            DeadSetDecline(f, start, mBufNumTris != 0);
        }
        return false;
    }
    D.nLists = n;
    D.deadLoadsBuilt = 0;

    /* Forward: each LOADBLOCK's TMEM range, when the window itself set its load tile's TMEM
     * address and the SETTIMG size (DpLoadBlock: base = texture_tile[t].tmem_index, bytes =
     * BytesForTexels(lrs + 1, texture_to_load.siz)). */
    int timgSiz = -1;
    int tileTmem[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    unsigned flat = 0;
    for (uint8_t k = 0; k < n; k++) {
        DeadSet::List& L = D.l[k];
        for (uint8_t i = 0; i < L.n; i++, flat++) {
            const Cmd& x = L.cmd[i];
            D.loadBase[flat] = 0xFFFFu;
            D.loadBytes[flat] = 0;
            D.coverFlat[flat] = 0;
            switch (Opcode(x.w0)) {
                case OP_SETTIMG:
                    timgSiz = static_cast<int>(C0(x.w0, 19, 2));
                    break;
                case OP_SETTILE:
                    tileTmem[C1(x.w1, 24, 3)] = static_cast<int>(C0(x.w0, 0, 9));
                    break;
                case OP_LOADBLOCK: {
                    const uint32_t t = C1(x.w1, 24, 3);
                    if (timgSiz >= 0 && tileTmem[t] >= 0) {
                        D.loadBase[flat] = static_cast<uint16_t>(tileTmem[t]);
                        D.loadBytes[flat] = DsBytesForTexels(static_cast<uint64_t>(C1(x.w1, 12, 12)) + 1u,
                                                             static_cast<uint32_t>(timgSiz));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    /* With triangles pending, a prim/env VALUE change splits (or folds into) the open batch
       (NoteRegChange): every prim/env command then stays, so the running values -- and with
       them every split and fold decision -- are the stock ones. No other modelled command
       touches the batch. */
    const bool pending = mBufNumTris != 0;
    /* Backward liveness, everything live after the window. */
    Live live = { 0xFFFFFFFFu, ~static_cast<uint64_t>(0), 0xFFFFFFFFu };
    int nextLoad = -1;  /* flattened index of the nearest later LOADBLOCK */
    int chainLive = -1; /* the live load that ends nextLoad's chain of identical-range loads */
    for (uint8_t k = 0; k < n; k++) {
        D.l[k].covers = 0;
    }
    for (int k = n - 1; k >= 0; k--) {
        DeadSet::List& L = D.l[k];
        L.live = 0;
        for (int i = L.n - 1; i >= 0; i--) {
            flat--;
            const Cmd& x = L.cmd[i];
            const uint8_t op = Opcode(x.w0);
            bool isLive = false;
            switch (op) {
                case OP_GEOMETRYMODE: {
                    const uint32_t bits = GeometryBits(x);
                    isLive = (bits & live.gm) != 0;
                    if (isLive) {
                        live.gm &= ~bits;
                    }
                    break;
                }
                case OP_SETOTHERMODE_L:
                case OP_SETOTHERMODE_H: {
                    uint64_t bits = 0;
                    (void)OtherModeBits(x, &bits);
                    isLive = (bits & live.om) != 0;
                    if (isLive) {
                        live.om &= ~bits;
                    }
                    break;
                }
                case OP_RDPSETOTHERMODE:
                    isLive = live.om != 0;
                    live.om = 0;
                    break;
                case OP_SETCOMBINE:
                case OP_SETPRIMCOLOR:
                case OP_SETENVCOLOR: {
                    const uint32_t a = op == OP_SETCOMBINE ? A_COMB : (op == OP_SETPRIMCOLOR ? A_PRIM : A_ENV);
                    isLive = (live.misc & a) != 0 || (pending && op != OP_SETCOMBINE);
                    live.misc &= ~a;
                    break;
                }
                case OP_TEXTURE: {
                    /* writes the scale and first_tile_index; arms textures_changed only when the
                       tile index moves, which reads the old index */
                    const bool chg = (live.misc & A_TEXCHG) != 0;
                    isLive = (live.misc & (A_TEXSCALE | A_FIRSTTILE)) != 0 || chg;
                    if (isLive) {
                        live.misc &= ~(A_TEXSCALE | A_FIRSTTILE);
                        if (chg) {
                            live.misc |= A_FIRSTTILE;
                        }
                    }
                    break;
                }
                case OP_SETTIMG:
                    isLive = (live.misc & (A_TIMG | A_TIMGRES)) != 0;
                    live.misc &= ~(A_TIMG | A_TIMGRES);
                    break;
                case OP_SETTILE:
                case OP_SETTILESIZE: {
                    /* value-gated: arms textures_changed only on a change, which reads the old
                       fields */
                    const uint32_t t = C1(x.w1, 24, 3);
                    const uint32_t a = (op == OP_SETTILE ? A_TILESET0 : A_TILESIZE0) << t;
                    const bool chg = (live.misc & A_TEXCHG) != 0;
                    isLive = (live.misc & a) != 0 || chg;
                    if (isLive) {
                        live.misc &= ~a;
                        if (chg) {
                            live.misc |= a;
                        }
                    }
                    break;
                }
                case OP_LOADBLOCK: {
                    /* reads SETTIMG's fields and every tile's SETTILE fields (the line-size search
                       and the load tile's TMEM address); writes the resolution, TMEM + the span
                       store over its range, and arms textures_changed (always: tmemskip is 0).
                       Dead only when the next load in the window has the identical range, so it
                       retires exactly the spans this one would have and rewrites every byte. */
                    const bool rangeKnown = D.loadBase[flat] != 0xFFFFu;
                    const bool covered = rangeKnown && nextLoad >= 0 && D.loadBase[nextLoad] == D.loadBase[flat] &&
                                         D.loadBytes[nextLoad] == D.loadBytes[flat] && D.loadBytes[flat] != 0;
                    isLive = (live.misc & (A_TIMGRES | A_TEXCHG)) != 0 || !covered;
                    if (isLive) {
                        live.misc &= ~(A_TIMGRES | A_TEXCHG);
                        live.misc |= A_TIMG | kAllTileSet;
                        chainLive = static_cast<int>(flat);
                    } else {
                        D.w.deadLoads++;
                        D.deadLoadsBuilt++;
                        D.coverFlat[chainLive] = 1; /* checked after it runs (DeadSetCheckCover) */
                    }
                    nextLoad = static_cast<int>(flat);
                    break;
                }
                default: /* syncs */
                    isLive = false;
                    break;
            }
            if (isLive) {
                L.live |= static_cast<uint64_t>(1) << i;
            }
        }
    }

    flat = 0;
    for (uint8_t k = 0; k < n; k++) {
        DeadSet::List& L = D.l[k];
        for (uint8_t i = 0; i < L.n; i++, flat++) {
            if (D.coverFlat[flat] != 0) {
                L.covers |= static_cast<uint64_t>(1) << i;
            }
        }
    }
    /* Worth running only when it removes a whole list or a load: a few dead register writes
       inside lists that still run cost less than the window's own checks. */
    unsigned emptied = 0;
    for (uint8_t k = 0; k < n; k++) {
        if ((D.l[k].live & D.l[k].effect) == 0) {
            emptied++;
        }
    }
    if (emptied == 0 && D.deadLoadsBuilt == 0) {
        DeadSetDecline(f, start, pending);
        return false;
    }
    if (D.planLogs < 12) {
        D.planLogs++;
        char line[400];
        int at = snprintf(line, sizeof(line), "[deadset] window at %p+%u: %u lists, stop op %02X |", static_cast<const void*>(f.list.base),
                          static_cast<unsigned>(start), static_cast<unsigned>(n), D.stopOp);
        for (uint8_t k = 0; k < n && at > 0 && static_cast<size_t>(at) < sizeof(line); k++) {
            at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %p:%u live=%llx/%llx", static_cast<const void*>(D.l[k].ref.base),
                           static_cast<unsigned>(D.l[k].n), static_cast<unsigned long long>(D.l[k].live & D.l[k].effect),
                           static_cast<unsigned long long>(D.l[k].effect));
        }
        GDXN_LOG("%s", line);
    }
    DeadSetStore(f, start, pending);
    DeadSetActivate(f);
    return true;
}

void Interp::DeadSetActivate(Frame& f) {
    DeadSet& D = *mDs;
    D.active = true;
    D.consumed = false;
    D.frame = &f;
    D.depth = mSp;
    D.cur = 0;
    D.w.plans++;
    D.sesPlans++;
}

/* Keep the window just built for the next frames (least recently used slot). */
void Interp::DeadSetStore(const Frame& f, size_t start, bool pending) {
    DeadSet& D = *mDs;
    const size_t last = D.l[D.nLists - 1].parentIdx;
    if (last - start + 1 > kDsMaxParent) {
        return;
    }
    const Cmd first = ReadCmd(f.list, start);
    DeadSet::Plan& p = D.plans[DeadSetSlot(first, pending)];
    p.valid = true;
    p.declined = false;
    p.madeAt = D.taskSeq;
    p.meshGen = (mTexHooks.generation != nullptr) ? mTexHooks.generation() : 0u;
    p.keyW1 = first.w1;
    p.keyW1hi = first.w1hi;
    p.pending = pending;
    p.nParent = static_cast<uint8_t>(last - start + 1);
    for (size_t j = 0; j < p.nParent; j++) {
        p.parent[j] = ReadCmd(f.list, start + j);
    }
    p.nLists = D.nLists;
    for (uint8_t k = 0; k < D.nLists; k++) {
        const DeadSet::List& L = D.l[k];
        DeadSet::PlanList& q = p.l[k];
        q.ref = L.ref;
        q.parentIdx = L.parentIdx - start; /* relative to the window's first G_DL */
        q.n = L.n;
        q.effect = L.effect;
        q.live = L.live;
        q.covers = L.covers;
        q.entry = D.entry[k];
        q.stamp = static_cast<const DlMemo::Entry*>(D.entry[k])->stamp;
    }
    p.lastUse = ++D.planClock;
}

/* The cached window at this G_DL, when nothing it was derived from has changed: the parent's
 * commands over the window, the pending flag, and for every list the memo hit (which re-reads
 * every word the memo holds) on the SAME entry with the SAME fill. */
bool Interp::DeadSetReuse(Frame& f, const GdxDlMemoKey& key) {
    DeadSet& D = *mDs;
    const size_t start = f.idx - 1;
    const bool pending = mBufNumTris != 0;
    const int pi = DeadSetFind(ReadCmd(f.list, start), pending);
    DeadSet::Plan* p = (pi >= 0) ? &D.plans[pi] : nullptr;
    if (p == nullptr || p->declined || start + p->nParent > f.list.limit) {
        D.w.rNone++;
        return false;
    }
    if ((mTexHooks.generation != nullptr ? mTexHooks.generation() : 0u) != p->meshGen) {
        return false; /* the machine registry moved: re-check every list at the next build */
    }
    for (size_t j = 0; j < p->nParent; j++) {
        const Cmd pc = ReadCmd(f.list, start + j);
        if (pc.w0 != p->parent[j].w0 || pc.w1 != p->parent[j].w1 || pc.w1hi != p->parent[j].w1hi) {
            D.w.rParent++;
            return false;
        }
    }
    for (uint8_t k = 0; k < p->nLists; k++) {
        const DeadSet::PlanList& q = p->l[k];
        ListRef ref;
        const void* entry = nullptr;
        if (!ResolveListMemoKeyed(p->parent[q.parentIdx], ref, true, &key, &entry) || !SameRef(ref, q.ref) ||
            !DeadSetSameWords(&q, entry)) {
            D.w.rList++;
            return false;
        }
    }
    D.nLists = p->nLists;
    for (uint8_t k = 0; k < p->nLists; k++) {
        const DeadSet::PlanList& q = p->l[k];
        DeadSet::List& L = D.l[k];
        L.ref = q.ref;
        L.parentIdx = start + q.parentIdx;
        L.n = q.n;
        L.effect = q.effect;
        L.live = q.live;
        L.covers = q.covers;
    }
    p->lastUse = ++D.planClock;
    D.w.reused++;
    D.sesReused++;
    DeadSetActivate(f);
    return true;
}

/* No window worth running at this G_DL: remember it, so the next frames take the stock walk
 * without looking ahead. Declining is always safe (the stock walk); it is re-examined after
 * kDsDeclineTasks tasks. */
void Interp::DeadSetDecline(const Frame& f, size_t start, bool pending) {
    DeadSet& D = *mDs;
    const Cmd first = ReadCmd(f.list, start);
    DeadSet::Plan* slot = &D.plans[DeadSetSlot(first, pending)];
    slot->valid = true;
    slot->declined = true;
    slot->madeAt = D.taskSeq;
    slot->keyW1 = first.w1;
    slot->keyW1hi = first.w1hi;
    slot->pending = pending;
    slot->nParent = 0;
    slot->nLists = 0;
    slot->lastUse = ++D.planClock;
}

bool Interp::DeadSetDeclined(const Frame& f) {
    DeadSet& D = *mDs;
    const int pi = DeadSetFind(ReadCmd(f.list, f.idx - 1), mBufNumTris != 0);
    DeadSet::Plan* p = (pi >= 0) ? &D.plans[pi] : nullptr;
    if (p == nullptr || !p->declined) {
        return false;
    }
    if (D.taskSeq - p->madeAt < kDsDeclineTasks) {
        p->lastUse = ++D.planClock;
        D.w.declined++;
        return true;
    }
    p->valid = false;
    return false;
}

/* The index of the cached plan (or decline) for a window whose first G_DL is `first`, else -1. */
int Interp::DeadSetFind(const Cmd& first, bool pending) {
    DeadSet& D = *mDs;
    for (unsigned i = 0; i < kDsPlans; i++) {
        const DeadSet::Plan& p = D.plans[i];
        if (p.valid && p.keyW1 == first.w1 && p.keyW1hi == first.w1hi && p.pending == pending) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/* The slot to (re)write for that key: its own, else an empty or the least recently used one. */
int Interp::DeadSetSlot(const Cmd& first, bool pending) {
    DeadSet& D = *mDs;
    const int own = DeadSetFind(first, pending);
    if (own >= 0) {
        return own;
    }
    int slot = 0;
    for (unsigned i = 0; i < kDsPlans; i++) {
        const DeadSet::Plan& p = D.plans[i];
        if (!p.valid) {
            return static_cast<int>(i);
        }
        if (p.lastUse < D.plans[slot].lastUse) {
            slot = static_cast<int>(i);
        }
    }
    return slot;
}

/* The list a memo hit just validated (`entry`: its words are the words in memory now) holds the
 * same words the plan was built on. The same entry with the same fill is that by construction;
 * another entry for the same list (another generation of the segment view: the 4P viewports)
 * is compared word for word with the builder entry, which still holds the plan's words as long
 * as it has not been refilled. */
bool Interp::DeadSetSameWords(const void* planList, const void* entry) const {
    const DeadSet::PlanList& q = *static_cast<const DeadSet::PlanList*>(planList);
    const DlMemo::Entry* e = static_cast<const DlMemo::Entry*>(entry);
    const DlMemo::Entry* b = static_cast<const DlMemo::Entry*>(q.entry);
    if (e == b) {
        return e->stamp == q.stamp;
    }
    if (b->stamp != q.stamp || b->base != e->base || b->nFull != e->nFull) {
        return false;
    }
    return memcmp(b->words, e->words, 2u * e->nFull * sizeof(uint32_t)) == 0;
}

/* A machine list (texture set, body, speck): its G_DL hook may consume it. */
bool Interp::DeadSetMachineList(const void* p) const {
    return (mTexHooks.setSlot != nullptr && mTexHooks.setSlot(p) >= 0) ||
           (mTexHooks.isBody != nullptr && mTexHooks.isBody(p) != 0) ||
           (mTexHooks.isSpeck != nullptr && mTexHooks.isSpeck(p) != 0);
}

/* Execute the current list of the active window at its G_DL. Returns Exec's code. */
int Interp::DeadSetRunList(Frame& f) {
    DeadSet& D = *mDs;
    DeadSet::List& L = D.l[D.cur];
    D.cur++;
    if (D.cur >= D.nLists) {
        D.active = false;
    }
    D.w.lists++;
    if (mDlHook != nullptr) {
        mDlHookSub = L.ref;
        if (mDlHook(mDlHookUser, L.ref.base, this) != 0) {
            D.w.trips++; /* cannot happen for a pure state list; counted if it ever does */
            D.sesTrips++;
            return 0;
        }
    }
    const uint64_t eff = L.effect;
    const uint64_t liveEff = L.live & eff;
    D.w.cmds += static_cast<uint32_t>(__builtin_popcountll(eff));
    if (liveEff == eff && L.covers == 0) {
        D.w.full++;
        PushList(L.ref);
        return 2;
    }
    D.w.dead += static_cast<uint32_t>(__builtin_popcountll(eff & ~liveEff));
    D.sesDead += static_cast<uint32_t>(__builtin_popcountll(eff & ~liveEff));
    if (liveEff == 0) {
        D.w.skipped++;
        D.sesSkipped++;
        return 0;
    }
    if (liveEff == eff) {
        D.w.full++;
    } else {
        D.w.partial++;
    }
    Frame tmp;
    tmp.list = L.ref;
    for (uint8_t i = 0; i < L.n; i++) {
        if (((liveEff >> i) & 1u) == 0) {
            continue;
        }
        tmp.idx = static_cast<size_t>(i) + 1u;
        const Cmd x = ReadCmd(L.ref, i);
        (void)Exec(x, tmp, 0);
        if (((L.covers >> i) & 1u) != 0) {
            DeadSetCheckCover(x);
        }
    }
    (void)f;
    return 0;
}

/* A live load that stands in for a dead one of the identical range must have written that
 * whole range: its source resolved and readable for the full extent (DpLoadBlock clamps the
 * copy to the readable bytes). Checked after the fact and counted; 0 is the gate. */
void Interp::DeadSetCheckCover(const Cmd& c) {
    const uint16_t base = mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index;
    if (base >= 512u) {
        mDs->w.shortLoads++;
        return;
    }
    const LoadedTexture& lt = mRdp.loaded_texture[base];
    const uint32_t room = 4096u - static_cast<uint32_t>(base) * 8u;
    const uint32_t need = lt.size_bytes < room ? lt.size_bytes : room;
    if (lt.addr == nullptr || DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(lt.addr)) < need) {
        mDs->w.shortLoads++;
        mDs->sesTrips++;
    }
}

int Interp::DeadSetOnDl(const Cmd& c, Frame& f) {
    DeadSet& D = *mDs;
    if (D.active) {
        if (&f != D.frame || mSp != D.depth || f.idx - 1 != D.l[D.cur].parentIdx) {
            /* the walker left the window between two of its G_DLs: impossible with only inert
               commands between them; counted, and the rest of the window runs stock */
            D.w.trips++;
            D.sesTrips++;
            D.active = false;
            return -1;
        }
        if (D.consumed) {
            D.cur++;
            if (D.cur >= D.nLists) {
                D.active = false;
            }
            return 0;
        }
        return DeadSetRunList(f);
    }
    if (!DeadSetEligible(c, f)) {
        return -1;
    }
    if (!DeadSetNextIsDl(f)) {
        return -1; /* a lone G_DL: no window (the common case, decided without a resolve) */
    }
    const bool timed = mDxOn;
    const uint64_t t0 = timed ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    if (DeadSetDeclined(f)) {
        if (timed) {
            D.w.ticks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
        }
        return -1;
    }
    GdxDlMemoKey key;
    mSrc.MemoKey(&key); /* nothing executes during the look-ahead: one key serves every list */
    bool built = DeadSetReuse(f, key);
    if (!built) {
        built = DeadSetBuild(c, f, key);
    }
    if (timed) {
        D.w.ticks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
    }
    if (!built) {
        return -1;
    }
    if (mDeadSetLevel == 2) {
        DeadSetVerify(f);
        D.consumed = true;
        D.cur = 1;
        if (D.cur >= D.nLists) {
            D.active = false;
        }
        return 0;
    }
    return DeadSetRunList(f);
}

void Interp::DeadSetTaskEnd() {
    if (mDs != nullptr) {
        mDs->taskSeq++;
    }
    if (mDs != nullptr && mDs->active) {
        mDs->w.trips++; /* a window open at the task end: the walk ended inside it */
        mDs->sesTrips++;
        mDs->active = false;
    }
}

/* ---- verify: run the window both ways from one start state and compare ----------------- */

namespace {

bool TileEq(const Tile& a, const Tile& b) {
    return a.fmt == b.fmt && a.siz == b.siz && a.cms == b.cms && a.cmt == b.cmt && a.masks == b.masks &&
           a.maskt == b.maskt && a.shifts == b.shifts && a.shiftt == b.shiftt && a.uls == b.uls && a.ult == b.ult &&
           a.lrs == b.lrs && a.lrt == b.lrt && a.tmem == b.tmem && a.line_size_bytes == b.line_size_bytes &&
           a.palette == b.palette && a.tmem_index == b.tmem_index;
}

bool LtEq(const LoadedTexture& a, const LoadedTexture& b) {
    return a.addr == b.addr && a.orig_size_bytes == b.orig_size_bytes && a.size_bytes == b.size_bytes &&
           a.full_image_line_size_bytes == b.full_image_line_size_bytes && a.line_size_bytes == b.line_size_bytes &&
           a.tmem_start == b.tmem_start && a.tmem_word_count == b.tmem_word_count && a.immutable == b.immutable &&
           a.native_rgba16 == b.native_rgba16 && a.masked == b.masked && a.blended == b.blended;
}

bool RgbaEq(const Rgba& a, const Rgba& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* 0 = equal, else the first group that differs (receipt vfyGroup[]). Everything a later command
 * can read; the generation counter and the rebuildable span list are caches. */
int RdpDiff(const Rdp& a, const Rdp& b) {
    const auto& ta = a.texture_to_load;
    const auto& tb = b.texture_to_load;
    if (ta.addr != tb.addr || ta.fmt != tb.fmt || ta.siz != tb.siz || ta.width != tb.width ||
        ta.immutable != tb.immutable || ta.native_rgba16 != tb.native_rgba16 || ta.cmd.w0 != tb.cmd.w0 ||
        ta.cmd.w1 != tb.cmd.w1 || ta.cmd.w1hi != tb.cmd.w1hi) {
        return 1;
    }
    for (int t = 0; t < 8; t++) {
        if (!TileEq(a.texture_tile[t], b.texture_tile[t])) {
            return 2;
        }
    }
    if (a.textures_changed[0] != b.textures_changed[0] || a.textures_changed[1] != b.textures_changed[1] ||
        a.first_tile_index != b.first_tile_index) {
        return 3;
    }
    if (a.other_mode_l != b.other_mode_l || a.other_mode_h != b.other_mode_h || a.combine_mode != b.combine_mode ||
        a.prim_lod_fraction != b.prim_lod_fraction || a.prim_depth != b.prim_depth ||
        !RgbaEq(a.env_color, b.env_color) || !RgbaEq(a.prim_color, b.prim_color) ||
        !RgbaEq(a.fog_color, b.fog_color) || !RgbaEq(a.blend_color, b.blend_color) ||
        !RgbaEq(a.fill_color, b.fill_color) || a.grayscale != b.grayscale) {
        return 4;
    }
    if (memcmp(a.tmem, b.tmem, sizeof(a.tmem)) != 0) {
        return 5;
    }
    if (memcmp(a.tmem_span_base, b.tmem_span_base, sizeof(a.tmem_span_base)) != 0 ||
        memcmp(a.tmem_span_words, b.tmem_span_words, sizeof(a.tmem_span_words)) != 0 ||
        memcmp(a.tmem_span_load_kind, b.tmem_span_load_kind, sizeof(a.tmem_span_load_kind)) != 0 ||
        memcmp(a.tmem_span_src_cls, b.tmem_span_src_cls, sizeof(a.tmem_span_src_cls)) != 0 ||
        memcmp(a.tmem_span_src_stamp, b.tmem_span_src_stamp, sizeof(a.tmem_span_src_stamp)) != 0) {
        return 6;
    }
    for (int i = 0; i < 512; i++) {
        if (a.tmem_span_base[i] == static_cast<uint16_t>(i + 1) && !LtEq(a.loaded_texture[i], b.loaded_texture[i])) {
            return 6;
        }
    }
    return 0;
}

} // namespace

GDX_COLD void Interp::DeadSetVerify(Frame& f) {
    DeadSet& D = *mDs;
    (void)f;
    /* start state */
    memcpy(static_cast<void*>(D.s0), &mRdp, sizeof(Rdp));
    const uint32_t gm0 = mRsp.geometry_mode;
    const uint16_t ss0 = mRsp.texture_scaling_factor.s, st0 = mRsp.texture_scaling_factor.t;
    const size_t ledN0 = DebtStateSave(D.led0, D.ledCap);
    Frame tmp;
    /* the window with the dead commands removed */
    for (uint8_t k = 0; k < D.nLists; k++) {
        const DeadSet::List& L = D.l[k];
        tmp.list = L.ref;
        for (uint8_t i = 0; i < L.n; i++) {
            if (((L.live & L.effect) >> i & 1u) == 0) {
                continue;
            }
            tmp.idx = static_cast<size_t>(i) + 1u;
            (void)Exec(ReadCmd(L.ref, i), tmp, 0);
        }
    }
    memcpy(static_cast<void*>(D.s1), &mRdp, sizeof(Rdp));
    const uint32_t gm1 = mRsp.geometry_mode;
    const uint16_t ss1 = mRsp.texture_scaling_factor.s, st1 = mRsp.texture_scaling_factor.t;
    const size_t ledN1 = DebtStateSave(D.led1, D.ledCap);
    /* back to the start, then the stock window (hooks in list order) */
    memcpy(static_cast<void*>(&mRdp), D.s0, sizeof(Rdp));
    mRsp.geometry_mode = gm0;
    mRsp.texture_scaling_factor.s = ss0;
    mRsp.texture_scaling_factor.t = st0;
    DebtStateLoad(D.led0, ledN0);
    for (uint8_t k = 0; k < D.nLists; k++) {
        const DeadSet::List& L = D.l[k];
        if (mDlHook != nullptr) {
            mDlHookSub = L.ref;
            (void)mDlHook(mDlHookUser, L.ref.base, this);
        }
        tmp.list = L.ref;
        for (uint8_t i = 0; i < L.n; i++) {
            tmp.idx = static_cast<size_t>(i) + 1u;
            (void)Exec(ReadCmd(L.ref, i), tmp, 0);
        }
    }
    int diff = RdpDiff(mRdp, *D.s1);
    if (diff == 0 && (mRsp.geometry_mode != gm1 || mRsp.texture_scaling_factor.s != ss1 ||
                      mRsp.texture_scaling_factor.t != st1)) {
        diff = 4;
    }
    if (diff == 0) {
        const size_t ledN2 = DebtStateSave(D.led0, D.ledCap);
        if (ledN2 != ledN1 || memcmp(D.led0, D.led1, ledN1) != 0) {
            diff = 7;
        }
    }
    if (diff == 0) {
        D.w.vfyOk++;
        D.sesVfyOk++;
    } else {
        D.w.vfyBad++;
        D.w.vfyGroup[diff]++;
        D.sesVfyBad++;
        if (D.sesVfyBad <= 8) {
            GDXN_LOG("[deadset] VERIFY MISMATCH group=%d lists=%u first=%p", diff, static_cast<unsigned>(D.nLists),
                     static_cast<const void*>(D.l[0].ref.base));
        }
    }
}

GDX_COLD int Interp::DeadSetFormat(char* buf, size_t cap, uint32_t tasks, double msPerTick) {
    if (mDs == nullptr || tasks == 0 || cap == 0) {
        return 0;
    }
    DeadSet::Win& w = mDs->w;
    const double inv = 1.0 / static_cast<double>(tasks);
    const int n = snprintf(buf, cap,
                           "[deadset] tasks=%u level=%d /task tries=%.1f plans=%.2f reused=%.2f declined=%.2f lists=%.2f skipped=%.2f "
                           "partial=%.2f full=%.2f cmds=%.1f dead=%.1f deadloads=%.2f plan_us=%.1f "
                           "rfail=%.2f/%.2f/%.2f | trips=%u "
                           "short=%u ses_trips=%u | vfy ok=%u bad=%u (g1..7 %u/%u/%u/%u/%u/%u/%u) ses_bad=%u",
                           static_cast<unsigned>(tasks), mDeadSetLevel, w.tries * inv, w.plans * inv, w.reused * inv,
                           w.declined * inv,
                           w.lists * inv,
                           w.skipped * inv, w.partial * inv, w.full * inv, w.cmds * inv, w.dead * inv,
                           w.deadLoads * inv, static_cast<double>(w.ticks) * msPerTick * 1000.0 * inv, w.rNone * inv,
                           w.rParent * inv, w.rList * inv, w.trips,
                           w.shortLoads, mDs->sesTrips, w.vfyOk, w.vfyBad, w.vfyGroup[1],
                           w.vfyGroup[2], w.vfyGroup[3], w.vfyGroup[4], w.vfyGroup[5], w.vfyGroup[6], w.vfyGroup[7],
                           mDs->sesVfyBad);
    memset(&w, 0, sizeof(w));
    return n;
}

void Interp::DeadSetSession(uint32_t out[7]) const {
    const DeadSet* D = mDs;
    out[6] = D ? D->sesReused : 0u;
    out[0] = D ? D->sesPlans : 0u;
    out[1] = D ? D->sesSkipped : 0u;
    out[2] = D ? D->sesDead : 0u;
    out[3] = D ? D->sesVfyOk : 0u;
    out[4] = D ? D->sesVfyBad : 0u;
    out[5] = D ? D->sesTrips : 0u;
}

uint32_t Interp::DlMemoSessionHits() const {
    return mDlMemo != nullptr ? mDlMemo->sesHits : 0u;
}

} // namespace gdxn
