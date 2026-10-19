/* port/gdx_block_cache.h -- the set-associative, file-backed block reader.
 *
 * WHY THIS FILE EXISTS. `diskstream` wrote this cache to stream the 64DD image off SD
 * (port/gdx_disk_stream.{h,cpp}, 523 host checks in tools/ci-3ds.sh). `audiodiet` needs the
 * same machinery for a second file -- the STORED `audio_blob/audio_table` entry inside
 * fzerox.o2r, 10,744,272 B held resident for the process lifetime and the #2 row of the Old
 * 3DS residency census. Two copies of an LRU cache is how two subtly different LRU caches
 * happen, so the state was lifted out of gdx_disk_stream.cpp's file statics into this struct
 * and gdx_disk_stream became a one-instance wrapper over it. The disk lever's existing 523
 * checks therefore gate THIS code unchanged -- that is the point of doing it this way round.
 *
 * WHAT IT IS. Read-only, seekable, over a WINDOW [base, base+span) of a file, plus a
 * fixed-size block cache: 4-way set-associative, LRU within the set, blocks indexed on
 * ABSOLUTE file offset so the window's alignment is whatever the file's is. Allocates
 * exactly twice (tags, data arena), both at open, never grows. <cstdio>/<cstdlib>/<cstring>
 * only: no decomp, no 3DS, no libultraship, no containers on the read path -- which is what
 * lets it compile for the host and be gated in CI.
 *
 * WHAT IT IS NOT. Not thread-safe and deliberately not: gdx_disk_stream is single-consumer,
 * and gdx_o2r_stream (the audio arm) is read by the audio producer thread and takes its own
 * lock around these calls. Not writable; a write-through consumer composes its own bytes in
 * through the overlay hook.
 *
 * THE CONTRACT THAT MATTERS, unchanged from diskstream and restated because it is the whole
 * reason for the two-pass read: ON REFUSAL `dst` IS NOT WRITTEN AT ALL -- not zeroed, not
 * partially filled. See ekdeadlock-progress.md section 6, where a bzero of a destination
 * derived from data just declared unloadable overwrote the receipt string meant to report it.
 */
#ifndef GDX_BLOCK_CACHE_H
#define GDX_BLOCK_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_BLOCK_CACHE_WAYS 4

/* READ-AHEAD stream slots. The audio consumer interleaves several sequential walks at once --
 * one SampleDma per voice per audio frame -- so a single "was the last fill block B-1" test
 * detects almost nothing. Four slots is enough to carry the concurrent voices the synthesiser
 * actually keeps walking, and costs four int compares on the MISS path only. */
#define GDX_BLOCK_CACHE_SEQ_SLOTS 4

typedef struct GdxBlockCacheStats {
    unsigned long long reads;          /* read() calls that were served */
    unsigned long long readBytes;      /* bytes those calls asked for */
    unsigned long long blockHits;      /* block lookups served from cache */
    unsigned long long blockMisses;    /* block lookups that went to the file */
    unsigned long long blockEvictions; /* a valid block replaced by another */
    unsigned long long hostReads;      /* fseek/fread pairs issued */
    unsigned long long hostBytes;      /* bytes pulled off the file */
    unsigned long long overlayBytes;   /* bytes rewritten by the overlay */
    unsigned long long refusals;       /* out-of-range or short-read refusals */
} GdxBlockCacheStats;

/* Called AFTER the base bytes land in `dst`, with the WINDOW-RELATIVE offset (offset - base)
 * and the same length, so a consumer's pending writes win over the file's pristine bytes. */
typedef void (*GdxBlockOverlayFn)(unsigned int offset, unsigned int length, void* dst);

typedef struct GdxBlockCacheSlot {
    unsigned int block;  /* absolute-file block index; valid only when `valid` */
    unsigned char valid;
    unsigned char age;   /* LRU rank inside the set: 0 = most recent */
} GdxBlockCacheSlot;

/* Declared in the header so instances can be file statics -- no allocation for the struct. */
typedef struct GdxBlockCache {
    void* file;                  /* FILE* */
    unsigned int fileSize;
    unsigned int base;           /* window start, absolute file offset */
    unsigned int span;           /* window length */
    unsigned int blockLog2;
    unsigned int blockBytes;
    unsigned int blocks;
    unsigned int sets;
    unsigned int setMask;
    GdxBlockCacheSlot* slots;
    unsigned char* data;
    /* WORKING-SET MAP (`ekdrive`, B11d): one bit per FILE block, set the first time that block
     * is fetched. See gdx_block_cache_unique_blocks(). */
    unsigned char* touched;
    unsigned int touchedBytes;
    unsigned long long uniqueBlocks;
    /* Every fillSlot(), demand or speculative. Kept OUT of GdxBlockCacheStats on purpose:
     * gdx_disk_stream.h mirrors that struct and gdx_disk_stream.cpp static_asserts the two
     * layouts identical, so growing it would ripple into the 64DD wrapper for no reason. */
    unsigned long long blockFills;
    unsigned long long prefetchFills;
    /* READ-AHEAD ([debug] audio_stream_readahead, default 0 = the unchanged path). */
    unsigned int readAhead;
    unsigned int seqTail[GDX_BLOCK_CACHE_SEQ_SLOTS];
    unsigned char seqValid[GDX_BLOCK_CACHE_SEQ_SLOTS];
    unsigned char seqNext;
    GdxBlockOverlayFn overlay;
    GdxBlockCacheStats stats;
    char path[512];
} GdxBlockCache;

/* Opens `path` and arms the cache over [base, base+span) of it. `expectedFileSize` is the
 * exact size the file must have (0 = accept any non-zero size); a file too short to contain
 * the window is REFUSED here rather than read through. `blockLog2` is clamped to 12..18 and
 * falls back to the default outside it; `cacheBlocks` is clamped to
 * [GDX_BLOCK_CACHE_WAYS, 4096] and its SET COUNT rounded DOWN to a power of two (the set
 * index is a mask, not a division, because this runs on a 268 MHz ARM11), so the caller
 * never gets a larger cache than it asked for -- gdx_block_cache_bytes() reports the truth.
 *
 * Returns 1 on success; 0 with nothing allocated and the cache left inactive otherwise. */
int gdx_block_cache_open(GdxBlockCache* c, const char* path, unsigned int base,
                         unsigned int span, unsigned int expectedFileSize,
                         unsigned int blockLog2, unsigned int cacheBlocks);

void gdx_block_cache_close(GdxBlockCache* c);
int gdx_block_cache_active(const GdxBlockCache* c);
unsigned int gdx_block_cache_bytes(const GdxBlockCache* c);
unsigned int gdx_block_cache_block_bytes(const GdxBlockCache* c);
const char* gdx_block_cache_path(const GdxBlockCache* c);
void gdx_block_cache_set_overlay(GdxBlockCache* c, GdxBlockOverlayFn fn);

/* Reads [offset, offset+length) -- ABSOLUTE file offsets, and the range must lie inside the
 * window -- into `dst`, crossing block boundaries as needed, then applies the overlay.
 * Returns 1 on full success, 0 on refusal with `dst` untouched (see the header note). */
int gdx_block_cache_read(GdxBlockCache* c, unsigned int offset, unsigned int length, void* dst);

/* QUICKFIX: 1 if gdx_block_cache_read(c, offset, length, ..) MAY touch the file -- a block of the
 * range is not resident, or read-ahead is on and one of the `readAhead` blocks after the range
 * is not (a superset of what noteSequential would fetch). 0 means the read is served from
 * memory. A pure lookup: no LRU touch, no statistics. The caller holds whatever lock guards `c`
 * across this call and the read. */
int gdx_block_cache_would_fill(const GdxBlockCache* c, unsigned int offset, unsigned int length);

/* Drops any cached blocks covering [offset, offset+length) (absolute file offsets). */
void gdx_block_cache_invalidate(GdxBlockCache* c, unsigned int offset, unsigned int length);

/* Reads straight off the file into `dst`, bypassing and not disturbing the cache. For whole
 * -file sweeps (a CRC pass) where no byte is revisited and filling the cache would only evict
 * everything useful. Counts hostReads/hostBytes; returns 1 on success. */
int gdx_block_cache_read_uncached(GdxBlockCache* c, unsigned int offset, unsigned int length,
                                  void* dst);

/* The number of DISTINCT file blocks fetched since the last stats reset -- the working set,
 * which is the quantity that actually sizes a cache. `blockMisses - this` is the RE-FETCH
 * count: the blocks the file had to serve twice because the cache could not hold the set. A
 * cache at or above the working set cannot re-fetch at all, so a sweep that chooses a cache
 * size reads these two numbers and nothing else -- not hit%%, which mixes first-touch traffic
 * (irreducible) with re-fetch (the only part capacity can buy back).
 *
 * `ekdrive` built this for the 64DD arm against a real image (B11d: 785 LEO reads, 854
 * lookups, a working set of 89 blocks, and a 128 KiB -> 4 MiB sweep that moved the card
 * traffic by 384 KB in total). C4d moved it down here when the cache became shared, so the
 * audio arm gets the same instrument: `audio_table` is read by the audio producer thread in
 * a pattern nobody had measured either.
 *
 * Cost: one bit per file block (496 B for a 64DD image, 131 B for an .o2r at 16 KiB blocks),
 * one allocation at open, one bit test-and-set per MISS and never on the hit path. That is
 * cheap enough to leave always on, and a lever that can only be read with a special build is
 * a lever nobody reads. */
unsigned long long gdx_block_cache_unique_blocks(const GdxBlockCache* c);

/* READ-AHEAD. `blocks` = how many blocks past the end of a SEQUENTIAL read to fetch
 * speculatively; 0 (the default, and what every existing caller gets) is the unchanged path
 * and not one instruction of this runs.
 *
 * WHY IT EXISTS, and why capacity is not the same lever. `diskstream` section 3 named
 * read-ahead as the right shape for audio and nobody built it: the synthesiser walks a
 * sample's loop region forward in ~760-byte DMAs, so a 16 KiB block serves ~20 of them and
 * then the NEXT block is a certain miss. On the emulator a miss is a host-directory read and
 * costs almost nothing, which is exactly why this was not caught before it shipped; on a real
 * SD card it is a seek. The quantity this moves is therefore the FILL COUNT (`pf=` and
 * `hostKB` in the receipt), not a millisecond figure any emulator lane can produce.
 *
 * SAFETY. Speculation happens AFTER a read has copied its bytes out, never inside the
 * prove-then-copy walk, so it can never evict a block the read in flight still needs -- the
 * "dst untouched on refusal" contract is unaffected. A block already cached is left alone and
 * does not even take an LRU touch. */
void gdx_block_cache_set_readahead(GdxBlockCache* c, unsigned int blocks);
unsigned int gdx_block_cache_readahead(const GdxBlockCache* c);

/* Speculative fills issued since the last stats reset. `blockFills` is every fill, demand or
 * speculative; with read-ahead off the two are `blockMisses` and 0. */
unsigned long long gdx_block_cache_prefetch_fills(const GdxBlockCache* c);
unsigned long long gdx_block_cache_fills(const GdxBlockCache* c);

void gdx_block_cache_note_overlay(GdxBlockCache* c, unsigned int bytes);
void gdx_block_cache_get_stats(const GdxBlockCache* c, GdxBlockCacheStats* out);
void gdx_block_cache_reset_stats(GdxBlockCache* c);
void gdx_block_cache_note_refusal(GdxBlockCache* c);

/* Formats the stats into `buf` as a one-line receipt body. Returns `buf`. */
const char* gdx_block_cache_format_stats(const GdxBlockCache* c, char* buf, unsigned int bufSize);

#ifdef __cplusplus
}
#endif

#endif /* GDX_BLOCK_CACHE_H */
