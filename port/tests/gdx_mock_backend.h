/* port/tests/gdx_mock_backend.h -- host mock of gdxn::Backend for the owned interpreter's
 * harness (docs/research/native/06-interp-plan.md §6.3). Records shader ids, uploads and
 * packed batches; hands out a heap VBO pool exactly like VboPackBegin does. */
#ifndef GDX_MOCK_BACKEND_H
#define GDX_MOCK_BACKEND_H

#include <stdint.h>
#include <string.h>

#include <vector>

#include "gdx_backend.h"

struct MockUpload {
    uint32_t id, w, h;
    std::vector<uint8_t> rgba;
};
struct MockBatch {
    uint64_t id0, id1;
    uint32_t tex[2];
    size_t numTris;
    std::vector<float> vbo;
    Gdx3dsVboPackAux aux;
    bool depthTest, depthMask, decal, alpha;
    int vp[4], sc[4];
};

class MockBackend final : public gdxn::Backend {
  public:
    struct Prg {
        uint64_t id0, id1;
        bool used[2];
        uint8_t numInputs;
    };
    std::vector<Prg> shaders;
    std::vector<MockUpload> uploads;
    std::vector<MockBatch> batches;
    uint32_t nextTex = 1;
    uint32_t bound[2] = { 0, 0 };
    int lastTile = 0;
    bool depthTest = false, depthMask = false, decal = false, alpha = false;
    int vp[4] = { 0, 0, 0, 0 }, sc[4] = { 0, 0, 0, 0 };
    float primDepth = 0, alphaThr = 0;
    int frames = 0, clears = 0;
    std::vector<float> pool = std::vector<float>(65536);
    size_t poolOff = 0;
    void* current = nullptr;
    bool trect = false;

    gdxn::ClipParams GetClipParameters() override {
        return gdxn::ClipParams{ true, false };
    }
    int GetMaxTextureSize() override {
        return 1024;
    }
    void UnloadShader(void*) override {
    }
    void* LookupShader(uint64_t id0, uint64_t id1) override {
        for (auto& p : shaders) {
            if (p.id0 == id0 && p.id1 == id1) return &p;
        }
        return nullptr;
    }
    void* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) override {
        shaders.reserve(64);
        Prg p{ id0, id1, { false, false }, 0 };
        /* usedTextures / numInputs from the id, as gfx_cc_get_features does */
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 4; j++) {
                    const int v = (id0 >> (c * 32 + i * 16 + j * 4)) & 0xf;
                    if (v >= 1 && v <= 7 && v > p.numInputs) p.numInputs = v;
                    if (v == 8 || v == 9) p.used[0] = true;
                    if (v == 10 || v == 11) p.used[1] = true;
                }
            }
        }
        shaders.push_back(p);
        current = &shaders.back();
        return current;
    }
    void LoadShader(void* prg) override {
        current = prg;
    }
    void ShaderGetInfo(void* prg, uint8_t* numInputs, bool used[2]) override {
        const Prg* p = static_cast<const Prg*>(prg);
        *numInputs = p->numInputs;
        used[0] = p->used[0];
        used[1] = p->used[1];
    }
    uint32_t NewTexture() override {
        return nextTex++;
    }
    void SelectTexture(int tile, uint32_t id) override {
        bound[tile & 1] = id;
        lastTile = tile;
    }
    void UploadTexture(const uint8_t* rgba32, uint32_t w, uint32_t h) override {
        MockUpload u;
        u.id = bound[lastTile & 1];
        u.w = w;
        u.h = h;
        u.rgba.assign(rgba32, rgba32 + static_cast<size_t>(w) * h * 4);
        uploads.push_back(u);
    }
    void SetSamplerParameters(int, bool, uint32_t, uint32_t) override {
    }
    void DeleteTexture(uint32_t) override {
    }
    int GetTextureFilter() override {
        return 0;
    }
    void SetDepthTestAndMask(bool t, bool m) override {
        depthTest = t;
        depthMask = m;
    }
    void SetZmodeDecal(bool d) override {
        decal = d;
    }
    void SetViewport(int x, int y, int w, int h) override {
        vp[0] = x; vp[1] = y; vp[2] = w; vp[3] = h;
    }
    void SetScissor(int x, int y, int w, int h) override {
        sc[0] = x; sc[1] = y; sc[2] = w; sc[3] = h;
    }
    void SetUseAlpha(bool a) override {
        alpha = a;
    }
    void SetCurrentPrimDepth(float d) override {
        primDepth = d;
    }
    void SetCurrentAlphaCompareThreshold(float t) override {
        alphaThr = t;
    }
    void DrawTriangles(float[], size_t, size_t) override {
    }
    float* VboPackBegin(Gdx3dsVboPackParams* p) override {
        if (current == nullptr) return nullptr;
        const Prg* prg = static_cast<const Prg*>(current);
        memset(p, 0, sizeof(*p));
        for (int t = 0; t < 2; t++) {
            p->uScale[t] = p->vScale[t] = 1.0f;
            p->wrapClampU[t] = p->wrapClampV[t] = 1e30f;
            p->used[t] = prg->used[t] ? 1 : 0;
        }
        p->shaderClampGate = 1;
        p->vtxColorInput = prg->numInputs > 0 ? 1 : 0;
        p->vtxAlphaInput = prg->numInputs > 0 ? 1 : 0;
        p->wantVtxAlpha = prg->numInputs > 0 ? 1 : 0;
        if (poolOff + 256 * 3 * 12 > pool.size()) return nullptr;
        return pool.data() + poolOff;
    }
    void DrawPackedBatch(size_t numTris, const Gdx3dsVboPackAux* aux) override {
        MockBatch b;
        const Prg* prg = static_cast<const Prg*>(current);
        b.id0 = prg ? prg->id0 : 0;
        b.id1 = prg ? prg->id1 : 0;
        b.tex[0] = bound[0];
        b.tex[1] = bound[1];
        b.numTris = numTris;
        b.vbo.assign(pool.data() + poolOff, pool.data() + poolOff + numTris * 3 * 12);
        b.aux = *aux;
        b.depthTest = depthTest; b.depthMask = depthMask; b.decal = decal; b.alpha = alpha;
        memcpy(b.vp, vp, sizeof(vp));
        memcpy(b.sc, sc, sizeof(sc));
        batches.push_back(b);
        poolOff += numTris * 3 * 12;
    }
    void StartFrame() override {
        frames++;
        poolOff = 0;
    }
    void UpdateFramebufferParameters(int, uint32_t, uint32_t, uint32_t, bool, bool, bool, bool) override {
    }
    void StartDrawToFramebuffer(int, float) override {
    }
    void ClearFramebuffer(bool, bool) override {
        clears++;
    }
    void ClearDepthRegion(int, int, int, int) override {
    }
    void FogNoteParams(int, int, int) override {
    }
    void* GetFramebufferTextureId(int) override {
        return nullptr;
    }
    void SelectTextureFb(int) override {
    }
    bool ProfActive() override {
        return false;
    }
    uint64_t ProfEnter(uint64_t*) override {
        return 0;
    }
    void ProfExit(int, uint64_t, uint64_t) override {
    }
    void VerifyNoteTri(const void*) override {
    }
    void VerifyNoteCmd(uint32_t, uint32_t) override {
    }
    void TexCacheNoteImport() override {
    }
    void TexCacheNoteMiss(const void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                          unsigned) override {
    }
    bool TrectBatchEnabled() override {
        return trect;
    }
    void AtlasArm(bool) override {
    }
    int TexPage(uint32_t) override {
        return -1;
    }
    void VboPackRefresh(Gdx3dsVboPackParams*) override {
    }
};

#endif /* GDX_MOCK_BACKEND_H */
