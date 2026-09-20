// G-Diffuser — determinism pins. Contract in gdx_determinism.h.
#define _CRT_SECURE_NO_WARNINGS 1

#include "gdx_determinism.h"

#include <stdlib.h>

static int sInited = 0;
static int sSeedHave[3] = { 0, 0, 0 };
static unsigned int sSeed[3] = { 0, 0, 0 };
static int sBootHoldTicks = 0;
static int sAudioPin = 0;

// Desktop fallback: the pre-existing GDX_RAND_SEED1/2 env pin, read once.
static void LazyEnvInit(void) {
    int which;
    if (sInited) {
        return;
    }
    sInited = 1;
    for (which = 1; which <= 2; which++) {
        const char* env = getenv(which == 1 ? "GDX_RAND_SEED1" : "GDX_RAND_SEED2");
        if (env != NULL && env[0] != '\0') {
            sSeed[which] = (unsigned int)strtoul(env, NULL, 0);
            sSeedHave[which] = 1;
        }
    }
}

void gdx_det_init(int seed1Have, unsigned int seed1, int seed2Have, unsigned int seed2, int bootHoldTicks,
                  int audioPin) {
    sInited = 1;
    sSeedHave[1] = seed1Have ? 1 : 0;
    sSeed[1] = seed1;
    sSeedHave[2] = seed2Have ? 1 : 0;
    sSeed[2] = seed2;
    sBootHoldTicks = bootHoldTicks > 0 ? bootHoldTicks : 0;
    sAudioPin = audioPin ? 1 : 0;
}

int gdx_det_rand_seed(int which, unsigned int* out) {
    LazyEnvInit();
    if (which < 1 || which > 2 || out == NULL || !sSeedHave[which]) {
        return 0;
    }
    *out = sSeed[which];
    return 1;
}

int gdx_det_boot_hold_ticks(void) {
    LazyEnvInit();
    return sBootHoldTicks;
}

int gdx_det_audio_pin(void) {
    LazyEnvInit();
    return sAudioPin;
}

int gdx_det_active(void) {
    LazyEnvInit();
    return sSeedHave[1] || sSeedHave[2] || sBootHoldTicks > 0 || sAudioPin;
}
