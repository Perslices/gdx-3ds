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

/* [native] ncensus / ownfold knobs for the fold tests (the device reads the ini). */
static int gdx_test_census = 0;
static int gdx_test_ownfold = 0;
static int gdx_test_dspcensus = 0;
/* [native] timgmemo: ON in the host tests, so every TMEM test in this file exercises the memo's
   hit path. The mock bridge's class epoch is a non-zero constant, which is exactly the "nothing
   invalidates" case -- so any test that goes red here is the memo returning a DIFFERENT answer
   than the three calls it replaces, which is the only thing a host test can prove about it. */
static int gdx_test_timgmemo = 1;
/* [native] tmemskip: the LEVEL under test. The mock source-stability seam below is the honest
   mock for a host harness that has no bridge tables: it classifies every mock range STABLE and
   answers "unchanged", so tmemskip=1 here is the STRONGEST form of the skip and any TMEM test
   that goes red is the widened gate skipping a load the shipping gate would have performed.
   The suite runs the TMEM tests at BOTH levels -- see TmemSkipSuite. */
static int gdx_test_tmemskip = 0;
static int gdx_test_tmemfault = 0;   /* [memotear] B54 fault injection */
static int gdx_test_tmemcls = 0;
/* Mock generations, so a test can move one and prove the skip stops firing, and the class the
   mock seam reports for an in-range source (STABLE unless a test asks for another). */
static uint64_t gdx_test_native_gen = 1;
static uint64_t gdx_test_dma_gen = 1;
static unsigned gdx_test_src_cls = GDX_TMEMSRC_STABLE;

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
void gdx_gfx_timg_class_epoch_parts(uint32_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = 0;
}
uint32_t gdx_gfx_timg_class_epoch(void) {
    /* The mock tables never mutate, so one constant epoch is the honest answer and it exercises
       the memo's hit path in the host tests. */
    return 1u;
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
/* [tmemblock] source-stability seam. Every mock range is a registered host range that no DMA
   ever writes, so the honest mock answers STABLE; a test that wants to see the skip STOP moves
   gdx_test_native_gen / gdx_test_dma_gen and asks for the matching class. */
unsigned gdx_gfx_tmem_source_class(uintptr_t source, size_t size) {
    const MockRange* r = FindRange(source);
    if (r == nullptr || size == 0 || source + size > r->base + r->size) {
        return GDX_TMEMSRC_UNKNOWN;
    }
    return gdx_test_src_cls;
}
void gdx_gfx_tmem_source_stamp(uint64_t out[2]) {
    out[0] = gdx_test_native_gen;
    out[1] = gdx_test_dma_gen;
}
int gdx_gfx_tmem_source_unchanged(uintptr_t source, size_t size, unsigned clsAtStamp, const uint64_t stamp[2]) {
    const unsigned cls = gdx_gfx_tmem_source_class(source, size);
    if (cls != clsAtStamp || stamp == nullptr) return 0;
    if (cls == GDX_TMEMSRC_NATIVE) return stamp[0] == gdx_test_native_gen ? 1 : 0;
    if (cls == GDX_TMEMSRC_DMA) return stamp[1] == gdx_test_dma_gen ? 1 : 0;
    return cls == GDX_TMEMSRC_STABLE ? 1 : 0;
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
int gdx_ninterp_census(void) {
    return gdx_test_census;
}
int gdx_ninterp_ownfold(void) {
    return gdx_test_ownfold;
}
int gdx_ninterp_palhash(void) {
    return 1;
}
long long gdx_ninterp_now(void) {
    return 0;
}
int gdx_ninterp_dspcensus(void) {
    return gdx_test_dspcensus;
}
int gdx_ninterp_timgmemo(void) {
    return gdx_test_timgmemo;
}
int gdx_ninterp_tmemskip(void) {
    return gdx_test_tmemskip;
}
int gdx_ninterp_tmemcls(void) {
    return gdx_test_tmemcls;
}
int gdx_ninterp_tmemfault(void) {
    return gdx_test_tmemfault;
}
/* [memotear] B54: the mock's generations are the ones gdx_gfx_tmem_source_unchanged above
   reads, so moving them here is the same fault the real seam injects through the bridge's bump
   helper and dirty-range recorder. STABLE has no generation arm in the real seam either, so the
   mock answers 0 for it and the injector falls back to the class mechanism -- which is the
   behaviour under test, not a mock limitation. */
int gdx_gfx_tmem_fault_move_generation(uintptr_t, size_t, unsigned cls) {
    if (cls == GDX_TMEMSRC_NATIVE) {
        gdx_test_native_gen++;
        return 1;
    }
    if (cls == GDX_TMEMSRC_DMA) {
        gdx_test_dma_gen++;
        return 1;
    }
    return 0;
}
unsigned long long gdx_ninterp_child_ticks(void) {
    return 0;
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

/* Test 5: [native] ownfold -- the prim/env fold inside the owned batches. Two texrects with a
 * SETPRIMCOLOR value change between them, under a combiner that reads PRIMITIVE. The fold must
 * be taken ONLY when the backend says no stage constant of the open batch reads that register. */
static void TestOwnFold() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    /* combiner cycle 0: rgb = (TEXEL0 - 0) * PRIMITIVE + 0, alpha = (TEXEL0 - 0) * PRIM + 0.
       SetCombineWords field map: rgbA=w0[20:4] rgbB=w1[28:4] rgbC=w0[15:5] rgbD=w1[15:3],
       alphaA=w0[12:3] alphaB=w1[12:3] alphaC=w0[9:3] alphaD=w1[9:3]. */
    const uint32_t combW0 = 0xFC000000u | (1u << 20) | (3u << 15) | (1u << 12) | (3u << 9);
    const uint32_t combW1 = (8u << 28) | (7u << 15) | (7u << 12) | (7u << 9);
    static W root[24];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(combW0, combW1);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xFA000000u, 0xFF0000FFu); /* SETPRIMCOLOR red */
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xFA000000u, 0x00FF00FFu); /* SETPRIMCOLOR green -- the split under test */
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;

    /* (a) ownfold=0: the prim change always splits. */
    gdx_test_ownfold = 0;
    gdx_test_census = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().rects == 2);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().flushDrain[gdxn::FLUSH_PRIM] == 1);
        /* the census sees it as NOT foldable: the mock binds every input as a stage constant
           and the combiner's input 1 IS prim. */
        CHECK(it.Stats().foldable[0] == 0);
    }
    /* (b) ownfold=1 but the shader still binds prim as a stage constant: no fold. */
    gdx_test_ownfold = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().folded[0] == 0);
    }
    /* (c) ownfold=1 and the backend reports prim riding the vertex colour attribute: fold,
       one draw, both rects in it. */
    {
        MockBackend mb;
        mb.constRgbMask = 0;
        mb.constAlphaMask = 0;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().rects == 2);
        CHECK(mb.batches.size() == 1);
        CHECK(it.Stats().folded[0] == 1);
        CHECK(it.Stats().flushDrain[gdxn::FLUSH_PRIM] == 0);
        if (mb.batches.size() == 1) {
            CHECK(mb.batches[0].numTris == 4); /* two rects, one draw */
        }
    }
    /* (d) the backend cannot report (no shader bound): never fold. */
    {
        MockBackend mb;
        mb.constInputsOk = false;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.batches.size() == 2);
        CHECK(it.Stats().folded[0] == 0);
    }
    gdx_test_ownfold = 0;
    gdx_test_census = 0;
}

/* Test 6 (GEOMLOSS): a batch that does not fit the pool must be SPLIT, not dropped.
 *
 * The owned interpreter has no legacy variable-stride path, so an all-or-nothing packed
 * begin turned pool pressure into missing triangles. Build one list with 300 TRI1s, run it
 * twice: once with a pool that comfortably holds them, once with a pool sized to exactly
 * the geometry (300 * 3 * 12 floats). The old begin needed room for a whole 256-tri batch
 * up front, so the second run dropped the last 44 triangles; the fixed begin reports the
 * cap and the batch splits there. The assertion is the strong one: the concatenated packed
 * floats must be BYTE-IDENTICAL between the two runs. */
static void BuildTriSpam(std::vector<uint32_t>& dl, int nTris) {
    dl.clear();
    dl.push_back(0xDA380003u); dl.push_back(0x06000000u); /* MTX modelview */
    dl.push_back(0xDA380007u); dl.push_back(0x06000000u); /* MTX projection */
    dl.push_back(0x01003006u); dl.push_back(0x06000100u); /* VTX n=3 v0=0 */
    dl.push_back(0xFC121824u); dl.push_back(0xFF33FFFFu); /* combine */
    dl.push_back(0xD9FFFFFFu); dl.push_back(0x00000004u); /* geometry mode set SHADE */
    for (int i = 0; i < nTris; i++) {
        dl.push_back(0x05000204u); dl.push_back(0x00000000u); /* TRI1 0,1,2 */
    }
    dl.push_back(0xDF000000u); dl.push_back(0u);
    for (auto& w : dl) w = gdxn::Bswap32(w);
}

static void RunTriSpam(std::vector<uint32_t>& dl, size_t poolFloats, std::vector<float>& outVbo,
                       size_t* outTris, uint32_t* outRefused, uint32_t* outSplit, size_t* outBatches) {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u; mtx[2] = 0x00000001u; mtx[5] = 0x00010000u; mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    alignas(8) static uint8_t seg6[512];
    memcpy(seg6, mtx, 64);
    memcpy(seg6 + 0x100, vtx, sizeof(vtx));
    gRanges.clear();
    Reg(dl.data(), dl.size() * 4, 8, true, false, true);
    Reg(seg6, sizeof(seg6), 8, false, false, false);
    gSegs[6] = reinterpret_cast<uintptr_t>(seg6);
    gRoot = reinterpret_cast<uintptr_t>(dl.data());
    gRootBytes = dl.size() * 4;
    MockBackend mb;
    mb.pool.assign(poolFloats, 0.0f);
    gdxn::Interp it(&mb);
    it.RunFrame(nullptr, Cfg(), 0);
    outVbo.clear();
    for (const MockBatch& b : mb.batches) {
        outVbo.insert(outVbo.end(), b.vbo.begin(), b.vbo.end());
    }
    *outTris = it.Stats().trisEmitted;
    *outRefused = it.Stats().packRefused;
    *outSplit = it.Stats().packSplit;
    *outBatches = mb.batches.size();
    gSegs[6] = 0;
}

static void TestPackSplitPreservesGeometry() {
    const int kTris = 300;
    const size_t kExactFloats = static_cast<size_t>(kTris) * 3u * 12u;
    std::vector<uint32_t> dl;
    BuildTriSpam(dl, kTris);

    std::vector<float> roomy;
    size_t roomyTris = 0, roomyBatches = 0;
    uint32_t roomyRef = 0, roomySplit = 0;
    RunTriSpam(dl, 65536, roomy, &roomyTris, &roomyRef, &roomySplit, &roomyBatches);
    CHECK(roomyTris == static_cast<size_t>(kTris));
    CHECK(roomyRef == 0 && roomySplit == 0);
    CHECK(roomyBatches == 2); /* 256 + 44 */
    CHECK(roomy.size() == kExactFloats);

    std::vector<float> tight;
    size_t tightTris = 0, tightBatches = 0;
    uint32_t tightRef = 0, tightSplit = 0;
    RunTriSpam(dl, kExactFloats, tight, &tightTris, &tightRef, &tightSplit, &tightBatches);
    /* The pool holds the geometry exactly, so nothing may be refused and nothing lost. */
    CHECK(tightRef == 0);
    CHECK(tightSplit == 1); /* the second begin is capped at the 44 tris that still fit */
    CHECK(tightTris == static_cast<size_t>(kTris));
    CHECK(tight.size() == kExactFloats);
    CHECK(tight.size() == roomy.size());
    if (tight.size() == roomy.size()) {
        CHECK(memcmp(tight.data(), roomy.data(), tight.size() * sizeof(float)) == 0);
    }

    /* Genuine exhaustion still refuses — and is still counted, loudly, on the receipt. */
    std::vector<float> starved;
    size_t starvedTris = 0, starvedBatches = 0;
    uint32_t starvedRef = 0, starvedSplit = 0;
    RunTriSpam(dl, 36, starved, &starvedTris, &starvedRef, &starvedSplit, &starvedBatches);
    CHECK(starvedTris == 1);   /* exactly the one triangle the pool can hold */
    CHECK(starvedRef > 0);     /* and the rest is refused, not silently forgotten */
}

/* ---- CI8 address recycling (audit 2026-09-23 finding 26) ---------------------------------
 * A CI8 texture and its TLUT are rewritten IN PLACE at the same addresses -- what the per-race
 * arena rewind does to `sCourseMinimapTex`, and what the GfxPool does to the night-course
 * TLUTs between two courses. The decoded result must follow the bytes, not the address.
 * The two halves of the fix are asserted separately because they answer different producers:
 * the content hash (`[native] texhash_ci`) is self-healing, and TexCacheClear /
 * TexCacheDeleteAddress are the queued gdx_rdram_mode_reset / minimap.c requests replayed onto
 * the owned cache (`[native] texinval`). */
static int BuildCi8List(W* root, const void* tlut, const void* indices) {
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u); /* othermode_h: 1cyc */
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu); /* combine: TEXEL0 only */
    /* TLUT: 256 RGBA16 entries to tmem word 256, through load tile 7. */
    root[n++] = Wp(0xFD1000FFu, tlut);        /* SETTIMG RGBA16 width 256 */
    root[n++] = Wv(0xF5100100u, 0x07000000u); /* SETTILE RGBA16 tmem=256 -> tile 7 */
    root[n++] = Wv(0xF0000000u, 0x073FC000u); /* LOADTLUT tile 7, highIndex 255 */
    /* CI8 8x8 index image at tmem word 0, through load tile 7. */
    root[n++] = Wp(0xFD480007u, indices);     /* SETTIMG CI 8b width 8 */
    root[n++] = Wv(0xF5480000u, 0x07000000u); /* SETTILE CI 8b line 0 tmem 0 -> tile 7 */
    root[n++] = Wv(0xF3000000u, 0x0703F800u); /* LOADBLOCK tile 7 lrs=63 dxt=0x800 */
    root[n++] = Wv(0xF5480200u, 0x00000000u); /* SETTILE CI 8b line 1 tmem 0 -> tile 0 */
    root[n++] = Wv(0xF2000000u, 0x0001C01Cu); /* SETTILESIZE tile 0 -> 8x8 */
    root[n++] = Wv(0xE401C01Cu, 0x00000000u); /* TEXRECT */
    root[n++] = Wv(0xE1000000u, 0x00000000u); /* RDPHALF_1: s,t = 0 */
    root[n++] = Wv(0xF1000000u, 0x04000400u); /* RDPHALF_2: dsdx=dtdy=1.0 */
    root[n++] = Wv(0xDF000000u, 0);
    return n;
}

static void TestCiRecycle() {
    static uint8_t tlut[512];
    static uint8_t indices[64];
    static W root[24];
    const uint16_t kRed = static_cast<uint16_t>((31u << 11) | 1u);
    const uint16_t kBlue = static_cast<uint16_t>((31u << 1) | 1u);
    const uint16_t kGreen = static_cast<uint16_t>((31u << 6) | 1u);
    const auto put = [](uint8_t* p, uint16_t c) {
        p[0] = static_cast<uint8_t>(c >> 8);
        p[1] = static_cast<uint8_t>(c & 0xFF);
    };
    const int n = BuildCi8List(root, tlut, indices);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(tlut, sizeof(tlut), 8, false, false, false);
    Reg(indices, sizeof(indices), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;

    /* course 1: every texel index 0, palette entry 0 = red, entry 1 = blue */
    memset(tlut, 0, sizeof(tlut));
    put(tlut + 0, kRed);
    put(tlut + 2, kBlue);
    memset(indices, 0, sizeof(indices));

    /* --- A: content hash ON (the shipping default) --- */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 1 && mb.uploads.size() == 1);
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[2] == 0); /* red */
        }
        /* same address, different INDEX bytes -> must re-decode */
        memset(indices, 1, sizeof(indices));
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 2 && mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 0 && mb.uploads[1].rgba[2] == 255); /* blue */
        }
        /* same address, same indices, different PALETTE bytes -> must re-decode too. This is
           the night-course TLUT case: the palette is half the decoded pixel, and the key
           carries only palette_dram_addr, which the pool re-hands to the next course. */
        put(tlut + 2, kGreen);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 3 && mb.uploads.size() == 3);
        if (mb.uploads.size() >= 3) {
            CHECK(mb.uploads[2].rgba[1] == 255 && mb.uploads[2].rgba[2] == 0); /* green */
        }
    }

    /* --- B: content hash OFF ([native] texhash_ci=0) -- identity keying, exactly what the
           owned path did before the fix, with TexCacheClear as the only way back --- */
    put(tlut + 2, kBlue);
    memset(indices, 0, sizeof(indices));
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetCiContentHash(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texMisses == 1 && mb.uploads.size() == 1);
        memset(indices, 1, sizeof(indices));
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texHits == 1); /* the bug: a recycled address is a cache HIT ... */
        CHECK(mb.uploads.size() == 1);  /* ... and the previous occupant's decode is served */
        it.TexCacheClear();             /* what gdx_rdram_mode_reset asks for */
        CHECK(it.Stats().texClears == 1);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 0 && mb.uploads[1].rgba[2] == 255); /* blue */
        }
    }

    /* --- C: TexCacheDeleteAddress is exact-address, like LUS TextureCacheDelete (which is why
           minimap.c invalidates each CI8 half separately) --- */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetCiContentHash(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 1);
        memset(indices, 0, sizeof(indices));
        it.TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(indices) + 1); /* not the key */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texInvalidated == 0);
        CHECK(mb.uploads.size() == 1); /* no match -> still the stale decode */
        it.TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(indices));
        CHECK(it.Stats().texInvalidated == 1);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(mb.uploads.size() == 2);
        if (mb.uploads.size() >= 2) {
            CHECK(mb.uploads[1].rgba[0] == 255 && mb.uploads[1].rgba[2] == 0); /* red again */
        }
    }
}


/* ---- DROPSITES 1: the texture-cache decode ceiling ---------------------------------------
 * Pre-fix, a decode that did not fit the fixed 256 KiB scratch returned without uploading and
 * ImportTexture marked the entry `uploaded` anyway. The backend texture id had just been handed
 * out of the free list, so it still held the previous texture's pixels -- and the entry being
 * marked uploaded made every later lookup of that key a cache HIT over those pixels.
 * `texDecodeOverflow` counted it and was printed by nothing.
 *
 * [debug] texscratchkib / texscratchmaxkib straddle the 8x8 CI8 decode (256 B) so both arms are
 * reachable without a megabyte display list, exactly as geomloss's vbopoolkib does for the VBO
 * pool. The pre-fix behaviour is [native] texstalefix=0, asserted here as the control. */
static void TestDecodeCeiling() {
    static uint8_t tlut[512];
    static uint8_t indices[64];
    static W root[24];
    const uint16_t kRed = static_cast<uint16_t>((31u << 11) | 1u);
    const int n = BuildCi8List(root, tlut, indices);
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(tlut, sizeof(tlut), 8, false, false, false);
    Reg(indices, sizeof(indices), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = static_cast<size_t>(n) * 16;
    memset(tlut, 0, sizeof(tlut));
    tlut[0] = static_cast<uint8_t>(kRed >> 8);
    tlut[1] = static_cast<uint8_t>(kRed & 0xFF);
    memset(indices, 0, sizeof(indices));

    /* --- A: the scratch grows, and a texture that used to be SKIPPED now decodes correctly. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 0); /* 128 B scratch, default 4 MiB ceiling */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeGrow == 1);
        CHECK(it.Stats().texDecodeOverflow == 0);
        CHECK(it.Stats().texNoUpload == 0);
        CHECK(mb.uploads.size() == 1);
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].w == 8 && mb.uploads[0].h == 8);
            CHECK(mb.uploads[0].rgba[0] == 255 && mb.uploads[0].rgba[2] == 0); /* red, decoded */
        }
        CHECK(it.TexDecodeHwBytes() == 8u * 8u * 4u); /* the receipt's margin number */
    }

    /* --- B: a genuine ceiling (no upload could ever succeed) -> neutral fallback, and the
           entry is NOT cached as uploaded, so the next frame retries instead of serving it. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 128); /* ceiling below the 256 B decode */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeOverflow == 1);
        CHECK(it.Stats().texNoUpload == 1);
        CHECK(it.Stats().texFallback == 1);
        CHECK(mb.uploads.size() == 1); /* pre-fix this was 0: the id kept its old pixels */
        if (!mb.uploads.empty()) {
            CHECK(mb.uploads[0].w == 8 && mb.uploads[0].h == 8);
            bool allWhite = true;
            for (size_t k = 0; k < mb.uploads[0].rgba.size(); k++) {
                if (mb.uploads[0].rgba[k] != 255) allWhite = false;
            }
            CHECK(allWhite); /* untextured, i.e. defined -- not an arbitrary previous texture */
        }
        it.RunFrame(nullptr, Cfg(), 0);
        /* TexCacheLookup matches the key but sees `uploaded == false`, so it re-arms the import
           instead of serving: not a hit, and not a second miss either -- the entry is still the
           same entry, it just has no content to give. The retry is what [ndrops] texnoup= / texfb=
           count every frame it happens. */
        CHECK(it.Stats().texHits == 0);
        CHECK(it.Stats().texMisses == 1);
        CHECK(it.Stats().texNoUpload == 2);
        CHECK(mb.uploads.size() == 2);
    }

    /* --- C: the pre-fix arm, from the same binary. This is the bug, asserted. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetTexScratchBytes(128, 128);
        it.SetTexStaleFix(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texDecodeOverflow == 1);
        CHECK(mb.uploads.empty()); /* the backend was told NOTHING about this id */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().texHits == 1); /* and the empty id is now a cache HIT, for good */
        CHECK(mb.uploads.empty());
    }
}

/* ---- DROPSITES 2: G_VTX past the 64-slot window -----------------------------------------
 * Pre-fix: mStats.vtx was incremented by the full n_vertices and then the WHOLE load was
 * dropped, so the receipt over-reported exactly the vertices that were never transformed and
 * nothing counted the drop. [debug] vtxmax shrinks the window so both arms are reachable. */
static void TestVtxWindow() {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    const int16_t ob[3][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;   /* VTX n=3 dest=0 */
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;
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

    /* In range: nothing is counted and the high-water is the real end of the load. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtx == 3 && it.Stats().vtxOver == 0);
        CHECK(it.VtxHwEnd() == 3);
    }
    /* Over the window: the prefix loads, the tail is refused, and vtx= counts only what was
       actually transformed. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetVtxSoftCap(2);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtxOver == 1);
        CHECK(it.Stats().vtxOverDropped == 1);
        CHECK(it.Stats().vtx == 2); /* pre-fix: 3, for a load that transformed none of them */
        CHECK(it.VtxHwEnd() == 3);
    }
    /* The pre-fix arm ([native] vtxclamp=0): the whole load is dropped. */
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.SetVtxSoftCap(2);
        it.SetVtxClamp(false);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().vtxOver == 1);
        CHECK(it.Stats().vtxOverDropped == 3);
        CHECK(it.Stats().vtx == 0);
    }
}

/* ---- DROPSITES 3: the clip polygon can never reach its cap -------------------------------
 * Sutherland-Hodgman against a half-space adds at most one vertex to a CONVEX polygon (two
 * crossings cost at least one vertex, and each step's result is convex again), so from a
 * triangle over 7 planes the output is bounded by 3 + 7 = 10 against a 16-slot buffer. The cap
 * is kept (it is the LUS clipper's, expression for expression) and counted instead of raised.
 * This pins the bound on a triangle that crosses every plane it can. */
static void TestClipPolyBound() {
    static uint32_t mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0] = 0x00010000u;
    mtx[2] = 0x00000001u;
    mtx[5] = 0x00010000u;
    mtx[7] = 0x00000001u;
    for (int i = 0; i < 16; i++) mtx[i] = gdxn::Bswap32(mtx[i]);
    static gdxn::Vtx vtx[3];
    memset(vtx, 0, sizeof(vtx));
    /* A huge triangle around the origin: every clip plane cuts it. */
    const int16_t ob[3][3] = { { -900, -700, 0 }, { 900, -700, 0 }, { 0, 900, 0 } };
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            vtx[v].ob[k] = static_cast<int16_t>(gdxn::Bswap16(static_cast<uint16_t>(ob[v][k])));
        }
        vtx[v].cn[0] = 255; vtx[v].cn[1] = 128; vtx[v].cn[2] = 0; vtx[v].cn[3] = 255;
    }
    static uint32_t dl[14];
    int n = 0;
    dl[n++] = 0xDA380003u; dl[n++] = 0x06000000u;
    dl[n++] = 0xDA380007u; dl[n++] = 0x06000000u;
    dl[n++] = 0x01003006u; dl[n++] = 0x06000100u;
    dl[n++] = 0xFC121824u; dl[n++] = 0xFF33FFFFu;
    dl[n++] = 0xD9FFFFFFu; dl[n++] = 0x00000004u;
    dl[n++] = 0x05000204u; dl[n++] = 0x00000000u;
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
    CHECK(it.Stats().clipTrunc == 0);
    CHECK(it.ClipPolyHw() >= 4);  /* the clipper really ran and really added vertices */
    CHECK(it.ClipPolyHw() <= 10); /* 3 + 7, the structural bound */
    printf("  [clip] polygon high-water %u of the 16-slot buffer\n", it.ClipPolyHw());
}

/* [tmemblock] the widened same-content skip ([native] tmemskip), built on TestRectTexture's
 * scene because that test already documents the shipping behaviour it changes: "second
 * identical frame: span same-content skip is gated off (raw source) -> a load, a cache HIT".
 *
 * Three things are asserted, and the third is the one that matters:
 *   1. tmemskip=0 is byte-for-byte the shipping path: 2 loads, 0 skips.
 *   2. tmemskip=1 skips the second frame's mirror -- 2 loads, 1 skip.
 *   3. the skipped frame draws the SAME BYTES as the mirrored one. A skip that changed a
 *      pixel would show up here as a vbo/upload difference, and a skip that lost the texture
 *      identity would show up as a second upload.
 * Then the generation is moved under a NATIVE-class source and the skip must stop firing:
 * a gate that never says "stale" is not a gate. */
static void TestTmemSkipWidened() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    const size_t rootBytes = static_cast<size_t>(n) * 16;

    struct Leg {
        int level;
        unsigned cls;
        uint32_t wantLoads, wantSkips;
        const char* what;
    };
    const Leg legs[] = {
        { 0, GDX_TMEMSRC_STABLE, 2, 0, "tmemskip=0 (shipping: immutable only)" },
        { 1, GDX_TMEMSRC_STABLE, 2, 1, "tmemskip=1, registered ROM-backed source" },
        { 1, GDX_TMEMSRC_NATIVE, 2, 1, "tmemskip=1, native-RGBA16 source" },
        { 1, GDX_TMEMSRC_DMA, 2, 0, "tmemskip=1 does NOT admit the DMA class" },
        { 2, GDX_TMEMSRC_DMA, 2, 1, "tmemskip=2 admits the DMA class" },
        { 1, GDX_TMEMSRC_UNKNOWN, 2, 0, "an UNKNOWN source is never skipped, at any level" },
    };
    std::vector<float> reference;
    for (const Leg& leg : legs) {
        gRanges.clear();
        Reg(root, sizeof(root), 16, false, false, true);
        Reg(texels, sizeof(texels), 8, false, false, false);
        gRoot = reinterpret_cast<uintptr_t>(root);
        gRootBytes = rootBytes;
        gdx_test_tmemskip = leg.level;
        gdx_test_src_cls = leg.cls;
        gdx_test_native_gen = 1;
        gdx_test_dma_gen = 1;
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        it.RunFrame(nullptr, Cfg(), 0);
        const gdxn::Counters& s = it.Stats();
        if (s.tmemLoads != leg.wantLoads || s.tmemSkips != leg.wantSkips) {
            printf("  [tmemskip] %s: loads=%u skips=%u (want %u/%u)\n", leg.what, s.tmemLoads, s.tmemSkips,
                   leg.wantLoads, leg.wantSkips);
        }
        CHECK(s.tmemLoads == leg.wantLoads);
        CHECK(s.tmemSkips == leg.wantSkips);
        /* One upload for two frames in every leg: the second frame must reach the SAME texture
           cache entry whether the mirror was rewritten or skipped. */
        CHECK(mb.uploads.size() == 1);
        CHECK(mb.batches.size() == 2);
        if (mb.batches.size() == 2) {
            CHECK(memcmp(mb.batches[0].vbo.data(), mb.batches[1].vbo.data(), mb.batches[0].vbo.size() * 4) == 0);
        }
        /* And the same bytes as every other leg: a skip must not change what is drawn. */
        if (reference.empty()) {
            reference = mb.batches[0].vbo;
        } else {
            CHECK(reference.size() == mb.batches[0].vbo.size());
            CHECK(memcmp(reference.data(), mb.batches[0].vbo.data(), reference.size() * 4) == 0);
        }
    }

    /* The gate must be able to say STALE. Move the generation the NATIVE class keys on between
       the two frames and the skip has to stop firing -- otherwise the test above proves only
       that a constant returns a constant. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 1;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    gdx_test_native_gen = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        gdx_test_native_gen = 2; /* a range registration / clear landed between the frames */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemLoads == 2);
        CHECK(it.Stats().tmemSkips == 0);
        /* Third frame, generation held still again: the skip comes back. */
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemLoads == 3);
        CHECK(it.Stats().tmemSkips == 1);
    }

    /* A CLASS change alone invalidates, even with both generations standing still: a stamp
       taken under one class says nothing about another. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 2;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    gdx_test_native_gen = 1;
    gdx_test_dma_gen = 1;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        gdx_test_src_cls = GDX_TMEMSRC_DMA;
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemSkips == 0);
    }
    gdx_test_tmemskip = 0;
    gdx_test_src_cls = GDX_TMEMSRC_STABLE;
}

/* [memotear] NEXT.md B54 -- the INJECTOR itself, tested before it is trusted in the guest.
 *
 * The two arms above prove the gate CAN say stale when the test drives the mock generations by
 * hand. B54 is the different claim that the shipping fault injector ([debug] tmemfault) drives
 * them, on real content, from inside the walk -- because the corpus never once reached the
 * refusal arm (412,326 skips, stale=0), so the only thing that has ever exercised it is a hand
 * -written frame. A fault injector that silently injected nothing would make the guest run say
 * `stale=0` too, and look exactly like a clean gate. So: every injection is caught, none is
 * missed, and the pixels do not move. */
static void TestTmemFaultInjection() {
    static uint8_t texels[16 * 16 * 2];
    for (int i = 0; i < 256; i++) {
        const uint16_t c = static_cast<uint16_t>((31 << 11) | 1);
        texels[2 * i] = c >> 8;
        texels[2 * i + 1] = c & 0xFF;
    }
    static W root[16];
    int n = 0;
    root[n++] = Wv(0xE3001001u, 0x00000000u);
    root[n++] = Wv(0xFC121824u, 0xFF33FFFFu);
    root[n++] = Wp(0xFD10000Fu, texels);
    root[n++] = Wv(0xF5100000u, 0x07000000u);
    root[n++] = Wv(0xF3000000u, 0x070FF800u);
    root[n++] = Wv(0xF5100800u, 0x00000000u);
    root[n++] = Wv(0xF2000000u, 0x0003C03Cu);
    root[n++] = Wv(0xE4064064u, 0x00000000u);
    root[n++] = Wv(0xE1000000u, 0x00000000u);
    root[n++] = Wv(0xF1000000u, 0x04000400u);
    root[n++] = Wv(0xDF000000u, 0);
    const size_t rootBytes = static_cast<size_t>(n) * 16;

    /* Both admitted classes, because they fail differently: NATIVE has a generation arm and
       STABLE has none at all (gdx_gfx_tmem_source_unchanged answers it with an unconditional
       1), so STABLE's only refusal is the class arm and the injector must fall back to it. */
    const unsigned classes[] = { GDX_TMEMSRC_NATIVE, GDX_TMEMSRC_STABLE };
    std::vector<float> reference;
    for (unsigned cls : classes) {
        /* Leg A: the lever on, the injector OFF -- the skip fires and nothing is stale. */
        uint32_t cleanSkips = 0;
        for (int inject = 0; inject <= 1; inject++) {
            gRanges.clear();
            Reg(root, sizeof(root), 16, false, false, true);
            Reg(texels, sizeof(texels), 8, false, false, false);
            gRoot = reinterpret_cast<uintptr_t>(root);
            gRootBytes = rootBytes;
            gdx_test_tmemskip = 1;
            gdx_test_src_cls = cls;
            gdx_test_native_gen = 1;
            gdx_test_dma_gen = 1;
            gdx_test_tmemfault = inject ? 1 : 0; /* 1 = every admitted skip */
            MockBackend mb;
            gdxn::Interp it(&mb);
            for (int f = 0; f < 4; f++) {
                it.RunFrame(nullptr, Cfg(), 0);
            }
            const gdxn::Counters& st = it.Stats();
            if (inject == 0) {
                cleanSkips = st.tmemSkips;
                CHECK(cleanSkips > 0);          /* the lever really does skip on this class */
                CHECK(st.tmemStale == 0);       /* ...and nothing was stale */
                CHECK(st.tmemFaultGen == 0 && st.tmemFaultCls == 0);
            } else {
                const uint32_t injections = st.tmemFaultGen + st.tmemFaultCls;
                if (injections == 0 || st.tmemFaultMissed != 0 || st.tmemFaultCaught != injections) {
                    printf("  [tmemfault] cls=%u inj=%u/%u caught=%u missed=%u stale=%u skips=%u\n", cls,
                           st.tmemFaultGen, st.tmemFaultCls, st.tmemFaultCaught, st.tmemFaultMissed,
                           st.tmemStale, st.tmemSkips);
                }
                /* It injected something... */
                CHECK(injections > 0);
                /* ...the gate REFUSED every one of them... */
                CHECK(st.tmemFaultCaught == injections);
                CHECK(st.tmemFaultMissed == 0);
                CHECK(st.tmemStale >= injections);
                /* ...and the refusals really removed skips, so the injector is not a counter
                   that increments beside a gate that was going to refuse anyway. */
                CHECK(st.tmemSkips < cleanSkips);
                /* NATIVE has a generation arm and must use it; STABLE has none and must fall
                   back to the class arm rather than report a fault it did not cause. */
                if (cls == GDX_TMEMSRC_NATIVE) {
                    CHECK(st.tmemFaultGen > 0);
                } else {
                    CHECK(st.tmemFaultGen == 0 && st.tmemFaultCls > 0);
                }
            }
            /* THE FRAME STILL DRAWS CORRECTLY -- the whole second half of B54. A refusal
               re-mirrors, which is what tmemskip=0 does every load, so every leg of every class
               must emit the same vertex bytes. */
            CHECK(!mb.batches.empty());
            if (reference.empty()) {
                reference = mb.batches[0].vbo;
            } else if (!mb.batches.empty()) {
                CHECK(reference.size() == mb.batches[0].vbo.size());
                CHECK(memcmp(reference.data(), mb.batches[0].vbo.data(), reference.size() * 4) == 0);
            }
            for (size_t b = 1; b < mb.batches.size(); b++) {
                CHECK(mb.batches[b].vbo.size() == mb.batches[0].vbo.size());
                CHECK(memcmp(mb.batches[b].vbo.data(), mb.batches[0].vbo.data(),
                             mb.batches[0].vbo.size() * 4) == 0);
            }
        }
    }

    /* The injector is inert with the lever off: it is only ever reached from an admitted skip,
       and tmemskip=0 admits none. This is what keeps it out of every shipped configuration. */
    gRanges.clear();
    Reg(root, sizeof(root), 16, false, false, true);
    Reg(texels, sizeof(texels), 8, false, false, false);
    gRoot = reinterpret_cast<uintptr_t>(root);
    gRootBytes = rootBytes;
    gdx_test_tmemskip = 0;
    gdx_test_tmemfault = 1;
    gdx_test_src_cls = GDX_TMEMSRC_NATIVE;
    {
        MockBackend mb;
        gdxn::Interp it(&mb);
        it.RunFrame(nullptr, Cfg(), 0);
        it.RunFrame(nullptr, Cfg(), 0);
        CHECK(it.Stats().tmemFaultGen == 0);
        CHECK(it.Stats().tmemFaultCls == 0);
        CHECK(it.Stats().tmemStale == 0);
    }
    gdx_test_tmemfault = 0;
    gdx_test_tmemskip = 0;
    gdx_test_src_cls = GDX_TMEMSRC_STABLE;
}

int main() {
    TestWalk();
    TestCC();
    TestRectTexture();
    TestTmemSkipWidened();
    TestTmemFaultInjection();
    TestTri3D();
    TestCiRecycle();
    TestOwnFold();
    TestPackSplitPreservesGeometry();
    TestDecodeCeiling();
    TestVtxWindow();
    TestClipPolyBound();
    if (gFails == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURES ===\n", gFails);
    return 1;
}
