/* port/gdx_disk_stream.cpp -- the 64DD arm of the block reader.
 * See gdx_disk_stream.h for the why, the contract and the cache-sizing argument.
 *
 * The cache itself now lives in port/gdx_block_cache.{h,cpp}: `audiodiet` needed the same
 * machinery for a second file (the STORED audio_blob/audio_table entry inside fzerox.o2r),
 * and two copies of an LRU cache is how two subtly different LRU caches happen. The state
 * moved into a struct and this file became a ONE-INSTANCE wrapper over it; every semantic
 * below -- the refusal-leaves-dst-untouched contract, the two-pass read, the prove/copy
 * hit accounting, the round-down set count, the wrong-size refusal, the overlay surviving a
 * close -- is unchanged, which is exactly what the 523 checks in
 * port/tests/gdx_disk_stream_tests.cpp re-gate.
 *
 * What stayed here because it is 64DD-specific: the CRC-64/XZ whole-image sweep (same
 * polynomial and reflection as port/disk_savefile.cpp, so a value produced here is
 * interchangeable with one produced there).
 *
 * What moved DOWN into the shared cache with everything else: `ekdrive`'s working-set map
 * (B11d) -- the per-block bitmap behind `uniqBlk`/`wsKB`/`refetch`. It is instrumentation of a
 * cache, not of a 64DD image, and the audio arm needs the same question answered about a file
 * nobody had measured either. `gdx_disk_stream_unique_blocks()` keeps its exact contract and
 * the three checks `ekdrive` added to the disk-stream suite (526 now, up from 523) gate it
 * from here.
 */
#include "gdx_disk_stream.h"

#include "gdx_block_cache.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The wrapper casts between the two stats structs and the two overlay typedefs rather than
 * copying field by field, so make the compiler prove they are the same shape. If a field is
 * ever added to one and not the other, this fails at build time instead of silently
 * mis-reporting a receipt. */
static_assert(sizeof(GdxDiskStreamStats) == sizeof(GdxBlockCacheStats),
              "GdxDiskStreamStats and GdxBlockCacheStats must stay layout-identical");
static_assert(offsetof(GdxDiskStreamStats, refusals) == offsetof(GdxBlockCacheStats, refusals),
              "GdxDiskStreamStats and GdxBlockCacheStats must stay layout-identical");
static_assert(GDX_DISK_STREAM_WAYS == GDX_BLOCK_CACHE_WAYS,
              "the 64DD arm's associativity is the shared cache's");

namespace {

GdxBlockCache s_disk = {};

unsigned long long s_crcTable[256];
bool s_crcReady = false;

void crcInit() {
    if (s_crcReady) {
        return;
    }
    for (unsigned n = 0; n < 256; n++) {
        unsigned long long c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1ull) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
        }
        s_crcTable[n] = c;
    }
    s_crcReady = true;
}

} // namespace

extern "C" {

int gdx_disk_stream_open(const char* path, unsigned int expectedSize,
                         unsigned int blockLog2, unsigned int cacheBlocks) {
    /* base 0 / span 0: the window is the whole image, which is what every 64DD consumer
     * addresses. `expectedSize` keeps its exact meaning (0 = accept any non-zero size). */
    return gdx_block_cache_open(&s_disk, path, 0u, 0u, expectedSize, blockLog2, cacheBlocks);
}

void gdx_disk_stream_close(void) {
    gdx_block_cache_close(&s_disk);
}

int gdx_disk_stream_active(void) {
    return gdx_block_cache_active(&s_disk);
}

unsigned int gdx_disk_stream_size(void) {
    return s_disk.fileSize;
}

unsigned int gdx_disk_stream_cache_bytes(void) {
    return gdx_block_cache_bytes(&s_disk);
}

unsigned int gdx_disk_stream_block_bytes(void) {
    return gdx_block_cache_block_bytes(&s_disk);
}

const char* gdx_disk_stream_path(void) {
    return gdx_block_cache_path(&s_disk);
}

void gdx_disk_stream_set_overlay(GdxDiskOverlayFn fn) {
    gdx_block_cache_set_overlay(&s_disk, (GdxBlockOverlayFn) fn);
}

int gdx_disk_stream_read(unsigned int offset, unsigned int length, void* dst) {
    return gdx_block_cache_read(&s_disk, offset, length, dst);
}

void gdx_disk_stream_invalidate(unsigned int offset, unsigned int length) {
    gdx_block_cache_invalidate(&s_disk, offset, length);
}

int gdx_disk_stream_read_uncached(unsigned int offset, unsigned int length, void* dst) {
    return gdx_block_cache_read_uncached(&s_disk, offset, length, dst);
}

unsigned long long gdx_disk_stream_crc64(int* ok) {
    if (ok != nullptr) {
        *ok = 0;
    }
    if (!gdx_block_cache_active(&s_disk)) {
        return 0ull;
    }
    crcInit();
    /* Cache-bypassing on purpose: a whole-image sweep would evict every useful block and
     * gain nothing, since no byte is revisited. Reads straight into a scratch chunk. */
    const unsigned int chunkBytes = gdx_block_cache_block_bytes(&s_disk);
    unsigned char* chunk = (unsigned char*) malloc(chunkBytes);
    if (chunk == nullptr) {
        return 0ull;
    }
    const unsigned int size = s_disk.fileSize;
    unsigned long long crc = 0xFFFFFFFFFFFFFFFFull;
    unsigned int done = 0;
    while (done < size) {
        unsigned int want = chunkBytes;
        if (size - done < want) {
            want = size - done;
        }
        if (!gdx_block_cache_read_uncached(&s_disk, done, want, chunk)) {
            free(chunk);
            return 0ull;
        }
        for (unsigned int i = 0; i < want; i++) {
            crc = s_crcTable[(crc ^ chunk[i]) & 0xFFull] ^ (crc >> 8);
        }
        done += want;
    }
    free(chunk);
    crc ^= 0xFFFFFFFFFFFFFFFFull;
    if (ok != nullptr) {
        *ok = 1;
    }
    return crc;
}

void gdx_disk_stream_note_overlay(unsigned int bytes) {
    gdx_block_cache_note_overlay(&s_disk, bytes);
}

void gdx_disk_stream_get_stats(GdxDiskStreamStats* out) {
    gdx_block_cache_get_stats(&s_disk, (GdxBlockCacheStats*) out);
}

void gdx_disk_stream_reset_stats(void) {
    gdx_block_cache_reset_stats(&s_disk);
}

unsigned long long gdx_disk_stream_unique_blocks(void) {
    return gdx_block_cache_unique_blocks(&s_disk);
}

const char* gdx_disk_stream_format_stats(char* buf, unsigned int bufSize) {
    return gdx_block_cache_format_stats(&s_disk, buf, bufSize);
}

} // extern "C"
