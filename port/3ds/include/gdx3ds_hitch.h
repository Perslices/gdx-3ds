/* port/3ds/include/gdx3ds_hitch.h — HITCH: the per-frame hitch receipt (docs/research/native/
 * hitch-progress.md).
 *
 * WHY. The Old 3DS race under frameskip=steady showed rwmax 80-515 ms in almost every 256-tick
 * window and 8-15 missed slots per window (hw-rel2, native/main@dbedfb5), while the windowed
 * receipts ([c5cat], [gpu]) only ever show means. This receipt names what happened INSIDE the
 * one frame that overran.
 *
 * EVENT COUNTERS (always on, zero heap). Hook sites across the renderer, the asset path and the
 * audio stream add to one static array; the hitch module reads and clears it once per host
 * iteration. An add is one relaxed atomic (the audio producer and the render thread count too).
 * The storage lives in the gfx library (gdx3ds_hitch_ev.c) because gdx3ds-dl-tests links that
 * library without the executable, like gdx3ds_cov.c.
 *
 * THE RECEIPT ([debug] hitch = 1, DEFAULT 0). A host iteration whose work (wall minus the
 * frameskip pacing waits) exceeds max(2 x the running median, median + half a vblank) of its
 * class (rendered / skipped) is a hitch. Its line is formatted into a static 8-line ring and
 * written to the log only on a later, non-hitch iteration, at most one line per 8 iterations and
 * 16 per 256 (the rest are counted: supp=). So the instrument never adds its own SD traffic to
 * the frame it reports. Format (ms unless named):
 *
 *   [hitch] f=N r=1 D=4 late=1 ms=123.4 med=30.1 x=4.1 | lg= dl= au= ld= vi= ta= tl= pc= | fb= mi=
 *     sk= tr= dr= ma= me= sh= fx= hu= mr= sw= fe= | tex=imports/misses/evicts up=n/KiB seg=n/KiB
 *     arc=n/KiB bake= rr= mtb= hudm= tsk= lod= gsp= | log=lines sync=n/KiB/ms async=busy/pending
 *     aud=reads/KiB/inflight supp=
 *
 * The category columns are this iteration's [c5cat] buckets and need the mprof clock
 * ([debug] gputrace=1 or mainprof=1); without it they print as "cats=off". A summary line
 * `[hitch] sum` every 1024 iterations carries the counts and the file-log writer's totals. */
#ifndef GDX3DS_HITCH_H
#define GDX3DS_HITCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX_HITCH_EV_TEX_IMPORT = 0, /* texture imports (decode into a GPU texture), any interpreter */
    GDX_HITCH_EV_TEX_MISS,       /* texture cache misses */
    GDX_HITCH_EV_TEX_EVICT,      /* owned texture cache LRU evictions */
    GDX_HITCH_EV_TEX_UP,         /* GPU texture uploads (backend UploadTexture / atlas writes) */
    GDX_HITCH_EV_TEX_UP_BYTES,
    GDX_HITCH_EV_TEX_DECODE_TICKS, /* owned interpreter: decode + upload time of cache misses */
    GDX_HITCH_EV_SEG_BLOB,       /* segment-source first-touch blob loads */
    GDX_HITCH_EV_SEG_BLOB_BYTES,
    GDX_HITCH_EV_ARC_LOAD,       /* archive file loads (SD read + inflate through the o2r) */
    GDX_HITCH_EV_ARC_LOAD_BYTES,
    GDX_HITCH_EV_MESH_BAKE,      /* native machine mesh bakes */
    GDX_HITCH_EV_RR_BUILD,       /* retained machine buffer builds (linearAlloc + flush) */
    GDX_HITCH_EV_MATTAB_BANK,    /* machine material records banked */
    GDX_HITCH_EV_HUD_MISS,       /* HUD replay source / load memo misses (a record rebuild) */
    GDX_HITCH_EV_TEXSKIP_REC,    /* texskip records captured */
    GDX_HITCH_EV_GSP_WAIT_TICKS, /* C3D_FrameBegin's vblank + GPU queue waits */
    GDX_HITCH_EV_PACE_TICKS,     /* frameskip's deliberate waits (slack: excluded from work) */
    GDX_HITCH_EV_AUD_SD_READ,    /* audio-stream block-cache SD reads (any thread) */
    GDX_HITCH_EV_AUD_SD_BYTES,
    GDX_HITCH_EV_TEX_UP_TICKS,   /* backend UploadTexture: alloc + swizzle + flush (inside DECODE) */
    GDX_HITCH_EV_COUNT
};

extern uint32_t gdx3ds_hitch_ev[GDX_HITCH_EV_COUNT];
/* Nonzero while an audio-stream SD read is in flight (block-cache fill). */
extern volatile int gdx3ds_hitch_aud_inflight;

/* svcGetSystemTick for TUs that do not include <3ds.h> (the host-compilable interpreter). */
uint64_t gdx3ds_hitch_now(void);

static inline void gdx3ds_hitch_add(int ev, uint32_t n) {
    __atomic_fetch_add(&gdx3ds_hitch_ev[ev], n, __ATOMIC_RELAXED);
}

/* Host loop (main_3ds.cpp): open the iteration after gdx3ds_mprof_frame_begin; close it just
 * before gdx3ds_mprof_frame_end (the category snapshot needs the iteration's buckets). */
void gdx3ds_hitch_frame_begin(void);
void gdx3ds_hitch_frame_end(unsigned long frame, int rendered);
/* After gdx3ds_mprof_frame_end (outside every bucket and every iteration's wall): write at most
 * one buffered line, on a non-hitch iteration, within the rate budget; the periodic summary. */
void gdx3ds_hitch_after_frame(unsigned long frame);

/* N60TEX [debug] texburst = N (default 0 = off): one `[texburst]` line for every iteration with
 * N or more texture cache misses (decode + upload ms, the upload share, KiB, the iteration's
 * work ms), unthrottled up to a session cap, and a `[texburst] sum` every 1024 iterations with
 * the window's worst. Independent of debug.hitch. */

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HITCH_H */
