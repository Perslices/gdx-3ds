/* port/3ds/gfx/n64/gdx_dl.cpp -- the display-list walker: 32-deep call stack, per-list
 * reader fixed at list entry, flat opcode switch, legacy-F3D dialect remap, G_LOAD_UCODE
 * variant switch. Plan §3.2/§3.3. Control-flow semantics mirror LUS's flat dispatch
 * (interpreter.cpp gfx_dl_handler_common / gfx_branch_z_handler_f3dex2 /
 * gfx_load_ucode_flat_handler) and the operand classification mirrors the bridge's
 * ProcessList cases, so a list walks here exactly as its converted copy walked there. */
#include "gdx_interp.h"

#include <math.h>
#include <string.h>

#include "gdx_log.h"

namespace gdxn {

static inline float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool Interp::PushList(const ListRef& list) {
    if (mSp >= kCallStack) {
        mStats.stackOverflow++;
        return false;
    }
    Frame& f = mStack[mSp++];
    f.list = list;
    f.idx = 0;
    mStats.lists++;
    mStats.listsByClass[static_cast<int>(ClassOf(list))]++;
    if (list.isF3d) {
        mStats.listsF3d++;
    }
    return true;
}

void Interp::Walk(const ListRef& root) {
    mSp = 0;
    if (!PushList(root)) {
        return;
    }
    while (mSp > 0) {
        Frame& f = mStack[mSp - 1];
        if (f.idx >= f.list.limit) {
            /* No terminator inside the readable bound: the bridge appends an ENDDL here. */
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
        mBackend->VerifyNoteCmd(c.w0, c.w1);
        const int r = Exec(c, f, f3dXfer);
        if (r == 1) {
            mSp--;
        }
    }
}

/* Legacy F3D -> F3DEX2, one command. Mirrors n64_gfx_bridge.cpp ProcessList's isF3DSource
 * arms (cases 0x01/0x03/0x04/0x06/0xB6..0xBF). Everything else passes through unchanged,
 * exactly as the bridge's default arm emits it. */
bool Interp::RemapF3d(Cmd& c, const ListRef& list, size_t* f3dXferSize) {
    (void)list;
    const uint8_t op = Opcode(c.w0);
    switch (op) {
        case F3D_MTX: {
            const uint8_t legacy = static_cast<uint8_t>(C0(c.w0, 16, 8));
            uint8_t params = 0;
            if (legacy & 0x01u) params |= MTX_PROJECTION;
            if (legacy & 0x02u) params |= MTX_LOAD;
            if (legacy & 0x04u) params |= MTX_PUSH;
            c.w0 = (static_cast<uint32_t>(OP_MTX) << 24) | static_cast<uint32_t>(params ^ 0x01u);
            return true;
        }
        case F3D_MOVEMEM: {
            const uint8_t legacyIndex = static_cast<uint8_t>(C0(c.w0, 16, 8));
            const size_t xfer = static_cast<size_t>(c.w0 & 0xFFFFu);
            uint8_t index = 0, offset = 0;
            if (legacyIndex == 0x80u) {
                index = MV_VIEWPORT;
            } else if (legacyIndex == 0x84u) {
                index = MV_LIGHT;
                offset = MVO_LOOKATX;
            } else if (legacyIndex == 0x82u) {
                index = MV_LIGHT;
                offset = MVO_LOOKATY;
            } else if (legacyIndex >= 0x86u && legacyIndex <= 0x94u && ((legacyIndex - 0x86u) & 1u) == 0) {
                index = MV_LIGHT;
                offset = static_cast<uint8_t>(MVO_L0 + ((legacyIndex - 0x86u) / 2u) * 24u);
            } else {
                return false;
            }
            c.w0 = (static_cast<uint32_t>(OP_MOVEMEM) << 24) | (static_cast<uint32_t>(offset / 8u) << 8) | index;
            *f3dXferSize = xfer;
            return true;
        }
        case F3D_VTX: {
            const uint16_t len = static_cast<uint16_t>(c.w0 & 0xFFFFu);
            const uint8_t n = (len >= 16u) ? static_cast<uint8_t>(len / 16u) : 1u;
            const uint8_t v0 = static_cast<uint8_t>(C0(c.w0, 16, 8) & 0x0Fu);
            c.w0 = (static_cast<uint32_t>(OP_VTX) << 24) | (static_cast<uint32_t>(n) << 12) |
                   static_cast<uint32_t>((v0 + n) * 2u);
            return true;
        }
        case F3D_DL:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_DL) << 24);
            return true;
        case F3D_CLEARGEOMETRYMODE:
            c.w0 = (static_cast<uint32_t>(OP_GEOMETRYMODE) << 24) | (~c.w1 & 0x00FFFFFFu);
            c.w1 = 0;
            return true;
        case F3D_SETGEOMETRYMODE:
            c.w0 = (static_cast<uint32_t>(OP_GEOMETRYMODE) << 24) | 0x00FFFFFFu;
            return true;
        case F3D_ENDDL:
            c.w0 = static_cast<uint32_t>(OP_ENDDL) << 24;
            c.w1 = 0;
            return true;
        case F3D_SETOTHERMODE_L:
        case F3D_SETOTHERMODE_H: {
            const uint32_t shift = C0(c.w0, 8, 8);
            const uint32_t length = C0(c.w0, 0, 8);
            if (length == 0u || shift + length > 32u) {
                return false;
            }
            const uint8_t ex2 = (op == F3D_SETOTHERMODE_L) ? OP_SETOTHERMODE_L : OP_SETOTHERMODE_H;
            c.w0 = (static_cast<uint32_t>(ex2) << 24) | ((32u - shift - length) << 8) | (length - 1u);
            return true;
        }
        case F3D_TEXTURE:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_TEXTURE) << 24);
            return true;
        case F3D_MOVEWORD:
            c.w0 = (c.w0 & 0x00FFFFFFu) | (static_cast<uint32_t>(OP_MOVEWORD) << 24);
            return true;
        case F3D_POPMTX:
            c.w0 = static_cast<uint32_t>(OP_POPMTX) << 24;
            c.w1 = 64u;
            return true;
        case F3D_CULLDL:
            return false;
        case F3D_TRI1: {
            const uint8_t v0 = static_cast<uint8_t>(C1(c.w1, 16, 8)) / 10u;
            const uint8_t v1 = static_cast<uint8_t>(C1(c.w1, 8, 8)) / 10u;
            const uint8_t v2 = static_cast<uint8_t>(C1(c.w1, 0, 8)) / 10u;
            c.w0 = (static_cast<uint32_t>(OP_TRI1) << 24) | (static_cast<uint32_t>(v0 * 2u) << 16) |
                   (static_cast<uint32_t>(v1 * 2u) << 8) | static_cast<uint32_t>(v2 * 2u);
            c.w1 = 0;
            return true;
        }
        default:
            return true;
    }
}

static inline uint32_t ColorComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xFu) | ((b & 0xFu) << 4) | ((c & 0x1Fu) << 8) | ((d & 7u) << 13);
}
static inline uint32_t AlphaComb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7u) | ((b & 7u) << 3) | ((c & 7u) << 6) | ((d & 7u) << 9);
}

static inline Rgba RgbaOf(uint32_t w1) {
    return Rgba{ static_cast<uint8_t>(w1 >> 24), static_cast<uint8_t>(w1 >> 16), static_cast<uint8_t>(w1 >> 8),
                 static_cast<uint8_t>(w1) };
}

/* == gfx_set_combine_handler_rdp: the LUS color_comb/alpha_comb packing. Shared by the
 * walker and the replay seam (template-embedded SETCOMBINE, machine material runs). */
void Interp::SetCombineWords(uint32_t w0, uint32_t w1) {
    DpSetCombineMode(ColorComb(C0(w0, 20, 4), C1(w1, 28, 4), C0(w0, 15, 5), C1(w1, 15, 3)),
                     AlphaComb(C0(w0, 12, 3), C1(w1, 12, 3), C0(w0, 9, 3), C1(w1, 9, 3)),
                     ColorComb(C0(w0, 5, 4), C1(w1, 24, 4), C0(w0, 0, 5), C1(w1, 6, 3)),
                     AlphaComb(C1(w1, 21, 3), C1(w1, 3, 3), C1(w1, 18, 3), C1(w1, 0, 3)));
}

int Interp::Exec(const Cmd& c, Frame& f, size_t f3dXferSize) {
    const uint8_t op = Opcode(c.w0);
    switch (op) {
        /* ---------------- flow ---------------- */
        case OP_NOOP:
        case OP_SPNOOP:
            /* == gfx_noop_handler_f3dex2 (the F3DEX2 table binds both opcodes to it): the
             * port's marker replay gets the two words as-is; p=7/8 (OPEN/CLOSE_DISPS) is
             * bookkeeping the hook ignores, as the LUS handler's exec-stack does on this port. */
            if (mNoopHook != nullptr) {
                mNoopHook(mNoopHookUser, c.w0, c.w1, this);
            }
            return 0;
        case OP_CULLDL: /* no-op today, as in the LUS path */
        case OP_RDPLOADSYNC:
        case OP_RDPPIPESYNC: /* pipesync is a no-op on 3DS (lus-3ds-pipesync-noop) */
        case OP_RDPTILESYNC:
        case OP_RDPFULLSYNC:
            return 0;

        case OP_ENDDL:
            return 1;

        case OP_DL: {
            ListRef sub;
            if (!mSrc.ResolveList(c, sub)) {
                mStats.noopLists++;
                return 0;
            }
            if (mDlHook != nullptr && mDlHook(mDlHookUser, sub.base, this) != 0) {
                return 0;
            }
            if (C0(c.w0, 16, 1) == 0) {
                PushList(sub);
            } else {
                f.list = sub;
                f.idx = 0;
                mStats.lists++;
                mStats.listsByClass[static_cast<int>(ClassOf(sub))]++;
                if (sub.isF3d) mStats.listsF3d++;
            }
            return 2;
        }

        case OP_RDPHALF_1: {
            /* Latched for a following G_BRANCH_Z (the bridge resolves the half as a list only
             * when the next command is BRANCH_Z); TEXRECT consumes its own halves inline. */
            mBranchZValid = false;
            if (f.idx < f.list.limit && Opcode(ReadCmd(f.list, f.idx).w0) == OP_BRANCH_Z) {
                mBranchZValid = mSrc.ResolveList(c, mBranchZTarget);
            }
            return 0;
        }

        case OP_BRANCH_Z: {
            const bool valid = mBranchZValid;
            mBranchZValid = false;
            const uint8_t vbidx = static_cast<uint8_t>((c.w0 & 0xFFFu) >> 1);
            const uint32_t zval = c.w1;
            if (!valid || vbidx >= kMaxVertices) {
                return 0;
            }
            const LoadedVertex& v = mRsp.loaded_vertices[vbidx];
            const float screenZ = (fabsf(v.w) > 0.000001f)
                                      ? ((v.z / v.w) * mRsp.viewport_z_scale + mRsp.viewport_z_trans)
                                      : mRsp.viewport_z_trans;
            const uint32_t screenZFixed = static_cast<uint32_t>(Clampf(screenZ, 0.0f, 65535.0f) * 65536.0f);
            if (screenZFixed <= zval || (mRsp.extra_geometry_mode & 0x00000002u) != 0) {
                mStats.branchZTaken++;
                f.list = mBranchZTarget;
                f.idx = 0;
                return 2;
            }
            return 0;
        }

        case OP_LOAD_UCODE: {
            int variant = 0, isLine = 0;
            if (DlSrc::UcodeVariant(c, &variant, &isLine) == 0) {
                mStats.droppedOps++;
                return 0;
            }
            mVariant = variant;
            mLineSection = isLine != 0;
            return 0;
        }

        /* ---------------- RSP ---------------- */
        case OP_MTX:
            SpMatrix(static_cast<uint8_t>(C0(c.w0, 0, 8) ^ MTX_PUSH), c, f.list);
            return 0;
        case OP_POPMTX:
            SpPopMatrix(c.w1 / 64u);
            return 0;
        case OP_VTX:
            SpVertex(C0(c.w0, 12, 8), C0(c.w0, 1, 7) - C0(c.w0, 12, 8), c, f.list);
            return 0;
        case OP_MODIFYVTX:
            SpModifyVertex(static_cast<uint16_t>(C0(c.w0, 1, 15)), static_cast<uint8_t>(C0(c.w0, 16, 8)), c.w1);
            return 0;
        case OP_TRI1:
            SpTri(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                  static_cast<uint8_t>(C0(c.w0, 0, 8) / 2), false);
            return 0;
        case OP_TRI2:
        case OP_QUAD:
            SpTri(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                  static_cast<uint8_t>(C0(c.w0, 0, 8) / 2), false);
            SpTri(static_cast<uint8_t>(C1(c.w1, 16, 8) / 2), static_cast<uint8_t>(C1(c.w1, 8, 8) / 2),
                  static_cast<uint8_t>(C1(c.w1, 0, 8) / 2), false);
            return 0;
        case OP_LINE3D:
            if (!mLineSection) {
                mStats.droppedOps++;
                return 0;
            }
            SpLine3d(static_cast<uint8_t>(C0(c.w0, 16, 8) / 2), static_cast<uint8_t>(C0(c.w0, 8, 8) / 2),
                     static_cast<uint8_t>(C0(c.w0, 0, 8)));
            return 0;
        case OP_GEOMETRYMODE:
            SpGeometryMode(~C0(c.w0, 0, 24), c.w1);
            return 0;
        case OP_EXTRAGEOMETRYMODE:
            mRsp.extra_geometry_mode &= ~(~C0(c.w0, 0, 24));
            mRsp.extra_geometry_mode |= c.w1;
            return 0;
        case OP_MOVEMEM:
            SpMovemem(static_cast<uint8_t>(C0(c.w0, 0, 8)), static_cast<uint8_t>(C0(c.w0, 8, 8) * 8), c,
                      f3dXferSize, f.list);
            return 0;
        case OP_MOVEWORD:
            SpMoveword(static_cast<uint8_t>(C0(c.w0, 16, 8)), static_cast<uint16_t>(C0(c.w0, 0, 16)), c);
            return 0;
        case OP_TEXTURE:
            SpTexture(static_cast<uint16_t>(C1(c.w1, 16, 16)), static_cast<uint16_t>(C1(c.w1, 0, 16)),
                      static_cast<uint8_t>(C0(c.w0, 11, 3)), static_cast<uint8_t>(C0(c.w0, 8, 3)),
                      static_cast<uint8_t>(C0(c.w0, 1, 7)));
            return 0;
        case OP_SETOTHERMODE_L:
            SpSetOtherMode(31 - C0(c.w0, 8, 8) - C0(c.w0, 0, 8), C0(c.w0, 0, 8) + 1, c.w1);
            return 0;
        case OP_SETOTHERMODE_H:
            SpSetOtherMode(63 - C0(c.w0, 8, 8) - C0(c.w0, 0, 8), C0(c.w0, 0, 8) + 1,
                           static_cast<uint64_t>(c.w1) << 32);
            return 0;
        case OP_DMA_IO:
            SpDmaIo(c);
            return 0;

        /* ---------------- RDP ---------------- */
        case OP_RDPSETOTHERMODE:
            DpSetOtherMode(C0(c.w0, 0, 24), c.w1);
            return 0;
        case OP_SETSCISSOR:
            DpSetScissor(C1(c.w1, 24, 2), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12), C1(c.w1, 0, 12));
            return 0;
        case OP_SETTIMG:
            DpSetTextureImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 12) + 1, c);
            return 0;
        case OP_SETTILE:
            DpSetTile(static_cast<uint8_t>(C0(c.w0, 21, 3)), C0(c.w0, 19, 2), C0(c.w0, 9, 9), C0(c.w0, 0, 9),
                      static_cast<uint8_t>(C1(c.w1, 24, 3)), C1(c.w1, 20, 4), C1(c.w1, 18, 2), C1(c.w1, 14, 4),
                      C1(c.w1, 10, 4), C1(c.w1, 8, 2), C1(c.w1, 4, 4), C1(c.w1, 0, 4));
            return 0;
        case OP_SETTILESIZE:
            DpSetTileSize(static_cast<uint8_t>(C1(c.w1, 24, 3)), static_cast<uint16_t>(C0(c.w0, 12, 12)),
                          static_cast<uint16_t>(C0(c.w0, 0, 12)), static_cast<uint16_t>(C1(c.w1, 12, 12)),
                          static_cast<uint16_t>(C1(c.w1, 0, 12)));
            return 0;
        case OP_LOADTLUT:
            DpLoadTlut(static_cast<uint8_t>(C1(c.w1, 24, 3)), C1(c.w1, 14, 10));
            return 0;
        case OP_LOADBLOCK:
            DpLoadBlock(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                        C1(c.w1, 0, 12));
            return 0;
        case OP_LOADTILE:
            DpLoadTile(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                       C1(c.w1, 0, 12));
            return 0;
        case OP_SETCOMBINE:
            SetCombineWords(c.w0, c.w1);
            return 0;
        case OP_SETENVCOLOR:
            DpSetEnvColor(RgbaOf(c.w1));
            return 0;
        case OP_SETPRIMCOLOR:
            DpSetPrimColor(static_cast<uint8_t>(C0(c.w0, 8, 8)), static_cast<uint8_t>(C0(c.w0, 0, 8)), RgbaOf(c.w1));
            return 0;
        case OP_SETFOGCOLOR:
            DpSetFogColor(RgbaOf(c.w1));
            return 0;
        case OP_SETBLENDCOLOR:
            DpSetBlendColor(RgbaOf(c.w1));
            return 0;
        case OP_SETFILLCOLOR:
            DpSetFillColor(c.w1);
            return 0;
        case OP_SETPRIMDEPTH:
            DpSetPrimDepth(static_cast<uint16_t>((c.w1 >> 16) & 0x7FFFu));
            return 0;
        case OP_SETKEYR:
            mRdp.key_center.r = static_cast<uint8_t>(C1(c.w1, 8, 8));
            mRdp.key_scale.r = static_cast<uint8_t>(C1(c.w1, 0, 8));
            return 0;
        case OP_SETKEYGB:
            mRdp.key_center.g = static_cast<uint8_t>(C1(c.w1, 24, 8));
            mRdp.key_scale.g = static_cast<uint8_t>(C1(c.w1, 16, 8));
            mRdp.key_center.b = static_cast<uint8_t>(C1(c.w1, 8, 8));
            mRdp.key_scale.b = static_cast<uint8_t>(C1(c.w1, 0, 8));
            return 0;
        case OP_SETCONVERT:
            mRdp.convert_k[0] = static_cast<int16_t>(C0(c.w0, 13, 9));
            mRdp.convert_k[1] = static_cast<int16_t>(C0(c.w0, 4, 9));
            mRdp.convert_k[2] = static_cast<int16_t>(((c.w0 & 0xFu) << 5) | C1(c.w1, 27, 5));
            mRdp.convert_k[3] = static_cast<int16_t>(C1(c.w1, 18, 9));
            mRdp.convert_k[4] = static_cast<int16_t>(C1(c.w1, 9, 9));
            mRdp.convert_k[5] = static_cast<int16_t>(C1(c.w1, 0, 9));
            return 0;

        case OP_TEXRECT:
        case OP_TEXRECTFLIP: {
            /* The two trailing halves (RDPHALF_1: s,t; RDPHALF_2: dsdx,dtdy) belong to this
             * command; consume them here so RDPHALF_1 never latches a branch target. */
            if (f.idx + 2 > f.list.limit) {
                mStats.droppedOps++;
                f.idx = f.list.limit;
                return 0;
            }
            const Cmd h1 = ReadCmd(f.list, f.idx);
            const Cmd h2 = ReadCmd(f.list, f.idx + 1);
            f.idx += 2;
            mStats.op[Opcode(h1.w0)]++;
            mStats.op[Opcode(h2.w0)]++;
            mStats.cmds += 2;
            const int32_t lrx = static_cast<int32_t>(C0(c.w0, 12, 12));
            const int32_t lry = static_cast<int32_t>(C0(c.w0, 0, 12));
            const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
            const int32_t ulx = static_cast<int32_t>(C1(c.w1, 12, 12));
            const int32_t uly = static_cast<int32_t>(C1(c.w1, 0, 12));
            const int16_t uls = static_cast<int16_t>(C1(h1.w1, 16, 16));
            const int16_t ult = static_cast<int16_t>(C1(h1.w1, 0, 16));
            const int16_t dsdx = static_cast<int16_t>(C1(h2.w1, 16, 16));
            const int16_t dtdy = static_cast<int16_t>(C1(h2.w1, 0, 16));
            DpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, op == OP_TEXRECTFLIP);
            return 0;
        }
        case OP_FILLRECT:
            DpFillRectangle(static_cast<int32_t>(C1(c.w1, 12, 12)), static_cast<int32_t>(C1(c.w1, 0, 12)),
                            static_cast<int32_t>(C0(c.w0, 12, 12)), static_cast<int32_t>(C0(c.w0, 0, 12)));
            return 0;
        case OP_SETZIMG:
            DpSetZImage(c);
            return 0;
        case OP_SETCIMG:
            DpSetColorImage(C0(c.w0, 21, 3), C0(c.w0, 19, 2), C0(c.w0, 0, 11), c);
            return 0;
        case OP_RDPHALF_2:
            /* Only meaningful as a TEXRECT trailer (consumed above); stray = ignore. */
            return 0;

        default:
            mStats.unknownOps++;
            if (UnknownLoggedOnce(op)) {
                GDXN_LOG("[ninterp] unknown opcode %02X w0=%08X w1=%08X (class=%d f3d=%d)", op, c.w0, c.w1,
                         static_cast<int>(ClassOf(f.list)), f.list.isF3d ? 1 : 0);
            }
            return 0;
    }
}

} // namespace gdxn
