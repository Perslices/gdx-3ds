#pragma once
/* Single byte-source shim for venue/geometry blob migration.
 *
 * Every chokepoint that reads the cartridge image routes through here rather than
 * doing `memcpy(dst, gdx_rom_buffer + romBase, size)` itself, so one place decides
 * where the bytes come from:
 *   1. archive-first  -- the generated segment_blob table (containment lookup),
 *   2. raw-ROM fallback -- gdx_rom_buffer, byte-identical to the old direct read.
 *
 * Blob entries are verbatim ROM slices: MIO0 families stay compressed,
 * uncompressed families stay big-endian. Every consumer keeps its existing
 * decode/swap step, so the result is provably byte-equal to the pre-shim path.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `size` bytes of the cartridge image starting at absolute ROM offset
 * `romBase` into `dst`. Resolves archive-first (blob whose span fully contains
 * [romBase, romBase+size)), else copies from the raw ROM image. Returns 1 on
 * success (either source), 0 only when no source can satisfy the read (ROM image
 * absent or the range is out of bounds) -- in which case `dst` is left untouched
 * and the caller applies its own miss policy (matching the old bounds check). */
int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst);

/* Span of the archive blob (if any) whose verbatim ROM slice contains `romBase`,
 * measured from `romBase` to the blob's end. A caller that must stage a whole
 * family span whose byte length is not known a priori (an MIO0 stream: the
 * compressed length is not carried in the header) uses this to size its staging
 * read to the family blob, so the staged `GdxSegmentSourceRead` stays
 * archive-first (fully contained) instead of resolving to the raw-ROM fallback.
 * Writes `*outSpan` and returns 1 when a blob contains `romBase`; returns 0
 * otherwise (no matching blob -- the caller bounds its stage by other means).
 * Pure table lookup: no lock, no allocation, no ROM read. */
int GdxSegmentSourceContainingSpan(uint32_t romBase, uint32_t* outSpan);

/* Telemetry: total raw-ROM fallback reads across every family plus the unmapped
 * bucket. Target 0 once the archive is complete. */
unsigned int gdx_segment_source_fallback_total(void);

/* Per-family telemetry iterator: call with index 0,1,2,... until it returns 0 (one past
 * the last populated family). On a hit it writes the family's archive key to *outKey and
 * its raw-ROM fallback count to *outFallbackReads (either may be NULL) and returns 1. The
 * unmapped bucket is NOT a family and shows up only in gdx_segment_source_fallback_total().
 * Each index is a snapshot; counts may advance between calls under concurrent reads. */
int GdxSegmentSourceFamilyStats(unsigned int index, const char** outKey,
                                unsigned int* outFallbackReads);

/* O3DS BUDGET residency census (port/3ds/gdx3ds_membudget.cpp). Same [0, familyCount)
 * enumeration as GdxSegmentSourceFamilyStats, but reporting BYTES rather than fallbacks:
 * *outPayloadBytes is the usable payload span and *outAllocBytes the whole-file buffer
 * the loader actually holds (payload + the 0x44 archive framing), or 0 for a family that
 * has been discovered but never loaded. Every one of these buffers is process-lifetime,
 * so a non-zero *outAllocBytes is resident for the rest of the run. Any out pointer may
 * be NULL; returns 1 while `index` names a populated slot. */
int GdxSegmentSourceFamilyResidency(unsigned int index, const char** outKey,
                                    unsigned int* outPayloadBytes,
                                    unsigned int* outAllocBytes);

/* Boot-time preload: forces the lazy archive load of the blob family whose verbatim ROM
 * slice contains `romBase`, so the payload is resident before the first audio DMA rather
 * than loading on a game/audio tick. Returns 1 when the payload is resident from the
 * archive, 0 when the family is absent or the load failed -- reads then degrade silently
 * to the raw-ROM fallback. Idempotent: a second call returns the cached state. */
int GdxSegmentSourcePreload(uint32_t romBase);

/* Boot-time split of GdxSegmentSourcePreload for hosts that run the (long, SD-bound)
 * archive read on a background worker: pre-allocates the family's process-lifetime
 * staging buffer WITHOUT touching the archive and hands back the payload view + its
 * capacity, so the address can be gdx_register_host_range()'d on the MAIN thread (the
 * host-range vector is not thread-safe) before the worker calls GdxSegmentSourcePreload.
 * Idempotent; an already-loaded family returns its live payload/size. Returns 0 when the
 * family is unmapped, already failed, or the allocation failed. */
int GdxSegmentSourcePreallocPayload(uint32_t romBase, void** outPayload, uint32_t* outCap);

/* C4d WINDOW INTERPOSER (port/gdx_audio_diet.cpp). Declares that reads landing fully inside
 * [romBase, romBase+span) should be offered to `readFn` FIRST, with a family-relative offset.
 *
 * `readFn` returns 1 when it served `dst` in full, or 0 to decline -- on which the ordinary
 * resident/raw-ROM path serves and `auditFn` (may be NULL) is called with the same relative
 * offset, the destination, and whether that path succeeded. The decline arm is what lets one
 * census instrument measure both the resident baseline and the streamed build.
 *
 * ON REFUSAL `readFn` MUST LEAVE `dst` UNTOUCHED, so that a decline after a partial write
 * cannot hand the fallback a half-filled destination. Arming is boot-time and one-shot; the
 * always-on cost on the read path is a single NULL compare. */
typedef int (*GdxSegWindowReadFn)(uint32_t relOffset, uint32_t size, void* dst);
typedef void (*GdxSegWindowAuditFn)(uint32_t relOffset, uint32_t size, const void* dst, int ok);
void GdxSegmentSourceSetWindow(uint32_t romBase, uint32_t span, GdxSegWindowReadFn readFn,
                               GdxSegWindowAuditFn auditFn);

/* A12b BOOT-WINDOW IN-FLIGHT BYPASS (docs/research/native/bootstall-progress.md).
 *
 * seg_ensure_loaded's in-flight wait is all-or-nothing: a reader arriving while another
 * thread holds the family mid-load sleeps until the WHOLE payload is published. For
 * audio_blob/audio_table that is a 10.7 MB archive read, and the waiter is the audio producer
 * asking for six small DMAs -- which also holds sAudioCtxMutex, so the GAME fiber stalls
 * behind it in AudioThread_QueueCmd. This declares an ALTERNATIVE BYTE SOURCE for
 * [romBase, romBase+span) to be offered any read that arrives before the payload is
 * published, so such a read is served in milliseconds instead of waiting out the load.
 *
 * `readFn` takes a family-relative offset and returns 1 when it served `dst` in full, or 0 to
 * decline -- on which the ordinary in-flight wait runs exactly as before. ON REFUSAL IT MUST
 * LEAVE `dst` UNTOUCHED (gdx_block_cache.h's contract). It MUST serve bytes identical to the
 * resident payload; it is consulted on a race, so "correct only while in flight" is not a
 * property this seam can offer.
 *
 * SELF-DISARMING: the hook sits after the lock-free published-payload fast path, so once the
 * loader publishes, no read reaches it again. The always-on cost when unarmed is one NULL
 * compare. Pass readFn = NULL to retract, then GdxSegmentSourceDrainBypass() to wait out any
 * reader already inside it before closing the underlying source. */
typedef int (*GdxSegBypassReadFn)(uint32_t relOffset, uint32_t size, void* dst);
void GdxSegmentSourceSetBypass(uint32_t romBase, uint32_t span, GdxSegBypassReadFn readFn);
void GdxSegmentSourceDrainBypass(void);

/* Receipt counters: reads served by the bypass instead of waiting, their bytes, and reads the
 * bypass declined (which fell through to the ordinary wait). Any pointer may be NULL. */
void GdxSegmentSourceBypassStats(unsigned int* outServed, unsigned int* outBytes,
                                 unsigned int* outDeclined);

/* Millisecond monotonic clock backing the [seg-src]/[audio-load] load receipts; exported
 * so decomp-side diag code can time loads without platform headers. */
double gdx_host_ms_now(void);

/* Companion getter to GdxSegmentSourcePreload: hands back the resident,
 * immutable (process-lifetime) payload view + its byte length for the blob
 * family containing `romBase`, so a caller can register it with the host-range
 * marshaller (truncated-low32 tokens of blob-served buffers must resolve exactly
 * like gAudioHeap/gdx_rom_buffer). Writes outPayload and outSize, and
 * returns 1 only when the family payload is resident; returns 0 (out-params
 * untouched) when the family is unmapped or not loaded from the archive. */
int GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);

#ifdef __cplusplus
}
#endif
