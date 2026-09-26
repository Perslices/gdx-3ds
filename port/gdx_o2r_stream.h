/* port/gdx_o2r_stream.h -- serve ONE archive entry from the .o2r file instead of RAM.
 *
 * WHY (NEXT.md C4d). `audio_blob/audio_table` is the #2 row of the Old 3DS residency census:
 * 10,744,340 B of whole-file buffer, malloc'd once by port/gdx_segment_source.c and never
 * freed ("the whole file buffer stays alive"). On the memtype-0 Old 3DS grant of 25,165,824 B
 * the build does not boot at all, and even with C4b's linear-heap override at 6 MiB the
 * canonical replay passes with 1.00 MB of heap to spare -- a demonstration, not a margin
 * (o3dssim-progress.md section 4.3).
 *
 * WHY IT CAN BE STREAMED AT ALL, which is the finding this module rests on:
 *
 *   1. `audio_blob/audio_table` is STORED in the .o2r, not deflated. Every other blob is
 *      method 8; this one is method 0, compressed size == uncompressed size == 10,744,340.
 *      So any byte of it is one fseek away, with no inflate and no restart from the stream
 *      head. (main_3ds.cpp's note that the preload worker "INFLATES the 10.7 MB Deflate-
 *      stored audio_table" reads the phrase the wrong way round: it is STORED inside a
 *      Deflate-capable container. The 4.3 s is the SD read, not an inflate.)
 *
 *   2. The game never dereferences it. All three cart sample banks are MEDIUM_CART +
 *      CACHEPOLICY_4, and AudioLoad_SyncLoadSampleBank returns the ROM address for that
 *      combination instead of loading anything (decomp/src/audio/rom/lib/load.c:512). Every
 *      sample byte then reaches the synthesiser through AudioLoad_DmaSampleData -> the
 *      SampleDma ring -> osEPiStartDma -> GdxSegmentSourceRead, i.e. a COPY into gAudioHeap.
 *      Residency is a read-through cache with a 10.25 MB working set of 1, nothing more.
 *
 * WHAT THIS IS. A second instance of port/gdx_block_cache.{h,cpp} -- diskstream's 4-way LRU,
 * not a new one -- plus a minimal zip locator that finds the entry, REFUSES anything that is
 * not STORED, and hands the cache the [dataOffset, +size) window. Reads are payload-relative
 * (the 0x40 Torch header + the u32 size field at 0x40 are folded into the window base), so a
 * caller asks for the same offsets the resident payload served.
 *
 * WHAT IT IS NOT. Not general: one entry at a time, read-only, no deflate. A deflated entry,
 * a missing entry, a size that disagrees with the generated table, or a file that cannot be
 * opened all REFUSE at open, and the caller keeps the resident path. There is no half-state.
 */
#ifndef GDX_O2R_STREAM_H
#define GDX_O2R_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Scans `paths` in the order given and arms the stream on the FIRST file that carries
 * `entryKey` as a STORED entry of exactly `expectedEntryBytes` bytes (the whole archive
 * entry: 0x40 header + 4 size bytes + payload, i.e. what the .o2r holds). The caller passes
 * the mount list in REVERSE mount order, because ArchiveManager is last-mounted-wins.
 *
 * `payloadOffset` is folded into the window base, so gdx_o2r_stream_read() takes offsets
 * relative to the payload, matching GdxSegmentSourceRead's family-relative addressing.
 *
 * Returns 1 when the stream is armed. Returns 0 -- with nothing allocated, nothing open and
 * the stream inactive -- on every refusal; `gdx_o2r_stream_refusal()` then names which one. */
int gdx_o2r_stream_open(const char* const* paths, unsigned int pathCount, const char* entryKey,
                        unsigned int expectedEntryBytes, unsigned int payloadOffset,
                        unsigned int blockLog2, unsigned int cacheBlocks);

void gdx_o2r_stream_close(void);
int gdx_o2r_stream_active(void);

/* A short, stable reason string for the last open(): "ok", "no-path", "no-entry",
 * "not-stored", "size-mismatch", "open-failed", "cache-failed", "not-attempted". Logged in
 * the receipt so a refusal on hardware is one grep, not a bisect. */
const char* gdx_o2r_stream_refusal(void);

const char* gdx_o2r_stream_source(void);      /* the .o2r path that won, or "" */
unsigned int gdx_o2r_stream_data_offset(void); /* absolute file offset of the PAYLOAD */
unsigned int gdx_o2r_stream_payload_bytes(void);
unsigned int gdx_o2r_stream_cache_bytes(void);

/* Reads [offset, offset+length) of the entry PAYLOAD into `dst`. Returns 1 on full success;
 * 0 on refusal, with `dst` untouched (gdx_block_cache.h's contract, inherited).
 *
 * Thread safety: this one takes a lock, because gdx_disk_stream's single-consumer assumption
 * does not hold here -- the audio producer thread is the hot caller and the boot worker and
 * game thread can both arrive during the first race load. */
int gdx_o2r_stream_read(unsigned int offset, unsigned int length, void* dst);

/* One-line receipt body: the cache stats plus the source and window. Returns `buf`. */
const char* gdx_o2r_stream_format_stats(char* buf, unsigned int bufSize);

#ifdef __cplusplus
}
#endif

#endif /* GDX_O2R_STREAM_H */
