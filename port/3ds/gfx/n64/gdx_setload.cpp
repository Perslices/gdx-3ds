/* port/3ds/gfx/n64/gdx_setload.cpp -- [native] machines_loadmemo (c5mach2): the LOADBLOCKs of
 * the machines' texture-set lists, with their resolved form memoised.
 *
 * WHY. After machines_texskip, the texture-set list the walker still runs every frame is the
 * last one of each group (`noNext`: nothing after it in the group rewrites its TMEM, so its
 * state is observable). In a steady 1P race that is the one visible machine's list, every
 * frame: ~50 commands, 7 LOADBLOCKs, ~28% of the [c5cat] mach bucket (c5mach2-progress.md §1).
 * A LOADBLOCK there re-derives, every frame, the same answer: the SETTIMG resolution and its
 * classification, the load descriptor (sizes, line strides, source offset) and the readable
 * extent of the source. Only then does it do the work that matters: the TMEM copy and the
 * span store.
 *
 * WHAT IS MEMOISED. The load's RESOLVED FORM, keyed on everything it is computed from: the
 * SETTIMG command words (format, size, width, operand), the LOADBLOCK words (tile, uls, ult,
 * lrs, dxt), the load tile's TMEM address, and the texture-source classification epoch
 * (gdx_gfx_timg_class_epoch: native-RGBA16 generation, venue window, resolve-tables version --
 * the freshness key [timgmemo] and hud_direct's load memo already ship on). Three restrictions
 * make the descriptor a pure function of that key:
 *   - the SETTIMG width is 1 (every machine set list: TexSkipScanList requires it), so
 *     DpLoadBlock's line-stride search over the render tiles never runs -- the descriptor does
 *     not depend on the tile state the previous list left;
 *   - the operand is a tagged host pointer and the source `immutable` (an o2r asset image:
 *     never evicted, never rewritten once published), so its bytes and its readable extent are
 *     the ones banked for as long as the epoch holds;
 *   - only lists the machine registry names as texture-set lists take this path.
 *
 * WHAT STILL RUNS, verbatim from DpLoadBlock and in its order: the texskip tripwire, the load
 * count, the texture_to_load fields a resolution writes, the same-content test and the source
 * stamp (TmemLoadIsRedundant), the TMEM copy (MirrorToTmem, so hud_direct's TMEM debt is cut
 * exactly as the stock copy cuts it), the TMEM generation, the span store, the load kind, the
 * stamp record and the texture arming. So TMEM, the span store and every register end up
 * byte-identical to the stock load's.
 *
 * Level 2 (verify) never serves the memo: it runs the stock load and compares its result with
 * what the memo would have served (`[ldmemo] vfy bad` must be 0). */
#include "gdx_interp.h"

#include <new>
#include <string.h>

#include "gdx_log.h"

namespace gdxn {

namespace {

constexpr uint32_t kLdSets = 64;
constexpr uint32_t kLdReceiptTasks = 64;

inline uint32_t LdSetOf(uint32_t timgW1, uint32_t loadW1) {
    return ((timgW1 >> 3) ^ (timgW1 >> 11) ^ (loadW1 * 2654435761u >> 20)) & (kLdSets - 1u);
}

bool LdLoadedEq(const LoadedTexture& a, const LoadedTexture& b) {
    return a.addr == b.addr && a.orig_size_bytes == b.orig_size_bytes && a.size_bytes == b.size_bytes &&
           a.full_image_line_size_bytes == b.full_image_line_size_bytes && a.line_size_bytes == b.line_size_bytes &&
           a.immutable == b.immutable && a.native_rgba16 == b.native_rgba16;
}

} // namespace

struct Interp::LdMemo {
    struct E {
        uint32_t tw0, tw1, tw1hi; /* SETTIMG */
        uint32_t lw0, lw1;        /* LOADBLOCK */
        uint32_t epoch;
        uint16_t tmemIndex;
        uint8_t filled;
        uint8_t imm, native;
        const uint8_t* tlAddr;
        LoadedTexture loaded; /* tmem_start / tmem_word_count cleared: StoreLoadedTexture sets them */
        uint32_t readable;    /* DlSrc::ReadableBytes(loaded.addr), clamped to 32 bits */
    };
    E e[kLdSets][2];
    uint8_t mru[kLdSets];
    struct Win {
        uint32_t loads, hits, misses, banked, unfit, vfyOk, vfyBad;
    } w;
    uint32_t tasks;
};

void Interp::SetLoadMemo(int level) {
    mLoadMemo = level < 0 ? 0 : (level > 2 ? 2 : level);
    if (mLoadMemo != 0 && mLdMemo == nullptr) {
        mLdMemo = new (std::nothrow) LdMemo;
        if (mLdMemo == nullptr) {
            mLoadMemo = 0;
        } else {
            memset(static_cast<void*>(mLdMemo), 0, sizeof(LdMemo));
        }
    }
    GDXN_LOG("[ldmemo] init machines_loadmemo=%d (%s)", mLoadMemo,
             mLoadMemo == 1 ? "set-list loads memoised" : (mLoadMemo == 2 ? "verify: never serves" : "off"));
}

/* Exec's OP_LOADBLOCK on this path: the frame is a machine texture-set list. */
bool Interp::SetListLoadBlock(const Cmd& c, const Frame& f) {
    if (mLoadMemo == 0 || mTexHooks.setSlot == nullptr || mTexHooks.setSlot(f.list.base) < 0) {
        return false;
    }
    LdMemo& M = *mLdMemo;
    M.w.loads++;
    auto& tl = mRdp.texture_to_load;
    const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    const uint16_t tmemIndex = mRdp.texture_tile[tile].tmem_index;
    const bool fits = tl.width <= 1 && tl.cmd.w1hi == kHostTag && mDspCensus == 0;
    if (!fits) {
        M.w.unfit++;
        return false; /* the caller runs the stock load */
    }
    const uint32_t epoch = mSrc.TimgClassEpoch();
    const uint32_t set = LdSetOf(tl.cmd.w1, c.w1);
    int way = -1;
    for (int k = 0; k < 2 && way < 0; k++) {
        const LdMemo::E& q = M.e[set][k];
        if (q.filled != 0 && q.tw0 == tl.cmd.w0 && q.tw1 == tl.cmd.w1 && q.tw1hi == tl.cmd.w1hi && q.lw0 == c.w0 &&
            q.lw1 == c.w1 && q.tmemIndex == tmemIndex) {
            way = k;
        }
    }
    const bool hit = way >= 0 && M.e[set][way].epoch == epoch;
    if (way < 0) {
        way = M.e[set][0].filled == 0 ? 0 : (M.e[set][1].filled == 0 ? 1 : (M.mru[set] ^ 1));
    }
    M.mru[set] = static_cast<uint8_t>(way);
    LdMemo::E& e = M.e[set][way];
    if (hit && mLoadMemo == 1) {
        M.w.hits++;
        SetListLoadServe(e.tlAddr, e.imm, e.native, e.loaded, e.readable, tmemIndex);
        return true;
    }
    /* The stock load, then its resolved form read back (banked, or compared in verify mode). */
    M.w.misses++;
    const uint32_t skips0 = mStats.tmemSkips;
    DpLoadBlock(tile, C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12), C1(c.w1, 0, 12));
    const bool live = tmemIndex < 512u && mRdp.tmem_span_base[tmemIndex] == static_cast<uint16_t>(tmemIndex + 1u) &&
                      mRdp.tmem_span_load_kind[tmemIndex] == 1u;
    if (!live || tl.addr == nullptr || tl.immutable == 0 || mStats.tmemSkips != skips0) {
        return true; /* not memoised: unresolved, mutable source, or a same-content skip */
    }
    LoadedTexture got = mRdp.loaded_texture[tmemIndex];
    got.tmem_start = 0;
    got.tmem_word_count = 0;
    const size_t readable = got.addr != nullptr ? DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(got.addr)) : 0;
    const uint32_t readable32 = readable > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(readable);
    if (hit) { /* verify: the memo against the stock load */
        const bool same = LdLoadedEq(got, e.loaded) && e.tlAddr == tl.addr && e.imm == tl.immutable &&
                          e.native == tl.native_rgba16 && e.readable == readable32;
        if (same) {
            M.w.vfyOk++;
        } else {
            M.w.vfyBad++;
        }
        return true;
    }
    e.tw0 = tl.cmd.w0;
    e.tw1 = tl.cmd.w1;
    e.tw1hi = tl.cmd.w1hi;
    e.lw0 = c.w0;
    e.lw1 = c.w1;
    e.tmemIndex = tmemIndex;
    e.epoch = epoch;
    e.tlAddr = tl.addr;
    e.imm = tl.immutable;
    e.native = tl.native_rgba16;
    e.loaded = got;
    e.readable = readable32;
    e.filled = 1;
    M.w.banked++;
    return true;
}

/* DpLoadBlock from the point its resolution is known, verbatim (gdx_tmem.cpp; keep in step). */
void Interp::SetListLoadServe(const uint8_t* tlAddr, uint8_t imm, uint8_t native, const LoadedTexture& memo,
                              uint32_t readable, uint16_t tmemIndex) {
    TexSkipNoteWriter(0xF3); /* [native] machines_texskip tripwire */
    mStats.tmemLoads++;
    auto& tl = mRdp.texture_to_load;
    tl.addr = tlAddr; /* what ResolveTextureSource writes */
    tl.immutable = imm;
    tl.native_rgba16 = native;
    LoadedTexture loaded = memo;
    loaded.tmem_start = 0;
    loaded.tmem_word_count = 0;
    const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
    uint32_t testBytes = 0;
    if (tmemByteOffset < sizeof(mRdp.tmem)) {
        testBytes = loaded.size_bytes < sizeof(mRdp.tmem) - tmemByteOffset
                        ? loaded.size_bytes
                        : static_cast<uint32_t>(sizeof(mRdp.tmem) - tmemByteOffset);
    }
    uint64_t srcStamp[2];
    unsigned srcCls = GDX_TMEMSRC_UNKNOWN;
    if (TmemLoadIsRedundant(tmemIndex, 1, loaded, testBytes, srcStamp, &srcCls)) {
        mStats.tmemSkips++;
        return;
    }
    uint32_t tmemWriteEndByte = 0;
    if (loaded.addr != nullptr && tmemByteOffset < sizeof(mRdp.tmem)) {
        const uint32_t copyBytes = readable < testBytes ? readable : testBytes;
        if (copyBytes != 0) {
            MirrorToTmem(mRdp.tmem + tmemByteOffset, loaded.addr, copyBytes, loaded.native_rgba16 != 0);
            mRdp.tmem_generation++;
            tmemWriteEndByte = tmemByteOffset + copyBytes;
        }
    }
    StoreLoadedTexture(tmemIndex, loaded);
    NoteTmemLoadKind(tmemIndex, 1, tmemWriteEndByte);
    RecordTmemSrcStamp(tmemIndex, srcStamp, srcCls);
    ArmTexturesChanged();
}

void Interp::LoadMemoTaskEnd() {
    if (mLoadMemo == 0 || mLdMemo == nullptr) {
        return;
    }
    LdMemo& M = *mLdMemo;
    if (++M.tasks < kLdReceiptTasks) {
        return;
    }
    M.tasks = 0;
    GDXN_LOG("[ldmemo] level=%d loads=%lu hit=%lu miss=%lu banked=%lu unfit=%lu | vfy ok=%lu bad=%lu", mLoadMemo,
             (unsigned long)M.w.loads, (unsigned long)M.w.hits, (unsigned long)M.w.misses,
             (unsigned long)M.w.banked, (unsigned long)M.w.unfit, (unsigned long)M.w.vfyOk,
             (unsigned long)M.w.vfyBad);
    memset(&M.w, 0, sizeof(M.w));
}

} // namespace gdxn
