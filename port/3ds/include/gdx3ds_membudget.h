/* port/3ds/include/gdx3ds_membudget.h — O3DS BUDGET ([debug] membudget, DEFAULT 0)
 *
 * Two receipts, both diagnostic, both inert when the lever is off:
 *
 *   [mem-budget]  ONE line at boot. What this console actually granted us: the model, the
 *                 kernel memory-layout id, the APPLICATION memory region's size/used, the
 *                 two libctru heap grants and the static image split. Every field is read
 *                 from the device, so the line is the same instrument on New 3DS, on Old
 *                 3DS and in an emulator pinned to either -- which is the point: the
 *                 Old 3DS budget stops being folklore the first time this prints there.
 *
 *   [mem-res]     A residency CENSUS, ranked by bytes descending: which owners hold the
 *                 heap right now. Emitted on the [mem-census] cadence. The last line
 *                 reconciles the ranked total against mallinfo's used bytes, so the
 *                 census states its own blind spot instead of hiding it.
 *
 * docs/research/native/o3dsbudget-progress.md.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latch the [debug] membudget / membudget_top keys. Call once, after gdx3ds_config_load. */
void gdx3ds_membudget_configure(void);

/* 1 when [debug] membudget=1. The frame loop uses this to run the census cadence even
 * when [debug] verbose is 0, so a budget run does not have to buy the whole telemetry set. */
int gdx3ds_membudget_enabled(void);

/* The one-line boot receipt. No-op unless enabled. Call after the heaps and the window
 * are up (it reads osGetMemRegionUsed, which must see the real process). */
void gdx3ds_membudget_boot(void);

/* The ranked residency census. `gfx` is gdx_gfx_mem_census's 8 slots, `lusBytes` the LUS
 * ResourceManager cache size, `bigBytes` the cumulative >=256 KB allocation counter.
 * Main thread only, render thread already joined (the caller owns that: see emitMemCensus). */
void gdx3ds_membudget_census(unsigned long frame, const char* why, const unsigned long gfx[8],
                             unsigned long lusBytes, unsigned long bigBytes);

#ifdef __cplusplus
}
#endif
