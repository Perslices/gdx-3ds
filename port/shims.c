// G-Diffuser — port shims.
// Minimal definitions for the libultra/N64 symbols libultraship does NOT provide and the
// decomp references. Three kinds live here, and the difference matters when reading any one:
//   * REAL implementations — _Printf, Arena_Allocate/StartInit, the bcmp/bcopy and CRT
//     wrappers, LeoTestUnitReady's deliberate "no medium" answer. Load-bearing behavior; see
//     each one's own comment before changing it.
//   * FAILURE ANSWERS — osPfs*/osEPi*/osDriveRomInit/osAiSetFrequency. No real work routes
//     through them: cart asset DMA goes through Dma_LoadAssets + gdx_segment_source, saves
//     through port/sram_buffer.cpp. Some ARE still called (sys_main.c assigns
//     gDriveRomHandle = osDriveRomInit()), so their return values are a contract, not dead
//     code: -1 means "no such device", which is what the port wants the game to conclude.
//   * ZERO-SIZED DATA MARKERS — the audio ROM-segment / microcode symbols at the bottom.
//
// C has no signature mangling, so the linker resolves these by name and the simplified
// prototypes below are enough to satisfy the decomp's references.

#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "port_log.h"
#if !defined(_WIN32)
#include <malloc.h>
#endif

/* Heap counters for the allocation-failure receipt. newlib (3DS) and glibc both answer
   mallinfo; on Windows there is no equivalent and the receipt degrades to the size alone. */
void gdx_host_heap_stats(unsigned long* used, unsigned long* avail, unsigned long* arena) {
#if !defined(_WIN32)
    struct mallinfo mi = mallinfo();
    if (used != NULL)  { *used  = (unsigned long)mi.uordblks; }
    if (avail != NULL) { *avail = (unsigned long)mi.fordblks; }
    if (arena != NULL) { *arena = (unsigned long)mi.arena; }
#else
    if (used != NULL)  { *used  = 0; }
    if (avail != NULL) { *avail = 0; }
    if (arena != NULL) { *arena = 0; }
#endif
}

#define GDX_LEO_TEST_UNIT_MR 0x01
#define GDX_LEO_ERROR_MEDIUM_NOT_PRESENT 42

// ---- _Printf ----------------------------------------------------------------
// The libultra formatter behind every _Printf-based text path in the game: the Course Edit
// info panels, Create Machine's machine names, the disk file list, the N64 crash screen.
// Stubbing it to `return 0` is silent rather than fatal — callers test the returned count
// before drawing, so a zero just skips the draw and blanks the text.
//
// Contract, from decomp/include/PR/xstdio.h:
//     typedef char *outfun(char*, const char*, size_t);
//     int _Printf(outfun prout, char *arg, const char *fmt, va_list args);
// `outfun` names a function TYPE, so the parameter is a function pointer. The formatter
// hands its bytes to `prout` and returns the character count; see the concrete consumer
// func_xk1_800290D0 (expansion_kit/A6340.c:24), a memcpy returning the advanced pointer.
//
// Two behaviours here are load-bearing:
//   * The count is the ONLY length signal. Callers do not rely on NUL termination —
//     func_xk1_8002924C walks exactly the returned number of bytes out of an
//     uninitialised char[0x100]. So the returned value must be what was actually handed
//     to prout, never vsnprintf's would-have-been length.
//   * The original streamed unbounded output into a caller buffer whose size it was never
//     told. Truncating at our own buffer is strictly safer than the hardware behaviour,
//     and 255 bytes clears the longest format any caller uses.
typedef char* gdx_prout_fn(char*, const char*, size_t);

int _Printf(gdx_prout_fn* prout, char* arg, const char* fmt, va_list args) {
    char buf[256];
    int wanted;
    size_t emitted;

    if (prout == NULL || fmt == NULL) {
        return -1;
    }

    wanted = vsnprintf(buf, sizeof(buf), fmt, args);
    if (wanted < 0) {
        return -1;
    }

    // vsnprintf reports the untruncated length but writes at most sizeof(buf)-1
    // characters, so clamp before reporting it as bytes the caller may read.
    emitted = ((size_t) wanted < sizeof(buf)) ? (size_t) wanted : (sizeof(buf) - 1);

    prout(arg, buf, emitted);
    return (int) emitted;
}

// ---- libultra function stubs ------------------------------------------------
// Controller Pak: report "no Pak" unconditionally. F-Zero X saves to cart SRAM, which the port
// backs with port/sram_buffer.cpp, so no game feature depends on a Controller Pak being present.
int osPfsInitPak(void)       { return -1; }
int osPfsAllocateFile(void)  { return -1; }
int osPfsReadWriteFile(void) { return -1; }
int osPfsFindFile(void)      { return -1; }

// PI / EPI (cart + 64DD register I/O): report "no device". Cart reads never reach here — they go
// through Dma_LoadAssets / gdx_segment_source — and the 64DD drive is emulated in port/n64_leo.c
// against the disk image, not through these registers.
//
// osEPiReadIo MUST fill the out-param: LeoDD_CheckPresence (EK=OFF boot, sys_main.c) tests
// `status & LEO_STATUS_PRESENCE_MASK` from an uninitialized stack u32 — with the old arg-less
// stub the value was whatever the stack held (0 on the 3DS boot), the drive read as "present",
// and LeoFault_LoadFonts dereferenced the NULL gDriveRomHandle (Write32 @0x13 spin, black
// screens). All-ones mirrors a floating bus with no device; callers passing NULL (EA90.c's
// register smoke-test) are tolerated.
int osEPiReadIo(void* pihandle, unsigned int devAddr, unsigned int* data) {
    (void)pihandle;
    (void)devAddr;
    if (data != NULL) {
        *data = 0xFFFFFFFFu;
    }
    return -1;
}
int osEPiWriteIo(void)       { return -1; }
int osEPiLinkHandle(void)    { return  0; }
int osDriveRomInit(void)     { return -1; }

// osStopThread and __osSetHWIntrRoutine deliberately have no shim here: the decomp's real
// libultra/os scheduler (stopthread.c, sethwinterrupt.c) provides them.

// libultra debug error hook. Some libultra paths reference this as a function, so it must be
// a real no-op function, not a data symbol that merely satisfies the linker.
void __osError(short code, short numArgs, ...) {
    (void)code;
    (void)numArgs;
}

// Audio interface (libultraship provides osAiSetNextBuffer but not this).
int osAiSetFrequency(void) { return 0; }

#ifndef EXPANSION_KIT
// 64DD / leo boot — stubbed for the US-only build. With EXPANSION_KIT enabled
// port/n64_leo.c provides the disk-image-backed implementations of both.
void LeoBootGame(void) {}

// Base-game 64DD probe: the PC port has no inserted disk. Report that state
// explicitly so title-screen logic does not mistake a zeroed status word for
// a fatal drive condition and cover the frame with its black error overlay.
int LeoTestUnitReady(unsigned char* status) {
    if (status != NULL) {
        *status = GDX_LEO_TEST_UNIT_MR;
    }
    return GDX_LEO_ERROR_MEDIUM_NOT_PRESENT;
}
#endif

// libc: BSD byte-compare not in the MSVC CRT. glibc provides both natively (with size_t
// signatures that would conflict), so these shims are Windows-only.
#ifdef _WIN32
int bcmp(const void* a, const void* b, int n) { return memcmp(a, b, (size_t)n); }
void bcopy(const void* src, void* dst, int n) { memmove(dst, src, (size_t)n); }
#endif

// Host CRT wrappers for decomp-side code. The gdiffuser_game object target must not include
// MSVC system headers, so it calls these wrappers instead of relying on implicit CRT prototypes.

// ---- C-side allocation-failure receipt --------------------------------------------------
// WHY THIS EXISTS. main_3ds.cpp replaces global operator new and logs "[fatal] operator new
// failed" with the size, the heap state and a return address. That covers `operator new` and
// nothing else -- and on this port `operator new` is 6.09 MB of a 44.46 MB heap. The other
// 38.36 MB (86%) is plain C malloc/calloc, and it reported NOTHING: a C allocation returning
// NULL under a tight grant produced a frozen frame counter, a [tt] park line and not one
// diagnostic (o3dssim-progress.md §4.2b, the frame-3 wedge that cost a whole bisect).
//
// This is the receipt. It is NOT a universal malloc hook -- it is called at the C sites that
// hold the big blocks (the RDRAM arena, the segment-source whole-file buffers, the venue
// texture cache, the raw ROM buffer), which is ~31 MB of that 38 MB. Sites it does not cover
// are still silent, and the progress doc says which.
//
// Always on: it costs one already-taken NULL branch on a path that was about to fail anyway,
// and gating a failure receipt behind a build flag would put it in every build EXCEPT the one
// that is failing.
// C4c UPDATE: port/3ds/gdx3ds_mallocwrap.c now calls this from inside the allocator itself
// (-Wl,--wrap=_malloc_r), so on a 3DS build the coverage above is no longer "~31 MB of the
// 38 MB" -- it is every byte the process mallocs. Two things follow.
//
// (1) The `ra` argument. A named site can say WHAT it was allocating; the allocator can only
//     say WHO asked, so the return address is the whole attribution there. gdx_alloc_fail
//     keeps its old signature and passes NULL, and the receipt then omits the field rather
//     than printing a fake one.
//
// (2) The reentry guard, which is not belt-and-braces. This runs when malloc has just
//     returned NULL. It formats through vsnprintf and writes through the port log, and
//     newlib's stdio allocates its buffer lazily on first use -- so the first [alloc-fail]
//     on a heap that is genuinely full can itself call malloc, fail, and re-enter here.
//     Unbounded recursion on the out-of-memory path turns a diagnosable OOM into a stack
//     overflow with no receipt at all, which is exactly what this instrument exists to
//     prevent. The flag is a plain int: a torn read costs at most one dropped duplicate
//     line, and the dropped count rides the next line that does get out.
static volatile int sAllocFailBusy = 0;
static volatile unsigned long sAllocFailDropped = 0;

void gdx_alloc_fail_ra(const char* site, size_t bytes, void* ra) {
    extern void gdx_dbg_logf(const char* fmt, ...);
    unsigned long used = 0, avail = 0, arena = 0;
    if (sAllocFailBusy) {
        sAllocFailDropped++;
        return;
    }
    sAllocFailBusy = 1;
    gdx_host_heap_stats(&used, &avail, &arena);
    if (ra != NULL) {
        gdx_dbg_logf("[alloc-fail] site=%s bytes=%d heapUsed=%d heapFree=%d arena=%d ra=%p"
                     " dropped=%d\n",
                     site != NULL ? site : "?", (int)bytes, (int)used, (int)avail, (int)arena,
                     ra, (int)sAllocFailDropped);
    } else {
        gdx_dbg_logf("[alloc-fail] site=%s bytes=%d heapUsed=%d heapFree=%d arena=%d\n",
                     site != NULL ? site : "?", (int)bytes, (int)used, (int)avail, (int)arena);
    }
    sAllocFailBusy = 0;
}

void gdx_alloc_fail(const char* site, size_t bytes) {
    gdx_alloc_fail_ra(site, bytes, NULL);
}

void* gdx_host_calloc(size_t count, size_t size) {
    void* p = calloc(count, size);
    if (p == NULL && count != 0 && size != 0) {
        gdx_alloc_fail("gdx_host_calloc", count * size);
    }
    return p;
}
void gdx_host_free(void* ptr) { free(ptr); }
void  gdx_host_exit(int status) { exit(status); }
/* Every abort so far reached the 3DS log as a bare "[fatal] abort() called": the reason
   breadcrumbs at the call sites rode the TRACE gate, which the 3DS build never sets. Name the
   caller unconditionally (symbolize with addr2line against G-Diffuser-3DS.elf). */
void  gdx_host_abort(void) {
    extern void gdx_dbg_logf(const char* fmt, ...);
    gdx_dbg_logf("[fatal] gdx_host_abort from ra=%p\n", __builtin_return_address(0));
    abort();
}

// ---- Memory arena (port reimplementation) ----------------------------------
// Arena_Allocate carves from the host RDRAM bump allocator (gdx_rdram_alloc_raw; the buffer is
// GDX_RDRAM_SIZE == 16 MB, see port/n64_rdram.h). The whole RDRAM buffer is registered once at
// startup in gdx_rdram_init() — no per-allocation gdx_register_host_range call needed here.
void* gdx_rdram_alloc_raw(size_t size, size_t align); // defined in decomp_port.c
void* gdx_rdram_peek_raw(size_t size, size_t align);  // non-committing peek
void  gdx_rdram_mode_reset(void);                     // per-mode arena rewind

/* Console arena semantics. ALLOC_PEEK is transient scratch: the cursor is not advanced and
   the next committed allocation may overwrite it, which is exactly how the decomp's texture
   loader stages mio0 input. FRONT and BACK both commit from the single bump region — the
   console's front/back split is an optimization, not a semantic callers depend on. */
void* Arena_Allocate(int allocationType, size_t size) {
    if (allocationType == 1 /* ALLOC_PEEK, sys.h */) {
        return gdx_rdram_peek_raw(size, 16u);
    }
    return gdx_rdram_alloc_raw(size, 16u);
}
void  Arena_StartInit(void)        { gdx_rdram_mode_reset(); }
void  Arena_DefaultStartInit(void) { gdx_rdram_mode_reset(); }
void  Arena_EndInit(void)          {}

// ---- N64 ROM-segment / audio-microcode symbols ------------------------------
// Address markers only, so the decomp's audio globals link. The real audio payloads are NOT read
// through these: the bases live as PORT_audio_{bank,seq,table}_ROM_START in
// decomp/include/port_segment_addrs.h and are served archive-first by gdx_segment_source. aspMain*
// is the RSP microcode task pointer, which the port never executes -- osSpTaskStartGo routes
// M_AUDTASK to the software interpreter instead.
unsigned char audio_bank_ROM_START[1];
unsigned char audio_table_ROM_START[1];
unsigned char audio_seq_ROM_START[1];
unsigned char audio_context_VRAM[1];
unsigned char audio_context_VRAM_END[1];
unsigned long long aspMainTextStart[1];
unsigned long long aspMainDataStart[1];
unsigned long long aspMainDataEnd[1];
