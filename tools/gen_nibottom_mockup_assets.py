#!/usr/bin/env python3
"""gen_nibottom_mockup_assets.py -- regenerate nibottom-mockup.html's REAL_ASSETS block.

COLD TOOL. Desktop-only, not part of the 3DS build. Pairs with
tools/o2r_texture_extract.py (the generic OTEX decoder this imports) to pull
the specific HUD textures docs/research/native/nibottom-plan.md cites by
file:line, decode them to PNG, base64-encode them, and splice the result into
docs/research/native/nibottom-mockup.html as one JS object literal between
two marker comments. Re-run any time a newer archive should be re-extracted;
it always rewrites the same block, never anything else in the file.

Usage:
    python3 tools/gen_nibottom_mockup_assets.py ARCHIVE.o2r

ARCHIVE.o2r must be a REAL game archive (3610 OTEX+other entries; the
EXPECTED_ENTRY_COUNT tools/o2r_harness/o2r_common.py already checks for), not
the smoke-test fixture that ships in build-3ds/host-tests/smoke-cwd/ (2
entries, ~300 bytes, no OTEX records at all -- o2r_texture_extract.py's own
`list` subcommand reports "0 OTEX entries matched" against that one).

What gets pulled, and why each symbol was picked (cites nibottom-plan.md §2):

  portraits (8, cycling the mockup's PILOTS[] list)
      hud_gfx/aPortrait<Name>Tex, 32x32 rgba16 -- the exact texture
      gCharacterPortraitTextures[] indexes (hud.c:67-97), not the higher-res
      aFullPortrait*/aSmallPortrait* siblings the archive also carries (those
      are the character-select/results-screen portraits, a different asset).

  timer digit atlas (14 glyphs: 0-9, ', ", +, -)
      machine_custom_gfx/aTimerSymbolsTex, 8x224 rgba16, sliced into 14
      8x16 cells top-to-bottom. Glyph order is fzx_hud.h's TimerDigits enum
      (TIMER_DIGIT_0..9, _PRIME, _DOUBLE_PRIME, _PLUS, _MINUS) -- confirmed
      by Hud_DrawTimerDigitRectangle's own `(number * 16) << 5` row stride
      (hud.c:270-278): 224 / 16 = 14, matching the enum's 14 entries exactly.
      The game's own time format uses prime/double-prime as the minute/second
      separators (0'42"18, not 0:42.18) -- Hud_DrawTimerScisThousandths
      (hud.c:338-363) draws exactly that glyph sequence, so the mockup now
      formats times the same way instead of the placeholder colon/dot it
      used before real pixels existed.

  lap-counter digit atlas (6 glyphs: /, 1, 2, 3, 4, 5)
      hud_gfx/aLapCounterSymbolsTex, 8x72 rgba16, sliced into 6 8x12 cells.
      NOT 0-9: Hud_DrawLapCounter (hud.c:1000-1043) indexes this strip
      directly with the lap number itself (`(lap * 12) << 5`, lap 1..5, the
      game's own max) and reserves glyph 0 for '/' (the literal 0 in the
      first gSPTextureRectangle's T coordinate). 72 / 12 = 6, matching.

  position digits (10, one full texture each, not a strip)
      hud_gfx/aPosition0Tex .. aPosition9Tex, 28x32 rgba16 each -- the real
      archive splits what hud_gfx.yaml described as one APPEND-mode strip
      (aPositionDigitTexs) into ten separately-named entries; both describe
      the same pixels, this is just how Torch happened to emit them.

  position ordinal suffixes (ST/ND/RD/TH)
      hud_gfx/aPositionOrdinalSuffix{ST,ND,RD,TH}Tex, 20x20 rgba16 each
      (hud.c:960, PositionSuffix enum in fzx_hud.h).

  position ribbon chips (1st..6th)
      hud_gfx/aPortraitPositionFirstTex .. SixthTex, 16x6 rgba16 each
      (hud.c:102-105, sPositionTextures[]) -- the small under-portrait
      ribbon the game itself draws; replaces the mockup's placeholder
      "4TH" text label.

  labels
      machine_custom_gfx/aLapTex (16x12, "LAP"), hud_gfx/aHudTimeTex
      (24x16, "TIME"), common_assets_compressed/aBestTex (16x12, "BEST"),
      hud_gfx/aHudEnergyTex (72x16, the energy bar's own decorative frame
      texture -- drawn as an underlay behind the mockup's procedural
      gradient fill, same as the game layers them).

  portrait highlight border
      hud_gfx/aCharacterPortraitHighlightBorderTex, 32x32 rgba16
      (hud.c:1521) -- replaces the mockup's plain stroked rectangle.

  general text font glyphs (Font1 16x16, Font2 8x8 -- A-Z, 0-9, '.', '-')
      common_assets_compressed/aFont1<X>Tex, common_assets_compressed/
      aFont2<X>Tex (font.c's sFont1CompTexInfo/sFont2CompTexInfo tables,
      ~font.c:40-420; the 48-slot glyph-index scheme and per-set texture
      naming is documented in decomp/include/fzx_font.h and
      Font_GetCharIndex, font.c). This is the general proportional text
      system the game itself draws course names, pilot names, and most
      non-HUD-digit strings with -- a different, separate asset family from
      aTimerSymbolsTex/aLapCounterSymbolsTex above, which are a dedicated
      fixed-cell digit font used only for the race clock/lap counter.
      Pulled: A-Z (idx 0-25), 0-9 (idx 32-41, texture suffix "Num<N>"),
      '.' (idx 42, suffix "Period"), '-' (idx 30, suffix "Dash"). Per-
      character advance widths (kFont1CharWidths/kFont2CharWidths, same
      file) and the space-glyph width (7px Font1, 3px Font2,
      Font_GetCharacterWidth's FONT_SET_1/FONT_SET_2 cases) are NOT baked
      into the PNGs -- they're compile-time constants, reproduced verbatim
      as JS arrays in the mockup's own <script> (FONT1_WIDTHS/FONT2_WIDTHS),
      cited the same way. Unexpected and worth noting: Font1's glyphs are
      not white -- they decode to a magenta/pink family (~RGB 189,33,189
      sampled from aFont1ATex) baked into the texture itself, i.e. the
      pink theme this pass adds leans on a colour the game's own font
      asset already is, not an invented tint.

  background: pink-themed backdrop, built from real decal/logo textures
      machine_custom_gfx/aDecalCheckeredTex (16x16, i4 -- decodes to pure
      opaque black/white, hud.c/machine customization's generic checker
      decal, reused across many in-game paint-job colour variants) is
      pulled as-is (real, unmodified pixels) and recoloured client-side in
      the mockup's own <script> by remapping its two luminance levels to
      two pink tones, then tiled at low alpha behind the HUD panels. This
      mirrors how the game itself already reuses this exact texture across
      different machine custom-colour palettes -- a pink recolour is the
      same kind of operation the game performs on this asset, not a new
      one invented for this mockup. common_assets_compressed/aTitleLogoTex
      (160x65 rgba16, the real "F-ZERO X" wordmark, transparent background)
      is pulled unmodified and drawn as a low-alpha corner watermark --
      real logo pixels, real colours, untouched.

Everything else in the mockup (the procedurally-drawn minimap shape, the
top-screen preview schematic's box layout) is intentionally NOT replaced
here: those were never asset citations, just mockup chrome -- though all
*text* drawn over them now goes through the real font glyphs above.
"""

import base64
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import o2r_texture_extract as oex  # noqa: E402

MOCKUP = HERE.parent / "docs" / "research" / "native" / "nibottom-mockup.html"
BEGIN_MARK = "/* BEGIN REAL_ASSETS (generated by tools/gen_nibottom_mockup_assets.py) */"
END_MARK = "/* END REAL_ASSETS */"

PILOT_NAMES = [
    ("CaptainFalcon", "CAPTAIN FALCON"),
    ("DrStewart", "DR. STEWART"),
    ("Pico", "PICO"),
    ("SamuraiGoroh", "SAMURAI GOROH"),
    ("JodySummer", "JODY SUMMER"),
    ("MightyGazelle", "MIGHTY GAZELLE"),
    ("MrEad", "MR. EAD"),
    ("Baba", "BABA"),
]

TIMER_GLYPH_NAMES = ["0","1","2","3","4","5","6","7","8","9","'",'"',"+","-"]
LAP_GLYPH_NAMES = ["/","1","2","3","4","5"]
SUFFIXES = ["ST", "ND", "RD", "TH"]
RIBBON_ORDINALS = ["First", "Second", "Third", "Fourth", "Fifth", "Sixth"]

# Font1/Font2 general text glyphs this mockup needs: A-Z, 0-9, '.', '-'.
# (char, texture-name-suffix) -- the suffix is appended to "aFont1"/"aFont2".
FONT_CHAR_SPEC = (
    [(chr(ord("A") + i), chr(ord("A") + i)) for i in range(26)]
    + [(str(d), "Num%d" % d) for d in range(10)]
    + [(".", "Period"), ("-", "Dash")]
)


def load_entries(archive_path):
    z = zipfile.ZipFile(archive_path)  # left open deliberately; caller reads lazily by name
    names = set(z.namelist())
    return z, names


def find_entry(z, names, *candidates):
    for c in candidates:
        if c in names:
            return c
    raise SystemExit("could not find any of %r in archive" % (candidates,))


def decode_entry(z, name):
    data = z.read(name)
    tex = oex.parse_otex(data)
    rgba = oex.decode_to_rgba8(tex)
    return tex, rgba


def png_data_uri(width, height, rgba_slice):
    png = oex.write_png(None, width, height, rgba_slice)
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


def slice_strip(rgba, full_w, cell_h, index):
    """One `full_w` x `cell_h` cell out of a vertically-stacked strip."""
    stride = full_w * 4
    start = index * cell_h * stride
    end = start + cell_h * stride
    return rgba[start:end]


def js_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_assets(archive_path):
    z, names = load_entries(archive_path)
    out = {}

    # -- portraits --------------------------------------------------------
    portraits = {}
    for suffix, _display in PILOT_NAMES:
        name = find_entry(z, names, "hud_gfx/aPortrait%sTex" % suffix)
        tex, rgba = decode_entry(z, name)
        portraits[suffix] = {
            "uri": png_data_uri(tex["width"], tex["height"], rgba),
            "w": tex["width"],
            "h": tex["height"],
            "src": name,
        }
    out["portraits"] = portraits

    # -- portrait highlight border -----------------------------------------
    name = find_entry(z, names, "hud_gfx/aCharacterPortraitHighlightBorderTex")
    tex, rgba = decode_entry(z, name)
    out["highlightBorder"] = {"uri": png_data_uri(tex["width"], tex["height"], rgba), "w": tex["width"], "h": tex["height"], "src": name}

    # -- timer digit atlas: slice into 14 glyphs ---------------------------
    name = find_entry(z, names, "machine_custom_gfx/aTimerSymbolsTex")
    tex, rgba = decode_entry(z, name)
    w = tex["width"]
    cell_h = tex["height"] // len(TIMER_GLYPH_NAMES)
    timer_glyphs = {}
    for i, g in enumerate(TIMER_GLYPH_NAMES):
        timer_glyphs[g] = png_data_uri(w, cell_h, slice_strip(rgba, w, cell_h, i))
    out["timerGlyphs"] = {"w": w, "h": cell_h, "glyphs": timer_glyphs, "src": name}

    # -- lap-counter digit atlas: slice into 6 glyphs -----------------------
    name = find_entry(z, names, "hud_gfx/aLapCounterSymbolsTex")
    tex, rgba = decode_entry(z, name)
    w = tex["width"]
    cell_h = tex["height"] // len(LAP_GLYPH_NAMES)
    lap_glyphs = {}
    for i, g in enumerate(LAP_GLYPH_NAMES):
        lap_glyphs[g] = png_data_uri(w, cell_h, slice_strip(rgba, w, cell_h, i))
    out["lapGlyphs"] = {"w": w, "h": cell_h, "glyphs": lap_glyphs, "src": name}

    # -- position digits (0-9), already ten separate textures --------------
    pos_digits = {}
    pw = ph = None
    for d in range(10):
        name = find_entry(z, names, "hud_gfx/aPosition%dTex" % d)
        tex, rgba = decode_entry(z, name)
        pw, ph = tex["width"], tex["height"]
        pos_digits[str(d)] = png_data_uri(tex["width"], tex["height"], rgba)
    out["positionDigits"] = {"w": pw, "h": ph, "glyphs": pos_digits}

    # -- position ordinal suffixes ------------------------------------------
    suffixes = {}
    sw = sh = None
    for suf in SUFFIXES:
        name = find_entry(z, names, "hud_gfx/aPositionOrdinalSuffix%sTex" % suf)
        tex, rgba = decode_entry(z, name)
        sw, sh = tex["width"], tex["height"]
        suffixes[suf] = png_data_uri(tex["width"], tex["height"], rgba)
    out["positionSuffix"] = {"w": sw, "h": sh, "glyphs": suffixes}

    # -- position ribbon chips (1st..6th) ------------------------------------
    ribbon = []
    rw = rh = None
    for ordw in RIBBON_ORDINALS:
        name = find_entry(z, names, "hud_gfx/aPortraitPosition%sTex" % ordw)
        tex, rgba = decode_entry(z, name)
        rw, rh = tex["width"], tex["height"]
        ribbon.append(png_data_uri(tex["width"], tex["height"], rgba))
    out["positionRibbon"] = {"w": rw, "h": rh, "images": ribbon}

    # -- labels ---------------------------------------------------------------
    labels = {}
    for key, candidates in [
        ("lap", ["machine_custom_gfx/aLapTex"]),
        ("time", ["hud_gfx/aHudTimeTex"]),
        ("best", ["common_assets_compressed/aBestTex"]),
        ("energyFrame", ["hud_gfx/aHudEnergyTex"]),
    ]:
        name = find_entry(z, names, *candidates)
        tex, rgba = decode_entry(z, name)
        labels[key] = {"uri": png_data_uri(tex["width"], tex["height"], rgba), "w": tex["width"], "h": tex["height"], "src": name}
    out["labels"] = labels

    # -- general text font glyphs (Font1 16x16, Font2 8x8) -------------------
    for font_prefix, out_key in (("aFont1", "font1Glyphs"), ("aFont2", "font2Glyphs")):
        glyphs = {}
        gw = gh = None
        for ch, suffix in FONT_CHAR_SPEC:
            name = find_entry(
                z, names,
                "common_assets_compressed/%s%sTex" % (font_prefix, suffix),
            )
            tex, rgba = decode_entry(z, name)
            glyphs[ch] = {
                "uri": png_data_uri(tex["width"], tex["height"], rgba),
                "w": tex["width"],
                "h": tex["height"],
            }
            gh = tex["height"]  # cell height is uniform per font set (16 or 8)
        out[out_key] = {"h": gh, "glyphs": glyphs}

    # -- pink-backdrop source textures: real, unrecoloured pixels -----------
    name = find_entry(z, names, "machine_custom_gfx/aDecalCheckeredTex")
    tex, rgba = decode_entry(z, name)
    out["bgTile"] = {"uri": png_data_uri(tex["width"], tex["height"], rgba), "w": tex["width"], "h": tex["height"], "src": name}

    name = find_entry(z, names, "common_assets_compressed/aTitleLogoTex")
    tex, rgba = decode_entry(z, name)
    out["titleLogo"] = {"uri": png_data_uri(tex["width"], tex["height"], rgba), "w": tex["width"], "h": tex["height"], "src": name}

    z.close()
    return out


def to_js(assets):
    import json

    # json.dumps is valid JS object-literal syntax for our plain
    # str/int/dict/list payload, and keeps this generator dependency-free.
    body = json.dumps(assets, indent=2, sort_keys=True)
    return "const REAL_ASSETS = %s;\n" % body


def splice(js_block):
    html = MOCKUP.read_text()
    if BEGIN_MARK not in html or END_MARK not in html:
        raise SystemExit("marker comments not found in %s -- add them once by hand first" % MOCKUP)
    pre, rest = html.split(BEGIN_MARK, 1)
    _old, post = rest.split(END_MARK, 1)
    new_html = pre + BEGIN_MARK + "\n" + js_block + END_MARK + post
    MOCKUP.write_text(new_html)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: gen_nibottom_mockup_assets.py ARCHIVE.o2r")
    archive_path = sys.argv[1]
    assets = build_assets(archive_path)
    js_block = to_js(assets)
    splice(js_block)
    total_bytes = sum(
        len(v) for v in _iter_data_uris(assets)
    )
    print("wrote REAL_ASSETS block to %s (%d data URIs, %d base64 chars total)" % (
        MOCKUP, _count_uris(assets), total_bytes))


def _iter_data_uris(obj):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "uri" and isinstance(v, str):
                yield v
            else:
                yield from _iter_data_uris(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from _iter_data_uris(v)
    elif isinstance(obj, str) and obj.startswith("data:image/png;base64,"):
        yield obj


def _count_uris(obj):
    return sum(1 for _ in _iter_data_uris(obj))


if __name__ == "__main__":
    main()
