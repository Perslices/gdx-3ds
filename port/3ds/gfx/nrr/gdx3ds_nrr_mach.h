/* gdx3ds_nrr_mach.h -- NRR category MACH (n60 milestone 2): the machines on the compact native race
 * renderer (docs/research/native/n60-design.md §4.3; inventory, decisions, gates and numbers:
 * docs/research/native/n60mach-progress.md). Shared by the game side (racer.c Racer_NrrMachStub,
 * decomp-3ds-nrr-mach.patch) and the render side (gdx3ds_nrr_mach.cpp).
 *
 * Levers `[native] nrr = 1` + `nrr_mach = 1` (core, gdx3ds_nrr.h; default 0, not under all=1).
 * The category init consumes the BODY section of Racer_Draw on rendered ticks with the decomp's
 * own stub (gdx3ds_c5tick_consume, not gdx3ds_nrr_register: the stub also writes stock commands
 * after the marker). The stub makes every game-state write the section makes -- each visible
 * machine's model matrix and highlight matrix, the view's lights in the GfxPool -- fills a
 * GdxNrrMachSnap and hands it to gdx3ds_nrr_mach_publish, which copies it into the core's frame
 * arena and writes the 0x63 marker. Racers the snapshot does not model (no complete record yet:
 * first sight or a rebake; crashed; the LOD-6 speck; time-attack ghosts) follow the marker as the
 * section's stock commands. The marker arms the recorder for them: the first legacy draw of a
 * (slot, LOD, pass) IS its cold record pass, and from the next rendered tick on that racer is in
 * the snapshot. */
#ifndef GDX3DS_NRR_MACH_H
#define GDX3DS_NRR_MACH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Passes a recorded mesh is drawn in (records exist for BODY and HL). */
#define GDX_NRR_MACH_BODY 0
#define GDX_NRR_MACH_HL 1
#define GDX_NRR_MACH_KINDS 2
/* Snapshot-only kind: the LOD-6 far speck (one list, D_7045098, shared by every slot). Its geometry
 * is the speck entry's record (list index 5: slot 0 LOD 6, the [native] machines_speck entry); it
 * has no state commands, so its material is the stock predecessor's last run (the previous
 * LOD 1-5 body in stock order: entry idx), or the speck record's own when there is none
 * (idx GDX_NRR_MACH_NOPRED). */
#define GDX_NRR_MACH_SPECK 2
#define GDX_NRR_MACH_SPECK_IDX 5
#define GDX_NRR_MACH_NOPRED 0xFF

/* Snapshot capacity per view: 30 bodies + 30 highlight passes. */
#define GDX_NRR_MACH_MAX 64

/* One drawn mesh: the GfxPool Mtx the stock G_MTX would have loaded (s15.16, host words),
 * the body list index (machineIndex * 6 + machineLod - 1), the pass, and its colour
 * (body: env rgb = racer body colour; highlight: blend rgb = attack highlight colour). */
typedef struct GdxNrrMachEnt {
    int32_t mtx[16];
    uint8_t idx;
    uint8_t kind;
    uint8_t rgb[3];
    uint8_t pad[3];
} GdxNrrMachEnt;

/* Per view: the lights the stock section loads (gSPLight x2, gSPLookAtY's FLX alpha light, the
 * fog colour), then the entries in stock draw order (highlight passes first, then bodies). */
typedef struct GdxNrrMachSnap {
    uint16_t n;
    uint8_t rec;      /* stock machine draws follow the marker: arm the recorder */
    uint8_t pad0;     /* 0 (nimach: a menu snapshot has GDX_NRR_MACH_MENU_TAG here) */
    int8_t ldir[3];   /* Lights1.l[0].l.dir */
    uint8_t lcol[3];  /* Lights1.l[0].l.col */
    uint8_t amb[3];   /* Lights1.a.l.col */
    uint8_t fog[3];   /* gDPSetFogColor rgb (alpha 255) */
    int16_t alpha[3]; /* LookAtY unk_08: the F3DFLX alpha light (s16, /256) */
    GdxNrrMachEnt e[GDX_NRR_MACH_MAX];
} GdxNrrMachSnap;

/* ---- nimach (nointerp S5b): machines in the menus ----
 * Machine select / machine settings / GP next machine draw the same body lists under the menus' own
 * setup list (D_303A5F8), lights and one viewport per machine. Their records are kept apart from the
 * race's ([idx][MBODY | MSBODY]: the material a run binds depends on the setup), and
 * the snapshot carries per entry the Vp the stock gSPViewport loads and which light set it uses.
 * `[native] ni_mach` (bits: 1 machine select, 2 machine settings / GP next machine) gates the
 * game side; it needs the race category (nrr_mach) on. The marker is the race's (NRR_CAT_MACH, view
 * 0); the menu snapshot is told apart by `tag` (a race GdxNrrMachSnap has 0 there). */
#define GDX_NRR_MACH_MBODY 3  /* machine select's body pass (D_303A5F8) */
#define GDX_NRR_MACH_MSBODY 4 /* machine settings' body pass (D_303A5F8 after the outline) */
/* The settings' outline pass (D_303A7D8, blend colour) stays the stock mesh draw: it has no texture-set
 * list of its own and samples whatever the walk left in TMEM, which a record cannot own. */
/* GdxNrrMachMenuSnap.ctx: which screen recorded the stock draws that follow the marker */
#define GDX_NRR_MACH_CTX_SELECT 1
#define GDX_NRR_MACH_CTX_SETTINGS 2
#define GDX_NRR_MACH_MENU_TAG 0x4Du
#define GDX_NRR_MACH_MENU_MAX 32
#define GDX_NIMACH_SELECT 0x1
#define GDX_NIMACH_SETTINGS 0x2
/* G_NOOP p=0x69 (free: 0x5A-0x67, 0x6A-0x6E taken, 0x6B nicourse): the next G_DL is a machine model of
 * the settings' reflection pass (n64/gdx_rt_glue.cpp OwnedDlHookBody: a retained mesh entry). */
#define GDX_NIMACH_MODEL_P 0x69u

typedef struct GdxNrrMachMenuEnt {
    int32_t mtx[16];  /* the GfxPool Mtx the stock G_MTX loads (s15.16, host words) */
    int16_t vp[8];    /* the Vp the stock gSPViewport loads (vscale[4], vtrans[4]) */
    uint8_t idx;      /* body list index (character * 6 + lod - 1) */
    uint8_t kind;     /* GDX_NRR_MACH_MBODY / MSBODY */
    uint8_t rgb[3];   /* env rgb (func_8009CCBC's gDPSetEnvColor) */
    uint8_t light;    /* light set (0 / 1) */
    uint8_t pad[2];
} GdxNrrMachMenuEnt;

typedef struct GdxNrrMachMenuSnap {
    uint16_t n;
    uint8_t rec;      /* stock machine draws follow the marker: arm the recorder (menu kinds) */
    uint8_t tag;      /* GDX_NRR_MACH_MENU_TAG */
    int8_t ldir[2][3];
    uint8_t lcol[2][3];
    uint8_t amb[2][3];
    uint8_t ctx;      /* GDX_NRR_MACH_CTX_*: the kinds the recorder files stock draws under */
    uint8_t pad;
    GdxNrrMachMenuEnt e[GDX_NRR_MACH_MENU_MAX];
} GdxNrrMachMenuSnap;

/* [native] ni_mach as latched by the category init (0 when nrr_mach is off or refused). */
int gdx3ds_nimach_level(void);
/* Copy the menu snapshot into the core's frame and write the marker. Returns the new cursor, or
 * NULL when nothing was written (arena full / NRR off): the caller then emits the stock loop. */
void* gdx3ds_nrr_mach_menu_publish(void* gfx, const GdxNrrMachMenuSnap* s, int fallback);
/* Write the p=0x69 model marker at gfx; returns the next word. */
void* gdx3ds_nimach_model_marker(void* gfx);

/* ---- game side (racer.c, the game fiber) ---- */
/* 1 when (list index, pass) has a complete record for the current machine-list generation.
 * (GDX_NRR_MACH_SPECK_IDX, GDX_NRR_MACH_BODY) = the speck geometry; (.., GDX_NRR_MACH_SPECK) = the
 * material of a speck with no predecessor. */
int gdx3ds_nrr_mach_ready(int idx, int kind);
/* Copy the snapshot's used part into the core's frame and write the marker at gfx; returns the
 * new cursor (gfx unchanged when there is nothing to draw or record, or the arena is full --
 * then the snapshot's racers are lost for this frame and the core's `ovf=` counts it). Receipt
 * counts: racers emitted the stock way because no record was ready (fallback), crashed / speck /
 * ghost racers (other). */
void* gdx3ds_nrr_mach_publish(void* gfx, int view, const GdxNrrMachSnap* s, int fallback, int other);

/* ---- render side ---- */
/* gdx3ds_mesh.cpp (XfDraw), drawing thread: the record hooks. recording(entry): 1 inside an armed
 * window for a machine entry -- the mesh path then binds every run and lets the PICA clip
 * (performance mode), so one legacy draw records the whole mesh. rec_begin returns 1 when this
 * draw records (armed, not yet ready, resident); the caller then reports every bound run (with
 * the env alpha and blend-colour mode it drew with, and which inputs are the inherited env) and,
 * at the end, how many non-empty runs the mesh has. */
struct Gdx3dsXfDraw;
struct Gdx3dsVboPackAux;
int gdx3ds_nrr_mach_recording(int entryIdx);
int gdx3ds_nrr_mach_rec_begin(int entryIdx, unsigned nRuns, const void* verts);
/* envMask: bits 0-6 = the aux inputs that read the env register, bit 7 = the run inherits env from
 * the section's gDPSetEnvColor; colEnv: bit 0 the xf constant colour is that env input, bit 1 (nimach)
 * the xf constant alpha is the RDP fog colour's alpha (a blend-colour pass's fog factor). */
void gdx3ds_nrr_mach_rec_run(const struct Gdx3dsXfDraw* xf, const struct Gdx3dsVboPackAux* aux, const uint16_t* idx,
                             uint32_t nIdx, uint8_t envMask, uint8_t colEnv, uint8_t envA, int blendColor);
void gdx3ds_nrr_mach_rec_end(int need);
/* gdx3ds_mesh_frame_begin: disarm, deferred frees, the window receipt. */
void gdx3ds_nrr_mach_frame_begin(void);

/* ---- backend half (gfx_citro3d_nrrmach.inc) ---- */
/* The state a legacy gpuxf run just drew with, read back right after it (cold): what a core state
 * key (gdx3ds_nrr.h NrrStateKey) needs, in the backend's terms. */
typedef struct GdxNrrBackendRun {
    uint64_t id0, id1;       /* the shader's combiner ids (the backend's shader pool key) */
    const void* tex[2];      /* C3D_Tex* as bound (NULL = unit unused) */
    uint8_t texLinear[2];    /* the texture's sampler filter */
    uint8_t texCms[2];       /* N64 G_TX_* clamp/mirror per axis */
    uint8_t texCmt[2];
    uint8_t depthTest, depthMask, decal, useAlpha;
    uint8_t alphaRef;        /* the alpha-compare reference ApplyAlphaTest would use (0-255) */
    uint8_t fogOk;           /* the draw took DisableFog (no fog-unit LUT) */
    int16_t scissor[4];      /* the backend's scissor (window x, y, w, h); all 0 = none */
} GdxNrrBackendRun;
int gdx3ds_nrr_backend_capture(GdxNrrBackendRun* out);

#ifdef __cplusplus
}
class GdxInterpSeam;
namespace gdxn {
class Interp;
}
/* n64/gdx_rt_glue.cpp, once: the walking interpreter (its projection is the view's P at the
 * marker; its render-state cache is reset after the NRR draw). */
void gdx3ds_nrr_mach_bind(GdxInterpSeam* seam, gdxn::Interp* interp);
#endif

#endif /* GDX3DS_NRR_MACH_H */
