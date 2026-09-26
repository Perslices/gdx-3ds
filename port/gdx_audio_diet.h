/* port/gdx_audio_diet.h -- NEXT.md C4d: take audio_blob/audio_table off the heap.
 *
 * THE CUT. `audio_blob/audio_table` is 10,744,340 B of process-lifetime malloc, the #2 row
 * of the Old 3DS residency census (o3dsbudget-progress.md section 2.1) behind only the 16 MB
 * RDRAM arena. It is STORED (uncompressed) in fzerox.o2r and the game never dereferences it:
 * all three cart sample banks are MEDIUM_CART + CACHEPOLICY_4, so AudioLoad_SyncLoadSampleBank
 * hands back a ROM address instead of loading anything, and every sample byte reaches the
 * synthesiser as a COPY through AudioLoad_DmaSampleData -> osEPiStartDma ->
 * GdxSegmentSourceRead. Residency is therefore a read-through cache with a working set of
 * one whole file. This lever replaces it with a bounded one (port/gdx_o2r_stream.*, which is
 * diskstream's 4-way LRU over a second window, not a second cache).
 *
 * THE CENSUS. Armed independently of the cut, so the resident baseline can be measured with
 * the same instrument. It records what the game actually reads out of the blob: reads, bytes,
 * per-bank split (SFX / BGM / GUITAR, the three gSampleBankTable entries), the set of 16 KiB
 * blocks touched, and -- at level 2 -- an order-independent checksum of every delivered byte.
 *
 * WHY A CENSUS AND NOT A FRAME HASH. `covfinish` deliberately did not add an audio census
 * family: "a SHOT is a framebuffer hash; an audio feature could be recorded but never
 * SAMPLED, so it would read as covered with no gate behind it." A frame hash cannot see a
 * dropped sample, a dropped soundfont or a dropped sequence. The block set and the delivered
 * -bytes checksum can, and they are what this lever is gated on.
 *
 * INI KEYS, all [debug], all default off:
 *   audio_stream        0 off | 1 stream audio_table from the .o2r instead of holding it
 *   audio_stream_cache_kb  default 512
 *   audio_stream_block_kb  default 16
 *   audio_stream_verify 1 = serve every read BOTH ways and memcmp (needs the resident
 *                       payload too, so it costs memory rather than saving it -- it is the
 *                       proof, not the product)
 *   audiodiet           0 off | 1 counters + block set | 2 + delivered-bytes checksum
 */
#ifndef GDX_AUDIO_DIET_H
#define GDX_AUDIO_DIET_H

#ifdef __cplusplus
extern "C" {
#endif

/* The generated-table facts this module is pinned to, duplicated by value exactly as
 * main.cpp/main_3ds.cpp duplicate the audio blob bases (this TU stays out of the decomp
 * include tree). port/gen/AssetBindings.c: { 0x00528730, 0x00A3F1D0, "audio_blob/audio_table" }.
 * The archive entry is that payload plus the 0x40 Torch header and the u32 size field. */
#define GDX_AUDIO_TABLE_ROM_BASE 0x00528730u
#define GDX_AUDIO_TABLE_BYTES 0x00A3F1D0u /* 10,744,272 */
#define GDX_AUDIO_TABLE_ENTRY_KEY "audio_blob/audio_table"
#define GDX_AUDIO_TABLE_PAYLOAD_OFFSET 0x44u
#define GDX_AUDIO_TABLE_ENTRY_BYTES (GDX_AUDIO_TABLE_BYTES + GDX_AUDIO_TABLE_PAYLOAD_OFFSET)

typedef struct GdxAudioDietConfig {
    int stream;        /* arm the cut */
    int verify;        /* serve both ways and compare (implies the payload stays resident) */
    int census;        /* 0 / 1 / 2 */
    unsigned int cacheKb;
    unsigned int blockKb;
} GdxAudioDietConfig;

/* Arms the census and, when cfg->stream is set, opens the stream over the first archive in
 * `paths` that carries the entry STORED. `paths` is scanned in the order given; pass the
 * mount list in REVERSE mount order (ArchiveManager is last-mounted-wins).
 *
 * Returns 1 when the STREAM is armed, 0 otherwise -- including when the census alone is
 * armed. A caller must therefore keep loading the resident payload unless this returns 1, and
 * gdx_audio_diet_streaming() says which happened after the fact. */
int gdx_audio_diet_init(const GdxAudioDietConfig* cfg, const char* const* paths,
                        unsigned int pathCount);

int gdx_audio_diet_streaming(void);  /* 1 = reads are served from the .o2r */
int gdx_audio_diet_verifying(void);  /* 1 = both-ways compare is on */
int gdx_audio_diet_census_level(void);

/* Emits the [audio-diet] receipt through gdx_port_logf. Safe to call at any time and from
 * the census cadence; prints nothing when neither the census nor the stream is armed. */
void gdx_audio_diet_receipt(void);

/* Number of both-ways comparisons that DISAGREED. Must be 0. Non-zero is the specific
 * failure this track exists to catch, and it is printed in the receipt whether or not verify
 * mode is on (so a reader cannot mistake "not measured" for "measured zero" -- the receipt
 * says verify=off in that case). */
unsigned long long gdx_audio_diet_mismatches(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_DIET_H */
