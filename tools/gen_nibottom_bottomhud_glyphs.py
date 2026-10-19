#!/usr/bin/env python3
"""tools/gen_nibottom_bottomhud_glyphs.py -- bake the DASH's REAL game glyph atlases into a C
header, straight from the .o2r archive (same technique and same source citations as
tools/gen_nibottom_mockup_assets.py, which already proved these exact entries decode and slice
correctly for docs/research/native/nibottom-mockup.html).

COLD TOOL. Desktop-only, not part of the 3DS build (no CMake step runs this; its OUTPUT is
committed, exactly like tools/gen_nibottom_bottomhud_assets.py's own widget bake). Consumer:
port/3ds/gdx3ds_bottomhud.c.

COORDINATOR FIX (nibottom Phase 2, 3rd visual-feedback round, item 1): "the hand-drawn 3x5 font
breaks an explicit user requirement... replace it with the real glyph atlases, baked into the C
headers from the o2r." This replaces gdx3ds_bottomhud.c's hand-authored kDashFont entirely.

What gets pulled and why (mirrors gen_nibottom_mockup_assets.py's own citations):

  timer digit atlas (14 glyphs: 0-9, ', ", +, -)
      machine_custom_gfx/aTimerSymbolsTex, 8x224 rgba16, sliced into 14 8x16 cells. Used for the
      LAP/TIME/BEST/GAP values this file renders live every tick (format "M'SS\"CC").

  lap-counter digit atlas (6 glyphs: /, 1, 2, 3, 4, 5)
      hud_gfx/aLapCounterSymbolsTex, 8x72 rgba16, sliced into 6 8x12 cells. Used for the LAP
      value ("<lap>/<totalLaps>").

  position digits (10, 28x32 rgba16 each) + ordinal suffixes (4, 20x20 rgba16 each)
      hud_gfx/aPosition0Tex..aPosition9Tex, hud_gfx/aPositionOrdinalSuffix{ST,ND,RD,TH}Tex. Used
      for the standings rank badges AND the OUR-position big numeral (replacing the old hand-font
      "1ST".."5TH"/"P<n>/<n>" console text in both places).

  Font1 (16x16, course names) / Font2 (8x8, the MODE chip label, the MENU button label)
      common_assets_compressed/aFont1<X>Tex / aFont2<X>Tex, A-Z (idx 0-25) + 0-9 (idx 32-41,
      suffix "Num<N>") + '.' (idx 42) + '-' (idx 30), per font.c's 48-slot glyph-index scheme
      (decomp/include/fzx_font.h, Font_GetCharIndex). Per-character PROPORTIONAL advance widths
      (kFont1CharWidths/kFont2CharWidths, font.c) are baked in per-glyph as `adv`, not derived at
      runtime -- the C side never needs the character->index mapping, just a linear glyph table.

  character portraits (30, 32x32 rgba16 each) -- hw bug card n60-nibottom-notes, item 2: "the
      standings portraits don't update based on the racers/position... they're static." Source of
      the ORIGINAL per-racer portrait+sizing: decomp/src/overlays/ovl_i3/hud.c's
      gCharacterPortraitTextures[] (hud_gfx/aPortrait<Name>Tex, same 30-entry order as the
      Racer::character enum) and Hud_DrawRacePortraits (i<3 draws at scale 1.0/32x32, i>=3 at
      0.75/24x24 -- the "top positions get a bigger portrait" rule gdx3ds_bottomhud.c's own
      DashPaintStandingsPortraits mirrors). Keyed here by the character index as a decimal string
      ("0".."29"), looked up with the same DashFindGlyph the rest of this file already uses.

  KO star icon (1, 8x8 rgba16) -- hw bug card n60-nibottom-notes, item 6: "the knockout stars...
      are not visible anywhere on the second screen HUD." hud_gfx/aHudRacersKOdTex, the same 8x8
      icon decomp/src/overlays/ovl_i3/hud.c's Hud_DrawKOStars stamps once per gRacersKOd.

Usage:
    python3 tools/gen_nibottom_bottomhud_glyphs.py [ARCHIVE.o2r] [--out FILE]

Default ARCHIVE.o2r: the Azahar SD copy this checkout's own tools/ci-3ds.sh --emu already reads
(~/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/fzerox.o2r), 17,150,811 bytes.
"""
import argparse
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import o2r_texture_extract as oex  # noqa: E402

DEFAULT_ARCHIVE = (
    Path.home() / "Library" / "Application Support" / "Azahar" / "sdmc" / "3ds" / "gdiffuser" /
    "fzerox.o2r"
)
DEFAULT_OUT = HERE.parent / "port" / "3ds" / "gdx3ds_bottomhud_glyphs.h"

TIMER_GLYPH_NAMES = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "'", '"', "+", "-"]
LAP_GLYPH_NAMES = ["/", "1", "2", "3", "4", "5"]
SUFFIXES = ["ST", "ND", "RD", "TH"]

# gCharacterPortraitTextures[] order, decomp/src/overlays/ovl_i3/hud.c:67-96 -- index == the
# Racer::character field DashPaintStandingsPortraits/DashPaintOurPosition's snapshot carries.
PORTRAIT_NAMES = [
    "CaptainFalcon", "DrStewart", "Pico", "SamuraiGoroh", "JodySummer", "MightyGazelle", "MrEad",
    "Baba", "Octoman", "GomarAndShioh", "KateAlen", "RogerBuster", "JamesMcCloud", "Leon",
    "AntonioGuster", "BlackShadow", "MichaelChain", "JackLevin", "SuperArrow", "MrsArrow",
    "JohnTanaka", "Beastman", "Zoda", "DrClash", "SilverNeelsen", "BioRex", "Draq", "Billy",
    "TheSkull", "BloodFalcon",
]

# font.c's kFont1CharWidths / kFont2CharWidths, verbatim -- same arrays
# docs/research/native/nibottom-mockup.html already embeds and cites (48-slot scheme; only the
# A-Z/0-9/./- slots are used here, same subset gen_nibottom_mockup_assets.py pulls textures for).
FONT1_WIDTHS = [13, 15, 14, 15, 14, 13, 15, 15, 7, 14, 15, 14, 17, 14, 14, 15, 15, 14, 14, 14, 14,
                12, 16, 16, 14, 14, 8, 8, 16, 8, 16, 7, 13, 7, 13, 13, 13, 12, 13, 12, 13, 13, 8,
                8, 16, 16, 16, 8]
FONT2_WIDTHS = [8, 8, 8, 8, 8, 8, 8, 8, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8]
FONT1_SPACE = 7
FONT2_SPACE = 3

# char -> (texture-name-suffix, kFontNCharWidths index)
FONT_CHAR_SPEC = (
    [(chr(ord("A") + i), chr(ord("A") + i), i) for i in range(26)]
    + [(str(d), "Num%d" % d, 32 + d) for d in range(10)]
    + [(".", "Period", 42), ("-", "Dash", 30)]
)


def load_entries(archive_path):
    z = zipfile.ZipFile(archive_path)
    return z, set(z.namelist())


def find_entry(z, names, *candidates):
    for c in candidates:
        if c in names:
            return c
    raise SystemExit("could not find any of %r in archive" % (candidates,))


def decode_words(z, name):
    """Returns (w, h, [uint16, ...]) -- RGBA5551, same bit layout gdx3ds_bottomhud.c's
    Rgba5551To565 already expects (r5 g5 b5 a1, MSB-first), as plain-int words (endianness
    already resolved here, same convention tools/gen_nibottom_bottomhud_assets.py uses)."""
    tex = oex.parse_otex(z.read(name))
    if tex["typeName"] != "rgba16":
        raise SystemExit("%s: expected rgba16, got %s" % (name, tex["typeName"]))
    w, h = tex["width"], tex["height"]
    words = struct.unpack(">%dH" % (w * h), tex["pixels"])
    return w, h, words


def slice_strip(words, full_w, cell_h, index):
    start = index * cell_h * full_w
    return words[start:start + cell_h * full_w]


class Glyph:
    __slots__ = ("key", "w", "h", "words", "adv")

    def __init__(self, key, w, h, words, adv=None):
        self.key = key
        self.w = w
        self.h = h
        self.words = words
        self.adv = adv if adv is not None else w


def emit_glyph_array(out, c_name, glyphs):
    """One flat pixel array per glyph set (same per-widget-array shape
    tools/gen_nibottom_bottomhud_assets.py already uses) plus an index table of
    {key, w, h, adv, pixel-offset} structs."""
    out.write("static const unsigned short %s_pixels[] = {\n" % c_name)
    offsets = []
    off = 0
    for g in glyphs:
        offsets.append(off)
        vals = list(g.words)
        for i in range(0, len(vals), 16):
            out.write("    " + ",".join(str(v) for v in vals[i:i + 16]) + ",\n")
        off += len(vals)
    out.write("};\n")
    out.write("static const DashGlyph %s[] = {\n" % c_name)
    for g, o in zip(glyphs, offsets):
        key_c = g.key.replace("\\", "\\\\").replace('"', '\\"')
        out.write('    {"%s", %d, %d, %d, %s_pixels + %d},\n' % (key_c, g.w, g.h, g.adv, c_name, o))
    out.write("};\n")
    out.write("#define %s_COUNT %d\n\n" % (c_name.upper(), len(glyphs)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive", nargs="?", default=str(DEFAULT_ARCHIVE))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    archive_path = Path(args.archive)
    if not archive_path.is_file():
        raise SystemExit("archive not found: %s (pass ARCHIVE.o2r explicitly)" % archive_path)
    z, names = load_entries(archive_path)

    # -- timer digit atlas --------------------------------------------------
    name = find_entry(z, names, "machine_custom_gfx/aTimerSymbolsTex")
    w, h, words = decode_words(z, name)
    cell_h = h // len(TIMER_GLYPH_NAMES)
    timer_glyphs = [Glyph(g, w, cell_h, slice_strip(words, w, cell_h, i))
                     for i, g in enumerate(TIMER_GLYPH_NAMES)]

    # -- lap-counter digit atlas ---------------------------------------------
    name = find_entry(z, names, "hud_gfx/aLapCounterSymbolsTex")
    w, h, words = decode_words(z, name)
    cell_h = h // len(LAP_GLYPH_NAMES)
    lap_glyphs = [Glyph(g, w, cell_h, slice_strip(words, w, cell_h, i))
                   for i, g in enumerate(LAP_GLYPH_NAMES)]

    # -- position digits ------------------------------------------------------
    pos_glyphs = []
    for d in range(10):
        name = find_entry(z, names, "hud_gfx/aPosition%dTex" % d)
        w, h, words = decode_words(z, name)
        pos_glyphs.append(Glyph(str(d), w, h, words))

    # -- position ordinal suffixes --------------------------------------------
    suffix_glyphs = []
    for suf in SUFFIXES:
        name = find_entry(z, names, "hud_gfx/aPositionOrdinalSuffix%sTex" % suf)
        w, h, words = decode_words(z, name)
        suffix_glyphs.append(Glyph(suf, w, h, words))

    # -- Font1 / Font2 ----------------------------------------------------------
    font1_glyphs = []
    font2_glyphs = []
    for ch, suffix, idx in FONT_CHAR_SPEC:
        name = find_entry(z, names, "common_assets_compressed/aFont1%sTex" % suffix)
        w, h, words = decode_words(z, name)
        font1_glyphs.append(Glyph(ch, w, h, words, adv=FONT1_WIDTHS[idx]))
        name = find_entry(z, names, "common_assets_compressed/aFont2%sTex" % suffix)
        w, h, words = decode_words(z, name)
        font2_glyphs.append(Glyph(ch, w, h, words, adv=FONT2_WIDTHS[idx]))

    # -- character portraits -----------------------------------------------------------
    portrait_glyphs = []
    for idx, suffix in enumerate(PORTRAIT_NAMES):
        name = find_entry(z, names, "hud_gfx/aPortrait%sTex" % suffix)
        w, h, words = decode_words(z, name)
        portrait_glyphs.append(Glyph(str(idx), w, h, words))

    # -- KO star icon -------------------------------------------------------------------
    name = find_entry(z, names, "hud_gfx/aHudRacersKOdTex")
    w, h, words = decode_words(z, name)
    ko_star_glyphs = [Glyph("star", w, h, words)]

    z.close()

    out_path = Path(args.out)
    with open(out_path, "w") as out:
        out.write("/* port/3ds/gdx3ds_bottomhud_glyphs.h -- GENERATED by\n")
        out.write(" * tools/gen_nibottom_bottomhud_glyphs.py. Do not hand-edit; see that script's\n")
        out.write(" * own module doc for what each glyph set is and why. Consumer:\n")
        out.write(" * port/3ds/gdx3ds_bottomhud.c. RGBA5551 words (r5g5b5a1, MSB-first), same\n")
        out.write(" * convention gdx3ds_bottomhud_assets.h's Rgba5551To565 already expects. */\n")
        out.write("#ifndef GDX3DS_BOTTOMHUD_GLYPHS_H\n#define GDX3DS_BOTTOMHUD_GLYPHS_H\n\n")
        out.write("typedef struct {\n")
        out.write("    const char* key;\n")
        out.write("    unsigned char w, h;\n")
        out.write("    unsigned char adv; /* proportional advance width (Font1/Font2); w for the rest */\n")
        out.write("    const unsigned short* pixels;\n")
        out.write("} DashGlyph;\n\n")
        emit_glyph_array(out, "kDashGlyphTimer", timer_glyphs)
        emit_glyph_array(out, "kDashGlyphLap", lap_glyphs)
        emit_glyph_array(out, "kDashGlyphPosDigit", pos_glyphs)
        emit_glyph_array(out, "kDashGlyphPosSuffix", suffix_glyphs)
        emit_glyph_array(out, "kDashGlyphFont1", font1_glyphs)
        emit_glyph_array(out, "kDashGlyphFont2", font2_glyphs)
        emit_glyph_array(out, "kDashGlyphPortrait", portrait_glyphs)
        emit_glyph_array(out, "kDashGlyphKoStar", ko_star_glyphs)
        out.write("#define DASH_FONT1_SPACE %d\n" % FONT1_SPACE)
        out.write("#define DASH_FONT2_SPACE %d\n" % FONT2_SPACE)
        out.write("\n#endif /* GDX3DS_BOTTOMHUD_GLYPHS_H */\n")

    total_bytes = sum(
        len(g.words) * 2
        for gl in (timer_glyphs, lap_glyphs, pos_glyphs, suffix_glyphs, font1_glyphs, font2_glyphs,
                    portrait_glyphs, ko_star_glyphs)
        for g in gl
    )
    print("wrote %s (%d bytes of raw glyph pixels)" % (out_path, total_bytes))


if __name__ == "__main__":
    main()
