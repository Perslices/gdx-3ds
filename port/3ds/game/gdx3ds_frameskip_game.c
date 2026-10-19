/* port/3ds/game/gdx3ds_frameskip_game.c — RACE0 (n60fs, [debug] frameskip = race0): the race
 * scope read from the game's own mode globals (read only). Contract: gdx3ds_frameskip.h.
 *
 * Compiled into gdx3ds_game (decomp include paths, decomp types). */
#include "global.h"
#include "fzx_game.h"
#include "fzx_racer.h"

extern s32 gGameMode;
extern s32 gQueuedGameMode;
extern s16 gGameModeChangeState;
extern s32 gRaceIntroTimer;
extern s32 gNumPlayers;

int gdx3ds_frameskip_game_race_scope(int* gameMode, int* changeState);
int gdx3ds_frameskip_game_racing(void);
int gdx3ds_frameskip_game_post_finish(void);

static int IsOnTrackRaceMode(s32 mode) {
    switch (mode) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

int gdx3ds_frameskip_game_race_scope(int* gameMode, int* changeState) {
    const s32 mode = GET_MODE(gGameMode);
    *gameMode = (int)mode;
    *changeState = (int)gGameModeChangeState;
    if (!IsOnTrackRaceMode(mode)) {
        return 0;
    }
    if (gQueuedGameMode != gGameMode) {
        return 0;
    }
    return gGameModeChangeState == GAMEMODE_UPDATE;
}

/* n60stereo: the race proper has started -- the intro camera and the countdown are over
 * (gRaceIntroTimer counts 460 (100 in some modes) down to 0 at the start, racer.c). */
int gdx3ds_frameskip_game_racing(void) {
    return gRaceIntroTimer == 0;
}

/* n60results: every human racer has crossed the line (racer.c sets RACER_STATE_FINISHED; a new race
 * clears it): from the FINISH banner on, the race scope holds only results screens. */
int gdx3ds_frameskip_game_post_finish(void) {
    const s32 players = gNumPlayers;
    if (!IsOnTrackRaceMode(GET_MODE(gGameMode)) || players < 1 || players > 4) {
        return 0;
    }
    for (s32 i = 0; i < players; i++) {
        if (!(gRacers[i].stateFlags & RACER_STATE_FINISHED)) {
            return 0;
        }
    }
    return 1;
}
