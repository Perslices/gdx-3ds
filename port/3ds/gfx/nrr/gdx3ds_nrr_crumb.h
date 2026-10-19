/* port/3ds/gfx/nrr/gdx3ds_nrr_crumb.h -- n60hwdiag: durable breadcrumbs and the boot-cycling
 * NRR bisect, for a freeze that leaves no log (docs/research/native/n60hwdiag-progress.md).
 *
 * [debug] nrr_crumbs (default 0): one line per FIRST event of interest, written to
 * sdmc:/3ds/gdiffuser/nrr_crumbs.txt with FSFILE_Write(FS_WRITE_FLUSH) before the call returns
 * (no stdio buffer, no async writer), so the last crumb on the card is the last thing that ran
 * before a hard freeze. The file APPENDS across boots. Every crumb is also copied into the normal
 * filelog. Each line: `[crumb] #<id> f=<gGameFrameCount> e=<epoch> ms=<since boot> w=<previous
 * crumb's write us> | <text>`.
 *
 * Epochs: a change of (gGameMode, gCourseIndex) starts a new epoch ("a course load"); every
 * "first N" budget below is per epoch, so the race after machine select gets its own firsts.
 *
 * [debug] filelog_sync_frames (default 600 with crumbs on, 0 otherwise): from an epoch change, the
 * normal filelog flushes every line synchronously AND fsyncs (FSFILE_Flush) for that many frames.
 *
 * [debug] nrr_bisect_cycle (default 0) / nrr_bisect_force=k: gdx3ds_nrr_crumb_boot_select().
 *
 * Off (the default) every site costs one load + branch of gGdxNrrCrumbs (inline), nothing else. */
#ifndef GDX3DS_NRR_CRUMB_H
#define GDX3DS_NRR_CRUMB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Crumb sites with a per-epoch budget: base + cat for the per-category ones (NRR_CRUMB_CATS slots
 * each, >= NRR_CAT_COUNT). */
#define NRR_CRUMB_CATS 16
enum {
    NRR_CRUMB_SNAP = 0,                       /* + cat: game side, first marker written (snapshot) */
    NRR_CRUMB_MARKER = NRR_CRUMB_SNAP + NRR_CRUMB_CATS,    /* + cat: render side, first marker dispatched */
    NRR_CRUMB_HANDOFF = NRR_CRUMB_MARKER + NRR_CRUMB_CATS, /* + cat: NrrBegin .. NrrEnd of the first hand-offs */
    NRR_CRUMB_STATE = NRR_CRUMB_HANDOFF + NRR_CRUMB_CATS,  /* + cat: cold NrrStateGet / NrrStateBuildInto builds */
    NRR_CRUMB_RING = NRR_CRUMB_STATE + NRR_CRUMB_CATS,     /* + cat: first NrrRingAlloc */
    NRR_CRUMB_DRAW2D = NRR_CRUMB_RING + NRR_CRUMB_CATS,    /* + cat: first NrrDraw2D */
    NRR_CRUMB_DRAWXF = NRR_CRUMB_DRAW2D + NRR_CRUMB_CATS,  /* + cat: first NrrDrawXf */
    NRR_CRUMB_DXEMIT = NRR_CRUMB_DRAWXF + NRR_CRUMB_CATS,  /* + cat: first direct-word emission (verify scratch / direct) */
    NRR_CRUMB_DXCMP = NRR_CRUMB_DXEMIT + NRR_CRUMB_CATS,   /* + cat: first verify compare */
    NRR_CRUMB_FALLBACK = NRR_CRUMB_DXCMP + NRR_CRUMB_CATS, /* + cat: fallbacks */
    NRR_CRUMB_TRKBAKE = NRR_CRUMB_FALLBACK + NRR_CRUMB_CATS,
    NRR_CRUMB_MACHREC,
    NRR_CRUMB_MACHTEX,
    NRR_CRUMB_SKYLEARN,
    NRR_CRUMB_HUDOPEN,
    NRR_CRUMB_KICK,
    NRR_CRUMB_DXSYNC,
    NRR_CRUMB_TRKWALK,
    NRR_CRUMB_DXALLOC,
    NRR_CRUMB_TRKREFUSE,
    NRR_CRUMB_SITES
};

#if defined(__3DS__)
extern int gGdxNrrCrumbs; /* latched [debug] nrr_crumbs; 0 until the boot select ran */
#define GDX_NRR_CRUMBS_ON() (__builtin_expect(gGdxNrrCrumbs != 0, 0))
#else
#define GDX_NRR_CRUMBS_ON() 0
#endif

/* A durable crumb (no budget). Cold; call only behind GDX_NRR_CRUMBS_ON(). */
void gdx3ds_nrr_crumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/* 1 for the first `limit` calls per epoch at `site` (counts the call). */
int gdx3ds_nrr_crumb_want(int site, int limit);
/* A crumb with a per-epoch budget at `site`. */
#define NRR_CRUMB_FIRST(site, limit, ...)                                         \
    do {                                                                          \
        if (GDX_NRR_CRUMBS_ON() && gdx3ds_nrr_crumb_want((site), (limit))) {      \
            gdx3ds_nrr_crumb(__VA_ARGS__);                                        \
        }                                                                         \
    } while (0)

/* The category whose hand-off is open (set by the core around a draw; -1 none): cold builds that
 * do not see an NrrCtx (NrrStateGet) attribute their crumb with it. */
void gdx3ds_nrr_crumb_set_cat(int cat);
int gdx3ds_nrr_crumb_cat(void);

/* A GPU-read buffer, for a crumb: "<name>=<va> pa=<pa> al=<va & 0x7f>", plus " NOT-GPU-VISIBLE" when the
 * address has no physical mapping the GPU can read (osConvertVirtToPhys = 0: heap, stack, .data). */
int gdx3ds_nrr_crumb_buf(char* out, int cap, const char* name, const void* p);
/* linearSpaceFree() in KiB, for allocation crumbs. */
unsigned gdx3ds_nrr_crumb_linfree_kb(void);

/* The backend's frame boundaries (gfx_citro3d.cpp): crumbs around C3D_FrameBegin / C3D_FrameEnd for
 * the first frames of an epoch and the frames right after any other crumb. */
void gdx3ds_nrr_crumb_frame_begin_pre(void);
void gdx3ds_nrr_crumb_frame_begin_post(void);
void gdx3ds_nrr_crumb_frame_end_pre(void);
void gdx3ds_nrr_crumb_frame_end_post(void);

/* main_3ds.cpp, right after the ini is loaded and BEFORE the filelog opens or any lever is read:
 * latches nrr_crumbs; with nrr_bisect_cycle / nrr_bisect_force picks this boot's NRR subset, writes
 * the counter back (flushed) and forces the subset's levers (gdx3ds_config_override), and names
 * this boot's filelog. Then, once the filelog is up: the receipt lines. */
void gdx3ds_nrr_crumb_boot_select(void);
void gdx3ds_nrr_crumb_boot_receipt(void);
/* "b3 s3:mach" for the fps line, "" when the bisect is off. */
const char* gdx3ds_nrr_crumb_tag(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_CRUMB_H */
