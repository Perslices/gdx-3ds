/* gdx3ds_track_shim.h — [native] track: GBI shim included by decomp/src/game/course.c.
 *
 * Included under `#if defined(PORT) && defined(GDX_NATIVE_TRACK)` AFTER the game's gbi.h. It
 * redefines the seven GBI macros the course tessellator uses so the C bodies of the ten
 * chunk-group functions, both chunk walks and both setup paths stay byte-identical while their
 * OUTPUT is redirected:
 *
 *   gSPVertex / gSPDisplayList(template) / gSPModifyVertex  -> recorded into a per-GfxPool
 *       op list (the interpreter replays them at a G_NOOP marker, see gdx3ds_track_sink.c)
 *   gSPTexture / gDPLoadSync / gDPSetTextureImage / gDPLoadBlock -> stay real GBI, preceded
 *       by a marker when ops are pending (the run boundary)
 *
 * Every real packet is written by gdx3ds_track_gbi.c, a TU compiled with the game's own
 * gbi.h that just invokes the ORIGINAL macros — so the pass-through encoding is identical by
 * construction, including the 32-bit PORT's tagged w1 pointer. When `gdxtrk_active` is 0
 * (the default: [native] track=0 and trkcensus=0) every macro below costs one global load
 * and forwards to those helpers; Course_DrawModel (course select, `gfx++` packets) never
 * runs while the track pass is active, so it needs no push/pop_macro.
 *
 * Packet expressions in course.c are `sCourseDisp++` (advance) or, once, `sCourseDisp - 1`
 * (Course_SetupChunkPipeTunnelJoinVtx OVERWRITES the previous slot). The macros detect which
 * by comparing sCourseDisp before/after evaluating the packet expression, and the sink
 * returns: 0 = emit the real command into the slot, 1 = recorded (rewind the cursor if it
 * advanced: nothing was written), 2 = a marker was written into the slot, emit the real
 * command into a fresh slot. */
#ifndef GDX3DS_TRACK_SHIM_H
#define GDX3DS_TRACK_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sink (port/3ds/gfx/gdx3ds_track_sink.c) ---- */
extern int gdxtrk_active;
void gdxtrk_begin(int poolIndex, unsigned frame, int cameraIndex);
void gdxtrk_end(void);
int gdxtrk_pending(void);
int gdxtrk_marker(void* slot);
int gdxtrk_state(void* slot, int kind);
int gdxtrk_vertex(void* slot, const void* vtx, int n, int v0, int advanced);
int gdxtrk_dl(void* slot, const void* dl, int advanced);
int gdxtrk_modify(void* slot, int vtx, int where, uint32_t val, int advanced);
/* [native] track_resident (C5 Stage 3c, decomp-native-track-resident-bake.patch): Course_Init
 * calls this once the chunks exist (the load-time bake); Course_Draw calls gdxtrk_draw_enter at
 * its top (the [trkcat] game-side clock). Both are no-ops unless their levers are on. */
void gdxtrk_course_loaded(void);
void gdxtrk_draw_enter(void);
/* n60 [native] nrr_trk (decomp-native-nrr-trk.patch, gfx/nrr/gdx3ds_nrr_trk.h): Course_Draw's walk
 * by the lean walk; 1 = done (the stock loop is skipped). */
int gdxtrk_nrr_walk(void* groups, void* groupsEnd);

/* ---- real GBI emitters (port/3ds/game/gdx3ds_track_gbi.c) ---- */
void gdxtrk_gbi_vertex(void* g, const void* v, int n, int v0);
void gdxtrk_gbi_dl(void* g, const void* dl);
void gdxtrk_gbi_modify(void* g, int vtx, int where, uint32_t val);
void gdxtrk_gbi_texture(void* g, int s, int t, int level, int tile, int on);
void gdxtrk_gbi_loadsync(void* g);
void gdxtrk_gbi_settimg(void* g, int fmt, int siz, int width, const void* img);
void gdxtrk_gbi_loadblock(void* g, int tile, int uls, int ult, int lrs, int dxt);
void gdxtrk_gbi_noop(void* g);
void gdxtrk_gbi_marker(void* g, uint32_t w0, const void* record);

/* State-command kinds for the census. */
#define GDXTRK_STATE_TEXTURE 0
#define GDXTRK_STATE_LOADSYNC 1
#define GDXTRK_STATE_SETTIMG 2
#define GDXTRK_STATE_LOADBLOCK 3

#ifdef __cplusplus
}
#endif

/* ---- hooks for Course_Draw (the decomp patch adds exactly these two lines) ---- */
#define GDXTRK_BEGIN(cam) gdxtrk_begin((int)(gGfxPool - D_8024DCE0), (unsigned)gGameFrameCount, (int)(cam))
#define GDXTRK_END()                                          \
    {                                                         \
        if (gdxtrk_active && gdxtrk_pending()) {              \
            gdxtrk_marker(sCourseDisp++);                     \
        }                                                     \
        gdxtrk_end();                                         \
    }

/* ---- the macro redefinitions ---- */
#undef gSPVertex
#define gSPVertex(pkt, v, n, v0)                                                        \
    {                                                                                   \
        Gfx* _gdxB = sCourseDisp;                                                       \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        const void* _gdxV = (const void*)(v);                                           \
        int _gdxN = (int)(n);                                                           \
        int _gdxV0 = (int)(v0);                                                         \
        int _gdxAdv = (_gdxB != sCourseDisp);                                           \
        int _gdxR = gdxtrk_active ? gdxtrk_vertex(_gdxG, _gdxV, _gdxN, _gdxV0, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                sCourseDisp = _gdxG;                                                    \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = sCourseDisp++;                                                  \
            }                                                                           \
            gdxtrk_gbi_vertex(_gdxG, _gdxV, _gdxN, _gdxV0);                             \
        }                                                                               \
    }

#undef gSPDisplayList
#define gSPDisplayList(pkt, dl)                                                         \
    {                                                                                   \
        Gfx* _gdxB = sCourseDisp;                                                       \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        const void* _gdxD = (const void*)(dl);                                          \
        int _gdxAdv = (_gdxB != sCourseDisp);                                           \
        int _gdxR = gdxtrk_active ? gdxtrk_dl(_gdxG, _gdxD, _gdxAdv) : 0;               \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                sCourseDisp = _gdxG;                                                    \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = sCourseDisp++;                                                  \
            }                                                                           \
            gdxtrk_gbi_dl(_gdxG, _gdxD);                                                \
        }                                                                               \
    }

#undef gSPModifyVertex
#define gSPModifyVertex(pkt, vtx, where, val)                                           \
    {                                                                                   \
        Gfx* _gdxB = sCourseDisp;                                                       \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        int _gdxVtx = (int)(vtx);                                                       \
        int _gdxWh = (int)(where);                                                      \
        uint32_t _gdxVal = (uint32_t)(val);                                             \
        int _gdxAdv = (_gdxB != sCourseDisp);                                           \
        int _gdxR = gdxtrk_active ? gdxtrk_modify(_gdxG, _gdxVtx, _gdxWh, _gdxVal, _gdxAdv) : 0; \
        if (_gdxR == 1) {                                                               \
            if (_gdxAdv) {                                                              \
                sCourseDisp = _gdxG;                                                    \
            }                                                                           \
        } else {                                                                        \
            if (_gdxR == 2) {                                                           \
                _gdxG = sCourseDisp++;                                                  \
            }                                                                           \
            gdxtrk_gbi_modify(_gdxG, _gdxVtx, _gdxWh, _gdxVal);                         \
        }                                                                               \
    }

#undef gSPTexture
#define gSPTexture(pkt, s, t, level, tile, on)                                          \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxtrk_active && gdxtrk_state(_gdxG, GDXTRK_STATE_TEXTURE)) {               \
            _gdxG = sCourseDisp++;                                                      \
        }                                                                               \
        gdxtrk_gbi_texture(_gdxG, (int)(s), (int)(t), (int)(level), (int)(tile), (int)(on)); \
    }

#undef gDPLoadSync
#define gDPLoadSync(pkt)                                                                \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxtrk_active && gdxtrk_state(_gdxG, GDXTRK_STATE_LOADSYNC)) {              \
            _gdxG = sCourseDisp++;                                                      \
        }                                                                               \
        gdxtrk_gbi_loadsync(_gdxG);                                                     \
    }

#undef gDPSetTextureImage
#define gDPSetTextureImage(pkt, f, s, w, i)                                             \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxtrk_active && gdxtrk_state(_gdxG, GDXTRK_STATE_SETTIMG)) {               \
            _gdxG = sCourseDisp++;                                                      \
        }                                                                               \
        gdxtrk_gbi_settimg(_gdxG, (int)(f), (int)(s), (int)(w), (const void*)(i));      \
    }

#undef gDPLoadBlock
#define gDPLoadBlock(pkt, tile, uls, ult, lrs, dxt)                                     \
    {                                                                                   \
        Gfx* _gdxG = (Gfx*)(pkt);                                                       \
        if (gdxtrk_active && gdxtrk_state(_gdxG, GDXTRK_STATE_LOADBLOCK)) {             \
            _gdxG = sCourseDisp++;                                                      \
        }                                                                               \
        gdxtrk_gbi_loadblock(_gdxG, (int)(tile), (int)(uls), (int)(ult), (int)(lrs), (int)(dxt)); \
    }

#endif /* GDX3DS_TRACK_SHIM_H */
