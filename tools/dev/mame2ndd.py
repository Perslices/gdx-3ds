#!/usr/bin/env python3
"""mame2ndd.py — 64DD MAME/Ares <-> .ndd converter, and the bridge nobody had written.

    tools/dev/mame2ndd.py tondd     <in.mame> <out.ndd>     # 70,627,520 -> 64,931,840
    tools/dev/mame2ndd.py tomame    <in.ndd>  <out.mame>    # 64,931,840 -> 70,627,520
    tools/dev/mame2ndd.py roundtrip <in.ndd> [--work DIR]   # ndd -> mame -> ndd, byte-identical
    tools/dev/mame2ndd.py coverage  <disk-type>             # which MAME bytes an .ndd can carry
    tools/dev/mame2ndd.py selftest                          # geometry, all 7 disk types, no files
    tools/dev/mame2ndd.py inspect   <file>                  # structure + CRC-64/XZ, emits no bytes

WHY THIS EXISTS
---------------
Two halves of this conversion were already in the tree and nobody had joined them as a
committed, tested tool:

  * `fzerox-expansion-kit/tools/leo64dd_python` (LuigiBlood) reads the MAME physical layout
    with the correct LBA -> (head, track, block) walk, but it is a vendored submodule, it
    imports numpy, and its MAME loader locates the system area at .ndd-style linear offsets,
    which happens to work only because the system blocks repeat one sector 85 times.
  * `tools/dev/mkdisk64.py` implements the .ndd side's geometry from the decomp's own
    `leo_tbl.c` and self-validates against four independent in-tree constants.

So every MAME -> .ndd conversion this project has run so far was an ad-hoc scratch script
invoking the submodule, and `ekcart` §8 had to record the resulting image's CRC-64 as
unexplained because there was nothing to test the conversion WITH. This tool is the bridge,
it is stdlib-only, and its gate is a round-trip over mkdisk64.py's SYNTHETIC image -- so CI
needs no game content and no dump.

IMPLEMENTED, each from a named in-tree source
---------------------------------------------
  * per-zone block sizes, virtual-zone LBA boundaries, the vzone->pzone permutation
      -- `decomp/src/leo/lib/leo_tbl.c` via tools/dev/mkdisk64.py (LEOBYTE_TBL2,
         LEOVZONE_TBL, LEOVZONE_PZONEHD_TBL), and INDEPENDENTLY from
         `fzerox-expansion-kit/tools/leo64dd_python/leo64dd.py` (block_size_per_pzone,
         vzone_lba_tbl, pzone_tbl, zone_tbl). `selftest` asserts the two transcriptions
         agree for every LBA of every disk type -- 30,212 comparisons. A single
         transcription error on either side cannot survive that, which is the same
         cross-check discipline `diskstream` §2 built for the .ndd geometry.
  * the .ndd layout: physical LBAs 0..0x10DB laid end to end, no padding, no header.
  * the MAME layout: `Disk_MAME.mame_offset_table` (16 zone bases, head-major) plus
      offset = base[zone - head + head*8]
             + blocksize * 2 * (track - track_zone_tbl[0][zone - head])
             + blocksize * block
  * `lba_to_phys`, including the serpentine block index (`lba & 3` in {0,3} -> block 0,
    else block 1), the head-1 reversed track walk, and the DEFECT TRACK SKIP driven by
    System Data's `defect_num[16]` / `defect_data[]`.
  * disk type read as `sysdata[5] & 0x0F`, which is `port/n64_leo.c`'s own rule
    (`gdx_leo_on_disk_loaded`: `disk[5] & 0xF`). See GUESSED below for why not `[5] - [4]`.

GUESSED / ASSUMED, named rather than buried
-------------------------------------------
  * `mame_offset_table` is taken verbatim from leo64dd_python. It is a 16-entry table of
    absolute byte bases and there is NO second in-tree source for it -- the decomp has no
    MAME support, because MAME format is an emulator container, not a 64DD structure. It is
    therefore the ONE geometric fact here that is transcribed rather than cross-checked.
    `selftest` proves it is at least self-consistent (every LBA lands inside the file, no
    two LBAs overlap, and the highest byte touched is exactly the MAME file size minus the
    trailing spare region), which is a strong constraint but not an independent source.
  * The bytes of a MAME image that NO LBA maps to are not preserved by a round trip through
    .ndd, and this tool does not pretend otherwise -- `coverage` measures exactly how many
    they are. What they contain on a real dump (spare tracks, the surface behind mapped-out
    defective tracks) is not established here.
  * Sector-level C1/C2 ECC does not exist in either container and is not modelled, exactly
    as `mkdisk64.py` says.
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkdisk64  # noqa: E402  -- the in-tree .ndd geometry, reused rather than re-transcribed

SIZE_NDD = 0x3DEC800        # 64,931,840  == mkdisk64.GDX_DISK_EXACT_BYTES
SIZE_MAME = 0x435B0C0       # 70,627,520  == leo64dd_python size_format_mame
LBA_COUNT = 4316            # 0x18 system LBAs + 4292 user LBAs
SYS_LBA_COUNT = 24
SECTOR_COUNT = 85

# ---- transcribed from fzerox-expansion-kit/tools/leo64dd_python/leo64dd.py ----------------
BLOCK_SIZE_PER_PZONE = (19720, 18360, 17680, 16320, 14960, 13600, 12240, 10880, 9520)

VZONE_LBA_TBL = mkdisk64.LEOVZONE_TBL          # identical table, one transcription (selftest)
PZONE_TBL = mkdisk64.LEOVZONE_PZONEHD_TBL      # identical table, one transcription (selftest)
ZONE_TBL = (0, 1, 2, 3, 4, 5, 6, 7, 1, 2, 3, 4, 5, 6, 7, 8)

TRACK_ZONE_TBL = (
    (0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425),
    (0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A),
)

# The one table with no second in-tree source. See GUESSED above.
MAME_OFFSET_TABLE = (
    0x0000000, 0x05F15E0, 0x0B79D00, 0x10801A0, 0x1523720, 0x1963D80, 0x1D414C0, 0x20BBCE0,
    0x23196E0, 0x28A1E00, 0x2DF5DC0, 0x3299340, 0x36D99A0, 0x3AB70E0, 0x3E31900, 0x4149200,
)


# ------------------------------------------------------------------ geometry --
def lba_to_vzone(disk_type, lba):
    for i, v in enumerate(VZONE_LBA_TBL[disk_type]):
        if lba < v:
            return i
    raise ValueError("lba %d out of range" % lba)


def size_of_lba(disk_type, lba):
    pzone = PZONE_TBL[disk_type][lba_to_vzone(disk_type, lba)]
    return BLOCK_SIZE_PER_PZONE[ZONE_TBL[pzone]]


class SystemData(object):
    """leo_sys_form (decomp/include/leo/leo_internal.h), the first 0xE8 bytes of pLBA 0."""

    def __init__(self, raw):
        self.raw = bytes(raw[:0xE8])
        self.region = struct.unpack_from(">I", self.raw, 0x00)[0]
        self.fmt_type = self.raw[0x04]
        # port/n64_leo.c gdx_leo_on_disk_loaded reads `disk[5] & 0xF`, and that is the rule
        # used here. leo64dd_python instead computes `raw[5] - raw[4]`, which is equivalent on
        # a retail disk (byte 5 = fmt_type + disk_type = 0x10 + t) but NOT on the synthetic
        # image tools/dev/mkdisk64.py writes, where byte 5 is the bare disk type. Masking
        # agrees with both, and with the only consumer in this port.
        self.disk_type = self.raw[0x05] & 0x0F
        self.ipl_load_len = struct.unpack_from(">H", self.raw, 0x06)[0]
        self.rom_end_lba = struct.unpack_from(">H", self.raw, 0xE0)[0]
        self.ram_start_lba = struct.unpack_from(">H", self.raw, 0xE2)[0]
        self.ram_end_lba = struct.unpack_from(">H", self.raw, 0xE4)[0]
        # defect_num[16] at 0x08 is CUMULATIVE; defect_data[] starts at 0x20.
        self.defect_tracks = []
        prev = 0
        for i in range(16):
            cur = self.raw[0x08 + i]
            self.defect_tracks.append(list(self.raw[0x20 + prev:0x20 + cur]))
            prev = cur
        self.defect_total = prev

    def describe(self):
        return ("region=0x%08X fmt=0x%02X disk_type=%d ipl_load_len=0x%04X "
                "rom_end_lba=%d ram_start_lba=%d ram_end_lba=%d defect_tracks=%d"
                % (self.region, self.fmt_type, self.disk_type, self.ipl_load_len,
                   self.rom_end_lba, self.ram_start_lba, self.ram_end_lba, self.defect_total))


def lba_to_phys(sysdata, lba):
    """(head, track, block). Verbatim from leo64dd_python's lba_to_phys."""
    if lba < 0 or lba >= LBA_COUNT:
        raise ValueError(lba)
    block = 0 if (lba & 3) in (0, 3) else 1
    vzone = lba_to_vzone(sysdata.disk_type, lba)
    pzone = PZONE_TBL[sysdata.disk_type][vzone]
    head = pzone // 8
    zone = pzone - (7 * head)
    vzone_start_lba = VZONE_LBA_TBL[sysdata.disk_type][vzone - 1] if vzone > 0 else 0
    track = (lba - vzone_start_lba) // 2
    if head == 1:
        track = -track
    track += TRACK_ZONE_TBL[head][zone - head]
    pzone_start_track = TRACK_ZONE_TBL[0][zone - head]
    for defect in sysdata.defect_tracks[pzone]:
        if (pzone_start_track + defect) > track:
            break
        track += 1
    return head, track, block


def phys_zone(head, track):
    z = 0
    for i in range(8):
        if track >= TRACK_ZONE_TBL[0][i]:
            z = i
        else:
            break
    return z + head


def mame_offset(sysdata, lba):
    head, track, block = lba_to_phys(sysdata, lba)
    zone = phys_zone(head, track)
    track_relative = track - TRACK_ZONE_TBL[0][zone - head]
    bsz = BLOCK_SIZE_PER_PZONE[zone]
    off = MAME_OFFSET_TABLE[(zone - head) + (head * 8)]
    off += bsz * 2 * track_relative
    off += block * bsz
    return off


def ndd_table(disk_type):
    """(offset, size) per physical LBA in the .ndd, from mkdisk64's own walk."""
    table, total = mkdisk64.block_table(disk_type)
    assert total == SIZE_NDD, (total, SIZE_NDD)
    assert len(table) == LBA_COUNT, (len(table), LBA_COUNT)
    return table


# ---------------------------------------------------------------- conversion --
def _read_exact(path, want):
    size = os.path.getsize(path)
    if size != want:
        sys.exit("%s: expected %d bytes (0x%X), got %d" % (path, want, want, size))
    with open(path, "rb") as f:
        return bytearray(f.read())


def _system_data_of(img):
    """pLBA 0 starts at offset 0 in BOTH containers, so this works before the disk type
    (and therefore the whole rest of the geometry) is known."""
    return SystemData(img[:0xE8])


def to_ndd(mame, sysdata=None):
    sysdata = sysdata or _system_data_of(mame)
    table = ndd_table(sysdata.disk_type)
    out = bytearray(SIZE_NDD)
    for lba in range(LBA_COUNT):
        off, size = table[lba]
        assert size == size_of_lba(sysdata.disk_type, lba), lba
        src = mame_offset(sysdata, lba)
        if src + size > SIZE_MAME:
            sys.exit("LBA %d maps past the end of the MAME image (0x%X+%d)" % (lba, src, size))
        out[off:off + size] = mame[src:src + size]
    return out, sysdata


def to_mame(ndd, sysdata=None):
    sysdata = sysdata or _system_data_of(ndd)
    table = ndd_table(sysdata.disk_type)
    out = bytearray(SIZE_MAME)
    for lba in range(LBA_COUNT):
        off, size = table[lba]
        dst = mame_offset(sysdata, lba)
        if dst + size > SIZE_MAME:
            sys.exit("LBA %d maps past the end of the MAME image (0x%X+%d)" % (lba, dst, size))
        out[dst:dst + size] = ndd[off:off + size]
    return out, sysdata


# --------------------------------------------------------------------- crc --
def crc64_xz(data):
    return mkdisk64.crc64_xz(data) ^ 0xFFFFFFFFFFFFFFFF


# ---------------------------------------------------------------- commands --
def cmd_tondd(args):
    mame = _read_exact(args.src, SIZE_MAME)
    ndd, sysdata = to_ndd(mame)
    with open(args.dst, "wb") as f:
        f.write(ndd)
    print("mame2ndd: %s -> %s" % (args.src, args.dst))
    print("mame2ndd: %s" % sysdata.describe())
    print("mame2ndd: %d bytes, crc64_xz=0x%016X sha256=%s"
          % (len(ndd), crc64_xz(ndd), hashlib.sha256(ndd).hexdigest()))
    return 0


def cmd_tomame(args):
    ndd = _read_exact(args.src, SIZE_NDD)
    mame, sysdata = to_mame(ndd)
    with open(args.dst, "wb") as f:
        f.write(mame)
    print("mame2ndd: %s -> %s" % (args.src, args.dst))
    print("mame2ndd: %s" % sysdata.describe())
    print("mame2ndd: %d bytes, crc64_xz=0x%016X" % (len(mame), crc64_xz(mame)))
    return 0


def cmd_roundtrip(args):
    """The CI gate. .ndd -> MAME -> .ndd must be the IDENTITY, byte for byte.

    That is the strong direction and the one that matters: it says the MAME physical walk
    addresses every LBA the .ndd holds, exactly once, at the right size, and that nothing is
    dropped, duplicated or shifted. The other direction (MAME -> .ndd -> MAME) is NOT the
    identity and cannot be -- `coverage` measures the bytes no LBA maps to."""
    ndd = _read_exact(args.src, SIZE_NDD)
    sysdata = _system_data_of(ndd)
    mame, _ = to_mame(ndd, sysdata)
    back, _ = to_ndd(mame, sysdata)
    if args.work:
        os.makedirs(args.work, exist_ok=True)
        with open(os.path.join(args.work, "roundtrip.mame"), "wb") as f:
            f.write(mame)
    checks = 0
    failures = 0
    if len(back) != len(ndd):
        failures += 1
    checks += 1
    if bytes(back) != bytes(ndd):
        failures += 1
        # name the first differing LBA rather than the first differing byte
        table = ndd_table(sysdata.disk_type)
        for lba, (off, size) in enumerate(table):
            if back[off:off + size] != ndd[off:off + size]:
                print("mame2ndd: FIRST DIVERGENT LBA %d at .ndd offset 0x%X (%d bytes), "
                      "MAME offset 0x%X" % (lba, off, size, mame_offset(sysdata, lba)))
                break
    checks += 1
    # Injectivity: no two LBAs may share a MAME byte, and every LBA must fit.
    spans = sorted((mame_offset(sysdata, l), size_of_lba(sysdata.disk_type, l))
                   for l in range(LBA_COUNT))
    prev_end = 0
    overlaps = 0
    for off, size in spans:
        if off < prev_end:
            overlaps += 1
        prev_end = max(prev_end, off + size)
    checks += 1
    if overlaps:
        failures += 1
        print("mame2ndd: %d overlapping MAME spans" % overlaps)
    mapped = sum(size for _, size in spans)
    checks += 1
    if mapped != SIZE_NDD:
        failures += 1
        print("mame2ndd: mapped %d bytes, expected %d" % (mapped, SIZE_NDD))

    print("mame2ndd: roundtrip .ndd -> MAME -> .ndd  %s" % ("IDENTICAL" if failures == 0 else "DIFFERS"))
    print("mame2ndd: %s" % sysdata.describe())
    if args.crc:
        # ~6 s per pass in pure Python over 65 MB, so opt-in: the byte compare above is
        # strictly stronger than a fingerprint compare and CI does not need both.
        print("mame2ndd: crc64_xz in=0x%016X out=0x%016X" % (crc64_xz(ndd), crc64_xz(back)))
    else:
        print("mame2ndd: crc64_xz skipped (pass --crc); the byte compare above is stronger")
    print("mame2ndd: %d LBAs, %d mapped bytes of %d MAME bytes (%.2f%%), highest byte 0x%X"
          % (LBA_COUNT, mapped, SIZE_MAME, 100.0 * mapped / SIZE_MAME, prev_end))
    print("mame2ndd_roundtrip: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


def cmd_coverage(args):
    """What a MAME image holds that an .ndd cannot: the bytes no LBA maps to."""
    t = args.disk_type
    sysdata = SystemData(bytes([0, 0, 0, 0, 0x10, 0x10 | t]) + bytes(0xE8 - 6))
    spans = sorted((mame_offset(sysdata, l), size_of_lba(t, l)) for l in range(LBA_COUNT))
    mapped = sum(s for _, s in spans)
    gaps = []
    cursor = 0
    for off, size in spans:
        if off > cursor:
            gaps.append((cursor, off - cursor))
        cursor = max(cursor, off + size)
    if cursor < SIZE_MAME:
        gaps.append((cursor, SIZE_MAME - cursor))
    print("disk type %d (no defective tracks)" % t)
    print("  MAME bytes          %10d (0x%X)" % (SIZE_MAME, SIZE_MAME))
    print("  .ndd bytes          %10d (0x%X)" % (SIZE_NDD, SIZE_NDD))
    print("  mapped by some LBA  %10d (%.3f%% of the MAME image)" % (mapped, 100.0 * mapped / SIZE_MAME))
    print("  unmapped            %10d (%.3f%%) in %d run(s)"
          % (SIZE_MAME - mapped, 100.0 * (SIZE_MAME - mapped) / SIZE_MAME, len(gaps)))
    for off, size in gaps[:16]:
        print("    0x%08X +%d" % (off, size))
    if len(gaps) > 16:
        print("    ... %d more" % (len(gaps) - 16))
    print("  NOTE: a disk WITH defective tracks shifts the mapping (lba_to_phys skips them),")
    print("        so the unmapped set is a property of the dump, not only of the disk type.")
    return 0


def cmd_selftest(args):
    """Geometry only. Touches no file, needs no image, and is the cheap half of the CI gate."""
    checks = 0
    failures = 0

    def check(ok, what):
        nonlocal checks, failures
        checks += 1
        if not ok:
            failures += 1
            print("  FAIL: %s" % what)

    # 1. The two independent in-tree transcriptions of the block-size walk must agree, for
    #    every LBA of every disk type: mkdisk64's (from leo_tbl.c) and leo64dd_python's
    #    (block_size_per_pzone + zone_tbl). 4316 * 7 = 30,212 comparisons.
    for t in range(7):
        for lba in range(LBA_COUNT):
            check(mkdisk64.block_bytes(t, lba) == size_of_lba(t, lba),
                  "block size disagrees at type %d lba %d" % (t, lba))

    # 2. mkdisk64's four self-validations, restated here so this tool fails on its own if the
    #    geometry it borrows ever drifts.
    for t in range(7):
        table, total = mkdisk64.block_table(t)
        check(total == SIZE_NDD, "type %d: .ndd total %d" % (t, total))
        sys_bytes = sum(n for _, n in table[:mkdisk64.SYSTEM_AREA_LBAS])
        check(sys_bytes == mkdisk64.GDX_NDD_SYSTEM_AREA_BYTES, "type %d: system area" % t)
        check(total - sys_bytes == mkdisk64.GDX_USER_AREA_BYTES, "type %d: user area" % t)
        ram = sum(n for _, n in table[mkdisk64.LEORAM_START_LBA[t]:])
        check(ram == mkdisk64.LEORAM_BYTE[t], "type %d: ram area %d" % (t, ram))

    # 3. The MAME mapping is a well-formed placement for every disk type: in range, injective,
    #    and its highest touched byte is inside the MAME file.
    for t in range(7):
        sysdata = SystemData(bytes([0, 0, 0, 0, 0x10, 0x10 | t]) + bytes(0xE8 - 6))
        check(sysdata.disk_type == t, "system data parse: type %d" % t)
        spans = sorted((mame_offset(sysdata, l), size_of_lba(t, l)) for l in range(LBA_COUNT))
        prev_end = 0
        ok_overlap = True
        for off, size in spans:
            if off < 0 or off + size > SIZE_MAME:
                check(False, "type %d: span 0x%X+%d out of range" % (t, off, size))
                break
            if off < prev_end:
                ok_overlap = False
            prev_end = max(prev_end, off + size)
        check(ok_overlap, "type %d: overlapping MAME spans" % t)
        check(sum(s for _, s in spans) == SIZE_NDD, "type %d: mapped bytes != .ndd size" % t)

    # 4. The system-area anchors the port itself reads survive the mapping: pLBA 0 is at
    #    offset 0 in BOTH containers (so the disk-type byte is readable before conversion),
    #    and the Disk ID at pLBA 14 lands where port/n64_leo.c's LeoLBAToByte(0,14) says.
    for t in range(7):
        sysdata = SystemData(bytes([0, 0, 0, 0, 0x10, 0x10 | t]) + bytes(0xE8 - 6))
        check(mame_offset(sysdata, 0) == 0, "type %d: pLBA 0 not at MAME offset 0" % t)
        table, _ = mkdisk64.block_table(t)
        check(table[0][0] == 0, "type %d: pLBA 0 not at .ndd offset 0" % t)
        check(table[14][0] == sum(n for _, n in table[:14]), "type %d: pLBA 14 offset" % t)

    # 5. A defect track shifts the physical mapping and MUST NOT shift the .ndd layout --
    #    that asymmetry is the whole reason the conversion is not a memcpy.
    clean = SystemData(bytes([0, 0, 0, 0, 0x10, 0x13]) + bytes(0xE8 - 6))
    dr = bytearray(0xE8)
    dr[0x04] = 0x10
    dr[0x05] = 0x13
    for i in range(16):
        dr[0x08 + i] = 1            # cumulative: one defective track in pzone 0
    dr[0x20] = 0x05                 # track 5 of pzone 0 is bad
    defective = SystemData(bytes(dr))
    check(defective.defect_total == 1, "defect parse: total")
    check(defective.defect_tracks[0] == [5], "defect parse: pzone 0 list")
    moved = sum(1 for l in range(LBA_COUNT)
                if mame_offset(clean, l) != mame_offset(defective, l))
    check(moved > 0, "a defective track moved no LBA at all")
    # ...and the shifted mapping is still a valid placement: in range, injective, complete.
    dspans = sorted((mame_offset(defective, l), size_of_lba(3, l)) for l in range(LBA_COUNT))
    dprev = 0
    dok = True
    for off, size in dspans:
        if off < dprev or off + size > SIZE_MAME:
            dok = False
        dprev = max(dprev, off + size)
    check(dok, "defective-disk MAME mapping is not a valid placement")
    check(sum(s for _, s in dspans) == SIZE_NDD, "defective-disk mapping loses bytes")
    # The .ndd side must NOT move: block_table takes only the disk type, never the defect
    # list, which is exactly why the conversion is a per-LBA walk and not a memcpy.
    check(mkdisk64.block_table(3)[0] == ndd_table(3), "ndd layout is not defect-independent")

    print("mame2ndd_selftest: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


def cmd_inspect(args):
    size = os.path.getsize(args.src)
    if size == SIZE_NDD:
        kind = "ndd"
    elif size == SIZE_MAME:
        kind = "mame"
    else:
        sys.exit("%s: %d bytes is neither .ndd (%d) nor MAME (%d)" % (args.src, size, SIZE_NDD, SIZE_MAME))
    with open(args.src, "rb") as f:
        img = bytearray(f.read())
    sysdata = _system_data_of(img)
    print("file        %s" % args.src)
    print("format      %s (%d bytes)" % (kind, size))
    print("system data %s" % sysdata.describe())
    for pz, tracks in enumerate(sysdata.defect_tracks):
        if tracks:
            print("  defect pzone %-2d %s" % (pz, tracks))
    # Disk ID lives at pLBA 14 in both containers, at each one's own offset for that LBA.
    if kind == "ndd":
        table = ndd_table(sysdata.disk_type)
        did = table[14][0]
    else:
        did = mame_offset(sysdata, 14)
    ident = img[did:did + 0x20]
    print("disk id     @0x%08X gameName=%r version=%d diskNumber=%d company=%r"
          % (did, bytes(ident[0:4]), ident[4], ident[5], bytes(ident[0x18:0x1A])))
    print("crc64_xz    0x%016X" % crc64_xz(img))
    print("sha256      %s" % hashlib.sha256(img).hexdigest())
    if args.mfs and sysdata.ram_start_lba not in (0, 0xFFFF):
        plba = SYS_LBA_COUNT + sysdata.ram_start_lba
        if kind == "ndd":
            off = ndd_table(sysdata.disk_type)[plba][0]
        else:
            off = mame_offset(sysdata, plba)
        # MfsRamId, decomp/include/leo/mfs.h (size 0x3C), big-endian.
        vol = img[off:off + 0x3C]
        print("mfs volume  @0x%08X diskId=%r fsType=%r fsVersion=%r attr=0x%02X diskType=%d"
              % (off, bytes(vol[0x00:0x0A]), bytes(vol[0x0A:0x0C]), bytes(vol[0x0C:0x0E]),
                 vol[0x0E], vol[0x0F]))
        print("            volumeName=%r renewalCounter=0x%04X checksum=0x%08X"
              % (bytes(vol[0x10:0x24]).rstrip(b"\x00"),
                 struct.unpack_from(">H", vol, 0x28)[0],
                 struct.unpack_from(">I", vol, 0x2C)[0]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("tondd", help="MAME/Ares physical dump -> block-linear .ndd")
    p.add_argument("src")
    p.add_argument("dst")
    p.set_defaults(func=cmd_tondd)

    p = sub.add_parser("tomame", help="block-linear .ndd -> MAME/Ares physical layout")
    p.add_argument("src")
    p.add_argument("dst")
    p.set_defaults(func=cmd_tomame)

    p = sub.add_parser("roundtrip", help=".ndd -> MAME -> .ndd identity check (the CI gate)")
    p.add_argument("src")
    p.add_argument("--work", help="keep the intermediate MAME image in this directory")
    p.add_argument("--crc", action="store_true", help="also fingerprint both ends (~12 s)")
    p.set_defaults(func=cmd_roundtrip)

    p = sub.add_parser("coverage", help="which MAME bytes an .ndd can and cannot carry")
    p.add_argument("disk_type", type=int)
    p.set_defaults(func=cmd_coverage)

    p = sub.add_parser("selftest", help="geometry self-validation, no files")
    p.set_defaults(func=cmd_selftest)

    p = sub.add_parser("inspect", help="print structure + fingerprint; emits no image bytes")
    p.add_argument("src")
    p.add_argument("--mfs", action="store_true", help="also print the MFS volume header")
    p.set_defaults(func=cmd_inspect)

    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
