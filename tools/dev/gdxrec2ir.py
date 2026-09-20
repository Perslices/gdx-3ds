#!/usr/bin/env python3
"""gdxrec2ir.py — convert a .gdxrec into Rosalina InputRedirection packets (UDP 4950).

    tools/dev/gdxrec2ir.py race.gdxrec out.bin [--fps 60]
    tools/dev/gdxrec2ir.py race.gdxrec --send <3ds-ip> [--fps 60]

NOT A PARITY ORACLE. UDP input redirection is wall-clock paced by the sender and sampled by
the console's HID at its own cadence, so a converted stream is NOT tick-exact: the same
.gdxrec replayed on-device through gdx_replay_start() / [debug] input_replay is the
deterministic path (docs/research/native/oracle-progress.md). This converter exists for the
hwlink track's interactive lane (drive the console from the Mac) only.

Packet layout (20 bytes, little-endian u32 x5, one per frame):
  [0] HID pad: ACTIVE-LOW mask (bit set = released) of the 3DS KEY_* bits
  [1] touch:   x | y<<12 | 1<<24 when touching, else 0x02000000
  [2] circle:  x | y<<12 (12-bit, 0x800 centre)          — neutral 0x00800800
  [3] cstick/ZL/ZR: cstick x | y<<12 | ZL<<... (neutral 0x80800081)
  [4] special: home/power bits, 0
Button mapping (N64 mask -> 3DS KEY_*): A->A, B->B, Z->L (the port's default), START->START,
L->L, R->R, D-pad->D-pad, C-buttons->cstick nudges are NOT sent (the C-stick word stays neutral).
Stdlib only.
"""
import argparse
import socket
import struct
import sys
import time

N64 = {"A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000, "UP": 0x0800, "DOWN": 0x0400,
       "LEFT": 0x0200, "RIGHT": 0x0100, "L": 0x0020, "R": 0x0010,
       "CUP": 0x0008, "CDOWN": 0x0004, "CLEFT": 0x0002, "CRIGHT": 0x0001}
KEY = {"A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3, "RIGHT": 1 << 4, "LEFT": 1 << 5,
       "UP": 1 << 6, "DOWN": 1 << 7, "R": 1 << 8, "L": 1 << 9, "X": 1 << 10, "Y": 1 << 11}
MAP = [("A", "A"), ("B", "B"), ("Z", "L"), ("START", "START"), ("UP", "UP"), ("DOWN", "DOWN"),
       ("LEFT", "LEFT"), ("RIGHT", "RIGHT"), ("L", "L"), ("R", "R")]


def parse(path):
    frames = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            tok = s.split()
            if not tok[0].isdigit() or len(tok) < 4:
                continue  # header
            frames.append((int(tok[0]), int(tok[1], 16), int(tok[2]), int(tok[3])))
    return frames


def packet(btn, sx, sy):
    keys = 0
    for n64, k in MAP:
        if btn & N64[n64]:
            keys |= KEY[k]
    hid = (~keys) & 0xFFF
    cx = max(0, min(0xFFF, 0x800 + int(sx * 0x800 // 80)))
    cy = max(0, min(0xFFF, 0x800 + int(sy * 0x800 // 80)))
    return struct.pack("<5I", hid, 0x02000000, cx | (cy << 12), 0x80800081, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gdxrec")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--send", metavar="IP")
    ap.add_argument("--fps", type=float, default=60.0)
    args = ap.parse_args()
    frames = parse(args.gdxrec)
    if not frames:
        print("no data lines in %s" % args.gdxrec)
        return 2
    pkts = [packet(b, x, y) for _, b, x, y in frames]
    if args.send:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        period = 1.0 / args.fps
        t0 = time.monotonic()
        for i, p in enumerate(pkts):
            sock.sendto(p, (args.send, 4950))
            due = t0 + (i + 1) * period
            now = time.monotonic()
            if due > now:
                time.sleep(due - now)
        sock.sendto(packet(0, 0, 0), (args.send, 4950))
        print("sent %d packets to %s:4950 (wall-clock paced; not tick-exact)" % (len(pkts), args.send))
        return 0
    if not args.out:
        print("need an output file or --send")
        return 2
    with open(args.out, "wb") as f:
        for p in pkts:
            f.write(p)
    print("wrote %d x 20-byte packets -> %s" % (len(pkts), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
