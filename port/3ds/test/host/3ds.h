/* port/3ds/test/host/3ds.h — the slice of <3ds.h> that gdx3ds_dynlod.c uses, for the
 * host unit test (tools/ci-3ds.sh: gdx3ds_dynlod_host_tests). NEVER on the 3DS include
 * path: the real libctru header wins there.
 *
 * Only two libctru surfaces reach the controller: svcGetSystemTick (the per-frame render
 * task wall) and svcOutputDebugString (receipt lines). The tick source here is a variable
 * the test drives, which is what lets one process replay a synthetic frame-time sequence.
 * The clock constants are copied verbatim from libctru's 3ds/os.h so the controller's
 * integer tick -> tenths-of-ms arithmetic is bit-for-bit what the device runs. */
#ifndef GDX3DS_TEST_HOST_3DS_H
#define GDX3DS_TEST_HOST_3DS_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Result;

/* libctru 3ds/os.h (verbatim). */
#define SYSCLOCK_SOC (16756991u)
#define SYSCLOCK_SYS (SYSCLOCK_SOC * 2)
#define SYSCLOCK_SDMMC (SYSCLOCK_SYS * 2)
#define SYSCLOCK_ARM9 (SYSCLOCK_SYS * 4)
#define SYSCLOCK_ARM11 (SYSCLOCK_ARM9 * 2)
#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)
#define CPU_TICKS_PER_USEC (SYSCLOCK_ARM11 / 1000000.0)

/* Test-driven clock: gdx3ds_dynlod_host_tests.c owns it and advances it by the synthetic
 * frame's render-task wall between task_begin and task_end. */
extern u64 gdx3ds_test_tick;
static inline u64 svcGetSystemTick(void) {
    return gdx3ds_test_tick;
}

Result svcOutputDebugString(const char* str, s32 length);

#endif /* GDX3DS_TEST_HOST_3DS_H */
