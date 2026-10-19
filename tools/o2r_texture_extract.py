#!/usr/bin/env python3
"""o2r_texture_extract.py -- decode OTEX (texture) resources out of a .o2r archive into PNGs.

COLD TOOL. Not part of the 3DS build, not imported by it, not referenced from
CMakeLists.txt anywhere. This is a desktop-only research/mockup helper for the
`nibottom` track (docs/research/native/nibottom-plan.md): pulling real pixels
for the bottom-screen race-HUD mockup (portraits, HUD digit strips, position
numerals, labels) out of the game's own .o2r archive instead of hand-drawing
placeholders.

FORMAT (from this repo's own Torch submodule docs, authoritative --
torch/docs/otr_format/mainpage.md + torch/docs/otr_format/texture.md --
cross-checked against the real parser in port/gdx_segment_source.c:112-120,
which reads the same 0x40-byte header's size field as "u32 little-endian"):

  A .o2r/.otr file is a plain ZIP archive (tools/o2r_harness/o2r_common.py
  already treats it as one). Every archive entry begins with a 64-byte (0x40)
  standard header:

      0x00  u8   byte order
      0x01  u8   is-custom-asset
      0x02  2B   padding
      0x04  u32  resource type (LE) -- magic, e.g. 'OTEX' = 0x4F544558
      0x08  u32  version (LE)
      0x0C  u64  unique asset id (LE)
      0x14  44B  reserved

  When the resource type is OTEX, a texture-specific header follows
  immediately at the same 0x40 offset (i.e. it OVERLAPS/replaces the tail of
  the generic layout -- this is the documented OTEX layout, taken as-is):

      0x40  u32  texture type id (LE)   -- see TEXTURE_TYPES below
      0x44  u32  width (LE)
      0x48  u32  height (LE)
      0x4C  u32  data size (LE)
      0x50  ...  raw texture data, `data size` bytes

  Multi-byte header INTEGERS are little-endian (confirmed against the real
  C reader, not just the doc). The raw PIXEL payload for N64-sourced formats
  (RGBA16/CI4/CI8/IA*) is left exactly as the N64 stored it in VRAM, which is
  BIG-ENDIAN 16-bit words for RGBA16/IA16 and plain byte-per-index/intensity
  for the 8-bit-and-under formats -- the same convention this project's own
  minimap TLUT code calls out (gdx3ds_racemap.c's MINIMAP_TLUT_ENTRY comment,
  bottomhud-progress.md SS4a: "the CI decoder reads palette bytes
  big-endian"). This decoder follows that convention for RGBA16/IA16 and
  treats CI4/CI8/I4/I8/IA4/IA8 as plain big-endian-irrelevant byte/nibble
  streams.

WHAT THIS SCRIPT DOES NOT DO: write .3dst, touch libultraship, or run on
device. It is a read side-tool. Stdlib only (same convention as
tools/o2r_harness/o2r_common.py) except PNG writing, which is also hand-rolled
here (zlib + binascii, both stdlib) rather than pulling in Pillow, so this
runs anywhere python3 does.

USAGE:
    python3 tools/o2r_texture_extract.py list ARCHIVE.o2r [--filter REGEX]
    python3 tools/o2r_texture_extract.py extract ARCHIVE.o2r --out DIR [--filter REGEX]
    python3 tools/o2r_texture_extract.py selftest

`selftest` builds synthetic OTEX entries in memory (a few formats, a known
checkerboard pattern) and round-trips them through the same decode path real
entries use, so the decoder's correctness does not depend on having a real
archive on hand.
"""

import argparse
import binascii
import re
import struct
import sys
import zipfile
import zlib
from pathlib import Path

STD_HEADER_BYTES = 0x40
OTEX_MAGIC = 0x4F544558  # 'OTEX', big-endian ASCII reading, matches torch/docs

# texture.md's "Texture Types" table, verbatim.
TYPE_ERROR = 0
TYPE_RGBA32 = 1
TYPE_RGBA16 = 2
TYPE_CI4 = 3
TYPE_CI8 = 4
TYPE_I4 = 5
TYPE_I8 = 6
TYPE_IA4 = 7
TYPE_IA8 = 8
TYPE_IA16 = 9
TYPE_I1 = 10
TYPE_TLUT = 11  # exported as RGBA16bpp per texture.md's note; decode as such

TEXTURE_TYPES = {
    TYPE_ERROR: "error",
    TYPE_RGBA32: "rgba32",
    TYPE_RGBA16: "rgba16",
    TYPE_CI4: "ci4",
    TYPE_CI8: "ci8",
    TYPE_I4: "i4",
    TYPE_I8: "i8",
    TYPE_IA4: "ia4",
    TYPE_IA8: "ia8",
    TYPE_IA16: "ia16",
    TYPE_I1: "i1",
    TYPE_TLUT: "tlut",
}


class DecodeError(Exception):
    pass


def parse_standard_header(data):
    """Returns dict with byteOrder/isCustom/resourceType/version/assetId, or
    raises DecodeError if `data` is too short to even hold the header."""
    if len(data) < STD_HEADER_BYTES:
        raise DecodeError("entry is %d bytes, shorter than the 0x40 standard header" % len(data))
    byte_order, is_custom = data[0], data[1]
    resource_type = struct.unpack_from("<I", data, 0x04)[0]
    version = struct.unpack_from("<I", data, 0x08)[0]
    asset_id = struct.unpack_from("<Q", data, 0x0C)[0]
    return {
        "byteOrder": byte_order,
        "isCustom": is_custom,
        "resourceType": resource_type,
        "version": version,
        "assetId": asset_id,
    }


def parse_otex(data):
    """Parses the OTEX texture header + payload. `data` is the WHOLE entry
    (standard header included). Returns dict: typeId, typeName, width,
    height, dataSize, pixels (raw bytes, exactly dataSize long)."""
    header = parse_standard_header(data)
    if header["resourceType"] != OTEX_MAGIC:
        raise DecodeError(
            "resourceType 0x%08X is not OTEX (0x%08X)" % (header["resourceType"], OTEX_MAGIC)
        )
    if len(data) < 0x50:
        raise DecodeError("entry is %d bytes, shorter than 0x50 (OTEX header)" % len(data))
    type_id, width, height, data_size = struct.unpack_from("<IIII", data, 0x40)
    pixels = data[0x50 : 0x50 + data_size]
    if len(pixels) != data_size:
        raise DecodeError(
            "OTEX dataSize=%d but only %d bytes follow the header" % (data_size, len(pixels))
        )
    header.update(
        {
            "typeId": type_id,
            "typeName": TEXTURE_TYPES.get(type_id, "unknown(%d)" % type_id),
            "width": width,
            "height": height,
            "dataSize": data_size,
            "pixels": pixels,
        }
    )
    return header


def _rgba5551_to_rgba8(hi, lo):
    """One big-endian RGBA5551 pixel (two bytes, N64 order) -> (r,g,b,a) 8bpc."""
    v = (hi << 8) | lo
    r5 = (v >> 11) & 0x1F
    g5 = (v >> 6) & 0x1F
    b5 = (v >> 1) & 0x1F
    a1 = v & 0x1
    # standard N64 5->8 expansion, same as this project's ORACLE: (v<<3)|(v>>2)
    expand5 = lambda c: (c << 3) | (c >> 2)
    return expand5(r5), expand5(g5), expand5(b5), 255 if a1 else 0


def decode_to_rgba8(tex, palette=None):
    """tex: a parse_otex() dict. palette: optional list of (r,g,b,a) for
    CI4/CI8, itself decoded from a paired TLUT (RGBA16) entry. Returns a flat
    bytearray of width*height*4 RGBA8 bytes, row-major, top row first."""
    w, h, data = tex["width"], tex["height"], tex["pixels"]
    out = bytearray(w * h * 4)
    t = tex["typeId"]

    if t == TYPE_RGBA32:
        need = w * h * 4
        if len(data) < need:
            raise DecodeError("rgba32: need %d bytes, have %d" % (need, len(data)))
        out[: w * h * 4] = data[:need]

    elif t in (TYPE_RGBA16, TYPE_TLUT):
        need = w * h * 2
        if len(data) < need:
            raise DecodeError("rgba16: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            r, g, b, a = _rgba5551_to_rgba8(data[2 * i], data[2 * i + 1])
            out[4 * i : 4 * i + 4] = bytes((r, g, b, a))

    elif t in (TYPE_CI4, TYPE_CI8):
        if palette is None:
            raise DecodeError("%s texture needs a palette (--tlut / paired _tlut entry)" % tex["typeName"])
        if t == TYPE_CI8:
            need = w * h
            if len(data) < need:
                raise DecodeError("ci8: need %d bytes, have %d" % (need, len(data)))
            for i in range(w * h):
                idx = data[i]
                out[4 * i : 4 * i + 4] = bytes(palette[idx % len(palette)])
        else:  # CI4, two indices per byte, high nibble first
            need = (w * h + 1) // 2
            if len(data) < need:
                raise DecodeError("ci4: need %d bytes, have %d" % (need, len(data)))
            for i in range(w * h):
                byte = data[i // 2]
                idx = (byte >> 4) & 0xF if (i % 2 == 0) else byte & 0xF
                out[4 * i : 4 * i + 4] = bytes(palette[idx % len(palette)])

    elif t == TYPE_I4:
        need = (w * h + 1) // 2
        if len(data) < need:
            raise DecodeError("i4: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            byte = data[i // 2]
            nib = (byte >> 4) & 0xF if (i % 2 == 0) else byte & 0xF
            v = (nib << 4) | nib
            out[4 * i : 4 * i + 4] = bytes((v, v, v, 255))

    elif t == TYPE_I8:
        need = w * h
        if len(data) < need:
            raise DecodeError("i8: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            v = data[i]
            out[4 * i : 4 * i + 4] = bytes((v, v, v, 255))

    elif t == TYPE_IA4:
        need = (w * h + 1) // 2
        if len(data) < need:
            raise DecodeError("ia4: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            byte = data[i // 2]
            nib = (byte >> 4) & 0xF if (i % 2 == 0) else byte & 0xF
            inten3 = (nib >> 1) & 0x7
            a1 = nib & 0x1
            v = (inten3 << 5) | (inten3 << 2) | (inten3 >> 1)
            out[4 * i : 4 * i + 4] = bytes((v, v, v, 255 if a1 else 0))

    elif t == TYPE_IA8:
        need = w * h
        if len(data) < need:
            raise DecodeError("ia8: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            byte = data[i]
            inten4 = (byte >> 4) & 0xF
            a4 = byte & 0xF
            v = (inten4 << 4) | inten4
            a = (a4 << 4) | a4
            out[4 * i : 4 * i + 4] = bytes((v, v, v, a))

    elif t == TYPE_IA16:
        need = w * h * 2
        if len(data) < need:
            raise DecodeError("ia16: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            v = data[2 * i]
            a = data[2 * i + 1]
            out[4 * i : 4 * i + 4] = bytes((v, v, v, a))

    elif t == TYPE_I1:
        need = (w * h + 7) // 8
        if len(data) < need:
            raise DecodeError("i1: need %d bytes, have %d" % (need, len(data)))
        for i in range(w * h):
            byte = data[i // 8]
            bit = (byte >> (7 - (i % 8))) & 0x1
            v = 255 if bit else 0
            out[4 * i : 4 * i + 4] = bytes((v, v, v, 255))

    else:
        raise DecodeError("unsupported texture type id %d (%s)" % (t, tex["typeName"]))

    return out


def write_png(path_or_none, width, height, rgba8):
    """Minimal stdlib PNG encoder (no filtering beyond per-row 'none'). If
    path_or_none is None, returns the PNG bytes instead of writing a file."""

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", binascii.crc32(tag + payload) & 0xFFFFFFFF)
        )

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8bpc RGBA, no interlace
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 ("none") per scanline
        raw += rgba8[y * stride : (y + 1) * stride]
    idat = zlib.compress(bytes(raw), 9)
    png = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    if path_or_none is None:
        return png
    Path(path_or_none).write_bytes(png)
    return png


def data_uri_png(width, height, rgba8):
    import base64

    png = write_png(None, width, height, rgba8)
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


# --------------------------------------------------------------------------- CLI

def _open_entries(archive_path):
    with zipfile.ZipFile(archive_path) as z:
        return {info.filename: z.read(info.filename) for info in z.infolist()}


def cmd_list(args):
    entries = _open_entries(args.archive)
    pat = re.compile(args.filter) if args.filter else None
    matched = 0
    for name, data in entries.items():
        if pat and not pat.search(name):
            continue
        try:
            header = parse_standard_header(data)
        except DecodeError as e:
            print("%-48s SKIP (%s)" % (name, e))
            continue
        if header["resourceType"] == OTEX_MAGIC:
            try:
                tex = parse_otex(data)
                print(
                    "%-48s OTEX %-9s %4dx%-4d %6d bytes"
                    % (name, tex["typeName"], tex["width"], tex["height"], tex["dataSize"])
                )
                matched += 1
            except DecodeError as e:
                print("%-48s OTEX but malformed (%s)" % (name, e))
        else:
            print("%-48s resourceType=0x%08X (not OTEX)" % (name, header["resourceType"]))
    print("-- %d OTEX texture entries matched of %d total entries" % (matched, len(entries)))


def cmd_extract(args):
    entries = _open_entries(args.archive)
    pat = re.compile(args.filter) if args.filter else None
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Build a name -> decoded-palette cache for any *_tlut entries present,
    # since CI4/CI8 textures need their paired TLUT to produce real colour.
    palettes = {}
    for name, data in entries.items():
        if not name.endswith("_tlut"):
            continue
        try:
            tex = parse_otex(data)
            rgba = decode_to_rgba8(tex)
            palettes[name] = [tuple(rgba[i : i + 4]) for i in range(0, len(rgba), 4)]
        except DecodeError:
            continue

    written = 0
    for name, data in entries.items():
        if pat and not pat.search(name):
            continue
        try:
            header = parse_standard_header(data)
        except DecodeError:
            continue
        if header["resourceType"] != OTEX_MAGIC:
            continue
        try:
            tex = parse_otex(data)
            palette = palettes.get(name + "_tlut")
            rgba = decode_to_rgba8(tex, palette=palette)
        except DecodeError as e:
            print("skip %s: %s" % (name, e))
            continue
        safe = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
        out_path = out_dir / (safe + ".png")
        write_png(out_path, tex["width"], tex["height"], rgba)
        print("%-48s -> %s" % (name, out_path))
        written += 1
    print("-- wrote %d PNGs to %s" % (written, out_dir))


def _make_synthetic_otex(type_id, width, height, pixel_bytes, name="selftest"):
    """Builds one well-formed OTEX archive-entry's worth of bytes in memory:
    the 0x40 standard header + the OTEX header + the payload. Used only by
    `selftest`; never written to a real archive."""
    std = bytearray(STD_HEADER_BYTES)
    std[0] = 1  # byteOrder = big (matches VERSION_ENDIANNESS_BIG convention elsewhere)
    std[1] = 0
    struct.pack_into("<I", std, 0x04, OTEX_MAGIC)
    struct.pack_into("<I", std, 0x08, 1)  # version
    struct.pack_into("<Q", std, 0x0C, 0xA11CE)  # asset id, arbitrary
    otex_hdr = struct.pack("<IIII", type_id, width, height, len(pixel_bytes))
    return bytes(std) + otex_hdr + pixel_bytes


def cmd_selftest(args):
    failures = 0

    # RGBA16: a 2x2 checkerboard, pure red / pure cyan, alpha bit set on both.
    # Big-endian N64 word: R=11111 G=00000 B=00000 A=1 -> 0xF801 ; cyan-ish:
    # R=00000 G=11111 B=11111 A=1 -> 0x07FF.
    px = bytes([0xF8, 0x01, 0x07, 0xFF, 0x07, 0xFF, 0xF8, 0x01])
    entry = _make_synthetic_otex(TYPE_RGBA16, 2, 2, px)
    tex = parse_otex(entry)
    rgba = decode_to_rgba8(tex)
    expect_red = (255, 0, 0, 255)
    expect_cyan = (0, 255, 255, 255)
    got = [tuple(rgba[i : i + 4]) for i in range(0, 16, 4)]
    want = [expect_red, expect_cyan, expect_cyan, expect_red]
    ok = got == want
    print("[selftest] rgba16 2x2 checkerboard decode: %s (got=%s want=%s)" % ("OK" if ok else "FAIL", got, want))
    failures += 0 if ok else 1

    # CI8 with a 2-entry palette: index 0 -> black, index 1 -> white.
    tlut_px = bytes([0x00, 0x01, 0xFF, 0xFF])  # two RGBA5551 big-endian words: ~black, white
    tlut_entry = _make_synthetic_otex(TYPE_RGBA16, 2, 1, tlut_px)
    tlut_tex = parse_otex(tlut_entry)
    palette = [tuple(x) for x in [decode_to_rgba8(tlut_tex)[i : i + 4] for i in range(0, 8, 4)]]
    ci_entry = _make_synthetic_otex(TYPE_CI8, 2, 2, bytes([0, 1, 1, 0]))
    ci_tex = parse_otex(ci_entry)
    ci_rgba = decode_to_rgba8(ci_tex, palette=palette)
    got2 = [tuple(ci_rgba[i : i + 4]) for i in range(0, 16, 4)]
    ok2 = got2[0] == palette[0] and got2[1] == palette[1] and got2[2] == palette[1] and got2[3] == palette[0]
    print("[selftest] ci8 + tlut palette decode: %s (got=%s)" % ("OK" if ok2 else "FAIL", got2))
    failures += 0 if ok2 else 1

    # Round-trip through the real PNG writer (just check it doesn't throw and
    # the IHDR says what we asked for).
    png = write_png(None, 2, 2, rgba)
    ok3 = png[:8] == b"\x89PNG\r\n\x1a\n" and png[16:20] == struct.pack(">I", 2) and png[20:24] == struct.pack(">I", 2)
    print("[selftest] PNG encode header sanity: %s" % ("OK" if ok3 else "FAIL"))
    failures += 0 if ok3 else 1

    # Negative test: a too-short entry must raise DecodeError, not crash some
    # other way -- this is what the real fzerox.o2r smoke-test fixture hits
    # (2 entries, 3 and 6 bytes, no header at all).
    try:
        parse_standard_header(b"FZX")
        ok4 = False
    except DecodeError:
        ok4 = True
    print("[selftest] short/non-OTR entry correctly refused: %s" % ("OK" if ok4 else "FAIL"))
    failures += 0 if ok4 else 1

    if failures:
        print("[selftest] %d FAILURE(S)" % failures)
        sys.exit(1)
    print("[selftest] all checks passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="list OTEX entries in an archive")
    p_list.add_argument("archive")
    p_list.add_argument("--filter", help="regex applied to entry names")
    p_list.set_defaults(func=cmd_list)

    p_extract = sub.add_parser("extract", help="decode matching OTEX entries to PNG")
    p_extract.add_argument("archive")
    p_extract.add_argument("--out", required=True)
    p_extract.add_argument("--filter", help="regex applied to entry names")
    p_extract.set_defaults(func=cmd_extract)

    p_self = sub.add_parser("selftest", help="decoder correctness check, no archive needed")
    p_self.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
