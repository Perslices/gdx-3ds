/* port/3ds/game/gdx3ds_prewarm_game.c -- N60PREWARM `[native] prewarm`: the game side (contract and
 * why: port/3ds/gfx/gdx3ds_prewarm.h; docs/research/native/n60prewarm-progress.md).
 *
 * Called from Racer_NrrMachStub (decomp-3ds-prewarm.patch) and from the oracle's frameskip veto
 * hooks. Reads game state only (the transition, the mode globals, the camera, the racers' machine
 * indices); writes one file-static Mtx and the display list it is handed.
 *
 * Compiled into gdx3ds_game (decomp include paths and types). */
#include "global.h"
#include "fzx_racer.h"
#include "fzx_game.h"
#include "fzx_camera.h"
#include "fzx_math.h"
#include "overlays/ovl_i2/transition.h"
#include ASSET_HEADER(machine_custom_gfx.h)

#include "nrr/gdx3ds_nrr_mach.h"
#include "gdx3ds_prewarm.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));

extern GfxPool* gGfxPool;
extern GfxPool D_1000000;
extern Racer* sLastRacer;
extern Camera gCameras[];
extern Gfx* D_800CDD38[];
extern Gfx* D_800CDDB0[];
extern u8 D_800CE3D0[];
extern f32 D_800CE748;
extern f32 D_800CE74C;
extern f32 D_800CE750;
extern s32 gGameMode;
extern s32 gQueuedGameMode;
extern s16 gGameModeChangeState;
extern Transition sTransition;

#define PREWARM_LODS 5        /* body LODs 1-5; LOD 6 is the shared far speck */
#define PREWARM_SLOTS 30      /* machine indices (the body table has 30 x 6 lists) */
#define PREWARM_DISTANCE 200.0f /* the pose: on the view axis this far from the eye (near = 16) */
/* Display-list room the rest of the frame needs after the BODY section (boosters, effects, HUD,
 * transition): the prewarm stops before it would eat into it (gfxBuffer has 8193 commands). */
#define PREWARM_DL_RESERVE 3072

static s32 sLatched = 0;
static u32 sMask = 0;
static Mtx sPose;
static u32 sFrames = 0;   /* prewarm frames written */
static u32 sVetoes = 0;   /* frameskip veto answers given (oracle hooks) */
static u32 sCapped = 0;   /* draws left out for want of display-list room */
/* What the plan saw missing, for the receipt (written by plan, emitted after the bodies / hl). */
static s32 sPlanBodies;
static s32 sPlanSpecks;
static s32 sPlanHl;
static s32 sPlanSlots;
static s32 sNopredDone;
static s32 sBudget;      /* [native] prewarm_draws: prewarm draws per opaque frame (0 = no cap) */
static s32 sLeft;        /* this frame's draws left */

static void Latch(void) {
    if (sLatched) {
        return;
    }
    sLatched = 1;
    sMask = (&gdx3ds_config_get_int != NULL) ? (u32) gdx3ds_config_get_int("native", "prewarm", 0) : 0u;
    sMask &= GDX_PREWARM_MACH | GDX_PREWARM_HL | GDX_PREWARM_RENDER;
    sBudget = (&gdx3ds_config_get_int != NULL) ? gdx3ds_config_get_int("native", "prewarm_draws", 256) : 256;
    if (sBudget < 0) {
        sBudget = 0;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[prewarm] init mask=%u (1=machines' bodies+specks, 2=highlight passes, 4=render the opaque "
                     "fade frame) draws=%d (0 = no cap; the GPU command list of the frame is the limit) window=race appear fade "
                     "at alpha 255, view 0\n",
                     (unsigned) sMask, (int) sBudget);
    }
}

static int IsRaceMode(s32 mode) {
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

/* The list being built (or, before the next tick's logic, the one just built) ends with the race's
 * appear fade at alpha 255: a black fill over the whole fade rectangle, opaque. */
static int FadeOpaque(void) {
    const Transition* t = &sTransition;
    if (!IsRaceMode(GET_MODE(gGameMode)) || gQueuedGameMode != gGameMode) {
        return 0;
    }
    if (gGameModeChangeState != GAMEMODE_UPDATE) {
        return 0;
    }
    if (t->activeTransitionType != TRANSITION_TYPE_FADE || !(t->flags & TRANSITION_FLAG_DRAW)) {
        return 0;
    }
    return t->state == FADE_IN_START && FADE_ALPHA(&t->transitionInfo) == 255;
}

int gdx3ds_prewarm_wants_render(void) {
    Latch();
    if (!(sMask & GDX_PREWARM_RENDER) || !FadeOpaque()) {
        return 0;
    }
    sVetoes++;
    return 1;
}

/* The machine indices in the race, each once (bit i = index i). */
static u32 RaceSlots(void) {
    Racer* racer;
    u32 slots = 0;
    for (racer = gRacers; racer <= sLastRacer; racer++) {
        if ((racer->machineIndex >= 0) && (racer->machineIndex < PREWARM_SLOTS)) {
            slots |= 1u << racer->machineIndex;
        }
    }
    return slots;
}

static s32 BodyIdx(s32 slot, s32 lod) {
    return slot * 6 + lod - 1;
}

int gdx3ds_prewarm_mach_plan(int view, int lod6Skip) {
    u32 slots;
    s32 slot;
    s32 lod;
    int bits = 0;

    Latch();
    if (!(sMask & (GDX_PREWARM_MACH | GDX_PREWARM_HL)) || (view != 0) || !FadeOpaque()) {
        return 0;
    }
    slots = RaceSlots();
    sLeft = (sBudget > 0) ? sBudget : 0x7FFFFFFF;
    sPlanBodies = 0;
    sPlanSpecks = 0;
    sPlanHl = 0;
    sPlanSlots = 0;
    for (slot = 0; slot < PREWARM_SLOTS; slot++) {
        if (!(slots & (1u << slot))) {
            continue;
        }
        sPlanSlots++;
        for (lod = 1; lod <= PREWARM_LODS; lod++) {
            if (!gdx3ds_nrr_mach_ready(BodyIdx(slot, lod), GDX_NRR_MACH_BODY)) {
                sPlanBodies++;
            }
            if ((lod6Skip < 2) && !gdx3ds_nrr_mach_ready(BodyIdx(slot, lod), GDX_NRR_MACH_SPECK)) {
                sPlanSpecks++;
            }
            if (!gdx3ds_nrr_mach_ready(BodyIdx(slot, lod), GDX_NRR_MACH_HL)) {
                sPlanHl++;
            }
        }
    }
    if ((lod6Skip < 2) && !gdx3ds_nrr_mach_ready(GDX_NRR_MACH_SPECK_IDX, GDX_NRR_MACH_SPECK)) {
        sPlanSpecks++;
    }
    if ((sMask & GDX_PREWARM_MACH) && (sPlanBodies + sPlanSpecks) != 0) {
        bits |= GDX_PREWARM_MACH;
    }
    if ((sMask & GDX_PREWARM_HL) && sPlanHl != 0) {
        bits |= GDX_PREWARM_HL;
    }
    return bits;
}

/* One pose for every prewarm draw: the camera's own basis, on the view axis PREWARM_DISTANCE from
 * the eye -- inside the frustum (the mesh path draws it: an off-screen mesh is skipped whole and
 * would record nothing), at the centre of the fade rectangle. */
static void BuildPose(int view) {
    Camera* camera = &gCameras[view];
    Vec3f dir;
    Vec3f pos;
    f32 len;

    dir.x = camera->at.x - camera->eye.x;
    dir.y = camera->at.y - camera->eye.y;
    dir.z = camera->at.z - camera->eye.z;
    len = sqrtf((dir.x * dir.x) + (dir.y * dir.y) + (dir.z * dir.z));
    if (len < 0.001f) {
        dir.x = camera->basis.z.x;
        dir.y = camera->basis.z.y;
        dir.z = camera->basis.z.z;
        len = 1.0f;
    }
    pos.x = camera->eye.x + ((dir.x / len) * PREWARM_DISTANCE);
    pos.y = camera->eye.y + ((dir.y / len) * PREWARM_DISTANCE);
    pos.z = camera->eye.z + ((dir.z / len) * PREWARM_DISTANCE);
    Matrix_ScaleFrom3DMatrix(&sPose, NULL, D_800CE748, D_800CE74C, D_800CE750, &camera->basis, &pos);
}

/* Display-list room for `cmds` more commands and `draws` more prewarm draws in this frame's budget. */
static int Room(const Gfx* gfx, s32 cmds, s32 draws) {
    const Gfx* end = &gGfxPool->gfxBuffer[ARRAY_COUNT(gGfxPool->gfxBuffer)];
    return ((gfx + cmds + PREWARM_DL_RESERVE) <= end) && (sLeft >= draws);
}

/* The stub's own stock body draw (racer.c, the legacy body loop), under the prewarm pose. */
static Gfx* Body(Gfx* gfx, s32 slot, s32 lod, s32 lod6Skip) {
    gSPMatrix(gfx++, &sPose, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    if (!((lod == 6) && (lod6Skip >= 1))) {
        gSPDisplayList(gfx++, D_800CDD38[slot]);
    }
    gDPSetEnvColor(gfx++, 255, 255, 255, 255);
    gSPDisplayList(gfx++, D_800CDDB0[BodyIdx(slot, lod)]);
    return gfx;
}

void* gdx3ds_prewarm_mach_bodies(void* gfxv, int view, int lod6Skip, int nopredOk) {
    Gfx* gfx = gfxv;
    u32 slots = RaceSlots();
    s32 slot;
    s32 lod;
    s32 bodies = 0;
    s32 specks = 0;
    s32 firstSlot = -1;
    const Gfx* start = gfx;

    BuildPose(view);
    sNopredDone = 0;
    for (slot = 0; slot < PREWARM_SLOTS; slot++) {
        if (slots & (1u << slot)) {
            firstSlot = slot;
            break;
        }
    }
    /* The speck with no predecessor: the first machine draw after the section's setup. */
    if ((firstSlot >= 0) && (lod6Skip < 2) && nopredOk &&
        !gdx3ds_nrr_mach_ready(GDX_NRR_MACH_SPECK_IDX, GDX_NRR_MACH_SPECK) && Room(gfx, 8, 1)) {
        gfx = Body(gfx, firstSlot, 6, lod6Skip);
        sLeft--;
        specks++;
        sNopredDone = 1;
    }
    /* Far LODs first: every racer is seen at LOD 5 before it comes close (the budget, when it
     * binds, leaves the near LODs -- seen by fewer racers -- to their first sight). */
    for (lod = PREWARM_LODS; lod >= 1; lod--) {
        for (slot = 0; slot < PREWARM_SLOTS; slot++) {
            const s32 idx = BodyIdx(slot, lod);
            if (!(slots & (1u << slot))) {
                continue;
            }
            const s32 needBody = !gdx3ds_nrr_mach_ready(idx, GDX_NRR_MACH_BODY);
            const s32 needSpeck = (lod6Skip < 2) && !gdx3ds_nrr_mach_ready(idx, GDX_NRR_MACH_SPECK);
            if (!needBody && !needSpeck) {
                continue;
            }
            if (!Room(gfx, 16, needSpeck ? 2 : 1)) {
                sCapped++;
                continue;
            }
            /* The body draw (recorded, or only noted as the predecessor when ready), then the speck
             * that inherits its last run: the [P][SPECK] material. */
            gfx = Body(gfx, slot, lod, lod6Skip);
            bodies++;
            sLeft--;
            if (needSpeck) {
                gfx = Body(gfx, slot, 6, lod6Skip);
                specks++;
                sLeft--;
            }
        }
    }
    sFrames++;
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[prewarm] mach frame=%u view=%d slots=%d missing bodies=%d specks=%d hl=%d | wrote bodies=%d "
                     "specks=%d nopred=%d cmds=%d dl=%d/%d capped=%u lod6skip=%d\n",
                     (unsigned) sFrames, view, sPlanSlots, sPlanBodies, sPlanSpecks, sPlanHl, bodies, specks,
                     sNopredDone, (int) (gfx - start), (int) (gfx - gGfxPool->gfxBuffer),
                     (int) ARRAY_COUNT(gGfxPool->gfxBuffer), (unsigned) sCapped, lod6Skip);
    }
    return gfx;
}

void* gdx3ds_prewarm_mach_hl(void* gfxv, int view, int fogR, int fogG, int fogB) {
    Gfx* gfx = gfxv;
    u32 slots = RaceSlots();
    s32 slot;
    s32 lod;
    s32 hl = 0;
    const Gfx* start = gfx;

    if (!(sMask & GDX_PREWARM_MACH)) {
        BuildPose(view);
    }
    if (!Room(gfx, 32, 1)) {
        sCapped++;
        return gfx;
    }
    gSPDisplayList(gfx++, D_303A7D8); /* the highlight setup, as the stock pass opens */
    for (lod = PREWARM_LODS; lod >= 1; lod--) {
        for (slot = 0; slot < PREWARM_SLOTS; slot++) {
            if (!(slots & (1u << slot))) {
                continue;
            }
            if (gdx3ds_nrr_mach_ready(BodyIdx(slot, lod), GDX_NRR_MACH_HL)) {
                continue;
            }
            if (!Room(gfx, 16 + 8, 1)) {
                sCapped++;
                continue;
            }
            sLeft--;
            gSPMatrix(gfx++, &sPose, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPPipeSync(gfx++);
            gDPSetBlendColor(gfx++, 255, 255, 255, 255);
            gSPDisplayList(gfx++, D_800CDDB0[BodyIdx(slot, lod)]);
            hl++;
        }
    }
    /* Close as the stock section does: the body pass's setup (the next sections see the body
     * prologue's state, not the highlight pass's). */
    gSPDisplayList(gfx++, D_303A6A8);
    gSPLight(gfx++, &D_1000000.unk_21A88[view].l[0], 1);
    gSPLight(gfx++, &D_1000000.unk_21A88[view].a, 2);
    gSPDmaRead(gfx++, 0x8B0, D_800CE3D0, 256);
    gSPLookAtY(gfx++, &D_1000000.unk_21AE8[view]);
    gDPSetFogColor(gfx++, fogR, fogG, fogB, 255);
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[prewarm] hl frame=%u view=%d wrote hl=%d cmds=%d dl=%d/%d capped=%u vetoes=%u\n",
                     (unsigned) sFrames, view, hl, (int) (gfx - start), (int) (gfx - gGfxPool->gfxBuffer),
                     (int) ARRAY_COUNT(gGfxPool->gfxBuffer), (unsigned) sCapped, (unsigned) sVetoes);
    }
    return gfx;
}
