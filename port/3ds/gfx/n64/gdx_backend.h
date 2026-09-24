/* port/3ds/gfx/n64/gdx_backend.h -- the backend surface the owned interpreter drives.
 *
 * This is exactly the subset of Fast::GfxRenderingAPI (+ the [triloop] packed-VBO and
 * [trectbatch] atlas entry points) that the LUS interpreter calls on the 3DS, spelled without
 * LUS headers so gdx_interp.cpp compiles on the host against a mock. gdx_rt_glue.cpp adapts
 * it to Fast::GfxRenderingAPIC3D (gfx_citro3d.h); the citro3d backend itself is unchanged
 * (plan §2.4). Shader handles are opaque: the backend keys them by the LUS (id0,id1) pair
 * and gdx_cc reproduces those ids bit-for-bit. */
#ifndef GDX_BACKEND_H
#define GDX_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "gdx3ds_vbopack.h"

namespace gdxn {

struct ClipParams {
    bool zIsFrom0To1;
    bool invertY;
};

class Backend {
  public:
    virtual ~Backend() {}

    virtual ClipParams GetClipParameters() = 0;
    virtual int GetMaxTextureSize() = 0;

    virtual void UnloadShader(void* prg) = 0;
    virtual void* LookupShader(uint64_t id0, uint64_t id1) = 0;
    virtual void* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) = 0;
    virtual void LoadShader(void* prg) = 0;
    virtual void ShaderGetInfo(void* prg, uint8_t* numInputs, bool usedTextures[2]) = 0;

    virtual uint32_t NewTexture() = 0;
    virtual void SelectTexture(int tile, uint32_t textureId) = 0;
    virtual void UploadTexture(const uint8_t* rgba32, uint32_t width, uint32_t height) = 0;
    virtual void SetSamplerParameters(int sampler, bool linearFilter, uint32_t cms, uint32_t cmt) = 0;
    virtual void DeleteTexture(uint32_t texId) = 0;
    virtual int GetTextureFilter() = 0;

    virtual void SetDepthTestAndMask(bool depthTest, bool zUpd) = 0;
    virtual void SetZmodeDecal(bool decal) = 0;
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetScissor(int x, int y, int width, int height) = 0;
    virtual void SetUseAlpha(bool useAlpha) = 0;
    virtual void SetCurrentPrimDepth(float depth) = 0;
    virtual void SetCurrentAlphaCompareThreshold(float threshold) = 0;

    virtual void DrawTriangles(float bufVbo[], size_t bufVboLen, size_t bufVboNumTris) = 0;
    /* GEOMLOSS: capTris receives how many triangles still fit in the backend's pool.
     * The owned interpreter has no legacy variable-stride path, so it splits its batch
     * at that cap instead of dropping it; a NULL return now means genuine exhaustion. */
    virtual float* VboPackBegin(Gdx3dsVboPackParams* params, uint32_t* capTris) = 0;
    virtual void DrawPackedBatch(size_t numTris, const Gdx3dsVboPackAux* aux) = 0;

    virtual void StartFrame() = 0;
    virtual void UpdateFramebufferParameters(int fbId, uint32_t width, uint32_t height, uint32_t msaaLevel,
                                             bool invertY, bool renderTarget, bool hasDepth,
                                             bool canExtractDepth) = 0;
    virtual void StartDrawToFramebuffer(int fbId, float noiseScale) = 0;
    virtual void ClearFramebuffer(bool color, bool depth) = 0;
    virtual void ClearDepthRegion(int x, int y, int width, int height) = 0;
    /* gdx3ds_fog_note_params: the fog line the batch was baked under (exact=0 for G_BL_CLR_BL). */
    virtual void FogNoteParams(int mul, int off, int exact) = 0;
    virtual void* GetFramebufferTextureId(int fbId) = 0;
    virtual void SelectTextureFb(int fbId) = 0;

    /* [prof] sectioned CPU profiler (gdx3ds_gpu_prof.h, gputrace-gated). sec: 2 VTX 3 TRI 4 IMP 6 MTX. */
    virtual bool ProfActive() = 0;
    virtual uint64_t ProfEnter(uint64_t* childSnap) = 0;
    virtual void ProfExit(int sec, uint64_t t0, uint64_t childSnap) = 0;

    /* [nverify] per-tri interpreter inputs (Gdx3dsVerifyTri*); no-op outside verify.
     * VerifyActive is latched ONCE per task in RunFrame (Interp::mVerifyActive) and the two
     * hot sites test that bool, so with [native] interp_verify=0 the per-command and
     * per-triangle diagnostics cost one predictable branch and nothing else -- no memset, no
     * FNV, no virtual call (audit 2026-09-23 finding 31). */
    virtual bool VerifyActive() = 0;
    virtual void VerifyNoteTri(const void* tri) = 0;
    virtual void VerifyNoteCmd(uint32_t w0, uint32_t w1) = 0;

    /* [c3d] texImp/texMiss receipts (gdx3ds_texcache_note_import/miss). */
    virtual void TexCacheNoteImport() = 0;
    virtual void TexCacheNoteMiss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord, unsigned lineBytes,
                                  unsigned sizeBytes, unsigned tileW, unsigned tileH, unsigned hash) = 0;

    /* [trectbatch] atlas contract (weak on the LUS side; here plain virtuals the glue routes to
     * the C shims). page < 0 = standalone texture. */
    /* Gdx_NativeBatchConstInputs: the combiner-input slots (1-based, as a bitmask over
     * shader_input_mapping[..][0..6]) that the CURRENT shader binds as TexEnv stage
     * constants from vertex 0. An input NOT in the mask rides the vertex colour attribute,
     * so a register feeding only such inputs may change without splitting the batch.
     * Returns false when no shader is bound (then nothing may be folded). */
    virtual bool BatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) = 0;

    virtual bool TrectBatchEnabled() = 0;
    virtual void AtlasArm(bool on) = 0;
    virtual int TexPage(uint32_t texId) = 0;
    virtual void VboPackRefresh(Gdx3dsVboPackParams* params) = 0;
};

} // namespace gdxn

#endif /* GDX_BACKEND_H */
