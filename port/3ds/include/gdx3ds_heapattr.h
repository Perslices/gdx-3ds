/* port/3ds/include/gdx3ds_heapattr.h — C4c: who owns the heap bytes no census row names.
 *
 * `o3dsbudget`'s [mem-res] attributes the heap by ENUMERATING known owners (the RDRAM
 * arena, the segment-family blobs, the bridge caches, the LUS cache). Everything else is
 * one `unattributed=` number, and `o3dssim` split that number by allocator: ~30% C++
 * `operator new` (which [live-ra] can already name) and ~70% plain C `malloc`, which NO
 * instrument in this port could see, because every one of them hooks `operator new`.
 *
 * This is the other half. `port/3ds/gdx3ds_mallocwrap.c` interposes newlib's allocator at
 * LINK time (`-Wl,--wrap=_malloc_r,...`), so the coverage is not a list of call sites
 * somebody remembered: it is every byte the process mallocs, from crt0 onwards, including
 * newlib's own internal allocations and libctru's.
 *
 * Two independently-gated things live behind that interposition:
 *
 *   -DGDX_3DS_WRAP_MALLOC=ON   (DEFAULT ON) the universal allocation-failure receipt.
 *                              `[alloc-fail] site=... bytes=... ra=...` on every NULL from
 *                              the allocator, with the caller's return address. `rdramdiet`
 *                              landed [alloc-fail] at the handful of C sites that hold the
 *                              big blocks (~31 MB of the 38 MB C side) and said so; the
 *                              remaining 7,138,708 B was silent, and silent matters here
 *                              because this port under a tight grant HANGS with a frozen
 *                              frame counter rather than crashing (o3dssim §3.1/§4.2b),
 *                              which on the console is indistinguishable from a deadlock.
 *
 *   -DGDX_3DS_HEAPATTR=ON      (DEFAULT OFF, implies WRAP_MALLOC) the ranked attribution
 *                              census: a live ptr->(size, return address) map, dumped as
 *                              `[heap-attr]` rows on the [mem-census] cadence under
 *                              `[debug] membudget=1`, in the same shape as [mem-res].
 *                              Symbolize the `ra=` column with
 *                              `arm-none-eabi-addr2line -Cfie <build>/G-Diffuser-3DS.elf`.
 *                              It costs ~2 MB of .bss and a lock on every allocation, so
 *                              it is a MEASUREMENT build and never a shipped one.
 *
 * docs/research/native/heapattr-progress.md.
 */
#ifndef GDX3DS_HEAPATTR_H
#define GDX3DS_HEAPATTR_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when this build carries the attribution map (GDX_3DS_HEAPATTR=ON). 0 otherwise, and
 * then gdx3ds_heapattr_census() prints nothing at all rather than a row of zeros that
 * would read like a measurement -- the same policy [rdram-res] uses for GDX_3DS_MEMPROBE. */
int gdx3ds_heapattr_available(void);

/* Rank the live allocations by return address and print the top `topRows`. Caller owns the
 * cadence and the lever ([debug] membudget); this does not read the config itself. */
void gdx3ds_heapattr_census(unsigned long frame, int topRows);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HEAPATTR_H */
