/* port/3ds/game/gdx3ds_replay_game.c -- C6 pose-replay: the game-side reads it needs, with the
 * decomp's own types (compiled into gdx3ds_game; the replay module links it weakly).
 *
 * READ ONLY. Nothing here writes game state: the replay must leave every [gstate] digest of
 * every tick identical, and it only ever looks.
 *
 *   - The two GfxPools (sys_gfx.c flips gGfxPool between D_8024DCE0[0] and [1] every game
 *     frame) and the segment-1 alias D_1000000 the display list's matrix addresses name: a
 *     G_MTX whose source lies in a pool is a per-tick matrix (camera, machines, shadows,
 *     background sprites), and the same offset in the NEXT tick's pool holds that tick's value.
 *   - The cut signals: game mode, mode change, transition, pause, player count, the race's
 *     clear countdown, per racer the LOD (0 = not visible) and the crash/retire/finish bits,
 *     and camera 0's eye and forward axis (the camera-delta gate). */
#include "global.h"
#include "buffers.h"
#include "fzx_racer.h"
#include "fzx_camera.h"

#include "gdx3ds_replay.h"

extern GfxPool D_8024DCE0[2];
extern GfxPool D_1000000;
extern GfxPool* gGfxPool;
extern s32 gGameMode;
extern s16 gGameModeChangeState;
extern s8 gGamePaused;
extern s32 gNumPlayers;
extern s32 gTotalRacers;
extern s32 gTransitionState;
extern s32 D_i2_80106F10;


static unsigned char RacerFlags(const Racer* racer) {
    unsigned char f = 0;

    if (racer->stateFlags & RACER_STATE_CRASHED) {
        f |= 1;
    }
    if (racer->stateFlags & RACER_STATE_RETIRED) {
        f |= 2;
    }
    if (racer->stateFlags & RACER_STATE_FINISHED) {
        f |= 4;
    }
    return f;
}

static int CurrentPool(void) {
    if (gGfxPool == &D_8024DCE0[0]) {
        return 0;
    }
    if (gGfxPool == &D_8024DCE0[1]) {
        return 1;
    }
    return -1;
}

static void CaptureRacers(GdxReplayGame* out) {
    s32 count = gTotalRacers;
    s32 i;

    if (count < 0) {
        count = 0;
    } else if (count > GDX_REPLAY_GAME_RACERS) {
        count = GDX_REPLAY_GAME_RACERS;
    }
    out->totalRacers = gTotalRacers;
    for (i = 0; i < GDX_REPLAY_GAME_RACERS; i++) {
        if (i < count) {
            s32 lod = gRacers[i].machineLod;
            out->lod[i] = (unsigned char) (lod < 0 ? 0 : (lod > 255 ? 255 : lod));
            out->flags[i] = RacerFlags(&gRacers[i]);
        } else {
            out->lod[i] = 0;
            out->flags[i] = 0;
        }
    }
}

void gdx3ds_replay_game_capture(GdxReplayGame* out) {
    const Camera* cam = &gCameras[0];

    out->poolBase = (const unsigned char*) &D_8024DCE0[0];
    out->aliasBase = (const unsigned char*) &D_1000000;
    out->poolSize = (unsigned int) sizeof(GfxPool);
    out->curPool = CurrentPool();
    out->gameMode = gGameMode;
    out->modeChange = gGameModeChangeState;
    out->transition = gTransitionState;
    out->paused = gGamePaused;
    out->numPlayers = gNumPlayers;
    out->clearTimer = D_i2_80106F10;
    CaptureRacers(out);
    out->eye[0] = cam->eye.x;
    out->eye[1] = cam->eye.y;
    out->eye[2] = cam->eye.z;
    out->fwd[0] = cam->basis.x.x;
    out->fwd[1] = cam->basis.x.y;
    out->fwd[2] = cam->basis.x.z;
}
