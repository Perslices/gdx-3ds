/* port/3ds/gfx/n64/gdx_gbi.h -- F3DEX2 (+ legacy F3D) command field decode for the owned
 * interpreter. No LUS headers, no decomp headers: the values below are the microcode's own
 * encoding (decomp/include/PR/gbi.h under F3DEX_GBI_2), spelled once here so the walker,
 * the host harness and the receipts agree. Plan: docs/research/native/06-interp-plan.md §3.3. */
#ifndef GDX_GBI_H
#define GDX_GBI_H

#include <stdint.h>

namespace gdxn {

/* ---- F3DEX2 opcodes ------------------------------------------------------------------- */
enum : uint8_t {
    OP_NOOP = 0x00,
    OP_VTX = 0x01,
    OP_MODIFYVTX = 0x02,
    OP_CULLDL = 0x03,
    OP_BRANCH_Z = 0x04,
    OP_TRI1 = 0x05,
    OP_TRI2 = 0x06,
    OP_QUAD = 0x07,
    OP_LINE3D = 0x08,      /* L3DEX2 line section only (plain F3DEX2: reserved, dropped) */
    OP_EXTRAGEOMETRYMODE = 0x3A, /* port-only (gbi.h G_EXTRAGEOMETRYMODE) */
    OP_DMA_IO = 0xD6,
    OP_TEXTURE = 0xD7,
    OP_POPMTX = 0xD8,
    OP_GEOMETRYMODE = 0xD9,
    OP_MTX = 0xDA,
    OP_MOVEWORD = 0xDB,
    OP_MOVEMEM = 0xDC,
    OP_LOAD_UCODE = 0xDD,
    OP_DL = 0xDE,
    OP_ENDDL = 0xDF,
    OP_SPNOOP = 0xE0,
    OP_RDPHALF_1 = 0xE1,
    OP_SETOTHERMODE_L = 0xE2,
    OP_SETOTHERMODE_H = 0xE3,
    OP_TEXRECT = 0xE4,
    OP_TEXRECTFLIP = 0xE5,
    OP_RDPLOADSYNC = 0xE6,
    OP_RDPPIPESYNC = 0xE7,
    OP_RDPTILESYNC = 0xE8,
    OP_RDPFULLSYNC = 0xE9,
    OP_SETKEYGB = 0xEA,
    OP_SETKEYR = 0xEB,
    OP_SETCONVERT = 0xEC,
    OP_SETSCISSOR = 0xED,
    OP_SETPRIMDEPTH = 0xEE,
    OP_RDPSETOTHERMODE = 0xEF,
    OP_LOADTLUT = 0xF0,
    OP_RDPHALF_2 = 0xF1,
    OP_SETTILESIZE = 0xF2,
    OP_LOADBLOCK = 0xF3,
    OP_LOADTILE = 0xF4,
    OP_SETTILE = 0xF5,
    OP_FILLRECT = 0xF6,
    OP_SETFILLCOLOR = 0xF7,
    OP_SETFOGCOLOR = 0xF8,
    OP_SETBLENDCOLOR = 0xF9,
    OP_SETPRIMCOLOR = 0xFA,
    OP_SETENVCOLOR = 0xFB,
    OP_SETCOMBINE = 0xFC,
    OP_SETTIMG = 0xFD,
    OP_SETZIMG = 0xFE,
    OP_SETCIMG = 0xFF,
};

/* ---- legacy F3D opcodes (segment-3 machine-part lists; remapped by the walker) ---------- */
enum : uint8_t {
    F3D_MTX = 0x01,
    F3D_MOVEMEM = 0x03,
    F3D_VTX = 0x04,
    F3D_DL = 0x06,
    F3D_CLEARGEOMETRYMODE = 0xB6,
    F3D_SETGEOMETRYMODE = 0xB7,
    F3D_ENDDL = 0xB8,
    F3D_SETOTHERMODE_L = 0xB9,
    F3D_SETOTHERMODE_H = 0xBA,
    F3D_TEXTURE = 0xBB,
    F3D_MOVEWORD = 0xBC,
    F3D_POPMTX = 0xBD,
    F3D_CULLDL = 0xBE,
    F3D_TRI1 = 0xBF,
};

/* ---- geometry mode (F3DEX2 bit layout) ------------------------------------------------- */
enum : uint32_t {
    GM_ZBUFFER = 0x00000001,
    GM_SHADE = 0x00000004,
    GM_CULL_FRONT = 0x00000200,
    GM_CULL_BACK = 0x00000400,
    GM_CULL_BOTH = 0x00000600,
    GM_FOG = 0x00010000,
    GM_LIGHTING = 0x00020000,
    GM_TEXTURE_GEN = 0x00040000,
    GM_TEXTURE_GEN_LINEAR = 0x00080000,
    GM_LOD = 0x00100000,
    GM_SHADING_SMOOTH = 0x00200000,
    GM_LIGHTING_POSITIONAL = 0x00400000,
    GM_CLIPPING = 0x00800000,
};

/* ---- othermode field shifts ------------------------------------------------------------ */
enum : uint32_t {
    MDSFT_ALPHACOMPARE = 0,
    MDSFT_ZSRCSEL = 2,
    MDSFT_RENDERMODE = 3,
    MDSFT_ALPHADITHER = 4,
    MDSFT_RGBDITHER = 6,
    MDSFT_COMBKEY = 8,
    MDSFT_TEXTCONV = 9,
    MDSFT_TEXTFILT = 12,
    MDSFT_TEXTLUT = 14,
    MDSFT_TEXTLOD = 16,
    MDSFT_TEXTDETAIL = 17,
    MDSFT_TEXTPERSP = 19,
    MDSFT_CYCLETYPE = 20,
    MDSFT_COLORDITHER = 22,
    MDSFT_PIPELINE = 23,
};

enum : uint32_t {
    CYC_1CYCLE = 0u << MDSFT_CYCLETYPE,
    CYC_2CYCLE = 1u << MDSFT_CYCLETYPE,
    CYC_COPY = 2u << MDSFT_CYCLETYPE,
    CYC_FILL = 3u << MDSFT_CYCLETYPE,
    TT_NONE = 0u << MDSFT_TEXTLUT,
    TT_RGBA16 = 2u << MDSFT_TEXTLUT,
    TT_IA16 = 3u << MDSFT_TEXTLUT,
    TF_POINT = 0u << MDSFT_TEXTFILT,
    TF_AVERAGE = 3u << MDSFT_TEXTFILT,
    TF_BILERP = 2u << MDSFT_TEXTFILT,
};

/* ---- render mode bits (othermode L) ---------------------------------------------------- */
enum : uint32_t {
    RM_AA_EN = 0x0008,
    RM_Z_CMP = 0x0010,
    RM_Z_UPD = 0x0020,
    RM_IM_RD = 0x0040,
    RM_CLR_ON_CVG = 0x0080,
    RM_CVG_DST_CLAMP = 0x0000,
    RM_CVG_DST_WRAP = 0x0100,
    RM_CVG_DST_FULL = 0x0200,
    RM_CVG_DST_SAVE = 0x0300,
    RM_ZMODE_OPA = 0x0000,
    RM_ZMODE_INTER = 0x0400,
    RM_ZMODE_XLU = 0x0800,
    RM_ZMODE_DEC = 0x0C00,
    RM_CVG_X_ALPHA = 0x1000,
    RM_ALPHA_CVG_SEL = 0x2000,
    RM_FORCE_BL = 0x4000,
    AC_NONE = 0,
    AC_THRESHOLD = 1,
    AC_DITHER = 3,
    ZS_PIXEL = 0 << 2,
    ZS_PRIM = 1 << 2,
};

/* Blender mux (2 bits each, packed in othermode L bits 16..31): cycle 1 in the high nibbles. */
enum : uint32_t {
    BL_CLR_IN = 0,
    BL_CLR_MEM = 1,
    BL_CLR_BL = 2,
    BL_CLR_FOG = 3,
    BL_A_IN = 0,
    BL_A_FOG = 1,
    BL_A_SHADE = 2,
    BL_1MA = 0,
    BL_A_MEM = 1,
    BL_1 = 2,
    BL_0 = 3,
};

/* ---- combiner mux values (lus_gbi.h; KEY/CONVERT are LUS-private remaps) -------------- */
enum : uint32_t {
    CCMUX_COMBINED = 0, CCMUX_TEXEL0 = 1, CCMUX_TEXEL1 = 2, CCMUX_PRIMITIVE = 3, CCMUX_SHADE = 4,
    CCMUX_ENVIRONMENT = 5, CCMUX_CENTER = 6, CCMUX_SCALE = 6, CCMUX_COMBINED_ALPHA = 7,
    CCMUX_TEXEL0_ALPHA = 8, CCMUX_TEXEL1_ALPHA = 9, CCMUX_PRIMITIVE_ALPHA = 10, CCMUX_SHADE_ALPHA = 11,
    CCMUX_ENV_ALPHA = 12, CCMUX_LOD_FRACTION = 13, CCMUX_PRIM_LOD_FRAC = 14, CCMUX_NOISE = 7, CCMUX_K4 = 7,
    CCMUX_K5 = 15, CCMUX_1 = 6, CCMUX_0 = 31, CCMUX_KEY_CENTER = 20, CCMUX_KEY_SCALE = 21,
    CCMUX_CONVERT_K4 = 22, CCMUX_CONVERT_K5 = 23,
    ACMUX_COMBINED = 0, ACMUX_TEXEL0 = 1, ACMUX_TEXEL1 = 2, ACMUX_PRIMITIVE = 3, ACMUX_SHADE = 4,
    ACMUX_ENVIRONMENT = 5, ACMUX_LOD_FRACTION = 0, ACMUX_PRIM_LOD_FRAC = 6, ACMUX_1 = 6, ACMUX_0 = 7,
};

/* ---- LUS shader ids (fast/interpreter.h SHADER_* / ShaderOpts): the backend keys programs
 * on (id0,id1) built from these, so they are reproduced here bit-for-bit. ---------------- */
enum : uint32_t {
    SHADER_0 = 0, SHADER_INPUT_1 = 1, SHADER_INPUT_2, SHADER_INPUT_3, SHADER_INPUT_4, SHADER_INPUT_5,
    SHADER_INPUT_6, SHADER_INPUT_7, SHADER_TEXEL0 = 8, SHADER_TEXEL0A = 9, SHADER_TEXEL1 = 10,
    SHADER_TEXEL1A = 11, SHADER_1 = 12, SHADER_COMBINED = 13, SHADER_NOISE = 14,
};
enum : uint64_t {
    SHOPT_ALPHA = 1ull << 0, SHOPT_FOG = 1ull << 1, SHOPT_TEXTURE_EDGE = 1ull << 2, SHOPT_NOISE = 1ull << 3,
    SHOPT_2CYC = 1ull << 4, SHOPT_ALPHA_THRESHOLD = 1ull << 5, SHOPT_INVISIBLE = 1ull << 6,
    SHOPT_GRAYSCALE = 1ull << 7, SHOPT_TEXEL0_CLAMP_S = 1ull << 8, SHOPT_TEXEL0_CLAMP_T = 1ull << 9,
    SHOPT_TEXEL1_CLAMP_S = 1ull << 10, SHOPT_TEXEL1_CLAMP_T = 1ull << 11, SHOPT_TEXEL0_MASK = 1ull << 12,
    SHOPT_TEXEL1_MASK = 1ull << 13, SHOPT_TEXEL0_BLEND = 1ull << 14, SHOPT_TEXEL1_BLEND = 1ull << 15,
    SHOPT_PRIM_DEPTH = 1ull << 16,
};
constexpr unsigned SHADER_ID_SHIFT = 17;

/* ---- movemem / moveword indices (F3DEX2) ----------------------------------------------- */
enum : uint8_t {
    MV_MMTX = 2,
    MV_PMTX = 6,
    MV_VIEWPORT = 8,
    MV_LIGHT = 10,
    MV_POINT = 12,
    MV_MATRIX = 14,
    MVO_LOOKATX = 0,
    MVO_LOOKATY = 24,
    MVO_L0 = 48,
    MW_MATRIX = 0,
    MW_NUMLIGHT = 2,
    MW_CLIP = 4,
    MW_SEGMENT = 6,
    MW_FOG = 8,
    MW_LIGHTCOL = 10,
    MW_FORCEMTX = 12,
    MW_PERSPNORM = 14,
    MWO_POINT_RGBA = 0x10,
    MWO_POINT_ST = 0x14,
    MWO_POINT_XYSCREEN = 0x18,
    MWO_POINT_ZSCREEN = 0x1C,
};

/* G_MTX parameter bits (F3DEX2: push is XOR'd with 1 by the macro) */
enum : uint8_t {
    MTX_NOPUSH = 0x00,
    MTX_PUSH = 0x01,
    MTX_LOAD = 0x02,
    MTX_PROJECTION = 0x04,
};

/* Texture formats */
enum : uint8_t { IM_FMT_RGBA = 0, IM_FMT_YUV = 1, IM_FMT_CI = 2, IM_FMT_IA = 3, IM_FMT_I = 4 };
enum : uint8_t { IM_SIZ_4b = 0, IM_SIZ_8b = 1, IM_SIZ_16b = 2, IM_SIZ_32b = 3 };
enum : uint8_t { TX_WRAP = 0, TX_MIRROR = 1, TX_CLAMP = 2 };
constexpr uint8_t TX_NOMASK = 0;

/* ---- one command as the walker sees it ------------------------------------------------- */
struct Cmd {
    uint32_t w0;
    uint32_t w1;   /* low 32 of the operand */
    uint32_t w1hi; /* wide packets only: 0 = value/segment token, GDX1 = tagged host pointer */
};

constexpr uint32_t kHostTag = 0x47445831u; /* "GDX1", decomp gbi.h GDX_GFXW1_HOST_TAG */

/* Field extractors (bit offset, width) on a 32-bit word, LUS/gbi spelling. */
static inline uint32_t C0(uint32_t w0, unsigned pos, unsigned width) {
    return (w0 >> pos) & ((1u << width) - 1u);
}
static inline uint32_t C1(uint32_t w1, unsigned pos, unsigned width) {
    return (w1 >> pos) & ((1u << width) - 1u);
}
static inline uint8_t Opcode(uint32_t w0) {
    return static_cast<uint8_t>(w0 >> 24);
}

static inline uint32_t Bswap32(uint32_t x) {
    return ((x & 0xFF000000u) >> 24) | ((x & 0x00FF0000u) >> 8) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x000000FFu) << 24);
}
static inline uint16_t Bswap16(uint16_t x) {
    return static_cast<uint16_t>((x >> 8) | (x << 8));
}

} // namespace gdxn

#endif /* GDX_GBI_H */
