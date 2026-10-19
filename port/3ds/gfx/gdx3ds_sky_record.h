/* gdx3ds_sky_record.h -- [native] sky_retain / skyclock (c5sky-progress.md): what the game-side
 * sky capture (gdx3ds_sky_capture.c, writer) and the owned interpreter's sky path
 * (n64/gdx_sky_retain.cpp, reader) share.
 *
 * Marker: G_NOOP, w0 = (G_NOOP<<24) | (0x65<<16) | (kind<<12) | index. Track owns 0x5A/0x5B and
 * 0x60, fx 0x5C/0x5D, the HUD capture 0x5E/0x5F, the c5cat scene markers 0x61; 0x65 is the sky's.
 *   kind 0 SECTION  index = GDXSKY_SEC_*, w1 = 0: the census boundary / the replay's section
 *   kind 1 RUN      index = run index, w1 = the GdxSkyRecord: a compacted run replays here
 *
 * The capture does not redefine a GBI macro. Background_Draw runs unchanged and writes its
 * display list as always; afterwards gdxsky_capture() compacts the words it wrote, in place:
 * every maximal run of "recordable" commands (RDP/RSP state, texture loads, rectangles,
 * triangles, G_MTX, G_VTX) moves into this per-GfxPool record behind ONE G_NOOP run marker;
 * everything else (G_DL, G_MOVEMEM/G_MOVEWORD, other markers) stays where it was. The replay
 * executes each run at its marker through the interpreter's own handlers -- same commands, same
 * order, same stream position.
 *
 * Same double-buffer contract as the HUD/fx/track records: the game fills pool N while the render
 * side may replay pool N^1 (renderthread=1); on the Old 3DS both happen in one iteration.
 *
 * One display-list word as the 32-bit PORT lays out a Gfx (16 bytes: w0, pad, the 64-bit w1
 * whose high half is the host-pointer tag). */
#ifndef GDX3DS_SKY_RECORD_H
#define GDX3DS_SKY_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDXSKY_OP_NOOP 0x00u
#define GDXSKY_MARKER_P 0x65u
#define GDXSKY_HOST_TAG 0x47445831u /* "GDX1": decomp gbi.h GDX_GFXW1_HOST_TAG */
#define GDXSKY_MAGIC 0x534B5931u    /* "SKY1" */

/* 1P race ~250 recordable words per frame (sprites ~80, up to 100 stars x 4); 4P ~4 x 170.
 * Overflow aborts the capture of that region (it stays stock) and is counted. */
#define GDXSKY_MAX_WORDS 2048
#define GDXSKY_MAX_RUNS 256

enum {
    GDXSKY_MK_SECTION = 0,
    GDXSKY_MK_RUN = 1,
};

/* Section ids: must match GDX_SKYSEC_* in decomp-native-sky-capture.patch (background.c). */
enum {
    GDXSKY_SEC_NONE = 0,
    GDXSKY_SEC_HEAD = 1,      /* projection, Camera_Draw, the setup list, the 28 skybox vertices */
    GDXSKY_SEC_GRAD = 2,      /* the 64x1 RGBA16 gradient strip and its quad */
    GDXSKY_SEC_STARS = 3,     /* night venues: up to 100 8x8 IA8 texrects */
    GDXSKY_SEC_SPRITES = 4,   /* camera 0: per visible sprite MTX, 4 verts, TLUT, CI4 64x64, quad */
    GDXSKY_SEC_FLOOR = 5,     /* the venue floor 64x32 RGBA16, two quads */
    GDXSKY_SEC_FIREWORKS = 6, /* the ending cutscene */
    GDXSKY_SEC_CLOUDS = 7,    /* 64x32 IA8, two quads */
    GDXSKY_SEC_END = 8,
    GDXSKY_SEC_COUNT = 9
};

/* What Background_Draw's textures are this frame (filled by the patch, hashed by the capture):
 * a change of any of them bumps the record's `generation`, which every retained answer on the
 * render side is keyed on (course change, the night palette set, the ending, records.c). */
typedef struct GdxSkyIdentity {
    const void* skybox;
    const void* floor;
    const void* cloud;
    const void* star;
    const void* spriteTex[6];
    const void* spritePal[6];
    int32_t spriteCount;
    int32_t paletteReplacements;
    int32_t courseIndex;
    int32_t gameMode;
    uint32_t skyboxFlags;
} GdxSkyIdentity;

typedef struct GdxSkyWord {
    uint32_t w0;
    uint32_t pad;
    uint32_t w1;
    uint32_t w1hi;
} GdxSkyWord;

typedef struct GdxSkyRun {
    uint16_t start;
    uint16_t end;
    uint8_t section; /* GDXSKY_SEC_* the run was captured in */
    uint8_t camera;
    uint8_t mode;    /* the lever: 1 retain, 2 verify */
    uint8_t pad;
} GdxSkyRun;

typedef struct GdxSkyRecord {
    uint32_t magic;
    uint32_t frame;      /* gGameFrameCount the record belongs to */
    uint32_t generation; /* sky identity generation (see GdxSkyIdentity) */
    uint32_t nWords;
    uint32_t nRuns;
    uint32_t badRecord;  /* written by the replay: markers whose run failed validation */
    GdxSkyWord words[GDXSKY_MAX_WORDS];
    GdxSkyRun runs[GDXSKY_MAX_RUNS];
} GdxSkyRecord;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_SKY_RECORD_H */
