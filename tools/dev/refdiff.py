#!/usr/bin/env python3
"""refdiff.py — compare two 320x240 autotest frames and say how different they really are.

Both renderers dump the SAME buffer through the SAME writer: gFrameMirrorFb, 320x240, read back
by gdx_request_frame_dump and written as a 24-bit BMP whose samples were quantised to RGBA16
(port/n64_gfx_bridge.cpp DumpRgba16Bmp). So one channel step in the FILE is 8/255 — a
sub-quantum difference cannot exist, and any threshold below 8 is measuring nothing.

Two renderers will never be byte-identical, so a single number is not a verdict. This prints:

  exact      % of pixels identical in all three channels
  within-1   % within one RGB5 quantum (<=8 per channel) — the floor two correct
             implementations of the same scene can reach
  MAE/RMSE   mean absolute / root-mean-square error over all samples, 0..255
  dE         mean and 95th-percentile perceptual distance, CIE76 in CIELAB (sRGB assumed):
             ~2.3 is the just-noticeable difference, so p95 under that reads as "the same image"
  layout     % of pixels whose LUMA differs by more than `--layout-threshold` (default 40).
             Geometry errors (a shifted viewport, a missing element, the wrong aspect) move
             large blocks of luma; palette and dither differences do not. This is the number
             that separates "a different renderer" from "a different picture".

Usage:
  refdiff.py A.bmp B.bmp [--out-dir DIR] [--label-a NAME] [--label-b NAME] [--json]
"""

import argparse
import json
import math
import os
import sys

from PIL import Image, ImageDraw

QUANTUM = 8  # one RGB5 step, expanded to 8-bit by DumpRgba16Bmp's (v<<3)|(v>>2)


def pixels(im):
    """Flat RGB tuple list. Pillow 12 deprecates Image.getdata() in favour of
    get_flattened_data(); accept either so this runs on both."""
    getter = getattr(im, "get_flattened_data", None) or im.getdata
    return list(getter())


def load(path):
    return Image.open(path).convert("RGB")


def content_box(im, threshold=6):
    """Bounding box of everything that is not the letter/pillarbox background.

    The 3DS blits its 400x240 top screen into the same 320x240 mirror the desktop uses, and a
    sub-window viewport there is FOLDED into a smaller rect (docs/research/viewport-progress.md),
    so the console's title art occupies ~243x220 of the mirror while the desktop's fills it. A
    raw pixel diff of those two is measuring the fold, not the renderer.
    """
    mask = im.convert("L").point(lambda v: 255 if v > threshold else 0)
    return mask.getbbox() or (0, 0, im.size[0], im.size[1])


def align(a_img, b_img, mode):
    """Return (a, b) at a common size. See --align.

    none  : compare as dumped; only honest when both renderers filled the frame the same way.
    frame : resample B onto A's frame. Corrects a pure resolution difference, nothing else.
    box   : crop each to its own content box, then resample B onto A's. This is the one that
            answers "is it the same PICTURE", with the viewport fold and the pillarbox
            divided out. It cannot see a geometry bug that is itself a uniform scale.
    """
    if mode == "none":
        return a_img, b_img
    if mode == "box":
        a_img = a_img.crop(content_box(a_img))
        b_img = b_img.crop(content_box(b_img))
    if b_img.size != a_img.size:
        b_img = b_img.resize(a_img.size, Image.LANCZOS)
    return a_img, b_img


def _srgb_to_lab(r, g, b):
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = lin(r), lin(g), lin(b)
    x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
    y = (0.2126 * r + 0.7152 * g + 0.0722 * b)
    z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883

    def f(t):
        return t ** (1.0 / 3.0) if t > 0.008856 else (7.787 * t + 16.0 / 116.0)
    fx, fy, fz = f(x), f(y), f(z)
    return (116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))


_LAB_CACHE = {}


def lab(px):
    v = _LAB_CACHE.get(px)
    if v is None:
        v = _srgb_to_lab(*px)
        _LAB_CACHE[px] = v
    return v


def compare(a_img, b_img, layout_threshold):
    a = pixels(a_img)
    b = pixels(b_img)
    n = min(len(a), len(b))
    exact = within = layout = 0
    abs_sum = sq_sum = 0
    de_list = []
    de_map = []
    for i in range(n):
        pa, pb = a[i], b[i]
        d = (abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
        m = max(d)
        if m == 0:
            exact += 1
        if m <= QUANTUM:
            within += 1
        abs_sum += d[0] + d[1] + d[2]
        sq_sum += d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
        la, lb = lab(pa), lab(pb)
        de = math.sqrt((la[0] - lb[0]) ** 2 + (la[1] - lb[1]) ** 2 + (la[2] - lb[2]) ** 2)
        de_list.append(de)
        de_map.append(de)
        # Rec.601 luma, the same weighting the N64's own YUV work uses.
        ya = 0.299 * pa[0] + 0.587 * pa[1] + 0.114 * pa[2]
        yb = 0.299 * pb[0] + 0.587 * pb[1] + 0.114 * pb[2]
        if abs(ya - yb) > layout_threshold:
            layout += 1
    de_sorted = sorted(de_list)
    return {
        "pixels": n,
        "exact_pct": 100.0 * exact / n,
        "within_quantum_pct": 100.0 * within / n,
        "mae": abs_sum / (3.0 * n),
        "rmse": math.sqrt(sq_sum / (3.0 * n)),
        "delta_e_mean": sum(de_list) / n,
        "delta_e_p95": de_sorted[int(0.95 * (n - 1))],
        "delta_e_max": de_sorted[-1],
        "layout_pct": 100.0 * layout / n,
        "layout_threshold": layout_threshold,
    }, de_map


def contact_sheet(a_img, b_img, de_map, stats, label_a, label_b, out_path):
    """A | B | 4x amplified absolute difference | dE heatmap, captioned with the numbers."""
    w, h = a_img.size
    pad, cap = 8, 34
    sheet = Image.new("RGB", (w * 4 + pad * 5, h + cap * 2 + pad * 2), (18, 18, 20))

    diff = Image.new("RGB", (w, h))
    heat = Image.new("RGB", (w, h))
    a = pixels(a_img)
    b = pixels(b_img)
    dpx, hpx = [], []
    for i in range(w * h):
        pa, pb = a[i], b[i]
        dpx.append(tuple(min(255, abs(pa[c] - pb[c]) * 4) for c in range(3)))
        # 0 -> black, JND(2.3) -> blue, 10 -> yellow, >=25 -> red.
        e = de_map[i]
        if e <= 2.3:
            hpx.append((0, 0, int(60 * e / 2.3)))
        elif e <= 10:
            t = (e - 2.3) / 7.7
            hpx.append((int(200 * t), int(200 * t), int(60 + 60 * (1 - t))))
        else:
            t = min(1.0, (e - 10) / 15.0)
            hpx.append((200 + int(55 * t), int(200 * (1 - t)), 0))
    diff.putdata(dpx)
    heat.putdata(hpx)

    panels = [(a_img, label_a), (b_img, label_b), (diff, "|A-B| x4"), (heat, "dE76 heatmap")]
    d = ImageDraw.Draw(sheet)
    for i, (im, name) in enumerate(panels):
        x = pad + i * (w + pad)
        sheet.paste(im, (x, pad + cap))
        d.text((x, pad + cap // 3), name, fill=(230, 230, 235))

    caption = ("exact %.2f%%   within 1 RGB5 quantum %.2f%%   MAE %.2f   RMSE %.2f   "
               "dE mean %.2f p95 %.2f   layout(|dY|>%d) %.2f%%" %
               (stats["exact_pct"], stats["within_quantum_pct"], stats["mae"], stats["rmse"],
                stats["delta_e_mean"], stats["delta_e_p95"], stats["layout_threshold"],
                stats["layout_pct"]))
    d.text((pad, pad + cap + h + 8), caption, fill=(200, 205, 215))
    sheet.save(out_path)
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--label-a", default=None)
    ap.add_argument("--label-b", default=None)
    ap.add_argument("--out-dir", default=None, help="write <name>.contact.png here")
    ap.add_argument("--layout-threshold", type=int, default=40)
    ap.add_argument("--align", choices=("none", "frame", "box"), default="none",
                    help="none: as dumped. frame: resample B to A. box: crop both to their "
                         "content box first — use this across renderers, where the 3DS "
                         "viewport fold otherwise dominates the number.")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    label_a = args.label_a or os.path.basename(args.a)
    label_b = args.label_b or os.path.basename(args.b)
    a_img, b_img = align(load(args.a), load(args.b), args.align)
    stats, de_map = compare(a_img, b_img, args.layout_threshold)
    stats["align"] = args.align
    stats["size"] = list(a_img.size)

    sheet = None
    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        name = "%s__vs__%s.%s.contact.png" % (os.path.splitext(label_a)[0],
                                              os.path.splitext(label_b)[0], args.align)
        sheet = contact_sheet(a_img, b_img, de_map, stats, label_a, label_b,
                              os.path.join(args.out_dir, name))
        stats["contact_sheet"] = sheet

    if args.json:
        print(json.dumps(stats, indent=2, sort_keys=True))
    else:
        print("A %s" % args.a)
        print("B %s" % args.b)
        print("  exact              %7.3f %%" % stats["exact_pct"])
        print("  within 1 quantum   %7.3f %%   (<=%d/255 per channel)" %
              (stats["within_quantum_pct"], QUANTUM))
        print("  MAE / RMSE         %7.3f / %.3f  (0..255)" % (stats["mae"], stats["rmse"]))
        print("  dE76 mean/p95/max  %7.3f / %.3f / %.3f  (JND ~2.3)" %
              (stats["delta_e_mean"], stats["delta_e_p95"], stats["delta_e_max"]))
        print("  layout |dY|>%-3d    %7.3f %%" % (stats["layout_threshold"], stats["layout_pct"]))
        print("  align / size       %s @ %dx%d" % (args.align, a_img.size[0], a_img.size[1]))
        if sheet:
            print("  contact sheet      %s" % sheet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
