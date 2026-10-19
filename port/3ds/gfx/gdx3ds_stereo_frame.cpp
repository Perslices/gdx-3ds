/* port/3ds/gfx/gdx3ds_stereo_frame.cpp -- n60stereo: the stereo FRAME REPLAY (contract in
 * gdx3ds_stereo.h, "frame replay"; design, gates and costs in docs/research/native/n60stereo-progress.md).
 *
 * The right eye is the left eye's command lists re-submitted: every draw of the body is emitted once
 * with an eye-independent stereo row (`stk`), and the shader multiplies that row by `steye`, which
 * only the prologues here write (raw words, never through citro3d, so no citro3d re-emission can
 * put a stale eye inside the body). Everything runs on the thread that owns the C3D frame.
 *
 * GX queue rules this file relies on (citro3d 1.7.1 + libctru gxqueue, see gdx3ds_overlap.c):
 *   - the frame's queue is cleared at C3D_FrameBegin and holds every entry of the frame until then
 *     (a kick submits entries, it does not remove them): entries [bodyStart, n) are the body;
 *   - gdx3ds_overlap_before_frame_end drains and STOPS a kicked queue, so what we add before
 *     C3D_FrameEnd waits for its gxCmdQueueRun, after its linear-heap flush; our own lists are
 *     added with GX_CMDLIST_FLUSH anyway;
 *   - a GX_MemoryFill entry: data[1..3] = buffer 0 start / value / end, data[4..6] buffer 1. */
#include "gdx3ds_stereo.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "../vendor/citro3d/source/internal.h"
}
#include "../vendor/citro3d/gdx/c3d_gdx.h" /* C3Dgdx_FlushState: the body's state, emitted at its start */
#include "gdx3ds_gputl.h"                    /* n60tail: a folded list's eye for the [gputl] classification */
#include "gdx3ds_replay.h"                   /* n60tail: C6 [replay] keeps its own transfer targets */
#include "gdx3ds_skylate.h"                  /* n60tail: [native] sky_late's view of the body start */

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_get_bool(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
/* niview Part 2: the race's live player count (decomp global). >1 means a multi-view (2P/4P split
 * screen) frame -- the frame replay's clip-square scissor (NrrClipScissorWords, gdx3ds_nrr.cpp) is
 * eye-independent (baked once as a fixed-function register word, not a shader output that can read
 * the dynamic steye), so a multi-view frame's outer-edge scissor is wrong per eye under replay
 * (docs/research/native/niview-progress.md §8 item 2). Part 1 (gdx3ds_stereo_nrr_ok /
 * ReplayOkThisFrame, same file) closed the "consumed via the lever but this frame's replay didn't
 * happen" black-section gap for ANY refusal, so refusing here is now safe: every NRR category falls
 * back cleanly to its own correct per-eye stock path instead of leaving a section undrawn. */
extern int gNumPlayers __attribute__((weak));

int gdx3ds_stereo_replay_frame = 0;
int gdx3ds_nrr_handoff_open = 0;
float gdx3ds_stereo_rows[2][4] = {};

namespace {

constexpr int kMaxSwaps = 4;     /* mid-body main-target rebinds per frame the replay can swap */
constexpr u32 kReserve = 4;      /* queue slots kept free: prologue, epilogue, two display transfers */
constexpr unsigned kWindow = 256;

/* C3DiF_* everything a body's first draw must re-emit (gdx3ds_replay.cpp kDirtyAll less the
 * framebuffer, which the prologues own). */
constexpr u32 kDirtyBody = C3DiF_AttrInfo | C3DiF_BufInfo | C3DiF_Effect | C3DiF_Viewport | C3DiF_Scissor |
                           C3DiF_Program | C3DiF_TexEnvBuf | C3DiF_TexStatus | C3DiF_FogLut | C3DiF_TexAll |
                           C3DiF_TexEnvAll;

/* A mid-body main-target rebind: queue entry `entry` is the left eye's framebuffer list; the right
 * eye's replay submits `buf`/`words` (the right target's framebuffer words) in its place. */
struct Swap {
    int entry;
    u32* buf;
    u32 words;
};

int sLever = -1;               /* [native] stereo_replay, latched at ReplayInit */
int sStkLoc = -1;
int sEyeLoc = -1;
C3D_RenderTarget* sMain = nullptr;
bool sEyeZeroPending = true;   /* the GPU may not hold steye = 0 (boot, HOME / sleep return) */
aptHookCookie sAptCookie;

bool sInBody = false;
/* niview: "this frame's replay outcome", set ONLY by ReplayEnter() (which runs on the thread that
 * owns the C3D frame -- see the file banner -- never called from here a second time). STICKY: it
 * is NOT cleared at ReplayFrameBegin. First attempt (round 1) cleared it there, "conservative until
 * this frame's real ReplayEnter() runs" -- wrong under the tick-ahead pipe, where the game fiber
 * that reads it (gdx3ds_stereo_nrr_ok, a game-side category admission check) runs AHEAD of the
 * render thread that writes it: the clear-at-begin window is not "rare," it is EVERY frame, so every
 * category refused NRR every frame and the stock interpreter's per-eye double-submit overflowed the
 * GPU command buffer (svcBreak in GPUCMD_Add, gated and caught). So instead this holds the latest
 * decided outcome across frame boundaries -- up to a little pipe-stale, same as this codebase's other
 * cross-thread signals, but never wrongly forced false before its own frame's Enter has even run.
 * Starts true (optimistic: no info yet should never block a category). ReplayOkThisFrame() is the
 * read-only accessor; it must NEVER call ReplayEnter(). */
bool sReplayOkThisFrame = true;
int sChunkStart = 0;           /* queue index of the first entry not yet replayed for the right eye */
int sInterleave = 1;           /* [native] stereo_interleave: replay each kicked chunk at its kick */
int sNoReflush = 1;            /* [native] stereo_noreflush: replayed lists skip GSP's cache flush */
/* n60tail (docs/research/native/n60tail-progress.md). Each: 0 off (default), 1 on, 2 a same-run A/B
 * (on in the even blocks of the frame counter bit named below, off in the odd ones; [gputl] tp=). */
int sFold = 0;                 /* [native] stereo_fold: right prologue in the closing left list, epilogue deferred */
int sXlEarly = 0;              /* [native] stereo_xl_early: the left display transfer right after the left eye */
constexpr unsigned kFoldAbBit = 13;  /* stereo_fold = 2: 8192-frame blocks */
constexpr unsigned kXlAbBit = 11;    /* stereo_xl_early = 2: 2048-frame blocks (zclear_elide = 2 uses bit 10) */
unsigned sFrameNo = 0;         /* frame begins (the A/B blocks) */
bool sFoldNow = false;         /* this frame's stereo_fold phase */
bool sXlNow = false;           /* this frame's stereo_xl_early phase */
bool sKickFolded = false;      /* the current kick's closing list carries the right prologue */
uint64_t sFrameTicks = 0;      /* this frame's right-eye CPU (enter + chunks + end) */
unsigned sFrameChunks = 0;
bool sOtherBound = false;      /* a texture target was bound in the body since the main target */
int sBodyStart = 0;
int sNumSwaps = 0;
Swap sSwaps[kMaxSwaps];
bool sMultiViewLogged = false; /* niview Part 2: "[stereo] replay refused: multi-view", logged once */

struct Window {
    unsigned frames;      /* frames begun */
    unsigned stereo;      /* frames with the right-eye pass active */
    unsigned replayed;    /* frames whose right eye was replayed */
    unsigned refused;     /* ReplayEnter refusals (queue / hold) */
    unsigned multiview;   /* niview Part 2: ReplayEnter refusals (gNumPlayers > 1) */
    unsigned overflow;    /* frame ends that could not fit the replay in the queue */
    unsigned lists;       /* body command lists re-submitted */
    unsigned fills;       /* body clears retargeted */
    unsigned skipped;     /* body captures / copies / uploads not repeated */
    unsigned swaps;       /* mid-body main rebinds swapped */
    unsigned chunks;      /* right-eye chunks replayed at a kick (interleave) */
    unsigned chunkSkip;   /* kicks not used (inside an NRR hand-off / no queue room) */
    unsigned unflushed;   /* replayed lists submitted without GSP's cache flush (noreflush) */
    unsigned gxEntries;   /* GX queue entries of the replayed frames at their end (before the transfers) */
    unsigned folds;       /* n60tail: right prologues folded into a closing left list */
    unsigned eyeDefer;    /* n60tail: epilogues (steye 0) deferred to the next frame's first list */
    unsigned xlEarly;     /* n60tail: left display transfers queued before the right eye's last chunk */
    uint64_t enterTicks;  /* CPU in ReplayEnter (the state re-emission included) */
    uint64_t syncTicks;   /* ...of which the body's full citro3d state re-emission */
    uint64_t endTicks;    /* CPU in ReplayFrameEnd */
    uint64_t chunkTicks;  /* CPU in the kick-time chunk replays */
} sWin;

gxCmdQueue_s* Queue() {
    return &C3Di_GetContext()->gxQueue;
}

void Log(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(msg, std::strlen(msg));
    }
}

void AptHook(APT_HookType hook, void* param) {
    (void)param;
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        sEyeZeroPending = true; /* another process drove the GPU: its uniform file is gone */
    }
}

/* steye = (e, 0, 0, 0), raw: C3D_FVec's register image is (w, z, y, x). */
void EmitEye(float e) {
    C3D_FVec v;
    v.x = e;
    v.y = 0.0f;
    v.z = 0.0f;
    v.w = 0.0f;
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | static_cast<u32>(sEyeLoc));
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, reinterpret_cast<u32*>(&v), 4);
}

/* The words of a switch to `target`'s framebuffer, in the open command list: the render cache
 * flushed first when anything was drawn since the last flush (C3Di_UpdateContext's rule). */
void EmitFrameBuf(C3D_RenderTarget* target) {
    C3D_Context* ctx = C3Di_GetContext();
    if (ctx->flags & C3DiF_DrawUsed) {
        ctx->flags &= ~static_cast<u32>(C3DiF_DrawUsed);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
    C3Di_FrameBufBind(&target->frameBuf);
}

/* Close the open command list into the queue (flushed), if it holds anything. */
void SubmitOpenList() {
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
}

/* The words C3Di_SplitFrame closes a list with after a draw (render cache flush + invalidate,
 * early-depth clear), written now: a folded prologue then follows them in the same list, so the GPU
 * runs exactly the words the unfolded split + prologue list would have (less one FINALIZE pair). */
void EmitSplitFlush() {
    C3D_Context* ctx = C3Di_GetContext();
    if (ctx->flags & C3DiF_DrawUsed) {
        ctx->flags &= ~static_cast<u32>(C3DiF_DrawUsed);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
}

/* The right eye's prologue (right target, steye = +1) at the END of the open left list, which the
 * caller closes next: the list then leaves the right target bound, and its replay re-runs those
 * words on the right target (a flush of the right target's render cache, the same bind: no-ops for
 * the image). `entry` = the queue index that list will get ([gputl] classifies it by this note). */
void EmitFoldedPrologue(int entry) {
    EmitSplitFlush();
    EmitFrameBuf(Gdx3dsStereo::RightTarget()); /* DrawUsed is clear: the bind words only */
    EmitEye(1.0f);
    gdx3ds_gputl_note_eye(entry, 1);
    sWin.folds++;
}

/* Close the open command list WITHOUT queueing it (the right eye's swap-in). */
void DetachOpenList(u32** buf, u32* words) {
    GPUCMD_Split(buf, words);
}

bool QueueRoom(u32 need) {
    const gxCmdQueue_s* q = Queue();
    return static_cast<u32>(q->numEntries) + need <= static_cast<u32>(q->maxEntries);
}

const Swap* SwapFor(int entry) {
    for (int i = 0; i < sNumSwaps; i++) {
        if (sSwaps[i].entry == entry) {
            return &sSwaps[i];
        }
    }
    return nullptr;
}

/* A clear of the main target, retargeted to the right one (same size and formats). */
void RetargetFill(gxCmdEntry_s& e) {
    const C3D_FrameBuf& m = sMain->frameBuf;
    const C3D_FrameBuf& r = Gdx3dsStereo::RightTarget()->frameBuf;
    for (int b = 0; b < 2; b++) {
        u32& start = e.data[1 + 3 * b];
        u32& end = e.data[3 + 3 * b];
        u32 to = 0;
        if (start == reinterpret_cast<u32>(m.colorBuf)) {
            to = reinterpret_cast<u32>(r.colorBuf);
        } else if (start == reinterpret_cast<u32>(m.depthBuf)) {
            to = reinterpret_cast<u32>(r.depthBuf);
        }
        if (to != 0) {
            end = to + (end - start);
            start = to;
        }
    }
}

bool FillTouchesMain(const gxCmdEntry_s& e) {
    const C3D_FrameBuf& m = sMain->frameBuf;
    for (int b = 0; b < 2; b++) {
        const u32 start = e.data[1 + 3 * b];
        if (start == reinterpret_cast<u32>(m.colorBuf) || start == reinterpret_cast<u32>(m.depthBuf)) {
            return true;
        }
    }
    return false;
}

/* A body command list again, for the right eye. Its words are unchanged since its own entry
 * (nothing writes a closed list) and that entry ran first in the same queue, so under
 * [native] stereo_noreflush the copy drops GSP's data-cache flush of the list (word 7 of a
 * ProcessCommandList command): the GPU reads the same bytes, the GSP skips a flush by address of
 * the whole list before it starts the GPU on it. */
void ReplayList(const gxCmdEntry_s& src) {
    if (sNoReflush <= 0 || src.data[7] == 0u) {
        gxCmdQueueAdd(Queue(), &src);
        return;
    }
    gxCmdEntry_s e = src;
    e.data[7] = 0u;
    gxCmdQueueAdd(Queue(), &e);
    sWin.unflushed++;
}

/* Re-add one body entry for the right eye. */
void ReplayEntry(int index, const gxCmdEntry_s& src) {
    const u32 id = src.data[0] & 0xFFu;
    if (id == 0x01u) {
        const Swap* sw = SwapFor(index);
        if (sw != nullptr) {
            GX_ProcessCommandList(sw->buf, sw->words * 4u, GX_CMDLIST_FLUSH);
            sWin.swaps++;
        } else {
            ReplayList(src);
            sWin.lists++;
        }
    } else if (id == 0x02u) {
        /* A clear: the main target's goes to the right target; a texture pass's target is cleared
         * again, because the replay draws that pass again. */
        gxCmdEntry_s e = src;
        if (FillTouchesMain(src)) {
            RetargetFill(e);
            sWin.fills++;
        }
        gxCmdQueueAdd(Queue(), &e);
    } else {
        sWin.skipped++; /* captures, copies, uploads: mono, done once (the per-draw loop's rule) */
    }
}


void MarkBodySelfContained() {
    C3D_Context* ctx = C3Di_GetContext();
    ctx->flags = (ctx->flags | kDirtyBody) & ~static_cast<u32>(C3DiF_FrameBuf);
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    /* stk is re-sent by the body even if citro3d never sent it before (EverDirty would skip it). */
    const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][sStkLoc];
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sStkLoc, cur.x, cur.y, cur.z, cur.w);
}

/* Re-add the queue entries [sChunkStart, n) for the right eye, between its prologue (right target,
 * steye = +1) and nothing: the caller adds what follows. false = no queue room (nothing added).
 * `folded`: the last of those entries already ends with the prologue (EmitFoldedPrologue), so no
 * prologue list; the caller checked the room before folding. */
bool ReplayPendingChunk(u32 tail, bool folded) {
    const int start = sChunkStart;
    const int n = static_cast<int>(Queue()->numEntries) - start;
    if (n <= 0) {
        return true;
    }
    if (!QueueRoom(static_cast<u32>(n) + tail)) {
        return false;
    }
    gxCmdEntry_s chunk[32];
    std::memcpy(chunk, Queue()->entries + start, sizeof(gxCmdEntry_s) * static_cast<size_t>(n));
    if (!folded) {
        EmitFrameBuf(Gdx3dsStereo::RightTarget());
        EmitEye(1.0f);
        SubmitOpenList();
    }
    for (int i = 0; i < n; i++) {
        ReplayEntry(start + i, chunk[i]);
    }
    return true;
}

/* The left eye again (main target, steye = -1) and a fresh self-contained start for the next chunk. */
void ResumeLeftEye() {
    EmitFrameBuf(sMain);
    EmitEye(-1.0f);
    SubmitOpenList();
    sChunkStart = Queue()->numEntries;
    MarkBodySelfContained();
    const uint64_t ts = svcGetSystemTick();
    C3Dgdx_FlushState();
    sWin.syncTicks += svcGetSystemTick() - ts;
}

/* Whether a list closed now (queue index numEntries) plus the replay of [sChunkStart, that list]
 * fits with `tail` slots left, as ReplayPendingChunk(tail) will test it after the close: the room
 * test of a fold, made BEFORE its words are written so that a folded list is always replayed. */
bool FoldRoom(u32 tail) {
    const u32 closed = static_cast<u32>(Queue()->numEntries) + 1u;
    const u32 n = closed - static_cast<u32>(sChunkStart);
    return closed + n + tail <= static_cast<u32>(Queue()->maxEntries);
}

/* n60tail [native] stereo_xl_early: the left eye's display transfer, queued right after the left
 * eye's last list (whose split flushed the left render cache) instead of by C3D_FrameEnd after the
 * right eye; C3D_FrameEnd then transfers only the right target (main->used cleared) and still
 * requests the top swap (the right target is used, so needSwapTop and isTopStereo are set as
 * before). Same PPF operation, same source, same destination (gfxGetFramebuffer: no swap happens
 * before C3D_FrameEnd's own queue finishes). */
bool QueueLeftTransferEarly() {
    C3D_RenderTarget* right = Gdx3dsStereo::RightTarget();
    if (!sXlNow || sMain == nullptr || right == nullptr || !sMain->used || !right->used || !sMain->linked ||
        sMain->screen != GFX_TOP || right->screen != GFX_TOP || gdx3ds_replay_on) {
        return false;
    }
    C3D_FrameBufTransfer(&sMain->frameBuf, sMain->screen, sMain->side, sMain->transferFlags);
    sMain->used = false;
    sWin.xlEarly++;
    return true;
}

bool PhaseOn(int lever, unsigned abBit) {
    if (lever == 1) {
        return true;
    }
    return lever == 2 && ((sFrameNo >> abBit) & 1u) == 0u;
}

void LatchRows() {
    Gdx3dsStereo::ClassRow(GDX3DS_STEREO_SCENE, gdx3ds_stereo_rows[0]);
    Gdx3dsStereo::ClassRow(GDX3DS_STEREO_UI_ZERO_PARALLAX, gdx3ds_stereo_rows[1]);
}

double TicksToMs(uint64_t t) {
    return static_cast<double>(t) * 1000.0 / static_cast<double>(SYSCLOCK_ARM11);
}

void EmitWindow() {
    if (sWin.frames < kWindow) {
        return;
    }
    if (sWin.stereo > 0) {
        char msg[416];
        const double per = sWin.replayed > 0 ? static_cast<double>(sWin.replayed) : 1.0;
        std::snprintf(msg, sizeof(msg),
                      "[stereo] replay frames=%u stereo=%u replayed=%u refused=%u multiview=%u full=%u "
                      "lists/f=%.2f "
                      "fills/f=%.2f skip/f=%.2f swaps=%u chunks/f=%.2f chunkskip=%u noflush/f=%.2f gx/f=%.1f enter_us=%.1f "
                      "(sync %.1f) chunk_us=%.1f end_us=%.1f fold/f=%.2f eye0defer/f=%.2f xl/f=%.2f",
                      sWin.frames, sWin.stereo, sWin.replayed, sWin.refused, sWin.multiview, sWin.overflow,
                      sWin.lists / per, sWin.fills / per, sWin.skipped / per, sWin.swaps, sWin.chunks / per,
                      sWin.chunkSkip, sWin.unflushed / per, sWin.gxEntries / per, TicksToMs(sWin.enterTicks) * 1000.0 / per,
                      TicksToMs(sWin.syncTicks) * 1000.0 / per, TicksToMs(sWin.chunkTicks) * 1000.0 / per,
                      TicksToMs(sWin.endTicks) * 1000.0 / per, sWin.folds / per, sWin.eyeDefer / per,
                      sWin.xlEarly / per);
        Log(msg);
    }
    std::memset(&sWin, 0, sizeof(sWin));
}

} // namespace

namespace Gdx3dsStereo {

void ReplayInit(int stkLoc, int eyeLoc, C3D_RenderTarget* mainTarget) {
    sStkLoc = stkLoc;
    sEyeLoc = eyeLoc;
    sMain = mainTarget;
    aptHook(&sAptCookie, AptHook, nullptr);
    if (stkLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, stkLoc, 0.0f, 0.0f, 0.0f, 0.0f); /* citro3d owns stk from here */
    }
    if (!Enabled()) {
        sLever = 0;
        return;
    }
    sLever = 1;
    if (&gdx3ds_config_get_int != nullptr) {
        sLever = gdx3ds_config_get_int("native", "stereo_replay", 1) != 0 ? 1 : 0;
        sInterleave = gdx3ds_config_get_int("native", "stereo_interleave", 1) != 0 ? 1 : 0;
        sNoReflush = gdx3ds_config_get_int("native", "stereo_noreflush", 1) != 0 ? 1 : 0;
        const int fold = gdx3ds_config_get_int("native", "stereo_fold", 0);
        const int xl = gdx3ds_config_get_int("native", "stereo_xl_early", 0);
        sFold = (fold == 1 || fold == 2) ? fold : 0;
        sXlEarly = (xl == 1 || xl == 2) ? xl : 0;
    }
    if (stkLoc < 0 || eyeLoc < 0 || mainTarget == nullptr || RightTarget() == nullptr) {
        sLever = 0;
    }
    char msg[320];
    std::snprintf(msg, sizeof(msg),
                  "[stereo] replay init stereo_replay=%d stereo_interleave=%d stereo_noreflush=%d stereo_fold=%d "
                  "stereo_xl_early=%d stk=%d steye=%d (%s)",
                  sLever, sInterleave, sNoReflush, sFold, sXlEarly, stkLoc, eyeLoc,
                  sLever ? "right eye = the left's command lists re-submitted" : "per-draw loop");
    Log(msg);
}

void ReplayFrameBegin() {
    sFrameTicks = 0;
    sFrameChunks = 0;
    sInBody = false;
    sOtherBound = false;
    sNumSwaps = 0;
    sKickFolded = false;
    sFrameNo++;
    sFoldNow = PhaseOn(sFold, kFoldAbBit);
    sXlNow = PhaseOn(sXlEarly, kXlAbBit);
    gdx3ds_stereo_replay_frame = 0;
    /* steye = 0 here also when the previous frame deferred its epilogue (stereo_fold): this frame's
     * first list, before any draw of it. */
    if (sEyeZeroPending && sEyeLoc >= 0) {
        EmitEye(0.0f);
        sEyeZeroPending = false;
    }
    if (sLever > 0) {
        sWin.frames++;
        if (Active()) {
            sWin.stereo++;
        }
        EmitWindow();
    }
}

bool ReplayEnter() {
    if (sInBody) {
        return true;
    }
    if (sLever <= 0 || !Active()) {
        sReplayOkThisFrame = false;
        return false;
    }
    if (&gNumPlayers != nullptr && gNumPlayers > 1) { /* niview Part 2: §8 item 2, the outer-edge scissor */
        sWin.multiview++;
        if (!sMultiViewLogged) {
            sMultiViewLogged = true;
            Log("[stereo] replay refused: multi-view");
        }
        sReplayOkThisFrame = false;
        return false;
    }
    if (HoldFrame() || !QueueRoom(2u * kReserve + 8u)) {
        sWin.refused++;
        sReplayOkThisFrame = false;
        return false;
    }
    gdx3ds_skylate_before_enter(); /* n60tail: may the sky's run start at the body start? */
    const uint64_t t0 = svcGetSystemTick();
    /* The pre-body list ends with the left eye's own words: steye = -1, the main framebuffer. */
    EmitEye(-1.0f);
    EmitFrameBuf(sMain);
    SubmitOpenList();
    sBodyStart = Queue()->numEntries;
    sChunkStart = sBodyStart;
    gdx3ds_skylate_after_enter(sBodyStart);
    MarkBodySelfContained();
    const uint64_t ts = svcGetSystemTick();
    C3Dgdx_FlushState(); /* every state group and uniform now, at the body's first word */
    sWin.syncTicks += svcGetSystemTick() - ts;
    LatchRows();
    sInBody = true;
    sReplayOkThisFrame = true;
    gdx3ds_stereo_replay_frame = 1;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.enterTicks += spent;
    sFrameTicks += spent;
    return true;
}

bool ReplayActive() {
    return sInBody;
}

/* niview: read-only, cross-thread-safe (a single bool, written only by ReplayEnter() on the thread
 * that owns the C3D frame, read from anywhere -- never calls ReplayEnter() itself: see its banner
 * comment on why that matters). Sticky across frame boundaries (see sReplayOkThisFrame): a reader
 * racing ahead of the render thread in the tick-ahead pipe sees the latest decided outcome, which
 * may be the previous render frame's, not necessarily this exact one. A first attempt cleared this
 * to false at ReplayFrameBegin ("conservative until this frame's Enter runs") and that was wrong:
 * under the pipe the game-side reader runs ahead EVERY frame, so every category refused NRR every
 * frame and the stock interpreter's per-eye double-submit overflowed the GPU command buffer
 * (svcBreak in GPUCMD_Add, gated and caught; see docs/research/native/niview-progress.md). */
bool ReplayOkThisFrame() {
    return sReplayOkThisFrame;
}

void ReplayNoteBind(int fbId) {
    if (!sInBody) {
        return;
    }
    if (fbId != 0) {
        sOtherBound = true;
        return;
    }
    if (!sOtherBound) {
        C3Di_GetContext()->flags &= ~static_cast<u32>(C3DiF_FrameBuf); /* the eye's target is bound */
        return;
    }
    sOtherBound = false;
    if (sNumSwaps >= kMaxSwaps) {
        sWin.overflow++;
        return; /* the right eye's rest of the frame lands on the left target: counted, never seen so far */
    }
    SubmitOpenList();
    Swap& sw = sSwaps[sNumSwaps];
    sw.entry = Queue()->numEntries;
    EmitFrameBuf(sMain);
    C3Di_GetContext()->flags &= ~static_cast<u32>(C3DiF_FrameBuf);
    SubmitOpenList();
    EmitFrameBuf(RightTarget());
    DetachOpenList(&sw.buf, &sw.words);
    sNumSwaps++;
}

/* n60tail [native] stereo_fold: called by the [overlap] kick right before its split. When the kick
 * chunk will replay (ReplayKickChunk's tests, one entry ahead), the right prologue goes at the end
 * of the list the split closes instead of into a list of its own (-1 GX entry per kick). */
void ReplayKickPreSplit() {
    sKickFolded = false;
    if (!sInBody || sInterleave <= 0 || !sFoldNow || gdx3ds_nrr_handoff_open != 0 || gpuCmdBufOffset == 0) {
        return;
    }
    const u32 closed = static_cast<u32>(Queue()->numEntries) + 1u;
    if (closed + 2u * kReserve + 8u > static_cast<u32>(Queue()->maxEntries) || !FoldRoom(2u * kReserve)) {
        return; /* the unfolded path decides (and counts the skip) */
    }
    const uint64_t t0 = svcGetSystemTick();
    EmitFoldedPrologue(static_cast<int>(closed) - 1);
    sKickFolded = true;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.chunkTicks += spent;
    sFrameTicks += spent;
}

/* [overlap] kick: the chunk just submitted for the left eye goes to the right eye at once, so the GPU
 * draws both eyes of it while the CPU builds the next categories; the frame end then replays only
 * the last chunk. Never inside an NRR hand-off (its direct words believe GPU state that the next
 * chunk's fresh start would re-set from citro3d's context). */
void ReplayKickChunk() {
    if (!sInBody || sInterleave <= 0) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    if (sKickFolded) {
        /* The closing list already switched the GPU to the right eye: replay it (the room was
         * checked before folding) and always come back to the left eye. */
        sKickFolded = false;
        SubmitOpenList();
        if (ReplayPendingChunk(2u * kReserve, true)) {
            sWin.chunks++;
        } else {
            sWin.chunkSkip++;
        }
        ResumeLeftEye();
    } else if (gdx3ds_nrr_handoff_open != 0 || !QueueRoom(2u * kReserve + 8u)) {
        sWin.chunkSkip++;
        return;
    } else {
        SubmitOpenList();
        if (ReplayPendingChunk(2u * kReserve, false)) {
            ResumeLeftEye();
            sWin.chunks++;
        } else {
            sWin.chunkSkip++;
        }
    }
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.chunkTicks += spent;
    sFrameTicks += spent;
    sFrameChunks++;
}

int ReplayStkLoc() {
    return sStkLoc;
}

/* n60tail [native] sky_late: the next list starts self-contained (every citro3d group and uniform
 * re-emitted before its first draw, lazily), the framebuffer left as it is. */
void MarkStateSelfContained() {
    C3D_Context* ctx = C3Di_GetContext();
    ctx->flags |= kDirtyBody;
    C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    if (sStkLoc >= 0) {
        const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][sStkLoc];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sStkLoc, cur.x, cur.y, cur.z, cur.w);
    }
}

void ReplayFrameEnd() {
    if (!sInBody) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    sInBody = false;
    gdx3ds_stereo_replay_frame = 0;
    /* n60tail stereo_fold: the right prologue closes the body's last list (room checked first). */
    const bool folded = sFoldNow && gpuCmdBufOffset != 0 && FoldRoom(kReserve);
    if (folded) {
        EmitFoldedPrologue(static_cast<int>(Queue()->numEntries));
    }
    SubmitOpenList(); /* the body's last list */
    QueueLeftTransferEarly(); /* n60tail stereo_xl_early: the left eye is complete in VRAM now */
    if (!ReplayPendingChunk(kReserve, folded)) {
        sWin.overflow++;
        if (folded) { /* not reached (room checked before folding): leave no right-eye state behind */
            C3Di_GetContext()->flags |= C3DiF_FrameBuf;
            sEyeZeroPending = true;
        }
        return;
    }
    if (sFoldNow) {
        /* n60tail stereo_fold: steye = 0 goes into the next frame's first list (ReplayFrameBegin):
         * nothing between here and there runs the vertex shader (display transfers, the oracle's
         * readback copies), and a frame that never comes back here emits it the same way. */
        sEyeZeroPending = true;
        sWin.eyeDefer++;
    } else {
        EmitEye(0.0f);
        SubmitOpenList();
    }
    sWin.gxEntries += static_cast<unsigned>(Queue()->numEntries);
    /* The GPU now has the right target bound: citro3d re-binds its own at the next draw. */
    C3Di_GetContext()->flags |= C3DiF_FrameBuf;
    sWin.replayed++;
    const uint64_t spent = svcGetSystemTick() - t0;
    sWin.endTicks += spent;
    sFrameTicks += spent;
}

uint64_t FrameCpuTicks(unsigned* chunks) {
    *chunks = sFrameChunks;
    return sFrameTicks;
}

} // namespace Gdx3dsStereo

extern "C" void gdx3ds_stereo_kick_chunk(void) {
    Gdx3dsStereo::ReplayKickChunk();
}

extern "C" void gdx3ds_stereo_kick_pre_split(void) {
    Gdx3dsStereo::ReplayKickPreSplit();
}

/* n60tail: the stereo_fold / stereo_xl_early phase of the frame being built (bit 0 fold, bit 1
 * xl_early), for the [gputl] receipt's tp= (each window holds one phase). */
extern "C" int gdx3ds_stereo_tail_phase(void) {
    return (sFoldNow ? 1 : 0) | (sXlNow ? 2 : 0);
}

/* The pace module's 3D rule ([debug] pace_swapgate_3d) asks this at each fresh begin: the right-eye
 * pass state latched at the latest StartFrame. (Round 3 declared it weak and nothing defined it, so
 * pace_swapgate_3d always fell back to pace_swapgate.) */
extern "C" int gdx3ds_stereo_active(void) {
    return Gdx3dsStereo::Active() ? 1 : 0;
}

/* [native] ni_race_mesh (nirace-progress.md 4R7): same reason as gdx3ds_stereo_active above --
 * gdx_nirace.cpp also builds as a host test TU (gdx_interp_host_tests links neither this file
 * nor gdx3ds_stereo.h's <3ds.h>/<citro3d.h> dependency), so it reads this weakly. */
extern "C" int gdx3ds_stereo_replay_ok_this_frame(void) {
    return Gdx3dsStereo::ReplayOkThisFrame() ? 1 : 0;
}

extern "C" unsigned long long gdx3ds_stereo_frame_cpu_ticks(unsigned* chunks) {
    return Gdx3dsStereo::FrameCpuTicks(chunks);
}

/* niview: "may an NRR category commit to drawing this section natively" -- the latest decided
 * replay outcome, not just the static levers. Active() already folds in [stereo] enabled and the
 * slider. IMPORTANT: this must NOT call ReplayEnter() itself -- ReplayEnter() is stateful and emits
 * GPU words (EmitEye/EmitFrameBuf/SubmitOpenList/C3Dgdx_FlushState) and the file banner is explicit
 * that "everything runs on the thread that owns the C3D frame"; gdx3ds_stereo_nrr_ok() is called from
 * category stubs on the GAME side (gdx3ds_nrr_frame_build), a different thread under the tick-ahead
 * pipe. Calling ReplayEnter() from there was tried and crashed (GPUCMD_Add, svcBreak) -- a real,
 * gated finding, not theoretical. So this reads ReplayOkThisFrame(), sticky across frame boundaries
 * (a strict "clear it every ReplayFrameBegin, true only once this exact frame's Enter ran" was also
 * tried and ALSO gated-and-caught: the game thread runs ahead of the render thread every frame under
 * the pipe, so that version forced every category to refuse NRR every single frame, every category,
 * and the stock interpreter's per-eye double-submit overflowed the GPU command buffer the same way).
 * Closes the latent bug where a category CONSUMES a section (skips Background_Draw/the stock path)
 * on the strength of the static lever alone, then the render side discovers -- too late -- that this
 * particular frame's replay didn't happen, leaving that section undrawn for one eye (the sky
 * black-screen class named in the mission brief) -- at the cost of the flag being a frame or so
 * stale under the pipe, same staleness as this codebase's other cross-thread signals. */
extern "C" int gdx3ds_stereo_nrr_ok(void) {
    if (!Gdx3dsStereo::Active()) {
        return 1;
    }
    return Gdx3dsStereo::ReplayOkThisFrame() ? 1 : 0;
}
