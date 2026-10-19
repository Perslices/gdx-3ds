/* port/gdx_net_enabled_default.c -- see the header for why this is its own TU. */
#include "gdx_net_enabled_default.h"

int gdx_net_enabled_was_defaulted(int explicitVal) {
    return (explicitVal != 0 && explicitVal != 1);
}

int gdx_net_enabled_resolve(int explicitVal, int launchedViaNetloader) {
    if (gdx_net_enabled_was_defaulted(explicitVal)) {
        return launchedViaNetloader ? 1 : 0;
    }
    return explicitVal;
}
