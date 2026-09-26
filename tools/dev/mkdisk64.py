#!/usr/bin/env python3
"""mkdisk64.py — synthetic 64DD (.ndd) disk image generator. NO GAME CONTENT.

    tools/dev/mkdisk64.py build out.ndd [--seed N] [--disk-type T] [--manifest out.json]
    tools/dev/mkdisk64.py geometry [--disk-type T]           # print the zone/LBA table
    tools/dev/mkdisk64.py corrupt in.ndd out.ndd --at OFF --len N [--seed N]
    tools/dev/mkdisk64.py truncate in.ndd out.ndd --bytes N

WHY THIS EXISTS
---------------
`port/disk_buffer.cpp` mounts a 64,931,840-byte 64DD image and `port/n64_leo.c` serves every
LEO read out of it. None of that can be tested, regression-gated or CI-run without an image,
and a 64DD dump is game content this repository must never carry. This tool builds an image
that is STRUCTURALLY a 64DD disk -- correct total size, correct per-zone block sizes, a
parseable system area, a mountable MFS volume -- and whose payload is deterministic PRNG
filler. It is enough to exercise the block reader, the LBA arithmetic, the CRC path and every
refusal path; it is NOT enough to boot the Expansion Kit, and it is not meant to be.

EVERY structural fact below is read out of THIS repository (the decomp's own libleo tables and
MFS headers, and the port's own constants), never from a dump. `manifest.json` records which
facts are implemented from an in-tree source and which are guesses -- see FACTS at the bottom.

SELF-VALIDATION (all three hold for every disk type 0..6, and the build asserts them):
  * total bytes                 == 64,931,840 == port/disk_buffer.cpp GDX_DISK_EXACT_BYTES
  * system area (pLBA 0..0x17)  == 0x738C0    == port/n64_leo.c GDX_NDD_SYSTEM_AREA_BYTES
  * user area                   == 0x3D78F40  == port/n64_leo.c LeoReadCapacity(OS_READ).nbytes
  * bytes from LEORAM_START_LBA == LEORAM_BYTE[type] (decomp/src/leo/lib/leo_tbl.c)
That last one is the strong check: the RAM-area capacity table is an independent constant in
the decomp, and summing our own per-block zone walk reproduces all seven entries exactly.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

# ---------------------------------------------------------------- geometry --
# decomp/src/leo/lib/leo_tbl.c, verbatim. These are drive firmware tables, not game data.
LEOBYTE_TBL2 = [0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70, 0x3520, 0x2FD0, 0x2A80, 0x2530]

LEOVZONE_TBL = [
    [0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8,
     0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x06A2, 0x07C6, 0x08D8,
     0x09EA, 0x0AFC, 0x0BC8, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08C6,
     0x09EA, 0x0AFC, 0x0C0E, 0x0CDA, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4,
     0x09C6, 0x0AEA, 0x0C0E, 0x0D20, 0x0DEC, 0x0EB8, 0x0FCA, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4,
     0x09C6, 0x0AD8, 0x0BEA, 0x0D0E, 0x0E32, 0x0EFE, 0x0FCA, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E,
     0x0980, 0x0A92, 0x0BA4, 0x0CB6, 0x0DC8, 0x0EEC, 0x1010, 0x10DC],
    [0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E,
     0x093A, 0x0A4C, 0x0B5E, 0x0C70, 0x0D82, 0x0E94, 0x0FB8, 0x10DC],
]

LEOVZONE_PZONEHD_TBL = [
    [0x00, 0x01, 0x02, 0x09, 0x08, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A],
    [0x00, 0x01, 0x02, 0x03, 0x0A, 0x09, 0x08, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B],
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x0B, 0x0A, 0x09, 0x08, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C],
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x06, 0x07, 0x0F, 0x0E, 0x0D],
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x0F, 0x0E],
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x0F],
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08],
]

LEORAM_START_LBA = [0x05A2, 0x07C6, 0x09EA, 0x0C0E, 0x0E32, 0x1010, 0x10DC]
LEORAM_BYTE = [0x024A9DC0, 0x01C226C0, 0x01450F00, 0x00D35680, 0x006CFD40, 0x001DA240, 0x00000000]

NUM_LBAS = 4292          # decomp/include/PR/leo.h
LEO_LBA_MAX = 4291       # decomp/include/PR/leoappli.h
LEO_LBA_ROM_END = [1417, 1965, 2513, 3061, 3609, 4087, LEO_LBA_MAX]
SYSTEM_AREA_LBAS = 0x18  # physical LBAs 0..0x17 precede the user area

# port/disk_buffer.cpp GDX_DISK_EXACT_BYTES / port/n64_leo.c GDX_NDD_SYSTEM_AREA_BYTES
GDX_DISK_EXACT_BYTES = 64931840
GDX_NDD_SYSTEM_AREA_BYTES = 0x738C0
GDX_USER_AREA_BYTES = 0x3D78F40   # LeoReadCapacity(OS_READ).nbytes

# decomp/include/leo/leo_internal.h
LEO_COUNTRY_JPN = 0xE848D316
LEO_COUNTRY_USA = 0x2263EE56

MFS_RAM_AREA_SIZE = 0xE730        # decomp/include/leo/mfs.h, sizeof(MfsRamArea)
MFS_FAT_COUNT = 0xB3A
MFS_DIRENT_OFFSET = 0x16B0
MFS_FAT_UNUSED = 0
MFS_FAT_OUT_OF_MANAGEMENT = 0xFFFD
MFS_FILE_ATTR_DIRECTORY = 1 << 15


def vzone_of(disk_type, plba):
    for i, v in enumerate(LEOVZONE_TBL[disk_type]):
        if plba < v:
            return i
    return 0xFF


def block_bytes(disk_type, plba):
    """Bytes in physical LBA `plba`. Mirrors LeoLBAToByte / leoTranslate exactly."""
    zone = LEOVZONE_PZONEHD_TBL[disk_type][vzone_of(disk_type, plba)]
    if zone >= 8:
        zone -= 7
    return LEOBYTE_TBL2[zone]


def block_table(disk_type):
    """(offset, size) of every physical LBA 0..0x10DB, file-linear."""
    out = []
    off = 0
    total_plbas = SYSTEM_AREA_LBAS + NUM_LBAS
    for plba in range(total_plbas):
        n = block_bytes(disk_type, plba)
        out.append((off, n))
        off += n
    return out, off


def logical_offset(disk_type, table, lba):
    """File offset of LOGICAL (user-area) LBA `lba`, the value port/n64_leo.c computes as
    LeoLBAToByte(0, lba) + GDX_NDD_SYSTEM_AREA_BYTES."""
    return table[SYSTEM_AREA_LBAS + lba][0]


# ------------------------------------------------------------------ filler --
def splitmix64_stream(seed, nbytes):
    """Deterministic, seekable, no-dependency filler. Not cryptographic and not meant to be:
    the only requirement is that it is reproducible from the recorded seed and contains no
    content from anywhere."""
    out = bytearray()
    x = seed & 0xFFFFFFFFFFFFFFFF
    while len(out) < nbytes:
        x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = x
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        z ^= z >> 31
        out += struct.pack(">Q", z)
    return bytes(out[:nbytes])


FILLER_KEYSTREAM_BYTES = 65536
FILLER_ROTOR = 4093       # coprime with 65536, so consecutive blocks do not align


def block_filler(keystream, plba, size):
    """Per-block filler, reproducible in O(1) for ANY block without generating the ones before
    it. That property is the whole point: the host test verifies a streamed read by recomputing
    the expected bytes for the block it asked for, so it never has to hold 62 MB.

        rot        = (plba * 4093) % 65536
        body[i]    = keystream[(rot + i) % 65536]
        block[0:16]= "GDXSYN%010d" % plba

    Ten lines of C (port/tests/gdx_disk_stream_tests.cpp carries the transcription). The
    16-byte plain-text stamp means a misaddressed read shows up in a hex dump as the WRONG
    BLOCK NUMBER rather than merely as wrong bytes."""
    stamp = b"GDXSYN%010d" % plba
    assert len(stamp) == 16
    n = size - 16
    assert n <= FILLER_KEYSTREAM_BYTES  # max 64DD block is 0x4D08 = 19720
    rot = (plba * FILLER_ROTOR) % FILLER_KEYSTREAM_BYTES
    return stamp + (keystream + keystream)[rot:rot + n]


# -------------------------------------------------------------- structures --
def system_data_block(disk_type, size, country):
    """leo_sys_form, decomp/include/leo/leo_internal.h (size 0xE8), big-endian, padded to a
    full zone-0 block. Field layout is IMPLEMENTED from that struct; the values are synthetic."""
    b = bytearray(size)
    struct.pack_into(">I", b, 0x00, country)          # country
    b[0x04] = 0x10                                    # fmt_type   (GUESSED)
    b[0x05] = disk_type & 0x0F                        # disk_type  <- port reads disk[5] & 0xF
    struct.pack_into(">H", b, 0x06, 0x00C0)           # ipl_load_len (GUESSED)
    # defect_num[20]: cumulative defect count per physical zone. A pristine synthetic disk has
    # no defective cylinders, so every entry is 0 and defect_data[192] stays zero. This is the
    # one field leoLba_to_phys consumes, and zero is the well-defined "no defects" case.
    struct.pack_into(">I", b, 0x1C, 0x80000000)       # loadptr    (GUESSED)
    struct.pack_into(">H", b, 0xE0, LEO_LBA_ROM_END[disk_type])
    struct.pack_into(">H", b, 0xE2, LEORAM_START_LBA[disk_type] - SYSTEM_AREA_LBAS)
    struct.pack_into(">H", b, 0xE4, LEO_LBA_MAX)
    return bytes(b)


def disk_id_block(size, serial):
    """LEODiskID, decomp/include/PR/leo.h (24 bytes), big-endian, padded to a full block.
    gameName is deliberately NOT a real title code: 'GDXS' = G-Diffuser synthetic."""
    b = bytearray(size)
    b[0x00:0x04] = b"GDXS"      # gameName[4]
    b[0x04] = 0x00              # gameVersion
    b[0x05] = 0x00              # diskNumber
    b[0x06] = 0x00              # ramUsage
    b[0x07] = 0x00              # diskUsage
    struct.pack_into(">Q", b, 0x08, serial)           # LEOSerialNum.lineNumber
    struct.pack_into(">Q", b, 0x10, 0)                # LEOSerialNum.rawTime
    b[0x18:0x1A] = b"GX"        # company[2]
    # freeArea[6] stays zero
    return bytes(b)


def mfs_ram_area(disk_type, ram_area_bytes):
    """An EMPTY, FORMATTED MFS version-B volume: MfsRamId header, a FAT with the volume's own
    six LBAs marked out-of-management, and a root directory entry. Modelled on
    decomp/src/leo/mfs/mfs_ram.c (Mfs_FormatRamArea / Mfs_CreateRootDirectory /
    Mfs_CalculateVolumeChecksum) -- see FACTS: the header and the checksum are implemented from
    that source; the exact free-list initial state is modelled, not validated against a dump."""
    b = bytearray(MFS_RAM_AREA_SIZE)
    b[0x00:0x0A] = b"64dd-Multi"                       # MfsRamId.diskId
    b[0x0A:0x0C] = b"01"                               # fsType
    b[0x0C:0x0E] = b"01"                               # fsVersion
    b[0x0E] = 0x00                                     # attr: no write/read protect
    b[0x0F] = disk_type & 0xFF                         # diskType
    name = b"GDX SYNTHETIC"
    b[0x10:0x10 + len(name)] = name                    # volumeName[20]
    struct.pack_into(">I", b, 0x24, 0)                 # formatDate
    struct.pack_into(">H", b, 0x28, 1)                 # renewalCounter
    b[0x2A] = 0x00                                     # destinationCode
    struct.pack_into(">I", b, 0x2C, 0)                 # checksum, filled below

    # FAT: entries 0..5 are the volume itself (main copy at startLBA+0..2, backup at +3..5 --
    # Mfs_BackupRamArea writes both), which is why Mfs_RamGetFreeSize starts its scan at 6.
    for i in range(MFS_FAT_COUNT):
        v = MFS_FAT_OUT_OF_MANAGEMENT if i < 6 else MFS_FAT_UNUSED
        struct.pack_into(">H", b, 0x3C + i * 2, v)
    # Entries past the end of this disk type's RAM area are out of management too.
    ram_lbas = LEO_LBA_MAX - (LEORAM_START_LBA[disk_type] - SYSTEM_AREA_LBAS) + 1
    for i in range(max(6, ram_lbas), MFS_FAT_COUNT):
        struct.pack_into(">H", b, 0x3C + i * 2, MFS_FAT_OUT_OF_MANAGEMENT)

    # Root directory entry (MfsRamDirectoryEntry, 0x30 bytes) at directoryEntry[0].
    d = MFS_DIRENT_OFFSET
    struct.pack_into(">H", b, d + 0x00, MFS_FILE_ATTR_DIRECTORY)
    struct.pack_into(">H", b, d + 0x02, 0xFFFE)        # parentDirId = MFS_ENTRY_ROOT_PARENT_DIR
    b[d + 0x04:d + 0x06] = b"GX"                       # companyCode
    b[d + 0x06:d + 0x0A] = b"SYNT"                     # gameCode
    struct.pack_into(">H", b, d + 0x0A, 0)             # dirId = MFS_ENTRY_ROOT_DIR

    # Mfs_CalculateVolumeChecksum: XOR of s32 words over LeoLBAToByte(startLBA, 3) bytes with
    # the checksum field zeroed. The volume is three LBAs, so it is LONGER than sizeof(MfsRamArea)
    # -- the trailing bytes are part of the checksummed span and are zero here.
    span = ram_area_bytes
    buf = bytearray(span)
    buf[:MFS_RAM_AREA_SIZE] = b
    checksum = 0
    for i in range(0, (span // 4) * 4, 4):
        checksum ^= struct.unpack_from(">i", buf, i)[0]
    checksum &= 0xFFFFFFFF
    struct.pack_into(">I", b, 0x2C, checksum)
    return bytes(b)


# ------------------------------------------------------------------- crc64 --
def crc64_xz(data, crc=0xFFFFFFFFFFFFFFFF, table=[]):
    """CRC-64/XZ, the same fingerprint port/disk_savefile.cpp computes."""
    if not table:
        for n in range(256):
            c = n
            for _ in range(8):
                c = (0xC96C5795D7870F42 ^ (c >> 1)) if (c & 1) else (c >> 1)
            table.append(c)
    for byte in data:
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc


# ------------------------------------------------------------------- build --
def build(args):
    t = args.disk_type
    if not 0 <= t <= 6:
        sys.exit("disk type must be 0..6")
    table, total = block_table(t)

    sys_bytes = sum(n for _, n in table[:SYSTEM_AREA_LBAS])
    user_bytes = total - sys_bytes
    ram_start_plba = LEORAM_START_LBA[t]
    ram_bytes = sum(n for _, n in table[ram_start_plba:])

    # The four self-validations. A generator that cannot reproduce the decomp's own constants
    # has no business claiming to model the geometry.
    assert total == GDX_DISK_EXACT_BYTES, (total, GDX_DISK_EXACT_BYTES)
    assert sys_bytes == GDX_NDD_SYSTEM_AREA_BYTES, (sys_bytes,)
    assert user_bytes == GDX_USER_AREA_BYTES, (user_bytes,)
    assert ram_bytes == LEORAM_BYTE[t], (ram_bytes, LEORAM_BYTE[t])

    country = LEO_COUNTRY_USA if args.country == "usa" else LEO_COUNTRY_JPN
    structured = {}   # plba -> bytes (padded to the block size)

    # System area, physical LBAs 0..0x17. The two anchors the port actually reads are
    # IMPLEMENTED: disk[5] (the System Data's disk_type) and the LEODiskID at pLBA 14, which
    # port/n64_leo.c finds via LeoLBAToByte(0, 14) == 14 * 0x4D08 because every system-area
    # block is a zone-0 block. The 8/8/8 repetition below is GUESSED.
    for plba in range(0, 8):
        structured[plba] = system_data_block(t, table[plba][1], country)
    for plba in range(8, 16):
        structured[plba] = disk_id_block(table[plba][1], args.seed)
    for plba in range(16, SYSTEM_AREA_LBAS):
        structured[plba] = system_data_block(t, table[plba][1], country)

    # MFS volume: three LBAs at the RAM-area start, plus the backup copy at +3. Disk type 6 is
    # all-ROM (LEORAM_BYTE[6] == 0, RAM start == one past the last block), so it has no volume.
    ram_area_span = sum(n for _, n in table[ram_start_plba:ram_start_plba + 3])
    mfs = mfs_ram_area(t, ram_area_span) if ram_bytes > 0 else b""
    for copy in ((0, 3) if ram_bytes > 0 else ()):   # main at startLBA, backup at +3
        pos = 0
        for k in range(3):
            plba = ram_start_plba + copy + k
            blk = table[plba][1]
            chunk = bytearray(blk)
            take = mfs[pos:pos + blk]
            chunk[:len(take)] = take
            pos += blk
            structured[plba] = bytes(chunk)

    # Write it out, block by block: never more than one block resident.
    keystream = splitmix64_stream(args.seed, FILLER_KEYSTREAM_BYTES)
    keystream2 = keystream + keystream
    h_sha = hashlib.sha256()
    crc = 0xFFFFFFFFFFFFFFFF
    written = 0
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        for plba, (off, size) in enumerate(table):
            blk = structured.get(plba)
            if blk is None:
                blk = block_filler(keystream, plba, size)
            assert len(blk) == size, (plba, len(blk), size)
            f.write(blk)
            h_sha.update(blk)
            crc = crc64_xz(blk, crc)
            written += size
    crc ^= 0xFFFFFFFFFFFFFFFF
    assert written == total

    manifest = {
        "tool": "tools/dev/mkdisk64.py",
        "format": 1,
        "warning": "SYNTHETIC. Structure only. Contains no game content of any kind.",
        "seed": args.seed,
        "filler": {"keystream_bytes": FILLER_KEYSTREAM_BYTES, "rotor": FILLER_ROTOR,
                   "keystream_sha256": hashlib.sha256(keystream).hexdigest()},
        "disk_type": t,
        "country": args.country,
        "size_bytes": total,
        "sha256": h_sha.hexdigest(),
        "crc64_xz": "0x%016X" % crc,
        "geometry": {
            "system_area_lbas": SYSTEM_AREA_LBAS,
            "system_area_bytes": sys_bytes,
            "user_area_bytes": user_bytes,
            "num_lbas": NUM_LBAS,
            "lba_max": LEO_LBA_MAX,
            "rom_end_lba": LEO_LBA_ROM_END[t],
            "ram_start_plba": ram_start_plba,
            "ram_area_bytes": ram_bytes,
            "block_sizes_by_zone": LEOBYTE_TBL2,
            "first_user_offset": table[SYSTEM_AREA_LBAS][0],
            "block_size_histogram": _histogram(table),
        },
        "anchors": {
            "disk_type_byte": {"offset": 5, "value": t},
            "disk_id_plba": 14,
            "disk_id_offset": table[14][0],
            "mfs_volume_plba": ram_start_plba,
            "mfs_volume_offset": table[ram_start_plba][0],
            "mfs_backup_plba": ram_start_plba + 3,
        },
        "facts": FACTS,
    }
    mpath = args.manifest or (args.out + ".manifest.json")
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=False)
        f.write("\n")
    print("mkdisk64: wrote %s (%d bytes, type %d, seed %d)" % (args.out, total, t, args.seed))
    print("mkdisk64: sha256 %s" % manifest["sha256"])
    print("mkdisk64: crc64  %s" % manifest["crc64_xz"])
    print("mkdisk64: manifest %s" % mpath)
    return 0


def _histogram(table):
    h = {}
    for _, n in table:
        h[n] = h.get(n, 0) + 1
    return {("0x%04X" % k): v for k, v in sorted(h.items())}


def geometry(args):
    t = args.disk_type
    table, total = block_table(t)
    print("disk type %d: %d physical LBAs, %d bytes total (0x%X)" % (t, len(table), total, total))
    print("  system area : pLBA 0..0x%X  %d bytes (0x%X)"
          % (SYSTEM_AREA_LBAS - 1, GDX_NDD_SYSTEM_AREA_BYTES, GDX_NDD_SYSTEM_AREA_BYTES))
    print("  user area   : %d bytes (0x%X)" % (total - GDX_NDD_SYSTEM_AREA_BYTES,
                                               total - GDX_NDD_SYSTEM_AREA_BYTES))
    print("  rom end LBA : %d   ram start pLBA: 0x%X   ram bytes: 0x%X"
          % (LEO_LBA_ROM_END[t], LEORAM_START_LBA[t], LEORAM_BYTE[t]))
    print("  block sizes : %s" % _histogram(table))
    for lba in (0, 1, 14, 100, 1000, 1001, 1002, NUM_LBAS - 1):
        if lba < NUM_LBAS:
            print("  logical LBA %5d -> file offset 0x%08X (%d bytes)"
                  % (lba, table[SYSTEM_AREA_LBAS + lba][0], table[SYSTEM_AREA_LBAS + lba][1]))
    return 0


def corrupt(args):
    """Flip a byte range in place. Used by the host test's 'refuse a corrupt image' case."""
    with open(args.inp, "rb") as f:
        data = bytearray(f.read())
    if args.at + args.len > len(data):
        sys.exit("range past end of image")
    noise = splitmix64_stream(args.seed, args.len)
    for i in range(args.len):
        data[args.at + i] ^= noise[i] or 0xFF
    with open(args.out, "wb") as f:
        f.write(data)
    print("mkdisk64: corrupted %d bytes at 0x%X -> %s" % (args.len, args.at, args.out))
    return 0


def truncate(args):
    with open(args.inp, "rb") as f:
        data = f.read(args.bytes)
    with open(args.out, "wb") as f:
        f.write(data)
    print("mkdisk64: truncated to %d bytes -> %s" % (len(data), args.out))
    return 0


# ------------------------------------------------------------------- facts --
# Read this beside the image. Every entry says where the fact came from, and a "guessed" entry
# is a placeholder that a real dump will correct -- none of them is on a path the block reader
# or the LBA arithmetic depends on.
FACTS = {
    "implemented_from_tree": [
        "per-zone block sizes LEOBYTE_TBL2 (decomp/src/leo/lib/leo_tbl.c)",
        "virtual-zone LBA boundaries LEOVZONE_TBL and the vzone->pzone/head map "
        "LEOVZONE_PZONEHD_TBL (same file)",
        "LBA->byte walk, including the +0x18 system-area bias (decomp/src/leo/lib/lbatobyte.c, "
        "leotranslat.c, leoutil.c leoLba_to_vzone)",
        "total image size 64,931,840 and the 0x738C0 system area "
        "(port/disk_buffer.cpp GDX_DISK_EXACT_BYTES, port/n64_leo.c GDX_NDD_SYSTEM_AREA_BYTES) "
        "-- both REPRODUCED by summing our own block table, not copied",
        "user-area capacity 0x3D78F40 (port/n64_leo.c LeoReadCapacity)",
        "RAM-area start LBA and capacity per disk type, LEORAM_START_LBA / LEORAM_BYTE "
        "(leo_tbl.c) -- all seven REPRODUCED by summing our own block table",
        "System Data field layout: leo_sys_form (decomp/include/leo/leo_internal.h), including "
        "disk_type at byte 5, which port/n64_leo.c gdx_leo_on_disk_loaded reads as disk[5] & 0xF",
        "Disk ID field layout: LEODiskID (decomp/include/PR/leo.h) and its location at physical "
        "LBA 14, which port/n64_leo.c LeoReadDiskID reads via LeoLBAToByte(0, 14)",
        "country codes LEO_COUNTRY_JPN / LEO_COUNTRY_USA (leo_internal.h)",
        "MfsRamId / MfsRamDirectoryEntry / MfsRamArea layout and sizes "
        "(decomp/include/leo/mfs.h)",
        "MFS volume checksum: XOR of big-endian s32 words over LeoLBAToByte(startLBA, 3) bytes "
        "with the checksum field zeroed (decomp/src/leo/mfs/mfs_ram.c "
        "Mfs_CalculateVolumeChecksum)",
        "MFS volume occupies 6 LBAs -- main copy at startLBA, backup at startLBA+3 "
        "(mfs_ram.c Mfs_BackupRamArea), which is why the free-space scan starts at FAT index 6",
    ],
    "guessed": [
        "the 8/8/8 repetition of System Data / Disk ID / System Data across physical LBAs "
        "0..0x17. Only two positions are constrained by the port (byte 5 of pLBA 0, and the "
        "Disk ID at pLBA 14) and both are satisfied; how a retail disk fills the other 22 "
        "blocks is not established here",
        "leo_sys_form fmt_type (0x10), ipl_load_len (0x00C0) and loadptr (0x80000000). Nothing "
        "in the port reads them",
        "defect_num[20] / defect_data[192] all zero, i.e. a disk with no defective cylinders. "
        "leoLba_to_phys consumes these; zero is the well-defined no-defect case but a retail "
        "disk will not be all-zero here",
        "the MFS free-list initial state (which FAT entries a freshly formatted retail volume "
        "marks OUT_OF_MANAGEMENT beyond the volume's own 6 LBAs). Modelled from "
        "Mfs_FormatRamArea, not validated against a dump",
        "the MFS format date and renewal counter",
        "everything outside the system area and the MFS volume is deterministic PRNG filler. "
        "A real disk has an IPL, the game's disk-side overlays, the EK asset payloads that "
        "port/gen/EkAssetBindings.c reads at 0x00B120F0..0x011DA910, and MFS file data",
    ],
    "deliberately_absent": [
        "any 64DD IPL / drive ROM content",
        "any game code, asset, texture, sequence or sample bank",
        "C1/C2 sector ECC. The retail drive computes it in its sequencer "
        "(decomp/src/leo/lib/leoc2ecc.c); a .ndd does not carry it and the port does not model "
        "it",
    ],
}


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="write a synthetic .ndd plus its manifest")
    b.add_argument("out")
    b.add_argument("--seed", type=int, default=20260924)
    b.add_argument("--disk-type", type=int, default=0)
    b.add_argument("--country", choices=("usa", "jpn"), default="usa")
    b.add_argument("--manifest")
    b.set_defaults(func=build)

    g = sub.add_parser("geometry", help="print the derived geometry and exit")
    g.add_argument("--disk-type", type=int, default=0)
    g.set_defaults(func=geometry)

    c = sub.add_parser("corrupt", help="flip a byte range (for refusal tests)")
    c.add_argument("inp")
    c.add_argument("out")
    c.add_argument("--at", type=lambda s: int(s, 0), required=True)
    c.add_argument("--len", type=lambda s: int(s, 0), default=64)
    c.add_argument("--seed", type=int, default=1)
    c.set_defaults(func=corrupt)

    t = sub.add_parser("truncate", help="write a short prefix (for refusal tests)")
    t.add_argument("inp")
    t.add_argument("out")
    t.add_argument("--bytes", type=lambda s: int(s, 0), required=True)
    t.set_defaults(func=truncate)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
