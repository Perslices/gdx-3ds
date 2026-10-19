#!/usr/bin/env python3
"""nrr_tol.py -- the NRR visual tolerance classes (n60 design 7.3) over two SHOT BMP sets.

    tools/native/nrr_tol.py A_DIR B_DIR [--class 3d|2d|split] [--hud-mask FILE]
                            [--sheets DIR] [--md] [--quiet]

A_DIR / B_DIR hold the oracle's SHOT BMPs (`<tag>_L.bmp`, 400x240; a run's `shots/` dir) of the
same replay, lever off (A) vs on (B), same binary, `--gfx vulkan` both sides. Frames are matched
by file name; a name present on one side only is reported and fails the set.

Per pixel the delta is the MAX channel |A - B| (0..255). A "hot" pixel is one whose delta exceeds
the class threshold; blobs are 4-connected components of hot pixels.

  2d   (HUD):  hot = delta > 8.  Per frame: hot <= 0.05% of the frame (48 of 96,000), every blob
               <= 8 px, and -- with --hud-mask -- no hot pixel outside the union of the mask's
               rects (the HUD rect bounds).
  3d   (track, machines, sky, dress): hot = delta > 24.  Per frame: hot <= 0.5% (480 px), every
               blob <= 64 px unless it lies within 8 px of the screen edge (PICA vs CPU clipping),
               and mean delta over ALL pixels <= 0.5.
  split        needs --hud-mask: pixels inside the mask's rects take the 2d rules, the rest the 3d
               rules (one frame, two regions; the counts are per region).

--hud-mask uses framediff's mask format (`mask 400 240` / `rect x y w h name`).
--sheets DIR writes framediff's contact sheet (A | B | diff, hot pixels red, mask outlined) for
every frame that is not byte-identical (pass or fail), named <frame>.png.
Exit 0 = every frame passes, 1 = a frame fails (or a frame is missing), 2 = usage error.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dev"))
import framediff  # noqa: E402  (read_bmp, read_mask, contact_sheet)

RULES = {
    "2d": {"thresh": 8, "max_frac": 0.0005, "max_blob": 8, "edge_ok": False, "max_mean": None},
    "3d": {"thresh": 24, "max_frac": 0.005, "max_blob": 64, "edge_ok": True, "max_mean": 0.5},
}
EDGE_PX = 8


def deltas(w, h, ra, rb):
    """Per-pixel max-channel |A - B|, row-major list of lists."""
    out = []
    for y in range(h):
        a, b = ra[y], rb[y]
        row = [0] * w
        for x in range(w):
            i = x * 3
            d0 = abs(a[i] - b[i])
            d1 = abs(a[i + 1] - b[i + 1])
            d2 = abs(a[i + 2] - b[i + 2])
            row[x] = max(d0, d1, d2)
        out.append(row)
    return out


def blobs(hot, w, h):
    """4-connected components of hot[y][x]: list of (size, x0, y0, x1, y1)."""
    seen = [[False] * w for _ in range(h)]
    out = []
    for y in range(h):
        for x in range(w):
            if not hot[y][x] or seen[y][x]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            n, x0, y0, x1, y1 = 0, x, y, x, y
            while stack:
                cx, cy = stack.pop()
                n += 1
                x0, y0, x1, y1 = min(x0, cx), min(y0, cy), max(x1, cx), max(y1, cy)
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and hot[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            out.append((n, x0, y0, x1, y1))
    return out


def near_edge(b, w, h):
    _, x0, y0, x1, y1 = b
    return x0 < EDGE_PX or y0 < EDGE_PX or x1 >= w - EDGE_PX or y1 >= h - EDGE_PX


def in_rects(x, y, rects):
    return any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh, _ in rects)


def judge_region(cls, d, region, w, h, rects, outside_check):
    """Apply class `cls` to the pixels where region(x, y) is true. -> (ok, info dict)."""
    r = RULES[cls]
    hot = [[region(x, y) and d[y][x] > r["thresh"] for x in range(w)] for y in range(h)]
    npx = sum(1 for y in range(h) for x in range(w) if region(x, y))
    nhot = sum(sum(1 for v in row if v) for row in hot)
    total = sum(d[y][x] for y in range(h) for x in range(w) if region(x, y))
    mean = total / npx if npx else 0.0
    bl = blobs(hot, w, h)
    big = [b for b in bl if b[0] > r["max_blob"] and not (r["edge_ok"] and near_edge(b, w, h))]
    fails = []
    limit = int(r["max_frac"] * w * h)
    if nhot > limit:
        fails.append("hot %d > %d" % (nhot, limit))
    if big:
        fails.append("blob %d px > %d at %d,%d" % (big[0][0], r["max_blob"], big[0][1], big[0][2]))
    if r["max_mean"] is not None and mean > r["max_mean"]:
        fails.append("mean %.3f > %.1f" % (mean, r["max_mean"]))
    if outside_check and rects:
        outside = sum(1 for y in range(h) for x in range(w) if hot[y][x] and not in_rects(x, y, rects))
        if outside:
            fails.append("%d hot px outside the HUD rects" % outside)
    info = {"hot": nhot, "blobs": len(bl), "maxblob": max((b[0] for b in bl), default=0), "mean": mean}
    return not fails, info, fails, hot


def judge(cls, d, w, h, rects):
    if cls == "split":
        ok2, i2, f2, h2 = judge_region("2d", d, lambda x, y: in_rects(x, y, rects), w, h, rects, False)
        ok3, i3, f3, h3 = judge_region("3d", d, lambda x, y: not in_rects(x, y, rects), w, h, rects, False)
        hot = [[h2[y][x] or h3[y][x] for x in range(w)] for y in range(h)]
        info = "2d[hot=%d maxblob=%d] 3d[hot=%d maxblob=%d mean=%.3f]" % (
            i2["hot"], i2["maxblob"], i3["hot"], i3["maxblob"], i3["mean"])
        return ok2 and ok3, info, ["2d: " + f for f in f2] + ["3d: " + f for f in f3], hot
    ok, i, fails, hot = judge_region(cls, d, lambda x, y: True, w, h, rects, cls == "2d")
    info = "hot=%d blobs=%d maxblob=%d mean=%.3f" % (i["hot"], i["blobs"], i["maxblob"], i["mean"])
    return ok, info, fails, hot


def frames_in(path):
    return {f for f in os.listdir(path) if f.lower().endswith(".bmp")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--class", dest="cls", choices=["2d", "3d", "split"], default="3d")
    ap.add_argument("--hud-mask", default=None)
    ap.add_argument("--sheets", default=None)
    ap.add_argument("--md", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if args.cls == "split" and not args.hud_mask:
        print("nrr_tol: --class split needs --hud-mask", file=sys.stderr)
        return 2
    rects = framediff.read_mask(args.hud_mask)[1] if args.hud_mask else []
    fa, fb = frames_in(args.a), frames_in(args.b)
    names = sorted(fa | fb)
    if not names:
        print("nrr_tol: no BMPs", file=sys.stderr)
        return 2
    if args.sheets:
        os.makedirs(args.sheets, exist_ok=True)
    n_ident = n_pass = n_fail = n_missing = 0
    if args.md:
        print("| frame | identical | verdict | detail |")
        print("|---|---|---|---|")
    for name in names:
        if name not in fa or name not in fb:
            n_missing += 1
            print("%s MISSING in %s" % (name, "A" if name not in fa else "B"))
            continue
        w, h, ra = framediff.read_bmp(os.path.join(args.a, name))
        w2, h2, rb = framediff.read_bmp(os.path.join(args.b, name))
        if (w, h) != (w2, h2):
            n_fail += 1
            print("%s FAIL size %dx%d vs %dx%d" % (name, w, h, w2, h2))
            continue
        if ra == rb:
            n_ident += 1
            if args.md:
                print("| %s | yes | pass | |" % name)
            elif not args.quiet:
                print("%s identical" % name)
            continue
        d = deltas(w, h, ra, rb)
        ok, info, fails, hot = judge(args.cls, d, w, h, rects)
        if ok:
            n_pass += 1
        else:
            n_fail += 1
        verdict = "pass" if ok else "FAIL"
        if args.md:
            print("| %s | no | %s | %s %s |" % (name, verdict, info, "; ".join(fails)))
        else:
            print("%s %s %s%s" % (name, verdict, info, (" | " + "; ".join(fails)) if fails else ""))
        if args.sheets:
            changed = [[d[y][x] > 0 for x in range(w)] for y in range(h)]
            sheet = os.path.join(args.sheets, os.path.splitext(name)[0] + ".png")
            framediff.contact_sheet(sheet, w, h, ra, rb, changed if ok else hot, rects)
    print("nrr_tol: class=%s frames=%d identical=%d pass=%d fail=%d missing=%d -> %s" % (
        args.cls, len(names), n_ident, n_pass, n_fail, n_missing,
        "PASS" if n_fail == 0 and n_missing == 0 else "FAIL"))
    return 0 if n_fail == 0 and n_missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
