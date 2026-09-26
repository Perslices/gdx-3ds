/* port/gdx_block_cache.cpp -- implementation of the set-associative file block reader.
 * See gdx_block_cache.h for the why and the contract.
 *
 * This is diskstream's cache, lifted verbatim out of gdx_disk_stream.cpp's file statics into
 * a struct. Every design note below is that agent's and is kept because the reasons still
 * hold for the second consumer:
 *
 * Cache shape: SET-ASSOCIATIVE, 4 ways, LRU inside the set. Direct-mapped was the first
 * instinct and it is wrong here: the EK asset fill walks 765 rows whose offsets are NOT
 * sorted, so two hot rows that happen to alias would ping-pong forever, and a cache that
 * thrashes is worse than the malloc it replaced. (The audio consumer has the same shape for
 * a different reason: 72 VADPCM samples scattered across 10.25 MB, with the synthesiser
 * walking several at once, one DMA per voice per audio frame.) Four ways costs four tag
 * compares per lookup and removes that failure mode. Fully-associative was the other option
 * and was rejected: at 96+ blocks a linear scan on every read is a real cost on a 268 MHz
 * ARM11.
 */
#include "gdx_block_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define GDX_BLOCK_CACHE_DEFAULT_BLOCK_LOG2 14u /* 16 KiB */

namespace {

inline FILE* fh(const GdxBlockCache* c) { return (FILE*) c->file; }

/* Fills slot `way` of set `set` with `block`. Returns false on a short or failed host read,
 * leaving the slot INVALID -- a half-filled slot that later reads as a hit would serve
 * garbage forever, which is the whole failure mode this file exists to avoid. */
bool fillSlot(GdxBlockCache* c, unsigned int set, unsigned int way, unsigned int block) {
    const unsigned int idx = set * GDX_BLOCK_CACHE_WAYS + way;
    unsigned char* dst = c->data + (size_t) idx * c->blockBytes;
    const unsigned long long off = (unsigned long long) block << c->blockLog2;
    unsigned int want = c->blockBytes;
    if (off >= (unsigned long long) c->fileSize) {
        return false;
    }
    if (off + want > (unsigned long long) c->fileSize) {
        /* The last block of a file whose size is not a multiple of the block size. 64DD
         * images are 64,931,840 B and an .o2r is whatever it is, so this is the NORMAL tail
         * case, not an error. */
        want = (unsigned int) ((unsigned long long) c->fileSize - off);
    }
    if (c->slots[idx].valid) {
        c->stats.blockEvictions++;
    }
    c->slots[idx].valid = 0;
    /* WORKING-SET MAP (`ekdrive`, B11d; see gdx_block_cache_unique_blocks). Marked HERE, on
     * the fill path, so it counts distinct blocks the FILE had to serve and never the hit
     * path. Marked before the fseek rather than after the fread deliberately: a block that
     * was attempted and failed is still a block this session reached for, and `refetch` must
     * not be able to go negative because a failed fill was not counted. */
    if (c->touched != nullptr && (block >> 3) < c->touchedBytes) {
        const unsigned char bit = (unsigned char) (1u << (block & 7u));
        if ((c->touched[block >> 3] & bit) == 0) {
            c->touched[block >> 3] |= bit;
            c->uniqueBlocks++;
        }
    }
    if (fseek(fh(c), (long) off, SEEK_SET) != 0) {
        return false;
    }
    const size_t got = fread(dst, 1, want, fh(c));
    c->stats.hostReads++;
    c->stats.hostBytes += got;
    if (got != want) {
        return false;
    }
    if (want < c->blockBytes) {
        memset(dst + want, 0, c->blockBytes - want);
    }
    c->slots[idx].block = block;
    c->slots[idx].valid = 1;
    return true;
}

void touch(GdxBlockCache* c, unsigned int set, unsigned int way) {
    const unsigned int base = set * GDX_BLOCK_CACHE_WAYS;
    const unsigned char was = c->slots[base + way].age;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (c->slots[base + w].age < was) {
            c->slots[base + w].age = (unsigned char) (c->slots[base + w].age + 1);
        }
    }
    c->slots[base + way].age = 0;
}

/* Returns a pointer to the cached bytes of `block`, or nullptr if it could not be read.
 *
 * `count` exists because the read walks its blocks TWICE (prove, then copy). If both walks
 * counted, every block would be charged one miss and an extra hit, and the receipt's hit%%
 * would be inflated by construction -- a statistic that flatters itself is worse than none,
 * because the dump will be read against this number. The PROVING walk counts (that is where
 * a miss genuinely happens) and the copying walk does not. */
const unsigned char* getBlock(GdxBlockCache* c, unsigned int block, bool count) {
    const unsigned int set = block & c->setMask;
    const unsigned int base = set * GDX_BLOCK_CACHE_WAYS;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (c->slots[base + w].valid && c->slots[base + w].block == block) {
            if (count) {
                c->stats.blockHits++;
            }
            touch(c, set, w);
            return c->data + (size_t) (base + w) * c->blockBytes;
        }
    }
    if (count) {
        c->stats.blockMisses++;
    }
    /* Victim: the first invalid way, else the least recently used (highest age). */
    unsigned int victim = 0;
    bool haveInvalid = false;
    for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
        if (!c->slots[base + w].valid) {
            victim = w;
            haveInvalid = true;
            break;
        }
    }
    if (!haveInvalid) {
        unsigned char bestAge = 0;
        for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
            if (c->slots[base + w].age >= bestAge) {
                bestAge = c->slots[base + w].age;
                victim = w;
            }
        }
    }
    if (!fillSlot(c, set, victim, block)) {
        return nullptr;
    }
    touch(c, set, victim);
    return c->data + (size_t) (base + victim) * c->blockBytes;
}

} // namespace

extern "C" {

int gdx_block_cache_open(GdxBlockCache* c, const char* path, unsigned int base,
                         unsigned int span, unsigned int expectedFileSize,
                         unsigned int blockLog2, unsigned int cacheBlocks) {
    if (c == nullptr) {
        return 0;
    }
    gdx_block_cache_close(c);
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    if (blockLog2 < 12u || blockLog2 > 18u) {
        blockLog2 = GDX_BLOCK_CACHE_DEFAULT_BLOCK_LOG2;
    }
    if (cacheBlocks < GDX_BLOCK_CACHE_WAYS) {
        cacheBlocks = GDX_BLOCK_CACHE_WAYS;
    }
    if (cacheBlocks > 4096u) {
        cacheBlocks = 4096u;
    }
    /* Sets must be a power of two so the index is a mask, and cacheBlocks a multiple of the
     * associativity. Round the SET COUNT down to a power of two, then blocks = sets * ways,
     * so the caller never gets a bigger cache than it asked for. */
    unsigned int sets = cacheBlocks / GDX_BLOCK_CACHE_WAYS;
    unsigned int pow2 = 1;
    while ((pow2 << 1) <= sets) {
        pow2 <<= 1;
    }
    sets = pow2;

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return 0;
    }
    if (expectedFileSize != 0u && (unsigned long) sz != (unsigned long) expectedFileSize) {
        /* Wrong size is a REFUSAL, not a warning. A short image read through would OOB the
         * fixed-offset consumers (disk_buffer.cpp already says so for the resident path); a
         * long one is not the file we were promised at all. */
        fclose(f);
        return 0;
    }
    if (span == 0u) {
        span = (unsigned int) sz - base;
    }
    if ((unsigned long long) base + span > (unsigned long long) sz) {
        /* The window does not fit in the file: the same refusal, for the same reason. */
        fclose(f);
        return 0;
    }

    const unsigned int blocks = sets * GDX_BLOCK_CACHE_WAYS;
    GdxBlockCacheSlot* slots = (GdxBlockCacheSlot*) calloc(blocks, sizeof(GdxBlockCacheSlot));
    unsigned char* data = (unsigned char*) malloc((size_t) blocks << blockLog2);
    /* Working-set bitmap: one bit per FILE block, so its size follows the file and the block
       size, not the cache. 496 bytes for a 64DD image, 131 for a 17 MB .o2r, at 16 KiB. It is
       indexed by the same absolute-file block number the tags carry, so a windowed cache
       simply never sets the bits outside its window. */
    const unsigned int fileBlocks =
        (unsigned int) ((((unsigned long long) sz) + (1ull << blockLog2) - 1ull) >> blockLog2);
    const unsigned int touchedBytes = (fileBlocks + 7u) / 8u;
    unsigned char* touched = (unsigned char*) calloc(touchedBytes ? touchedBytes : 1u, 1u);
    if (slots == nullptr || data == nullptr || touched == nullptr) {
        free(slots);
        free(data);
        free(touched);
        fclose(f);
        return 0;
    }
    for (unsigned int set = 0; set < sets; set++) {
        for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
            slots[set * GDX_BLOCK_CACHE_WAYS + w].age = (unsigned char) w;
        }
    }

    c->file = f;
    c->fileSize = (unsigned int) sz;
    c->base = base;
    c->span = span;
    c->blockLog2 = blockLog2;
    c->blockBytes = 1u << blockLog2;
    c->blocks = blocks;
    c->sets = sets;
    c->setMask = sets - 1u;
    c->slots = slots;
    c->data = data;
    c->touched = touched;
    c->touchedBytes = touchedBytes;
    c->uniqueBlocks = 0;
    memset(&c->stats, 0, sizeof(c->stats));
    snprintf(c->path, sizeof(c->path), "%s", path);
    return 1;
}

void gdx_block_cache_close(GdxBlockCache* c) {
    if (c == nullptr) {
        return;
    }
    if (c->file != nullptr) {
        fclose(fh(c));
        c->file = nullptr;
    }
    free(c->slots);
    c->slots = nullptr;
    free(c->data);
    c->data = nullptr;
    free(c->touched);
    c->touched = nullptr;
    c->touchedBytes = 0;
    c->uniqueBlocks = 0;
    c->fileSize = 0;
    c->base = 0;
    c->span = 0;
    c->blocks = 0;
    c->sets = 0;
    c->setMask = 0;
    c->blockBytes = 0;
    c->blockLog2 = 0;
    c->path[0] = '\0';
    /* The overlay hook deliberately SURVIVES a close: it belongs to the consumer's write
     * journal, not to a particular mount, and is installed once. */
}

int gdx_block_cache_active(const GdxBlockCache* c) {
    return (c != nullptr && c->file != nullptr) ? 1 : 0;
}

unsigned int gdx_block_cache_bytes(const GdxBlockCache* c) {
    return (c != nullptr) ? (c->blocks << c->blockLog2) : 0u;
}

unsigned int gdx_block_cache_block_bytes(const GdxBlockCache* c) {
    return (c != nullptr) ? c->blockBytes : 0u;
}

const char* gdx_block_cache_path(const GdxBlockCache* c) {
    return (c != nullptr) ? c->path : "";
}

void gdx_block_cache_set_overlay(GdxBlockCache* c, GdxBlockOverlayFn fn) {
    if (c != nullptr) {
        c->overlay = fn;
    }
}

int gdx_block_cache_read(GdxBlockCache* c, unsigned int offset, unsigned int length, void* dst) {
    if (c == nullptr || c->file == nullptr || dst == nullptr) {
        if (c != nullptr) {
            c->stats.refusals++;
        }
        return 0;
    }
    if (length == 0u) {
        return 1;
    }
    if (offset < c->base ||
        (unsigned long long) offset + length > (unsigned long long) c->base + c->span) {
        c->stats.refusals++;
        return 0;
    }

    /* Two passes. The first only PROVES every block is readable; the second copies. That
     * costs one extra cache lookup per block on the success path and it is what makes the
     * "dst is untouched on refusal" contract in the header true even for a multi-block read
     * whose last block is the one that fails. */
    const unsigned int firstBlock = offset >> c->blockLog2;
    const unsigned int lastBlock = (offset + length - 1u) >> c->blockLog2;
    for (unsigned int b = firstBlock; b <= lastBlock; b++) {
        if (getBlock(c, b, true) == nullptr) {
            c->stats.refusals++;
            return 0;
        }
    }

    unsigned char* out = (unsigned char*) dst;
    unsigned int remaining = length;
    unsigned int pos = offset;
    while (remaining > 0u) {
        const unsigned int block = pos >> c->blockLog2;
        const unsigned int inBlock = pos & (c->blockBytes - 1u);
        unsigned int take = c->blockBytes - inBlock;
        if (take > remaining) {
            take = remaining;
        }
        const unsigned char* src = getBlock(c, block, false);
        if (src == nullptr) {
            /* Only reachable if a block was evicted between the two passes, which cannot
             * happen single-threaded (the proving pass leaves every block of this read in the
             * cache, and a read spans at most length/blockBytes + 1 blocks). Kept as a hard
             * refusal rather than an assert: this runs on a console. */
            c->stats.refusals++;
            return 0;
        }
        memcpy(out, src + inBlock, take);
        out += take;
        pos += take;
        remaining -= take;
    }

    if (c->overlay != nullptr) {
        c->overlay(offset - c->base, length, dst);
    }
    c->stats.reads++;
    c->stats.readBytes += length;
    return 1;
}

void gdx_block_cache_invalidate(GdxBlockCache* c, unsigned int offset, unsigned int length) {
    if (c == nullptr || c->file == nullptr || length == 0u) {
        return;
    }
    if (offset >= c->fileSize) {
        return;
    }
    if ((unsigned long long) offset + length > (unsigned long long) c->fileSize) {
        length = c->fileSize - offset;
    }
    const unsigned int firstBlock = offset >> c->blockLog2;
    const unsigned int lastBlock = (offset + length - 1u) >> c->blockLog2;
    for (unsigned int b = firstBlock; b <= lastBlock; b++) {
        const unsigned int base = (b & c->setMask) * GDX_BLOCK_CACHE_WAYS;
        for (unsigned int w = 0; w < GDX_BLOCK_CACHE_WAYS; w++) {
            if (c->slots[base + w].valid && c->slots[base + w].block == b) {
                c->slots[base + w].valid = 0;
            }
        }
    }
}

int gdx_block_cache_read_uncached(GdxBlockCache* c, unsigned int offset, unsigned int length,
                                  void* dst) {
    if (c == nullptr || c->file == nullptr || dst == nullptr) {
        return 0;
    }
    if (length == 0u) {
        return 1;
    }
    if ((unsigned long long) offset + length > (unsigned long long) c->fileSize) {
        c->stats.refusals++;
        return 0;
    }
    if (fseek(fh(c), (long) offset, SEEK_SET) != 0) {
        c->stats.refusals++;
        return 0;
    }
    const size_t got = fread(dst, 1, length, fh(c));
    c->stats.hostReads++;
    c->stats.hostBytes += got;
    if (got != length) {
        c->stats.refusals++;
        return 0;
    }
    return 1;
}

void gdx_block_cache_note_overlay(GdxBlockCache* c, unsigned int bytes) {
    if (c != nullptr) {
        c->stats.overlayBytes += bytes;
    }
}

void gdx_block_cache_note_refusal(GdxBlockCache* c) {
    if (c != nullptr) {
        c->stats.refusals++;
    }
}

void gdx_block_cache_get_stats(const GdxBlockCache* c, GdxBlockCacheStats* out) {
    if (out != nullptr && c != nullptr) {
        *out = c->stats;
    }
}

void gdx_block_cache_reset_stats(GdxBlockCache* c) {
    if (c != nullptr) {
        memset(&c->stats, 0, sizeof(c->stats));
        /* The working-set bitmap resets WITH the counters, so `refetch = miss - uniq` is
         * always a statement about one window. The 64DD mount calls this right after its 64
         * sample reads, which it has already invalidated -- so the sample does not get counted
         * into the session's working set, and a block the game genuinely re-reads afterwards
         * is charged as new. */
        if (c->touched != nullptr) {
            memset(c->touched, 0, c->touchedBytes);
        }
        c->uniqueBlocks = 0;
    }
}

unsigned long long gdx_block_cache_unique_blocks(const GdxBlockCache* c) {
    return (c != nullptr) ? c->uniqueBlocks : 0ull;
}

const char* gdx_block_cache_format_stats(const GdxBlockCache* c, char* buf, unsigned int bufSize) {
    if (buf == nullptr || bufSize == 0u) {
        return "";
    }
    if (c == nullptr) {
        buf[0] = '\0';
        return buf;
    }
    const unsigned long long lookups = c->stats.blockHits + c->stats.blockMisses;
    const unsigned int hitPct = lookups ? (unsigned int) ((c->stats.blockHits * 100ull) / lookups) : 0u;
    /* wsKB is the WORKING SET (distinct file blocks ever fetched); refetch is what the cache
       failed to hold. Those two, not hit%%, are what choose a cache size: a cache at or above
       wsKB cannot re-fetch, and one below it re-fetches by exactly how badly the access
       pattern re-visits. */
    const unsigned long long refetch =
        (c->stats.blockMisses > c->uniqueBlocks) ? (c->stats.blockMisses - c->uniqueBlocks) : 0ull;
    snprintf(buf, bufSize,
             "reads=%llu readKB=%llu hit=%llu miss=%llu hit%%=%u evict=%llu hostKB=%llu "
             "ovlKB=%llu refused=%llu uniqBlk=%llu wsKB=%llu refetch=%llu cacheKB=%u blockKB=%u",
             c->stats.reads, c->stats.readBytes / 1024ull, c->stats.blockHits,
             c->stats.blockMisses, hitPct, c->stats.blockEvictions, c->stats.hostBytes / 1024ull,
             c->stats.overlayBytes / 1024ull, c->stats.refusals,
             c->uniqueBlocks, (c->uniqueBlocks * c->blockBytes) / 1024ull, refetch,
             gdx_block_cache_bytes(c) / 1024u, c->blockBytes / 1024u);
    return buf;
}

} // extern "C"
