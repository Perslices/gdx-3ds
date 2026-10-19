/* port/3ds/game/gdx3ds_c5tick_game.c — C5TICK helpers that need the decomp's types.
 *
 * 1. `[debug] dlwrite`: the display list's own scratch, which the write-set check leaves out
 *    of the hashed static image.
 *
 * These objects are written by every draw call by design and read by nothing the next tick
 * runs except the RSP task (the GfxPool holds the display list, matrices, vertices and
 * lights the list points at; the framebuffers and the RSP output buffer are render targets):
 *   D_8024DCE0[2]   the two GfxPools (sys_gfx.c Gfx_InitBuffer flips between them)
 *   D_1000000       the segment-1 alias GfxPool the list's matrix addresses resolve through
 *   gFrameBuffer1-3 the colour images
 *   gTaskOutputBuffer the RSP FIFO output buffer
 *
 * 2. `[debug] gstate_ext = 1`: a second digest next to [gstate]'s hash= (which covers the RNG,
 *    the frame counter and every racer's physics), for the state draw code can touch and that
 *    digest cannot see: the race intro timer, the clear countdown, and per racer the visibility
 *    bit the AI reads (machineLod != 0; the LOD tier itself is visual and levers change it) and
 *    soundEffectFlags (the approach-warning latch Racer_Draw's HUD tail sets). `acmd=` is the
 *    number of audio commands the game has queued: NOT part of the digest and NOT a gate --
 *    the game polls the audio thread's state (sequence status), so that count depends on the
 *    producer thread's timing and differs between two runs of one replay with the same levers.
 *
 * Compiled into gdx3ds_game (decomp include paths, decomp types for the sizes). */
#include "global.h"
#include "buffers.h"
#include "fzx_racer.h"

extern GfxPool D_8024DCE0[2];
extern GfxPool D_1000000;
extern FrameBuffer gFrameBuffer1;
extern FrameBuffer gFrameBuffer2;
extern FrameBuffer gFrameBuffer3;

typedef struct GdxDlwRange {
    unsigned long lo;
    unsigned long hi;
} GdxDlwRange;

static int AddRange(GdxDlwRange* out, int count, int max, const void* base, unsigned long size) {
    if (count >= max) {
        return count;
    }
    out[count].lo = (unsigned long) base;
    out[count].hi = (unsigned long) base + size;
    return count + 1;
}

int gdx3ds_dlw_scratch_ranges(GdxDlwRange* out, int max) {
    int count = 0;

    count = AddRange(out, count, max, D_8024DCE0, sizeof(GfxPool) * 2);
    count = AddRange(out, count, max, &D_1000000, sizeof(GfxPool));
    count = AddRange(out, count, max, &gFrameBuffer1, sizeof(FrameBuffer));
    count = AddRange(out, count, max, &gFrameBuffer2, sizeof(FrameBuffer));
    count = AddRange(out, count, max, &gFrameBuffer3, sizeof(FrameBuffer));
    count = AddRange(out, count, max, gTaskOutputBuffer, sizeof(gTaskOutputBuffer));
    return count;
}

/* ---- gstate_ext ---- */

/* The audio command ring's write cursor. audio/rom keeps it in a global; the Expansion Kit's
 * audio/disk keeps it inside gAudioCtx (o3dsek: the EK build did not link without this). */
#ifdef EXPANSION_KIT
#include "audio.h"
#define GSTATE_AUDIO_CMD_WRITE_POS (gAudioCtx.threadCmdWritePos)
#else
extern u8 gThreadCmdWritePos;
#define GSTATE_AUDIO_CMD_WRITE_POS (gThreadCmdWritePos)
#endif
extern s32 gRaceIntroTimer;
extern s32 D_i2_80106F10;
extern s32 gTotalRacers;

static u8 sAudioCmdPos = 0;
static u32 sAudioCmdCount = 0;

static u32 ExtMix(u32 h, const void* data, u32 len) {
    const u8* p = (const u8*) data;
    u32 i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Count the commands queued since the last call (the write cursor is a u8 ring index). */
static void ExtCountAudioCommands(void) {
    u8 writePos = GSTATE_AUDIO_CMD_WRITE_POS;

    sAudioCmdCount += (u8) (writePos - sAudioCmdPos);
    sAudioCmdPos = writePos;
}

unsigned int gdx3ds_gstate_ext_hash(unsigned int* audioCmds) {
    u32 h = 2166136261u;
    s32 count = gTotalRacers;
    s32 i;

    ExtCountAudioCommands();
    if (count < 0) {
        count = 0;
    } else if (count > TOTAL_RACER_COUNT) {
        count = TOTAL_RACER_COUNT;
    }
    h = ExtMix(h, &gRaceIntroTimer, sizeof(gRaceIntroTimer));
    h = ExtMix(h, &D_i2_80106F10, sizeof(D_i2_80106F10));
    for (i = 0; i < count; i++) {
        u8 visible = gRacers[i].machineLod != 0;

        h = ExtMix(h, &visible, sizeof(visible));
        h = ExtMix(h, &gRacers[i].soundEffectFlags, sizeof(gRacers[i].soundEffectFlags));
    }
    if (audioCmds != NULL) {
        *audioCmds = sAudioCmdCount;
    }
    return h;
}
