/* port/3ds/gfx/gdx3ds_hitch_ev.c — HITCH event counter storage (contract: gdx3ds_hitch.h).
 * Here, not in the executable, because gdx3ds-dl-tests links this library too and every hook
 * in it resolves these symbols (the gdx3ds_cov.c precedent). Host-compilable for the owned
 * interpreter's host tests (tools/ci-3ds.sh gdx_interp_host_tests): the clock reads 0 there. */
#include "gdx3ds_hitch.h"

#if defined(__3DS__)
#include <3ds.h>
#endif

uint32_t gdx3ds_hitch_ev[GDX_HITCH_EV_COUNT];
volatile int gdx3ds_hitch_aud_inflight = 0;

uint64_t gdx3ds_hitch_now(void) {
#if defined(__3DS__)
    return svcGetSystemTick();
#else
    return 0;
#endif
}
