/* port/3ds/gfx/n64/gdx_machsplit.cpp -- [debug] machsplit: what the [c5cat] MACH category is
 * spent on (c5mach2, docs/research/native/c5mach2-progress.md).
 *
 * The MACH category is the owned walk between Racer_Draw's MACH marker (after the shadow
 * section) and its FX marker, minus the native mesh draw (the MESH bucket). Racer_Draw emits
 * there, all at one list depth: the highlight setup list, per highlighted machine G_MTX +
 * SETBLENDCOLOR + body, the body setup list and lights, then per drawn machine
 *     G_MTX  G_DL D_800CDD38[slot]  G_SETENVCOLOR  G_DL D_800CDDB0[slot*6+lod-1]
 * (the far speck D_7045098 at LOD 6), then crashed machines and ghosts. Every command at that
 * depth opens a class; the time until the next one (the lists it pushed included) is charged
 * to it, less the native mesh draw time that elapsed inside (so the classes sum to `mach`).
 *
 * Diagnostic only: a separate walk loop carries the one clock read per section command, and the
 * shipping loop is untouched. Host-compilable (the mesh clock is injected). */
#include "gdx_interp.h"

#include <stdio.h>
#include <string.h>

#include "gdx_log.h"

extern "C" long long gdx_ninterp_now(void);

namespace gdxn {

namespace {

enum MsClass {
    MS_MTX = 0,
    MS_TEXWALK,  /* a texture-set list the walker ran */
    MS_TEXSKIP,  /* a texture-set list machines_texskip consumed */
    MS_BODY,     /* a registered body (native mesh; its draw time is subtracted) */
    MS_HILITE,   /* a body right after SETBLENDCOLOR: the attack-highlight pass */
    MS_SPECK,    /* the LOD-6 far speck, walked by the interpreter */
    MS_SETUPDL,  /* a segment-addressed list (the setup lists D_303A7D8 / D_303A6A8 / crash) */
    MS_OTHERDL,  /* a host list that is none of the above (crashed-machine lists) */
    MS_STATE,    /* everything else: blend/fog colours, lights, lookat, DMA, syncs */
    MS_ENV,      /* SETENVCOLOR: where the previous speck's batch is drained (b= column) */
};

const char* const kMsName[] = { "mtx", "texw", "texs", "body", "hili", "speck", "setup", "odl", "state", "env" };

constexpr uint32_t kMsReceiptTasks = 64;
constexpr uint8_t kCatMach = 5; /* GDX3DS_C5CAT_MACH */

#ifndef CPU_TICKS_PER_MSEC
constexpr double kTicksPerMs = 268111.856;
#else
constexpr double kTicksPerMs = CPU_TICKS_PER_MSEC;
#endif

const void* HostTarget(const Cmd& c) {
    if (c.w1hi != kHostTag) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(c.w1));
}

} // namespace

void Interp::MachSplitSet(int on, uint64_t (*meshTicks)(void)) {
    mMsOn = (on != 0 && meshTicks != nullptr && mTexHooks.setSlot != nullptr) ? 1 : 0;
    mMsMeshTicks = meshTicks;
    memset(&mMsWin, 0, sizeof(mMsWin));
    GDXN_LOG("[machsplit] init machsplit=%d (diagnostic: splits [c5cat] mach by section command)", mMsOn);
}

/* The class a section command opens, read before it executes. */
void Interp::MachSplitOpen(const Cmd& c) {
    const uint8_t op = Opcode(c.w0);
    int cls = MS_STATE;
    if (op == OP_SETENVCOLOR) {
        cls = MS_ENV;
    } else if (op == OP_MTX) {
        cls = MS_MTX;
    } else if (op == OP_DL) {
        const void* t = HostTarget(c);
        if (t == nullptr) {
            cls = MS_SETUPDL;
        } else if (mTexHooks.setSlot(t) >= 0) {
            cls = MS_TEXWALK; /* re-classed to MS_TEXSKIP at close if the skip took it */
        } else if (mTexHooks.isSpeck(t) != 0) {
            cls = MS_SPECK;
        } else if (mTexHooks.isBody(t) != 0) {
            cls = mMsAfterBlend ? MS_HILITE : MS_BODY;
        } else {
            cls = MS_OTHERDL;
        }
    }
    mMsAfterBlend = (op == OP_SETBLENDCOLOR) || (mMsAfterBlend && op == OP_RDPPIPESYNC);
    /* The operand resolution the handler is about to pay, timed once more on the side (the
     * resolvers are pure lookups; this doubles their cost in a machsplit run, never elsewhere). */
    if (op == OP_DL || op == OP_MTX) {
        const uint64_t r0 = static_cast<uint64_t>(gdx_ninterp_now());
        if (op == OP_DL) {
            ListRef sub;
            (void)mSrc.ResolveList(c, sub);
        } else {
            const Frame& f = mStack[mSp - 1];
            (void)mSrc.ResolveData(c, 64, true, reinterpret_cast<uintptr_t>(f.list.base));
        }
        mMsWin.resolveTicks[cls] += static_cast<uint64_t>(gdx_ninterp_now()) - r0;
    }
    mMsClass = cls;
    mMsT0 = static_cast<uint64_t>(gdx_ninterp_now());
    mMsMesh0 = mMsMeshTicks();
    mMsCmds0 = mStats.cmds;
    mMsLoads0 = mStats.tmemLoads;
    mMsBatches0 = mStats.batches;
    mMsSkips0 = mTexSkipStats.skips;
}

void Interp::MachSplitClose() {
    if (mMsClass < 0) {
        return;
    }
    int cls = mMsClass;
    if (cls == MS_TEXWALK && mTexSkipStats.skips != mMsSkips0) {
        cls = MS_TEXSKIP;
    }
    const uint64_t gross = static_cast<uint64_t>(gdx_ninterp_now()) - mMsT0;
    const uint64_t mesh = mMsMeshTicks() - mMsMesh0;
    mMsWin.ticks[cls] += mesh < gross ? gross - mesh : 0;
    mMsWin.meshTicks[cls] += mesh;
    mMsWin.calls[cls]++;
    mMsWin.cmds[cls] += mStats.cmds - mMsCmds0;
    mMsWin.loads[cls] += mStats.tmemLoads - mMsLoads0;
    mMsWin.batches[cls] += mStats.batches - mMsBatches0;
    mMsClass = -1;
}

/* A [c5cat] scene marker (G_NOOP p=0x61) at the current depth: the section it opens. */
void Interp::MachSplitMarker(uint8_t cat) {
    MachSplitClose();
    mMsCat = cat;
    mMsDepth = mSp;
    mMsAfterBlend = false;
}

/* Walk with the section clock: the shipping loop plus one depth test per command. */
void Interp::WalkMachSplit() {
    while (mSp > 0) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            mStats.runaway++;
            mSp--;
            continue;
        }
        Cmd c = ReadCmd(f.list, f.idx);
        f.idx++;
        mStats.op[Opcode(c.w0)]++;
        mStats.cmds++;
        size_t f3dXfer = 0;
        if (f.list.isF3d && !RemapF3d(c, f.list, &f3dXfer)) {
            mStats.droppedOps++;
            continue;
        }
        if (mVerifyActive) {
            mBackend->VerifyNoteCmd(c.w0, c.w1);
        }
        const bool isMarker = Opcode(c.w0) == OP_NOOP && ((c.w0 >> 16) & 0xFFu) == 0x61u;
        if (mMsCat == kCatMach && mSp == mMsDepth && !isMarker) {
            MachSplitClose();
            MachSplitOpen(c);
        }
        if (isMarker) {
            MachSplitMarker(static_cast<uint8_t>(c.w0 & 0xFFu));
        }
        const bool subOp = mMsCat == kCatMach && mSp > mMsDepth && mMsClass == MS_SPECK;
        const uint64_t tSub = subOp ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        const int r = Exec(c, f, f3dXfer);
        if (subOp) {
            const uint8_t op = Opcode(c.w0);
            const int g = op == OP_VTX ? 0 : ((op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD) ? 1 : 2);
            mMsWin.subTicks[g] += static_cast<uint64_t>(gdx_ninterp_now()) - tSub;
            mMsWin.subCalls[g]++;
        }
        if (r == 1) {
            mSp--;
            if (mMsCat == kCatMach && mSp < mMsDepth) {
                MachSplitClose(); /* the section's own list ended */
                mMsCat = 0;
            }
        }
    }
}

void Interp::MachSplitTaskEnd() {
    if (mMsOn == 0) {
        return;
    }
    MachSplitClose();
    mMsCat = 0;
    mMsWin.tasks++;
    if (mMsWin.tasks >= kMsReceiptTasks) {
        MachSplitReceipt();
        memset(&mMsWin, 0, sizeof(mMsWin));
    }
}

/* [machsplit] every 64 tasks, per task (one task = one rendered frame):
 *   [machsplit] tasks=64 us <class>=<us>/<calls> ... | tot=<us> mesh=<us>
 *   [machsplit] work <class>=c<cmds>/l<loads>/b<batches> ...                                    */
void Interp::MachSplitReceipt() {
    const double inv = 1.0 / static_cast<double>(mMsWin.tasks);
    const double usPerTick = 1000.0 / kTicksPerMs;
    char buf[300];
    uint64_t tot = 0, mesh = 0;
    int n = snprintf(buf, sizeof(buf), "[machsplit] tasks=%u us", mMsWin.tasks);
    for (int k = 0; k < kMsClasses && n > 0 && n < static_cast<int>(sizeof(buf)); k++) {
        tot += mMsWin.ticks[k];
        mesh += mMsWin.meshTicks[k];
        n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s=%.0f/%.1f", kMsName[k],
                      static_cast<double>(mMsWin.ticks[k]) * usPerTick * inv, mMsWin.calls[k] * inv);
    }
    if (n > 0 && n < static_cast<int>(sizeof(buf))) {
        snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " | tot=%.0f mesh=%.0f",
                 static_cast<double>(tot) * usPerTick * inv, static_cast<double>(mesh) * usPerTick * inv);
    }
    GDXN_LOG("%s", buf);
    n = snprintf(buf, sizeof(buf), "[machsplit] work");
    for (int k = 0; k < kMsClasses && n > 0 && n < static_cast<int>(sizeof(buf)); k++) {
        n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s=c%.1f/l%.1f/b%.1f", kMsName[k],
                      mMsWin.cmds[k] * inv, mMsWin.loads[k] * inv, mMsWin.batches[k] * inv);
    }
    GDXN_LOG("%s", buf);
    n = snprintf(buf, sizeof(buf), "[machsplit] resolve(us, side-timed)");
    for (int k = 0; k < kMsClasses && n > 0 && n < static_cast<int>(sizeof(buf)); k++) {
        n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s=%.0f", kMsName[k],
                      static_cast<double>(mMsWin.resolveTicks[k]) * usPerTick * inv);
    }
    GDXN_LOG("%s | texskip=%.0fus/%.1f plan=%.0fus/%.1f", buf,
             static_cast<double>(mMsWin.texskipTicks) * usPerTick * inv, mMsWin.texskipCalls * inv,
             static_cast<double>(mMsWin.planTicks) * usPerTick * inv, mMsWin.plans * inv);
    GDXN_LOG("[machsplit] speck ops vtx=%.0fus/%.1f tri=%.0fus/%.1f other=%.0fus/%.1f (inside the speck lists)",
             static_cast<double>(mMsWin.subTicks[0]) * usPerTick * inv, mMsWin.subCalls[0] * inv,
             static_cast<double>(mMsWin.subTicks[1]) * usPerTick * inv, mMsWin.subCalls[1] * inv,
             static_cast<double>(mMsWin.subTicks[2]) * usPerTick * inv, mMsWin.subCalls[2] * inv);
}

} // namespace gdxn
