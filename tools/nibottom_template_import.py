#!/usr/bin/env python3
"""nibottom_template_import.py -- turn an edited Photoshop template back into
a Phase-2-ready pixel bundle.

COLD TOOL. Desktop-only, not part of the 3DS build, not game code. Pairs
with tools/gen_nibottom_template.py (which exports the DASH layers in the
first place). Reads whatever the user edited under a template directory
(layout.json + layers/*.png, optionally top-*.png) and writes:

  - a validated, RGB565/RGBA5551-quantised PNG per widget (so the user can
    see exactly what the real framebuffer's colour depth will do to their
    edit before anything is wired into the build);
  - a flattened dash-full (quantised) composite, same way
    gen_nibottom_template.py built the unquantised one;
  - manifest.json: per widget, its x/y/w/h (from layout.json, not wherever
    the pixels happen to sit on the PNG), pixel format, and the raw
    big-endian 16-bit pixel words as base64 -- the same byte layout
    tools/o2r_texture_extract.py's _rgba5551_to_rgba8() decodes (N64/this
    project's existing RGBA5551 convention), so a future Phase 2 painter
    reads these the same way it already reads every other real texture
    this track cites.

This writes only into --out (default: <template-dir>/import-output); it
never touches decomp/, port/, or gdiffuser.ini. Wiring any of this into the
actual build is Phase 2 and still needs explicit approval
(nibottom-plan.md's STOP line).

Usage:
    python3 tools/nibottom_template_import.py [--template-dir DIR] [--out DIR]
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path

from PIL import Image

DEFAULT_TEMPLATE_DIR = Path.home() / "Desktop" / "g-diffuser-builds" / "fzerox-raw-assets" / "template"

# Widgets that keep their own alpha (textured overlays, blitted over
# whatever's already drawn) use RGBA5551, the N64/this project's existing
# convention (tools/o2r_texture_extract.py's _rgba5551_to_rgba8). The
# background is the one layer that becomes the framebuffer's own base fill
# and has no alpha channel in the real GSP_RGB565_OES console target
# (bottomhud-progress.md §1) -- it quantises to RGB565 instead.
RGB565_WIDGETS = {"background"}


def quantize_channel(v, bits):
    """Round an 8-bit channel down to `bits` bits, then re-expand to 8-bit
    the same way the decoder does (c<<shift | c>>(bits-shift)-ish), so the
    *preview* PNG shows the same banding the real framebuffer will."""
    levels = 1 << bits
    q = round(v * (levels - 1) / 255)
    shift = 8 - bits
    return (q << shift) | (q >> (bits - shift)) if bits < 8 else v, q


def pack_rgb565(r, g, b):
    _, r5 = quantize_channel(r, 5)
    _, g6 = quantize_channel(g, 6)
    _, b5 = quantize_channel(b, 5)
    return (r5 << 11) | (g6 << 5) | b5


def pack_rgba5551(r, g, b, a):
    _, r5 = quantize_channel(r, 5)
    _, g5 = quantize_channel(g, 5)
    _, b5 = quantize_channel(b, 5)
    a1 = 1 if a >= 128 else 0
    return (r5 << 11) | (g5 << 6) | (b5 << 1) | a1


def quantize_preview_rgb565(im):
    im = im.convert("RGB")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            r8, _ = quantize_channel(r, 5)
            g8, _ = quantize_channel(g, 6)
            b8, _ = quantize_channel(b, 5)
            px[x, y] = (r8, g8, b8)
    return im


def quantize_preview_rgba5551(im):
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            r8, _ = quantize_channel(r, 5)
            g8, _ = quantize_channel(g, 5)
            b8, _ = quantize_channel(b, 5)
            px[x, y] = (r8, g8, b8, 255 if a >= 128 else 0)
    return im


def raw_bytes_rgb565(im):
    im = im.convert("RGB")
    w, h = im.size
    out = bytearray(w * h * 2)
    px = im.load()
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = pack_rgb565(r, g, b)
            out[i] = (v >> 8) & 0xFF
            out[i + 1] = v & 0xFF
            i += 2
    return bytes(out)


def raw_bytes_rgba5551(im):
    im = im.convert("RGBA")
    w, h = im.size
    out = bytearray(w * h * 2)
    px = im.load()
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = pack_rgba5551(r, g, b, a)
            out[i] = (v >> 8) & 0xFF
            out[i + 1] = v & 0xFF
            i += 2
    return bytes(out)


def import_template(template_dir, out_dir, layers_dir_override=None):
    layout_path = template_dir / "layout.json"
    if not layout_path.is_file():
        raise SystemExit("no layout.json in %s -- run gen_nibottom_template.py first" % template_dir)
    layout = json.loads(layout_path.read_text())
    layers_dir = Path(layers_dir_override) if layers_dir_override else (template_dir / "layers")
    if not layers_dir.is_dir():
        raise SystemExit("no layers/ dir found at %s" % layers_dir)

    bottom_w = layout["canvas"]["bottom"]["w"]
    bottom_h = layout["canvas"]["bottom"]["h"]

    out_dir.mkdir(parents=True, exist_ok=True)
    quant_dir = out_dir / "quantized"
    quant_dir.mkdir(exist_ok=True)

    manifest = {"canvas": layout["canvas"], "widgets": []}
    errors = []
    composite = Image.new("RGBA", (bottom_w, bottom_h), (0, 0, 0, 255))
    z_order = ["background", "map_box", "header", "mode_button", "portrait",
               "position", "lap", "timer", "gap", "boost", "energy_bar"]

    for w in layout["widgets"]:
        wid = w["id"]
        png_path = layers_dir / f"{wid}.png"
        if not png_path.is_file():
            errors.append("missing layers/%s.png" % wid)
            continue
        im = Image.open(png_path)
        if im.size != (bottom_w, bottom_h):
            errors.append("layers/%s.png is %dx%d, expected %dx%d -- layout.json's x/y/w/h "
                           "assume the full canvas size; resize or re-export, don't crop"
                           % (wid, im.width, im.height, bottom_w, bottom_h))
            continue

        is_rgb565 = wid in RGB565_WIDGETS
        if is_rgb565:
            preview = quantize_preview_rgb565(im)
            raw = raw_bytes_rgb565(im)
            fmt = "rgb565"
        else:
            preview = quantize_preview_rgba5551(im)
            raw = raw_bytes_rgba5551(im)
            fmt = "rgba5551"
        preview.save(quant_dir / f"{wid}.png")
        if wid in z_order:
            composite.alpha_composite(preview.convert("RGBA"))

        manifest["widgets"].append({
            "id": wid,
            "x": w["x"], "y": w["y"], "w": w["w"], "h": w["h"],
            "format": fmt,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "raw_base64": base64.b64encode(raw).decode("ascii"),
            "source": w.get("source"),
            "font": w.get("font"),
        })

    for mode_name in ("all_top", "split", "all_bottom"):
        top_path = template_dir / f"top-{mode_name}.png"
        if not top_path.is_file():
            continue
        im = Image.open(top_path)
        expect = (layout["canvas"]["top"]["w"], layout["canvas"]["top"]["h"])
        if im.size != expect:
            errors.append("top-%s.png is %dx%d, expected %dx%d" % (mode_name, im.width, im.height, *expect))
            continue
        raw = raw_bytes_rgb565(im)
        preview = quantize_preview_rgb565(im)
        preview.save(quant_dir / f"top-{mode_name}.png")
        manifest.setdefault("top_previews", []).append({
            "mode": mode_name, "format": "rgb565",
            "sha256": hashlib.sha256(raw).hexdigest(),
            "raw_base64": base64.b64encode(raw).decode("ascii"),
        })

    composite.convert("RGB").save(out_dir / "dash-full-quantized.png")
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    if errors:
        print("WARNINGS (not fatal, but these widgets were skipped):")
        for e in errors:
            print("  - %s" % e)

    return manifest, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--template-dir", default=str(DEFAULT_TEMPLATE_DIR))
    ap.add_argument("--layers-dir", default=None, help="override layers/ location (default: <template-dir>/layers)")
    ap.add_argument("--out", default=None, help="default: <template-dir>/import-output")
    args = ap.parse_args()

    template_dir = Path(args.template_dir)
    out_dir = Path(args.out) if args.out else (template_dir / "import-output")
    manifest, errors = import_template(template_dir, out_dir, args.layers_dir)
    print("wrote %d widgets + %d top previews to %s" % (
        len(manifest["widgets"]), len(manifest.get("top_previews", [])), out_dir))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
