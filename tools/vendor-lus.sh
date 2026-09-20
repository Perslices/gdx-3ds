#!/usr/bin/env bash
# tools/vendor-lus.sh — regenerate / audit port/3ds/vendor/lus/.
#
# The 3DS build compiles a 36-source carve of libultraship plus the 83-file header
# closure those sources pull in. Phase 0 moved that carve — with all 38 lus-*.patch
# folded in — into port/3ds/vendor/lus/ so the libultraship submodule leaves the 3DS
# build entirely. This script is how that tree was produced, kept in-repo so the
# result is auditable and reproducible rather than a one-off copy.
#
# Modes:
#   --closure <build-dir>   re-derive HEADERS.txt from <build-dir>/**/*.o.d and print
#                           it on stdout (the header closure is MEASURED from depfiles,
#                           never hand-maintained).
#   --regen [outdir]        rebuild the vendored tree from the submodule at the pinned
#                           sha with the 38 patches applied, into <outdir>
#                           (default: port/3ds/vendor/lus). Restores the submodule
#                           working tree to the pin on the way out.
#   --verify                --regen into a temp dir, then diff against the committed
#                           tree. This is the reproducibility receipt; it needs the
#                           libultraship submodule checked out, so it runs on desktop
#                           lanes only, never in the 3DS CI.
#   --manifest              recompute MANIFEST.sha256 in place.
#   (no args)               same as --verify.
#
# NOTE (see PROVENANCE.md): from the vendoring commit onward the vendored tree is the
# SOURCE OF TRUTH and port/3ds/vendor/lus/patches/ is frozen history. --verify is
# expected to fail as soon as the vendored tree is edited; PROVENANCE.md records the
# commit at which it last passed.
#
# Written for macOS's stock bash 3.2 (no associative arrays).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LUS_SUB="${ROOT}/libultraship"
VENDOR="${ROOT}/port/3ds/vendor/lus"
PIN="7bca0e22ed8fd28f761a8631741c163e6f4b51e5"

die() { echo "vendor-lus: $*" >&2; exit 1; }

# ------------------------------------------------------------------- closure --
# Every header the 3DS build actually opens out of libultraship/, as recorded by
# gcc's own -MD depfiles. Normalises the port/3ds/<dir>/../../../ prefix the include
# flags produce, drops the .cpp/.c entries and the "<obj>.o:" target lines.
do_closure() { # build-dir
    local bd="$1"
    [ -d "$bd" ] || die "no such build dir: $bd"
    local n
    n="$(find "$bd" -name '*.o.d' | wc -l | tr -d ' ')"
    [ "$n" -gt 0 ] || die "no *.o.d under $bd (configure+build it first)"
    cat $(find "$bd" -name '*.o.d') | tr ' ' '\n' | grep '/libultraship/' \
        | sed "s|^${ROOT}/||; s|port/3ds/[a-z_]*/\.\./\.\./\.\./||; s|^libultraship/||" \
        | grep -vE '\.(cpp|c)$|\.o:$' | sort -u
}

# ------------------------------------------------------------------- sources --
# Read the carve list back out of the CMake file so the two can never drift.
carve_sources() {
    sed -n 's|.*"\${GDX3DS_LUS_ROOT}/\(src/[^"]*\)".*|\1|p' "${ROOT}/port/3ds/gfx/CMakeLists.txt"
}

# ------------------------------------------------------------------ manifest --
write_manifest() { # dir
    local d="$1"
    ( cd "$d" && rm -f MANIFEST.sha256 \
      && find . -type f ! -path './patches/*' ! -name MANIFEST.sha256 \
         | sed 's|^\./||' | LC_ALL=C sort \
         | xargs shasum -a 256 > MANIFEST.sha256 )
}

# --------------------------------------------------------------------- regen --
do_regen() { # outdir
    local out="$1"
    [ -f "${LUS_SUB}/CMakeLists.txt" ] || die "libultraship submodule not checked out — --regen needs it"
    local head
    head="$(git -C "$LUS_SUB" rev-parse HEAD)"
    [ "$head" = "$PIN" ] || die "libultraship is at ${head}, expected pin ${PIN}"
    [ -f "${VENDOR}/patches/ORDER.txt" ] || die "missing ${VENDOR}/patches/ORDER.txt"
    [ -f "${VENDOR}/HEADERS.txt" ]      || die "missing ${VENDOR}/HEADERS.txt"

    # Read the two inputs out of the way FIRST: --regen with no argument writes into
    # $VENDOR itself, and the wipe below would otherwise eat its own inputs.
    local stash
    stash="$(mktemp -d)"
    cp "${VENDOR}/HEADERS.txt" "${stash}/HEADERS.txt"
    cp "${VENDOR}/patches/ORDER.txt" "${stash}/ORDER.txt"

    # Start from the pin regardless of what the working tree looks like, then apply
    # the frozen stack in ORDER.txt. Restore the pin on the way out either way.
    git -C "$LUS_SUB" checkout -f -q "$PIN" -- .
    local restore='git -C "'"$LUS_SUB"'" checkout -f -q "'"$PIN"'" -- .; rm -rf "'"$stash"'"'
    trap "$restore" EXIT

    local pats=()
    while IFS= read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        [ -f "${VENDOR}/patches/${p}" ] || die "patch listed in ORDER.txt but not on disk: $p"
        pats+=("${VENDOR}/patches/${p}")
    done < "${stash}/ORDER.txt"
    echo "applying ${#pats[@]} patches to ${LUS_SUB} at ${PIN}"
    git -C "$LUS_SUB" apply "${pats[@]}" || die "patch stack does not apply to the pin"

    # Wipe only the generated subtrees; patches/ is frozen provenance and
    # PROVENANCE.md is hand-written, so neither is ever regenerated.
    rm -rf "$out/include" "$out/src" "$out/cmake" \
           "$out/LICENSE" "$out/HEADERS.txt" "$out/MANIFEST.sha256"
    mkdir -p "$out/cmake"
    cp "${LUS_SUB}/LICENSE"           "$out/LICENSE"
    cp "${LUS_SUB}/cmake/cvars.cmake" "$out/cmake/cvars.cmake"
    cp "${stash}/HEADERS.txt"         "$out/HEADERS.txt"

    local f n=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        mkdir -p "$out/$(dirname "$f")"
        cp "${LUS_SUB}/${f}" "$out/$f"
        n=$((n + 1))
    done < "${stash}/HEADERS.txt"
    echo "copied ${n} closure headers"

    n=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        mkdir -p "$out/$(dirname "$f")"
        cp "${LUS_SUB}/${f}" "$out/$f"
        n=$((n + 1))
    done < <(carve_sources)
    echo "copied ${n} carve sources"

    write_manifest "$out"
    trap - EXIT
    eval "$restore"
    echo "regen OK -> ${out} (submodule restored to ${PIN})"
}

# -------------------------------------------------------------------- verify --
do_verify() {
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    do_regen "${tmp}/lus" >/dev/null
    # patches/ and PROVENANCE.md are provenance, not regenerated output.
    if diff -ru -x patches -x PROVENANCE.md "${tmp}/lus" "$VENDOR"; then
        echo "verify: PASS — committed tree is byte-identical to a fresh regen"
    else
        echo "verify: FAIL — see the diff above." >&2
        echo "  Expected once the vendored tree is edited: it is the source of truth" >&2
        echo "  now, and patches/ is frozen history (PROVENANCE.md)." >&2
        exit 1
    fi
}

case "${1:---verify}" in
    --closure)  shift; do_closure "${1:?--closure needs a build dir}" ;;
    --regen)    shift; do_regen "${1:-$VENDOR}" ;;
    --manifest) write_manifest "$VENDOR"; echo "manifest rewritten" ;;
    --verify)   do_verify ;;
    -h|--help)  sed -n '2,34p' "$0" ;;
    *)          die "unknown option: $1 (try --help)" ;;
esac
