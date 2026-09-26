/* port/gdx_audio_diet.cpp -- implementation. See gdx_audio_diet.h for the why.
 *
 * Three things live here and they are deliberately separable:
 *   - the CENSUS, which measures what the game reads out of audio_blob/audio_table and works
 *     identically whether the bytes come from the resident buffer or the stream. It is the
 *     instrument; arming it changes nothing else.
 *   - the CUT, which serves those reads from the .o2r through a bounded cache.
 *   - the VERIFY, which serves every read BOTH ways and compares. It needs the resident
 *     payload as well as the stream, so it COSTS memory instead of saving it. It is the proof
 *     that the cut is byte-exact, and it is the only check in this port that can see an audio
 *     regression a frame hash cannot.
 */
#include "gdx_audio_diet.h"

#include "gdx_o2r_stream.h"
#include "gdx_segment_source.h"

#include <cstdint>
#include "port_log.h"

#include <cstdio>
#include <cstring>

#if defined(GDX_PLATFORM_3DS)
#include <3ds.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {

/* The census block size is FIXED at 16 KiB and independent of the cache block size, so a
 * census taken at one cache configuration is comparable with one taken at another -- and
 * with the resident baseline, which has no cache at all. */
const unsigned int kCensusBlockLog2 = 14u;
const unsigned int kCensusBlocks = (GDX_AUDIO_TABLE_BYTES + (1u << kCensusBlockLog2) - 1u) >> kCensusBlockLog2;

/* gSampleBankTableData (decomp/src/audio/rom/audio_tables.c), by value: three MEDIUM_CART +
 * CACHEPOLICY_4 banks whose offsets partition the blob exactly (0x2AA90 + 0xA048D0 + 0xFE70
 * == 0xA3F1D0). The split is in the receipt because it is the only per-family breakdown
 * available without a soundfont parse, and because SAMPLE_BGM alone is 97.8% of the blob. */
const unsigned int kBankStart[3] = { 0x0u, 0x2AA90u, 0xA2F360u };
const unsigned int kBankEnd[3] = { 0x2AA90u, 0xA2F360u, 0xA3F1D0u };
const char* const kBankName[3] = { "sfx", "bgm", "guitar" };

GdxAudioDietConfig s_cfg = {};
int s_streaming = 0;

struct Census {
    unsigned long long reads;
    unsigned long long bytes;
    unsigned long long refusals;
    unsigned long long bankBytes[3];
    unsigned long long sum;         /* additive fold of per-read FNV-1a; order-independent */
    unsigned int minOff;
    unsigned int maxOff;
    unsigned int blocksTouched;
    unsigned char block[(kCensusBlocks + 7u) / 8u];
};
Census s_c = {};
unsigned long long s_mismatch = 0;
unsigned long long s_verified = 0;

#if defined(GDX_PLATFORM_3DS)
LightLock s_lock;
bool s_lockReady = false;
void lockTake() {
    if (!s_lockReady) { LightLock_Init(&s_lock); s_lockReady = true; }
    LightLock_Lock(&s_lock);
}
void lockDrop() { LightLock_Unlock(&s_lock); }
#elif defined(_WIN32)
SRWLOCK s_lock = SRWLOCK_INIT;
void lockTake() { AcquireSRWLockExclusive(&s_lock); }
void lockDrop() { ReleaseSRWLockExclusive(&s_lock); }
#else
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
void lockTake() { pthread_mutex_lock(&s_lock); }
void lockDrop() { pthread_mutex_unlock(&s_lock); }
#endif

/* FNV-1a over (offset, length, bytes). The per-read hashes are SUMMED, not XOR-folded:
 * addition is still order-independent -- which matters, because the audio producer is its own
 * thread and its read ORDER is not deterministic across runs even under [debug] det=1 -- but
 * unlike XOR it does not silently cancel a read that happened twice. */
unsigned long long readHash(unsigned int off, unsigned int len, const unsigned char* p) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned char hdr[8] = {
        (unsigned char) off, (unsigned char) (off >> 8), (unsigned char) (off >> 16),
        (unsigned char) (off >> 24), (unsigned char) len, (unsigned char) (len >> 8),
        (unsigned char) (len >> 16), (unsigned char) (len >> 24)
    };
    for (unsigned int i = 0; i < 8u; i++) {
        h = (h ^ hdr[i]) * 1099511628211ull;
    }
    for (unsigned int i = 0; i < len; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

void censusRecord(unsigned int off, unsigned int len, const void* bytes, int ok) {
    if (s_cfg.census <= 0) {
        return;
    }
    lockTake();
    if (!ok) {
        s_c.refusals++;
        lockDrop();
        return;
    }
    if (s_c.reads == 0ull) {
        s_c.minOff = off;
        s_c.maxOff = off + len;
    } else {
        if (off < s_c.minOff) { s_c.minOff = off; }
        if (off + len > s_c.maxOff) { s_c.maxOff = off + len; }
    }
    s_c.reads++;
    s_c.bytes += len;
    for (int b = 0; b < 3; b++) {
        const unsigned int a = off > kBankStart[b] ? off : kBankStart[b];
        const unsigned int z = (off + len) < kBankEnd[b] ? (off + len) : kBankEnd[b];
        if (z > a) {
            s_c.bankBytes[b] += (z - a);
        }
    }
    const unsigned int first = off >> kCensusBlockLog2;
    unsigned int last = (off + len - 1u) >> kCensusBlockLog2;
    if (last >= kCensusBlocks) {
        last = kCensusBlocks - 1u;
    }
    for (unsigned int blk = first; blk <= last && blk < kCensusBlocks; blk++) {
        const unsigned char mask = (unsigned char) (1u << (blk & 7u));
        if ((s_c.block[blk >> 3] & mask) == 0) {
            s_c.block[blk >> 3] |= mask;
            s_c.blocksTouched++;
        }
    }
    if (s_cfg.census >= 2 && bytes != nullptr) {
        s_c.sum += readHash(off, len, (const unsigned char*) bytes);
    }
    lockDrop();
}

/* The resident payload, if it is loaded. Cached after the first successful lookup: the
 * payload pointer is immutable for the process lifetime once published. */
const unsigned char* residentPayload() {
    static const unsigned char* cached = nullptr;
    if (cached != nullptr) {
        return cached;
    }
    void* p = nullptr;
    uint32_t n = 0;
    if (GdxSegmentSourcePayload(GDX_AUDIO_TABLE_ROM_BASE, &p, &n) && p != nullptr &&
        n >= GDX_AUDIO_TABLE_BYTES) {
        cached = (const unsigned char*) p;
    }
    return cached;
}

/* The interposer GdxSegmentSourceRead calls for every read inside the audio_table window.
 * Returns 1 when it served `dst`, 0 to decline (the resident/raw-ROM path then serves and
 * gdx_audio_diet_audit() is called with what it produced). */
int windowRead(uint32_t off, uint32_t len, void* dst) {
    if (!s_streaming) {
        return 0; /* census-only: let the resident path serve, audit afterwards */
    }
    if (!gdx_o2r_stream_read(off, len, dst)) {
        /* Refusal: dst is untouched (gdx_block_cache.h's contract). Declining here lets the
         * resident/raw-ROM fallback try, which is the honest degradation -- and the refusal
         * is counted so a run that silently fell back cannot read as a clean run. */
        lockTake();
        s_c.refusals++;
        lockDrop();
        return 0;
    }
    if (s_cfg.verify) {
        const unsigned char* res = residentPayload();
        if (res != nullptr) {
            lockTake();
            s_verified++;
            if (memcmp(dst, res + off, len) != 0) {
                s_mismatch++;
                const unsigned long long n = s_mismatch;
                lockDrop();
                if (n <= 8ull) {
                    gdx_port_logf("[audio-diet] MISMATCH off=%u len=%u (#%llu)\n", off, len, n);
                }
            } else {
                lockDrop();
            }
        }
    }
    censusRecord(off, len, dst, 1);
    return 1;
}

void windowAudit(uint32_t off, uint32_t len, const void* dst, int ok) {
    censusRecord(off, len, dst, ok);
}

} // namespace

extern "C" {

int gdx_audio_diet_init(const GdxAudioDietConfig* cfg, const char* const* paths,
                        unsigned int pathCount) {
    if (cfg == nullptr) {
        return 0;
    }
    s_cfg = *cfg;
    s_streaming = 0;
    memset(&s_c, 0, sizeof(s_c));
    s_mismatch = 0;
    s_verified = 0;

    if (s_cfg.stream) {
        unsigned int blockLog2 = 14u;
        unsigned int kb = s_cfg.blockKb ? s_cfg.blockKb : 16u;
        blockLog2 = 12u;
        while ((1u << blockLog2) < (kb * 1024u) && blockLog2 < 18u) {
            blockLog2++;
        }
        const unsigned int cacheKb = s_cfg.cacheKb ? s_cfg.cacheKb : 512u;
        unsigned int blocks = (cacheKb * 1024u) >> blockLog2;
        if (blocks < 4u) {
            blocks = 4u;
        }
        s_streaming = gdx_o2r_stream_open(paths, pathCount, GDX_AUDIO_TABLE_ENTRY_KEY,
                                          GDX_AUDIO_TABLE_ENTRY_BYTES,
                                          GDX_AUDIO_TABLE_PAYLOAD_OFFSET, blockLog2, blocks);
        gdx_port_logf("[audio-diet] stream %s (%s) cacheKB=%u blockKB=%u\n",
                      s_streaming ? "armed" : "REFUSED", gdx_o2r_stream_refusal(),
                      gdx_o2r_stream_cache_bytes() / 1024u, (1u << blockLog2) / 1024u);
    }

    if (s_streaming || s_cfg.census > 0) {
        GdxSegmentSourceSetWindow(GDX_AUDIO_TABLE_ROM_BASE, GDX_AUDIO_TABLE_BYTES, windowRead,
                                  s_cfg.census > 0 ? windowAudit : nullptr);
    }
    return s_streaming;
}

int gdx_audio_diet_streaming(void) {
    return s_streaming;
}

int gdx_audio_diet_verifying(void) {
    return (s_streaming && s_cfg.verify) ? 1 : 0;
}

int gdx_audio_diet_census_level(void) {
    return s_cfg.census;
}

unsigned long long gdx_audio_diet_mismatches(void) {
    return s_mismatch;
}

void gdx_audio_diet_receipt(void) {
    if (!s_streaming && s_cfg.census <= 0) {
        return;
    }
    lockTake();
    const Census c = s_c;
    const unsigned long long mism = s_mismatch;
    const unsigned long long ver = s_verified;
    lockDrop();

    gdx_port_logf("[audio-diet] mode=%s census=%d reads=%llu KB=%llu refused=%llu "
                  "blocks=%u/%u blockKB=16 span=[%u,%u] sum=%016llX\n",
                  s_streaming ? (s_cfg.verify ? "stream+verify" : "stream") : "resident",
                  s_cfg.census, c.reads, c.bytes / 1024ull, c.refusals, c.blocksTouched,
                  kCensusBlocks, c.minOff, c.maxOff, c.sum);
    gdx_port_logf("[audio-diet] banks %s=%lluKB %s=%lluKB %s=%lluKB verify=%s compared=%llu "
                  "mismatches=%llu\n",
                  kBankName[0], c.bankBytes[0] / 1024ull, kBankName[1], c.bankBytes[1] / 1024ull,
                  kBankName[2], c.bankBytes[2] / 1024ull,
                  gdx_audio_diet_verifying() ? "on" : "off", ver, mism);
    if (s_streaming) {
        char body[320];
        gdx_port_logf("[audio-diet] cache %s\n", gdx_o2r_stream_format_stats(body, sizeof(body)));
    }
    if (s_cfg.census >= 2) {
        /* The touched-block bitmap, hex, 82 bytes. This is the artefact that answers "did a
         * sample stop being delivered": a sample that is no longer read loses its blocks from
         * this set, and no frame hash anywhere can see that. */
        char hex[(sizeof(c.block) * 2u) + 1u];
        for (unsigned int i = 0; i < sizeof(c.block); i++) {
            static const char* d = "0123456789abcdef";
            hex[i * 2u] = d[(c.block[i] >> 4) & 0xF];
            hex[i * 2u + 1u] = d[c.block[i] & 0xF];
        }
        hex[sizeof(c.block) * 2u] = '\0';
        gdx_port_logf("[audio-diet] blockset %s\n", hex);
    }
}

} // extern "C"
