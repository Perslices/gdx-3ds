/* port/3ds/gfx/n64/gdx_tmem.cpp -- TMEM: tile descriptors with value gates, the span store,
 * lazy per-word views, LOADBLOCK / LOADTILE / LOADTLUT with the same-content skip. Plan §3.6;
 * bodies mirror GfxDpSetTile / GfxDpSetTileSize / GfxDpLoadTlut / GfxDpLoadBlock /
 * GfxDpLoadTile / StoreLoadedTexture / RDP::MaterializeTmemSlot (interpreter.cpp 5941-6906)
 * with the [tmem2] whole-span teardown as the only teardown. */
#include "gdx_interp.h"

#include "gdx3ds_cov.h" // COVERAGE: [cov] per-frame feature census ([debug] cov)

#include <string.h>

/* [native] dspcensus>=2 clock (gdx_rt_glue.cpp / the host harness). */
extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

/* One-line phase timer for the dspcensus>=2 sub-buckets. `on` is mDspCensus >= 2, so the
 * whole thing folds to nothing with the lever off. */
#define DSP_PH_BEGIN(on) const uint64_t dspT0_ = (on) ? (uint64_t)gdx_ninterp_now() : 0
#define DSP_PH_END(on, ph)                                                                                   \
    do {                                                                                                     \
        if (on) {                                                                                            \
            mStats.phTicks[ph] += (uint64_t)gdx_ninterp_now() - dspT0_;                                       \
            mStats.phCalls[ph]++;                                                                             \
        }                                                                                                    \
    } while (0)

static constexpr uint16_t kListInvalid = 0xFFFFu;

void Interp::ArmTexturesChanged() {
    mRdp.textures_changed[0] = true;
    mRdp.textures_changed[1] = true;
}

void Interp::DpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                       uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                       uint32_t shifts) {
    if (cms == TX_WRAP && masks == TX_NOMASK) {
        cms = TX_CLAMP;
    }
    if (cmt == TX_WRAP && maskt == TX_NOMASK) {
        cmt = TX_CLAMP;
    }
    Tile& t = mRdp.texture_tile[tile];
    /* [crowd2] value-gated re-assertion skip (lus-crowd2-tilestate-value-gate). */
    if (t.palette == palette && t.fmt == fmt && t.siz == siz && t.cms == cms && t.cmt == cmt && t.masks == masks &&
        t.maskt == maskt && t.shifts == shifts && t.shiftt == shiftt && t.line_size_bytes == line * 8 &&
        t.tmem == tmem && t.tmem_index == static_cast<uint16_t>(tmem)) {
        return;
    }
    t.palette = static_cast<uint8_t>(palette);
    t.fmt = fmt;
    t.siz = static_cast<uint8_t>(siz);
    t.cms = static_cast<uint8_t>(cms);
    t.cmt = static_cast<uint8_t>(cmt);
    t.masks = static_cast<uint8_t>(masks);
    t.maskt = static_cast<uint8_t>(maskt);
    t.shifts = static_cast<uint8_t>(shifts);
    t.shiftt = static_cast<uint8_t>(shiftt);
    t.line_size_bytes = line * 8;
    t.tmem = static_cast<uint16_t>(tmem);
    t.tmem_index = static_cast<uint16_t>(tmem);
    ArmTexturesChanged();
}

void Interp::DpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    Tile& t = mRdp.texture_tile[tile];
    if (t.uls == uls && t.ult == ult && t.lrs == lrs && t.lrt == lrt) {
        return;
    }
    t.uls = uls;
    t.ult = ult;
    t.lrs = lrs;
    t.lrt = lrt;
    ArmTexturesChanged();
}

/* SETTIMG resolution is lazy: the load extent is only known here. Mirrors the bridge's
 * TranslateTexturePointer(estimate) / wide-stub hop, minus the persistent copy. */
bool Interp::ResolveTextureSource(size_t requiredBytes) {
    auto& tl = mRdp.texture_to_load;
    const bool ph = mDspCensus >= 2;

    DSP_PH_BEGIN(ph);
    const uintptr_t p = mSrc.ResolveTimg(tl.cmd, requiredBytes);
    DSP_PH_END(ph, DSP_PH_TIMGRES);
    if (p == 0) {
        tl.addr = nullptr;
        mStats.timgUnresolved++;
        return false; /* failures are not memoised (see the contract) */
    }
    tl.addr = reinterpret_cast<const uint8_t*>(p);

    /* [native] timgmemo -- see the memo's contract on Interp::mTimgClassMemo. The RESOLVED
     * pointer is the key, not an output: the classification is a pure function of it (plus the
     * command and the load extent) and two counted tables, so this memo needs no segment term
     * and is genuinely cross-frame. */
    if (mTimgMemo) {
        const uint32_t epoch = mSrc.TimgClassEpoch();
        const uint32_t req32 = (requiredBytes <= 0xFFFFFFFFu) ? static_cast<uint32_t>(requiredBytes) : 0xFFFFFFFFu;
        const uint32_t h = (static_cast<uint32_t>(p) * 2654435761u) ^ (req32 * 40503u) ^ (tl.cmd.w1 * 2246822519u);
        TimgClassEntry& e = mTimgClassMemo[(h >> 17) & (kTimgMemoSlots - 1u)];
        if (e.filled != 0 && e.epoch == epoch && e.addr == p && e.w1 == tl.cmd.w1 && e.w1hi == tl.cmd.w1hi &&
            e.required == req32) {
            mStats.timgMemoHit++;
            tl.native_rgba16 = e.native_rgba16;
            tl.immutable = e.immutable;
            return true;
        }
        mStats.timgMemoMiss++;
        if (e.filled == 0) {
            mStats.timgMemoMissEmpty++;
        } else if (e.addr == p && e.w1 == tl.cmd.w1 && e.w1hi == tl.cmd.w1hi && e.required == req32) {
            mStats.timgMemoMissEpoch++;
        } else {
            mStats.timgMemoMissKey++;
        }
        if (epoch != mTimgEpochLast) {
            mStats.timgEpochChanges++;
            mTimgEpochLast = epoch;
        }
        DSP_PH_BEGIN(ph);
        tl.native_rgba16 = DlSrc::TexelsAreNativeRgba16(p, 2) ? 1 : 0;
        tl.immutable = DlSrc::TimgIsResource(tl.cmd, p, requiredBytes) ? 1 : 0;
        DSP_PH_END(ph, DSP_PH_TIMGCLASS);
        e.w1 = tl.cmd.w1;
        e.w1hi = tl.cmd.w1hi;
        e.required = req32;
        e.addr = p;
        e.native_rgba16 = tl.native_rgba16;
        e.immutable = tl.immutable;
        e.epoch = epoch; /* the two calls above mutate nothing, so the probe's epoch still holds */
        e.filled = 1;
        return true;
    }

    {
        DSP_PH_BEGIN(ph);
        tl.native_rgba16 = DlSrc::TexelsAreNativeRgba16(p, 2) ? 1 : 0;
        tl.immutable = DlSrc::TimgIsResource(tl.cmd, p, requiredBytes) ? 1 : 0;
        DSP_PH_END(ph, DSP_PH_TIMGCLASS);
    }
    return true;
}

void Interp::DpLoadTlut(uint8_t tile, uint32_t highIndex) {
    const uint32_t entryCount = highIndex + 1;
    const uint32_t byteCount = entryCount * 2;
    if (!ResolveTextureSource(byteCount)) {
        return;
    }
    const uint16_t tmem = mRdp.texture_tile[tile].tmem;
    const uint8_t* src = mRdp.texture_to_load.addr;
    /* COVERAGE: a TLUT load. B9 (the Silence lamps cycling 8+ palettes in place) is the bug
     * class that lives here, so the schedule has to know which frames load palettes and how
     * big the loads are. value = (tmem << 8) | log2(entries). */
    {
        uint32_t lg = 0, n = entryCount;
        while (n > 1u && lg < 15u) { n >>= 1; lg++; }
        gdx3ds_cov_mark(GDX3DS_COV_KEY(GDX3DS_COV_FAM_TLUT, ((uint32_t)tmem << 8) | lg));
    }
    if (tmem >= 256) {
        const uint32_t paletteByteOffset = (tmem - 256) * 2;
        if (highIndex == 255 && paletteByteOffset == 0) {
            memcpy(mRdp.palette_staging[0], src, 256);
            memcpy(mRdp.palette_staging[1], src + 256, 256);
            mRdp.palettes[0] = mRdp.palette_staging[0];
            mRdp.palettes[1] = mRdp.palette_staging[1];
            mRdp.palette_dram_addr[0] = src;
            mRdp.palette_dram_addr[1] = src + 256;
        } else if (paletteByteOffset < 256) {
            const uint32_t copyLen = (paletteByteOffset + byteCount <= 256) ? byteCount : (256 - paletteByteOffset);
            memcpy(mRdp.palette_staging[0] + paletteByteOffset, src, copyLen);
            mRdp.palettes[0] = mRdp.palette_staging[0];
            mRdp.palette_dram_addr[0] = src;
        } else {
            const uint32_t offset = paletteByteOffset - 256;
            const uint32_t copyLen = (offset + byteCount <= 256) ? byteCount : (256 - offset);
            memcpy(mRdp.palette_staging[1] + offset, src, copyLen);
            mRdp.palettes[1] = mRdp.palette_staging[1];
            mRdp.palette_dram_addr[1] = src;
        }
    } else {
        mRdp.palettes[1] = src;
        mRdp.palette_dram_addr[1] = src;
    }
    ArmTexturesChanged();
    mStats.tmemLoads++;
}

/* ---- span store ---------------------------------------------------------------------- */

static void ListRemove(Rdp& rdp, uint32_t base) {
    if (rdp.tmem_span_list_n == kListInvalid) return;
    for (uint32_t i = 0; i < rdp.tmem_span_list_n; ++i) {
        if (rdp.tmem_span_list[i] == base) {
            rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n];
            return;
        }
    }
}
static void ListInsert(Rdp& rdp, uint32_t base) {
    if (rdp.tmem_span_list_n == kListInvalid) return;
    for (uint32_t i = 0; i < rdp.tmem_span_list_n; ++i) {
        if (rdp.tmem_span_list[i] == base) return;
    }
    if (rdp.tmem_span_list_n >= 64) {
        rdp.tmem_span_list_n = kListInvalid;
        return;
    }
    rdp.tmem_span_list[rdp.tmem_span_list_n++] = static_cast<uint16_t>(base);
}
static bool ListRebuild(Rdp& rdp) {
    uint32_t n = 0;
    for (uint32_t w = 0; w < 512u; ++w) {
        if (rdp.tmem_span_base[w] == static_cast<uint16_t>(w + 1u)) {
            if (n >= 64) {
                rdp.tmem_span_list_n = kListInvalid;
                return false;
            }
            rdp.tmem_span_list[n++] = static_cast<uint16_t>(w);
        }
    }
    rdp.tmem_span_list_n = static_cast<uint16_t>(n);
    return true;
}

/* == StoreLoadedTexture end state: every span overlapping [start, end) is removed (its words
 * outside the new range unmapped), then one span is recorded at `start`. */
void Interp::StoreLoadedTexture(uint16_t start, const LoadedTexture& t) {
    if (start >= 512) return;
    Rdp& rdp = mRdp;
    const uint32_t logicalSize = t.orig_size_bytes != 0 ? t.orig_size_bytes : t.size_bytes;
    uint32_t requested = (logicalSize + 7u) / 8u;
    if (requested < 1) requested = 1;
    const uint32_t wordCount = requested < (512u - start) ? requested : (512u - start);
    const uint32_t newEnd = start + wordCount;

    if (rdp.tmem_span_list_n == kListInvalid) {
        ListRebuild(rdp);
    }
    if (rdp.tmem_span_list_n != kListInvalid) {
        for (uint32_t i = 0; i < rdp.tmem_span_list_n;) {
            const uint32_t base = rdp.tmem_span_list[i];
            if (rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) {
                rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n];
                continue;
            }
            const uint32_t words = rdp.tmem_span_words[base];
            const uint32_t spanEnd = base + words;
            if (words == 0 || spanEnd <= start || base >= newEnd) {
                ++i;
                continue;
            }
            memset(&rdp.loaded_texture[base], 0, sizeof(LoadedTexture));
            rdp.tmem_span_words[base] = 0;
            rdp.tmem_span_load_kind[base] = 0;
            for (uint32_t w = base; w < spanEnd; ++w) {
                if (w < start || w >= newEnd) rdp.tmem_span_base[w] = 0;
            }
            rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n];
        }
    } else {
        /* Overflowed list: per-word walk (rare). */
        for (uint32_t w = start; w < newEnd;) {
            const uint16_t bp1 = rdp.tmem_span_base[w];
            if (bp1 == 0) {
                ++w;
                continue;
            }
            const uint32_t base = bp1 - 1u;
            const uint32_t spanEnd = base + rdp.tmem_span_words[base];
            memset(&rdp.loaded_texture[base], 0, sizeof(LoadedTexture));
            rdp.tmem_span_words[base] = 0;
            rdp.tmem_span_load_kind[base] = 0;
            for (uint32_t k = base; k < spanEnd && k < 512u; ++k) {
                if (k < start || k >= newEnd) rdp.tmem_span_base[k] = 0;
            }
            w = spanEnd > w ? spanEnd : w + 1;
        }
    }
    for (uint32_t w = start; w < newEnd; ++w) {
        rdp.tmem_span_base[w] = static_cast<uint16_t>(start + 1u);
    }
    ListInsert(rdp, start);
    rdp.tmem_span_words[start] = static_cast<uint16_t>(wordCount);
    rdp.tmem_span_load_kind[start] = 0;
    LoadedTexture& baseSlot = rdp.loaded_texture[start];
    baseSlot = t;
    baseSlot.tmem_start = start;
    baseSlot.tmem_word_count = static_cast<uint16_t>(wordCount);
}

/* == RDP::MaterializeTmemSlot, computed on read. */
LoadedTexture Interp::LoadedTexAt(uint16_t word) const {
    LoadedTexture empty;
    memset(&empty, 0, sizeof(empty));
    if (word >= 512) return empty;
    const uint16_t bp1 = mRdp.tmem_span_base[word];
    if (bp1 == 0) return empty;
    const uint16_t base = static_cast<uint16_t>(bp1 - 1u);
    const LoadedTexture& t = mRdp.loaded_texture[base];
    const uint32_t w = static_cast<uint32_t>(word) - base;
    if (w == 0) return t;
    const uint32_t logicalSize = t.orig_size_bytes != 0 ? t.orig_size_bytes : t.size_bytes;
    const uint32_t logicalByteOffset = w * 8u;
    const uint32_t hostByteOffset =
        logicalSize != 0 ? static_cast<uint32_t>((static_cast<uint64_t>(logicalByteOffset) * t.size_bytes) / logicalSize)
                         : logicalByteOffset;
    LoadedTexture e = t;
    if (e.addr != nullptr) e.addr += hostByteOffset;
    e.size_bytes = t.size_bytes > hostByteOffset ? t.size_bytes - hostByteOffset : 0;
    e.orig_size_bytes = t.orig_size_bytes > logicalByteOffset ? t.orig_size_bytes - logicalByteOffset : 0;
    e.line_size_bytes = 0;
    e.full_image_line_size_bytes = 0;
    return e;
}

void Interp::NoteTmemLoadKind(uint16_t base, uint8_t kind, uint32_t writeEndByte) {
    Rdp& rdp = mRdp;
    if (base >= 512u || rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) return;
    rdp.tmem_span_load_kind[base] = kind;
    const uint32_t spanEnd = base + rdp.tmem_span_words[base];
    uint32_t writeEndWord = (writeEndByte + 7u) / 8u;
    if (writeEndWord > 512u) writeEndWord = 512u;
    for (uint32_t w = spanEnd; w < writeEndWord; ++w) {
        if (rdp.tmem_span_base[w] != 0) {
            rdp.tmem_span_load_kind[rdp.tmem_span_base[w] - 1u] = 0;
        }
    }
}

/* == Everything GdxTmemSameContentLoad tests EXCEPT the source bytes.
 *
 * This is the whole of the skip's TMEM-side reasoning and it is UNCHANGED by [native] tmemskip:
 * the live span at `base` must still be this span (tmem_span_base[base] == base+1), still have
 * been written by this kind of load, still be exactly `want` words long, and the descriptor it
 * carries -- source pointer, both sizes, both line strides, the byte-swap mode and the o2r
 * class -- must be identical. A later load that overlapped these TMEM words retired this span
 * (StoreLoadedTexture zeroes tmem_span_words[base] and memsets loaded_texture[base]), so it
 * fails here; a load at the same base with different geometry fails on `want`.
 *
 * The ONLY input it does not cover is the CONTENT of [fresh.addr, fresh.addr + bytes), and the
 * only job of the gate on top of it is to cover that one input. */
bool Interp::SameContentDescriptor(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const {
    if (base >= 512u) return false;
    const Rdp& rdp = mRdp;
    const uint32_t logicalSize = fresh.orig_size_bytes != 0 ? fresh.orig_size_bytes : fresh.size_bytes;
    uint32_t want = (logicalSize + 7u) / 8u;
    if (want < 1) want = 1;
    if (want > 512u - base) want = 512u - base;
    if (rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u) || rdp.tmem_span_load_kind[base] != kind ||
        rdp.tmem_span_words[base] != want) {
        return false;
    }
    const LoadedTexture& prev = rdp.loaded_texture[base];
    return prev.tmem_word_count == want && prev.addr == fresh.addr && prev.orig_size_bytes == fresh.orig_size_bytes &&
           prev.size_bytes == fresh.size_bytes && prev.full_image_line_size_bytes == fresh.full_image_line_size_bytes &&
           prev.line_size_bytes == fresh.line_size_bytes && prev.native_rgba16 == fresh.native_rgba16 &&
           prev.immutable == fresh.immutable;
}

/* == GdxTmemSameContentLoad: gated on the immutable (LUS resource-backed) class. The shipping
 * gate, byte for byte, and the one [native] tmemskip=0 still takes. */
bool Interp::SameContentLoad(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const {
    return fresh.immutable != 0 && SameContentDescriptor(base, kind, fresh);
}

void Interp::RecordTmemSrcStamp(uint16_t base, const uint64_t stamp[2], unsigned cls) {
    if (base >= 512u) return;
    mRdp.tmem_span_src_stamp[base][0] = stamp[0];
    mRdp.tmem_span_src_stamp[base][1] = stamp[1];
    mRdp.tmem_span_src_cls[base] = static_cast<uint8_t>(cls);
}

/* Census only: which test of SameContentDescriptor failed first. See Counters::tmemMissSpan. */
void Interp::NoteDescriptorMiss(uint16_t base, uint8_t kind, const LoadedTexture& fresh) {
    if (base >= 512u) {
        mStats.tmemMissSpan++;
        return;
    }
    const Rdp& rdp = mRdp;
    if (rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) {
        mStats.tmemMissSpan++;
        return;
    }
    if (rdp.tmem_span_load_kind[base] != kind) {
        mStats.tmemMissKind++;
        return;
    }
    const uint32_t logicalSize = fresh.orig_size_bytes != 0 ? fresh.orig_size_bytes : fresh.size_bytes;
    uint32_t want = (logicalSize + 7u) / 8u;
    if (want < 1) want = 1;
    if (want > 512u - base) want = 512u - base;
    if (rdp.tmem_span_words[base] != want || rdp.loaded_texture[base].tmem_word_count != want) {
        mStats.tmemMissWords++;
        return;
    }
    if (rdp.loaded_texture[base].addr != fresh.addr) {
        mStats.tmemMissAddr++;
        return;
    }
    mStats.tmemMissOther++;
}

/* == [native] tmemskip -- the widened same-content skip.
 *
 * Shape: the descriptor test above answers "is everything except the bytes the same"; this adds
 * the one remaining question, "are the BYTES the same", using the bridge's own source-stability
 * seam instead of a compare or a second content hash. The stamp for the NEXT load is sampled
 * here, BEFORE the mirror copy reads the source, so a write that lands during the copy leaves
 * the live generation ahead of the stamp and the next load re-mirrors. (The bridge stamps AFTER
 * its copy; this is deliberately the other way round -- see tmemblock-progress.md S2.)
 *
 * The lever also decides which classes may skip: 1 = the two the bridge treats as stable with
 * no compare at all, 2 = additionally the DMA-tracked class. The stamp is recorded for EVERY
 * class regardless, so a level change does not need a warm-up frame. */
bool Interp::TmemLoadIsRedundant(uint16_t base, uint8_t kind, const LoadedTexture& fresh, uint32_t readBytes,
                                 uint64_t outStamp[2], unsigned* outCls) {
    outStamp[0] = 0;
    outStamp[1] = 0;
    *outCls = GDX_TMEMSRC_UNKNOWN;

    /* LEVER OFF: the shipping gate, by the same call, in the same order -- not one bridge call
       and not one extra comparison. The classification below is a range scan and the descriptor
       test is ten compares that `!fresh.immutable` used to short-circuit; paying either on the
       default path would be a regression dressed up as a diagnostic. */
    if (mTmemSkip == 0 && mTmemCls == 0) {
        return SameContentLoad(base, kind, fresh);
    }

    const bool descSame = SameContentDescriptor(base, kind, fresh);
    if (!descSame) {
        NoteDescriptorMiss(base, kind, fresh);
    }
    if (descSame) {
        mStats.tmemDescSame++;
        /* The shipping gate first: an o2r resource is immutable by class and needs no stamp.
           Its span's stored stamp stays whatever the last mirror left, which is correct -- a
           skip writes nothing, so nothing about the mirror has changed. */
        if (fresh.immutable != 0) {
            if (mTmemSkip != 0) mStats.tmemSkipImm++;
            return true;
        }
    }

    /* From here the mirror is going to be rewritten unless the staleness test says otherwise,
       so this is where classifying and stamping is paid for. An unresolved load still rewrites
       the span store with a null descriptor; leave its class UNKNOWN so it can never satisfy a
       later load. */
    if (fresh.addr == nullptr || readBytes == 0) {
        return false;
    }
    const uintptr_t src = reinterpret_cast<uintptr_t>(fresh.addr);
    const unsigned cls = gdx_gfx_tmem_source_class(src, readBytes);
    gdx_gfx_tmem_source_stamp(outStamp);
    *outCls = cls;
    switch (cls) {
        case GDX_TMEMSRC_NATIVE: mStats.tmemClsN++; break;
        case GDX_TMEMSRC_STABLE: mStats.tmemClsS++; break;
        case GDX_TMEMSRC_DMA: mStats.tmemClsD++; break;
        default: mStats.tmemClsU++; break;
    }
    if (mTmemSkip == 0 || !descSame) {
        return false;
    }
    /* Only the classes this level admits. UNKNOWN is admitted at no level: the owned walk reads
       the live source and holds no copy to fall back to a compare against. */
    const bool admitted = (cls == GDX_TMEMSRC_NATIVE || cls == GDX_TMEMSRC_STABLE) ||
                          (mTmemSkip >= 2 && cls == GDX_TMEMSRC_DMA);
    if (!admitted) {
        return false;
    }
    /* [memotear] B54: this load is admitted and is about to skip. If fault injection is armed
       it is forced stale HERE -- after admission, before either test -- so the two arms below
       are the code that has to catch it. See TmemInjectStaleness. */
    const bool injected = (mTmemFault != 0) && TmemInjectStaleness(base, cls, src, readBytes);
    /* The class the STAMP was taken under has to be the class being tested now; the seam
       re-derives the live class and refuses a mismatch, and this is the stored half of it. */
    if (mRdp.tmem_span_src_cls[base] != static_cast<uint8_t>(cls)) {
        mStats.tmemStale++;
        if (injected) {
            mStats.tmemFaultCaught++;
        }
        return false;
    }
    if (gdx_gfx_tmem_source_unchanged(src, readBytes, cls, mRdp.tmem_span_src_stamp[base]) == 0) {
        mStats.tmemStale++;
        if (injected) {
            mStats.tmemFaultCaught++;
        }
        return false;
    }
    if (injected) {
        /* The gate skipped a load that was deliberately made stale. That is the B54 failure and
           it is counted, not asserted, so one occurrence does not take the run down with it --
           the gate is `missed == 0` on the receipt, and the frame hashes beside it. */
        mStats.tmemFaultMissed++;
    }
    mStats.tmemSkipGen++;
    return true;
}

/* [memotear] NEXT.md B54 -- FAULT INJECTION for the tmemskip refusal arm.
 *
 * tmemblock gated tmemskip green over 412,326 corpus skips with `stale=0`, which reads as a
 * clean invalidation argument and is in fact the opposite: it means the corpus never drove the
 * gate to say STALE even once, so nothing outside the host test has ever executed the refusal.
 * An arm that is never taken is not evidence, and `tmemskip` is default 0 partly because of it.
 *
 * This forces the world stale on every Nth load that would otherwise have skipped, alternating
 * the two mechanisms the gate is supposed to catch:
 *
 *   0  THE SOURCE GENERATION MOVED. Driven through the bridge's own bump/dirty-range plumbing
 *      (gdx_gfx_tmem_fault_move_generation), not by corrupting the stored stamp, so what runs
 *      is the invalidation path rather than a comparison. Returns 0 for STABLE and UNKNOWN,
 *      which have no generation arm at all -- see the seam's comment -- and the injector then
 *      falls through to the class mechanism rather than counting a fault it did not cause.
 *   1  THE CLASS CHANGED UNDER THE SPAN. The stored class is moved to another admitted class,
 *      which is what a range registered as native-RGBA16 after the first load looks like.
 *
 * Both are conservative: a refusal re-mirrors, which is exactly what tmemskip=0 does on every
 * load, so an injected run must draw the same pixels as the tmemskip=0 run. That is the second
 * half of the gate and the reason the injector never touches the SOURCE BYTES: mutating those
 * would change the picture, and then "the frame still draws correctly" could not be asserted. */
bool Interp::TmemInjectStaleness(uint16_t base, unsigned cls, uintptr_t src, uint32_t readBytes) {
    mTmemFaultTick++;
    if (mTmemFault <= 0 || (mTmemFaultTick % static_cast<uint32_t>(mTmemFault)) != 0) {
        return false;
    }
    /* Alternate the mechanisms so one run exercises both arms. */
    const bool wantGeneration = ((mTmemFaultTick / static_cast<uint32_t>(mTmemFault)) & 1u) == 0u;
    if (wantGeneration && gdx_gfx_tmem_fault_move_generation(src, readBytes, cls) != 0) {
        mStats.tmemFaultGen++;
        return true;
    }
    /* Class mechanism -- also the fallback for STABLE/UNKNOWN, which have no generation. Pick a
       class that is NOT the live one and is still a value the stored half can legitimately
       hold, so the arm under test is the mismatch and not a malformed enum. */
    const uint8_t other = (cls == GDX_TMEMSRC_NATIVE) ? static_cast<uint8_t>(GDX_TMEMSRC_STABLE)
                                                      : static_cast<uint8_t>(GDX_TMEMSRC_NATIVE);
    mRdp.tmem_span_src_cls[base] = other;
    mStats.tmemFaultCls++;
    return true;
}

/* The TMEM mirror sees what LUS saw: CopyRawTextureBytes byte-swapped host-order RGBA16
 * sources before the interpreter read them. */
void Interp::MirrorToTmem(uint8_t* dst, const uint8_t* src, uint32_t n, bool nativeRgba16) {
    if (!nativeRgba16) {
        memcpy(dst, src, n);
        return;
    }
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
    if (i < n) dst[i] = src[i];
}

static uint32_t BytesForTexels(uint64_t texels, uint8_t siz) {
    uint64_t bytes = 0;
    switch (siz) {
        case IM_SIZ_4b: bytes = (texels + 1u) / 2u; break;
        case IM_SIZ_8b: bytes = texels; break;
        case IM_SIZ_16b: bytes = texels * 2u; break;
        case IM_SIZ_32b: bytes = texels * 4u; break;
        default: break;
    }
    return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes);
}

void Interp::DpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    mStats.tmemLoads++;
    auto& tl = mRdp.texture_to_load;
    const uint32_t orig_size_bytes = BytesForTexels(static_cast<uint64_t>(lrs) + 1u, tl.siz);
    const uint32_t size_bytes = orig_size_bytes;
    const uint16_t tmemIndex = mRdp.texture_tile[tile].tmem_index;
    LoadedTexture loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.orig_size_bytes = orig_size_bytes;
    loaded.size_bytes = size_bytes;

    uint32_t logical_line_bytes = 0;
    for (uint32_t renderTile = 0; renderTile < 8; ++renderTile) {
        const Tile& ct = mRdp.texture_tile[renderTile];
        if (renderTile == tile || ct.tmem_index != tmemIndex || ct.line_size_bytes == 0 || tl.width <= 1) {
            continue;
        }
        uint32_t candidateTmemLineBytes = ct.line_size_bytes;
        if (ct.siz == IM_SIZ_32b) candidateTmemLineBytes *= 2u;
        if (dxt != 0 && tl.siz != ct.siz) {
            constexpr uint32_t kDxtOne = 1u << 11;
            const uint32_t dxtWordsPerLine = (kDxtOne + dxt - 1u) / dxt;
            const uint32_t dxtLineBytes = dxtWordsPerLine * 8u;
            if (dxtLineBytes == candidateTmemLineBytes && dxtLineBytes <= size_bytes && size_bytes % dxtLineBytes == 0) {
                logical_line_bytes = dxtLineBytes;
                break;
            }
        }
        const uint32_t candidateSourceLineBytes = BytesForTexels(tl.width, ct.siz);
        const uint32_t candidateAlignedTmemLineBytes = (candidateSourceLineBytes + 7u) & ~7u;
        if (candidateSourceLineBytes != 0 && candidateTmemLineBytes == candidateAlignedTmemLineBytes &&
            candidateSourceLineBytes <= size_bytes && size_bytes % candidateSourceLineBytes == 0) {
            logical_line_bytes = candidateSourceLineBytes;
            break;
        }
    }
    uint32_t source_line_bytes = logical_line_bytes != 0 ? logical_line_bytes : size_bytes;
    if (tl.width > 1) {
        const uint32_t transferLineBytes = BytesForTexels(tl.width, tl.siz);
        if (logical_line_bytes == 0 && transferLineBytes > 0 && transferLineBytes <= size_bytes &&
            size_bytes % transferLineBytes == 0) {
            source_line_bytes = transferLineBytes;
        }
    }
    loaded.line_size_bytes = source_line_bytes;
    loaded.full_image_line_size_bytes = source_line_bytes;
    const size_t rowOffset = static_cast<size_t>(ult) * source_line_bytes;
    const size_t columnOffset = BytesForTexels(uls, tl.siz);
    /* Resolve the source with the full read extent (bridge EstimateRawTextureCopyBytes). */
    const size_t need = rowOffset + columnOffset + size_bytes;
    if (!ResolveTextureSource(need > 0 ? need : 1)) {
        loaded.addr = nullptr;
    } else {
        loaded.addr = tl.addr + rowOffset + columnOffset;
        loaded.immutable = tl.immutable;
        loaded.native_rgba16 = tl.native_rgba16;
    }

    const bool ph = mDspCensus >= 2;
    /* [native] tmemskip: the staleness test is bounded by the TMEM window, which is an UPPER
       BOUND on the bytes MirrorToTmem will read (the ReadableBytes clamp below can only shrink
       it further). Testing a superset for staleness over-invalidates and never under-; the
       seam's whole-extent classification is what makes the bound safe. */
    const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
    uint32_t testBytes = 0;
    if (tmemByteOffset < sizeof(mRdp.tmem)) {
        testBytes = size_bytes < sizeof(mRdp.tmem) - tmemByteOffset
                        ? size_bytes
                        : static_cast<uint32_t>(sizeof(mRdp.tmem) - tmemByteOffset);
    }
    uint64_t srcStamp[2];
    unsigned srcCls = GDX_TMEMSRC_UNKNOWN;
    {
        DSP_PH_BEGIN(ph);
        const bool same = TmemLoadIsRedundant(tmemIndex, 1, loaded, testBytes, srcStamp, &srcCls);
        DSP_PH_END(ph, DSP_PH_TMEMSAME);
        if (same) {
            mStats.tmemSkips++;
            return;
        }
    }

    uint32_t tmemWriteEndByte = 0;
    if (loaded.addr != nullptr) {
        if (tmemByteOffset < sizeof(mRdp.tmem)) {
            uint32_t copyBytes = testBytes;
            const size_t readable = DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(loaded.addr));
            if (readable < copyBytes) copyBytes = static_cast<uint32_t>(readable);
            if (copyBytes != 0) {
                DSP_PH_BEGIN(ph);
                MirrorToTmem(mRdp.tmem + tmemByteOffset, loaded.addr, copyBytes, loaded.native_rgba16 != 0);
                DSP_PH_END(ph, DSP_PH_TMEMCOPY);
                mRdp.tmem_generation++;
                tmemWriteEndByte = tmemByteOffset + copyBytes;
            }
        }
    }
    {
        DSP_PH_BEGIN(ph);
        StoreLoadedTexture(tmemIndex, loaded);
        NoteTmemLoadKind(tmemIndex, 1, tmemWriteEndByte);
        /* After StoreLoadedTexture, which is what makes tmemIndex a live base. The stamp was
           sampled BEFORE the copy read the source. */
        RecordTmemSrcStamp(tmemIndex, srcStamp, srcCls);
        DSP_PH_END(ph, DSP_PH_TMEMSTORE);
    }
    ArmTexturesChanged();
}

void Interp::DpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    mStats.tmemLoads++;
    auto& tl = mRdp.texture_to_load;
    uint32_t word_size_shift = 0;
    switch (tl.siz) {
        case IM_SIZ_16b: word_size_shift = 1; break;
        case IM_SIZ_32b: word_size_shift = 2; break;
        default: word_size_shift = 0; break;
    }
    const uint32_t offset_x = uls >> 2;
    const uint32_t offset_y = ult >> 2;
    const uint32_t tile_width = ((lrs - uls) >> 2) + 1;
    const uint32_t tile_height = ((lrt - ult) >> 2) + 1;
    const uint32_t full_image_width = tl.width;
    const uint32_t offset_x_in_bytes = offset_x << word_size_shift;
    const uint32_t tile_line_size_bytes = tile_width << word_size_shift;
    const uint32_t full_image_line_size_bytes = full_image_width << word_size_shift;
    const uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    const uint32_t size_bytes = orig_size_bytes;
    const uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    const uint16_t tmemIndex = mRdp.texture_tile[tile].tmem_index;
    LoadedTexture loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.orig_size_bytes = orig_size_bytes;
    loaded.size_bytes = size_bytes;
    loaded.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded.line_size_bytes = tile_line_size_bytes;
    const size_t need = static_cast<size_t>(start_offset_bytes) +
                        (tile_height > 0 ? static_cast<size_t>(tile_height - 1) * full_image_line_size_bytes : 0) +
                        tile_line_size_bytes;
    if (!ResolveTextureSource(need > 0 ? need : 1)) {
        loaded.addr = nullptr;
    } else {
        loaded.addr = tl.addr + start_offset_bytes;
        loaded.immutable = tl.immutable;
        loaded.native_rgba16 = tl.native_rgba16;
    }

    /* [native] tmemskip: LOADTILE reads a STRIDED window, so the extent the staleness test has
       to cover runs from loaded.addr to the end of the last row -- `need` above, minus the
       start offset that loaded.addr already carries. The copy loop below can stop short of it
       (the ReadableBytes break), which is the same over-bound as LOADBLOCK's. */
    const size_t tileReadSpan = need > static_cast<size_t>(start_offset_bytes)
                                    ? need - static_cast<size_t>(start_offset_bytes)
                                    : 0;
    const uint32_t tileTestBytes =
        tileReadSpan <= 0xFFFFFFFFu ? static_cast<uint32_t>(tileReadSpan) : 0xFFFFFFFFu;
    uint64_t srcStamp[2];
    unsigned srcCls = GDX_TMEMSRC_UNKNOWN;
    if (TmemLoadIsRedundant(tmemIndex, 2, loaded, tileTestBytes, srcStamp, &srcCls)) {
        mStats.tmemSkips++;
        Tile& t = mRdp.texture_tile[tile];
        if (!(t.uls == uls && t.ult == ult && t.lrs == lrs && t.lrt == lrt)) {
            t.uls = uls;
            t.ult = ult;
            t.lrs = lrs;
            t.lrt = lrt;
            ArmTexturesChanged();
        }
        return;
    }

    uint32_t tmemWriteEndByte = 0;
    if (loaded.addr != nullptr) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
        const uint32_t destStride = (tile_line_size_bytes + 7u) & ~7u;
        if (tmemByteOffset < sizeof(mRdp.tmem) && destStride != 0) {
            const size_t readable = DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(loaded.addr));
            bool wrote = false;
            for (uint32_t y = 0; y < tile_height; y++) {
                const uint32_t destOff = tmemByteOffset + y * destStride;
                const size_t srcOff = static_cast<size_t>(y) * full_image_line_size_bytes;
                if (destOff >= sizeof(mRdp.tmem) || srcOff + tile_line_size_bytes > readable) break;
                uint32_t rowBytes = tile_line_size_bytes;
                if (rowBytes > sizeof(mRdp.tmem) - destOff) rowBytes = sizeof(mRdp.tmem) - destOff;
                MirrorToTmem(mRdp.tmem + destOff, loaded.addr + srcOff, rowBytes, loaded.native_rgba16 != 0);
                wrote = true;
                tmemWriteEndByte = destOff + rowBytes;
            }
            if (wrote) mRdp.tmem_generation++;
        }
    }
    StoreLoadedTexture(tmemIndex, loaded);
    NoteTmemLoadKind(tmemIndex, 2, tmemWriteEndByte);
    RecordTmemSrcStamp(tmemIndex, srcStamp, srcCls);
    Tile& t = mRdp.texture_tile[tile];
    t.uls = uls;
    t.ult = ult;
    t.lrs = lrs;
    t.lrt = lrt;
    ArmTexturesChanged();
}

} // namespace gdxn
