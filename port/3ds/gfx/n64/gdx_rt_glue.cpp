/* port/3ds/gfx/n64/gdx_rt_glue.cpp -- render-thread integration of the owned interpreter.
 *
 * gdx_gfx_job_run (n64_gfx_bridge.cpp) calls gdx_ninterp_job_run for every prepared job; this
 * returns 0 (LUS path) unless `[native] interp = 1`. The latch is read once at first use and
 * the receipt line names the path so a log always shows which interpreter drew.
 *
 * Killswitches (plan §3.15, all under [native]):
 *   interp        0/1   owned path on (default 0 until M4 parity)
 *   interp_verify 0/1   M0: bridge-vs-owned opcode histogram per task, [nverify] on mismatch
 *                       M1+: both paths on one job, per-batch hash compare
 *   cpuclip       0/1   Sutherland-Hodgman fallback (M2)
 *
 * This is the only TU in n64/ that includes LUS/citro3d headers. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "gdx_dlsrc_bridge.h"
#include "gdx_interp.h"
#include "gdx_perf.h"
#include "gdx_log.h"
#include "gdx_seam_owned.h"
#include "gdx3ds_gpu_prof.h"
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_verify.h"
#include "gfx_citro3d.h"

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
extern "C" int gdx3ds_native_lever(const char* key, int fallback);
extern "C" int gdx3ds_config_loaded(void);
extern "C" void gdx3ds_filelog_write(const char* s, size_t n);
extern "C" int gdx3ds_trectbatch_enabled(void) __attribute__((weak));
extern "C" void gdx3ds_atlas_arm(int on) __attribute__((weak));
extern "C" int gdx3ds_tex_page(uint32_t texId) __attribute__((weak));
extern "C" void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);
extern "C" void gdx3ds_texcache_note_import(void);
extern "C" void gdx3ds_texcache_note_miss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord,
                                          unsigned lineBytes, unsigned sizeBytes, unsigned tileW, unsigned tileH,
                                          unsigned hash);

namespace {

void RtLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

/* Adapter: gdxn::Backend over the process-lifetime citro3d renderer (unchanged backend). */
class BackendC3D final : public gdxn::Backend {
  public:
    explicit BackendC3D(Fast::GfxRenderingAPIC3D* r) : mR(r) {
    }
    gdxn::ClipParams GetClipParameters() override {
        const Fast::GfxClipParameters p = mR->GetClipParameters();
        return gdxn::ClipParams{ p.z_is_from_0_to_1, p.invertY };
    }
    int GetMaxTextureSize() override {
        return mR->GetMaxTextureSize();
    }
    void UnloadShader(void* prg) override {
        mR->UnloadShader(static_cast<Fast::ShaderProgram*>(prg));
    }
    void* LookupShader(uint64_t id0, uint64_t id1) override {
        return mR->LookupShader(id0, id1);
    }
    void* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) override {
        return mR->CreateAndLoadNewShader(id0, id1);
    }
    void LoadShader(void* prg) override {
        mR->LoadShader(static_cast<Fast::ShaderProgram*>(prg));
    }
    void ShaderGetInfo(void* prg, uint8_t* numInputs, bool used[2]) override {
        mR->ShaderGetInfo(static_cast<Fast::ShaderProgram*>(prg), numInputs, used);
    }
    uint32_t NewTexture() override {
        return mR->NewTexture();
    }
    void SelectTexture(int tile, uint32_t id) override {
        mR->SelectTexture(tile, id);
    }
    void UploadTexture(const uint8_t* rgba32, uint32_t w, uint32_t h) override {
        mR->UploadTexture(rgba32, w, h);
    }
    void SetSamplerParameters(int sampler, bool linear, uint32_t cms, uint32_t cmt) override {
        mR->SetSamplerParameters(sampler, linear, cms, cmt);
    }
    void DeleteTexture(uint32_t id) override {
        mR->DeleteTexture(id);
    }
    int GetTextureFilter() override {
        return static_cast<int>(mR->GetTextureFilter());
    }
    void SetDepthTestAndMask(bool t, bool m) override {
        mR->SetDepthTestAndMask(t, m);
    }
    void SetZmodeDecal(bool d) override {
        mR->SetZmodeDecal(d);
    }
    void SetViewport(int x, int y, int w, int h) override {
        mR->SetViewport(x, y, w, h);
    }
    void SetScissor(int x, int y, int w, int h) override {
        mR->SetScissor(x, y, w, h);
    }
    void SetUseAlpha(bool a) override {
        mR->SetUseAlpha(a);
    }
    void SetCurrentPrimDepth(float d) override {
        mR->SetCurrentPrimDepth(d);
    }
    void SetCurrentAlphaCompareThreshold(float t) override {
        mR->SetCurrentAlphaCompareThreshold(t);
    }
    void DrawTriangles(float buf[], size_t len, size_t tris) override {
        mR->DrawTriangles(buf, len, tris);
    }
    float* VboPackBegin(Gdx3dsVboPackParams* p) override {
        return mR->VboPackBegin(p);
    }
    void DrawPackedBatch(size_t tris, const Gdx3dsVboPackAux* aux) override {
        mR->DrawPackedBatch(tris, aux);
    }
    void StartFrame() override {
        mR->StartFrame();
    }
    void UpdateFramebufferParameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool invY, bool rt, bool depth,
                                     bool extract) override {
        mR->UpdateFramebufferParameters(fb, w, h, msaa, invY, rt, depth, extract);
    }
    void StartDrawToFramebuffer(int fb, float noise) override {
        mR->StartDrawToFramebuffer(fb, noise);
    }
    void ClearFramebuffer(bool c, bool d) override {
        mR->ClearFramebuffer(c, d);
    }
    void ClearDepthRegion(int x, int y, int w, int h) override {
        mR->ClearDepthRegion(x, y, w, h);
    }
    void FogNoteParams(int mul, int off, int exact) override {
        gdx3ds_fog_note_params(mul, off, exact);
    }
    bool ProfActive() override {
        return gdx3ds_prof_active != 0;
    }
    uint64_t ProfEnter(uint64_t* snap) override {
        return gdx3ds_prof_enter(snap);
    }
    void ProfExit(int sec, uint64_t t0, uint64_t snap) override {
        gdx3ds_prof_exit(sec, t0, snap);
    }
    void VerifyNoteTri(const void* tri) override {
        if (gdx3ds_verify_active()) gdx3ds_verify_note_tri(static_cast<const Gdx3dsVerifyTri*>(tri));
    }
    void VerifyNoteCmd(uint32_t w0, uint32_t w1) override {
        if (gdx3ds_verify_active()) gdx3ds_verify_note_cmd(w0, w1);
    }
    void* GetFramebufferTextureId(int fb) override {
        return mR->GetFramebufferTextureId(fb);
    }
    void SelectTextureFb(int fb) override {
        mR->SelectTextureFb(fb);
    }
    void TexCacheNoteImport() override {
        gdx3ds_texcache_note_import();
    }
    void TexCacheNoteMiss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord, unsigned lineBytes,
                          unsigned sizeBytes, unsigned tileW, unsigned tileH, unsigned hash) override {
        gdx3ds_texcache_note_miss(addr, fmt, siz, tmemWord, lineBytes, sizeBytes, tileW, tileH, hash);
    }
    bool TrectBatchEnabled() override {
        return &gdx3ds_trectbatch_enabled != nullptr && gdx3ds_trectbatch_enabled() != 0;
    }
    void AtlasArm(bool on) override {
        if (&gdx3ds_atlas_arm != nullptr) gdx3ds_atlas_arm(on ? 1 : 0);
    }
    int TexPage(uint32_t id) override {
        return (&gdx3ds_tex_page != nullptr) ? gdx3ds_tex_page(id) : -1;
    }
    void VboPackRefresh(Gdx3dsVboPackParams* p) override {
        if (&gdx3ds_vbopack_refresh != nullptr) gdx3ds_vbopack_refresh(p);
    }

  private:
    Fast::GfxRenderingAPIC3D* mR;
};

int sLatched = 0;   /* 0 = not yet read, 1 = read */
int sInterpOn = 0;
int sVerifyOn = 0;
int sCpuClip = 0;
int sBlendFlush = 1; /* [native] blendflush: Flush() before a blend-alpha change (LUS parity) */
BackendC3D* sBackend = nullptr;
gdxn::Interp* sInterp = nullptr;
GdxInterpSeam* sSeam = nullptr; /* the native replay seam over sInterp (gdx_seam_owned.cpp) */

/* == gfx_noop_handler_f3dex2's marker arms (vendored interpreter.cpp): the track marker
 * (p=0x5A end / 0x5B verify start) and the fx marker (0x5C / 0x5D), handed the command's two
 * words as-is. Under interp=1 the replays run through the owned seam, so the recorded runs
 * draw natively exactly where the stock commands stood -- the course under [native] track and
 * the effect sprites under [native] fx_sink no longer vanish with the owned walker. */
void OwnedNoopHook(void* user, uint32_t w0, uint32_t w1, gdxn::Interp* interp) {
    (void)interp;
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    const uint32_t p = (w0 >> 16) & 0xFFu;
    if (p == 0x5A || p == 0x5B) {
        gdx3ds_track_marker_seam(*seam, w0, static_cast<uintptr_t>(w1));
    } else if (p == 0x5C || p == 0x5D) {
        gdx3ds_fx_marker_seam(*seam, w0, static_cast<uintptr_t>(w1));
    }
}

/* == gfx_dl_handler_common's gdx3ds_mesh_dl_hook call: a registered machine body list
 * reaching G_DL is drawn natively and the walker skips it. No bridge conversion runs under
 * interp=1, so the hook meets the game's own list and the mesh bakes lazily here, with the
 * walker's G_VTX resolution standing in for the bridge's. */
int OwnedDlHook(void* user, const void* listPtr, gdxn::Interp* interp) {
    (void)interp;
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    GdxOwnedSeamSetListHint(seam, listPtr);
    return gdx3ds_mesh_dl_hook_seam(*seam, listPtr, GdxOwnedSeamResolveVtx, seam);
}
uint32_t sTasksSinceReceipt = 0;
uint32_t sVerifyMismatches = 0;  /* tasks with a per-batch mismatch */
uint32_t sVerifyChecked = 0;
uint32_t sHistMismatches = 0;    /* M0 opcode-histogram receipt only (the bridge skips wide-cache hits) */

void Latch() {
    if (sLatched) {
        return;
    }
    if (!gdx3ds_config_loaded()) {
        return; /* defer: keep compiled-in defaults until the ini has been read */
    }
    sLatched = 1;
    sInterpOn = gdx3ds_native_lever("interp", 0) != 0; /* always explicit: [native] all is Tier-2 only (gdx3ds_native_cfg.h) */
    sVerifyOn = gdx3ds_config_get_int("native", "interp_verify", 0) != 0;
    sCpuClip = gdx3ds_config_get_int("native", "cpuclip", 1) != 0; /* LUS parity default; 0 = PICA-only clip (M2 A/B) */
    sBlendFlush = gdx3ds_config_get_int("native", "blendflush", 1) != 0;
    char buf[160];
    snprintf(buf, sizeof(buf), "[native] interp=%d path=%s cpuclip=%d blendflush=%d verify=%d hooks=%s", sInterpOn,
             sInterpOn ? "owned" : "lus", sCpuClip, sBlendFlush, sVerifyOn,
             sInterpOn ? "noop(track,fx)+dl(machines)" : "lus");
    RtLog(buf);
}

/* [dynlod] receipt (port/3ds/gdx3ds_dynlod.c). The LUS path prints it from the bridge's
 * [race-dl] census inside GdxGfxJobRun, which the owned walker never enters, so under interp=1
 * the controller's tier/raises/lowers were invisible ([wobble] audit). Weak like the bridge's. */
extern "C" int gdx3ds_dynlod_receipt(char* buf, int cap) __attribute__((weak));

void EmitReceipt() {
    const gdxn::Counters& s = sInterp->Stats();
    char buf[400];
    snprintf(buf, sizeof(buf),
             "[ninterp] tasks=%u lists=%u (wide=%u n64=%u be=%u f3d=%u noop=%u) cmds=%u vtx=%u tri=%u rect=%u "
             "tmem=%u batches=%u unknown=%u dropped=%u runaway=%u ovf=%u rootrej=%u | dl=%u vtxop=%u tri2=%u "
             "settimg=%u ldblk=%u mtx=%u | rej=%u cull=%u emit=%u packref=%u full=%u",
             s.tasks, s.lists, s.listsByClass[0], s.listsByClass[1], s.listsByClass[2], s.listsF3d, s.noopLists,
             s.cmds, s.vtx, s.tris, s.rects, s.tmemLoads, s.batches, s.unknownOps, s.droppedOps, s.runaway,
             s.stackOverflow, s.rootRejects, s.op[0xDE], s.op[0x01], s.op[0x06], s.op[0xFD], s.op[0xF3],
             s.op[0xDA], s.trisRejected, s.trisCulled, s.trisEmitted, s.packRefused, s.flush[gdxn::FLUSH_FULL]);
    RtLog(buf);
    if (&gdx3ds_dynlod_receipt != nullptr) {
        char dl[192];
        if (gdx3ds_dynlod_receipt(dl, (int)sizeof(dl)) > 0) {
            RtLog(dl);
        }
    }
    if (sVerifyOn) {
        snprintf(buf, sizeof(buf), "[nverify] tasks=%u batch_mismatch_tasks=%u hist_mismatch_tasks=%u", sVerifyChecked,
                 sVerifyMismatches, sHistMismatches);
        RtLog(buf);
    }
    sInterp->ResetStats();
}

/* M0 verify: the bridge converted this job on the submitter; compare its per-opcode input
 * histogram with the owned walk's. Differences name the first opcode that diverged. Known
 * benign classes: the bridge counts the two TEXRECT halves as their own opcodes (so do we),
 * and a G_BRANCH_Z decision differs until [M2] loads vertices. */
void VerifyHistogram(GdxGfxJob* job, const gdxn::Counters& before) {
    unsigned long lists = 0, cmds = 0, ops[256];
    gdx_gfx_job_bridge_stats(job, &lists, &cmds, ops);
    const gdxn::Counters& after = sInterp->Stats();
    sVerifyChecked++;
    /* The bridge's opCounts only count its fast-passthrough opcodes (TRI1/TRI2/QUAD, TEXTURE/
       POPMTX/GEOMETRYMODE, the 0xE2-0xEF RDP state family, 0xF0-0xFC loads/tiles/colours);
       everything else is resolved on its slow path and never counted, so only those compare. */
    int firstOp = -1;
    for (int i = 0; i < 256; i++) {
        const bool passthrough = (i >= 0x05 && i <= 0x07) || (i >= 0xD7 && i <= 0xD9) || (i >= 0xE2 && i <= 0xFC);
        if (!passthrough) continue;
        const unsigned long mine = after.op[i] - before.op[i];
        if (mine != ops[i]) {
            firstOp = i;
            break;
        }
    }
    if (firstOp >= 0) {
        sHistMismatches++;
        if (sHistMismatches <= 8) {
            char buf[240];
            snprintf(buf, sizeof(buf),
                     "[nverify] task=%u first_mismatch op=%02X bridge=%lu owned=%lu | lists bridge=%lu owned=%u "
                     "cmds bridge=%lu owned=%u",
                     after.tasks, firstOp, ops[firstOp], static_cast<unsigned long>(after.op[firstOp] - before.op[firstOp]),
                     lists, after.lists - before.lists, cmds, after.cmds - before.cmds);
            RtLog(buf);
        }
    }
}

} // namespace

extern "C" void gdxn_logf(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        RtLog(buf);
    }
}

extern "C" int gdx_ninterp_active(void) {
    Latch();
    return sInterpOn;
}

/* The bridge's prepare skips ConvertRoot when the owned path will walk the job itself. */
extern "C" int gdx_ninterp_owns_walk(void) {
    Latch();
    return (sLatched && sInterpOn && !sVerifyOn) ? 1 : 0;
}

extern "C" int gdx_ninterp_cpuclip(void) {
    Latch();
    return sCpuClip;
}

extern "C" int gdx_ninterp_blendflush(void) {
    Latch();
    return sBlendFlush;
}

/* Called from gdx_gfx_job_run on the render thread with the job's segment view installed.
 * Returns 1 when the owned path consumed the job. */
extern "C" int gdx_ninterp_job_run(GdxGfxJob* job) {
    Latch();
    if (!sLatched || !sInterpOn || job == nullptr) {
        return 0;
    }
    if (sInterp == nullptr) {
        Fast::GfxRenderingAPIC3D* r = static_cast<Fast::GfxRenderingAPIC3D*>(Gdx3ds_GetCitro3dRenderer());
        sBackend = new BackendC3D(r);
        sInterp = new gdxn::Interp(sBackend);
        sSeam = GdxOwnedSeamCreate(sInterp);
        sInterp->RegisterNoopHook(OwnedNoopHook, sSeam);
        sInterp->RegisterDlHook(OwnedDlHook, sSeam);
    }
    GdxFrameConfig cfg = {};
    if (!gdx_gfx_frame_config(&cfg)) {
        return 0; /* no window yet: let the LUS path take it (it early-outs the same way) */
    }
    /* Task variant: the job's entry ucode (F3dex2Variant numbering). */
    const int variant = gdx_gfx_job_ucode(job);

    const gdxn::Counters before = sInterp->Stats();
    if (sVerifyOn) {
        /* Both paths on one job: LUS records (and draws, epilogue included), the owned path
           replays into the compare ring with the GPU suppressed. */
        static uint32_t sPrevLusEnd[64], sPrevOwnEnd[64];
        static unsigned sPrevN = 0;
        static uint32_t sStartDiffLogs = 0;
        {
            uint32_t lusStart[64], ownStart[64];
            const unsigned n1 = gdx_gfx_lus_state(lusStart, 64);
            const unsigned n2 = sInterp->StateSnapshot(ownStart, 64);
            if (sPrevN != 0 && n1 == sPrevN && n2 == sPrevN && sStartDiffLogs < 40) {
                int lusMoved = -1, ownMoved = -1, diverged = -1;
                for (unsigned i = 0; i < sPrevN; i++) {
                    if (lusMoved < 0 && lusStart[i] != sPrevLusEnd[i]) lusMoved = static_cast<int>(i);
                    if (ownMoved < 0 && ownStart[i] != sPrevOwnEnd[i]) ownMoved = static_cast<int>(i);
                    if (diverged < 0 && lusStart[i] != ownStart[i]) diverged = static_cast<int>(i);
                }
                if (lusMoved >= 0 || ownMoved >= 0 || diverged >= 0) {
                    sStartDiffLogs++;
                    char sb[300];
                    snprintf(sb, sizeof(sb),
                             "[nverify] startstate task=%u lusMoved=%d(%08X->%08X) ownMoved=%d(%08X->%08X) diverged=%d(lus %08X own %08X)",
                             sInterp->Stats().tasks + 1, lusMoved, lusMoved >= 0 ? sPrevLusEnd[lusMoved] : 0,
                             lusMoved >= 0 ? lusStart[lusMoved] : 0, ownMoved, ownMoved >= 0 ? sPrevOwnEnd[ownMoved] : 0,
                             ownMoved >= 0 ? ownStart[ownMoved] : 0, diverged, diverged >= 0 ? lusStart[diverged] : 0,
                             diverged >= 0 ? ownStart[diverged] : 0);
                    RtLog(sb);
                }
            }
        }
        /* One backend, two drivers: each pass re-applies its own cached backend state first so
           its state diff (which assumes the backend still holds what it last sent) stays exact. */
        gdx_gfx_lus_resync_backend();
        gdx3ds_verify_begin(1);
        gdx_gfx_job_run_lus(job);
        uint32_t lusState[64];
        const unsigned lusN = gdx_gfx_lus_state(lusState, 64);
        sInterp->ResyncBackend();
        gdx3ds_verify_begin(2);
        sInterp->RunFrame(job, cfg, variant);
        gdx3ds_verify_begin(0);
        {
            static uint32_t sStateDiffLogs = 0;
            uint32_t ownState[64];
            const unsigned ownN = sInterp->StateSnapshot(ownState, 64);
            memcpy(sPrevLusEnd, lusState, sizeof(sPrevLusEnd));
            memcpy(sPrevOwnEnd, ownState, sizeof(sPrevOwnEnd));
            sPrevN = (lusN == ownN) ? lusN : 0;
            int firstDiff = -1;
            for (unsigned i = 0; i < lusN && i < ownN; i++) {
                if (lusState[i] != ownState[i]) {
                    firstDiff = static_cast<int>(i);
                    break;
                }
            }
            if (firstDiff >= 0 && sStateDiffLogs < 40) {
                sStateDiffLogs++;
                char sb[300];
                snprintf(sb, sizeof(sb), "[nverify] endstate task=%u first_diff_word=%d lus=%08X owned=%08X (w0..3 lus %08X %08X %08X %08X owned %08X %08X %08X %08X)",
                         sInterp->Stats().tasks, firstDiff, lusState[firstDiff], ownState[firstDiff], lusState[0], lusState[1],
                         lusState[2], lusState[3], ownState[0], ownState[1], ownState[2], ownState[3]);
                RtLog(sb);
            }
        }
        unsigned rec = 0, cmp = 0, first = 0, cls = 0, detail = 0;
        gdx3ds_verify_result(&rec, &cmp, &first, &cls, &detail);
        sVerifyChecked++;
        if (first != 0xFFFFFFFFu) {
            sVerifyMismatches++;
            if (sVerifyMismatches <= 40) {
                char buf[520];
                snprintf(buf, sizeof(buf),
                         "[nverify] task=%u batches lus=%u owned=%u first_mismatch=%u class=%s%s%s%s%s field=%u",
                         sInterp->Stats().tasks, rec, cmp, first, (cls & 1) ? "kind " : "", (cls & 2) ? "state " : "",
                         (cls & 4) ? "vbo " : "", (cls & 8) ? "extra " : "", (cls & 16) ? "missing " : "", detail);
                RtLog(buf);
                gdx3ds_verify_first_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_tri_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_src_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_seq_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_cmd_detail(buf, sizeof(buf));
                RtLog(buf);
                {
                    unsigned long bl = 0, bc = 0, ops[256];
                    gdx_gfx_job_bridge_stats(job, &bl, &bc, ops);
                    const gdxn::Counters& st = sInterp->Stats();
                    snprintf(buf, sizeof(buf),
                             "[nverify]   walk bridge lists=%lu cmds=%lu | owned lists=%u cmds=%u bz=%u noop=%u drop=%u var=%d",
                             bl, bc, st.lists - before.lists, st.cmds - before.cmds, st.branchZTaken - before.branchZTaken,
                             st.noopLists - before.noopLists, st.droppedOps - before.droppedOps, sInterp->Variant());
                    RtLog(buf);
                }
            }
        }
        VerifyHistogram(job, before);
        if (++sTasksSinceReceipt >= 64) {
            sTasksSinceReceipt = 0;
            EmitReceipt();
        }
        return 1;
    }
    uint64_t profSnap = 0, profT0 = 0;
    const bool profOn = gdx3ds_prof_active != 0;
    if (profOn) {
        profT0 = gdx3ds_prof_enter(&profSnap);
    }
    sInterp->RunFrame(job, cfg, variant);
    if (profOn) {
        gdx3ds_prof_exit(GDX3DS_PROF_RUN, profT0, profSnap);
    }
    gdx_gfx_job_epilogue(job);
    gdx_perf_sub_end(GDX_PERF_SUB_GFXRUN); /* GdxGfxJobPrepare opened it; GdxGfxJobRun's guard would close it */

    if (++sTasksSinceReceipt >= 64) {
        sTasksSinceReceipt = 0;
        EmitReceipt();
    }
    return 1;
}
