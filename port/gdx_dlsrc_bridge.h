/* port/gdx_dlsrc_bridge.h -- [native] interp: command-source exports of the graphics bridge.
 *
 * The owned F3DEX2 interpreter (port/3ds/gfx/n64/, docs/research/native/06-interp-plan.md)
 * reads the game's own Gfx stream instead of the bridge's converted copy. What it still needs
 * from the bridge is ADDRESS RESOLUTION: the segment table view of the running job, the
 * placeholder-stub map, the registered range tables (wide / host-N64 / raw big-endian), the
 * mode-owned segment 9 switch and the venue-bank aliases. Those bodies stay in
 * n64_gfx_bridge.cpp until the M5 deletion moves them into gdx_dlsrc (plan §2.6); this header
 * is the seam. Every function takes the job so it resolves against the job's private segment
 * view exactly as the bridge walk does (gdx_gfx_segment_view_set is installed by the caller).
 *
 * All functions are safe to call from the render thread inside gdx_gfx_job_run. */
#ifndef GDX_DLSRC_BRIDGE_H
#define GDX_DLSRC_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxGfxJob GdxGfxJob;

/* One resolved display-list source. stride 16 = host-built wide packet (w0 @0, w1lo @8,
 * w1hi @12, host-endian); stride 8 = N64 packet (w0 @0, w1 @4). isBig = the 8-byte words are
 * big-endian (untagged raw ROM slice, sniffed). isF3d = legacy F3D dialect (segment-3 machine
 * parts): opcodes 0x01/0x03/0x04/0x06/0xB6..0xBF carry F3D meanings. */
typedef struct GdxDlSrcList {
    const void* ptr;
    size_t limit; /* commands readable from ptr (bound, not a terminator scan) */
    uint8_t stride;
    uint8_t isBig;
    uint8_t isF3d;
    uint8_t reserved;
} GdxDlSrcList;

/* The task's root list (the job's dl/dlSize), with the bridge's root-validation failsafe
 * applied: returns 0 when the root does not validate (render nothing this task). */
int gdx_gfx_job_root_list(GdxGfxJob* job, GdxDlSrcList* out);

/* Sub-list target of G_DL / G_BRANCH_Z. hostTagged = the operand carried the GDX1 host tag and
 * is not a placeholder/BSS alias (see gdx_gfx_low32_needs_resolver); hostFull = that pointer.
 * Returns 0 when the target does not resolve or does not validate as a display list (the
 * bridge routes those to a no-op list; the caller should skip the call). */
int gdx_gfx_job_resolve_list(GdxGfxJob* job, uint32_t raw, int hostTagged, uintptr_t hostFull,
                             GdxDlSrcList* out);

/* Data operand (VTX / MTX / MOVEMEM / DMA_IO / SETTIMG / SETCIMG / SETZIMG / LOADTLUT
 * sources). requiredBytes = the full payload the caller will read; the resolver's readability
 * gates reject short matches. Returns the host pointer or 0 (unresolved, or a segment reload
 * raced the lookup -- the caller applies its opcode-specific fallback). The pointer has the
 * bridge's odd-bit normalisation applied. preferPhysical/sourceHint mirror the bridge's
 * VTX/MTX arguments (sourceHint = the referencing list's host address). */
uintptr_t gdx_gfx_job_resolve_data(GdxGfxJob* job, uint32_t raw, int hostTagged, uintptr_t hostFull,
                                   size_t requiredBytes, int preferPhysical, uintptr_t sourceHint);

/* Texture source for SETTIMG when the operand is a tagged host pointer: the wide-stub hop
 * (venue banks / generated asset stubs / registered host arrays) that the bridge applies
 * before taking the pointer verbatim. Returns the redirected pointer, or hostFull unchanged. */
uintptr_t gdx_gfx_job_resolve_timg_host(GdxGfxJob* job, uintptr_t hostFull, size_t requiredBytes);

/* [timgmemo] Freshness key for a caller-side memo of the SETTIMG classification PAIR
 * (native-RGBA16, o2r-resource) over an already-resolved source pointer. It changes whenever
 * either of the two mutable tables those answers read changes -- gNativeRgba16Ranges, via
 * gNativeRgba16Generation, and the venue building-texture base -- and it needs no segment or
 * resolve-tables term, because neither answer reads either. The derivation, with the mutating
 * code path per table, is docs/research/native/loadblock-progress.md S1. */
uint32_t gdx_gfx_timg_class_epoch(void);
/* [timgmemo] diagnostic: [0..3] the last gNativeRgba16Generation, venue base, resolve-tables
 * version and mSegGen; [4..7] how often each changed. mSegGen is reported but NOT keyed on --
 * it folds gSegments[1] and turns over once per task. See the bridge. */
void gdx_gfx_timg_class_epoch_parts(uint32_t out[8]);

/* 1 when a SETTIMG source is what the bridge rewrote to an o2r resource for LUS (asset-segment
 * symbol with a 1:1 o2r key, not RDRAM, not the venue building window): those took LUS's
 * resource decode path and same-content skip. */
int gdx_gfx_timg_is_resource(uint32_t raw, int hostTagged, uintptr_t resolved, size_t loadBytes);
/* [nverify] diagnostic: address class bits (1 RDRAM, 2 GfxPool, 4 native-RGBA16, 8 registered host). */
unsigned gdx_gfx_classify_host_ptr(unsigned long p);

/* 1 when [source, source+size) lies inside a registered host-order RGBA16 range (CPU
 * framebuffer readbacks, transition captures): the decoder must byte-swap each texel. */
int gdx_gfx_texels_are_native_rgba16(uintptr_t source, size_t size);

/* Bytes readable from `address` (0 = unreadable). */
size_t gdx_gfx_readable_bytes(uintptr_t address);

/* [tmemblock] SOURCE-STABILITY SEAM -- the generation/DMA staleness test the bridge already
 * runs on the LUS path (MakePersistentRawTextureCopy, n64_gfx_bridge.cpp) and the owned walk
 * never ran. The owned walk re-mirrors 4 KiB into TMEM and re-hashes it on every G_LOADBLOCK,
 * 128x per task, and lands on last frame's texture-cache entry every time; this is how it asks
 * "did these bytes move" without a compare and without a second content hash. The invalidation
 * argument -- every mutable input, the term that covers it, and the inputs that are NOT
 * covered -- is docs/research/native/tmemblock-progress.md S2. */
#define GDX_TMEMSRC_UNKNOWN 0u /* the bridge memcmps this class; no cheap test exists */
#define GDX_TMEMSRC_NATIVE  1u /* native-RGBA16, not a framebuffer: gNativeRgba16Generation */
#define GDX_TMEMSRC_DMA     2u /* RDRAM or framebuffer shadow: gDmaDirtyRanges */
#define GDX_TMEMSRC_STABLE  3u /* registered host range, not GfxPool, not the venue window */

/* The class of a resolved texture source, in MakePersistentRawTextureCopy's own cascade order. */
unsigned gdx_gfx_tmem_source_class(uintptr_t source, size_t size);
/* [0] = gNativeRgba16Generation (through the atomic accessor), [1] = gDmaGeneration. Sample it
 * BEFORE reading the source, never after: a write that lands during the read then leaves the
 * live generation ahead of the stamp, and the next load re-reads. */
void gdx_gfx_tmem_source_stamp(uint64_t out[2]);
/* 1 only when the bytes are PROVABLY identical to what they held when `stamp` was taken under
 * class `clsAtStamp`. Every uncertainty -- an unknown class, a class that changed under the
 * source, a missing stamp -- answers 0, which means "do the work", never "changed". */
int gdx_gfx_tmem_source_unchanged(uintptr_t source, size_t size, unsigned clsAtStamp,
                                  const uint64_t stamp[2]);

/* 1 when a GDX1-tagged low32 must NOT be taken verbatim: generated asset placeholder stubs and
 * the D_1000000 / D_2000000 BSS aliases. The bridge routes those through the low32 resolver. */
int gdx_gfx_low32_needs_resolver(uint32_t low32);

/* G_MOVEWORD segment write (index 6): same resolution + epoch policy as the bridge's case,
 * writing the job's segment view. Returns the base now in the table. */
uintptr_t gdx_gfx_job_set_segment(GdxGfxJob* job, unsigned segment, uint32_t raw, int hostTagged,
                                  uintptr_t hostFull);

/* G_LOAD_UCODE classification. Returns 1 when the text pointer names a known microcode and
 * fills *variant (0 Standard, 1 Reject, 2 FZeroFlxReject) and *isLine (L3DEX2 line section,
 * runs as Standard with G_LINE3D live). 0 = unknown load (drop the load, keep walking). */
int gdx_gfx_ucode_variant(uint32_t raw, int hostTagged, uintptr_t hostFull, int* variant, int* isLine);

/* interp_verify record pass: the LUS path on this job, unconditionally. */
void gdx_gfx_job_run_lus(GdxGfxJob* job);

/* The task's entry microcode variant (0 Standard, 1 Reject, 2 FZeroFlxReject). */
int gdx_gfx_job_ucode(GdxGfxJob* job);

/* Per-job bridge walk statistics (the converted copy's opcode histogram), for the M0
 * histogram parity receipt: lists converted, commands emitted, per-opcode counts. */
void gdx_gfx_job_bridge_stats(GdxGfxJob* job, unsigned long* lists, unsigned long* commands,
                              unsigned long opCounts[256]);

/* Task PROLOGUE shared by both paths: apply the queued texture-cache clear/deletes
 * (gdx_texcache_request_clear, gdx_invalidate_texture_address) to whichever caches are live.
 * Must run BEFORE the walk, like GdxGfxJobRun's own call, so an in-place content refresh takes
 * effect on the frame that produced it. Render thread. */
void gdx_gfx_texcache_drain(void);

/* Task epilogue shared by both paths (VI-fallback flag, cadence counter, GPU-content latch,
 * frame mirror refresh, transition capture, named-frame dump). Called by the owned path
 * after its walk, on the render thread, exactly where GdxGfxJobRun calls the same code. */
void gdx_gfx_job_epilogue(GdxGfxJob* job);

/* Frame-level configuration the LUS interpreter latched in StartFrame, borrowed by the owned
 * path until M5 (plan §3.13): render target policy and dimensions. */
typedef struct GdxFrameConfig {
    uint32_t windowW, windowH;   /* mGfxCurrentWindowDimensions */
    uint32_t curW, curH;         /* mCurDimensions */
    float internalMul;
    uint32_t nativeW, nativeH;   /* mNativeDimensions */
    int rendersToFb;             /* mRendersToFb */
    int gameFb;                  /* mGameFb */
    int widescreen;              /* mWidescreenEnabledCache */
    int forceFixedAspect;        /* live sGdxForceFixedAspect (Run re-latches per task) */
    int widescreenUi;
    int ultrawide;
    int removeBorders;
    float hudMaxAspect;
    int textureFilter;           /* FilteringMode */
} GdxFrameConfig;
int gdx_gfx_frame_config(GdxFrameConfig* out);
/* [nverify] re-apply the LUS interpreter's cached backend state (verify shares one backend). */
void gdx_gfx_lus_resync_backend(void);

/* [nverify] LUS end-of-task RDP/RSP state words (layout in n64_gfx_bridge.cpp). */
unsigned gdx_gfx_lus_state(uint32_t* out, unsigned cap);

/* After the owned path's walk: publishes mGfxFrameBuffer exactly as Run()'s epilogue does. */
void gdx_gfx_frame_publish(uintptr_t gfxFrameBuffer);

#ifdef __cplusplus
}
#endif

#endif /* GDX_DLSRC_BRIDGE_H */
