#!/usr/bin/env bash
# machines_harness.sh -- host differential harness for the native machine meshes
# (docs/research/native/07-machines-plan.md section 4). Tests:
#   A  walker vs independent Python decoder over all 295 body emitters (EK on: x2 greyscale)
#   B  colour attribution / INHERIT table vs the committed golden file
#   C  gdx3ds_vtx_math.h vs a double-precision reference + the verbatim interpreter snippet
# Usage: tools/native/machines_harness.sh [--update-golden] [--out DIR]
# Needs fzerox.o2r: $GDX_O2R, else the Azahar virtual SD copy. Exit 0 = all pass;
# exit 3 = skipped (no o2r), which the CI records as a pass with a "skipped" detail.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/machines-harness"
UPDATE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --update-golden) UPDATE=1 ;;
        --out) OUT="$2"; shift ;;
        *) echo "unknown arg $1" >&2; exit 2 ;;
    esac
    shift
done
O2R="${GDX_O2R:-$HOME/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/fzerox.o2r}"
if [ ! -f "$O2R" ]; then
    echo "SKIP: no fzerox.o2r (set GDX_O2R)"
    exit 3
fi
mkdir -p "$OUT"
DEC="$ROOT/decomp"
MD="$DEC/src/overlays/ovl_i9/machine_draw.c"
GAME_FLAGS=(-I"$DEC" -I"$DEC/include" -I"$DEC/src" -I"$ROOT/include" -DPORT=1 -DGDIFFUSER_PORT=1 -DNON_MATCHING=1
            -DNON_EQUIVALENT=1 -DAVOID_UB=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -DASSET_VERSION=us
            -DASSET_REVISION=rev0 -D_STRINGS_H_)
EK_FLAGS=(-DEXPANSION_KIT=1 -DMFS_VERSION=1 -DBUILD_VERSION=VERSION_J)
CFL=(${HARNESS_CFLAGS:--O1})   # e.g. HARNESS_CFLAGS="-g -O0 -fsanitize=address" to debug

# Function table from the DEFINITIONS in machine_draw.c (prototypes end with ';').
# Kind by name: Front* = 12 colour args, Rear*/Wing* = 6, Load*Textures = texture list,
# Machine_DrawCustom = composite (skipped), everything else = stock (Gfx*) only.
grep -E '^Gfx\* Machine_Draw[A-Za-z0-9_]+\(' "$MD" | grep -v ';$' | sed -E 's/^Gfx\* (Machine_Draw[A-Za-z0-9_]+)\(.*/\1/' \
  | awk '{
      n=$1;
      if (n=="Machine_DrawCustom" || n=="Machine_DrawLoadCustomTextures") next;  # composites taking args
      if (n ~ /Textures$/) print "TEX(" n ")";
      else if (n ~ /^Machine_DrawFront/) print "FRONT(" n ")";
      else if (n ~ /^Machine_DrawRear/) print "REAR(" n ")";
      else if (n ~ /^Machine_DrawWing/) print "WING(" n ")";
      else print "STOCK(" n ")";
    }' > "$OUT/machines_funcs.inc"
NFUNCS=$(wc -l < "$OUT/machines_funcs.inc" | tr -d ' ')
echo "functions: $NFUNCS (stock+parts+textures)"

STATUS=0
run_variant() { # name extra-flags...
    local name="$1"; shift
    echo "== $name"
    cc -c "${CFL[@]}" -w "$MD" "${GAME_FLAGS[@]}" "$@" -o "$OUT/md_$name.o"
    cc -c "${CFL[@]}" -w "$ROOT/port/gen/AssetBindings.c" "${GAME_FLAGS[@]}" "$@" -I"$ROOT/port" -o "$OUT/ab_$name.o"
    cc "${CFL[@]}" -w "$ROOT/port/tests/machines_harness.c" "$ROOT/port/3ds/gfx/gdx3ds_mesh_walk.c" \
        "$ROOT/port/3ds/assets/third_party/miniz/miniz.c" -DMINIZ_NO_TIME \
        "${GAME_FLAGS[@]}" "$@" -I"$OUT" -I"$ROOT/port/3ds/gfx" -I"$ROOT/port/3ds/assets/third_party/miniz" \
        "$OUT/md_$name.o" "$OUT/ab_$name.o" -o "$OUT/harness_$name"
    mkdir -p "$OUT/$name"
    if ! "$OUT/harness_$name" "$O2R" "$OUT/$name" > "$OUT/$name/harness.log" 2>&1; then
        echo "FAIL: harness self-checks ($name)"; tail -5 "$OUT/$name/harness.log"; STATUS=1
    fi
    tail -2 "$OUT/$name/harness.log"
    # Test A: independent decoder
    python3 "$ROOT/tools/native/gfx_decode.py" lists "$OUT/$name" > "$OUT/$name/python.txt"
    if cmp -s "$OUT/$name/walk.txt" "$OUT/$name/python.txt"; then
        echo "PASS: test A walker == python decoder ($(grep -c '^T ' "$OUT/$name/walk.txt") triangles, $(grep -c '^F ' "$OUT/$name/walk.txt") lists)"
    else
        echo "FAIL: test A ($name): walker vs python differ"; diff "$OUT/$name/walk.txt" "$OUT/$name/python.txt" | head -10; STATUS=1
    fi
    # Test B: golden INHERIT / attribution table
    local golden="$ROOT/port/tests/golden/machines_inherit_$name.txt"
    if [ "$UPDATE" = 1 ]; then
        cp "$OUT/$name/inherit.txt" "$golden"; echo "updated $golden"
    elif [ ! -f "$golden" ]; then
        echo "FAIL: missing golden $golden (run with --update-golden)"; STATUS=1
    elif cmp -s "$OUT/$name/inherit.txt" "$golden"; then
        echo "PASS: test B inherit/attribution table == golden ($(grep -c '^F ' "$golden") lists, $(grep -o 'INHERIT' "$golden" | wc -l | tr -d ' ') inherit runs)"
    else
        echo "FAIL: test B ($name): inherit table differs from golden"; diff "$OUT/$name/inherit.txt" "$golden" | head -10; STATUS=1
    fi
}
run_variant noek
run_variant ek "${EK_FLAGS[@]}"

# Test C: vertex math
echo "== test C"
if cc -O1 -ffp-contract=off -w "$ROOT/port/tests/vtx_math_tests.c" -I"$ROOT/port/3ds/gfx" -lm -o "$OUT/vtx_math_tests" \
   && "$OUT/vtx_math_tests" > "$OUT/vtx_math.log" 2>&1; then
    tail -1 "$OUT/vtx_math.log"
else
    echo "FAIL: test C"; tail -5 "$OUT/vtx_math.log" 2>/dev/null; STATUS=1
fi
if [ "$STATUS" = 0 ]; then echo "ALL PASS: machines harness (A, B x2 variants, C)"; fi
exit $STATUS
