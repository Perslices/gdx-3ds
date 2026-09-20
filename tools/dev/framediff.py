#!/usr/bin/env python3
"""framediff.py — exact / masked / perceptual compare of two oracle BMPs, contact sheet on failure.

    tools/dev/framediff.py a.bmp b.bmp [--mask race.mask] [--perceptual T] [--sheet out.png] [--quiet]

Inputs are the 24-bit bottom-up BMPs the 3DS oracle writes (autotest/<tag>_L.bmp, 400x240) or
any uncompressed 24/32-bit BMP of equal size. Prints the differing-byte count, differing-pixel
count, the bounding box of the diff, and a per-mask-region breakdown; exit 0 = identical
(outside masked regions), 1 = different, 2 = usage error.

Mask format (`#` comments; coordinates in the landscape frame, top-left origin):

    mask 400 240
    rect 296 8 104 24 timer
    rect 0 200 96 40 speed

`rect x y w h name` excludes the region from the exact compare. Masked regions are STILL
counted and reported (so a masked failure is never silently green) and drawn as outlines on
the contact sheet.

`--perceptual T` passes when the max per-channel delta over unmasked pixels is <= T (0 = exact).

`--sheet out.png` writes a 3-up contact sheet (A | B | amplified diff, masks outlined) ONLY when
the compare fails (or with --sheet-always). Stdlib-only PNG writer (zlib), no Pillow.
"""
import argparse
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("%s: not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<I", data, 14)[0]
    if hdr < 40:
        raise ValueError("%s: unsupported BMP header %d" % (path, hdr))
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if comp != 0 or bpp not in (24, 32):
        raise ValueError("%s: need uncompressed 24/32-bit BMP (bpp=%d comp=%d)" % (path, bpp, comp))
    bottom_up = h > 0
    h = abs(h)
    bpr = (w * (bpp // 8) + 3) & ~3
    step = bpp // 8
    rows = []
    for y in range(h):
        src = off + (h - 1 - y if bottom_up else y) * bpr
        row = bytearray(w * 3)
        for x in range(w):
            b, g, r = data[src + x * step:src + x * step + 3]
            row[x * 3] = r
            row[x * 3 + 1] = g
            row[x * 3 + 2] = b
        rows.append(bytes(row))
    return w, h, rows


def read_mask(path):
    rects = []
    size = None
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            if tok[0] == "mask" and len(tok) == 3:
                size = (int(tok[1]), int(tok[2]))
            elif tok[0] == "rect" and len(tok) >= 5:
                x, y, w, h = (int(v) for v in tok[1:5])
                name = tok[5] if len(tok) > 5 else "rect%d" % len(rects)
                rects.append((x, y, w, h, name))
            else:
                raise ValueError("%s:%d: bad mask line: %s" % (path, n, line))
    return size, rects


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def contact_sheet(path, w, h, ra, rb, diff_mask, rects, gap=4):
    sw = w * 3 + gap * 2
    sheet = []
    for y in range(h):
        drow = bytearray(w * 3)
        for x in range(w):
            if diff_mask[y][x]:
                inside = any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh, _ in rects)
                drow[x * 3:x * 3 + 3] = b"\xff\xb0\x00" if inside else b"\xff\x00\x00"
            else:
                v = (ra[y][x * 3] + ra[y][x * 3 + 1] + ra[y][x * 3 + 2]) // 12
                drow[x * 3:x * 3 + 3] = bytes((v, v, v))
        for rx, ry, rw, rh, _ in rects:
            on_edge = (ry == y or ry + rh - 1 == y)
            for x in range(max(0, rx), min(w, rx + rw)):
                if on_edge or x == rx or x == rx + rw - 1:
                    if ry <= y < ry + rh:
                        drow[x * 3:x * 3 + 3] = b"\x00\xc0\xff"
        sheet.append(bytes(ra[y]) + b"\x20" * (gap * 3) + bytes(rb[y]) + b"\x20" * (gap * 3) + bytes(drow))
    write_png(path, sw, h, sheet)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--mask")
    ap.add_argument("--perceptual", type=int, default=0, help="max per-channel delta tolerated (default 0)")
    ap.add_argument("--sheet", help="contact sheet PNG path (written on failure)")
    ap.add_argument("--sheet-always", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    try:
        wa, ha, ra = read_bmp(args.a)
        wb, hb, rb = read_bmp(args.b)
    except (OSError, ValueError) as e:
        print("framediff: %s" % e)
        return 2
    if (wa, ha) != (wb, hb):
        print("framediff: size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    rects = []
    if args.mask:
        size, rects = read_mask(args.mask)
        if size and size != (wa, ha):
            print("framediff: mask is for %dx%d, images are %dx%d" % (size[0], size[1], wa, ha))
            return 2

    def in_mask(x, y):
        for rx, ry, rw, rh, _ in rects:
            if rx <= x < rx + rw and ry <= y < ry + rh:
                return True
        return False

    diff_bytes = 0
    diff_px = 0
    masked_px = 0
    unmasked_fail = 0
    max_delta = 0
    bbox = None
    per_rect = {r[4]: 0 for r in rects}
    diff_mask = [[False] * wa for _ in range(ha)]
    for y in range(ha):
        A = ra[y]
        B = rb[y]
        if A == B:
            continue
        for x in range(wa):
            a3 = A[x * 3:x * 3 + 3]
            b3 = B[x * 3:x * 3 + 3]
            if a3 == b3:
                continue
            diff_px += 1
            diff_mask[y][x] = True
            d = max(abs(a3[i] - b3[i]) for i in range(3))
            diff_bytes += sum(1 for i in range(3) if a3[i] != b3[i])
            hit = None
            for rx, ry, rw, rh, name in rects:
                if rx <= x < rx + rw and ry <= y < ry + rh:
                    hit = name
                    break
            if hit is not None:
                masked_px += 1
                per_rect[hit] += 1
                continue
            if d > max_delta:
                max_delta = d
            if d > args.perceptual:
                unmasked_fail += 1
                bbox = (x, y, x, y) if bbox is None else (min(bbox[0], x), min(bbox[1], y), max(bbox[2], x), max(bbox[3], y))

    ok = unmasked_fail == 0
    verdict = "IDENTICAL" if diff_px == 0 else ("PASS(masked/perceptual)" if ok else "DIFF")
    print("framediff: %s %s vs %s size=%dx%d diff_bytes=%d diff_pixels=%d masked_diff_pixels=%d unmasked_fail=%d max_unmasked_delta=%d bbox=%s"
          % (verdict, args.a, args.b, wa, ha, diff_bytes, diff_px, masked_px, unmasked_fail, max_delta,
             ("%d,%d-%d,%d" % bbox) if bbox else "none"))
    for name, n in per_rect.items():
        if not args.quiet:
            print("  mask %-12s differing_pixels=%d" % (name, n))
    if args.sheet and (not ok or args.sheet_always):
        contact_sheet(args.sheet, wa, ha, ra, rb, diff_mask, rects)
        print("  contact sheet -> %s (A | B | diff: red=unmasked, orange=masked, cyan=mask outline)" % args.sheet)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
