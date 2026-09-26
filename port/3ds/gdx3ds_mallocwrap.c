/* port/3ds/gdx3ds_mallocwrap.c — C4c: link-time interposition of newlib's allocator.
 * Contract + why: port/3ds/include/gdx3ds_heapattr.h.
 *
 * WHICH SYMBOLS, AND WHY IT IS THE `_r` PAIR RATHER THAN `malloc`/`free`
 * ---------------------------------------------------------------------
 * `o3dssim` §5 specified `-Wl,--wrap=malloc,--wrap=free`. That is NOT enough, and the gap
 * is invisible until you look at the archive: in devkitARM's newlib, `malloc` is a thunk
 * (libc_a-malloc.o) around `_malloc_r`, and **26 objects in libc.a reference `_malloc_r`
 * directly against 17 that reference `malloc`** -- every stdio buffer, every `strdup`,
 * `_calloc_r`, `_realloc_r` and `_memalign_r` among them. Wrapping `malloc` alone would
 * have measured a fraction of the C side and reported it as the whole of it.
 *
 * So the wrap set is the PRIMITIVE pair plus the public entry points:
 *
 *   --wrap=_malloc_r --wrap=_free_r      the two primitives EVERY allocation funnels
 *                                        through, including newlib's own and libctru's.
 *                                        This pair alone is complete coverage.
 *   --wrap=_calloc_r --wrap=_realloc_r --wrap=_memalign_r
 *   --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc --wrap=memalign
 *                                        NOT for coverage -- for ATTRIBUTION. They exist
 *                                        so the recorded return address is the caller's
 *                                        and not a line inside libc_a-callocr.o.
 *
 * THE DOUBLE-COUNT THAT SHAPE AVOIDS
 * ----------------------------------
 * `_calloc_r`, `_realloc_r` and `_memalign_r` live in their own objects and call
 * `_malloc_r` as an UNDEFINED symbol, so the linker redirects them into __wrap__malloc_r
 * too. A naive wrapper set would therefore note the same block twice. Two rules fix it:
 *
 *   1. Every public wrapper calls the `__real__*_r` primitive DIRECTLY, never the public
 *      `__real_*` thunk, so it can never re-enter a wrapper it is already inside.
 *   2. The outer wrappers use MapNoteOrReplace(), which is idempotent on the key: if the
 *      inner __wrap__malloc_r already recorded the block, the outer call only overwrites
 *      the return address with the real caller's. Bytes are counted exactly once.
 *
 * This also removes the need for any reentrancy flag, which would have had to be
 * per-thread -- malloc here is called from the game thread, the audio producer OS thread,
 * the render thread and libctru's service threads at once.
 *
 * ALWAYS-ON COST (GDX_3DS_WRAP_MALLOC=ON, the default)
 * ---------------------------------------------------
 * `malloc` -> `_malloc_r` becomes `__wrap_malloc` -> `_malloc_r`: the SAME call depth,
 * because the wrapper replaces newlib's thunk rather than wrapping it. The added work on
 * a successful allocation is one NULL compare. Nothing on the failure path runs unless the
 * allocation already failed. `-DGDX_3DS_WRAP_MALLOC=OFF` restores the original link
 * exactly (this TU is then not compiled and no --wrap option is passed).
 */

#include <3ds.h>

#include <malloc.h>
#include <reent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_filelog.h"
#include "gdx3ds_heapattr.h"

/* GDX_3DS_WRAP_MALLOC=OFF still compiles this TU, so gdx3ds_heapattr_available()/census()
   keep a definition and gdx3ds_membudget.cpp links either way; everything below the
   attribution map is then compiled out and no --wrap option is passed, which is what makes
   "OFF restores the original link exactly" true rather than approximately true. */
#if !defined(GDX3DS_WRAP_MALLOC)
#define GDX3DS_WRAP_MALLOC 0
#endif
#if !GDX3DS_WRAP_MALLOC && defined(GDX3DS_HEAPATTR)
#undef GDX3DS_HEAPATTR
#endif

/* port/shims.c. Prints the [alloc-fail] receipt `rdramdiet` established (C4h), plus the
   return address this wrapper can supply and its named call sites cannot. It is guarded
   against reentry there, which matters: the logger formats and writes, and a logger that
   allocated on the out-of-heap path would recurse forever. */
extern void gdx_alloc_fail_ra(const char* site, size_t bytes, void* ra);

#if GDX3DS_WRAP_MALLOC
/* Provided by the linker under --wrap. */
void* __real__malloc_r(struct _reent* r, size_t n);
void  __real__free_r(struct _reent* r, void* p);
void* __real__calloc_r(struct _reent* r, size_t a, size_t b);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
void* __real__memalign_r(struct _reent* r, size_t align, size_t n);
#endif

/* ------------------------------------------------------------------------------------
 * The live map. Compiled only under -DGDX_3DS_HEAPATTR=ON.
 * ------------------------------------------------------------------------------------ */
#if defined(GDX3DS_HEAPATTR) && GDX3DS_HEAPATTR

#ifndef GDX3DS_HEAPATTR_SLOT_BITS
#define GDX3DS_HEAPATTR_SLOT_BITS 17 /* 131072 tracked live blocks */
#endif
#define GDX3DS_HEAPATTR_SLOTS (1u << GDX3DS_HEAPATTR_SLOT_BITS)
#define GDX3DS_HEAPATTR_BUCKET_BITS (GDX3DS_HEAPATTR_SLOT_BITS - 2)
#define GDX3DS_HEAPATTR_BUCKETS (1u << GDX3DS_HEAPATTR_BUCKET_BITS)

/* Chained hash map, ONE lock. Chaining rather than open addressing because deletions are
   as frequent as insertions here and linear-probe tombstones would degrade without bound
   over a five-thousand-frame run. Every index is stored +1 so the zero-initialised .bss
   IS the empty map and nothing has to run before the first malloc -- which is the whole
   requirement: the first allocation in this process happens in crt0, long before main(). */
static void* sKey[GDX3DS_HEAPATTR_SLOTS];
static uint32_t sRa[GDX3DS_HEAPATTR_SLOTS];
static uint32_t sSize[GDX3DS_HEAPATTR_SLOTS];
static uint32_t sNext[GDX3DS_HEAPATTR_SLOTS];  /* index+1, 0 = end of chain */
static uint32_t sHead[GDX3DS_HEAPATTR_BUCKETS];/* index+1, 0 = empty bucket */
static uint32_t sBump = 0;                     /* slots never yet used */
static uint32_t sFreeTop = 0;                  /* index+1 of the free-list head */

static unsigned long sLive = 0;        /* tracked live blocks */
static unsigned long sLiveBytes = 0;   /* tracked live usable bytes */
static unsigned long sOverflow = 0;    /* allocations the table had no slot for */
static unsigned long sOverflowBytes = 0;
static unsigned long sUnmappedFree = 0;/* frees of blocks we never tracked (= overflowed) */

/* Unlocked is 1 (libctru's LightLock_Init post-state). Initialised HERE rather than by a
   call, because there is no point in the process early enough to make that call. */
static LightLock sMapLock = 1;

static inline uint32_t MapBucket(const void* p) {
    /* newlib chunks are 8-byte aligned, so the low bits carry no entropy. */
    uint32_t h = (uint32_t)(uintptr_t)p >> 3;
    h *= 2654435761u;
    return h >> (32u - GDX3DS_HEAPATTR_BUCKET_BITS);
}

/* Caller holds sMapLock. Returns index+1, or 0 when absent. */
static uint32_t MapFindLocked(const void* p, uint32_t bucket, uint32_t* prevOut) {
    uint32_t prev = 0;
    uint32_t cur = sHead[bucket];
    while (cur != 0) {
        if (sKey[cur - 1u] == p) {
            if (prevOut != NULL) {
                *prevOut = prev;
            }
            return cur;
        }
        prev = cur;
        cur = sNext[cur - 1u];
    }
    return 0;
}

/* Record `p`, or refresh the return address of a block an inner wrapper already recorded.
   Idempotent on the key -- see the header comment's rule 2. */
static void MapNoteOrReplace(void* p, void* ra) {
    if (p == NULL) {
        return;
    }
    const uint32_t usable = (uint32_t)malloc_usable_size(p);
    const uint32_t bucket = MapBucket(p);
    LightLock_Lock(&sMapLock);
    uint32_t found = MapFindLocked(p, bucket, NULL);
    if (found != 0) {
        sLiveBytes -= sSize[found - 1u];
        sSize[found - 1u] = usable;
        sRa[found - 1u] = (uint32_t)(uintptr_t)ra;
        sLiveBytes += usable;
        LightLock_Unlock(&sMapLock);
        return;
    }
    uint32_t slot;
    if (sFreeTop != 0) {
        slot = sFreeTop;
        sFreeTop = sNext[slot - 1u];
    } else if (sBump < GDX3DS_HEAPATTR_SLOTS) {
        slot = ++sBump;
    } else {
        /* Stated rather than silent: the totals line prints overflow= and its bytes, so an
           undersized table reads as a known gap instead of as an attribution result. */
        sOverflow++;
        sOverflowBytes += usable;
        LightLock_Unlock(&sMapLock);
        return;
    }
    sKey[slot - 1u] = p;
    sSize[slot - 1u] = usable;
    sRa[slot - 1u] = (uint32_t)(uintptr_t)ra;
    sNext[slot - 1u] = sHead[bucket];
    sHead[bucket] = slot;
    sLive++;
    sLiveBytes += usable;
    LightLock_Unlock(&sMapLock);
}

static void MapDrop(void* p) {
    if (p == NULL) {
        return;
    }
    const uint32_t bucket = MapBucket(p);
    LightLock_Lock(&sMapLock);
    uint32_t prev = 0;
    uint32_t found = MapFindLocked(p, bucket, &prev);
    if (found == 0) {
        sUnmappedFree++;
        LightLock_Unlock(&sMapLock);
        return;
    }
    if (prev == 0) {
        sHead[bucket] = sNext[found - 1u];
    } else {
        sNext[prev - 1u] = sNext[found - 1u];
    }
    sLive--;
    sLiveBytes -= sSize[found - 1u];
    sKey[found - 1u] = NULL;
    sNext[found - 1u] = sFreeTop;
    sFreeTop = found;
    LightLock_Unlock(&sMapLock);
}

#define GDX3DS_HEAPATTR_SITES 512
static uint32_t sSiteRa[GDX3DS_HEAPATTR_SITES];
static unsigned long sSiteBytes[GDX3DS_HEAPATTR_SITES];
static unsigned long sSiteCount[GDX3DS_HEAPATTR_SITES];

static void Emit(const char* line, size_t len) {
    svcOutputDebugString(line, len);
    gdx3ds_filelog_write(line, len);
}

static void EmitF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void EmitF(const char* fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n > 0) {
        Emit(msg, (size_t)(n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1));
    }
}

int gdx3ds_heapattr_available(void) {
    return 1;
}

void gdx3ds_heapattr_census(unsigned long frame, int topRows) {
    if (topRows <= 0) {
        topRows = 24;
    }
    memset(sSiteRa, 0, sizeof(sSiteRa));
    memset(sSiteBytes, 0, sizeof(sSiteBytes));
    memset(sSiteCount, 0, sizeof(sSiteCount));

    unsigned long live = 0;
    unsigned long liveBytes = 0;
    unsigned long distinct = 0;
    unsigned long lost = 0; /* live bytes whose site table slot could not be found */

    /* The whole walk is under the lock: the audio producer thread allocates while this
       runs, and a chain followed across a concurrent removal is a wild pointer. It is
       ~131k iterations of two loads on the game thread, on the [mem-census] cadence. */
    LightLock_Lock(&sMapLock);
    const uint32_t used = sBump;
    for (uint32_t i = 0; i < used; i++) {
        if (sKey[i] == NULL) {
            continue;
        }
        live++;
        liveBytes += sSize[i];
        const uint32_t ra = sRa[i];
        uint32_t h = (ra >> 2) * 2654435761u;
        uint32_t idx = h % (uint32_t)GDX3DS_HEAPATTR_SITES;
        int placed = 0;
        for (int probe = 0; probe < GDX3DS_HEAPATTR_SITES; probe++) {
            if (sSiteCount[idx] == 0) {
                sSiteRa[idx] = ra;
                distinct++;
                placed = 1;
            } else if (sSiteRa[idx] == ra) {
                placed = 1;
            }
            if (placed) {
                sSiteBytes[idx] += sSize[i];
                sSiteCount[idx]++;
                break;
            }
            idx = (idx + 1u) % (uint32_t)GDX3DS_HEAPATTR_SITES;
        }
        if (!placed) {
            lost += sSize[i];
        }
    }
    const unsigned long overflow = sOverflow;
    const unsigned long overflowBytes = sOverflowBytes;
    const unsigned long unmappedFree = sUnmappedFree;
    const uint32_t slotsUsed = sBump;
    LightLock_Unlock(&sMapLock);

    struct mallinfo mi = mallinfo();
    EmitF("[heap-attr] frame=%lu sites=%lu live=%lu bytes=%lu slots=%lu/%lu top=%d", frame,
          distinct, live, liveBytes, (unsigned long)slotsUsed,
          (unsigned long)GDX3DS_HEAPATTR_SLOTS, topRows);

    /* Partial selection sort: topRows is <= 32 in practice and the site table is 512, so
       this is a few thousand compares and needs no scratch allocation (which would itself
       perturb the thing being measured). */
    for (int rank = 0; rank < topRows; rank++) {
        int best = -1;
        for (int i = 0; i < GDX3DS_HEAPATTR_SITES; i++) {
            if (sSiteCount[i] == 0) {
                continue;
            }
            if (best < 0 || sSiteBytes[i] > sSiteBytes[best]) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        EmitF("[heap-attr] rank=%d bytes=%lu kb=%lu count=%lu ra=%p", rank + 1,
              sSiteBytes[best], sSiteBytes[best] / 1024ul, sSiteCount[best],
              (void*)(uintptr_t)sSiteRa[best]);
        sSiteCount[best] = 0;
        sSiteBytes[best] = 0;
    }

    /* The reconciliation, in [mem-res]'s spirit: `tracked` is what the rows above sum to,
       `heapUsed` is mallinfo's. They do not match exactly and the difference is KNOWN, not
       mysterious: mallinfo counts each chunk's header (8 B on this newlib) while this map
       counts malloc_usable_size, so heapUsed - tracked should be about 8 x live plus
       overflowBytes. A larger gap than that is a finding. */
    EmitF("[heap-attr] totals frame=%lu tracked=%lu heapUsed=%lu hdrEst=%lu overflow=%lu/%lu "
          "unmappedFree=%lu lost=%lu",
          frame, liveBytes, (unsigned long)mi.uordblks, live * 8ul, overflow, overflowBytes,
          unmappedFree, lost);
}

#else /* !GDX3DS_HEAPATTR */

#define MapNoteOrReplace(p, ra) ((void)(p), (void)(ra))
#define MapDrop(p) ((void)(p))

int gdx3ds_heapattr_available(void) {
    return 0;
}

void gdx3ds_heapattr_census(unsigned long frame, int topRows) {
    (void)frame;
    (void)topRows;
}

#endif /* GDX3DS_HEAPATTR */

#if GDX3DS_WRAP_MALLOC
/* ------------------------------------------------------------------------------------
 * The wrappers.
 *
 * Failure reporting is at the two places an allocation can first come back NULL without
 * having gone through another wrapper: __wrap__malloc_r (the primitive every other path
 * funnels into, including calloc/realloc/memalign and all of newlib's internals) and
 * __wrap_malloc (which calls the primitive's __real_ form directly and so bypasses it).
 * Reporting anywhere else would double-log the same failure.
 * ------------------------------------------------------------------------------------ */

void* __wrap__malloc_r(struct _reent* r, size_t n) {
    void* ra = __builtin_return_address(0);
    void* p = __real__malloc_r(r, n);
    if (p == NULL) {
        gdx_alloc_fail_ra("_malloc_r", n, ra);
        return NULL;
    }
    MapNoteOrReplace(p, ra);
    return p;
}

void __wrap__free_r(struct _reent* r, void* p) {
    MapDrop(p);
    __real__free_r(r, p);
}

void* __wrap__calloc_r(struct _reent* r, size_t a, size_t b) {
    void* ra = __builtin_return_address(0);
    void* p = __real__calloc_r(r, a, b);
    MapNoteOrReplace(p, ra);
    return p;
}

void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) {
    void* ra = __builtin_return_address(0);
    /* Drop FIRST: newlib's realloc may grow or shrink the chunk in place, in which case
       the same pointer comes back with a different usable size and a stale ledger entry
       would never be corrected. MapNoteOrReplace re-measures it either way. */
    MapDrop(p);
    void* q = __real__realloc_r(r, p, n);
    if (q != NULL) {
        MapNoteOrReplace(q, ra);
    } else if (p != NULL) {
        MapNoteOrReplace(p, ra); /* a failed realloc leaves the original block live */
    }
    return q;
}

void* __wrap__memalign_r(struct _reent* r, size_t align, size_t n) {
    void* ra = __builtin_return_address(0);
    void* p = __real__memalign_r(r, align, n);
    MapNoteOrReplace(p, ra);
    return p;
}

void* __wrap_malloc(size_t n) {
    void* ra = __builtin_return_address(0);
    void* p = __real__malloc_r(_REENT, n);
    if (p == NULL) {
        gdx_alloc_fail_ra("malloc", n, ra);
        return NULL;
    }
    MapNoteOrReplace(p, ra);
    return p;
}

void __wrap_free(void* p) {
    MapDrop(p);
    __real__free_r(_REENT, p);
}

void* __wrap_calloc(size_t a, size_t b) {
    void* ra = __builtin_return_address(0);
    void* p = __real__calloc_r(_REENT, a, b);
    MapNoteOrReplace(p, ra);
    return p;
}

void* __wrap_realloc(void* p, size_t n) {
    void* ra = __builtin_return_address(0);
    MapDrop(p);
    void* q = __real__realloc_r(_REENT, p, n);
    if (q != NULL) {
        MapNoteOrReplace(q, ra);
    } else if (p != NULL) {
        MapNoteOrReplace(p, ra);
    }
    return q;
}

void* __wrap_memalign(size_t align, size_t n) {
    void* ra = __builtin_return_address(0);
    void* p = __real__memalign_r(_REENT, align, n);
    MapNoteOrReplace(p, ra);
    return p;
}

#endif /* GDX3DS_WRAP_MALLOC */
