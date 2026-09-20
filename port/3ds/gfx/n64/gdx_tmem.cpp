/* port/3ds/gfx/n64/gdx_tmem.cpp -- TMEM: tile descriptors with value gates, the span store,
 * lazy per-word views, LOADBLOCK / LOADTILE / LOADTLUT with the same-content skip. Plan §3.6;
 * bodies mirror GfxDpSetTile / GfxDpSetTileSize / GfxDpLoadTlut / GfxDpLoadBlock /
 * GfxDpLoadTile / StoreLoadedTexture / RDP::MaterializeTmemSlot (interpreter.cpp 5941-6906)
 * with the [tmem2] whole-span teardown as the only teardown. */
#include "gdx_interp.h"

#include <string.h>

namespace gdxn {

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
    const uintptr_t p = mSrc.ResolveTimg(tl.cmd, requiredBytes);
    if (p == 0) {
        tl.addr = nullptr;
        mStats.timgUnresolved++;
        return false;
    }
    tl.addr = reinterpret_cast<const uint8_t*>(p);
    tl.native_rgba16 = DlSrc::TexelsAreNativeRgba16(p, 2) ? 1 : 0;
    tl.immutable = DlSrc::TimgIsResource(tl.cmd, p, requiredBytes) ? 1 : 0;
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

/* == GdxTmemSameContentLoad: gated on the immutable (LUS resource-backed) class. */
bool Interp::SameContentLoad(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const {
    if (base >= 512u || !fresh.immutable) return false;
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

    if (SameContentLoad(tmemIndex, 1, loaded)) {
        mStats.tmemSkips++;
        return;
    }

    uint32_t tmemWriteEndByte = 0;
    if (loaded.addr != nullptr) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
        if (tmemByteOffset < sizeof(mRdp.tmem)) {
            uint32_t copyBytes = size_bytes;
            if (copyBytes > sizeof(mRdp.tmem) - tmemByteOffset) copyBytes = sizeof(mRdp.tmem) - tmemByteOffset;
            const size_t readable = DlSrc::ReadableBytes(reinterpret_cast<uintptr_t>(loaded.addr));
            if (readable < copyBytes) copyBytes = static_cast<uint32_t>(readable);
            if (copyBytes != 0) {
                MirrorToTmem(mRdp.tmem + tmemByteOffset, loaded.addr, copyBytes, loaded.native_rgba16 != 0);
                mRdp.tmem_generation++;
                tmemWriteEndByte = tmemByteOffset + copyBytes;
            }
        }
    }
    StoreLoadedTexture(tmemIndex, loaded);
    NoteTmemLoadKind(tmemIndex, 1, tmemWriteEndByte);
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

    if (SameContentLoad(tmemIndex, 2, loaded)) {
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
    Tile& t = mRdp.texture_tile[tile];
    t.uls = uls;
    t.ult = ult;
    t.lrs = lrs;
    t.lrt = lrt;
    ArmTexturesChanged();
}

} // namespace gdxn
