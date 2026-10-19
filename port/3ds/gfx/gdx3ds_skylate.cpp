/* port/3ds/gfx/gdx3ds_skylate.cpp -- n60tail: [native] sky_late (contract and exactness argument in
 * gdx3ds_skylate.h; design, gates and costs in docs/research/native/n60tail-progress.md §3).
 *
 * GX queue facts used (citro3d 1.7.1 + libctru gxqueue, see gdx3ds_overlap.c): from C3D_FrameBegin
 * until the first [overlap] kick the frame's queue is stopped, so entries added before that kick can
 * be reordered freely (nothing has been submitted). A kick inside the sky or the region refuses the
 * frame. */
#include "gdx3ds_skylate.h"

#include <cstdio>
#include <cstring>

#include <3ds.h>
#include <citro3d.h>

extern "C" {
#include "../vendor/citro3d/source/internal.h"
}
#include "../vendor/citro3d/gdx/c3d_gdx.h"
#include "gdx3ds_interp_seam.h"
#include "../gdx3ds_mainprof.h" /* GDX3DS_C5CAT_* */
#include "gdx3ds_overlap.h"
#include "gdx3ds_replay.h"
#include "gdx3ds_stereo.h"
#include "nrr/gdx3ds_nrr.h" /* NRR_CAT_SKY */

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

int gdx3ds_skylate_marking = 0;

namespace {

constexpr unsigned kAbBit = 12;     /* sky_late = 2: on in even 4096-frame blocks */
constexpr unsigned kWindow = 512;

enum Phase { kIdle, kSky, kMark, kDone };

/* Why a frame kept the original order (the receipt's columns). */
enum Why {
    kWhyNoClear = 0, /* no main depth clear this frame before the sky */
    kWhyKick,        /* the queue was kicked before or inside the sky / region */
    kWhyHandoff,     /* a marker inside an NRR hand-off */
    kWhyBind,        /* a texture target bound in the sky / region, or the sky not on the main target */
    kWhyEntry,       /* a GX entry other than a command list (a clear, a copy) in the sky / region */
    kWhyOrder,       /* SKY not followed by TRK, or the frame ended inside the sky / region */
    kWhyStereo,      /* the right eye on the per-draw loop, or the replay body started inside the sky */
    kWhyEmpty,       /* the sky or the region has no command list */
    kWhyEffect,      /* an effect bound in the sky / region breaks the exactness rule (bits in `bad`) */
    kWhyCount
};

int sLever = -1;
bool sThroughDress = false;
bool sNegative = false;      /* sky_late + 256: TEST ONLY, the region does not mark (the sky then covers it) */
bool sOn = false;            /* this frame's phase */
Phase sPhase = kIdle;
bool sCleared = false;
bool sBindBad = false;
bool sEnterBad = false;
bool sAdopt = false;
bool sSkyInRegion = false;
int sFb = 0;
int sSkyStart = 0;
int sSkyEnd = 0;
int sKicksAtStart = 0;
const u32* sSplitBuf = nullptr;
u32 sSplitOff = 0;
u32 sSkyBad = 0;

struct Window {
    unsigned frames;      /* frames begun with the phase on */
    unsigned skies;       /* frames whose SKY marker opened a sky run */
    unsigned moved;       /* frames reordered */
    unsigned why[kWhyCount];
    unsigned skyLists;    /* command lists moved (sky) */
    unsigned regionLists; /* command lists moved ahead of it (region) */
    u32 bad;              /* OR of C3DGDX_STB_* of the refused frames */
    unsigned skyBad;      /* ...of which the sky drew with a bad effect */
    u32 first[4];         /* the window's first bad effect: fragOpMode, alphaBlend, logicOp, depth/colour mask */
    bool haveFirst;
} sWin;
unsigned sWinFrames = 0;

void Log(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(msg, std::strlen(msg));
    }
}

gxCmdQueue_s* Queue() {
    return &C3Di_GetContext()->gxQueue;
}

int ConfigInt(const char* section, const char* key, int fallback) {
    return &gdx3ds_config_get_int != nullptr ? gdx3ds_config_get_int(section, key, fallback) : fallback;
}

void Latch() {
    const int v = ConfigInt("native", "sky_late", 0);
    const int mode = v & 3;
    sLever = (mode == 1 || mode == 2) ? mode : 0;
    sThroughDress = (v & 4) != 0;
    sNegative = (v & 256) != 0;
    const char* off = nullptr;
    if (sLever != 0 && gdx3ds_replay_on) {
        off = "C6 replay";
    } else if (sLever != 0 && ConfigInt("native", "nrr_direct", 0) == 2) {
        off = "nrr_direct=2 verify";
    } else if (sLever != 0 && ConfigInt("debug", "gpucensus", 0) >= 2) {
        off = "gpucensus fill mode";
    }
    if (off != nullptr) {
        sLever = 0;
    }
    char msg[224];
    std::snprintf(msg, sizeof(msg),
                  "[skylate] init sky_late=%d (lever %d%s, region = track%s; the sky's lists run after the region's "
                  "with stencil EQUAL 0, the region marks stencil 1)%s%s%s",
                  v, sLever, sLever == 2 ? " A/B: on in even 4096-frame blocks" : "", sThroughDress ? " + dressing" : "",
                  off != nullptr ? " REFUSED: " : "", off != nullptr ? off : "",
                  sNegative ? " NEGATIVE CONTROL (test only): the region marks nothing" : "");
    Log(msg);
}

void EmitWindow() {
    char msg[400];
    std::snprintf(msg, sizeof(msg),
                  "[skylate] frames=%u skies=%u moved=%u lists sky/f=%.2f region/f=%.2f | kept: noclear=%u kick=%u "
                  "handoff=%u bind=%u entry=%u order=%u stereo=%u empty=%u effect=%u (sky %u; bad=0x%lx first "
                  "op=0x%lx blend=0x%lx logic=%lu depth=0x%lx)",
                  sWin.frames, sWin.skies, sWin.moved, sWin.moved ? (double)sWin.skyLists / sWin.moved : 0.0,
                  sWin.moved ? (double)sWin.regionLists / sWin.moved : 0.0, sWin.why[kWhyNoClear], sWin.why[kWhyKick],
                  sWin.why[kWhyHandoff], sWin.why[kWhyBind], sWin.why[kWhyEntry], sWin.why[kWhyOrder],
                  sWin.why[kWhyStereo], sWin.why[kWhyEmpty], sWin.why[kWhyEffect], sWin.skyBad, (unsigned long)sWin.bad,
                  (unsigned long)sWin.first[0], (unsigned long)sWin.first[1], (unsigned long)sWin.first[2],
                  (unsigned long)sWin.first[3]);
    Log(msg);
    std::memset(&sWin, 0, sizeof(sWin));
}

int Kicks() {
    return gdx3ds_overlap_frame_kicks();
}

void FlushSeam(void* seam) {
    if (seam != nullptr) {
        static_cast<GdxInterpSeam*>(seam)->Flush(); /* painter's order: the open batch before the boundary */
    }
}

/* Close the open command list (if it holds anything) and make the next list self-contained. */
void Boundary() {
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    Gdx3dsStereo::MarkStateSelfContained();
}

void NoteBad(u32 bad) {
    sWin.bad |= bad;
    if (!sWin.haveFirst) {
        std::memcpy(sWin.first, C3Dgdx_stencilFirst, sizeof(sWin.first));
        sWin.haveFirst = true;
    }
}

void SetStencil(u32 mode) {
    if (mode == C3DGDX_STENCIL_SKY) {
        /* test EQUAL ref 0 under mask 0xFF, write mask 0; ops KEEP */
        C3Dgdx_stencilWords[0] = 1u | (static_cast<u32>(GPU_EQUAL) << 4) | (0x00u << 8) | (0u << 16) | (0xFFu << 24);
        C3Dgdx_stencilWords[1] = static_cast<u32>(GPU_STENCIL_KEEP) | (static_cast<u32>(GPU_STENCIL_KEEP) << 4) |
                                 (static_cast<u32>(GPU_STENCIL_KEEP) << 8);
    } else if (mode == C3DGDX_STENCIL_MARK) {
        /* test ALWAYS ref 1, write mask 0xFF; zpass REPLACE */
        C3Dgdx_stencilWords[0] = 1u | (static_cast<u32>(GPU_ALWAYS) << 4) | (0xFFu << 8) | (1u << 16) | (0xFFu << 24);
        C3Dgdx_stencilWords[1] = static_cast<u32>(GPU_STENCIL_KEEP) | (static_cast<u32>(GPU_STENCIL_KEEP) << 4) |
                                 (static_cast<u32>(sNegative ? GPU_STENCIL_KEEP : GPU_STENCIL_REPLACE) << 8);
    }
    C3Dgdx_stencilMode = mode;
    C3Dgdx_stencilBad = 0;
    C3Dgdx_stencilCur = 0;
    gdx3ds_skylate_marking = mode == C3DGDX_STENCIL_MARK ? 1 : 0;
}

void StencilOff() {
    C3Dgdx_stencilMode = 0;
    gdx3ds_skylate_marking = 0;
}

/* The frame keeps the original order from here: no more stencil words, citro3d's own state next. */
void Keep(Why why, void* seam, bool split) {
    if (sPhase == kSky || sPhase == kMark) {
        FlushSeam(seam);
        StencilOff();
        if (split) {
            Boundary();
        } else {
            Gdx3dsStereo::MarkStateSelfContained();
        }
    }
    sWin.why[why]++;
    sPhase = kDone;
}

void StartSky(void* seam) {
    sWin.skies++;
    if (!sCleared) {
        Keep(kWhyNoClear, seam, false);
        return;
    }
    if (Kicks() != 0) {
        Keep(kWhyKick, seam, false);
        return;
    }
    if (gdx3ds_nrr_handoff_open != 0) {
        Keep(kWhyHandoff, seam, false);
        return;
    }
    if (sFb != 0) {
        Keep(kWhyBind, seam, false);
        return;
    }
    sKicksAtStart = Kicks();
    SetStencil(C3DGDX_STENCIL_SKY);
    if (Gdx3dsStereo::Active() && !Gdx3dsStereo::ReplayActive()) {
        /* 3D, replay body not entered yet: the sky's own NRR hand-off enters it, and ReplayEnter's
         * pre-body split + full state flush are the sky's start (no list of our own). Adopted in
         * gdx3ds_skylate_before_enter if nothing was written in between. The interpreter's open batch
         * is NOT flushed here: it goes out at the hand-off's NrrBegin, after ReplayEnter, as it would
         * without the lever (inside the body, replayed for the right eye; flushed here it would be
         * drawn before the body by the per-draw loop instead). It then belongs to the sky's run. */
        sSkyStart = -1;
        sSplitBuf = gpuCmdBuf;
        sSplitOff = gpuCmdBufOffset;
    } else {
        FlushSeam(seam);
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        sSkyStart = Queue()->numEntries;
        sSplitBuf = gpuCmdBuf;
        sSplitOff = gpuCmdBufOffset;
        Gdx3dsStereo::MarkStateSelfContained();
    }
    sPhase = kSky;
}

void StartMark(void* seam) {
    if (gdx3ds_nrr_handoff_open != 0) {
        Keep(kWhyHandoff, seam, true);
        return;
    }
    if (sSkyStart < 0) {
        Keep(kWhyStereo, seam, true); /* the replay body never started at the sky */
        return;
    }
    FlushSeam(seam);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    sSkyEnd = Queue()->numEntries;
    sSkyBad = C3Dgdx_stencilBad;
    if (sSkyBad != 0) {
        NoteBad(sSkyBad);
        sWin.skyBad++;
        StencilOff();
        Gdx3dsStereo::MarkStateSelfContained();
        sWin.why[kWhyEffect]++;
        sPhase = kDone;
        return;
    }
    SetStencil(C3DGDX_STENCIL_MARK);
    Gdx3dsStereo::MarkStateSelfContained();
    sPhase = kMark;
}

bool OnlyLists(int from, int to) {
    const gxCmdQueue_s* q = Queue();
    for (int k = from; k < to; k++) {
        if ((q->entries[k].data[0] & 0xFFu) != 0x01u) {
            return false;
        }
    }
    return true;
}

/* [sSkyStart, sSkyEnd) [sSkyEnd, end) -> [sSkyEnd, end) [sSkyStart, sSkyEnd) */
void Rotate(int end) {
    gxCmdQueue_s* q = Queue();
    const int nSky = sSkyEnd - sSkyStart;
    const int nRegion = end - sSkyEnd;
    gxCmdEntry_s sky[32];
    std::memcpy(sky, q->entries + sSkyStart, sizeof(gxCmdEntry_s) * static_cast<size_t>(nSky));
    std::memmove(q->entries + sSkyStart, q->entries + sSkyEnd, sizeof(gxCmdEntry_s) * static_cast<size_t>(nRegion));
    std::memcpy(q->entries + sSkyStart + nRegion, sky, sizeof(gxCmdEntry_s) * static_cast<size_t>(nSky));
    sWin.skyLists += static_cast<unsigned>(nSky);
    sWin.regionLists += static_cast<unsigned>(nRegion);
}

void Close(void* seam) {
    if (gdx3ds_nrr_handoff_open != 0) {
        Keep(kWhyHandoff, seam, true);
        return;
    }
    FlushSeam(seam);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    const int end = Queue()->numEntries;
    const u32 bad = C3Dgdx_stencilBad;
    StencilOff();
    Gdx3dsStereo::MarkStateSelfContained();
    sPhase = kDone;
    Why why = kWhyCount;
    if (bad != 0) {
        NoteBad(bad);
        why = kWhyEffect;
    } else if (Kicks() != sKicksAtStart || Kicks() != 0 || Queue()->curEntry > static_cast<u16>(sSkyStart)) {
        why = kWhyKick;
    } else if (sBindBad) {
        why = kWhyBind;
    } else if (sSkyInRegion) {
        why = kWhyOrder;
    } else if (sEnterBad || (Gdx3dsStereo::Active() && !Gdx3dsStereo::ReplayActive())) {
        why = kWhyStereo;
    } else if (sSkyEnd <= sSkyStart || end <= sSkyEnd) {
        why = kWhyEmpty;
    } else if (!OnlyLists(sSkyStart, end)) {
        why = kWhyEntry;
    }
    if (why != kWhyCount) {
        sWin.why[why]++;
        return;
    }
    Rotate(end);
    sWin.moved++;
}

} // namespace

extern "C" void gdx3ds_skylate_frame_begin(unsigned long frameIndex) {
    if (sLever < 0) {
        Latch();
    }
    StencilOff();
    sPhase = kIdle;
    sSkyInRegion = false;
    sCleared = false;
    sBindBad = false;
    sEnterBad = false;
    sAdopt = false;
    sFb = 0;
    sOn = sLever == 1 || (sLever == 2 && ((frameIndex >> kAbBit) & 1ul) == 0ul);
    if (sLever == 0) {
        return;
    }
    if (sOn) {
        sWin.frames++;
    }
    if (++sWinFrames >= kWindow) {
        sWinFrames = 0;
        if (sWin.frames > 0) {
            EmitWindow();
        }
    }
}

extern "C" void gdx3ds_skylate_marker(unsigned category, void* seam) {
    if (!sOn || sPhase == kDone) {
        return;
    }
    if (sPhase == kIdle) {
        if (category == GDX3DS_C5CAT_SKY) {
            StartSky(seam);
        }
    } else if (sPhase == kSky) {
        if (category == GDX3DS_C5CAT_TRK) {
            StartMark(seam);
        } else if (category != GDX3DS_C5CAT_SKY) {
            Keep(kWhyOrder, seam, true);
        }
    } else if (category != GDX3DS_C5CAT_TRK && !(sThroughDress && category == GDX3DS_C5CAT_DRESS)) {
        Close(seam);
    }
}

extern "C" void gdx3ds_skylate_note_depth_clear(void) {
    if (sOn && sPhase == kIdle) {
        sCleared = true;
    }
}

extern "C" void gdx3ds_skylate_note_bind(int fbId) {
    sFb = fbId;
    if ((sPhase == kSky || sPhase == kMark) && fbId != 0) {
        sBindBad = true;
    }
}

extern "C" void gdx3ds_skylate_before_enter(void) {
    if (sPhase != kSky && sPhase != kMark) {
        return;
    }
    /* The body may start at the sky's own first list only if nothing was written since the sky's
     * split: then the pre-body list holds the prologue words alone and the body starts the sky. */
    if (sPhase == kSky && gpuCmdBuf == sSplitBuf && gpuCmdBufOffset == sSplitOff &&
        (sSkyStart < 0 || Queue()->numEntries == sSkyStart)) {
        sAdopt = true;
    } else {
        sEnterBad = true;
    }
}

extern "C" void gdx3ds_skylate_after_enter(int bodyStart) {
    if (sAdopt) {
        sAdopt = false;
        sSkyStart = bodyStart;
    }
}

extern "C" void gdx3ds_skylate_frame_end(void) {
    if (sPhase == kSky || sPhase == kMark) {
        sWin.why[kWhyOrder]++;
        sPhase = kDone;
    }
    StencilOff();
}

extern "C" void gdx3ds_skylate_note_nrr(int nrrCat) {
    if (nrrCat == NRR_CAT_SKY) {
        sSkyInRegion = true;
    }
}

extern "C" int gdx3ds_skylate_phase(void) {
    return sOn ? 4 : 0;
}
