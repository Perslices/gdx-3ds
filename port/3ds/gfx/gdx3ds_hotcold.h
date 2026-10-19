/* port/3ds/gfx/gdx3ds_hotcold.h -- hotcold: per-function hot/cold attributes for the render path.
 *
 * WHY. On the Old 3DS (16 KiB L1 I-cache, no L2) 43.6% of render cycles are instruction-fetch
 * stalls (hwprof-progress.md §10.1). Receipts, verify modes, censuses, logging and miss/fallback
 * paths compiled inline in the hot interpreter/backend functions spread the per-frame code over
 * more 32-byte lines than it executes. Moving them into separate cold functions keeps the hot
 * straight-line code compact (docs/research/native/hotcold-progress.md).
 *
 * WHAT. Source attributes only: no compiler flag, no section-ordering change. GDX_COLD marks an
 * extracted slow path `cold, noinline`: GCC emits it in .text.unlikely.<name> and predicts every
 * path that reaches a call to it as unlikely, so the caller's hot blocks stay contiguous.
 * GDX_UNLIKELY/GDX_LIKELY are __builtin_expect hints for branches whose slow side is not a call.
 * None of these changes what the code computes.
 *
 * ONE-LINE REVERT. `cold` also makes GCC optimise the marked function for size. The Old 3DS
 * boot crash (rel1crash) came with whole-image -Os cold code and is not root-caused; if a hardware
 * card ever implicates size-optimised diagnostics, define GDX3DS_HOTCOLD_NO_COLD_ATTR to keep the
 * extraction (noinline) and drop the `cold` half everywhere at once. */
#ifndef GDX3DS_HOTCOLD_H
#define GDX3DS_HOTCOLD_H

#if defined(__GNUC__) && !defined(GDX3DS_HOTCOLD_NO_COLD_ATTR)
#define GDX_COLD __attribute__((cold, noinline))
#elif defined(__GNUC__)
#define GDX_COLD __attribute__((noinline))
#else
#define GDX_COLD
#endif

#if defined(__GNUC__)
#define GDX_LIKELY(x) __builtin_expect(!!(x), 1)
#define GDX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GDX_LIKELY(x) (x)
#define GDX_UNLIKELY(x) (x)
#endif

#endif /* GDX3DS_HOTCOLD_H */
