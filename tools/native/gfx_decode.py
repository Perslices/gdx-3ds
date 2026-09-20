#!/usr/bin/env python3
"""gfx_decode.py -- independent F3DEX2 decoder for the machines harness (test A) and for
reading binary DLs out of the o2r.

Modes:
  gfx_decode.py lists <outdir>            decode <outdir>/lists.bin (host-built 16-byte
                                          packets written by port/tests/machines_harness.c)
                                          into the canonical per-triangle text the C walker
                                          emits as walk.txt; prints to stdout.
  gfx_decode.py o2r <fzerox.o2r> <key>    disassemble a big-endian N64 DL asset (8-byte
                                          packets after the 64-byte OTR header), e.g.
                                          machine_custom_gfx/D_303A6A8 (race setup state).

The 'lists' decoder shares NOTHING with gdx3ds_mesh_walk.c beyond the packet layout: it
keeps its own RSP/RDP state machine (tile, scale, combine, prim, env, 64-slot vertex
buffer) and derives triangle vertices as (array base symbol, element) from syms.txt using
"greatest base <= pointer".
"""
import bisect
import struct
import sys
import zipfile

OP_VTX, OP_TRI1, OP_TRI2 = 0x01, 0x05, 0x06
OP_TEXTURE, OP_ENDDL, OP_PIPESYNC = 0xD7, 0xDF, 0xE7
OP_PRIM, OP_ENV, OP_COMBINE = 0xFA, 0xFB, 0xFC


def read_lists(path):
    data = open(path, "rb").read()
    pos = 0
    while pos < len(data):
        end = data.index(b"\0", pos)
        name = data[pos:end].decode()
        pos = end + 1
        kind, grey, n = struct.unpack_from("<III", data, pos)
        pos += 12
        raw = data[pos:pos + 16 * n]
        pos += 16 * n
        yield name, kind, grey, n, raw


def read_syms(path):
    bases, names = [], {}
    for line in open(path):
        a, k = line.split()
        a = int(a, 16)
        bases.append(a)
        names[a] = k
    bases.sort()
    return bases, names


def sym_of(ptr, bases, names):
    i = bisect.bisect_right(bases, ptr) - 1
    if i < 0:
        return "?"
    base = bases[i]
    if (ptr - base) % 16 != 0:
        return "?"
    return "%s+%d" % (names[base], (ptr - base) // 16)


class State:
    def __init__(self):
        self.tile = 0
        self.s = 0
        self.t = 0
        self.cc = None
        self.prim = None   # (rgba, min, lod)
        self.env = None
        self.slots = [None] * 64  # pointer to the Vtx (host address)
        self.dirty = True
        self.run = -1


def decode_lists(outdir):
    bases, names = read_syms(outdir + "/syms.txt")
    out = []
    for name, kind, grey, n, raw in read_lists(outdir + "/lists.bin"):
        if kind == 4:  # K_TEX: not walked
            continue
        st = State()
        tris = []
        ended = False
        for i in range(n):
            w0, w1 = struct.unpack_from("<I", raw, i * 16)[0], struct.unpack_from("<Q", raw, i * 16 + 8)[0]
            op = w0 >> 24
            if op == OP_ENDDL:
                ended = True
                break
            elif op == OP_PIPESYNC:
                pass
            elif op == OP_TEXTURE:
                st.s, st.t = (w1 >> 16) & 0xFFFF, w1 & 0xFFFF
                st.tile = (w0 >> 8) & 7
                st.dirty = True
            elif op == OP_COMBINE:
                st.cc = (w0 & 0xFFFFFF, w1 & 0xFFFFFFFF)
                st.dirty = True
            elif op == OP_PRIM:
                st.prim = (w1 & 0xFFFFFFFF, (w0 >> 8) & 0xFF, w0 & 0xFF)
                st.dirty = True
            elif op == OP_ENV:
                st.env = w1 & 0xFFFFFFFF
                st.dirty = True
            elif op == OP_VTX:
                cnt = (w0 >> 12) & 0xFF
                v0 = ((w0 >> 1) & 0x7F) - cnt
                ptr = w1 & 0xFFFFFFFF if struct.calcsize("P") == 4 else w1
                for k in range(cnt):
                    st.slots[v0 + k] = ptr + 16 * k
            elif op in (OP_TRI1, OP_TRI2):
                words = [w0] if op == OP_TRI1 else [w0, w1 & 0xFFFFFFFF]
                for w in words:
                    idx = (((w >> 16) & 0xFF) // 2, ((w >> 8) & 0xFF) // 2, (w & 0xFF) // 2)
                    key = (st.tile, st.s, st.t, st.cc, st.prim, st.env)
                    if st.dirty:
                        if st.run < 0 or tris[-1][0] != key:
                            st.run += 1
                        st.dirty = False
                    tris.append((key, st.run, tuple(sym_of(st.slots[j], bases, names) if st.slots[j] is not None else "?" for j in idx)))
            else:
                raise SystemExit("%s: unexpected opcode %02X at %d" % (name, op, i))
        if not ended:
            raise SystemExit("%s: no G_ENDDL" % name)
        runs = st.run + 1
        out.append("F %s grey=%d cmds=%d tris=%d runs=%d" % (name, grey, n, len(tris), runs))
        for key, run, verts in tris:
            tile, s, t, cc, prim, env = key
            ccs = "%06X:%08X" % cc if cc is not None else "NONE"
            prims = "%08X/%02X%02X" % prim if prim is not None else "I"
            envs = "%08X" % env if env is not None else "I"
            out.append("T %d tile=%d s=%04X t=%04X cc=%s prim=%s env=%s v=%s" % (run, tile, s, t, ccs, prims, envs, ",".join(verts)))
    return "\n".join(out) + "\n"


# ---- o2r binary DL disassembly (big-endian N64 packets) ---------------------------------
GEOM = {
    0x1: "ZBUFFER", 0x4: "SHADE", 0x200: "CULL_FRONT", 0x400: "CULL_BACK", 0x10000: "FOG",
    0x20000: "LIGHTING", 0x40000: "TEXTURE_GEN", 0x80000: "TEXTURE_GEN_LINEAR", 0x200000: "SHADING_SMOOTH",
    0x800000: "CLIPPING",
}


def geom_names(v):
    return "|".join(n for b, n in sorted(GEOM.items()) if v & b) or "0"


def disasm_o2r(o2r, key):
    z = zipfile.ZipFile(o2r)
    data = z.read(key)
    payload = data[64:]
    out = []
    for i in range(len(payload) // 8):
        w0, w1 = struct.unpack_from(">II", payload, i * 8)
        op = w0 >> 24
        if op == 0xD9:
            out.append("%3d G_GEOMETRYMODE clear=%s set=%s" % (i, geom_names(~w0 & 0xFFFFFF), geom_names(w1)))
        elif op == 0xE2:
            out.append("%3d G_SETOTHERMODE_L shift=%d len=%d data=%08X" % (i, 32 - ((w0 >> 8) & 0xFF) - ((w0 & 0xFF) + 1), (w0 & 0xFF) + 1, w1))
        elif op == 0xE3:
            out.append("%3d G_SETOTHERMODE_H shift=%d len=%d data=%08X" % (i, 32 - ((w0 >> 8) & 0xFF) - ((w0 & 0xFF) + 1), (w0 & 0xFF) + 1, w1))
        elif op == 0xDB:
            out.append("%3d G_MOVEWORD index=%02X offset=%04X data=%08X" % (i, (w0 >> 16) & 0xFF, w0 & 0xFFFF, w1))
        elif op == 0xD7:
            out.append("%3d G_TEXTURE level=%d tile=%d on=%d s=%04X t=%04X" % (i, (w0 >> 11) & 7, (w0 >> 8) & 7, (w0 >> 1) & 0x7F, w1 >> 16, w1 & 0xFFFF))
        elif op == 0xFC:
            out.append("%3d G_SETCOMBINE %06X:%08X" % (i, w0 & 0xFFFFFF, w1))
        elif op == 0xDF:
            out.append("%3d G_ENDDL" % i)
            break
        else:
            out.append("%3d op=%02X %08X %08X" % (i, op, w0, w1))
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) >= 3 and argv[1] == "lists":
        sys.stdout.write(decode_lists(argv[2]))
        return 0
    if len(argv) >= 4 and argv[1] == "o2r":
        sys.stdout.write(disasm_o2r(argv[2], argv[3]))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
