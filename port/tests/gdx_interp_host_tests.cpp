/* port/tests/gdx_interp_host_tests.cpp -- host harness for the owned F3DEX2 interpreter
 * (docs/research/native/06-interp-plan.md §6.3). Feeds hand-built Gfx arrays (16-byte tagged
 * and 8-byte forms) into the walker with a mock backend and a mock resolver seam, and asserts
 * walk counts, combiner ids, TMEM spans, decoded texels and packed-VBO floats.
 *
 * Build (tools/ci-3ds.sh): c++ -std=c++20 -O1 port/tests/gdx_interp_host_tests.cpp
 *   port/3ds/gfx/n64/{gdx_dlsrc,gdx_dl,gdx_interp,gdx_cc,gdx_tmem,gdx_texcache,gdx_rsp,gdx_tri,gdx_rect}.cpp
 *   -Iport/3ds/gfx/n64 -Iport/3ds/gfx -Iport -Iport/tests   (n64 first: port/gdx_interp.h exists too) */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "gdx_dlsrc_bridge.h"
#include "gdx_interp.h"
#include "gdx_mock_backend.h"

/* ---- mock seam: a fixed set of host lists/data blocks, resolvable by low32 -------------- */
struct MockRange {
    uintptr_t base;
    size_t size;
    uint8_t stride;
    uint8_t isBig;
    uint8_t isF3d;
    uint8_t isList;
};
static std::vector<MockRange> gRanges;
static uintptr_t gRoot = 0;
static size_t gRootBytes = 0;
static uintptr_t gSegs[16] = {};

static const MockRange* FindRange(uintptr_t p) {
    for (auto& r : gRanges) {
        if (p >= r.base && p < r.base + r.size) return &r;
    }
    return nullptr;
}
/* Host pointers are 64-bit here but the GDX1 scheme carries low32 only (exact on the 32-bit
 * device): the mock maps a tagged low32 back onto its range. */
static uintptr_t ResolveLow32(uint32_t raw) {
    const uint8_t seg = raw >> 24;
    if (seg != 0 && seg < 16 && gSegs[seg] != 0) return gSegs[seg] + (raw & 0xFFFFFF);
    for (auto& r : gRanges) {
        if (raw >= static_cast<uint32_t>(r.base) && raw < static_cast<uint32_t>(r.base + r.size)) return r.base + (raw - static_cast<uint32_t>(r.base));
    }
    return 0;
}

extern "C" {
int gdx_gfx_job_root_list(GdxGfxJob*, GdxDlSrcList* out) {
    const MockRange* r = FindRange(gRoot);
    if (r == nullptr) return 0;
    out->ptr = reinterpret_cast<const void*>(gRoot);
    out->limit = gRootBytes / r->stride;
    out->stride = r->stride;
    out->isBig = r->isBig;
    out->isF3d = r->isF3d;
    return 1;
}
int gdx_gfx_job_resolve_list(GdxGfxJob*, uint32_t raw, int hostTagged, uintptr_t hostFull, GdxDlSrcList* out) {
    const uintptr_t p = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    const MockRange* r = FindRange(p);
    if (r == nullptr || !r->isList) return 0;
    out->ptr = reinterpret_cast<const void*>(p);
    out->limit = (r->base + r->size - p) / r->stride;
    out->stride = r->stride;
    out->isBig = r->isBig;
    out->isF3d = r->isF3d;
    return 1;
}
uintptr_t gdx_gfx_job_resolve_data(GdxGfxJob*, uint32_t raw, int hostTagged, uintptr_t hostFull, size_t need, int, uintptr_t) {
    const uintptr_t p = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    const MockRange* r = FindRange(p);
    if (r == nullptr || p + need > r->base + r->size) return 0;
    return p;
}
uintptr_t gdx_gfx_job_resolve_timg_host(GdxGfxJob*, uintptr_t hostFull, size_t need) {
    const uintptr_t p = ResolveLow32(static_cast<uint32_t>(hostFull));
    const MockRange* r = FindRange(p);
    if (r == nullptr || p + need > r->base + r->size) return 0;
    return p;
}
unsigned gdx_gfx_classify_host_ptr(unsigned long) {
    return 0;
}
int gdx_gfx_timg_is_resource(uint32_t, int, uintptr_t, size_t) {
    return 0;
}
int gdx_gfx_texels_are_native_rgba16(uintptr_t, size_t) {
    return 0;
}
size_t gdx_gfx_readable_bytes(uintptr_t p) {
    const MockRange* r = FindRange(p);
    return r ? (r->base + r->size - p) : 0;
}
int gdx_gfx_low32_needs_resolver(uint32_t) {
    return 0;
}
uintptr_t gdx_gfx_job_set_segment(GdxGfxJob*, unsigned seg, uint32_t raw, int hostTagged, uintptr_t hostFull) {
    gSegs[seg & 15] = hostTagged ? ResolveLow32(static_cast<uint32_t>(hostFull)) : ResolveLow32(raw);
    return gSegs[seg & 15];
}
int gdx_gfx_ucode_variant(uint32_t raw, int, uintptr_t, int* variant, int* isLine) {
    *isLine = 0;
    *variant = static_cast<int>(raw & 3);
    return raw >= 0x1000 ? 1 : 0;
}
void gdx_gfx_job_run_lus(GdxGfxJob*) {
}
int gdx_gfx_job_ucode(GdxGfxJob*) {
    return 0;
}
void gdx_gfx_job_bridge_stats(GdxGfxJob*, unsigned long* a, unsigned long* b, unsigned long c[256]) {
    *a = *b = 0;
    memset(c, 0, 256 * sizeof(unsigned long));
}
void gdx_gfx_job_epilogue(GdxGfxJob*) {
}
int gdx_gfx_frame_config(GdxFrameConfig*) {
    return 0;
}
void gdx_gfx_frame_publish(uintptr_t) {
}
int gdx_ninterp_blendflush(void) {
    return 1;
}
int gdx_ninterp_cpuclip(void) {
    return 1;
}
void gdxn_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
}

/* ---- test scaffolding -------------------------------------------------------------------- */
static int gFails = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            gFails++;                                                                 \
        }                                                                             \
    } while (0)

/* Wide (16-byte tagged) command builder. */
struct W {
    uint32_t w0, pad, w1lo, w1hi;
};
static W Wv(uint32_t w0, uint32_t w1) {
    return W{ w0, 0, w1, 0 };
}
static W Wp(uint32_t w0, const void* p) {
    return W{ w0, 0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)), gdxn::kHostTag };
}
static void Reg(const void* p, size_t size, uint8_t stride, bool big, bool f3d, bool list) {
    MockRange r;
    r.base = reinterpret_cast<uintptr_t>(p);
    r.size = size;
    r.stride = stride;
    r.isBig = big ? 1 : 0;
    r.isF3d = f3d ? 1 : 0;
    r.isList = list ? 1 : 0;
    gRanges.push_back(r);
}
static GdxFrameConfig Cfg() {
    GdxFrameConfig c;
    memset(&c, 0, sizeof(c));
    c.windowW = c.curW = 400;
    c.windowH = c.curH = 240;
    c.nativeW = 320;
    c.nativeH = 240;
    c.internalMul = 1.0f;
    c.widescreen = 1;
    return c;
}

/* Test 1: flow -- push/branch/ENDDL, histogram, F3D remap on a narrow list. */
static void TestWalk() {
    static W sub[] = { Wv(0xE7000000, 0), Wv(0xDF000000, 0) };
    static W root[8];
    root[0] = Wv(0xE7000000, 0);
    root[1] = Wp(0xDE000000, sub);  /* push */
    root[2] = Wv(0xE6000000, 0);
    root[3] = Wp(0xDE010000, sub);  /* branch: ends the root */
    root[4] = Wv(0xE7000000, 0);
    root[5] = Wv(0xDF000000, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(sub, sizeof(sub), 16, false, false, true);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = sizeof(root);
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    const gdxn::Counters& s = it.Stats();
    CHECK(s.lists == 3);
    CHECK(s.op[0xE7] == 3); /* root, sub (push), sub (branch) */
    CHECK(s.op[0xE6] == 1);
    CHECK(s.op[0xDF] == 2);
    CHECK(s.op[0xDE] == 2);
    CHECK(s.cmds == 8);
    CHECK(mb.frames == 1 && mb.clears == 1);

    /* narrow legacy-F3D list: 0xB8 ENDDL, 0xBF TRI1 with *10 indices */
    static uint32_t f3d[] = { 0xBF000000u, 0x00000A14u /* v0=0,v1=1,v2=2 (x10) */, 0xB8000000u, 0 };
    gRanges.clear();
    Reg(f3d, sizeof(f3d), 8, false, true, true);
    gRoot = reinterpret_cast<uintptr_t>(f3d);
    gRootBytes = sizeof(f3d);
    it.ResetStats();
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().op[0xBF] == 1 && it.Stats().op[0xB8] == 1);
    CHECK(it.Stats().listsF3d == 1);
    CHECK(it.Stats().tris == 1); /* submitted (vertices are zero -> rejected/culled downstream) */
}

/* Test 2: combiner ids for the census's most common mode (TEXEL0*SHADE, 1-cycle). */
static void TestCC() {
    gdxn::CombinerKey k;
    /* gDPSetCombineLERP(TEXEL0,0,SHADE,0, TEXEL0,0,SHADE,0, ...) -> color_comb(1,31,4,31)=? */
    const uint32_t cc = (1u & 0xF) | ((31u & 0xF) << 4) | ((4u & 0x1F) << 8) | ((7u & 7) << 13);
    const uint32_t ac = (1u & 7) | ((7u & 7) << 3) | ((4u & 7) << 6) | ((7u & 7) << 9);
    k.combine_mode = static_cast<uint64_t>(cc) | (static_cast<uint64_t>(ac) << 16);
    k.options = 0;
    gdxn::Combiner c;
    gdxn::GenerateCC(&c, k);
    /* rgb: A=TEXEL0(8) B=0 C=SHADE->INPUT_1 D=0 ; alpha: A=TEXEL0 B=0 C=SHADE->INPUT_1 D=0 */
    CHECK(((c.shader_id0 >> 0) & 0xF) == 8);
    CHECK(((c.shader_id0 >> 4) & 0xF) == 0);
    CHECK(((c.shader_id0 >> 8) & 0xF) == 1);
    CHECK(((c.shader_id0 >> 16) & 0xF) == 8);
    CHECK(((c.shader_id0 >> 24) & 0xF) == 1);
    CHECK(c.usedTextures[0] && !c.usedTextures[1]);
    CHECK(c.shader_input_mapping[0][0] == 4 && c.shader_input_mapping[1][0] == 4);
    gdxn::CombinerPool pool;
    bool created = false;
    gdxn::Combiner* a = pool.Lookup(k, &created);
    CHECK(created && a != nullptr);
    gdxn::Combiner* b = pool.Lookup(k, &created);
    CHECK(!created && a == b);
}

/* Test 3: SETTIMG/SETTILE/LOADBLOCK/SETTILESIZE + TEXRECT: TMEM span, RGBA16 decode, one
 * packed batch of two rect tris with the expected NDC corners. */
static void TestRectTexture() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1); /* red, opaque */
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);           /* othermode_h cycle type 1cyc (shift 20 len 2) */
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);           /* combine: TEXEL0 only */
    root[n++] = Wp(0xFD10000Fu, texels);                /* SETTIMG RGBA16 w=16 */
    root[n++] = Wv(0xF5100000u, 0x07000000u);           /* SETTILE RGBA16 line 0 tmem 0 -> tile 7 */
    root[n++] = Wv(0xF3000000u, 0x070FF800u);           /* LOADBLOCK tile 7 lrs=255 dxt=0x800 */
    root[n++] = Wv(0xF5100800u, 0x00000000u);           /* SETTILE RGBA16 line 4 (32B) tmem 0 tile 0 */
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);           /* SETTILESIZE tile0 0,0..15,15 */
    root[n++] = Wv(0xE4064064u, 0x00000000u);           /* TEXRECT lrx=100 lry=100 (U10.2 -> 25px) ulx=0 uly=0 tile 0 */
    root[n++] = Wv(0xE1000000u, 0x00000000u);           /* RDPHALF_1: s,t = 0 */
    root[n++] = Wv(0xF1000000u, 0x04000400u);           /* RDPHALF_2: dsdx=dtdy=1.0 (S5.10 0x400) */
    root[n++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    const gdxn::Counters& s = it.Stats();
    CHECK(s.tmemLoads == 1);
    CHECK(s.rects == 1);
    CHECK(s.texImports == 1 && s.texMisses == 1);
    CHECK(mb.uploads.size() == 1);
    if (!mb.uploads.empty()) {
        CHECK(mb.uploads[0].w == 16 && mb.uploads[0].h == 16);
        CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[1] == 0 && mb.uploads[0].rgba[3] == 255);
    }
    CHECK(mb.batches.size() == 1);
    if (!mb.batches.empty()) {
        const MockBatch& b = mb.batches[0];
        CHECK(b.numTris == 2);
        /* ul corner: ulx=0 -> x=-1 (then hor+ 4:3/(400/240) = 0.8), uly=0 -> y=+1 */
        CHECK(fabsf(b.vbo[0] - (-0.8f)) < 1e-6f);
        CHECK(fabsf(b.vbo[1] - 1.0f) < 1e-6f);
        CHECK(b.vbo[3] == 1.0f);
        /* z is from 0..1 in the mock: (-1 + 1)/2 = 0 */
        CHECK(b.vbo[2] == 0.0f);
        CHECK(b.tex[0] == 1);
    }
    /* second identical frame: span same-content skip is gated off (raw source) -> a load, a
       cache HIT, no new upload, and identical floats */
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().texHits == 1);
    CHECK(mb.uploads.size() == 1);
    CHECK(mb.batches.size() == 2);
    if (mb.batches.size() == 2) {
        CHECK(memcmp(mb.batches[0].vbo.data(), mb.batches[1].vbo.data(), mb.batches[0].vbo.size() * 4) == 0);
    }
}

/* Test 4: G_MTX + G_VTX + G_TRI1 on a narrow big-endian list with byte-swapped operands. */
static void TestTri3D() {
    /* identity fixed-point matrix: int parts diag 1, frac 0 -- packed as N64 Mtx (big-endian) */
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    /* int part rows: word i*2 + j/2 holds (m[i][j] << 16) | m[i][j+1] */
    /* word i*2 + j/2 = (m[i][j] << 16) | m[i][j+1] */
    mtx[0] = 0x00010000u; /* m00=1 m01=0 */
    mtx[2] = 0x00000001u; /* m10=0 m11=1 */
    mtx[5] = 0x00010000u; /* m22=1 m23=0 */
    mtx[7] = 0x00000001u; /* m32=0 m33=1 */
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } }; /* inside |x|,|y| <= w=1 */
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    /* narrow big-endian list, segment 6 tokens */
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;   /* MTX modelview|load|nopush (params 2 ^ push=1 -> 3) */
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;   /* MTX projection|load|nopush (params 6 ^ push=1 -> 7) */
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;   /* VTX n=3 v0=0 */
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;   /* combine */
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;   /* geometry mode set SHADE */
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;   /* TRI1 0,1,2 */
    dl[n++] = 0xDF000000u; dl[n++] = 0;
    for (int i = 0; i < n; i++) dl[i] = gdxn::Bswap32(dl[i]);
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl, sizeof(dl), 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl);
    gRootBytes = static_cast<size_t>(n) * 4;
    MockBackend mb;
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    CHECK(it.Stats().vtx == 3);
    CHECK(it.Stats().tris == 1 && it.Stats().trisEmitted == 1);
    CHECK(mb.batches.size() == 1);
    if (!mb.batches.empty()) {
        const MockBatch& b = mb.batches[0];
        /* identity MP: x = ob.x * aspect adj (0.8), y = ob.y, w = 1 */
        CHECK(fabsf(b.vbo[0] - (-0.8f)) < 1e-6f);
        CHECK(fabsf(b.vbo[1] - (-1.0f)) < 1e-6f);
        CHECK(b.vbo[3] == 1.0f);
        /* shade rgb from cn: 255,128,0 -> 1, 128/255, 0 */
        CHECK(b.vbo[6] == 1.0f && fabsf(b.vbo[7] - 128.0f / 255.0f) < 1e-7f && b.vbo[8] == 0.0f);
    }
    gSegs[6] = 0;
}

int main() {
    TestWalk();
    TestCC();
    TestRectTexture();
    TestTri3D();
    if (gFails == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURES ===\n", gFails);
    return 1;
}
