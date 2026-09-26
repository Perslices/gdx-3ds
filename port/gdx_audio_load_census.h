/* [audioload] -- producer-tick PHASE ATTRIBUTION for the audio loader (audioasync track).
 *
 * WHY. `audioring` made crackle a number (`sil_ppk`) and settled the AXIS: at an Expansion Kit
 * stall the producer's TICK histogram gains a >=64 ms entry while its GAP histogram does not
 * move, so the producer is blocked INSIDE gdx_audio_produce_one_tick holding sAudioCtxMutex,
 * not descheduled. The user's console log says the same thing in one field:
 * `tick{in=1 for=92006us ... max=112008us}`. What neither answers is WHICH CALL inside that
 * tick spends the time -- and the obvious suspect is not safe to assume, because audioring's
 * emulator runs recorded ZERO `AudioLoad_DiskDrive ENTER` lines while still producing 1.2 s
 * ticks. A fix aimed at the wrong call site would measure as "no change" and teach nothing.
 *
 * WHAT IT IS. A fixed set of nested scopes around the loader calls reachable from the producer
 * tick. Scopes NEST and their totals therefore OVERLAP by construction (cmds > heapinit >
 * preload > disk); that is the point -- the nesting is what localises the cost. Per-tick µs are
 * reported for any tick over `[debug] audioload_ms` (default 32), beside session totals.
 *
 * COST WHEN OFF. `gdx_aload_begin()` returns 0 on one int compare and `gdx_aload_end()` returns
 * on `t0 == 0`. No allocation, no lock, no clock read. `[debug] audioload` defaults to 0.
 *
 * This header is DECLARATIONS ONLY and includes nothing, so it is safe inside the decomp's
 * headers-only translation units (same contract as port/gdx_disk_stream.h).
 */
#ifndef GDX_AUDIO_LOAD_CENSUS_H
#define GDX_AUDIO_LOAD_CENSUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Site ids. Append only -- the names table in port/n64_sched.c is indexed by these. */
#define GDX_ALOAD_CMDS 0     /* AudioThread_ProcessCmds: the whole command drain            */
#define GDX_ALOAD_HEAPINIT 1 /* AudioHeap_Init: spec change / heap reset                     */
#define GDX_ALOAD_LOADS 2    /* AudioLoad_ProcessLoads: slow + sample-preload + async pumps  */
#define GDX_ALOAD_SYNCSAMP 3 /* AudioLoad_SyncLoadSample: one sample, on demand              */
#define GDX_ALOAD_RELOC 4    /* AudioLoad_RelocateFontAndPreloadSamples                      */
#define GDX_ALOAD_PRELOAD 5  /* AudioLoad_PreloadSamplesForFont                              */
#define GDX_ALOAD_DISK 6     /* AudioLoad_SyncDiskDrive -> AudioLoad_DiskDrive (blocking)    */
#define GDX_ALOAD_DMA 7      /* AudioLoad_SyncDma                                            */
#define GDX_ALOAD_FONTCONV 8 /* gdx_audio_convert_font (the port's font converter)           */
#define GDX_ALOAD_SLOWP 9    /* AudioLoad_ProcessSlowLoads                                  */
#define GDX_ALOAD_PRELP 10   /* AudioLoad_ProcessSamplePreloads                             */
#define GDX_ALOAD_ASYNCP 11  /* AudioLoad_ProcessAsyncLoads                                 */
#define GDX_ALOAD_DISKLD 12  /* AudioLoad_DiskLoad: one async disk state-machine step        */
#define GDX_ALOAD_LEORW 13   /* AudioLoad_ReadWriteDisk -> sLeoHandler: the device read      */
#define GDX_ALOAD_SITES 14

/* Returns a start stamp in microseconds, or 0 when the census is off. A 0 return is the
   "do nothing" token gdx_aload_end() tests, so a caller never needs its own guard. The stamp is
   biased by 1 µs so a genuine zero reading cannot masquerade as "off". */
unsigned int gdx_aload_begin(void);
void gdx_aload_end(int site, unsigned int startStamp);

/* Producer-tick bracket (port/n64_sched.c, gdx_audio_produce_one_tick). */
void gdx_aload_tick_begin(void);
void gdx_aload_tick_end(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_AUDIO_LOAD_CENSUS_H */
