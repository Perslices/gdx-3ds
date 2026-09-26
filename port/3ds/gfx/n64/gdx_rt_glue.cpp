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
/* The bridge's two memo levers, RESOLVED, for the [native] receipt (n64_gfx_bridge.cpp). */
extern "C" int gdx_bridge_dlfacts_on(void) __attribute__((weak));
extern "C" int gdx_bridge_stubgen_narrow_on(void) __attribute__((weak));
extern "C" int gdx_bridge_memoorder(void) __attribute__((weak));
extern "C" void gdx3ds_atlas_arm(int on) __attribute__((weak));
extern "C" int gdx3ds_tex_page(uint32_t texId) __attribute__((weak));
extern "C" void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);
/* gfx_citro3d.cpp: the stage-constant input mask of the shader currently bound. */
extern "C" int Gdx_NativeBatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask);
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
    float* VboPackBegin(Gdx3dsVboPackParams* p, uint32_t* capTris) override {
        return mR->VboPackBegin(p, capTris);
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
    bool VerifyActive() override {
        return gdx3ds_verify_mode != 0;
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
    bool BatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) override {
        return Gdx_NativeBatchConstInputs(rgbMask, alphaMask) != 0;
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
int sCensus = 0;     /* [native] ncensus: batch-split attribution ([ncensus] receipt) */
int sOwnFold = 0;    /* [native] ownfold: prim/env fold inside the owned batches (B2) */
/* [native] dspcensus: per-opcode SELF-time census of the owned walker (B4). 0 off, 1 opcode
 * self time, 2 + the G_DL sub-phases. A diagnostic: three clock reads per dispatched command,
 * so ms are inflated and only the RANKING is a claim. */
int sDspCensus = 0;
/* [native] palhash: fold the live TLUT contents into the CI texture-cache key. Both caches key
 * CI entries on the palette's DRAM ADDRESS, which cannot see a TLUT rewritten in place at that
 * same address, so an animated palette serves whatever decode the entry was born with. */
/* [native] timgmemo: the owned walker's cross-frame SETTIMG classification memo (B4's F3
 * target). A stale texture classification is a wrong-pixels bug, which is why the invalidation
 * key was derived in writing first (docs/research/native/loadblock-progress.md S1).
 * PROMOTED to default 1 on native/tmemblock (stubmemo-progress.md S6): its own corpus gate is
 * green (12/12 pairs, 30 972 hashes, 0 divergent -- loadblock-progress.md S4) and the three
 * memo levers were then re-gated TOGETHER at these new defaults. `[native] timgmemo = 0`
 * restores the per-load re-derivation. */
int sPalHash = 1;
int sTexHashCi = 1;  /* [native] texhash_ci: content-key the CI/RGBA32 classes (plan §3.7) */
int sTexInval = 1;   /* [native] texinval: replay the queued cache clear/deletes on the owned path */
/* DROPSITES. sTexStaleFix / sVtxClamp are killswitches over the two fixes (default = fixed);
 * sTexScratchKib / sVtxMax are forcing knobs (default 0 = shipped behaviour). */
int sTexStaleFix = 1;
int sVtxClamp = 1;
int sTexScratchKib = 0;
int sTexScratchMaxKib = 0;
int sVtxMax = 0;
int sClipPolyMax = 0;
/* [native] rawcopywhole: the bridge's persistent texture copy must span the WHOLE upcoming load,
 * not just the registered extent of the buffer the load starts in. A registered range is
 * bookkeeping, not a memory bound: the game legitimately issues a load that runs past the end of
 * its own buffer (the last strip of a transition reads one row past backgroundBuffer), and on
 * the console DRAM simply continues there -- which is what the owned walk reads. Clamping the
 * copy instead left the LUS interpreter reading past the end of the COPY, i.e. unrelated host
 * heap. Third instance of one class, after the o2r-by-name truncation of 2026-09-17. */
int sRawCopyWhole = 1;
int sTimgMemo = 1;   /* [native] timgmemo: cross-frame SETTIMG classification memo (B4/F3) */
/* [native] tmemskip: widen G_LOADBLOCK/G_LOADTILE's same-content skip past the o2r resource
 * class, using the bridge's own source-stability seam (gdx_dlsrc_bridge.h). 0 off (the shipping
 * immutable-only gate), 1 the classes that are stable with NO compare (native-RGBA16 by
 * generation, registered ROM-backed host ranges), 2 additionally the DMA-tracked class.
 * Default 0 until the corpus gate is green: a stale TMEM mirror is a wrong-pixels bug AND a
 * stale texture-cache identity, so the invalidation argument was written first
 * (docs/research/native/tmemblock-progress.md S2).
 * [native] tmemcls arms the class census alone and changes no behaviour. */
int sTmemSkip = 0;
int sTmemCls = 0;
/* [memotear] [debug] tmemfault -- FAULT INJECTION for the tmemskip refusal arm (NEXT.md B54).
 * tmemblock gated tmemskip green over 412,326 corpus skips with stale=0, i.e. the corpus never
 * once drove the gate to say STALE; only the host test does. 0 = off (always, by default).
 * N > 0 = force staleness on every Nth load that WOULD have skipped, alternating the two
 * mechanisms the gate is supposed to catch (the source generation moving, and the class
 * changing under the span). Inert unless tmemskip != 0, so the shipping default cannot reach
 * it. The gate result is the pair (a) tmemStale climbs by the injection count and the skip does
 * not happen, (b) the frame is byte-identical to the tmemskip=0 run -- a refusal means the
 * mirror is rewritten, which is exactly what tmemskip=0 does every time. */
int sTmemFault = 0;
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
/* Mismatching tasks split by class, and by the state sub-field that diverged first. The single
 * "first mismatch" detail dump is capped, and a screen that mismatches early (the title) used
 * the whole budget before a later screen was ever reached, so the counters carry the shape of
 * the run and the detail budget is now per receipt window instead of per run. */
uint32_t sVfyClsKind = 0, sVfyClsState = 0, sVfyClsVbo = 0, sVfyClsExtra = 0, sVfyClsMissing = 0;
uint32_t sVfyFieldHist[12] = {};
uint32_t sVerifyDetailWindow = 0;
constexpr uint32_t kVerifyDetailPerWindow = 2;
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
    sCensus = gdx3ds_config_get_int("native", "ncensus", 0) != 0; /* raw: a diagnostic, not a path */
    sDspCensus = gdx3ds_config_get_int("native", "dspcensus", 0);
    /* NOT Tier-2 (it is absent from kTier2Levers in gdx3ds_native_cfg.c), so `all=1` neither
       turns it on nor off -- it now defaults ON in its own right, and `all=1 interp=1` is one
       of the two configurations the corpus gate runs it in. An explicit `timgmemo = 0` is
       still the killswitch; the Tier-2 master has never had any say over this lever. */
    sTimgMemo = gdx3ds_native_lever("timgmemo", 1) != 0;
    /* Not Tier-2 and not a boolean: tmemskip is a LEVEL (see sTmemSkip). */
    sTmemSkip = gdx3ds_config_get_int("native", "tmemskip", 0);
    sTmemCls = gdx3ds_config_get_int("native", "tmemcls", 0);
    sTmemFault = gdx3ds_config_get_int("debug", "tmemfault", 0);
    /* ownfold IS Tier-2 (`all=1` turns it on, exactly as it turns on fx_hud -- they are the
       same fold, one per interpreter); an explicit `ownfold = 0` still wins. */
    sOwnFold = gdx3ds_native_lever("ownfold", 0) != 0;
    /* palhash and texhash_ci were found independently by two agents for the same B9 failure and
       fix the same class from different ends: palhash folds the live TLUT into the CI key,
       texhash_ci content-keys the CI/RGBA32 classes. Both stay: texinval is additionally the
       only thing honouring gdx_rdram_mode_reset and minimap.c on the owned path. */
    sPalHash = gdx3ds_config_get_int("native", "palhash", 1) != 0;
    sTexHashCi = gdx3ds_config_get_int("native", "texhash_ci", 1) != 0;
    sTexInval = gdx3ds_config_get_int("native", "texinval", 1) != 0;
    /* DROPSITES. Two killswitches (default = the fix) and two forcing knobs (default = shipped).
     * texstalefix=0 restores "an import that decoded nothing is still marked uploaded", the arm
     * that leaves a recycled backend id's previous pixels cached under the new key.
     * vtxclamp=0 restores "an over-range G_VTX loads nothing at all".
     * [debug] texscratchkib caps the decode scratch's growth ceiling; [debug] vtxmax shrinks the
     * effective G_VTX window. Both exist to force the drop arms in a normal build. */
    sTexStaleFix = gdx3ds_config_get_int("native", "texstalefix", 1) != 0;
    sVtxClamp = gdx3ds_config_get_int("native", "vtxclamp", 1) != 0;
    sTexScratchKib = gdx3ds_config_get_int("debug", "texscratchkib", 0);
    sTexScratchMaxKib = gdx3ds_config_get_int("debug", "texscratchmaxkib", 0);
    sVtxMax = gdx3ds_config_get_int("debug", "vtxmax", 0);
    sClipPolyMax = gdx3ds_config_get_int("debug", "clippolymax", 0);
    sRawCopyWhole = gdx3ds_config_get_int("native", "rawcopywhole", 1) != 0;
    /* The three memo levers ship default-ON as of native/tmemblock, so the receipt prints the
       RESOLVED value of all three side by side: timgmemo from this file's latch, dlfacts and
       stubgen read back out of the bridge through gdx_bridge_*(). Before the promotion a leg
       was identified by a number that only an explicit ini key could produce; now an absent
       key and an explicit `=1` agree, so a mistyped override would be invisible unless the
       receipt reports what resolved. Both accessors are weak: a build that links the glue
       without the bridge prints -1 rather than failing to link. */
    const int dlFactsResolved = (&gdx_bridge_dlfacts_on != nullptr) ? gdx_bridge_dlfacts_on() : -1;
    const int stubGenResolved =
        (&gdx_bridge_stubgen_narrow_on != nullptr) ? gdx_bridge_stubgen_narrow_on() : -1;
    /* [memotear] [native] memoorder -- the publication-order/barrier level of the six
       process-wide [brfast] memos. Same weak-accessor rule as the two above. */
    const int memoOrderResolved = (&gdx_bridge_memoorder != nullptr) ? gdx_bridge_memoorder() : -1;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[native] interp=%d path=%s cpuclip=%d blendflush=%d palhash=%d texhash_ci=%d texinval=%d "
             "rawcopywhole=%d verify=%d ncensus=%d dspcensus=%d ownfold=%d timgmemo=%d dlfacts=%d stubgen=%d "
             "tmemskip=%d tmemcls=%d texstalefix=%d vtxclamp=%d memoorder=%d tmemfault=%d hooks=%s",
             sInterpOn, sInterpOn ? "owned" : "lus", sCpuClip, sBlendFlush, sPalHash, sTexHashCi,
             sTexInval, sRawCopyWhole, sVerifyOn, sCensus, sDspCensus, sOwnFold, sTimgMemo,
             dlFactsResolved, stubGenResolved, sTmemSkip, sTmemCls, sTexStaleFix, sVtxClamp,
             memoOrderResolved, sTmemFault,
             sInterpOn ? "noop(track,fx)+dl(machines)" : "lus");
    RtLog(buf);
}

/* [dynlod] receipt (port/3ds/gdx3ds_dynlod.c). The LUS path prints it from the bridge's
 * [race-dl] census inside GdxGfxJobRun, which the owned walker never enters, so under interp=1
 * the controller's tier/raises/lowers were invisible ([wobble] audit). Weak like the bridge's. */
extern "C" int gdx3ds_dynlod_receipt(char* buf, int cap) __attribute__((weak));

/* [ncensus] — what ended each batch over the window, ranked by the caller. Two lines:
 *   [ncensus] tasks= batches= b/task= maxb/task= tris/batch= drained= | hist=<8 buckets>
 *   [ncensus] <class>=<drains>/<calls>@<ms> ... | foldp=<n>/<primDrains> folde=<n>/<envDrains>
 *             folded p=<n> e=<n>
 * `drains` = flushes that actually cut a non-empty batch (the split that costs a draw);
 * `calls` = every time the cause fired (an empty batch costs nothing); `ms` = time inside
 * those drains (state application + DrawPackedBatch) over the whole window, not per frame. */
void EmitCensus(const gdxn::Counters& s) {
    static const char* const kName[gdxn::FLUSH_COUNT] = { "none",  "shader", "texture", "depth",  "decal",
                                                          "vport", "scissor", "alpha",  "prim",   "env",
                                                          "fogcol", "blenda", "primdep", "fbop",  "poolfull",
                                                          "taskend", "fogline", "comb",  "sampler", "marker" };
    char buf[400];
    const unsigned drained = [&] {
        unsigned n = 0;
        for (int i = 0; i < gdxn::FLUSH_COUNT; i++) n += s.flushDrain[i];
        return n;
    }();
    int n = snprintf(buf, sizeof(buf),
                     "[ncensus] tasks=%u batches=%u bpt=%u maxbpt=%u tpb=%u drained=%u | hist", s.tasks, s.batches,
                     s.tasks ? s.batches / s.tasks : 0u, s.maxBatchesPerTask,
                     s.batches ? s.batchTris / s.batches : 0u, drained);
    for (int i = 0; i < gdxn::kBatchHistBuckets && n > 0 && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, " %u", s.batchHist[i]);
    }
    RtLog(buf);
    n = snprintf(buf, sizeof(buf), "[ncensus] split");
    for (int i = 1; i < gdxn::FLUSH_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        if (s.flush[i] == 0 && s.flushDrain[i] == 0) continue;
        n += snprintf(buf + n, sizeof(buf) - n, " %s=%u/%u@%u", kName[i], s.flushDrain[i], s.flush[i],
                      (unsigned)((double)s.flushTicks[i] / CPU_TICKS_PER_MSEC * 1000.0)); /* us */
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - n, " | us | foldable p=%u e=%u folded p=%u e=%u", s.foldable[0], s.foldable[1],
                 s.folded[0], s.folded[1]);
    }
    RtLog(buf);
}

/* [dspop] -- the per-opcode SELF-time census of the owned walker ([native] dspcensus).
 * Two lines per receipt window:
 *   [dspop] tasks= cmds= walk=<ms> attributed=<ms> fetch=<ms>/<calls> loop=<ms> | top8 by self ms
 *   [dspop] ph dlresolve=<ms>/<calls> dlhook=.. dlpush=..            (dspcensus=2 only)
 * `walk` is the whole Walk() wall time minus nothing; `attributed` is the sum of the opcode
 * self buckets plus fetch; `loop` is the remainder (the while/stack/branch overhead plus the
 * census's own clock reads). Opcode ms are SELF -- the [prof] vtx/tri/imp/drw/mtx sections a
 * handler entered are already subtracted, so these are the milliseconds that live in `dsp`. */
void EmitDspCensus(const gdxn::Counters& s) {
    char buf[480];
    const double msPerTick = 1.0 / CPU_TICKS_PER_MSEC;
    uint64_t attributed = s.phTicks[gdxn::DSP_PH_FETCH];
    for (int i = 0; i < 256; i++) {
        attributed += s.opTicks[i];
    }
    const uint64_t loop = (s.walkTicks > attributed) ? (s.walkTicks - attributed) : 0;
    int n = snprintf(buf, sizeof(buf),
                     "[dspop] tasks=%u cmds=%u walk=%.2f attributed=%.2f fetch=%.2f/%u loop=%.2f | top",
                     s.tasks, s.cmds, (double)s.walkTicks * msPerTick, (double)attributed * msPerTick,
                     (double)s.phTicks[gdxn::DSP_PH_FETCH] * msPerTick, s.phCalls[gdxn::DSP_PH_FETCH],
                     (double)loop * msPerTick);
    /* Top 8 opcodes by self ticks, selection sort over a local copy (256 entries, once per
     * 64 tasks -- the receipt is not the hot path). */
    uint64_t t[256];
    memcpy(t, s.opTicks, sizeof(t));
    for (int rank = 0; rank < 8 && n > 0 && n < (int)sizeof(buf); rank++) {
        int best = -1;
        uint64_t bestTicks = 0;
        for (int op = 0; op < 256; op++) {
            if (t[op] > bestTicks) {
                bestTicks = t[op];
                best = op;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(buf + n, sizeof(buf) - n, " %02X=%.2f/%u", (unsigned)best, (double)bestTicks * msPerTick,
                      s.op[best]);
        t[best] = 0;
    }
    RtLog(buf);
    if (sDspCensus >= 2) {
        snprintf(buf, sizeof(buf),
                 "[dspop] ph dlresolve=%.2f/%u dlhook*=%.2f/%u dlpush=%.2f/%u | timgres=%.2f/%u "
                 "timgclass=%.2f/%u tmemsame=%.2f/%u tmemcopy=%.2f/%u tmemstore=%.2f/%u | skips=%u",
                 (double)s.phTicks[gdxn::DSP_PH_DLRESOLVE] * msPerTick, s.phCalls[gdxn::DSP_PH_DLRESOLVE],
                 (double)s.phTicks[gdxn::DSP_PH_DLHOOK] * msPerTick, s.phCalls[gdxn::DSP_PH_DLHOOK],
                 (double)s.phTicks[gdxn::DSP_PH_DLPUSH] * msPerTick, s.phCalls[gdxn::DSP_PH_DLPUSH],
                 (double)s.phTicks[gdxn::DSP_PH_TIMGRES] * msPerTick, s.phCalls[gdxn::DSP_PH_TIMGRES],
                 (double)s.phTicks[gdxn::DSP_PH_TIMGCLASS] * msPerTick, s.phCalls[gdxn::DSP_PH_TIMGCLASS],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMSAME] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMSAME],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMCOPY] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMCOPY],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMSTORE] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMSTORE],
                 s.tmemSkips);
        RtLog(buf);
        /* dlhook is starred because it is NOT dispatch -- see DSP_PH_DLHOOK in gdx_interp.h.
           It is the machines DrawMesh billed to G_DL, and it is inclusive where every other
           phase on this line is self time. Printed once per census window so the annotation
           travels with the numbers into whatever log someone reads next. */
        RtLog("[dspop] note: dlhook* is the machines DrawMesh hook (real drawing, inclusive), "
              "not dispatch -- the dispatch part of G_DL is dlresolve");
    }
    /* The full per-opcode call table, so the owned walker's command mix can be diffed against
     * the other walker's without a second run. One line, non-zero entries only. */
    n = snprintf(buf, sizeof(buf), "[dspop] calls");
    for (int op = 0; op < 256 && n > 0 && n < (int)sizeof(buf) - 16; op++) {
        if (s.op[op] != 0) {
            n += snprintf(buf + n, sizeof(buf) - n, " %02X=%u", (unsigned)op, s.op[op]);
        }
    }
    RtLog(buf);
}

void EmitReceipt() {
    const gdxn::Counters& s = sInterp->Stats();
    char buf[560];
    snprintf(buf, sizeof(buf),
             "[ninterp] tasks=%u lists=%u (wide=%u n64=%u be=%u f3d=%u noop=%u) cmds=%u vtx=%u tri=%u rect=%u "
             "tmem=%u batches=%u unknown=%u dropped=%u runaway=%u ovf=%u rootrej=%u | dl=%u vtxop=%u tri2=%u "
             "settimg=%u ldblk=%u mtx=%u | rej=%u cull=%u emit=%u packref=%u packsplit=%u full=%u "
             "| tex hit=%u miss=%u evict=%u clr=%u inval=%u/%u",
             s.tasks, s.lists, s.listsByClass[0], s.listsByClass[1], s.listsByClass[2], s.listsF3d, s.noopLists,
             s.cmds, s.vtx, s.tris, s.rects, s.tmemLoads, s.batches, s.unknownOps, s.droppedOps, s.runaway,
             s.stackOverflow, s.rootRejects, s.op[0xDE], s.op[0x01], s.op[0x06], s.op[0xFD], s.op[0xF3],
             s.op[0xDA], s.trisRejected, s.trisCulled, s.trisEmitted, s.packRefused, s.packSplit, s.flush[gdxn::FLUSH_FULL],
             s.texHits, s.texMisses, s.texEvicts, s.texClears, s.texInvalidated, s.texInvalidates);
    RtLog(buf);
    /* DROPSITES: the always-on drop receipt. Every one of these counted a silent loss that no
     * log line carried -- the texture-cache decode ceiling in particular had a counter that was
     * printed by nothing at all. It is emitted unconditionally, NOT behind [debug] verbose:
     * putting a drop counter behind a verbose gate is the failure mode itself.
     *   texovf  = decodes above the PICA ceiling (no upload was ever possible)
     *   texgrow = decodes the scratch grew for (pre-fix each of these was a SKIPPED texture
     *             whose cache id kept the previous texture's pixels)
     *   texnoup = imports that decoded nothing and took the neutral fallback
     *   texfb   = neutral fallback uploads issued
     *   texhw   = session high-water decode bytes / current scratch capacity
     *   vtxover = G_VTX loads past the 64-slot window / vertices they refused / high-water end
     *   cliphw  = largest clip polygon the run produced (bounded by 3+7=10) / truncations
     *   texuse  = live cache entries / session high-water / capacity    [B23 cache pressure] */
    const gdxn::Interp::SessionDrops d = sInterp->Drops();
    snprintf(buf, sizeof(buf),
             "[ndrops] texovf=%u texgrow=%u texnoup=%u texfb=%u texhw=%uK/%uK | vtxover=%u/%u hw=%u "
             "| cliphw=%u trunc=%u | texuse=%u/%u/%u | ses tex=%u/%u/%u/%u vtx=%u/%u clip=%u",
             s.texDecodeOverflow, s.texDecodeGrow, s.texNoUpload, s.texFallback,
             sInterp->TexDecodeHwBytes() / 1024u, sInterp->TexScratchCap() / 1024u, s.vtxOver,
             s.vtxOverDropped, sInterp->VtxHwEnd(), sInterp->ClipPolyHw(), s.clipTrunc,
             (unsigned)sInterp->TexLive(), (unsigned)sInterp->TexLiveHw(), (unsigned)gdxn::kTexCacheSize,
             d.texOvf, d.texGrow, d.texNoUp, d.texFb, d.vtxOver, d.vtxDropped, d.clipTrunc);
    RtLog(buf);
    if (sTimgMemo) {
        /* [timgmemo] hit/miss/inert per window, plus the load and skip counts it sits under.
           `inert` = the bridge returned epoch 0 (brfast off, legacy resolve, or a dev gate), so
           the memo stood down and the three original calls ran -- a non-zero inert with the
           lever on is the thing to check before reading any A/B from this run. */
        char tm[320];
        uint32_t ep[8] = {};
        gdx_gfx_timg_class_epoch_parts(ep);
        snprintf(tm, sizeof(tm),
                 "[timgmemo] hit=%u miss=%u (empty=%u epoch=%u key=%u) inert=%u epochchg=%u | loads=%u skips=%u "
                 "unres=%u | rgba16gen=%u venue=%08X ver=%u | chg rgba16=%u venue=%u ver=%u seggen*=%u",
                 s.timgMemoHit, s.timgMemoMiss, s.timgMemoMissEmpty, s.timgMemoMissEpoch, s.timgMemoMissKey,
                 s.timgMemoInert, s.timgEpochChanges, s.tmemLoads, s.tmemSkips, s.timgUnresolved,
                 ep[0], ep[1], ep[2], ep[4], ep[5], ep[6], ep[7]);
        RtLog(tm);
    }
    if (sTmemSkip != 0 || sTmemCls != 0) {
        /* [tmemskip] the widened G_LOADBLOCK/G_LOADTILE same-content skip.
             loads/skips  every TMEM load this window, and the ones that skipped the mirror
             imm/gen      which gate fired: the shipping o2r class, or the staleness test
             desc         loads whose DESCRIPTOR matched the live span -- the population a
                          staleness test is even allowed to look at. desc - imm - gen - stale
                          is the loads the descriptor matched but the LEVEL did not admit.
             stale        descriptor matched and the staleness test said the bytes MOVED. This
                          is the column that carries the invalidation claim: it is the memo
                          catching a real write, not a collision.
             cls          every load by source class: Native / Stable / Dma / Unknown. */
        char ts[320];
        snprintf(ts, sizeof(ts),
                 "[tmemskip] lvl=%d loads=%u skips=%u (imm=%u gen=%u) desc=%u stale=%u | "
                 "cls n=%u s=%u d=%u u=%u | miss span=%u kind=%u words=%u addr=%u other=%u | "
                 "fault=%d inj=%u/%u caught=%u missed=%u",
                 sTmemSkip, s.tmemLoads, s.tmemSkips, s.tmemSkipImm, s.tmemSkipGen, s.tmemDescSame,
                 s.tmemStale, s.tmemClsN, s.tmemClsS, s.tmemClsD, s.tmemClsU, s.tmemMissSpan,
                 s.tmemMissKind, s.tmemMissWords, s.tmemMissAddr, s.tmemMissOther,
                 sTmemFault, s.tmemFaultGen, s.tmemFaultCls, s.tmemFaultCaught, s.tmemFaultMissed);
        RtLog(ts);
    }
    if (&gdx3ds_dynlod_receipt != nullptr) {
        char dl[192];
        if (gdx3ds_dynlod_receipt(dl, (int)sizeof(dl)) > 0) {
            RtLog(dl);
        }
    }
    if (sVerifyOn) {
        snprintf(buf, sizeof(buf),
                 "[nverify] tasks=%u batch_mismatch_tasks=%u hist_mismatch_tasks=%u | cls kind=%u state=%u vbo=%u "
                 "extra=%u missing=%u | field 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u 8=%u 9=%u 10=%u 11=%u",
                 sVerifyChecked, sVerifyMismatches, sHistMismatches, sVfyClsKind, sVfyClsState, sVfyClsVbo,
                 sVfyClsExtra, sVfyClsMissing, sVfyFieldHist[1], sVfyFieldHist[2], sVfyFieldHist[3], sVfyFieldHist[4],
                 sVfyFieldHist[5], sVfyFieldHist[6], sVfyFieldHist[7], sVfyFieldHist[8], sVfyFieldHist[9],
                 sVfyFieldHist[10], sVfyFieldHist[11]);
        RtLog(buf);
        sVerifyDetailWindow = 0;
    }
    if (sCensus) {
        EmitCensus(s);
    }
    if (sDspCensus) {
        EmitDspCensus(s);
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

/* The bridge's texture-cache drain broadcasts into the owned cache through these (weakly
 * referenced there, so desktop and the DL harness link nothing). Render thread: the drain is
 * only ever called from GdxGfxJobRun, the VI fallback present, or gdx_ninterp_job_run. */
extern "C" void gdx_ninterp_texcache_clear(void) {
    if (sInterp != nullptr) {
        sInterp->TexCacheClear();
    }
}

extern "C" void gdx_ninterp_texcache_delete(uintptr_t addr) {
    if (sInterp != nullptr) {
        sInterp->TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(addr));
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

extern "C" int gdx_ninterp_palhash(void) {
    Latch();
    return sPalHash;
}

/* Read from the bridge (weakly: desktop and the DL harness link neither this TU nor the ini). */
extern "C" int gdx_ninterp_rawcopywhole(void) {
    Latch();
    return sRawCopyWhole;
}

extern "C" int gdx_ninterp_blendflush(void) {
    Latch();
    return sBlendFlush;
}

extern "C" int gdx_ninterp_census(void) {
    Latch();
    return sCensus;
}

extern "C" int gdx_ninterp_ownfold(void) {
    Latch();
    return sOwnFold;
}

/* Census clock. Only called with ncensus=1 (two reads per drained batch). */
extern "C" long long gdx_ninterp_now(void) {
    return gdx3ds_prof_now();
}

extern "C" int gdx_ninterp_dspcensus(void) {
    Latch();
    return sDspCensus;
}

/* [native] tmemskip / tmemcls -- see Interp::mTmemSkip. */
extern "C" int gdx_ninterp_tmemskip(void) {
    Latch();
    return sTmemSkip;
}

extern "C" int gdx_ninterp_tmemcls(void) {
    Latch();
    return sTmemCls;
}

/* [memotear] [debug] tmemfault -- see sTmemFault. */
extern "C" int gdx_ninterp_tmemfault(void) {
    Latch();
    return sTmemFault;
}

extern "C" int gdx_ninterp_timgmemo(void) {
    Latch();
    return sTimgMemo;
}

/* The running [prof] child-section total, so the dispatch census can subtract the vtx/tri/
 * imp/drw/mtx time a handler spent and report SELF time. Read-only: the census must not
 * publish itself as a child, or the RUN ("dsp") bucket it is explaining would go to zero. */
extern "C" unsigned long long gdx_ninterp_child_ticks(void) {
    return (unsigned long long)gdx3ds_prof_child_ticks;
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
        sInterp->SetCiContentHash(sTexHashCi != 0);
        /* DROPSITES levers + forcing knobs (see docs/research/native/dropsites-progress.md). */
        sInterp->SetTexStaleFix(sTexStaleFix != 0);
        sInterp->SetVtxClamp(sVtxClamp != 0);
        sInterp->SetTexScratchKib(static_cast<uint32_t>(sTexScratchKib), static_cast<uint32_t>(sTexScratchMaxKib));
        sInterp->SetVtxSoftCap(static_cast<uint32_t>(sVtxMax));
        sInterp->SetClipPolyMax(static_cast<uint32_t>(sClipPolyMax));
    }
    /* The owned cache is the live one under interp=1 and GdxGfxJobRun -- where the LUS path
     * drains -- is never reached, so the drain runs here, at the same point in the task:
     * before the walk, so an eviction raised for this frame's content takes effect now. */
    if (sTexInval) {
        gdx_gfx_texcache_drain();
    }
    GdxFrameConfig cfg = {};
    if (!gdx_gfx_frame_config(&cfg)) {
        return 0; /* no window yet: let the LUS path take it (it early-outs the same way) */
    }
    /* Task variant: the job's entry ucode (F3dex2Variant numbering). */
    const int variant = gdx_gfx_job_ucode(job);

    /* [nverify] the pre-task counter snapshot is ~1.2 KB (Counters leads with uint32_t op[256])
     * and is read ONLY by the verify block below and by VerifyHistogram, so it is copied under
     * the lever, not on every task. Audit 2026-09-23 finding 31, same class. */
    if (sVerifyOn) {
        const gdxn::Counters before = sInterp->Stats();
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
            if (cls & 1) sVfyClsKind++;
            if (cls & 2) sVfyClsState++;
            if (cls & 4) sVfyClsVbo++;
            if (cls & 8) sVfyClsExtra++;
            if (cls & 16) sVfyClsMissing++;
            if (detail < 12) sVfyFieldHist[detail]++;
            if (sVerifyDetailWindow < kVerifyDetailPerWindow) {
                sVerifyDetailWindow++;
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
                gdx3ds_verify_bytes_detail(buf, sizeof(buf));
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
