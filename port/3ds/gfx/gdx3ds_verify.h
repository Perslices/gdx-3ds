/* port/3ds/gfx/gdx3ds_verify.h -- [nverify] per-batch differential for the owned interpreter
 * ([native] interp_verify=1, docs/research/native/06-interp-plan.md §6.2).
 *
 * Mode 1 (record): every draw that reaches the backend's submission point, every clear and
 * every render-target switch appends a record {kind, state hash, vbo hash} to a ring; mode 2
 * (compare): the same events are hashed and compared against the ring in order, and NOTHING
 * reaches the GPU (no frame begin, no clear, no draw) so the screen shows the recorded pass.
 * The state hash covers the shader ids, the bound textures' upload-content hashes + sampler
 * state, depth/decal/alpha state, prim depth, alpha threshold, viewport fold, scissor, the
 * packed aux record and the vertex count; the vbo hash covers the final PICA-layout floats. */
#ifndef GDX3DS_VERIFY_H
#define GDX3DS_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

void gdx3ds_verify_begin(int mode);
/* recorded = mode-1 records, compared = mode-2 events seen, first = index of the first
 * mismatch (0xFFFFFFFF = none), cls = bitmask 1 kind, 2 state, 4 vbo, 8 extra event in the
 * compare pass, 16 compare pass ended early. detail = the state sub-field that differed
 * first (see gfx_citro3d.cpp VfyState field order) for class 2. */
void gdx3ds_verify_result(unsigned* recorded, unsigned* compared, unsigned* first, unsigned* cls,
                          unsigned* detail);

/* Interpreter-side inputs of the LAST triangle appended (both interpreters call this right
 * before their packed write while a verify pass is active); captured into the batch record
 * at submission so a vbo/state mismatch names the diverging input. */
typedef struct Gdx3dsVerifyTri {
    unsigned tile0, tile1;
    float uls0, ult0, lrs0, lrt0;
    unsigned texW0, texH0, texW20, texH20;
    unsigned texW1, texH1;
    unsigned tm;
    float vu, vv;
    int shifts0, shiftt0;
    unsigned line0, size0, full0, orig0;
    unsigned scaleS, scaleT;
    unsigned firstTile;
    unsigned isRect;
    unsigned omh, oml, geo, variant;
    int vpx, vpy;
    unsigned vpw, vph;
    unsigned lists, cmds, branchZ, noop;
    /* source identity: loaded-record host address + bridge class bits, FNV of its first 64
     * bytes, FNV of the 64 TMEM bytes at the render tile's tmem offset, blend alpha. */
    unsigned long addr0;
    unsigned cls0, srcHash0, tmemHash0, blendA;
    unsigned long addr1;
    unsigned cls1, srcHash1, tmemHash1, tmem1, line1, size1, full1, orig1, tc0, tc1;
    /* unit-0 span record: render tile tmem word, span base word, base record addr/size/orig. */
    unsigned tmem0, base0;
    unsigned long baseAddr0;
    unsigned baseSize0, baseOrig0, used0, used1;
    /* First 8 bytes of the unit-0 source and of its TMEM window, verbatim. A hash says the
     * two sides differ; these say HOW -- a byte-swapped pair is a byte-order difference
     * (the bridge's persistent copy is swapped for a native-RGBA16 source), anything else
     * is different content. */
    /* srcB[0..1] = the first 8 source bytes, srcB[2..3] = the last 8; same for tmemB. */
    unsigned srcB0[4], tmemB0[4];
    unsigned rtLine0;   /* the RENDER tile's line_size_bytes: the decode's TMEM row stride */
    /* if this side's unit-0 source is a bridge persistent copy: its ORIGINAL source address and
     * the copy bytes remaining from that point (0/0 when the pointer is not a copy). */
    unsigned long copyOrigin0;
    unsigned copyRemain0;
} Gdx3dsVerifyTri;
void gdx3ds_verify_note_tri(const Gdx3dsVerifyTri* t);
/* Fills addr0/cls0/srcHash0/tmemHash0 from a loaded-record address and a TMEM window. */
void gdx3ds_verify_tri_source(Gdx3dsVerifyTri* t, const void* addr, unsigned sizeBytes, const void* tmem, unsigned tmemBytes);
void gdx3ds_verify_src_detail(char* buf, unsigned bufSize);
void gdx3ds_verify_bytes_detail(char* buf, unsigned bufSize);
void gdx3ds_verify_tri_source1(Gdx3dsVerifyTri* t, const void* addr, unsigned sizeBytes, const void* tmem, unsigned tmemBytes);
/* Command-stream differential: every VALUE-carrying command each interpreter dispatches
 * (TRI1/TRI2/QUAD, TEXTURE/POPMTX/GEOMETRYMODE, E2-EF minus RDPHALF, F0-FC), in order. */
void gdx3ds_verify_note_cmd(unsigned w0, unsigned w1);
void gdx3ds_verify_cmd_detail(char* buf, unsigned bufSize);
int gdx3ds_verify_active(void);

/* [nverify] the mode as a plain global (0 off, 1 record, 2 compare), so the two interpreters'
 * hottest loops -- per GBI command and per triangle -- can test it INLINE instead of calling
 * gdx3ds_verify_active() across a TU boundary with no LTO in the tree (audit 2026-09-23
 * findings 5 and 31: that call, and the diagnostic work behind it, was being paid on every
 * command and every triangle of every [prof] run on both paths, in different amounts).
 * Written only by gdx3ds_verify_begin, on the render thread, between passes. Mirrors the
 * gdx3ds_prof_active contract (gdx3ds_gpu_prof.h). */
extern int gdx3ds_verify_mode;

/* One line describing the first mismatching batch: both sides' vertex 0 (12 floats), tri
 * counts, shader ids. Empty string when nothing mismatched. */
void gdx3ds_verify_first_detail(char* buf, unsigned bufSize);
void gdx3ds_verify_tri_detail(char* buf, unsigned bufSize);
void gdx3ds_verify_seq_detail(char* buf, unsigned bufSize);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_VERIFY_H */
