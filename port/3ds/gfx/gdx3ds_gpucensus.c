/* port/3ds/gfx/gdx3ds_gpucensus.c -- n60gpu: [debug] gpucensus (contract in the header).
 *
 * The walk reads what the GPU will read: the frame's GX queue (citro3d's context, first member) and
 * every command list in it, decoded with the PICA command format (3dbrew "GPU/Commands": param0,
 * header = reg | mask << 16 | extra << 20 | consecutive << 31, extra params, 8-byte padding). The
 * shadow register file persists across lists and frames, like the GPU's. A right-eye replay
 * (gdx3ds_stereo_frame.cpp) re-submits the left eye's lists, so the same words are walked twice,
 * once after a write of the left colour buffer location and once after the right one: the eye of a
 * draw is the colour buffer bound when it is triggered. */
#include "gdx3ds_gpucensus.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "../vendor/citro3d/source/internal.h"

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

enum {
    CEN_CATS = 13,     /* 0 = before the first marker, 1..12 = GDX3DS_C5CAT_* */
    CEN_EYES = 2,      /* L, R */
    CEN_MARKS = 96,
    CEN_TEXSET = 256,
    CEN_REGS = 0x300,
};

static const char* const kCatName[CEN_CATS] = {"pre", "misc", "sky", "trk", "dress", "mach", "mesh",
                                               "shd", "fx",  "hud",  "fbeg", "mirr", "swap"};
/* PICA texture formats (GPU_TEXCOLOR) and their bits per texel. */
static const char* const kFmtName[16] = {"RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8", "L8",
                                         "A8",    "LA4",  "L4",       "A4",     "ETC1",  "ETC1A4", "f14", "f15"};
static const uint8_t kFmtBits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 32, 32};

static int sMode = -1;
static unsigned sWindowFrames = 128;

/* ---- the frame's category marks: (word address, category), increasing addresses ---- */
static const u32* sMarkAddr[CEN_MARKS];
static uint8_t sMarkCat[CEN_MARKS];
static int sMarks = 0;
static unsigned sMarkOverflow = 0;

/* ---- the walker's GPU model ---- */
static u32 sReg[CEN_REGS];
static u32 sMainColorPa = 0, sMainDepthPa = 0, sRightColorPa = 0, sRightDepthPa = 0;
static int sEye = 0;      /* the eye of the colour buffer bound last (0 L, 1 R) */
static int sLastDrawElements = 0; /* the draw being accounted is a DrawElements */
static int sOffscreen = 0; /* the bound colour buffer is neither eye's (a texture pass) */
static const u32* sListLo = NULL; /* the frame command buffer, for category lookup */
static const u32* sListHi = NULL;

typedef struct {
    uint32_t draws, verts, tris, blend, dtest, dwrite, atest, stencil, offscreen, textured, tex16, texVram, tcClears;
    uint64_t texTexels; /* sum over draws and enabled units of w*h (the bound texture footprint) */
    double bboxArea;    /* sum of the draws' clip-space bounding boxes, in screens (2D-layout draws only) */
    uint32_t bboxDraws;
} CatAcc;

typedef struct {
    uint32_t lists, foreign, words, fills, fillMain, xfers, copies, flushes, other, fbBinds, rcFlushes;
    uint64_t fillBytes, copyBytes;
} EyeAcc;

typedef struct {
    CatAcc cat[CEN_EYES][CEN_CATS];
    EyeAcc eye[CEN_EYES + 1]; /* [2] = not attributable (offscreen / captures) */
    uint32_t entries;
    uint32_t uniqTex[CEN_EYES];
    uint64_t uniqBytes[CEN_EYES];
    uint32_t fmtDraws[16];
    uint32_t dimHist[6]; /* max(w,h) <= 8, 16, 32, 64, 128, more */
    uint32_t frames;
} Window;

static Window sWin;

typedef struct {
    float area;
    u32 blend, atest, dim, n, op, depth, fmt;
} BigDraw;
static BigDraw sBig[4]; /* the window's largest draws of the HUD category (by bbox), eye L */
static char sDrawList[2][600]; /* the seq frame's sky / hud draws, eye L */
static int sDrawListLen[2];
static int sDrawListOn = 0;
extern u32 __ctru_linear_heap;      /* libctru: the linear heap's VA and size */
extern u32 __ctru_linear_heap_size;

/* per-frame unique textures (by physical address + format + size), per eye */
typedef struct {
    u32 key;
    u32 dim;
    uint8_t fmt;
    uint8_t eyes;
} TexSlot;
static TexSlot sTex[CEN_TEXSET];

/* ---- fill mode ---- */
static const uint8_t kFillCycle[] = {0xFF, 2, 3, 4, 5, 7, 8, 9, 1};
enum { kFillCycleLen = sizeof(kFillCycle) / sizeof(kFillCycle[0]) };
static unsigned long sFrame = 0;
static int sFillFrame = 0;   /* this frame is a counting frame */
static uint8_t sFillCat = 0xFF;
static unsigned sFillRefused = 0;
static u32* sReadBuf = NULL;
typedef struct {
    uint32_t frames;
    uint64_t frags;
    uint64_t covered;
    uint32_t maxLayer;
    uint64_t hist[7]; /* pixels with 0, 1, 2, 3, 4, 5-7, 8+ fragments */
} FillAcc;
static FillAcc sFill[CEN_CATS + 1]; /* [CEN_CATS] = all */
static unsigned sFillFrames = 0;
static unsigned sFillReadFail = 0;

/* ---- upload census ---- */
enum { UP_RGBA8, UP_RGB8, UP_RGBA5551, UP_RGB565, UP_RGBA4, UP_LA8, UP_L8, UP_A8, UP_LA4, UP_L4, UP_A4, UP_N };
static const char* const kUpName[UP_N] = {"rgba8", "rgb8", "rgba5551", "rgb565", "rgba4", "la8",
                                          "l8",    "a8",   "la4",      "l4",     "a4"};
static const uint8_t kUpBits[UP_N] = {32, 24, 16, 16, 16, 16, 8, 8, 8, 4, 4};
static uint32_t sUpCount[UP_N];
static uint64_t sUpBytes[UP_N]; /* the texture's RGBA8 bytes, by its best exact class */
static uint32_t sUpAtlas = 0;

static void CenLog(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static int ConfigInt(const char* key, int fallback) {
    if (&gdx3ds_config_get_int == NULL) {
        return fallback;
    }
    return gdx3ds_config_get_int("debug", key, fallback);
}

static void Latch(void) {
    if (sMode >= 0) {
        return;
    }
    sMode = ConfigInt("gpucensus", 0);
    if (sMode < 0 || sMode > 3) {
        sMode = 0;
    }
    int w = ConfigInt("gpucensus_window", 128);
    sWindowFrames = w > 0 ? (unsigned)w : 128u;
    if (sMode != 0) {
        CenLog("[gpucen] init mode=%d window=%u (%s)", sMode, sWindowFrames,
               sMode == 1 ? "stats walk of the GX queue after each frame end"
                          : (sMode == 2 ? "FILL raster count per category, image destroyed"
                                        : "FILL depth-passing count, image destroyed"));
    }
}

int gdx3ds_gpucensus_mode(void) {
    Latch();
    return sMode;
}

int gdx3ds_gpucensus_markers_armed(void) {
    return gdx3ds_gpucensus_mode() != 0;
}

void gdx3ds_gpucensus_marker(unsigned category) {
    if (sMode <= 0) {
        return;
    }
    if (sMarks >= CEN_MARKS) {
        sMarkOverflow++;
        return;
    }
    sMarkAddr[sMarks] = gpuCmdBuf + gpuCmdBufOffset;
    sMarkCat[sMarks] = (uint8_t)(category < CEN_CATS ? category : 1u);
    sMarks++;
}

static int CatOf(const u32* word) {
    if (word < sListLo || word >= sListHi) {
        return 0;
    }
    int lo = 0, hi = sMarks - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (sMarkAddr[mid] <= word) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best < 0 ? 0 : sMarkCat[best];
}

static u32 ByteMask(u32 mask) {
    return ((mask & 1u) ? 0x000000FFu : 0u) | ((mask & 2u) ? 0x0000FF00u : 0u) | ((mask & 4u) ? 0x00FF0000u : 0u) |
           ((mask & 8u) ? 0xFF000000u : 0u);
}

/* ---- fill-mode patching: every write of the fragment pipeline becomes the count state ---- */
static int IsTexEnvReg(u32 r, u32* off) {
    static const u32 kBase[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
    for (int s = 0; s < 6; s++) {
        if (r >= kBase[s] && r <= kBase[s] + 4u) {
            *off = r - kBase[s];
            return 1;
        }
    }
    return 0;
}

/* The count-state value of register r given the game's value v (mode 2 / 3), or 0 = keep. *need =
 * the header mask bits the new value needs. */
static int CountValue(u32 r, u32 v, u32* out, u32* need) {
    u32 off = 0;
    *need = 0xF;
    if (IsTexEnvReg(r, &off)) {
        static const u32 kStage[5] = {0x0EEE0EEEu, 0u, 0u, 0x00000001u, 0u}; /* constant (R = 1), replace */
        *out = kStage[off];
        return 1;
    }
    switch (r) {
        case 0xE0: /* TEXENV_UPDATE_BUFFER: fog off */
            *out = v & ~7u;
            *need = 0x1;
            return 1;
        case 0x100: /* COLOR_OPERATION: default fragment op, blend mode */
            *out = 0x00E40100u;
            *need = 0x7;
            return 1;
        case 0x101: /* BLEND_FUNC: ADD, ONE, ONE */
            *out = 0x11110000u;
            return 1;
        case 0x104: /* alpha test off */
        case 0x105: /* stencil test off */
        case 0x62:  /* early depth off */
        case 0x118:
            *out = 0u;
            return 1;
        case 0x107: /* DEPTH_COLOR_MASK */
            *out = sMode == 2 ? 0x0F11u /* test on, ALWAYS, colour RGBA, no depth write */
                              : ((v & 0x10F1u) | 0x0F00u);
            return 1;
        case 0x112: /* COLORBUFFER_READ / WRITE: blending reads the count */
        case 0x113:
            *out = 0xFu;
            return 1;
        default:
            return 0;
    }
}

/* ---- the walk ---- */
static void TexNote(int eye, u32 pa, u32 dim, u32 fmt) {
    const u32 key = pa ^ (dim * 2654435761u) ^ fmt;
    u32 h = (key * 2654435761u) >> 24;
    for (int probe = 0; probe < CEN_TEXSET; probe++) {
        TexSlot* s = &sTex[(h + (u32)probe) & (CEN_TEXSET - 1)];
        if (s->eyes == 0) {
            s->key = pa;
            s->dim = dim;
            s->fmt = (uint8_t)fmt;
            s->eyes = (uint8_t)(1u << eye);
            return;
        }
        if (s->key == pa && s->dim == dim && s->fmt == fmt) {
            s->eyes |= (uint8_t)(1u << eye);
            return;
        }
    }
}

/* The clip-space bounding box of a draw whose vertex layout is the backend's 2D/legacy one (attribute 0
 * = float x4 clip position, first in buffer 0), as a fraction of the screen; -1 = another layout. */
static float DrawBBox(void) {
    const u32 fmtLo = sReg[0x201];
    const u32 cfg1 = sReg[0x204];
    const u32 cfg2 = sReg[0x205];
    if ((fmtLo & 0xFu) != 0xFu || (cfg1 & 0xFu) != 0u) {
        return -1.0f;
    }
    const u32 stride = (cfg2 >> 16) & 0xFFu;
    if (stride < 16u) {
        return -1.0f;
    }
    static u32 sHeapPa = 0;
    if (sHeapPa == 0u) {
        sHeapPa = osConvertVirtToPhys((const void*)__ctru_linear_heap);
    }
    const u32 heapVa = __ctru_linear_heap;
    const u32 heapSize = __ctru_linear_heap_size;
    const u32 basePa = (sReg[0x200] << 3) + sReg[0x203];
    if (basePa < sHeapPa || basePa - sHeapPa >= heapSize) {
        return -1.0f;
    }
    const uintptr_t base = (uintptr_t)heapVa + (basePa - sHeapPa);
    const uintptr_t heapEnd = (uintptr_t)heapVa + heapSize;
    const u32 n = sReg[0x228] < 512u ? sReg[0x228] : 512u;
    const int indexed = sLastDrawElements;
    const u32 idxPa = (sReg[0x200] << 3) + (sReg[0x227] & 0x0FFFFFFFu);
    if (indexed && (idxPa < sHeapPa || idxPa - sHeapPa + 2u * n >= heapSize)) {
        return -1.0f;
    }
    const uintptr_t idx = (uintptr_t)heapVa + (idxPa - sHeapPa);
    const int idx16 = (int)(sReg[0x227] >> 31);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 k = 0; k < n; k++) {
        u32 v = sReg[0x22A] + k;
        if (indexed) {
            v = idx16 ? ((const uint16_t*)idx)[k] : ((const uint8_t*)idx)[k];
        }
        const uintptr_t at = base + (uintptr_t)v * stride;
        if (at + 16u > heapEnd) {
            return -1.0f;
        }
        const float* p = (const float*)at;
        const float w = p[3];
        if (!(w > 1e-6f)) {
            continue;
        }
        const float x = p[0] / w, y = p[1] / w;
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
    }
    if (x1 < x0) {
        return 0.0f;
    }
    x0 = x0 < -1.0f ? -1.0f : x0;
    y0 = y0 < -1.0f ? -1.0f : y0;
    x1 = x1 > 1.0f ? 1.0f : x1;
    y1 = y1 > 1.0f ? 1.0f : y1;
    return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) * 0.25f : 0.0f;
}

static void OnDraw(const u32* word) {
    const int cat = CatOf(word);
    CatAcc* a = &sWin.cat[sEye][cat];
    const u32 n = sReg[0x228];
    const u32 prim = (sReg[0x25E] >> 8) & 3u;
    a->draws++;
    a->verts += n;
    a->tris += (prim == 1u || prim == 2u) ? (n >= 2u ? n - 2u : 0u) : n / 3u;
    const u32 op = sReg[0x100];
    const u32 bf = sReg[0x101];
    if ((op & 0x100u) != 0u && !(((bf >> 16) & 0xFu) == 1u && ((bf >> 20) & 0xFu) == 0u)) {
        a->blend++;
    }
    const u32 dcm = sReg[0x107];
    if (dcm & 1u) {
        a->dtest++;
    }
    if ((dcm >> 12) & 1u) {
        a->dwrite++;
    }
    if (sReg[0x104] & 1u) {
        a->atest++;
    }
    if (sReg[0x105] & 1u) {
        a->stencil++;
    }
    if (sOffscreen) {
        a->offscreen++;
    }
    static const u32 kDim[3] = {0x82, 0x92, 0x9A};
    static const u32 kAddr[3] = {0x85, 0x95, 0x9D};
    static const u32 kType[3] = {0x8E, 0x96, 0x9E};
    const u32 cfg = sReg[0x80];
    int textured = 0;
    for (int u = 0; u < 3; u++) {
        if (((cfg >> u) & 1u) == 0u) {
            continue;
        }
        textured = 1;
        const u32 dim = sReg[kDim[u]];
        const u32 w = (dim >> 16) & 0x7FFu;
        const u32 h = dim & 0x7FFu;
        const u32 fmt = sReg[kType[u]] & 0xFu;
        const u32 pa = sReg[kAddr[u]] << 3;
        a->texTexels += (uint64_t)w * h;
        if (kFmtBits[fmt] <= 16u) {
            a->tex16++;
        }
        if (pa >= 0x18000000u && pa < 0x18600000u) {
            a->texVram++;
        }
        sWin.fmtDraws[fmt]++;
        const u32 m = w > h ? w : h;
        sWin.dimHist[m <= 8 ? 0 : m <= 16 ? 1 : m <= 32 ? 2 : m <= 64 ? 3 : m <= 128 ? 4 : 5]++;
        TexNote(sEye, pa, dim, fmt);
    }
    if (textured) {
        a->textured++;
    }
    if (!sFillFrame) {
        const float area = DrawBBox();
        if (sDrawListOn && sEye == 0 && (cat == 2 || cat == 9)) {
            const int li = cat == 2 ? 0 : 1;
            if (sDrawListLen[li] < (int)sizeof(sDrawList[0]) - 48) {
                const u32 bfn = sReg[0x101];
                const int bl = (sReg[0x100] & 0x100u) != 0u && !(((bfn >> 16) & 0xFu) == 1u && ((bfn >> 20) & 0xFu) == 0u);
                const int m = snprintf(sDrawList[li] + sDrawListLen[li], sizeof(sDrawList[0]) - (size_t)sDrawListLen[li],
                                       " %.2f%s%s/%ux%u", (double)area, bl ? "b" : "", (sReg[0x104] & 1u) ? "a" : "",
                                       (cfg & 1u) ? (unsigned)((sReg[0x82] >> 16) & 0x7FFu) : 0u,
                                       (cfg & 1u) ? (unsigned)(sReg[0x82] & 0x7FFu) : 0u);
                if (m > 0) {
                    sDrawListLen[li] += m;
                }
            }
        }
        if (area >= 0.0f) {
            a->bboxArea += area;
            a->bboxDraws++;
            if (cat == 9 && sEye == 0) {
                int slot = -1;
                for (int k = 0; k < 4; k++) {
                    if (slot < 0 || sBig[k].area < sBig[slot].area) {
                        slot = k;
                    }
                }
                if (area > sBig[slot].area) {
                    BigDraw* bd = &sBig[slot];
                    bd->area = area;
                    bd->blend = sReg[0x101];
                    bd->atest = sReg[0x104];
                    bd->dim = (cfg & 1u) ? sReg[0x82] : 0u;
                    bd->fmt = (cfg & 1u) ? (sReg[0x8E] & 0xFu) : 0xFFu;
                    bd->n = n;
                    bd->op = sReg[0x100];
                    bd->depth = sReg[0x107];
                }
            }
        }
    }
}

static void OnReg(u32 r, const u32* word) {
    if (r == 0x22E || r == 0x22F) {
        sLastDrawElements = r == 0x22F;
        OnDraw(word);
    } else if (r == 0x11D) {
        const u32 pa = sReg[0x11D] << 3;
        sOffscreen = 0;
        if (pa == sMainColorPa) {
            sEye = 0;
        } else if (pa == sRightColorPa && sRightColorPa != 0u) {
            sEye = 1;
        } else {
            sOffscreen = 1;
        }
        sWin.eye[sOffscreen ? 2 : sEye].fbBinds++;
    } else if (r == 0x111) {
        sWin.eye[sOffscreen ? 2 : sEye].rcFlushes++;
    }
}

/* Walk (and in fill frames patch) one command list. */
static void WalkList(u32* w, u32 nwords, int patch) {
    u32 i = 0;
    while (i + 1u < nwords) {
        u32* hdrp = &w[i + 1u];
        const u32 hdr = *hdrp;
        const u32 reg = hdr & 0xFFFFu;
        const u32 extra = (hdr >> 20) & 0xFFu;
        const int seq = (int)(hdr >> 31);
        for (u32 k = 0; k <= extra; k++) {
            const u32 idx = k == 0u ? i : i + 1u + k;
            if (idx >= nwords) {
                break;
            }
            const u32 r = seq ? reg + k : reg;
            if (r >= CEN_REGS) {
                continue;
            }
            u32* pw = &w[idx];
            if (patch) {
                u32 nv = 0, need = 0;
                if (CountValue(r, *pw, &nv, &need)) {
                    *pw = nv;
                    *hdrp |= need << 16;
                } else if (r == 0x228 && sMode == 2 && sFillCat != 0xFFu) {
                    const int cat = CatOf(pw);
                    const int counted = cat == sFillCat || (sFillCat == 1u && cat == 0);
                    if (!counted && *pw > 2u) {
                        *pw = 2u; /* two vertices / indices: no triangle in any primitive mode (0 trips the
                                   * emulator's vertex stream assertion) */
                    }
                }
            }
            const u32 bm = ByteMask((*hdrp >> 16) & 0xFu);
            sReg[r] = (sReg[r] & ~bm) | (*pw & bm);
            if (r == 0x80u && (bm & 0x00FF0000u) != 0u && (*pw & 0x10000u) != 0u) {
                sWin.cat[sEye][CatOf(pw)].tcClears++; /* TEXUNIT_CONFIG bit 16: texture cache clear */
            }
            OnReg(r, pw);
        }
        i += 2u + extra;
        i = (i + 1u) & ~1u;
    }
}

static u32 Pa(const void* va) {
    return va != NULL ? osConvertVirtToPhys(va) : 0u;
}

static int FillEye(u32 va) {
    const u32 pa = Pa((const void*)va);
    if (pa == sMainColorPa || pa == sMainDepthPa) {
        return 0;
    }
    if (sRightColorPa != 0u && (pa == sRightColorPa || pa == sRightDepthPa)) {
        return 1;
    }
    return 2;
}

static char sSeq[520];
static int sSeqLen = 0;
static int sSeqOn = 0;

static void SeqAdd(const char* fmt, ...) {
    if (!sSeqOn || sSeqLen >= (int)sizeof(sSeq) - 24) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(sSeq + sSeqLen, sizeof(sSeq) - (size_t)sSeqLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sSeqLen += n;
    }
}

static void WalkQueue(int patch) {
    gxCmdQueue_s* q = &C3Di_GetContext()->gxQueue;
    const int n = q->numEntries;
    if (!patch) {
        sWin.entries += (uint32_t)n;
    }
    int listEye = 0;
    for (int e = 0; e < n; e++) {
        gxCmdEntry_s* en = &q->entries[e];
        const u32 id = en->data[0] & 0xFFu;
        if (id == 0x01u) {
            u32* buf = (u32*)en->data[1];
            const u32 words = en->data[2] / 4u;
            const int inFrame = (const u32*)buf >= sListLo && (const u32*)buf < sListHi;
            if (patch && !inFrame) {
                continue;
            }
            const int eyeIn = sEye;
            WalkList(buf, words, patch);
            SeqAdd(" %c%c:%s-%s/%u", 'l', eyeIn == 0 ? 'L' : 'R', kCatName[CatOf(buf)],
                   kCatName[CatOf(buf + (words > 0u ? words - 1u : 0u))], (unsigned)words);
            if (!patch) {
                EyeAcc* ea = &sWin.eye[sEye];
                ea->lists++;
                ea->words += words;
                if (!inFrame) {
                    ea->foreign++;
                }
            }
            listEye = sEye;
        } else if (!patch) {
            if (id == 0x02u) {
                for (int b = 0; b < 2; b++) {
                    const u32 start = en->data[1 + 3 * b];
                    const u32 end = en->data[3 + 3 * b];
                    if (start == 0u || end <= start) {
                        continue;
                    }
                    const int eye = FillEye(start);
                    SeqAdd(" F%c:%s%uK", eye == 0 ? 'L' : (eye == 1 ? 'R' : 'o'),
                           (Pa((const void*)start) == sMainDepthPa || Pa((const void*)start) == sRightDepthPa) ? "z" : "c",
                           (unsigned)((end - start) / 1024u));
                    sWin.eye[eye].fillBytes += end - start;
                    if (b == 0) {
                        sWin.eye[eye].fills++;
                    }
                    if (eye < 2) {
                        sWin.eye[eye].fillMain++;
                    }
                }
            } else if (id == 0x03u) {
                const u32 pa = Pa((const void*)en->data[1]);
                const int eye = pa == sMainColorPa ? 0 : (pa == sRightColorPa && pa != 0u ? 1 : 2);
                SeqAdd(" X%c", eye == 0 ? 'L' : (eye == 1 ? 'R' : 'o'));
                sWin.eye[eye].xfers++;
            } else if (id == 0x04u) {
                SeqAdd(" C%uK", (unsigned)(en->data[3] / 1024u));
                sWin.eye[2].copies++;
                sWin.eye[2].copyBytes += en->data[3];
            } else if (id == 0x05u) {
                sWin.eye[listEye].flushes++;
            } else {
                sWin.eye[2].other++;
            }
        }
    }
}

/* The open command list (written since the last split), before C3D_FrameEnd closes it. */
static void WalkOpenList(int patch) {
    if (gpuCmdBuf != NULL && gpuCmdBufOffset > 0u) {
        WalkList(gpuCmdBuf, gpuCmdBufOffset, patch);
    }
}

static void EmitWindow(void) {
    const double inv = sWin.frames > 0 ? 1.0 / (double)sWin.frames : 0.0;
    for (int eye = 0; eye < CEN_EYES; eye++) {
        const EyeAcc* ea = &sWin.eye[eye];
        if (ea->lists == 0u && ea->fills == 0u && ea->xfers == 0u) {
            continue;
        }
        CenLog("[gpucen] frame=%lu n=%u eye=%c | ent=%.1f lists=%.2f foreign=%.2f kw=%.2f fills=%.2f (%.0fK) "
               "xfer=%.2f flush=%.2f fb=%.2f rcflush=%.2f | uniqtex=%.1f (%.0fK)",
               sFrame, sWin.frames, eye == 0 ? 'L' : 'R', sWin.entries * inv, ea->lists * inv, ea->foreign * inv,
               ea->words * inv / 1000.0, ea->fills * inv, (double)ea->fillBytes * inv / 1024.0, ea->xfers * inv,
               ea->flushes * inv, ea->fbBinds * inv, ea->rcFlushes * inv, sWin.uniqTex[eye] * inv,
               (double)sWin.uniqBytes[eye] * inv / 1024.0);
        for (int c = 0; c < CEN_CATS; c++) {
            const CatAcc* a = &sWin.cat[eye][c];
            if (a->draws == 0u && a->tcClears == 0u) {
                continue;
            }
            CenLog("[gpucen]   eye=%c cat=%s d=%.1f v=%.0f t=%.0f blend=%.1f dtest=%.1f dwrite=%.1f atest=%.1f "
                   "sten=%.1f off=%.1f tex=%.1f tex16=%.1f vram=%.1f texel=%.0fK tcclr=%.1f bbox=%.2f/%.1f",
                   eye == 0 ? 'L' : 'R', kCatName[c], a->draws * inv, a->verts * inv, a->tris * inv, a->blend * inv,
                   a->dtest * inv, a->dwrite * inv, a->atest * inv, a->stencil * inv, a->offscreen * inv,
                   a->textured * inv, a->tex16 * inv, a->texVram * inv, (double)a->texTexels * inv / 1024.0,
                   a->tcClears * inv, a->bboxArea * inv, a->bboxDraws * inv);
        }
    }
    for (int k = 0; k < 4; k++) {
        const BigDraw* bd = &sBig[k];
        if (bd->area <= 0.0f) {
            continue;
        }
        CenLog("[gpucen] frame=%lu hudbig area=%.3f n=%u blend=%08lx op=%06lx atest=%04lx depth=%04lx tex=%ux%u fmt=%u",
               sFrame, (double)bd->area, (unsigned)bd->n, (unsigned long)bd->blend, (unsigned long)(bd->op & 0xFFFFFFu),
               (unsigned long)(bd->atest & 0xFFFFu), (unsigned long)(bd->depth & 0xFFFFu), (unsigned)((bd->dim >> 16) & 0x7FFu),
               (unsigned)(bd->dim & 0x7FFu), (unsigned)bd->fmt);
    }
    memset(sBig, 0, sizeof(sBig));
    const EyeAcc* x = &sWin.eye[2];
    CenLog("[gpucen] frame=%lu other: lists=%.2f fills=%.2f (%.0fK) xfer=%.2f copies=%.2f (%.0fK) other=%.2f "
           "fb=%.2f | marks/f=%.1f overflow=%u",
           sFrame, x->lists * inv, x->fills * inv, (double)x->fillBytes * inv / 1024.0, x->xfers * inv,
           x->copies * inv, (double)x->copyBytes * inv / 1024.0, x->other * inv, x->fbBinds * inv,
           (double)sMarks, sMarkOverflow);
    char fmts[200];
    int p = 0;
    fmts[0] = '\0';
    for (int f = 0; f < 16 && p < (int)sizeof(fmts) - 24; f++) {
        if (sWin.fmtDraws[f] != 0u) {
            p += snprintf(fmts + p, sizeof(fmts) - (size_t)p, " %s=%.1f", kFmtName[f], sWin.fmtDraws[f] * inv);
        }
    }
    CenLog("[gpucen] frame=%lu texbinds/f:%s | dim<=8 %.1f 16 %.1f 32 %.1f 64 %.1f 128 %.1f more %.1f", sFrame, fmts,
           sWin.dimHist[0] * inv, sWin.dimHist[1] * inv, sWin.dimHist[2] * inv, sWin.dimHist[3] * inv,
           sWin.dimHist[4] * inv, sWin.dimHist[5] * inv);
    uint32_t upN = 0;
    for (int k = 0; k < UP_N; k++) {
        upN += sUpCount[k];
    }
    if (upN > 0u) {
        char ups[300];
        int q = 0;
        uint64_t rgba = 0, best = 0;
        ups[0] = '\0';
        for (int k = 0; k < UP_N && q < (int)sizeof(ups) - 32; k++) {
            rgba += sUpBytes[k];
            best += sUpBytes[k] * kUpBits[k] / 32u;
            if (sUpCount[k] != 0u) {
                q += snprintf(ups + q, sizeof(ups) - (size_t)q, " %s=%u/%luK", kUpName[k], (unsigned)sUpCount[k],
                              (unsigned long)(sUpBytes[k] / 1024u));
            }
        }
        CenLog("[gpucen] frame=%lu uploads=%u (atlas-armed %u) rgba8 %luK -> exact-narrowest %luK |%s", sFrame,
               (unsigned)upN, (unsigned)sUpAtlas, (unsigned long)(rgba / 1024u), (unsigned long)(best / 1024u), ups);
        memset(sUpCount, 0, sizeof(sUpCount));
        memset(sUpBytes, 0, sizeof(sUpBytes));
        sUpAtlas = 0;
    }
    memset(&sWin, 0, sizeof(sWin));
}

static void FrameUniqueTex(void) {
    for (int k = 0; k < CEN_TEXSET; k++) {
        TexSlot* s = &sTex[k];
        if (s->eyes == 0) {
            continue;
        }
        const u32 w = (s->dim >> 16) & 0x7FFu;
        const u32 h = s->dim & 0x7FFu;
        const uint64_t bytes = (uint64_t)w * h * kFmtBits[s->fmt & 0xFu] / 8u;
        for (int eye = 0; eye < CEN_EYES; eye++) {
            if (s->eyes & (1u << eye)) {
                sWin.uniqTex[eye]++;
                sWin.uniqBytes[eye] += bytes;
            }
        }
    }
    memset(sTex, 0, sizeof(sTex));
}

/* ---- fill mode ---- */
static void EmitCountState(void) {
    static const u32 kRegs[] = {0xE0, 0x100, 0x101, 0x104, 0x105, 0x107, 0x112, 0x113, 0x62, 0x118};
    static const u32 kTexEnv[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};
    for (int s = 0; s < 6; s++) {
        for (u32 off = 0; off < 5u; off++) {
            u32 v = 0, need = 0;
            CountValue(kTexEnv[s] + off, 0u, &v, &need);
            GPUCMD_AddWrite(kTexEnv[s] + off, v);
        }
    }
    for (size_t k = 0; k < sizeof(kRegs) / sizeof(kRegs[0]); k++) {
        u32 v = 0, need = 0;
        CountValue(kRegs[k], sReg[kRegs[k]], &v, &need);
        GPUCMD_AddWrite(kRegs[k], v);
    }
}

static int FillAllowed(void) {
    gxCmdQueue_s* q = &C3Di_GetContext()->gxQueue;
    /* a kicked or running queue has executed lists we would patch too late */
    return q->curEntry == 0 && q->lastEntry == 0;
}

static void FillReadback(const void* mainColor) {
    const u32 fbW = 240u, fbH = 400u;
    const size_t bytes = (size_t)fbW * fbH * 4u;
    if (sReadBuf == NULL) {
        sReadBuf = (u32*)linearAlloc(bytes);
        if (sReadBuf == NULL) {
            sFillReadFail++;
            return;
        }
    }
    volatile u32* sentinel = (volatile u32*)((uint8_t*)sReadBuf + bytes - 8u);
    sentinel[0] = 0xDEADC0DEu;
    sentinel[1] = 0x51A7BEEFu;
    GSPGPU_FlushDataCache((void*)sentinel, 8);
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer((u32*)mainColor, GX_BUFFER_DIM(fbW, fbH), sReadBuf, GX_BUFFER_DIM(fbW, fbH), flags);
    int landed = 0;
    for (int spin = 0; spin < 4000; spin++) {
        GSPGPU_InvalidateDataCache((void*)sentinel, 8);
        if (sentinel[0] != 0xDEADC0DEu || sentinel[1] != 0x51A7BEEFu) {
            landed = 1;
            break;
        }
        svcSleepThread(100 * 1000);
    }
    if (!landed) {
        sFillReadFail++;
        return;
    }
    GSPGPU_InvalidateDataCache(sReadBuf, bytes);
    FillAcc* f = &sFill[sFillCat == 0xFFu ? CEN_CATS : sFillCat];
    f->frames++;
    for (size_t k = 0; k < (size_t)fbW * fbH; k++) {
        const u32 layers = sReadBuf[k] >> 24; /* R of R<<24|G<<16|B<<8|A */
        f->frags += layers;
        if (layers > 0u) {
            f->covered++;
        }
        if (layers > f->maxLayer) {
            f->maxLayer = layers;
        }
        f->hist[layers <= 4u ? layers : (layers <= 7u ? 5u : 6u)]++;
    }
}

static void EmitFill(void) {
    const double px = 240.0 * 400.0;
    for (int c = 0; c <= CEN_CATS; c++) {
        FillAcc* f = &sFill[c];
        if (f->frames == 0u) {
            continue;
        }
        const double inv = 1.0 / (double)f->frames;
        CenLog("[gpufill] frame=%lu mode=%d cat=%s n=%u frags=%.0f (%.2f screens) covered=%.1f%% max=%u | px with "
               "0:%.1f%% 1:%.1f%% 2:%.1f%% 3:%.1f%% 4:%.1f%% 5-7:%.1f%% 8+:%.1f%% | refused=%u readfail=%u",
               sFrame, sMode, c == CEN_CATS ? "ALL" : kCatName[c], f->frames, (double)f->frags * inv,
               (double)f->frags * inv / px, 100.0 * (double)f->covered * inv / px, f->maxLayer,
               100.0 * (double)f->hist[0] * inv / px, 100.0 * (double)f->hist[1] * inv / px,
               100.0 * (double)f->hist[2] * inv / px, 100.0 * (double)f->hist[3] * inv / px,
               100.0 * (double)f->hist[4] * inv / px, 100.0 * (double)f->hist[5] * inv / px,
               100.0 * (double)f->hist[6] * inv / px, sFillRefused, sFillReadFail);
    }
    memset(sFill, 0, sizeof(sFill));
}

void gdx3ds_gpucensus_frame_begin(void) {
    Latch();
    if (sMode <= 0) {
        return;
    }
    C3D_Context* ctx = C3Di_GetContext();
    sListLo = ctx->cmdBuf;
    sListHi = ctx->cmdBuf + ctx->cmdBufSize;
    sMarks = 0;
    sFillFrame = 0;
    if (sMode >= 2) {
        sFillCat = sMode == 3 ? 0xFFu : kFillCycle[sFrame % kFillCycleLen];
        sFillFrame = 1;
        EmitCountState(); /* the frame starts in the count state; every later write is patched to it */
    }
}

void gdx3ds_gpucensus_before_frame_end(void) {
    if (sMode < 2 || !sFillFrame) {
        return;
    }
    if (!FillAllowed()) {
        sFillRefused++;
        sFillFrame = 0;
        return;
    }
    WalkQueue(1);
    WalkOpenList(1);
}

void gdx3ds_gpucensus_after_frame_end(const void* mainColor, const void* mainDepth, const void* rightColor,
                                      const void* rightDepth) {
    if (sMode <= 0) {
        return;
    }
    sFrame++;
    sMainColorPa = Pa(mainColor);
    sMainDepthPa = Pa(mainDepth);
    sRightColorPa = Pa(rightColor);
    sRightDepthPa = Pa(rightDepth);
    if (sMode == 1) {
        sSeqOn = sWin.frames == 0u;
        sSeqLen = 0;
        sSeq[0] = '\0';
        sDrawListOn = sSeqOn;
        sDrawListLen[0] = sDrawListLen[1] = 0;
        sDrawList[0][0] = sDrawList[1][0] = '\0';
        WalkQueue(0);
        if (sSeqOn) {
            CenLog("[gpucen] seq frame=%lu n=%d:%s", sFrame, (int)C3Di_GetContext()->gxQueue.numEntries, sSeq);
            CenLog("[gpucen] draws frame=%lu sky (area[b=blend a=alphatest]/tex):%s", sFrame, sDrawList[0]);
            CenLog("[gpucen] draws frame=%lu hud:%s", sFrame, sDrawList[1]);
            sSeqOn = 0;
            sDrawListOn = 0;
        }
        FrameUniqueTex();
        sWin.frames++;
        if (sWin.frames >= sWindowFrames) {
            EmitWindow();
        }
        return;
    }
    if (sFillFrame && mainColor != NULL) {
        FillReadback(mainColor);
        sFillFrames++;
        if (sFillFrames >= sWindowFrames) {
            EmitFill();
            sFillFrames = 0;
        }
    }
}

/* ---- upload census ---- */
static int Rep5(uint8_t v) {
    const uint8_t x = (uint8_t)(v >> 3);
    return (uint8_t)((x << 3) | (x >> 2)) == v;
}

static int Rep6(uint8_t v) {
    const uint8_t x = (uint8_t)(v >> 2);
    return (uint8_t)((x << 2) | (x >> 4)) == v;
}

static int Rep4(uint8_t v) {
    return (uint8_t)((v >> 4) * 17u) == v;
}

void gdx3ds_gpucensus_note_upload(const uint8_t* rgba, uint32_t width, uint32_t height, int atlas) {
    if (sMode != 1 || rgba == NULL) {
        return;
    }
    int opaque = 1, alpha1 = 1, grey = 1, black = 1, r5 = 1, g6 = 1, g5 = 1, b5 = 1, all4 = 1, grey4 = 1, a4 = 1;
    const size_t n = (size_t)width * height;
    for (size_t k = 0; k < n; k++) {
        const uint8_t r = rgba[k * 4u + 0u], g = rgba[k * 4u + 1u], b = rgba[k * 4u + 2u], a = rgba[k * 4u + 3u];
        opaque &= a == 255u;
        alpha1 &= a == 0u || a == 255u;
        grey &= r == g && g == b;
        black &= (r | g | b) == 0u;
        r5 &= Rep5(r);
        g6 &= Rep6(g);
        g5 &= Rep5(g);
        b5 &= Rep5(b);
        const int c4 = Rep4(r) && Rep4(g) && Rep4(b);
        all4 &= c4 && Rep4(a);
        grey4 &= c4;
        a4 &= Rep4(a);
    }
    int cls = UP_RGBA8;
    if (grey && opaque && grey4) {
        cls = UP_L4;
    } else if (black && a4) {
        cls = UP_A4;
    } else if (grey && opaque) {
        cls = UP_L8;
    } else if (black) {
        cls = UP_A8;
    } else if (grey && grey4 && a4) {
        cls = UP_LA4;
    } else if (grey) {
        cls = UP_LA8;
    } else if (opaque && r5 && g6 && b5) {
        cls = UP_RGB565;
    } else if (alpha1 && r5 && g5 && b5) {
        cls = UP_RGBA5551;
    } else if (all4) {
        cls = UP_RGBA4;
    } else if (opaque) {
        cls = UP_RGB8;
    }
    sUpCount[cls]++;
    sUpBytes[cls] += (uint64_t)n * 4u;
    if (atlas) {
        sUpAtlas++;
    }
}
