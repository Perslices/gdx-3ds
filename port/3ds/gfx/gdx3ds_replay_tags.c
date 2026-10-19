/* port/3ds/gfx/gdx3ds_replay_tags.c -- C6 pose-replay: the two objects the owned interpreter
 * touches (gdx3ds_replay.h). Plain C with no 3DS dependency, so the interpreter's host test
 * (tools/ci-3ds.sh gdx_interp_host_tests) links it; the module proper (gdx3ds_replay.cpp) sets
 * gdx3ds_replay_interp when [native] replay is latched on. */
#include "gdx3ds_replay.h"

const GdxReplayInterpHooks* gdx3ds_replay_interp = 0;
uint16_t gdx3ds_replay_vtx_tag[GDX_REPLAY_VTX_SLOTS];
uint32_t gdx3ds_replay_vtx_src[GDX_REPLAY_VTX_SLOTS];
uint16_t gdx3ds_replay_nb_tag = GDX_REPLAY_TAG_UNKNOWN;
