/* port/3ds/gdx3ds_heapcfg.c -- the build-time heap knob (NEXT.md C4b / C4e).
 * Contract, mechanism and the disassembled libctru formula: port/3ds/include/gdx3ds_heapcfg.h.
 *
 * DEFAULT OFF. With neither CMake cache variable set, GDX3DS_SIM_AVAIL_BYTES and
 * GDX3DS_PIN_LINEAR_BYTES are both 0, this TU defines no __ctru_* symbol at all, libctru's
 * weak zeros survive, and the resulting .3dsx is the shipped build -- the knob cannot change
 * a byte of it. That is deliberate: this is a change to the memory layout of every build in
 * the fork, and it stays off until a console says it is needed.
 *
 * THE THREE MODES
 *   sim         GDX_3DS_SIM_AVAIL_KIB=N alone. Pins BOTH globals to exactly what libctru
 *               would have computed on a console whose commit budget is N KiB. This is C4e:
 *               it lets the emulator -- whose "Old 3DS" is memtype 2, the 96 MB extended
 *               layout, and is therefore MORE generous than the console -- reproduce the
 *               64 MB memtype-0 grant that is the binding constraint.
 *   linear      GDX_3DS_LINEAR_HEAP_KIB=N alone. Pins ONLY __ctru_linear_heap_size and
 *               leaves the heap at 0, so libctru takes its `heap = avail - linear` branch
 *               against the REAL device budget with no 24 MiB cap. This is C4b, and it is
 *               the form that ships if it ever ships.
 *   sim+linear  Both. Pins heap = simAvail - linear. This is C4b measured under C4e: the
 *               same arithmetic the `linear` mode would get on a memtype-0 console, made
 *               reproducible on the emulator lane.
 *
 * The values are computed here, in C, from the macros in the header, rather than in CMake:
 * a host test can then check the same expressions against grants the guest actually printed
 * (port/3ds/test/gdx3ds_heapcfg_host_tests.c), which CMake arithmetic could not be.
 */

#include "gdx3ds_heapcfg.h"

#ifndef GDX3DS_SIM_AVAIL_BYTES
#define GDX3DS_SIM_AVAIL_BYTES 0UL
#endif
#ifndef GDX3DS_PIN_LINEAR_BYTES
#define GDX3DS_PIN_LINEAR_BYTES 0UL
#endif

/* NOTE for anyone editing: the #if conditions below may only mention the two raw CMake
 * macros. Every macro in gdx3ds_heapcfg.h contains a cast, and a cast is not a valid token
 * in a #if -- the preprocessor would reject it. So the arithmetic stays in the
 * initialisers, and only these two plain integers steer the conditionals. */
#define GDX3DS_HEAPCFG_SIM  ((unsigned long)(GDX3DS_SIM_AVAIL_BYTES))
#define GDX3DS_HEAPCFG_LIN  ((unsigned long)(GDX3DS_PIN_LINEAR_BYTES))

#if (GDX3DS_SIM_AVAIL_BYTES) != 0 || (GDX3DS_PIN_LINEAR_BYTES) != 0

/* What each mode pins. Read the header's transcription alongside this table.
 *   sim only:    heap = AUTO_HEAP(sim),               linear = AUTO_LINEAR(sim)
 *   linear only: heap = 0 (libctru computes it),      linear = LIN
 *   both:        heap = PAGE_DOWN(sim) - LIN,         linear = LIN                */
#if (GDX3DS_SIM_AVAIL_BYTES) != 0 && (GDX3DS_PIN_LINEAR_BYTES) != 0
#define GDX3DS_HEAPCFG_PINNED_HEAP   GDX3DS_HEAPCFG_HEAP_GIVEN_LINEAR(GDX3DS_HEAPCFG_SIM, GDX3DS_HEAPCFG_LIN)
#define GDX3DS_HEAPCFG_PINNED_LINEAR GDX3DS_HEAPCFG_LIN
#define GDX3DS_HEAPCFG_MODE_STR      "sim+linear"
#elif (GDX3DS_SIM_AVAIL_BYTES) != 0
#define GDX3DS_HEAPCFG_PINNED_HEAP   GDX3DS_HEAPCFG_AUTO_HEAP(GDX3DS_HEAPCFG_SIM)
#define GDX3DS_HEAPCFG_PINNED_LINEAR GDX3DS_HEAPCFG_AUTO_LINEAR(GDX3DS_HEAPCFG_SIM)
#define GDX3DS_HEAPCFG_MODE_STR      "sim"
#else
#define GDX3DS_HEAPCFG_PINNED_HEAP   0UL
#define GDX3DS_HEAPCFG_PINNED_LINEAR GDX3DS_HEAPCFG_LIN
#define GDX3DS_HEAPCFG_MODE_STR      "linear"
#endif

/* The override itself. libctru declares both as `u32` and defines them weak (`V` in nm);
 * a strong definition here wins at link time. __system_allocateHeaps reads them in crt0.
 *
 * `used` because nothing in this image references these by name -- the only reader is
 * libctru's crt0 path, and -ffunction-sections/--gc-sections would otherwise be entitled
 * to drop a .data object no TU mentions. */
#if (GDX3DS_SIM_AVAIL_BYTES) != 0   /* `linear` mode deliberately leaves the heap global at 0 */
__attribute__((used)) unsigned int __ctru_heap_size = (unsigned int)(GDX3DS_HEAPCFG_PINNED_HEAP);
#endif
__attribute__((used)) unsigned int __ctru_linear_heap_size = (unsigned int)(GDX3DS_HEAPCFG_PINNED_LINEAR);

#else /* knob off -- define nothing, leave libctru's weak zeros alone */

#define GDX3DS_HEAPCFG_PINNED_HEAP   0UL
#define GDX3DS_HEAPCFG_PINNED_LINEAR 0UL
#define GDX3DS_HEAPCFG_MODE_STR      "auto"

#endif

const char* gdx3ds_heapcfg_mode(void) {
    return GDX3DS_HEAPCFG_MODE_STR;
}

unsigned long gdx3ds_heapcfg_pin_heap(void) {
    return (unsigned long)(GDX3DS_HEAPCFG_PINNED_HEAP);
}

unsigned long gdx3ds_heapcfg_pin_linear(void) {
    return (unsigned long)(GDX3DS_HEAPCFG_PINNED_LINEAR);
}

unsigned long gdx3ds_heapcfg_sim_avail(void) {
    return GDX3DS_HEAPCFG_SIM;
}
