/* gdx3ds_xfprof.h -- [debug] xfprof (c5mach2): a section profile of the gpuxf machine path, per
 * bound run. Diagnostic only (default 0): every section boundary is one tick read. The mesh code
 * (gdx3ds_mesh.cpp) owns the accumulators and prints `[xfprof]` every 64 frames; the backend
 * (gfx_citro3d.cpp) marks its own sections inside a gpuxf DrawTriangles. */
#ifndef GDX3DS_XFPROF_H
#define GDX3DS_XFPROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XFP_STATE = 0,  /* ApplyRunState */
    XFP_SCREEN,     /* XfFirstSurvivor */
    XFP_BIND,       /* BindRunMaterial (mattab or full resolve) */
    XFP_PARAMS,     /* backend xf params (NativeXfParams) */
    XFP_VERTEX,     /* the first survivor's T&L */
    XFP_PACKCTX,    /* PreparePackCtx */
    XFP_FILLRUN,    /* XfFillRun */
    XFP_AUX,        /* BuildAux + cull + ApplyDrawConstants */
    XFP_B_TEX,      /* backend: entry + texture units */
    XFP_B_CONST,    /* backend: stage constants (RefreshStageConstantsPacked) */
    XFP_B_ATFOG,    /* backend: alpha test + fog */
    XFP_B_XF,       /* backend: vertex buffer, attr, xf uniforms, cull */
    XFP_B_SUBMIT,   /* backend: projection + C3D_DrawElements */
    XFP_B_TAIL,     /* backend: the rest of DrawTriangles + NativeXfDraw */
    XFP_MESH,       /* per mesh: latch, box, classify, bound, retained source, XfFillMesh */
    XFP_COUNT
};

extern int gdx3ds_xfprof_on;
extern int gdx3ds_xfprof_inrun; /* a machine run's backend draw is in progress (track draws are not) */
extern uint64_t gdx3ds_xfprof_ticks[XFP_COUNT];
extern uint64_t gdx3ds_xfprof_last;

/* Charge the time since the previous mark to `section` and start the next one. */
static inline void gdx3ds_xfprof_mark(int section, uint64_t now) {
    gdx3ds_xfprof_ticks[section] += now - gdx3ds_xfprof_last;
    gdx3ds_xfprof_last = now;
}

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_XFPROF_H */
