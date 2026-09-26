/* port/3ds/include/gdx3ds_heapcfg.h -- build-time control of libctru's two heap grants
 * (NEXT.md C4b / C4e). Implementation: port/3ds/gdx3ds_heapcfg.c. Host test:
 * port/3ds/test/gdx3ds_heapcfg_host_tests.c.
 *
 * WHY THIS IS BUILD-TIME AND CAN NEVER BE AN INI LEVER
 * ---------------------------------------------------
 * libctru's __system_allocateHeaps() runs from crt0, before main() and before a single
 * static constructor, and it is what carves the malloc heap and the linear heap out of the
 * process's APPLICATION commit budget. By the time gdiffuser.ini has been opened the split
 * is already made and svcControlMemory has already been called. The only lever that reaches
 * it is the pair of weak (`V`) symbols the function reads on entry, __ctru_heap_size and
 * __ctru_linear_heap_size, which this port otherwise leaves at 0. A strong definition in
 * this build overrides the weak one at link time; that is the whole mechanism.
 *
 * THE FORMULA, TRANSCRIBED FROM THE ARCHIVE WE LINK
 * ------------------------------------------------
 * `arm-none-eabi-objdump -d` of /opt/devkitpro/libctru/lib/libctru.a(allocateHeaps.o),
 * re-read 2026-09-24 rather than recalled (the register->global mapping below is pinned by
 * the `heap = avail - linear` store at 0x138, which is the only branch that names them):
 *
 *   avail = (svcGetResourceLimitLimitValues(COMMIT)
 *            - svcGetResourceLimitCurrentValues(COMMIT)) & ~0xFFF
 *   if (heap + linear > avail) svcBreak(USERBREAK_PANIC);   <-- reads the globals AS WRITTEN
 *   if (heap == 0) {
 *       if (linear == 0) {                                  # the default this port has
 *           linear = (avail >> 1) & ~0xFFF;
 *           heap   = avail - linear;
 *           if (heap > 24 MiB) {                            # strictly greater
 *               heap = 24 MiB; linear = avail - 24 MiB;
 *               if (linear > 32 MiB) { linear = 32 MiB; heap = avail - 32 MiB; }
 *           }
 *       } else {
 *           heap = avail - linear;                          # NO 24 MiB CAP ON THIS PATH
 *       }
 *   } else if (linear == 0) {
 *       linear = avail - heap;                              # no cap here either
 *   }
 *
 * Two consequences the whole Old 3DS track turns on:
 *   1. With both globals at 0 and avail between 24 MiB and 24+32 MiB, the malloc heap is
 *      EXACTLY 24 MiB -- a constant, independent of how much memory the console actually
 *      has. That is the Old 3DS memtype-0 regime.
 *   2. Setting __ctru_linear_heap_size alone escapes the cap entirely (C4b).
 *
 * The svcBreak on line 3 reads the globals BEFORE the formula runs, so a build that pins
 * heap+linear above the device's real avail dies in crt0 with no output whatsoever -- not a
 * [fatal], not a log line, a black screen. That is the failure mode of an over-large
 * GDX_3DS_SIM_AVAIL_KIB, and it is why the sim knob is documented in terms of a REAL
 * console's budget rather than an arbitrary number.
 */
#ifndef GDX3DS_HEAPCFG_H
#define GDX3DS_HEAPCFG_H

/* --- libctru's algorithm as constant expressions -------------------------------------
 * These are usable in a file-scope initialiser, which is the point: the pinned values have
 * to exist as data in .data before any code runs. Kept as macros in a header, rather than
 * inline in the TU, so the host test can check them against grants the guest actually
 * printed instead of against my reading of the disassembly. */
#define GDX3DS_HEAPCFG_PAGE_DOWN(x) ((unsigned long)(x) & ~0xFFFUL)
#define GDX3DS_HEAPCFG_HEAP_CAP     (24UL * 1024UL * 1024UL)
#define GDX3DS_HEAPCFG_LINEAR_CAP   (32UL * 1024UL * 1024UL)

/* Both globals left at 0 -- what this port ships today:
 *   linear = (avail >> 1) & ~0xFFF;  heap = avail - linear;  then the two caps. */
#define GDX3DS_HEAPCFG_L0(a) GDX3DS_HEAPCFG_PAGE_DOWN((a) >> 1)
#define GDX3DS_HEAPCFG_H0(a) ((a) - GDX3DS_HEAPCFG_L0(a))

#define GDX3DS_HEAPCFG_AUTO_HEAP(avail)                                                    \
    (GDX3DS_HEAPCFG_H0(GDX3DS_HEAPCFG_PAGE_DOWN(avail)) <= GDX3DS_HEAPCFG_HEAP_CAP          \
         ? GDX3DS_HEAPCFG_H0(GDX3DS_HEAPCFG_PAGE_DOWN(avail))                               \
         : ((GDX3DS_HEAPCFG_PAGE_DOWN(avail) - GDX3DS_HEAPCFG_HEAP_CAP) > GDX3DS_HEAPCFG_LINEAR_CAP \
                ? (GDX3DS_HEAPCFG_PAGE_DOWN(avail) - GDX3DS_HEAPCFG_LINEAR_CAP)             \
                : GDX3DS_HEAPCFG_HEAP_CAP))

#define GDX3DS_HEAPCFG_AUTO_LINEAR(avail)                                                  \
    (GDX3DS_HEAPCFG_H0(GDX3DS_HEAPCFG_PAGE_DOWN(avail)) <= GDX3DS_HEAPCFG_HEAP_CAP          \
         ? GDX3DS_HEAPCFG_L0(GDX3DS_HEAPCFG_PAGE_DOWN(avail))                               \
         : ((GDX3DS_HEAPCFG_PAGE_DOWN(avail) - GDX3DS_HEAPCFG_HEAP_CAP) > GDX3DS_HEAPCFG_LINEAR_CAP \
                ? GDX3DS_HEAPCFG_LINEAR_CAP                                                 \
                : (GDX3DS_HEAPCFG_PAGE_DOWN(avail) - GDX3DS_HEAPCFG_HEAP_CAP)))

/* __ctru_linear_heap_size pinned, __ctru_heap_size left at 0 -- the C4b branch, no cap. */
#define GDX3DS_HEAPCFG_HEAP_GIVEN_LINEAR(avail, lin) (GDX3DS_HEAPCFG_PAGE_DOWN(avail) - (unsigned long)(lin))

/* --- the receipt -------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* What this BUILD pinned, for the [mem-budget] line. Always linked; with no knob set it
 * reports mode "auto" and two zeros, which is the shipped configuration. These describe
 * the build's intent; envGetHeapSize()/envGetLinearHeapSize() report what libctru actually
 * committed, and the two are printed side by side precisely so a knob that silently failed
 * to apply is visible as a disagreement rather than as a plausible number. */
const char* gdx3ds_heapcfg_mode(void);       /* "auto" | "sim" | "linear" | "sim+linear" */
unsigned long gdx3ds_heapcfg_pin_heap(void);   /* bytes, 0 = not pinned */
unsigned long gdx3ds_heapcfg_pin_linear(void); /* bytes, 0 = not pinned */
unsigned long gdx3ds_heapcfg_sim_avail(void);  /* bytes of simulated commit budget, 0 = off */

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_HEAPCFG_H */
