/* port/3ds/gfx/gdx3ds_replay.h -- C6 pose-replay, "60 Hz motion" (docs/research/native/
 * c6replay-progress.md, r60-research.md P1). Levers default OFF.
 *
 * WHAT IT DOES. Under `[debug] frameskip = steady` at D = 2 the console renders every other
 * logic tick; the tick in between still runs its logic and builds its display list, and its
 * GPU task is dropped. With `[native] replay = 1` that skipped tick is PRESENTED anyway: the
 * last rendered frame A's command list, GX queue, VBO pool and retained meshes are all still
 * intact (the C6 contract, c5overlap-progress.md section 4), so the replay patches A's uniform
 * writes in the command list in place with the skipped tick B's exact camera and machine
 * matrices, re-runs A's GX queue with the final display transfer re-targeted at the current
 * back buffer, and swaps from the queue's finish callback. 60 images per second of true game
 * poses from a 30 Hz render budget; the HUD, particles and anything not driven by a matrix stay
 * at A (the render rate). Game state is never written: every input is read.
 * At D = 3 (`[native] replay_dmax`, default 3) the slot's LAST dropped tick (built by its first
 * skip, in A's own pool) is presented the same way at the next slot's start and held for two
 * vblanks: two images per three vblanks instead of one. Only a render whose slot can replay is
 * recorded (gdx3ds_replay_rec); every other frame runs the lever-off path at no cost.
 *
 * HOW A UNIFORM WRITE IS MAPPED TO A POSE. The owned interpreter reports every G_MTX / POPMTX
 * (the source pointer, so a matrix that lives in the game's double-buffered GfxPool is known by
 * its pool offset) and tags every vertex it transforms with the matrix state it used. Every draw
 * that reaches the backend carries such a tag (legacy CPU-transformed batches, native batches,
 * CPU and GPU machine meshes, the retained/resident track); a draw whose tag changes forces the
 * uniform that carries the pose (the legacy `projection`, the gpuxf xmvp/lights/xoff) to be
 * re-emitted, and the backend scans the words the draw emitted for those uniform writes. At
 * replay, the matrix ops are re-run with the pool matrices read from B's pool (a slot B's tick
 * did not rewrite keeps A's value: a machine that left view stays where A drew it), giving each
 * tag its B matrix; each recorded write is replaced by the B value.
 *
 * Receipts: `[replay] init ...` once; every 256 host frames `[replay] frame= n= slots= replays=
 * rps= ips= cuts=... reasons ... P_us= patch_us= submit_us= sites/f= tags/f= ...`; with
 * `[debug] replay_verify = 1` also `[replay] verify ...` (pose diff and pixel diff). */
#ifndef GDX3DS_REPLAY_H
#define GDX3DS_REPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game-side reads (port/3ds/game/gdx3ds_replay_game.c, decomp types; read only). */
#define GDX_REPLAY_GAME_RACERS 30
typedef struct GdxReplayGame {
    const unsigned char* poolBase;  /* &D_8024DCE0[0]: the two GfxPools, back to back */
    const unsigned char* aliasBase; /* &D_1000000: the segment-1 alias the list's addresses name */
    unsigned int poolSize;          /* sizeof(GfxPool) */
    int curPool;                    /* gGfxPool's index, -1 = neither */
    int gameMode;
    int modeChange;
    int transition;
    int paused;
    int numPlayers;
    int clearTimer;
    int totalRacers;
    unsigned char lod[GDX_REPLAY_GAME_RACERS];   /* machineLod (0 = not visible this tick) */
    unsigned char flags[GDX_REPLAY_GAME_RACERS]; /* bit0 crashed, bit1 retired, bit2 finished */
    float eye[3]; /* camera 0 */
    float fwd[3];
} GdxReplayGame;
void gdx3ds_replay_game_capture(GdxReplayGame* out);

/* Draw tags. 0 = screen space (never patched), 1 = unknown (a draw no path tagged: the frame is
 * not replayable), 2 = reserved, >= 3 = a matrix-state entry of the frame (plus
 * GDX_REPLAY_TAG_SKYBIT for a sky rectangle). */
enum {
    GDX_REPLAY_TAG_SCREEN = 0,
    GDX_REPLAY_TAG_UNKNOWN = 1,
    GDX_REPLAY_TAG_SKY_SCREEN = 2,
    GDX_REPLAY_TAG_FIRST = 3,
};

/* 1 when [native] replay is latched on for this session (rtMode 0, owned interpreter). Hooks
 * test this first and are free when it is 0. */
extern int gdx3ds_replay_on;

/* 1 while the frame being built RECORDS: the lever is on and the frame can be a replay source
 * (its slot is steady D = 2 with no catch-up, or verify needs it). Hot paths test this, not
 * gdx3ds_replay_on: an unrecorded frame runs the lever-off path (the interpreter hook table is
 * NULL too). */
extern int gdx3ds_replay_rec;
/* c5tick: 1 when the tick this iteration builds is the one a replay will present (its hollow
 * build must keep what the replay reads). */
int gdx3ds_replay_wants_tick(void);

/* Per loaded-vertex slot tag (the owned interpreter's loaded_vertices, 64 + 4 scratch). */
#define GDX_REPLAY_VTX_SLOTS 72
extern uint16_t gdx3ds_replay_vtx_tag[GDX_REPLAY_VTX_SLOTS];
/* Per loaded-vertex slot: where its Vtx came from when it lies in a GfxPool (the game rebuilds it
 * every tick): pool offset | pool << 30 | big-endian << 31; GDX_REPLAY_SRC_NONE otherwise. */
#define GDX_REPLAY_SRC_NONE 0x3FFFFFFFu
extern uint32_t gdx3ds_replay_vtx_src[GDX_REPLAY_VTX_SLOTS];
/* The tag of the open native batch (gdx3ds_native_tri.cpp appends, Gdx_NativeBatchFlush draws). */
extern uint16_t gdx3ds_replay_nb_tag;

/* A screen-space draw inside the sky category carries its matrix-state tag with this bit: its
 * projection gets the rotation homography (stars, background sprites drawn as rectangles). */
#define GDX_REPLAY_TAG_SKYBIT 0x8000u

/* The owned interpreter reaches the module through this table (gdx3ds_replay_tags.c, plain C,
 * so the interpreter's host test links without the 3DS-only module). NULL = lever off: every
 * interpreter hook is one pointer test. */
typedef struct GdxReplayInterpHooks {
    void (*taskBegin)(const float* p16, const float* mvStack16x11, int mvSize, const float* mp16);
    void (*opMtx)(unsigned params, const void* src, const int32_t* words);
    void (*opPop)(unsigned count);
    uint16_t (*curTag)(float xscale);
    void (*setDrawTag)(uint16_t tag);
    int (*inSky)(void);
    void (*vtxSrc)(const void* src, unsigned n, unsigned dest, int isBig); /* a G_VTX's source (fills gdx3ds_replay_vtx_src) */
    void (*outVtx)(float* out, int slot); /* a packed vertex written to the VBO from loaded slot `slot` (-1: clipper output) */
} GdxReplayInterpHooks;
extern const GdxReplayInterpHooks* gdx3ds_replay_interp;

/* ---- lifecycle (main thread) ---- */
void gdx3ds_replay_init(int rtMode);
/* Host loop, after the frameskip decision: `skip` = the iteration drops its GFX task. */
void gdx3ds_replay_iteration_begin(unsigned long frame, int skip);
/* Host loop, after the iteration's dispatch. */
void gdx3ds_replay_iteration_end(unsigned long frame, int skip);
/* APT suspend / sleep (main_3ds.cpp aptLifecycleHook). */
void gdx3ds_replay_note_apt(void);

/* ---- frameskip glue (gdx3ds_frameskip.c) ---- */
/* A paced steady render iteration just started its slot (after the start-vblank wait). */
void gdx3ds_replay_slot_begin(int d, int beginMissed, int catchup);
/* Right before a paced present: a replayed frame's swap must have latched first. */
void gdx3ds_replay_before_present(void);
/* [debug] replay_verify: 1 = turn this SKIP decision into a verified render. */
int gdx3ds_replay_verify_take(void);

/* ---- owned interpreter (through gdx3ds_replay_interp) and the glue ---- */
/* taskBegin: a task's walk is about to start (after the frame opened; RunFrame after SpReset):
 * the verified replay runs here, the first task of a frame resets the tables, and the matrix
 * state the task starts from is logged. opMtx: a G_MTX after it was applied (`params` = the
 * interpreter's parameter bits, `src` the resolved host pointer, `words` the 16 host-order words
 * it converted). curTag: the tag of the current matrix state for vertices packed with x scale
 * `xscale`. */
uint16_t gdx3ds_replay_cur_tag(float xscale);
/* A c5cat scene marker (p=0x61): screen-space draws in the SKY category get the sky bit. */
void gdx3ds_replay_scene_marker(unsigned category);
/* 1 when the lever wants the scene markers written (n64_sched.c gdx_c5cat_armed). */
int gdx3ds_replay_markers_armed(void);

/* ---- backend (gfx_citro3d.cpp) ---- */
void gdx3ds_replay_backend_bind(int projLoc, int xfLoc);
void gdx3ds_replay_set_draw_tag(uint16_t tag);
uint16_t gdx3ds_replay_take_draw_tag(void);
/* The machine mesh path's x scale for its CPU-packed runs (gdx3ds_mesh.cpp, per mesh). */
void gdx3ds_replay_set_mesh_xscale(float xscale);
float gdx3ds_replay_mesh_xscale(void);
/* [debug] diagnostics: the CPU mesh's latched MP (row-major 4x4), checked against the draw's tag. */
void gdx3ds_replay_note_mesh_mp(const float* mp);
void gdx3ds_replay_check_mesh_mp(void);
void gdx3ds_replay_note_vtx0(const float* xyzw);
void gdx3ds_replay_note_range(const float* p, unsigned nFloats);
/* native_tri: a packed vertex written from loaded slot `slot`. */
void gdx3ds_replay_out_vtx(float* out, int slot);
/* StartFrame, first begin of a frame (after C3D_FrameBegin): re-dirty citro3d's state so the
 * frame's command list does not depend on the previous frame's (a replay runs it after A). */
void gdx3ds_replay_frame_begin(void);
/* Legacy draw: 1 = force the projection uniform to be re-emitted before this draw. */
int gdx3ds_replay_legacy_needs_proj(uint16_t tag);
/* An xf draw is about to issue: 1 = the projection register holds a legacy draw's pose (the
 * replay would patch it), so the draw must re-emit the unposed projection itself. */
int gdx3ds_replay_xf_needs_proj(void);
/* gpuxf draw: 1 = mark the pose vec4s dirty before this draw. */
int gdx3ds_replay_xf_needs_pose(uint16_t tag);
/* After the draw's IssueDraw: scan [w0, w1) for the tracked uniform writes. `xfShadow` = the
 * backend's gpuxf uniform shadow (28 vec4s) for xf draws, NULL for legacy draws. */
void gdx3ds_replay_draw_post(uint16_t tag, int isXf, uint32_t* w0, uint32_t* w1, const float* xfShadow);
/* A backend draw arrived with no tag (GDX_REPLAY_TAG_UNKNOWN): `inner` = DrawTriangles' caller,
 * `outer` = the caller of the packed/mesh/xf entry that led there (NULL: a direct DrawTriangles).
 * Receipt `[replay] untagged ...` (first draws of the session, resolve with addr2line). */
void gdx3ds_replay_note_untagged(void* inner, void* outer, unsigned tris);
/* The frame cannot be replayed (stereo, readback, ...). */
void gdx3ds_replay_frame_bad(int reason);
void gdx3ds_replay_frame_end_pre(void);
void gdx3ds_replay_frame_end_post(unsigned drawCalls);

enum {
    GDX_REPLAY_BAD_STEREO = 1,
    GDX_REPLAY_BAD_TEXBACKED = 2,
};

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_REPLAY_H */
