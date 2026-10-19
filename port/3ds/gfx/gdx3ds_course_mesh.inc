/* port/3ds/gfx/gdx3ds_course_mesh.inc -- nicourse `[native] ni_course` (nointerp S5): the 3D course
 * preview's models as GPU-resident meshes. Included by gdx3ds_mesh.cpp (its gpuxf helpers: XfClassify,
 * XfErrorBoundMpx, XfFillMesh / XfFillRun, PreparePackCtx, BuildAux, the resident buffers and the
 * deferred retire). docs/research/native/nicourse-progress.md.
 *
 * WHAT. The course select, the GP next-course card and the records course select draw up to six course
 * models per frame (course_model.c func_i5_80115E64): each a G_VIEWPORT and Course_DrawModel's G_VTX /
 * G_TRI2 run -- a triangulated tube, 30-vertex loads advancing by 27, six triangles per ring step --
 * over a vertex buffer func_800A2D2C wrote once when the cup was chosen. Per frame only the camera
 * (orbit yaw +1 degree), the viewports (the carousel) and the prim colour change. Walked, that is
 * 2-3.5k TRI2 and 40-70 G_VTX (1-2k CPU T&Ls) a frame, the heaviest interpreted frames of the game
 * (nicensus-progress.md §6 item 4).
 *
 * KEY. The game's marker pair carries (vertex buffer, vertex count, content generation); the generation
 * is bumped by every func_800A2D2C call, so the key changes whenever any model buffer is rewritten
 * (cup change, X cup, a reload). The baked entry keeps a copy of the run's words (w0, w1, w1hi per
 * command) and every frame checks the live run against it word for word: the triangles and loads the
 * walker would execute are exactly the ones baked. The vertex bytes are copied once into the resident
 * buffer; [native] ni_course=2 re-reads them every frame and counts any difference from the copy
 * (the generation key's claim), drawing nothing itself.
 *
 * DRAW. One indexed GPU-transform draw per model from the resident buffer (raw Vtx + u16 indices in
 * stock triangle order), under the machines' gpuxf gates on the walker's latched MP matrix: the
 * model's object box wholly inside the clip volume (XF_INSIDE: the CPU clipper would not cut a
 * triangle) and the float24 error bound under gpuxf_maxerr. Then:
 *  - box wholly outside one clip plane (XF_OUTSIDE): every triangle is rejected by the CPU path before
 *    any state bind -- the run is consumed and nothing is drawn, exactly as stock;
 *  - viewport outside the scissor / window (the carousel's off-screen models): the CPU path binds the
 *    state and submits triangles the clipper removes whole -- consumed, nothing drawn (the next draw
 *    binds its own state; no pixel differs);
 *  - otherwise the state binds through the walker's ResolveDrawState (BindMaterial: shader, viewport,
 *    scissor, combiner inputs -- the PRIMITIVE colour), the batch open before it is flushed first, and
 *    the draw goes out like a finish-banner segment (raw Vtx, shader transform, the backend's state
 *    elision; its stereo class row from the first vertex's w, SCENE).
 * Refused (nothing touched, the walker walks the run): a straddling box, fog (the GPU's fog factor may
 * differ by one RGB5 quantum), lighting the shader cannot express, a bind the gpuxf path cannot take.
 *
 * HOT: NrrCourseHot* (tools/native/nrr_hot.list registers the shared gpuxf helpers under `course`). */
Gdx3dsCourseWin gdx3ds_course_win = {};
int gdx3ds_course_frame_models = 0;

namespace {

constexpr int kCmEntries = 12;
constexpr unsigned kCmMaxCmds = 4096;
constexpr unsigned kCmMaxVerts = 4096;
constexpr unsigned kCmMaxChunks = 250; /* Mesh::vtxChunk is a u8 */
constexpr unsigned kCmMaxTris = 8192;
constexpr uint32_t kCmNone = 0xFFFFFFFFu;
constexpr uint8_t kCmOpVtx = 0x01u;
constexpr uint8_t kCmOpTri1 = 0x05u;
constexpr uint8_t kCmOpTri2 = 0x06u;

struct CmEntry {
    uint32_t vtxW1, vtxCount, gen, nCmds, stride;
    uint32_t* words; /* nCmds * 3: w0, w1, w1hi as baked */
    Mesh* mesh;
    uint8_t* res;
    uint32_t resBytes;
    XfBox box;
    uint32_t tail[2]; /* run-relative: the second-to-last and the last G_VTX */
    uint32_t lastUse;
    uint8_t used;
    uint8_t failed;
    Gdx3dsCourseModel pub; /* what the walker's hot path draws from */
};
CmEntry sCm[kCmEntries];
int sCmLevel = -1; /* [native] ni_course: 0 off (default), 1 on, 2 verify */
uint32_t sCmUse = 0;

#define sCmWin gdx3ds_course_win
#define sCmFrameModels gdx3ds_course_frame_models

GDX_COLD void CmLatch() {
    Enabled(); /* latches gpuxf with the rest of the mesh keys */
    if (sEnabled < 0 || &gdx3ds_config_get_int == nullptr) {
        return; /* not ready yet: retried at the next marker */
    }
    int level = gdx3ds_config_get_int("native", "ni_course", 0);
    if (level < 0 || level > 2) {
        level = 0;
    }
    /* gpuxf is IMPLIED, not required as a separate ini key: nicourse's draw needs exactly the
     * backend readiness `gpuxf=1` itself gates (gdx3ds_mesh_backend_xf_ready(), forced off under
     * the machines-only sVerify/sWalkOnly debug levers) -- the same codepath sGpuXf=1 reaches, so
     * reading that readiness directly instead of the generic `[native] gpuxf` flag is exact-safe:
     * no other sGpuXf consumer (sRetained, sMatTab, finish_mesh) is touched, they keep their own
     * explicit levers. A user who sets only `ni_course=N` no longer also has to discover and set an
     * unrelated-sounding `gpuxf` key (`interp` is NOT implied the same way: it changes WHICH
     * interpreter runs for the WHOLE game, always explicit by design -- gdx3ds_native_cfg.c). */
    const int backendOk = !sVerify && !sWalkOnly && gdx3ds_mesh_backend_xf_ready();
    const int on = level != 0 && backendOk;
    if (level != 0 && !backendOk) {
        Log("[nicourse] ni_course=%d IGNORED: the gpuxf backend is not ready (shader missing, or "
            "machines_verify=%d/machines_walk_only=%d forced it off) -- falling back to the walked "
            "list",
            level, sVerify, sWalkOnly);
    }
    /* ni_course_clip removed (W6, Fable review): no documented use of the off path, never set by
     * any gate in this tree -- the PICA clip on a straddling model is now unconditional. */
    Log("[nicourse] init render ni_course=%d gpuxf=%d backend_ok=%d machines=%d maxerr=%dmpx -> %s", level, sGpuXf,
        backendOk, sEnabled, sGpuXfMaxErr,
        !on ? "off (the preview is walked)"
            : (level == 2 ? "verify (walked; baked copies checked against the live words and vertices)"
                          : "the course models draw from GPU-resident buffers"));
    sCmLevel = on ? level : 0;
}

inline int CmLevel() {
    if (GDX_LIKELY(sCmLevel >= 0)) {
        return sCmLevel;
    }
    CmLatch();
    return sCmLevel < 0 ? 0 : sCmLevel;
}

inline void CmReadWords(const uint8_t* p, uint32_t stride, uint32_t* w0, uint32_t* w1, uint32_t* w1hi) {
    memcpy(w0, p, 4);
    if (stride == 16) {
        memcpy(w1, p + 8, 4);
        memcpy(w1hi, p + 12, 4);
    } else {
        memcpy(w1, p + 4, 4);
        *w1hi = 0;
    }
}

CmEntry* NrrCourseHotFind(const Gdx3dsCourseRun& run) {
    for (int i = 0; i < kCmEntries; i++) {
        CmEntry& e = sCm[i];
        if (e.used && e.vtxW1 == run.vtxW1 && e.vtxCount == run.vtxCount && e.gen == run.gen &&
            e.nCmds == run.nCmds && e.stride == run.stride) {
            return &e;
        }
    }
    return nullptr;
}

/* The live run's words equal the baked copy (w1hi included: the host-pointer tag of every G_VTX). */
bool NrrCourseHotWords(const CmEntry& e, const Gdx3dsCourseRun& run) {
    const uint32_t* a = e.words;
    const uint8_t* p = run.words;
    uint32_t diff = 0;
    if (run.stride == 16) {
        for (uint32_t i = 0; i < run.nCmds; i++, p += 16, a += 3) {
            uint32_t w[4];
            memcpy(w, p, 16);
            diff |= (w[0] ^ a[0]) | (w[2] ^ a[1]) | (w[3] ^ a[2]);
        }
    } else {
        for (uint32_t i = 0; i < run.nCmds; i++, p += 8, a += 3) {
            uint32_t w[2];
            memcpy(w, p, 8);
            diff |= (w[0] ^ a[0]) | (w[1] ^ a[1]) | a[2];
        }
    }
    return diff == 0;
}

GDX_COLD void CmRelease(CmEntry& e) {
    if (e.mesh != nullptr) {
        RetireMesh(e.mesh);
        e.mesh = nullptr;
    }
    if (e.res != nullptr) {
        ResRetire(e.res, e.resBytes); /* accounts sResBytes / sResCount */
        e.res = nullptr;
        e.resBytes = 0;
    }
    free(e.words);
    e.words = nullptr;
    e.used = 0;
    e.failed = 0;
}

/* The bake's working set (drawing thread only). */
struct CmBuild {
    Gdx3dsMeshChunk chunks[kCmMaxChunks];
    uint16_t tris[kCmMaxTris * 3];
    unsigned nChunks, nTris, nVerts;
    uint8_t slotChunk[GDX3DS_MESH_SLOTS];
    uint8_t slotLocal[GDX3DS_MESH_SLOTS];
};
CmBuild* sCmB = nullptr;

GDX_COLD bool CmFail(CmEntry& e, const char* why, unsigned cmd, unsigned op) {
    e.failed = 1;
    sCmWin.fails++;
    Log("[nicourse] not a course model run vtx=%08lx n=%lu gen=%lu: %s at cmd %u op %02X (walked)",
        (unsigned long)e.vtxW1, (unsigned long)e.vtxCount, (unsigned long)e.gen, why, cmd, op);
    return false;
}

GDX_COLD bool CmAddTri(CmEntry& e, unsigned a, unsigned b, unsigned c, unsigned cmd, unsigned op) {
    const unsigned v[3] = { a, b, c };
    if (sCmB->nTris >= kCmMaxTris) {
        return CmFail(e, "triangle capacity", cmd, op);
    }
    for (int k = 0; k < 3; k++) {
        if (v[k] >= GDX3DS_MESH_SLOTS || sCmB->slotChunk[v[k]] == 0xFF) {
            return CmFail(e, "unloaded vertex slot", cmd, op);
        }
        const Gdx3dsMeshChunk& ch = sCmB->chunks[sCmB->slotChunk[v[k]]];
        sCmB->tris[sCmB->nTris * 3 + k] = (uint16_t)(ch.baseVertex + sCmB->slotLocal[v[k]]);
    }
    sCmB->nTris++;
    return true;
}

GDX_COLD bool CmAddVtx(CmEntry& e, uint32_t w0, uint32_t w1, uint32_t w1hi, uint16_t texS, uint16_t texT, unsigned cmd,
                       Gdx3dsMeshResolveFn resolve, void* user) {
    const unsigned n = (w0 >> 12) & 0xFFu;
    const unsigned end = (w0 >> 1) & 0x7Fu;
    if (n == 0 || end < n || end > GDX3DS_MESH_SLOTS) {
        return CmFail(e, "G_VTX slot range", cmd, kCmOpVtx);
    }
    const unsigned v0 = end - n;
    if (sCmB->nChunks >= kCmMaxChunks || sCmB->nVerts + n > kCmMaxVerts) {
        return CmFail(e, "vertex capacity", cmd, kCmOpVtx);
    }
    const uint64_t w1word = ((uint64_t)w1hi << 32) | w1;
    const void* src = resolve(w1word, (size_t)n * 16u, user);
    if (src == nullptr) {
        return CmFail(e, "vertex pointer refused", cmd, kCmOpVtx);
    }
    Gdx3dsMeshChunk& ch = sCmB->chunks[sCmB->nChunks];
    memset(&ch, 0, sizeof(ch));
    ch.srcW1 = w1word;
    ch.src = src;
    ch.count = (uint16_t)n;
    ch.baseVertex = (uint16_t)sCmB->nVerts;
    ch.texS = texS;
    ch.texT = texT;
    for (unsigned k = 0; k < n; k++) {
        sCmB->slotChunk[v0 + k] = (uint8_t)sCmB->nChunks;
        sCmB->slotLocal[v0 + k] = (uint8_t)k;
    }
    sCmB->nChunks++;
    sCmB->nVerts += n;
    return true;
}

/* First sight of a key: parse the run (G_VTX / G_TRI1 / G_TRI2 only), copy its words, bake the mesh
 * (one run, stock triangle order) and its resident buffer. false = not bakeable (the entry stays,
 * marked failed: that key is walked until the generation moves on). */
GDX_COLD bool CmBake(GdxInterpSeam& g, CmEntry& e, const Gdx3dsCourseRun& run, Gdx3dsMeshResolveFn resolve, void* user) {
    sCmWin.bakes++;
    if (sCmB == nullptr) {
        sCmB = (CmBuild*)malloc(sizeof(CmBuild));
        if (sCmB == nullptr) {
            return CmFail(e, "no heap", 0, 0);
        }
    }
    if (run.nCmds > kCmMaxCmds) {
        return CmFail(e, "command capacity", run.nCmds, 0);
    }
    memset(sCmB, 0, sizeof(*sCmB));
    memset(sCmB->slotChunk, 0xFF, sizeof(sCmB->slotChunk));
    e.words = (uint32_t*)malloc((size_t)run.nCmds * 3u * sizeof(uint32_t));
    if (e.words == nullptr) {
        return CmFail(e, "no heap", 0, 0);
    }
    const uint16_t texS = *g.R().tex_scale_s;
    const uint16_t texT = *g.R().tex_scale_t;
    e.tail[0] = e.tail[1] = kCmNone;
    const uint8_t* p = run.words;
    for (uint32_t i = 0; i < run.nCmds; i++, p += run.stride) {
        uint32_t w0, w1, w1hi;
        CmReadWords(p, run.stride, &w0, &w1, &w1hi);
        e.words[i * 3 + 0] = w0;
        e.words[i * 3 + 1] = w1;
        e.words[i * 3 + 2] = w1hi;
        const uint8_t op = (uint8_t)(w0 >> 24);
        switch (op) {
            case kCmOpVtx:
                if (!CmAddVtx(e, w0, w1, w1hi, texS, texT, i, resolve, user)) {
                    return false;
                }
                e.tail[0] = e.tail[1];
                e.tail[1] = i;
                break;
            case kCmOpTri1:
                if (!CmAddTri(e, ((w0 >> 16) & 0xFFu) / 2, ((w0 >> 8) & 0xFFu) / 2, (w0 & 0xFFu) / 2, i, op)) {
                    return false;
                }
                break;
            case kCmOpTri2:
                if (!CmAddTri(e, ((w0 >> 16) & 0xFFu) / 2, ((w0 >> 8) & 0xFFu) / 2, (w0 & 0xFFu) / 2, i, op) ||
                    !CmAddTri(e, ((w1 >> 16) & 0xFFu) / 2, ((w1 >> 8) & 0xFFu) / 2, (w1 & 0xFFu) / 2, i, op)) {
                    return false;
                }
                break;
            default:
                return CmFail(e, "unexpected opcode", i, op);
        }
    }
    if (sCmB->nTris == 0) {
        return CmFail(e, "no triangles", run.nCmds, 0);
    }
    Mesh* m = (Mesh*)calloc(1, sizeof(Mesh));
    if (m == nullptr) {
        return CmFail(e, "no heap", 0, 0);
    }
    Gdx3dsMeshRun r;
    memset(&r, 0, sizeof(r));
    r.firstTri = 0;
    r.triCount = (uint16_t)sCmB->nTris;
    r.primKind = GDX3DS_MESH_COLOR_INHERIT;
    r.envKind = GDX3DS_MESH_COLOR_INHERIT;
    m->magic = kMeshMagic;
    m->nRuns = 1;
    m->nChunks = (uint16_t)sCmB->nChunks;
    m->nTris = (uint16_t)sCmB->nTris;
    m->nVerts = (uint16_t)sCmB->nVerts;
    m->runs = DupArray(&r, 1);
    m->chunks = DupArray(sCmB->chunks, sCmB->nChunks);
    m->tris = DupArray(sCmB->tris, (size_t)sCmB->nTris * 3);
    m->vtxChunk = (uint8_t*)malloc(sCmB->nVerts);
    m->xfRunTex = (uint16_t*)malloc(sizeof(uint16_t) * 2u);
    if (m->runs == nullptr || m->chunks == nullptr || m->tris == nullptr || m->vtxChunk == nullptr ||
        m->xfRunTex == nullptr) {
        FreeMesh(m);
        return CmFail(e, "no heap", 0, 0);
    }
    for (unsigned c = 0; c < sCmB->nChunks; c++) {
        for (unsigned k = 0; k < sCmB->chunks[c].count; k++) {
            m->vtxChunk[sCmB->chunks[c].baseVertex + k] = (uint8_t)c;
        }
    }
    m->xfScaleOk = XfComputeRunScales(m);
    e.res = ResBuild(m);
    if (e.res == nullptr) {
        FreeMesh(m);
        return CmFail(e, "no linear memory", 0, 0);
    }
    e.resBytes = ResBytesFor(m);
    sResBytes += e.resBytes;
    sResCount++;
    m->res = e.res;
    m->resState = 1;
    XfMeshBox(m, e.box);
    e.mesh = m;
    e.pub.verts = e.res;
    e.pub.idx = (const uint16_t*)(e.res + ResIdxOffset(m));
    e.pub.nIdx = (uint32_t)m->nTris * 3u;
    for (int a = 0; a < 3; a++) {
        e.pub.lo[a] = e.box.lo[a];
        e.pub.hi[a] = e.box.hi[a];
    }
    e.pub.tail[0] = e.tail[0];
    e.pub.tail[1] = e.tail[1];
    Log("[nicourse] bake vtx=%08lx n=%lu gen=%lu cmds=%lu chunks=%u verts=%u tris=%u res=%luB box=[%d %d %d]..[%d %d %d]",
        (unsigned long)e.vtxW1, (unsigned long)e.vtxCount, (unsigned long)e.gen, (unsigned long)run.nCmds,
        sCmB->nChunks, sCmB->nVerts, sCmB->nTris, (unsigned long)e.resBytes, (int)e.box.lo[0], (int)e.box.lo[1],
        (int)e.box.lo[2], (int)e.box.hi[0], (int)e.box.hi[1], (int)e.box.hi[2]);
    return true;
}

/* A slot for a new key: a free one, else the least recently used (its buffers retire deferred). */
GDX_COLD CmEntry* CmClaim(const Gdx3dsCourseRun& run) {
    CmEntry* best = &sCm[0];
    for (int i = 0; i < kCmEntries; i++) {
        CmEntry& e = sCm[i];
        if (!e.used) {
            best = &e;
            break;
        }
        if (e.lastUse < best->lastUse) {
            best = &e;
        }
    }
    if (best->used) {
        CmRelease(*best);
    }
    best->used = 1;
    best->failed = 0;
    best->vtxW1 = run.vtxW1;
    best->vtxCount = run.vtxCount;
    best->gen = run.gen;
    best->nCmds = run.nCmds;
    best->stride = run.stride;
    return best;
}

GDX_COLD CmEntry* CmLookupSlow(GdxInterpSeam& g, const Gdx3dsCourseRun& run, Gdx3dsMeshResolveFn resolve, void* user) {
    CmEntry* e = CmClaim(run);
    if (!CmBake(g, *e, run, resolve, user)) {
        free(e->words);
        e->words = nullptr;
        return e; /* failed: remembered for this key */
    }
    return e;
}

/* The round-1 draw (cold since round 2: the first frame of a material, or every frame when the NRR
 * hand-off is not available). 1 = the run is taken (drawn, or provably nothing to draw); 0 = nothing
 * touched. `out` = what the draw bound, for the walker's material memo. */
GDX_COLD int CmColdDraw(GdxInterpSeam& g, CmEntry& e, Gdx3dsCourseCold* out) {
    Mesh* m = e.mesh;
    out->drawn = 0;
    Gdx3dsVtxMathState vs;
    if (!g.LatchVtxState(vs) || vs.fog || !XfMeshEligible(m, vs)) {
        sCmWin.fbVs++;
        return 0;
    }
    float wMin = 0.0f;
    const XfCover cover = XfClassify(vs, e.box, &wMin);
    if (cover == XF_OUTSIDE) {
        sCmWin.outside++; /* every triangle all-out: the CPU path binds nothing, draws nothing */
        return 1;
    }
    if (cover == XF_STRADDLE) {
        /* The preview frames each course to its viewport, so the tube reaches the edges: the CPU path
         * Sutherland-Hodgman clips those triangles, the PICA clips them here (the machines' performance
         * mode argument: every box corner in front of the eye keeps the error bound a real bound).
         * Tolerance class (nrr_tol --class 3d). */
        if (!(wMin > kXfMinW)) {
            sCmWin.fbClip++;
            return 0;
        }
        sCmWin.straddle++;
    }
    const unsigned errMpx = XfErrorBoundMpx(vs, e.box, wMin);
    sCmWin.maxErrMpx = errMpx > sCmWin.maxErrMpx ? errMpx : sCmWin.maxErrMpx;
    if (sGpuXfMaxErr >= 0 && errMpx > (unsigned)sGpuXfMaxErr) {
        sCmWin.fbErr++;
        return 0;
    }
    const int cullMode = CullModeOf(*g.R().geometry_mode);
    if (cullMode == GDX3DS_CULL_BOTH) {
        sCmWin.fbBind++;
        return 0;
    }
    g.Flush(); /* the batch open before the run draws first, as the run's first resolve would flush it */
    GdxSeamMaterial mat;
    memset(&mat, 0, sizeof(mat));
    if (!g.BindMaterial(mat)) {
        sCmWin.fbBind++;
        return 0;
    }
    for (int j = 0; j < mat.numInputs && j < 7; j++) {
        if (mat.inputMap[0][j] == GDXSEAM_CCMUX_LOD_FRACTION || mat.inputMap[1][j] == GDXSEAM_CCMUX_LOD_FRACTION) {
            sCmWin.fbBind++;
            return 0;
        }
    }
    Gdx3dsVboPackParams params;
    memset(&params, 0, sizeof(params));
    if (!gdx3ds_mesh_backend_xf_params(&params)) {
        sCmWin.fbBind++;
        return 0;
    }
    sXfEpoch++;
    const Gdx3dsLoadedVtx first = *XfVertex(vs, m, m->tris[0]);
    PackCtx c;
    if (!PreparePackCtx(g, mat, params, &first, c)) {
        sCmWin.fbBind++;
        return 0;
    }
    Gdx3dsXfDraw xf;
    XfFillMesh(vs, xf);
    xf.verts = e.res;
    xf.nVerts = m->nVerts;
    xf.idxResident = 1;
    xf.stateElide = 1;
    XfFillRun(c, mat, m->xfRunTex[0], m->xfRunTex[1], vs, xf);
    Gdx3dsVboPackAux aux;
    BuildAux(g.R(), mat, first, aux);
    const bool invert = (*g.R().extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0;
    xf.cullMode = cullMode;
    if (invert && cullMode == GDX3DS_CULL_FRONT) {
        xf.cullMode = GDX3DS_CULL_BACK;
    } else if (invert && cullMode == GDX3DS_CULL_BACK) {
        xf.cullMode = GDX3DS_CULL_FRONT;
    }
    xf.clsW = first.w;
    g.ApplyDrawConstants();
    gdx3ds_mesh_backend_xf_draw((const uint16_t*)(e.res + ResIdxOffset(m)), (uint32_t)m->nTris * 3u, &aux, &xf);
    sCmWin.drawn++;
    sCmWin.cold++;
    sCmWin.tris += m->nTris;
    sCmWin.verts += m->nVerts;
    out->drawn = 1;
    memcpy(out->xf, &xf.mvp[0][0], sizeof(out->xf));
    memcpy(out->in, aux.inputRgba, sizeof(out->in));
    out->numInputs = aux.numInputs;
    out->cullMode = xf.cullMode;
    out->xMode = vs.xMode;
    out->aspect = vs.aspect;
    out->memoOk = (!mat.usedTextures[0] && !mat.usedTextures[1] && !vs.fog && !vs.lighting && !mat.use_fog) ? 1 : 0;
    return 1;
}

GDX_COLD void CmReceipt() {
    const Gdx3dsCourseWin& w = sCmWin;
    if (w.models == 0 && w.bakes == 0 && w.fails == 0 && w.vfyModels == 0) {
        return;
    }
    Log("[nicourse] frames=%u models=%u hits=%u drawn=%u (nrr=%u cold=%u) memos=%u/%u straddle=%u offscreen=%u "
        "outside=%u tris=%lu verts=%lu | fb words=%u vs=%u clip=%u err=%u bind=%u open=%u | bakes=%u fails=%u "
        "maxerr=%umpx | vfy models=%u bad=%u words=%u miss=%u | us=%lu/f",
        w.frames, w.models, w.hits, w.drawn, w.nrr, w.cold, w.memos, w.memoFails, w.straddle, w.offscreen, w.outside,
        w.tris, w.verts, w.fbWords, w.fbVs, w.fbClip, w.fbErr, w.fbBind, w.fbOpen, w.bakes, w.fails, w.maxErrMpx, w.vfyModels, w.vfyBad, w.vfyWords, w.vfyMiss,
        (unsigned long)(w.frames != 0 ? w.ticks * 1000ull / (uint64_t)CPU_TICKS_PER_MSEC / w.frames : 0ull));
    memset(&sCmWin, 0, sizeof(sCmWin));
}

/* Frame boundary (gdx3ds_mesh_frame_begin, drawing thread): count frames that drew a model; the
 * receipt every 64 of them. */
void CmFrameBegin() {
    if (sCmFrameModels == 0) {
        return;
    }
    sCmFrameModels = 0;
    sCmWin.frames++;
    if (sCmWin.frames >= 64) {
        CmReceipt();
    }
}

} // namespace

int gdx3ds_mesh_course_level(void) {
    return CmLevel();
}

int gdx3ds_mesh_course_maxerr(void) {
    return sGpuXfMaxErr;
}

const Gdx3dsCourseModel* gdx3ds_mesh_course_model(GdxInterpSeam& I, const Gdx3dsCourseRun& run,
                                                  Gdx3dsMeshResolveFn resolve, void* user) {
    if (CmLevel() != 1 || resolve == nullptr || (run.stride != 16 && run.stride != 8)) {
        return nullptr;
    }
    sCmWin.models++;
    sCmFrameModels++;
    CmEntry* e = NrrCourseHotFind(run);
    if (GDX_UNLIKELY(e == nullptr)) {
        e = CmLookupSlow(I, run, resolve, user);
    } else {
        sCmWin.hits++;
    }
    e->lastUse = ++sCmUse;
    if (GDX_UNLIKELY(e->failed)) {
        return nullptr;
    }
    if (GDX_UNLIKELY(!NrrCourseHotWords(*e, run))) {
        sCmWin.fbWords++;
        return nullptr;
    }
    return &e->pub;
}

int gdx3ds_mesh_course_cold(GdxInterpSeam& I, const Gdx3dsCourseModel* m, Gdx3dsCourseCold* out) {
    CmEntry* e = reinterpret_cast<CmEntry*>(reinterpret_cast<uint8_t*>(const_cast<Gdx3dsCourseModel*>(m)) -
                                            offsetof(CmEntry, pub));
    return CmColdDraw(I, *e, out);
}

void gdx3ds_mesh_course_verify(const Gdx3dsCourseRun& run, Gdx3dsMeshResolveFn resolve, void* user) {
    if (CmLevel() != 2 || resolve == nullptr || (run.stride != 16 && run.stride != 8)) {
        return;
    }
    sCmWin.vfyModels++;
    sCmFrameModels++;
    CmEntry* e = NrrCourseHotFind(run);
    if (e == nullptr) {
        /* the bake reads the tex scale through a seam; verify keys need none (they are never drawn) */
        e = CmLookupSlow(*static_cast<GdxInterpSeam*>(user), run, resolve, user);
        if (e->failed) {
            return;
        }
        /* first sight: the copy was just taken from the live bytes; mesh / resident buffer unused */
        return;
    }
    if (e->failed || e->mesh == nullptr) {
        sCmWin.vfyMiss++;
        return;
    }
    if (!NrrCourseHotWords(*e, run)) {
        sCmWin.vfyWords++;
    }
    /* The resident copy against the bytes the stock walk reads now (ResMatches' comparison). */
    const Mesh* m = e->mesh;
    bool same = true;
    for (unsigned ci = 0; ci < m->nChunks && same; ci++) {
        const Gdx3dsMeshChunk& ch = m->chunks[ci];
        const void* live = resolve(ch.srcW1, (size_t)ch.count * 16u, user);
        if (live == nullptr || memcmp(e->res + (size_t)ch.baseVertex * 16u, live, (size_t)ch.count * 16u) != 0) {
            same = false;
        }
    }
    if (!same) {
        sCmWin.vfyBad++;
    }
}
