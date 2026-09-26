#!/usr/bin/env python3
"""G-Diffuser Expansion Kit ENGLISH TEXT PACK generator (NEXT.md B11m).

The problem this exists for, in the owner's words after the first hardware
session: "idk how to read japanese so the course/machine edits, i cant validate
beyond they 'work'." The Expansion Kit runs on the console, but every caption in
the course editor and the machine creator is Japanese, so "it launches" is as far
as validation can get.

The port ALREADY has a translated-disk seam:

  port/gen/EkTranslatedOverrides.c   three reshaped Create-Machine label textures
  port/gen/EkTranslatedStrings.c     the compiled-in C-initializer text
  port/disk_buffer.cpp               applies both, gated on the mounted image's
                                     CRC-64 being GDX_EK_TRANSLATED_DISK_CRC64

and that seam is DORMANT for everybody who does not own that one exact image.
Two separate reasons, both measured rather than assumed:

  1. The gate is an EXACT-IMAGE fingerprint. nddconv (B11l) showed an .ndd's
     whole-image CRC-64 necessarily covers the system area's per-medium defect
     map and the MFS RAM area, so two dumps of two physical discs can never
     agree. Handing a user "a translated .ndd" therefore does not reliably turn
     the seam on -- it turns it on for ONE file.
  2. It reads the English out of the mounted disk. A user with a retail-JP dump
     has no source of English bytes at all, and that is the dump the editors,
     the MFS volume with its 29 saved machines, and the DD cups all come from.

So this generator supplies the missing SOURCE: it bakes the translated art and
text into the build as a small constant, keyed to no image at all, so the user's
own dump stays authoritative for everything else -- drive presence, MFS, saves,
audio, courses -- while the words on screen come from the fan translation.

WHAT GOES IN THE PACK, and why each exclusion is a measurement

The row universe is port/gen/ek_slice_manifest.txt, the same 765 (symbol,
physical .ndd offset, size) rows gdx_ek_assets_fill() copies out of a mounted
image. Comparing the retail-JP disk against the translated release at ekcart's
constant delta, 131 of the 765 differ. They are NOT all translated content:

  23 FZX:SOUNDFONT rows      EXCLUDED. Their bytes are BYTE-IDENTICAL between
                             the two disks; they differ at a fixed offset only
                             because Zoinkity moved the soundfont table (user
                             LBA 1000 -> 831), so the same content sits 2,762,160
                             bytes earlier on the cart. Baking the bytes found at
                             the JP offsets would write whatever the cart happens
                             to store there over working audio tables. This tool
                             PROVES the relocation per row (--census prints it)
                             rather than trusting the classification.
   3 COMPRESSED_TEXTURE      EXCLUDED. aCopyrightDDTex, aCupSelectDD2Tex and
                             aMasterDDTrophyTex differ in their slot PADDING only:
                             MIO0-decoded, both disks yield byte-identical pixels.
                             Not translated; nothing to gain.
   1 GFX row                 EXCLUDED. D_xk1_80033630 is a display list, not a
                             glyph. The JP list draws English textures at the same
                             slot dimensions perfectly well, and a translated list
                             is drawing commands this port has not traced.
 104 texture rows            INCLUDED -- 102 raw + the two MIO0 help screens
                             (aExpansionKitEditControlHelpTex,
                             aExpansionKitCameraControlHelpTex), whose translated
                             streams decode to the declared 228x180 and are
                             SHORTER than the JP slot, so a slot-sized copy is
                             safe with room to spare.

Three of those 104 cannot be copied at their own offsets, because the translator
reshaped them and swapped two of them: aMachineCreateMachineNameTex,
aMachineCreateSettingsTex and D_xk3_80138930. For those the pack takes the rows
straight out of port/gen/EkTranslatedOverrides.c -- translated offset, translated
size, same destination symbol -- which is the table that already encodes that
recovery. The runtime raises gdx_ek_disk_is_translated() when the pack applies,
which is what makes the three decomp blit sites
(machine_create_draw.c:402/908, course_edit/191080.c:913) use the reshaped
geometry and expansion_kit/A3AE0.c:535 repoint its two menu textures.

Finally the pack carries port/gen/EkTranslatedStrings.c's array rows -- the
Course Edit tooltips and node-panel labels the decomp compiles in as Japanese C
initializers. Their English lives in the translated overlay .data, so those rows
are read from the translated source the same way. The three POINTER tables in
that file already hold English string literals in the repository and need no
bytes at all; the runtime repoints them.

CONTENT RULE. This tool reads the two images IN PLACE and writes exactly one
output that contains image bytes, port/gen/EkTranslatedTextPack.c, which is
.gitignore'd for the same reason port/gen/EkAssetPack.c is. It also writes
port/gen/ek_text_pack_manifest.txt, which is names, offsets and sizes only --
no bytes -- so a reviewer can see what is in a pack they cannot see.

USAGE

  tools/gen_ek_text_pack.py --translated <english .z64 or .ndd> \
                            --reference  <retail-JP .ndd or MAME dump>

  --translated  the LuigiBlood/Zoinkity English release. Either container:
                a 66,551,504-byte .z64 (the disk's user area verbatim at
                z64_off = ndd_off - 0x727D8, ekcart B11e) or a 64,931,840-byte
                block-linear .ndd.
  --reference   the retail-JP disk the pack is DIFFED against, so every row in
                the pack is one this tool measured to differ. A MAME-format dump
                is converted through tools/dev/mame2ndd.py into a temp file.
  --census      print the full per-row classification and exit without writing.

Without arguments the pack is written EMPTY (gEkTextPackRowCount == 0), which is
byte-for-byte the behaviour the build had before it existed. The file is always
written so the CMake source list is stable.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(REPO, "port", "gen")
MANIFEST_IN = os.path.join(GEN, "ek_slice_manifest.txt")
BINDINGS_C = os.path.join(GEN, "EkAssetBindings.c")
OVERRIDES_C = os.path.join(GEN, "EkTranslatedOverrides.c")
STRINGS_C = os.path.join(GEN, "EkTranslatedStrings.c")
OUT_C = os.path.join(GEN, "EkTranslatedTextPack.c")
OUT_MANIFEST = os.path.join(GEN, "ek_text_pack_manifest.txt")

NDD_BYTES = 64_931_840
MAME_BYTES = 70_627_520
# ekcart B11e: the .z64 carries the disk's user area block-linear and verbatim,
# with the 0x738C0 system area stripped, so z64_off = ndd_off - 0x727D8.
Z64_BYTES = 66_551_504
Z64_DELTA = 0x727D8

# Rows whose payload is not text and must never be taken from a foreign image.
# Kept as a TYPE rule rather than a symbol list so a regenerated manifest cannot
# silently widen it.
TEXTURE_TYPES = ("TEXTURE", "COMPRESSED_TEXTURE")


def fail(msg):
    sys.stderr.write("gen_ek_text_pack: " + msg + "\n")
    sys.exit(2)


# ---------------------------------------------------------------- MIO0 ------
def mio0_decode(buf):
    """Decode an MIO0 stream. Returns (bytes, bytes_consumed).

    Used for one purpose: telling a translated texture apart from a slot whose
    trailing PADDING happens to differ. Three of the five compressed rows are the
    latter and are excluded on this evidence.
    """
    if buf[:4] != b"MIO0":
        return None, 0
    dec_len, comp_off, uncomp_off = struct.unpack(">III", buf[4:16])
    out = bytearray()
    bitpos, bitbuf, nbits = 16, 0, 0
    c, u = comp_off, uncomp_off
    while len(out) < dec_len:
        if nbits == 0:
            if bitpos + 4 > len(buf):
                return None, 0
            bitbuf = struct.unpack(">I", buf[bitpos:bitpos + 4])[0]
            bitpos += 4
            nbits = 32
        if bitbuf & 0x80000000:
            if u >= len(buf):
                return None, 0
            out.append(buf[u])
            u += 1
        else:
            if c + 2 > len(buf):
                return None, 0
            w = struct.unpack(">H", buf[c:c + 2])[0]
            c += 2
            length, back = (w >> 12) + 3, (w & 0xFFF) + 1
            if back > len(out):
                return None, 0
            for _ in range(length):
                out.append(out[len(out) - back])
        bitbuf = (bitbuf << 1) & 0xFFFFFFFF
        nbits -= 1
    return bytes(out[:dec_len]), max(c, u, bitpos)


# ------------------------------------------------------------- parsing ------
def read_slice_manifest():
    rows = []
    with open(MANIFEST_IN, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] in ("version", "count", "textures"):
                continue
            rows.append({"name": parts[0], "offset": int(parts[1], 16),
                         "size": int(parts[2]), "type": parts[3]})
    if not rows:
        fail("no rows parsed from " + MANIFEST_IN)
    return rows


DECL_RE = re.compile(r"^\s*(u8|u16|u32|s8|s16|s32)\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\]\s*;")
CTYPE = {"u8": "unsigned char", "s8": "signed char", "u16": "unsigned short",
         "s16": "short", "u32": "unsigned int", "s32": "int"}
CSIZE = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4}


def read_binding_decls():
    """symbol -> (c type, element count, byte capacity), from the DEFINITIONS.

    The extern this tool emits must match EkAssetBindings.c's real definition, or
    sizeof() in the generated table would be a lie and the bounds check with it.
    """
    decls = {}
    with open(BINDINGS_C, "r") as f:
        for line in f:
            m = DECL_RE.match(line)
            if m:
                t, name, n = m.group(1), m.group(2), int(m.group(3))
                decls[name] = (CTYPE[t], n, n * CSIZE[t])
    if not decls:
        fail("no array definitions parsed from " + BINDINGS_C)
    return decls


OVERRIDE_RE = re.compile(
    r'\{\s*"([A-Za-z_]\w*)"\s*,\s*[A-Za-z_]\w*\s*,\s*0x([0-9A-Fa-f]+)U\s*,\s*(\d+)U\s*,')


def read_overrides():
    with open(OVERRIDES_C, "r") as f:
        text = f.read()
    rows = [{"name": m.group(1), "offset": int(m.group(2), 16), "size": int(m.group(3))}
            for m in OVERRIDE_RE.finditer(text)]
    if not rows:
        fail("no override rows parsed from " + OVERRIDES_C)
    return rows


STRING_RE = re.compile(
    r'\{\s*"([A-Za-z_]\w*)"\s*,\s*([A-Za-z_]\w*)\s*,\s*0x([0-9A-Fa-f]+)U\s*,\s*(\d+)U\s*,\s*([A-Za-z_0-9]+)\s*\}')
STRING_EXTERN_RE = re.compile(
    r"^\s*extern\s+(unsigned char|char)\s+([A-Za-z_]\w*)\s*\[\s*([A-Za-z_0-9]+)\s*\]\s*;")


def read_strings():
    """The gEkTranslatedStrings[] rows plus each destination's declared bound.

    gdx_ek_label_* are defined in EkTranslatedStrings.c itself rather than
    externed, so both shapes are collected.
    """
    with open(STRINGS_C, "r") as f:
        text = f.read()
    cap = {}
    m = re.search(r"#define\s+GDX_EK_TEXT_CAP\s+(\d+)",
                  open(os.path.join(GEN, "EkTranslatedStrings.h")).read())
    if not m:
        fail("GDX_EK_TEXT_CAP not found in EkTranslatedStrings.h")
    text_cap = int(m.group(1))
    for line in text.splitlines():
        em = STRING_EXTERN_RE.match(line)
        if em:
            bound = em.group(3)
            cap[em.group(2)] = text_cap if bound == "GDX_EK_TEXT_CAP" else int(bound)
    body = text[text.index("gEkTranslatedStrings[]"):]
    rows = []
    for m in STRING_RE.finditer(body):
        name, dest, off, size, capname = m.groups()
        rows.append({"name": name, "dest": dest, "offset": int(off, 16), "size": int(size),
                     "capacity": text_cap if capname == "GDX_EK_TEXT_CAP" else cap.get(dest, 0)})
    if not rows:
        fail("no string rows parsed from " + STRINGS_C)
    return rows, text_cap


# ------------------------------------------------------------- sources ------
class Source(object):
    """A translated or reference image, addressed in physical .ndd offsets."""

    def __init__(self, path, tmpdir, label):
        self.path = path
        self.label = label
        size = os.path.getsize(path)
        if size == Z64_BYTES:
            self.delta = Z64_DELTA
            self.kind = "z64"
            with open(path, "rb") as f:
                head = f.read(0x40)
            if head[:4] != b"\x80\x37\x12\x40":
                fail("%s: %s is %d bytes but not a big-endian .z64 (magic %s)"
                     % (label, path, size, head[:4].hex()))
            self.region = head[0x1B:0x1C].decode("latin-1")
        elif size == NDD_BYTES:
            self.delta = 0
            self.kind = "ndd"
            self.region = self._ndd_region(path)
        elif size == MAME_BYTES:
            conv = os.path.join(tmpdir, os.path.basename(path) + ".ndd")
            sys.stderr.write("gen_ek_text_pack: %s is a MAME dump; converting with "
                             "tools/dev/mame2ndd.py\n" % label)
            subprocess.check_call([sys.executable,
                                   os.path.join(REPO, "tools", "dev", "mame2ndd.py"),
                                   "tondd", path, conv], stdout=subprocess.DEVNULL)
            self.path = conv
            self.delta = 0
            self.kind = "ndd(from mame)"
            self.region = self._ndd_region(conv)
        else:
            fail("%s: %s is %d bytes; expected a %d-byte .z64, a %d-byte .ndd or a "
                 "%d-byte MAME dump" % (label, path, size, Z64_BYTES, NDD_BYTES, MAME_BYTES))
        self.f = open(self.path, "rb")
        self.size = os.path.getsize(self.path)

    @staticmethod
    def _ndd_region(path):
        # The Disk ID structure lives in the 0x738C0-byte system area; ekcart read
        # the same gameName field out of the .z64 header at 0x18. Only the region
        # letter is used here, and only to refuse baking Japanese into an English
        # pack, so a scan of the system area is enough and needs no zone tables.
        with open(path, "rb") as f:
            blob = f.read(0x738C0)
        i = blob.find(b"EFZ")
        return blob[i + 3:i + 4].decode("latin-1") if i >= 0 else "?"

    def whole(self):
        """The entire image, cached. Used once, to prove the soundfont relocation."""
        if not hasattr(self, "_whole"):
            with open(self.path, "rb") as f:
                self._whole = f.read()
        return self._whole

    def read(self, ndd_offset, size):
        off = ndd_offset - self.delta
        if off < 0 or off + size > self.size:
            return None
        self.f.seek(off)
        return self.f.read(size)


# --------------------------------------------------------------- main -------
def main():
    ap = argparse.ArgumentParser(description="EK English text pack generator")
    ap.add_argument("--translated", metavar="IMAGE",
                    help="the fan-translated English release (.z64 or .ndd)")
    ap.add_argument("--reference", metavar="IMAGE",
                    help="the retail-JP disk to diff against (.ndd or MAME dump)")
    ap.add_argument("--census", action="store_true",
                    help="print the per-row classification and write nothing")
    opts = ap.parse_args()

    if bool(opts.translated) != bool(opts.reference):
        fail("--translated and --reference go together: the pack is a DIFF, so a row "
             "is only baked once this tool has measured it to differ")

    rows = read_slice_manifest()
    decls = read_binding_decls()

    if not opts.translated:
        write_pack([], bytearray(), 0, "", "", census=None)
        print("EK text pack: EMPTY (no --translated given); the build behaves exactly as "
              "it did before the pack existed")
        return

    tmpdir = tempfile.mkdtemp(prefix="ektextpack-")
    tr = Source(opts.translated, tmpdir, "--translated")
    jp = Source(opts.reference, tmpdir, "--reference")

    if tr.region != "E":
        fail("--translated %s has Disk ID region '%s'; the fan-translated release is 'E' "
             "(retail Japanese is 'J'). Refusing rather than baking Japanese text into a "
             "pack whose whole purpose is English." % (opts.translated, tr.region))

    overrides = read_overrides()
    override_names = set(o["name"] for o in overrides)
    string_rows, text_cap = read_strings()

    census = {"identical": 0, "differ": 0, "soundfont_relocated": 0, "soundfont_other": 0,
              "padding_only": 0, "non_texture": 0, "included": 0}
    lines = []
    pack_rows = []
    blob = bytearray()

    def emit(name, dest, dest_c_type, capacity, payload, zero_tail, note):
        pack_rows.append({"name": name, "dest": dest, "ctype": dest_c_type,
                          "elems": decls[dest][1] if dest in decls else None,
                          "packOffset": len(blob), "size": len(payload),
                          "capacity": capacity, "zeroTail": 1 if zero_tail else 0,
                          "note": note})
        blob.extend(payload)
        census["included"] += 1

    for row in rows:
        a = jp.read(row["offset"], row["size"])
        b = tr.read(row["offset"], row["size"])
        if a is None or b is None:
            fail("row %s (0x%08X+%d) is outside one of the images" %
                 (row["name"], row["offset"], row["size"]))
        if a == b:
            census["identical"] += 1
            continue
        census["differ"] += 1

        if row["type"] == "FZX:SOUNDFONT":
            # Prove the relocation rather than assume it: the JP content must exist
            # verbatim somewhere in the translated image. If it ever does not, this
            # says so instead of silently excluding a row that mattered.
            found = tr.whole().find(a)
            if found >= 0:
                census["soundfont_relocated"] += 1
                lines.append("EXCLUDE %-44s soundfont, content byte-identical at a shifted "
                             "offset (shift %d)" % (row["name"], found - (row["offset"] - tr.delta)))
            else:
                census["soundfont_other"] += 1
                lines.append("EXCLUDE %-44s soundfont, content NOT found verbatim in the "
                             "translated image -- excluded anyway (not text)" % row["name"])
            continue

        if row["type"] not in TEXTURE_TYPES:
            census["non_texture"] += 1
            lines.append("EXCLUDE %-44s type %s is not a texture" % (row["name"], row["type"]))
            continue

        if row["type"] == "COMPRESSED_TEXTURE":
            pa, ca = mio0_decode(a)
            pb, cb = mio0_decode(b)
            if pa is None or pb is None:
                lines.append("EXCLUDE %-44s MIO0 stream did not decode on one side"
                             % row["name"])
                census["padding_only"] += 1
                continue
            if pa == pb:
                census["padding_only"] += 1
                lines.append("EXCLUDE %-44s MIO0 pixels IDENTICAL; only the slot padding "
                             "differs (%d/%d bytes consumed)" % (row["name"], cb, row["size"]))
                continue
            if cb > row["size"]:
                fail("row %s: the translated MIO0 stream consumes %d bytes but the slot is "
                     "%d" % (row["name"], cb, row["size"]))
            lines.append("INCLUDE %-44s MIO0, translated stream %d B of a %d B slot"
                         % (row["name"], cb, row["size"]))

        if row["name"] in override_names:
            # Reshaped and/or swapped by the translator; taken from the override
            # table below at its TRANSLATED offset instead of this one.
            lines.append("DEFER   %-44s reshaped by the translation; taken from "
                         "EkTranslatedOverrides.c" % row["name"])
            continue

        if row["name"] not in decls:
            fail("row %s has no array definition in EkAssetBindings.c" % row["name"])
        ctype, elems, cap = decls[row["name"]]
        if row["size"] > cap:
            fail("row %s: slice is %d bytes but %s is declared %d" %
                 (row["name"], row["size"], row["name"], cap))
        if row["type"] != "COMPRESSED_TEXTURE":
            lines.append("INCLUDE %-44s %d B at 0x%08X" % (row["name"], row["size"], row["offset"]))
        emit(row["name"], row["name"], ctype, cap, b, False,
             "same slot, translated content")

    for ov in overrides:
        payload = tr.read(ov["offset"], ov["size"])
        if payload is None:
            fail("override row %s is outside the translated image" % ov["name"])
        ctype, elems, cap = decls[ov["name"]]
        if ov["size"] > cap:
            fail("override row %s: %d bytes into a %d-byte symbol" % (ov["name"], ov["size"], cap))
        lines.append("INCLUDE %-44s override: translated offset 0x%08X, %d B"
                     % (ov["name"], ov["offset"], ov["size"]))
        emit(ov["name"], ov["name"], ctype, cap, payload, False,
             "reshaped by the translation; translated offset 0x%08X" % ov["offset"])

    for sr in string_rows:
        payload = tr.read(sr["offset"], sr["size"])
        if payload is None:
            fail("string row %s is outside the translated image" % sr["name"])
        if sr["size"] > sr["capacity"]:
            fail("string row %s: %d bytes into a %d-byte array" %
                 (sr["name"], sr["size"], sr["capacity"]))
        lines.append("INCLUDE %-44s string, %d B at 0x%08X" %
                     (sr["name"], sr["size"], sr["offset"]))
        emit(sr["name"], sr["dest"], None, sr["capacity"], payload, True,
             "overlay .data text, translated offset 0x%08X" % sr["offset"])

    crc = crc64_xz_file(tr.path)

    if opts.census:
        for line in lines:
            print(line)
        print_census(census, blob, pack_rows)
        return

    write_pack(pack_rows, blob, crc, tr.path, jp.path, census=census)
    write_manifest(pack_rows, crc, tr, jp)
    for line in lines:
        print(line)
    print_census(census, blob, pack_rows)


def print_census(census, blob, pack_rows):
    print("")
    print("EK text pack census (765 manifest rows)")
    print("  byte-identical between the two disks : %d" % census["identical"])
    print("  differing                            : %d" % census["differ"])
    print("    excluded, soundfont relocated      : %d" % census["soundfont_relocated"])
    print("    excluded, soundfont not located    : %d" % census["soundfont_other"])
    print("    excluded, MIO0 padding only        : %d" % census["padding_only"])
    print("    excluded, not a texture            : %d" % census["non_texture"])
    print("  rows in the pack (incl. strings)     : %d" % len(pack_rows))
    print("  pack size                            : %d bytes" % len(blob))


def crc64_xz_file(path):
    """CRC-64/XZ, the same fingerprint disk_savefile.cpp's crc64() computes."""
    poly = 0xC96C5795D7870F42
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (poly if (c & 1) else 0)
        table.append(c)
    crc = 0xFFFFFFFFFFFFFFFF
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            for byte in chunk:
                crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFFFFFFFFFF


def write_pack(pack_rows, blob, crc, tr_path, jp_path, census):
    externs = []
    seen = set()
    for r in pack_rows:
        if r["dest"] in seen:
            continue
        seen.add(r["dest"])
        if r["ctype"] is not None and r["elems"] is not None:
            externs.append("extern %s %s[%d];" % (r["ctype"], r["dest"], r["elems"]))
        else:
            externs.append("extern char %s[%d];" % (r["dest"], r["capacity"]))

    with open(OUT_C, "w", newline="\n") as f:
        f.write("// AUTO-GENERATED by tools/gen_ek_text_pack.py. Do not edit by hand.\n")
        f.write("// The Expansion Kit ENGLISH TEXT PACK (B11m): translated art and text for the\n")
        f.write("// rows this tool MEASURED to differ between the retail-JP disk and the\n")
        f.write("// fan-translated release, so an English build needs no translated image.\n")
        f.write("// EMPTY unless the generator was run with --translated/--reference, in which\n")
        f.write("// case gdx_ek_text_pack_apply() is a no-op and the build behaves exactly as\n")
        f.write("// it did before the pack existed.\n")
        f.write("//\n")
        f.write("// NOT IN THE REPOSITORY: this file is disk bytes and is .gitignore'd, the\n")
        f.write("// same way port/gen/EkAssetPack.c is. port/gen/ek_text_pack_manifest.txt\n")
        f.write("// carries the row list (names, offsets, sizes -- no bytes) for review.\n")
        f.write('#include "gdx_ek_text_pack.h"\n\n')
        for line in externs:
            f.write(line + "\n")
        f.write("\n")
        f.write("const unsigned int gEkTextPackSize = %uU;\n" % len(blob))
        f.write("const unsigned long long gEkTextPackSourceCrc64 = 0x%016XULL;\n\n" % crc)
        if blob:
            f.write("const unsigned char gEkTextPack[%u] = {\n" % len(blob))
            for i in range(0, len(blob), 16):
                f.write("    " + ",".join("0x%02X" % b for b in blob[i:i + 16]) + ",\n")
            f.write("};\n\n")
        else:
            f.write("const unsigned char gEkTextPack[1] = { 0 };\n\n")
        f.write("const GdxEkTextPackRow gEkTextPackRows[%u] = {\n" % max(1, len(pack_rows)))
        if pack_rows:
            for r in pack_rows:
                f.write('    { "%s", %s, %uU, %uU, %uU, %u },  // %s\n' %
                        (r["name"], r["dest"], r["packOffset"], r["size"],
                         r["capacity"], r["zeroTail"], r["note"]))
        else:
            f.write("    { 0, 0, 0U, 0U, 0U, 0 },\n")
        f.write("};\n")
        f.write("const unsigned int gEkTextPackRowCount = %uU;\n" % len(pack_rows))


def write_manifest(pack_rows, crc, tr, jp):
    with open(OUT_MANIFEST, "w", newline="\n") as f:
        f.write("# AUTO-GENERATED by tools/gen_ek_text_pack.py. Do not edit by hand.\n")
        f.write("# The row list of the EK English text pack -- NAMES, SIZES and CAPACITIES\n")
        f.write("# ONLY. No image bytes are in this file, which is why it is committed while\n")
        f.write("# port/gen/EkTranslatedTextPack.c is not. It is the reviewable half: it says\n")
        f.write("# exactly which symbols a baked build overwrites with translated content.\n")
        f.write("# fields: symbol dest packOffset(dec) size(dec) capacity(dec) zeroTail\n")
        f.write("version 1\n")
        f.write("rows %d\n" % len(pack_rows))
        f.write("bytes %d\n" % sum(r["size"] for r in pack_rows))
        f.write("source_crc64 0x%016X\n" % crc)
        f.write("source_container %s\n" % tr.kind)
        f.write("source_region %s\n" % tr.region)
        f.write("reference_region %s\n" % jp.region)
        for r in pack_rows:
            f.write("%s %s %d %d %d %d\n" % (r["name"], r["dest"], r["packOffset"],
                                             r["size"], r["capacity"], r["zeroTail"]))


if __name__ == "__main__":
    main()
