#!/usr/bin/env python3
"""mkdcrc.py -- precompute a 64DD image's `.dcrc` boot-fingerprint sidecar on the host.

WHY. Mounting a 64DD image in the 3DS build costs one whole-image CRC-64/XZ pass the first
time a given `.ndd` is seen: 64.7 MB read off the SD card in 16 KiB requests. The New 3DS took
about a minute for it (B11m, dcrc-progress.md). The Old 3DS reads the same card through the same
FS service with a third of the CPU for the byte loop, so it costs at least as long, all of it a
black screen before the first frame (o3dsek-plan.md section 3). The result is cached in
`saves/<leaf>.dcrc` and every later boot reads 256 KiB instead. A card is staged from the host
anyway, so this tool writes that sidecar on the host and the console's FIRST boot is already a
cache hit.

The record is byte-for-byte what port/gdx_disk_dcrc.cpp writes (and validates): "GDXC1", u32
image size, u64 sample fingerprint, u64 CRC-64/XZ, little-endian (the 3DS's native order), 25
bytes. The sample is port/disk_buffer.cpp's gdx_disk_sample_fingerprint: 64 windows of 4 KiB at
i*(size-4096)/64, each CRC-64/XZ'd and folded FNV-style. The console still re-validates size and
sample on every boot, so a sidecar for a different image is rejected (`dcrc=sample-mismatch`)
and recomputed; it can cost time, never a wrong identity.

Usage:
    mkdcrc.py <image.ndd> <out-dir> [--name LEAF]
writes <out-dir>/<LEAF>.dcrc (LEAF defaults to the image's file name, which is what the guest
keys on: stage the image as media/baserom.jp.ek.ndd and the sidecar as
saves/baserom.jp.ek.ndd.dcrc).
    mkdcrc.py --check <image.ndd> <sidecar.dcrc>
re-derives and compares.

Stdlib only (PROTOCOL.md: host tools are stdlib-only).
"""

import argparse
import os
import struct
import sys

MAGIC = b"GDXC1"
RECORD_BYTES = 25
EXACT_BYTES = 64931840          # port/disk_buffer.cpp GDX_DISK_EXACT_BYTES
SAMPLE_WINDOWS = 64             # GDX_DCRC_SAMPLE_WINDOWS
SAMPLE_WINDOW_BYTES = 4096      # GDX_DCRC_SAMPLE_WINDOW_BYTES
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def _table():
    t = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (0xC96C5795D7870F42 ^ (c >> 1)) if (c & 1) else (c >> 1)
        t.append(c)
    return t


TABLE = _table()


def crc64_update(crc, data):
    t = TABLE
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


def crc64_xz(data):
    return crc64_update(MASK64, data) ^ MASK64


def sample_fingerprint(f, size):
    acc = FNV_OFFSET
    for i in range(SAMPLE_WINDOWS):
        off = i * (size - SAMPLE_WINDOW_BYTES) // SAMPLE_WINDOWS
        f.seek(off)
        window = f.read(SAMPLE_WINDOW_BYTES)
        if len(window) != SAMPLE_WINDOW_BYTES:
            raise ValueError("short read at %d" % off)
        acc ^= crc64_xz(window)
        acc = (acc * FNV_PRIME) & MASK64
    return acc if acc else 1


def whole_crc(f):
    f.seek(0)
    crc = MASK64
    while True:
        chunk = f.read(1 << 20)
        if not chunk:
            break
        crc = crc64_update(crc, chunk)
    return crc ^ MASK64


def fingerprint(path):
    size = os.path.getsize(path)
    if size != EXACT_BYTES:
        raise ValueError("%s is %d bytes; the 3DS mounts only %d-byte block-linear .ndd images"
                         % (path, size, EXACT_BYTES))
    with open(path, "rb") as f:
        sample = sample_fingerprint(f, size)
        crc = whole_crc(f)
    return size, sample, crc


def record(size, sample, crc):
    return MAGIC + struct.pack("<IQQ", size, sample, crc)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="compare an existing sidecar instead")
    ap.add_argument("--name", default=None, help="sidecar leaf (default: the image's file name)")
    ap.add_argument("image")
    ap.add_argument("target", help="output directory, or the sidecar file with --check")
    a = ap.parse_args(argv)

    size, sample, crc = fingerprint(a.image)
    rec = record(size, sample, crc)
    assert len(rec) == RECORD_BYTES
    print("mkdcrc: size=%d sample=0x%016X crc64=0x%016X" % (size, sample, crc))
    if a.check:
        with open(a.target, "rb") as f:
            have = f.read()
        if have != rec:
            print("mkdcrc: MISMATCH against %s" % a.target)
            return 1
        print("mkdcrc: %s matches" % a.target)
        return 0
    leaf = a.name or os.path.basename(a.image)
    os.makedirs(a.target, exist_ok=True)
    out = os.path.join(a.target, leaf + ".dcrc")
    with open(out, "wb") as f:
        f.write(rec)
    print("mkdcrc: wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
