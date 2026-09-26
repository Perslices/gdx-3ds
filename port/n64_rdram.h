#pragma once
/* Do NOT include <stddef.h> or <stdint.h> here.
   C callers must include global.h first (provides size_t/uint32_t via decomp libc).
   C++ callers must include a standard header before this one (e.g. ship/Context.h). */
#ifdef __cplusplus
extern "C" {
#endif

/* Maps N64 physical addr `phys` to `gdx_rdram + phys`.
 *
 * THE SIZE IS A PORT DECISION, NOT A CONSOLE ONE. The retail cartridge runs in 4 MB and the
 * Expansion Pak makes it 8 MB, so 16 MB is not a console size at all (see
 * docs/research/native/rdramdiet-progress.md for the occupancy census). The 16 MB here is
 * the PORT's arena: gdx_rdram_alloc_raw bump-allocates UP from
 * gdx_rdram_arena_start and gdx_rdram_persist_alloc_raw bump-allocates DOWN from the top, so
 * the constant is the distance those two cursors are allowed to travel before they meet --
 * and on a 64-bit host the decomp's structures are wider than the N64's, so the port needs
 * more of it than the console did. It is also a CLASSIFICATION bound: the gfx bridge treats
 * a bare u32 below GDX_RDRAM_SIZE as an RDRAM offset (n64_gfx_bridge.cpp, the
 * GDX_RDRAM_GFXPOOL_OFFSET..GDX_RDRAM_SIZE window), so changing it changes which raw words
 * resolve into the arena. Both properties are why it is a build-time knob with a receipt and
 * not an ini lever.
 *
 * Override with -DGDX_3DS_RDRAM_MB=N (port/3ds/CMakeLists.txt); 16 is the shipped default and
 * an unset knob compiles to exactly the constant that was here before the knob existed. */
#ifndef GDX_RDRAM_SIZE_BYTES
#define GDX_RDRAM_SIZE_BYTES     0x1000000u             /* 16 MB -- the shipped default */
#endif
#define GDX_RDRAM_SIZE           ((size_t)GDX_RDRAM_SIZE_BYTES)
#define GDX_RDRAM_GFXPOOL_OFFSET ((size_t)0x4000u)     /* lower bound for bare RDRAM offsets (course sub-DLs start ~0x6000) */

/* Dedicated ALLOC_PEEK staging block, carved right after the GfxPool reservation and before
   gdx_rdram_arena_start (see gdx_rdram_init/gdx_rdram_peek_raw in decomp_port.c). Keeping
   transient peek scratch out of the mode arena is what stops a concurrent ALLOC_FRONT/BACK
   commit from landing on top of a live peek mid-decode. Sized well above any compressed-texture
   header/payload the decomp's texture loader uses (see object.c). */
#define GDX_RDRAM_STAGING_SIZE   ((size_t)0x100000u)   /* 1 MiB */

/* Allocated once at startup by gdx_rdram_init(), before bootproc(). */
extern unsigned char* gdx_rdram;

static inline void* gdx_rdram_ptr(unsigned int phys) {
    return gdx_rdram + phys;
}

/* Allocates the buffer, zeroes it, places the GfxPool pointer and registers the host range.
   Fatal on failure. */
void gdx_rdram_init(void);

/* Bump-allocate from the RDRAM arena. Fatal if exhausted. */
void* gdx_rdram_alloc_raw(size_t size, size_t align);

/* Bump-allocate session-lifetime data from the top of RDRAM, downward. Never reclaimed by the
   per-mode arena rewind. Fatal if exhausted. */
void* gdx_rdram_persist_alloc_raw(size_t size, size_t align);


/* ---- GDX_MEMPROBE: RDRAM occupancy probe (compile-time, OFF in the shipped build) --------
 * Answers "how much of the arena is ever touched, and by whom" without trusting the
 * allocators: gdx_rdram_probe_stats reports the two allocator cursors' high-waters AND a
 * top-down scan for the highest NON-ZERO byte below the persist region. The buffer is
 * calloc'd, so a non-zero byte is a byte something wrote -- including writes that never went
 * through gdx_rdram_alloc_raw (raw gdx_rdram_ptr(phys) stores, DMA, the audio LLE). The scan
 * is the honest measurement; the cursors are the attribution.
 *
 * Indices into the `out` array (all byte offsets/sizes):
 *   0 size          GDX_RDRAM_SIZE
 *   1 stagingBase   first byte of the ALLOC_PEEK staging block
 *   2 arenaStart    first byte the bump allocator may hand out
 *   3 bumpNow       current bump cursor
 *   4 bumpHwm       highest bump cursor ever reached (survives gdx_rdram_mode_reset)
 *   5 persistLow    lowest byte handed out by the persist (downward) allocator
 *   6 touchedHi     highest non-zero byte below persistLow, +1 (0 = the region is all zero)
 *   7 preArenaHi    highest non-zero byte below arenaStart, +1 (GfxPool reservation + staging)
 *   8 physHi        highest RDRAM offset the gfx bridge ever RESOLVED (classification
 *                   watermark; 0 unless n64_gfx_bridge.cpp was built with GDX_MEMPROBE)
 *   9 allocFail     count of gdx_rdram_alloc_raw/persist/peek refusals (always 0 on a run
 *                   that did not abort, since exhaustion is fatal) */
#define GDX_RDRAM_PROBE_SLOTS 10
void gdx_rdram_probe_stats(size_t* out /* [GDX_RDRAM_PROBE_SLOTS] */);

#ifdef __cplusplus
}
#endif
