/* gdx3ds_sky_capture.c -- [native] sky_retain / skyclock (c5sky-progress.md): the game side of
 * the sky/background path.
 *
 * decomp-native-sky-capture.patch brackets Background_Draw (every caller: the race's 1P/2P/4P
 * cameras, records.c, the ending, the course editor) with
 *
 *   GDX_SKYSEC(HEAD) ... GDX_SKYSEC(GRAD) ... GDX_SKYSEC(SPRITES) ... GDX_SKYSEC(END)
 *   gfx = gdxsky_capture(regionStart, gfx, cameraIndex, &identity);
 *
 * Each GDX_SKYSEC writes one G_NOOP p=0x65 SECTION marker (kind 0, index = section id). The
 * owned walker hands it to Interp::SkyMarker, which charges the census ([skycnt], lever
 * skyclock) and tells the replay which section it is in.
 *
 * With [native] sky_retain != 0 the capture then COMPACTS what Background_Draw wrote, in place
 * (gdx3ds_sky_record.h): each maximal run of recordable commands moves into the per-GfxPool
 * record behind ONE G_NOOP p=0x65 RUN marker, RDP syncs are dropped (no-ops on this port) and
 * everything else stays where it was. The output is never longer than the input, so the
 * compaction writes at or behind the read cursor. A region with control flow the replay cannot
 * express (G_ENDDL, a branching G_DL, G_BRANCH_Z, G_LOAD_UCODE, a stray RDPHALF, a G_DL into the
 * region) or that would overflow the record is left EXACTLY as written and counted.
 *
 * Background_Draw itself runs unchanged -- the sprite matrices, the night palette flash, the
 * star projection are all game logic and all still happen. With every lever off nothing is
 * written: gdxsky_armed() is 0 and the list is byte-identical to the unpatched build.
 *
 * Everything external is weak: this TU also links into the DL-test harness. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_native_cfg.h"
#include "gdx3ds_hotcold.h"
#include "gdx3ds_sky_record.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern uint32_t gGameFrameCount __attribute__((weak));
extern void* gGfxPool __attribute__((weak));

/* F3DEX2 opcodes (port/3ds/gfx/n64/gdx_gbi.h spells the same values as OP_*). */
enum {
    SKY_OP_NOOP = 0x00,
    SKY_OP_VTX = 0x01,
    SKY_OP_BRANCH_Z = 0x04,
    SKY_OP_TRI1 = 0x05,
    SKY_OP_TRI2 = 0x06,
    SKY_OP_QUAD = 0x07,
    SKY_OP_EXTRAGEOMETRYMODE = 0x3A,
    SKY_OP_TEXTURE = 0xD7,
    SKY_OP_GEOMETRYMODE = 0xD9,
    SKY_OP_MTX = 0xDA,
    SKY_OP_LOAD_UCODE = 0xDD,
    SKY_OP_DL = 0xDE,
    SKY_OP_ENDDL = 0xDF,
    SKY_OP_RDPHALF_1 = 0xE1,
    SKY_OP_SETOTHERMODE_L = 0xE2,
    SKY_OP_SETOTHERMODE_H = 0xE3,
    SKY_OP_TEXRECT = 0xE4,
    SKY_OP_TEXRECTFLIP = 0xE5,
    SKY_OP_RDPLOADSYNC = 0xE6,
    SKY_OP_RDPPIPESYNC = 0xE7,
    SKY_OP_RDPTILESYNC = 0xE8,
    SKY_OP_RDPFULLSYNC = 0xE9,
    SKY_OP_SETKEYGB = 0xEA,
    SKY_OP_SETKEYR = 0xEB,
    SKY_OP_SETCONVERT = 0xEC,
    SKY_OP_SETSCISSOR = 0xED,
    SKY_OP_SETPRIMDEPTH = 0xEE,
    SKY_OP_RDPSETOTHERMODE = 0xEF,
    SKY_OP_LOADTLUT = 0xF0,
    SKY_OP_RDPHALF_2 = 0xF1,
    SKY_OP_SETTILESIZE = 0xF2,
    SKY_OP_LOADBLOCK = 0xF3,
    SKY_OP_LOADTILE = 0xF4,
    SKY_OP_SETTILE = 0xF5,
    SKY_OP_FILLRECT = 0xF6,
    SKY_OP_SETFILLCOLOR = 0xF7,
    SKY_OP_SETFOGCOLOR = 0xF8,
    SKY_OP_SETBLENDCOLOR = 0xF9,
    SKY_OP_SETPRIMCOLOR = 0xFA,
    SKY_OP_SETENVCOLOR = 0xFB,
    SKY_OP_SETCOMBINE = 0xFC,
    SKY_OP_SETTIMG = 0xFD,
};

/* How a word takes part in the capture. */
enum SkyClass {
    SKY_KEEP = 0,   /* stays in the main list (a run boundary) */
    SKY_RECORD = 1, /* one recordable word */
    SKY_RECT3 = 2,  /* TEXRECT/TEXRECTFLIP: recordable with its two RDPHALF trailers */
    SKY_DROP = 3,   /* RDP sync: a no-op on this port, dropped */
    SKY_ABORT = 4,  /* control flow the replay cannot express: leave the region stock */
};

enum SkyAbort {
    SKY_AB_FLOW = 0, /* ENDDL / BRANCH_Z / LOAD_UCODE / stray RDPHALF */
    SKY_AB_DLBRANCH, /* G_DL without push (a branch) */
    SKY_AB_DLSELF,   /* G_DL into the region itself */
    SKY_AB_RECT,     /* TEXRECT without its trailers */
    SKY_AB_FULL,     /* record capacity */
    SKY_AB_COUNT
};

/* -1 unread; else the latched levers. */
static int sRetain = -1;
static int sClock = 0;

static GdxSkyRecord sRec[2];
static const void* sPoolPtr[2];
static int sPoolNext;

/* Identity: the last frame's texture set and the generation it earned. */
static GdxSkyIdentity sLastIdentity;
static uint32_t sGeneration = 1;
static int sHaveIdentity;

/* Receipt window (64 captured frames). */
typedef struct {
    uint32_t frames, regions, scanned, recorded, dropped, runs, kept, generations;
    uint32_t aborts[SKY_AB_COUNT];
} SkyWindow;
static SkyWindow sWin;

static void SkyLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
GDX_COLD static void SkyLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    if (&gdx_dbg_logf == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

static int SkyClampLevel(int v, int max) {
    if (v < 0 || v > max) {
        return 0;
    }
    return v;
}

/* Both levers need the owned walker (the markers are its), and neither may run under the
 * interp_verify oracle, whose LUS pass ignores p=0x65 and would draw nothing for a run. */
static const char* SkyRefusal(void) {
    if (gdx3ds_config_get_int("native", "interp", 0) == 0) {
        return "needs [native] interp=1";
    }
    if (gdx3ds_native_verify_forces_fold_off()) {
        return "off under interp_verify";
    }
    return NULL;
}

static void SkyLatch(void) {
    const char* refusal;
    int retain;
    int clock;
    if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
        return;
    }
    retain = SkyClampLevel(gdx3ds_native_raw("sky_retain", 0), 3);
    clock = SkyClampLevel(gdx3ds_native_raw("skyclock", 0), 2);
    refusal = (retain != 0 || clock != 0) ? SkyRefusal() : NULL;
    if (refusal != NULL) {
        SkyLog("[sky] init sky_retain=%d skyclock=%d: %s -> off", retain, clock, refusal);
        retain = 0;
        clock = 0;
    } else {
        SkyLog("[sky] init sky_retain=%d skyclock=%d (0 off, 1 retain, 2 verify, 3 retain+PICA clip: not exact) words=%u runs=%u", retain, clock,
               (unsigned)GDXSKY_MAX_WORDS, (unsigned)GDXSKY_MAX_RUNS);
    }
    sClock = clock;
    sRetain = retain;
}

int gdxsky_armed(void) {
    if (sRetain < 0) {
        SkyLatch();
        if (sRetain < 0) {
            return 0;
        }
    }
    return sRetain != 0 || sClock != 0;
}

static void SkyWriteMarker(GdxSkyWord* out, uint32_t kind, uint32_t index, const void* ptr) {
    out->w0 = ((uint32_t)SKY_OP_NOOP << 24) | ((uint32_t)GDXSKY_MARKER_P << 16) | ((kind & 0xFu) << 12) |
              (index & 0xFFFu);
    out->pad = 0;
    out->w1 = (uint32_t)(uintptr_t)ptr;
    out->w1hi = ptr != NULL ? GDXSKY_HOST_TAG : 0u;
}

void* gdxsky_section(void* gfx, int section) {
    GdxSkyWord* out = (GdxSkyWord*)gfx;
    SkyWriteMarker(out, GDXSKY_MK_SECTION, (uint32_t)section, NULL);
    return out + 1;
}

/* ---- identity / generation -------------------------------------------------------------- */

static void SkyNoteIdentity(const GdxSkyIdentity* id) {
    if (id == NULL) {
        return;
    }
    if (!sHaveIdentity || memcmp(&sLastIdentity, id, sizeof(*id)) != 0) {
        if (sHaveIdentity) {
            sGeneration++;
            sWin.generations++;
        }
        sLastIdentity = *id;
        sHaveIdentity = 1;
    }
}

/* ---- the receipt ------------------------------------------------------------------------- */

static void SkyMaybeEmit(void) {
    double inv;
    if (sWin.frames < 64) {
        return;
    }
    inv = 1.0 / (double)sWin.frames;
    SkyLog("[sky] cap frames=%u regions=%.2f scanned=%.1f rec=%.1f drop=%.1f kept=%.1f runs=%.2f gen=%u(+%u) "
           "abort flow=%u dlbr=%u dlself=%u rect=%u full=%u",
           sWin.frames, sWin.regions * inv, sWin.scanned * inv, sWin.recorded * inv, sWin.dropped * inv,
           sWin.kept * inv, sWin.runs * inv, sGeneration, sWin.generations, sWin.aborts[SKY_AB_FLOW],
           sWin.aborts[SKY_AB_DLBRANCH], sWin.aborts[SKY_AB_DLSELF], sWin.aborts[SKY_AB_RECT],
           sWin.aborts[SKY_AB_FULL]);
    memset(&sWin, 0, sizeof(sWin));
}

static GdxSkyRecord* SkyRecordFor(const void* pool, uint32_t frame) {
    int slot = -1;
    int i;
    GdxSkyRecord* rec;
    for (i = 0; i < 2; i++) {
        if (sPoolPtr[i] == pool) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = sPoolNext;
        sPoolNext ^= 1;
        sPoolPtr[slot] = pool;
        sRec[slot].magic = 0;
    }
    rec = &sRec[slot];
    if (rec->magic != GDXSKY_MAGIC || rec->frame != frame) {
        SkyMaybeEmit();
        rec->magic = GDXSKY_MAGIC;
        rec->frame = frame;
        rec->nWords = 0;
        rec->nRuns = 0;
        rec->badRecord = 0;
        sWin.frames++;
    }
    rec->generation = sGeneration;
    return rec;
}

/* ---- compaction -------------------------------------------------------------------------- */

/* The class of every opcode, as a table (a byte load per word, not a switch). */
static const uint8_t kSkyClass[256] = {
    [SKY_OP_SETTIMG] = SKY_RECORD,        [SKY_OP_SETTILE] = SKY_RECORD,
    [SKY_OP_SETTILESIZE] = SKY_RECORD,    [SKY_OP_LOADBLOCK] = SKY_RECORD,
    [SKY_OP_LOADTILE] = SKY_RECORD,       [SKY_OP_LOADTLUT] = SKY_RECORD,
    [SKY_OP_FILLRECT] = SKY_RECORD,       [SKY_OP_SETFILLCOLOR] = SKY_RECORD,
    [SKY_OP_SETFOGCOLOR] = SKY_RECORD,    [SKY_OP_SETBLENDCOLOR] = SKY_RECORD,
    [SKY_OP_SETPRIMCOLOR] = SKY_RECORD,   [SKY_OP_SETENVCOLOR] = SKY_RECORD,
    [SKY_OP_SETCOMBINE] = SKY_RECORD,     [SKY_OP_SETOTHERMODE_H] = SKY_RECORD,
    [SKY_OP_SETOTHERMODE_L] = SKY_RECORD, [SKY_OP_RDPSETOTHERMODE] = SKY_RECORD,
    [SKY_OP_SETSCISSOR] = SKY_RECORD,     [SKY_OP_SETPRIMDEPTH] = SKY_RECORD,
    [SKY_OP_SETKEYR] = SKY_RECORD,        [SKY_OP_SETKEYGB] = SKY_RECORD,
    [SKY_OP_SETCONVERT] = SKY_RECORD,     [SKY_OP_GEOMETRYMODE] = SKY_RECORD,
    [SKY_OP_EXTRAGEOMETRYMODE] = SKY_RECORD, [SKY_OP_TEXTURE] = SKY_RECORD,
    [SKY_OP_TRI1] = SKY_RECORD,           [SKY_OP_TRI2] = SKY_RECORD,
    [SKY_OP_QUAD] = SKY_RECORD,
    [SKY_OP_MTX] = SKY_RECORD,            [SKY_OP_VTX] = SKY_RECORD,
    [SKY_OP_TEXRECT] = SKY_RECT3,         [SKY_OP_TEXRECTFLIP] = SKY_RECT3,
    [SKY_OP_RDPLOADSYNC] = SKY_DROP,      [SKY_OP_RDPPIPESYNC] = SKY_DROP,
    [SKY_OP_RDPTILESYNC] = SKY_DROP,      [SKY_OP_RDPFULLSYNC] = SKY_DROP,
    [SKY_OP_ENDDL] = SKY_ABORT,           [SKY_OP_BRANCH_Z] = SKY_ABORT,
    [SKY_OP_LOAD_UCODE] = SKY_ABORT,      [SKY_OP_RDPHALF_1] = SKY_ABORT,
    [SKY_OP_RDPHALF_2] = SKY_ABORT,
};

static uint8_t SkyOp(const GdxSkyWord* w) {
    return (uint8_t)(w->w0 >> 24);
}

/* G_MTX / G_VTX resolve their operand with the containing list as a hint (SpMatrix/SpVertex).
 * The replay passes the walker's list -- the list this word was captured from, where its run
 * marker now stands -- so they are recordable (the HUD capture, whose replay passes the record,
 * keeps them). The hint matters only to the bridge's last-resort source window anyway: a host
 * pointer resolves to itself, and Background_Draw's D_1000000 / D_2000000 aliases resolve
 * through the segment and alias tables. */
static uint8_t SkyClassOf(const GdxSkyWord* w) {
    return kSkyClass[SkyOp(w)];
}

/* The section a kept section marker opens, or -1 for any other word. */
static int SkySectionOf(const GdxSkyWord* w) {
    if (SkyOp(w) != SKY_OP_NOOP || ((w->w0 >> 16) & 0xFFu) != GDXSKY_MARKER_P ||
        ((w->w0 >> 12) & 0xFu) != GDXSKY_MK_SECTION) {
        return -1;
    }
    return (int)(w->w0 & 0xFFFu);
}

/* A G_DL whose host-tagged target lies inside the region would read words the compaction
 * moves. (A segmented target cannot point into the region: it is the GfxPool's master list.) */
static int SkyDlIntoRegion(const GdxSkyWord* w, const GdxSkyWord* begin, const GdxSkyWord* end) {
    const uintptr_t target = (uintptr_t)w->w1;
    if (w->w1hi != GDXSKY_HOST_TAG) {
        return 0;
    }
    return target >= (uintptr_t)begin && target < (uintptr_t)end;
}

/* Pass 1: can the region be compacted? Returns 0, or -(reason + 1) when it must stay stock. */
static int SkyValidate(const GdxSkyWord* begin, const GdxSkyWord* end) {
    const GdxSkyWord* w = begin;
    while (w < end) {
        const uint8_t op = SkyOp(w);
        const uint8_t cls = SkyClassOf(w);
        if (cls == SKY_RECT3) {
            if (w + 2 >= end || SkyOp(w + 1) != SKY_OP_RDPHALF_1 || SkyOp(w + 2) != SKY_OP_RDPHALF_2) {
                return -SKY_AB_RECT - 1;
            }
            w += 3;
            continue;
        }
        if (cls == SKY_ABORT) {
            return -SKY_AB_FLOW - 1;
        }
        if (op == SKY_OP_DL) {
            if (((w->w0 >> 16) & 1u) != 0) {
                return -SKY_AB_DLBRANCH - 1;
            }
            if (SkyDlIntoRegion(w, begin, end)) {
                return -SKY_AB_DLSELF - 1;
            }
        }
        w++;
    }
    return 0;
}

typedef struct {
    GdxSkyRecord* rec;
    GdxSkyWord* out;
    uint32_t runStart;
    int runOpen;
    int section;
    int camera;
} SkyCompactor;

static void SkyOpenRun(SkyCompactor* c) {
    if (!c->runOpen) {
        c->runStart = c->rec->nWords;
        c->runOpen = 1;
    }
}

static void SkyCloseRun(SkyCompactor* c) {
    GdxSkyRecord* rec = c->rec;
    GdxSkyRun* run;
    if (!c->runOpen) {
        return;
    }
    run = &rec->runs[rec->nRuns];
    run->start = (uint16_t)c->runStart;
    run->end = (uint16_t)rec->nWords;
    run->section = (uint8_t)c->section;
    run->camera = (uint8_t)c->camera;
    run->mode = (uint8_t)sRetain;
    run->pad = 0;
    SkyWriteMarker(c->out, GDXSKY_MK_RUN, rec->nRuns, rec);
    rec->nRuns++;
    c->out++;
    c->runOpen = 0;
    sWin.runs++;
}

static void SkyRecordWords(SkyCompactor* c, GdxSkyWord** in, uint32_t count) {
    uint32_t i;
    SkyOpenRun(c);
    for (i = 0; i < count; i++) {
        c->rec->words[c->rec->nWords++] = **in;
        (*in)++;
    }
}

static void SkyKeepWord(SkyCompactor* c, GdxSkyWord** in) {
    const int section = SkySectionOf(*in);
    SkyCloseRun(c);
    if (section >= 0) {
        c->section = section;
    }
    *c->out++ = **in;
    (*in)++;
}

/* Pass 2: the in-place compaction (the region validated). Returns the new end. */
static GdxSkyWord* SkyCompact(GdxSkyRecord* rec, GdxSkyWord* begin, GdxSkyWord* end, int camera) {
    SkyCompactor c;
    GdxSkyWord* in = begin;
    c.rec = rec;
    c.out = begin;
    c.runStart = 0;
    c.runOpen = 0;
    c.section = GDXSKY_SEC_NONE;
    c.camera = camera;
    while (in < end) {
        const uint8_t cls = SkyClassOf(in);
        if (cls == SKY_RECORD) {
            SkyRecordWords(&c, &in, 1);
        } else if (cls == SKY_RECT3) {
            SkyRecordWords(&c, &in, 3);
        } else if (cls == SKY_DROP) {
            in++;
        } else {
            SkyKeepWord(&c, &in);
        }
    }
    SkyCloseRun(&c);
    return c.out;
}

void* gdxsky_capture(void* regionStart, void* regionEnd, int cameraIndex, const GdxSkyIdentity* identity) {
    GdxSkyWord* begin = (GdxSkyWord*)regionStart;
    GdxSkyWord* end = (GdxSkyWord*)regionEnd;
    GdxSkyRecord* rec;
    uint32_t len;
    uint32_t words0;
    uint32_t runs0;
    GdxSkyWord* out;
    int v;
    if (sRetain <= 0 || begin == NULL || end <= begin || &gGameFrameCount == NULL) {
        return regionEnd;
    }
    SkyNoteIdentity(identity);
    rec = SkyRecordFor(&gGfxPool != NULL ? gGfxPool : NULL, gGameFrameCount);
    len = (uint32_t)(end - begin);
    sWin.regions++;
    sWin.scanned += len;
    /* Upper bounds: every recorded word came from the region, and runs are separated by at
     * least one kept word, so a region of len words yields at most (len + 1) / 2 runs. */
    if (rec->nWords + len > GDXSKY_MAX_WORDS || rec->nRuns + (len + 1u) / 2u > GDXSKY_MAX_RUNS) {
        sWin.aborts[SKY_AB_FULL]++;
        return regionEnd;
    }
    v = SkyValidate(begin, end);
    if (v < 0) {
        sWin.aborts[-v - 1]++;
        return regionEnd;
    }
    words0 = rec->nWords;
    runs0 = rec->nRuns;
    out = SkyCompact(rec, begin, end, cameraIndex);
    sWin.recorded += rec->nWords - words0;
    sWin.kept += (uint32_t)(out - begin) - (rec->nRuns - runs0);
    sWin.dropped += len - (rec->nWords - words0) - ((uint32_t)(out - begin) - (rec->nRuns - runs0));
    return out;
}
