#!/usr/bin/env bash
# tools/ci-3ds.sh — one-command local CI for the Nintendo 3DS port.
#
# From a clean checkout (submodules initialized) this script:
#   1. verifies the devkitPro toolchain is present;
#   2. verifies/applies the port/3ds/patches/ decomp patches (idempotent —
#      each submodule's FULL ordered stack is checked as one combined patchset,
#      so already-applied trees are detected via a whole-stack reverse check
#      even when patches share overlapping context; a stack that neither applies
#      nor reverses cleanly fails LOUDLY, or warns under SKIP_PATCH_GATE=1);
#   3. builds the host-side unit tests that run on this machine and runs them
#      (gdx_gfx_pack_tests, gdx_gfx_convert_tests, gdx_track_template_tests, gdx3ds_fs_host_smoke,
#      gdx_machines_harness —
#      compiled directly with the host compiler; no desktop configure needed);
#   4. configures + builds the 3DS target (G-Diffuser-3DS.3dsx) and the DL
#      replay harness (gdx3ds-dl-tests.3dsx) with the devkitARM toolchain,
#      preferring Ninja + ccache when both are on PATH and timing configure,
#      full and incremental builds;
#   5. with --emu, boots the game .3dsx in Azahar for ${EMU_BOOT_SECONDS}s and
#      asserts the frame heartbeat advances with zero [fatal] lines
#      (honours the shared /tmp/azahar.lock emulator-lock protocol);
#   6. prints a PASS/FAIL summary table and exits nonzero on any failure.
#
# Usage:  tools/ci-3ds.sh [--emu] [--jobs N] [--no-ninja] [--no-ccache]
#                         [--no-incremental]
#
# Notes:
#  - The LUS carve is VENDORED at port/3ds/vendor/lus/ with all 38 lus-*.patch
#    already folded in (port/3ds/vendor/lus/PROVENANCE.md), so the 3DS lane
#    needs neither the libultraship submodule nor its patch stack. Only the 10
#    decomp-*.patch are applied here.
#  - Written for macOS's stock bash 3.2 (no associative arrays, BSD stat).
#  - DEVKITPRO defaults to /opt/devkitpro; export DEVKITPRO to override.
#  - The emulator smoke needs the prebaked archives staged on Azahar's virtual
#    SD card and log_filter=*:Debug — see docs/3DS-HARDWARE.md, "Building from source".

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD_3DS="${ROOT}/build-3ds"
HOST_TEST_DIR="${BUILD_3DS}/host-tests"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
RUN_EMU=0
USE_NINJA=1
USE_CCACHE=1
RUN_INCREMENTAL=1
# The file an incremental-build timing touches: the biggest carve TU.
INCR_TOUCH="port/3ds/vendor/lus/src/fast/interpreter.cpp"

EMU_BOOT_SECONDS=45
AZAHAR_APP="/Applications/Azahar.app"
AZAHAR_BIN="${AZAHAR_APP}/Contents/MacOS/azahar"   # existence probe only — see the launch below
AZAHAR_LOG="${HOME}/Library/Application Support/Azahar/log/azahar_log.txt"
AZAHAR_CFG="${HOME}/Library/Application Support/Azahar/config/qt-config.ini"
AZAHAR_SDMC="${HOME}/Library/Application Support/Azahar/sdmc"
AZAHAR_LOCK="/tmp/azahar.lock"
AZAHAR_LOCK_POLL_SECONDS=30
AZAHAR_LOCK_WAIT_MAX_SECONDS=600   # 10 min
AZAHAR_LOCK_STALE_SECONDS=900      # steal if older than 15 min
HOLDING_LOCK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --emu)  RUN_EMU=1 ;;
        --jobs) shift; JOBS="$1" ;;
        --no-ninja)       USE_NINJA=0 ;;
        --no-ccache)      USE_CCACHE=0 ;;
        --no-incremental) RUN_INCREMENTAL=0 ;;
        -h|--help)
            sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------- reporting --
STEP_NAMES=()
STEP_RESULTS=()
STEP_DETAILS=()
FAILED=0

record() { # name result detail
    STEP_NAMES+=("$1"); STEP_RESULTS+=("$2"); STEP_DETAILS+=("$3")
    if [ "$2" = "FAIL" ]; then FAILED=1; fi
}

banner() { printf '\n=== %s ===\n' "$1"; }

summary() {
    printf '\n%s\n' '================ ci-3ds summary ================'
    printf '%-34s %-6s %s\n' 'step' 'result' 'detail'
    printf '%-34s %-6s %s\n' '----' '------' '------'
    local i
    for i in "${!STEP_NAMES[@]}"; do
        printf '%-34s %-6s %s\n' "${STEP_NAMES[$i]}" "${STEP_RESULTS[$i]}" "${STEP_DETAILS[$i]}"
    done
    if [ "$FAILED" -ne 0 ]; then
        printf '\nRESULT: FAIL\n'
    else
        printf '\nRESULT: PASS\n'
    fi
}

release_lock() {
    if [ "$HOLDING_LOCK" -eq 1 ]; then
        rmdir "$AZAHAR_LOCK" 2>/dev/null || true
        HOLDING_LOCK=0
    fi
}
trap 'release_lock' EXIT

# ---------------------------------------------------------------- preflight --
banner "preflight: toolchain"
PREFLIGHT_OK=1
for probe in \
    "${DEVKITPRO}/cmake/3DS.cmake" \
    "${DEVKITPRO}/devkitARM/bin/arm-none-eabi-gcc"; do
    if [ ! -e "$probe" ]; then
        echo "MISSING: $probe" >&2
        PREFLIGHT_OK=0
    fi
done
command -v cmake >/dev/null 2>&1 || { echo "MISSING: cmake on PATH" >&2; PREFLIGHT_OK=0; }
command -v cc    >/dev/null 2>&1 || { echo "MISSING: cc (host compiler)" >&2; PREFLIGHT_OK=0; }
# The 3DS lane needs decomp only — the LUS carve is vendored (see the note above),
# so libultraship/ may be absent or empty here and everything must still work.
if [ ! -e "${ROOT}/decomp/Makefile" ]; then
    echo "decomp submodule not initialized. Run: git submodule update --init decomp fzerox-expansion-kit" >&2
    PREFLIGHT_OK=0
fi
if [ ! -e "${ROOT}/port/3ds/vendor/lus/cmake/cvars.cmake" ]; then
    echo "MISSING: port/3ds/vendor/lus/ — the vendored LUS carve is part of this repo" >&2
    PREFLIGHT_OK=0
fi
if [ "$PREFLIGHT_OK" -ne 1 ]; then
    record "preflight" FAIL "toolchain/submodules missing (see above)"
    summary; exit 1
fi
echo "devkitPro: ${DEVKITPRO}  cmake: $(cmake --version | head -1)"
record "preflight" PASS "devkitPro + host compiler + decomp + vendored LUS carve present"

# ----------------------------------------------------------- vendor manifest --
# port/3ds/vendor/lus/MANIFEST.sha256 is the vendored carve's content receipt: with the
# libultraship submodule gone from the 3DS build, it is the ONLY thing that says which
# bytes the carve is supposed to contain. It went stale twice (interpreter.h drifted, and
# PROVENANCE.md was never listed) because nothing checked it, so check it here. Editing a
# vendored file is fine and expected — regenerate with `tools/vendor-lus.sh --manifest` in
# the same commit. Needs no submodule and no network: it is a plain checksum -c.
#
# macOS ships `shasum` (perl); a stripped Linux CI container may ship only GNU
# `sha256sum` and no perl at all. Both read the same `<hash>  <path>` lines and both
# print `<path>: OK`, so either verifies the manifest — pick whichever is on PATH
# rather than making perl a build dependency.
banner "vendored LUS manifest"
if command -v shasum >/dev/null 2>&1; then
    sha256_check() { shasum -a 256 -c "$1"; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256_check() { sha256sum -c "$1"; }
else
    sha256_check() { echo "neither shasum nor sha256sum is on PATH" >&2; return 1; }
fi
VENDOR_LUS="${ROOT}/port/3ds/vendor/lus"
MANIFEST_LOG="${HOST_TEST_DIR:-${ROOT}/build-3ds/host-tests}/vendor-manifest.log"
mkdir -p "$(dirname "${MANIFEST_LOG}")"
VENDOR_UNLISTED="$( (cd "${VENDOR_LUS}" && find . -type f ! -path './patches/*' ! -name MANIFEST.sha256 \
    | sed 's|^\./||' | LC_ALL=C sort) | comm -23 - <(awk '{print $2}' "${VENDOR_LUS}/MANIFEST.sha256" | LC_ALL=C sort) )"
if (cd "${VENDOR_LUS}" && sha256_check MANIFEST.sha256) > "${MANIFEST_LOG}" 2>&1 && [ -z "${VENDOR_UNLISTED}" ]; then
    echo "PASS: MANIFEST.sha256 matches the vendored tree ($(grep -c ': OK$' "${MANIFEST_LOG}") files)"
    record "vendor manifest" PASS "$(grep -c ': OK$' "${MANIFEST_LOG}") files match"
else
    echo "FAIL: port/3ds/vendor/lus/MANIFEST.sha256 is stale — run tools/vendor-lus.sh --manifest" >&2
    grep -v ': OK$' "${MANIFEST_LOG}" | head -10 >&2
    [ -n "${VENDOR_UNLISTED}" ] && echo "unlisted files: ${VENDOR_UNLISTED}" >&2
    record "vendor manifest" FAIL "stale — run tools/vendor-lus.sh --manifest"
fi

# ------------------------------------------------------------------ patches --
# Apply order follows port/3ds/patches/README.md. Every *.patch on disk must be
# listed here — an unknown patch means this script is stale, and that FAILS.
#
# decomp only. The 38 lus-*.patch are folded into the vendored carve at
# port/3ds/vendor/lus/ and live on there as frozen provenance under
# port/3ds/vendor/lus/patches/ — they are NOT applied and NOT listed here (they
# also sit outside PATCH_DIR, so the unknown-patch guard below is not tripped
# by them).
PATCH_LIST=(
    "decomp:decomp-ilp32.patch"
    "decomp:decomp-port-segment-bzero.patch"
    "decomp:decomp-3ds-dma-low-address.patch"
    "decomp:decomp-port-audio-specwait-yield.patch"
    "decomp:decomp-race-cull-diagnostics.patch"
    "decomp:decomp-3ds-sky-overscan-deadend-note.patch"
    "decomp:decomp-3ds-machineselect-gradient-coalesce.patch"
    "decomp:decomp-3ds-rom-audio-port.patch"
    "decomp:decomp-port-course-select-state-reset.patch"
    "decomp:decomp-port-rival-detail.patch"
    "decomp:decomp-native-track-shim.patch"
    "decomp:decomp-port-determinism-pins.patch"
    "decomp:decomp-native-machines-rebuilt.patch"
    "decomp:decomp-port-title-demo-audio-pin.patch"
    "decomp:decomp-native-fx-shim.patch"
)

banner "submodule patches (decomp)"
PATCH_DIR="${ROOT}/port/3ds/patches"
PATCHES_OK=1
APPLIED_NOW=0
ALREADY=0

known_patch() {
    local f="$1" entry
    for entry in "${PATCH_LIST[@]}"; do
        if [ "${entry#*:}" = "$f" ]; then return 0; fi
    done
    return 1
}
for f in "${PATCH_DIR}"/*.patch; do
    base="$(basename "$f")"
    if ! known_patch "$base"; then
        echo "FAIL: ${base} exists in port/3ds/patches/ but is not in this script's PATCH_LIST." >&2
        echo "      Update tools/ci-3ds.sh (and port/3ds/patches/README.md) together." >&2
        PATCHES_OK=0
    fi
done

# Confirm every listed patch is on disk before we reason about apply state.
for entry in "${PATCH_LIST[@]}"; do
    patch="${entry#*:}"
    if [ ! -f "${PATCH_DIR}/${patch}" ]; then
        echo "FAIL: listed patch missing on disk: ${patch}" >&2
        PATCHES_OK=0
    fi
done

# The gate is verified PER SUBMODULE against the FULL ordered stack, not
# per-patch. Order-dependent stacks are the reason: patches later in a series are
# generated against the tree the earlier ones produce, so their hunks are
# addressed AT line numbers that only exist post-application. A per-patch check
# against a fully patched tree then matches neither forward NOR reverse, and the
# old gate false-aborted on a correctly patched checkout. (The worst offenders
# were in the libultraship series, now vendored; the decomp stack keeps the same
# whole-stack treatment because the property is not specific to it.)
#
# Why not a combined `git apply --check`? `--check` with several patch files
# validates every file against the SAME starting tree, so an order-dependent
# stack fails the combined check even when it applies fine. A real `git apply`
# (no --check), by contrast, applies the listed patches SEQUENTIALLY and is
# atomic — on any failure it touches nothing. We exploit exactly that:
#   * combined forward apply succeeds       -> clean tree, now patched. Done.
#   * else combined reverse apply succeeds   -> tree WAS fully applied; we then
#     re-apply forward to restore it (a proven forward<->reverse roundtrip).
#   * else                                   -> real drift, FAIL.
# Sequential real applies are immune to the inter-patch context overlap that
# broke the per-patch check.
#
# SKIP_PATCH_GATE=1 downgrades a drift FAIL to a warning and force-applies any
# individual patch that still applies (escape hatch for a knowingly-dirty
# working tree; leaves the build to catch anything genuinely broken).
SKIP_PATCH_GATE="${SKIP_PATCH_GATE:-0}"

# Distinct submodules in first-seen order (bash 3.2: no associative arrays).
SUBMODULES=()
for entry in "${PATCH_LIST[@]}"; do
    sub="${entry%%:*}"
    seen=0
    for s in "${SUBMODULES[@]:-}"; do
        if [ "$s" = "$sub" ]; then seen=1; break; fi
    done
    if [ "$seen" -eq 0 ]; then SUBMODULES+=("$sub"); fi
done

# Ordered absolute patch paths for one submodule, on stdout (one per line).
patches_for_sub() { # sub
    local want="$1" entry
    for entry in "${PATCH_LIST[@]}"; do
        if [ "${entry%%:*}" = "$want" ]; then
            printf '%s\n' "${PATCH_DIR}/${entry#*:}"
        fi
    done
}

if [ "$PATCHES_OK" -eq 1 ]; then
  for sub in "${SUBMODULES[@]}"; do
    # Forward order (apply order) and reverse order (undo order) arrays.
    fwd=()
    while IFS= read -r line; do fwd+=("$line"); done < <(patches_for_sub "$sub")
    rev=()
    idx=$(( ${#fwd[@]} - 1 ))
    while [ "$idx" -ge 0 ]; do rev+=("${fwd[$idx]}"); idx=$((idx - 1)); done

    # 1. Clean tree? A real sequential forward apply lands the whole stack.
    #    Atomic: if it can't fully apply it changes nothing, and we fall through.
    if git -C "${ROOT}/${sub}" apply "${fwd[@]}" >/dev/null 2>&1; then
        echo "applied:         ${sub} (${#fwd[@]} patches, clean tree)"
        APPLIED_NOW=$((APPLIED_NOW + ${#fwd[@]}))
    # 2. Already fully applied? A real sequential reverse apply reverts it; if
    #    that succeeds we immediately re-apply forward to restore the tree,
    #    proving a clean forward<->reverse roundtrip without leaving it reverted.
    elif git -C "${ROOT}/${sub}" apply --reverse "${rev[@]}" >/dev/null 2>&1; then
        git -C "${ROOT}/${sub}" apply "${fwd[@]}"
        echo "already applied: ${sub} (${#fwd[@]} patches, full-stack roundtrip)"
        ALREADY=$((ALREADY + ${#fwd[@]}))
    elif [ "$SKIP_PATCH_GATE" = "1" ]; then
        echo "WARN: ${sub} stack neither applies nor reverses as a whole." >&2
        echo "      SKIP_PATCH_GATE=1 set — force-applying still-applicable patches." >&2
        for p in "${fwd[@]}"; do
            if git -C "${ROOT}/${sub}" apply --check "$p" >/dev/null 2>&1; then
                git -C "${ROOT}/${sub}" apply "$p"
                echo "applied (forced): ${sub} <- $(basename "$p")"
                APPLIED_NOW=$((APPLIED_NOW + 1))
            else
                echo "skipped (already/blocked): ${sub} <- $(basename "$p")" >&2
            fi
        done
    else
        echo "FAIL: ${sub} patch stack neither applies nor reverses cleanly." >&2
        echo "      The submodule working tree has drifted (partial application or" >&2
        echo "      conflicting local edits). Inspect with:" >&2
        echo "        git -C ${sub} diff" >&2
        echo "        git -C ${sub} apply --check ${PATCH_DIR#${ROOT}/}/<patch>" >&2
        echo "      Or re-run with SKIP_PATCH_GATE=1 to force past a known-dirty tree." >&2
        PATCHES_OK=0
    fi
  done
fi

if [ "$PATCHES_OK" -ne 1 ]; then
    record "submodule patches" FAIL "partial application / unknown patch (see above)"
    summary; exit 1
fi
record "submodule patches" PASS "${ALREADY} already applied, ${APPLIED_NOW} newly applied"

# --------------------------------------------------------------- host tests --
# These are the desktop console tests that matter for the 3DS port (pointer
# packing + DL conversion) plus stream D's fs/zipshim smoke test. They are
# standalone by design, so we compile them directly — a full desktop configure
# (libultraship + SDL) is NOT required on macOS.
banner "host unit tests"
mkdir -p "${HOST_TEST_DIR}"

run_host_test() { # name log-file -> pass/fail via $?
    local name="$1" bin="$2" cwd="$3" log="${HOST_TEST_DIR}/$1.log"
    if (cd "$cwd" && "$bin" > "$log" 2>&1); then
        echo "PASS: ${name}  ($(tail -1 "$log"))"
        record "test: ${name}" PASS "$(tail -1 "$log")"
    else
        echo "FAIL: ${name} — last lines:" >&2
        tail -5 "$log" >&2
        record "test: ${name}" FAIL "see ${log#${ROOT}/}"
    fi
}

# gdx_gfx_pack_tests — real decomp gbi.h under the game's defines; proves a
# 64-bit host pointer survives in Gfx.w1 (mirrors port/CMakeLists.txt:404).
if cc -O1 "${ROOT}/port/tests/gfx_pack_tests.c" \
      -I"${ROOT}/decomp/include" \
      -DPORT=1 -DGDIFFUSER_PORT=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1 \
      -o "${HOST_TEST_DIR}/gfx_pack_tests"; then
    run_host_test "gdx_gfx_pack_tests" "${HOST_TEST_DIR}/gfx_pack_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_pack_tests" FAIL "compile failed"
fi

# gdx_gfx_convert_tests — n64_gfx_convert.cpp compiled unmodified
# (mirrors port/CMakeLists.txt:420).
if c++ -std=c++20 -O1 \
      "${ROOT}/port/tests/gfx_convert_tests.cpp" \
      "${ROOT}/port/n64_gfx_convert.cpp" \
      -I"${ROOT}/port" \
      -o "${HOST_TEST_DIR}/gfx_convert_tests"; then
    run_host_test "gdx_gfx_convert_tests" "${HOST_TEST_DIR}/gfx_convert_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_convert_tests" FAIL "compile failed"
fi

# gdx_track_template_tests — [native] track index-template decoder over the committed raw
# blobs vs the golden table (port/3ds/gfx/gdx3ds_track_templates.c, plain C, no deps).
if cc -O1 -Wall \
      "${ROOT}/port/tests/track_template_tests.c" \
      "${ROOT}/port/3ds/gfx/gdx3ds_track_templates.c" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/track_template_tests"; then
    run_host_test "gdx_track_template_tests" "${HOST_TEST_DIR}/track_template_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_track_template_tests" FAIL "compile failed"
fi

# gdx_interp_host_tests — the owned F3DEX2 interpreter (port/3ds/gfx/n64/) against a mock
# backend + mock resolver seam (docs/research/native/06-interp-plan.md §6.3). n64/ first on
# the include path: port/gdx_interp.h is a different header.
if c++ -std=c++20 -O1 \
      "${ROOT}/port/tests/gdx_interp_host_tests.cpp" \
      "${ROOT}"/port/3ds/gfx/n64/gdx_{dlsrc,dl,interp,cc,tmem,texcache,rsp,tri,rect}.cpp \
      -I"${ROOT}/port/3ds/gfx/n64" -I"${ROOT}/port/3ds/gfx" -I"${ROOT}/port" -I"${ROOT}/port/tests" \
      -o "${HOST_TEST_DIR}/gdx_interp_host_tests"; then
    run_host_test "gdx_interp_host_tests" "${HOST_TEST_DIR}/gdx_interp_host_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_interp_host_tests" FAIL "compile failed"
fi

# gdx_gfx_atlas_uv_tests — the pow2-padding / [trectbatch] atlas-view UV clamp contract
# (RESIDUALS-2): drives gdx3ds_pad_clamp_limit + gdx3ds_uv_prep/gdx3ds_uv_pack, the real
# production derivations, and carries the negative control for "fold uOff into the clamp
# limit" (which would walk an atlas view's sample out of its own cell).
if c++ -std=c++20 -O1 -ffp-contract=off \
      "${ROOT}/port/tests/gfx_atlas_uv_tests.cpp" \
      -I"${ROOT}/port/3ds/gfx" \
      -o "${HOST_TEST_DIR}/gfx_atlas_uv_tests"; then
    run_host_test "gdx_gfx_atlas_uv_tests" "${HOST_TEST_DIR}/gfx_atlas_uv_tests" "${HOST_TEST_DIR}"
else
    record "test: gdx_gfx_atlas_uv_tests" FAIL "compile failed"
fi

# gdx3ds_fs_host_smoke — stream D's SD/O2R/zipshim smoke test; needs a scratch
# CWD because "sdmc:" is an ordinary directory to the host
# (mirrors port/3ds/assets/CMakeLists.txt:45).
if cc -O1 \
      "${ROOT}/port/3ds/assets/test/gdx3ds_fs_host_smoke.c" \
      "${ROOT}/port/3ds/assets/gdx3ds_fs_sd.c" \
      "${ROOT}/port/3ds/assets/zipshim/gdx3ds_zipshim.c" \
      "${ROOT}/port/3ds/assets/third_party/miniz/miniz.c" \
      -I"${ROOT}/port/3ds/include" \
      -I"${ROOT}/port/3ds/lus_stubs" \
      -I"${ROOT}/port/3ds/assets/third_party/miniz" \
      -DMINIZ_NO_TIME \
      -o "${HOST_TEST_DIR}/fs_host_smoke"; then
    rm -rf "${HOST_TEST_DIR}/smoke-cwd"
    mkdir -p "${HOST_TEST_DIR}/smoke-cwd"
    run_host_test "gdx3ds_fs_host_smoke" "${HOST_TEST_DIR}/fs_host_smoke" "${HOST_TEST_DIR}/smoke-cwd"
else
    record "test: gdx3ds_fs_host_smoke" FAIL "compile failed"
fi

# gdx_machines_harness — native machine-mesh differential tests (tools/native/
# machines_harness.sh: A walker vs independent decoder over the 263 body emitters,
# B colour-attribution golden table, C gdx3ds_vtx_math.h vs the interpreter
# transcription). Needs fzerox.o2r ($GDX_O2R or the Azahar SD copy); without it the
# script exits 3 and the step is recorded as PASS "skipped".
MH_LOG="${HOST_TEST_DIR}/gdx_machines_harness.log"
if "${ROOT}/tools/native/machines_harness.sh" --out "${HOST_TEST_DIR}/machines" > "${MH_LOG}" 2>&1; then
    echo "PASS: gdx_machines_harness  ($(tail -1 "${MH_LOG}"))"
    record "test: gdx_machines_harness" PASS "$(tail -1 "${MH_LOG}")"
else
    MH_RC=$?
    if [ "${MH_RC}" -eq 3 ]; then
        echo "PASS: gdx_machines_harness  (skipped: no fzerox.o2r)"
        record "test: gdx_machines_harness" PASS "skipped: no fzerox.o2r (set GDX_O2R)"
    else
        echo "FAIL: gdx_machines_harness — last lines:" >&2
        tail -8 "${MH_LOG}" >&2
        record "test: gdx_machines_harness" FAIL "see ${MH_LOG#${ROOT}/}"
    fi
fi

# ---------------------------------------------------------------- 3DS build --
banner "3DS build (game + harness)"
BUILD_OK=1
mkdir -p "${BUILD_3DS}"

# Generator + compiler launcher are chosen from what is actually on PATH, so the
# same script works on a devkitpro container that has neither (before its
# `apt-get install -y ninja-build ccache` lands) and on a dev box that has both.
# --no-ninja / --no-ccache force the fallbacks.
CMAKE_EXTRA=()
GEN_NAME="Unix Makefiles"
if [ "$USE_NINJA" -eq 1 ] && command -v ninja >/dev/null 2>&1; then
    CMAKE_EXTRA+=(-G Ninja)
    GEN_NAME="Ninja"
fi
CCACHE_NAME="off"
if [ "$USE_CCACHE" -eq 1 ] && command -v ccache >/dev/null 2>&1; then
    CMAKE_EXTRA+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    CCACHE_NAME="ccache $(ccache --version | head -1 | awk '{print $3}')"
fi
# Switching generator in place is not supported by CMake — start clean if the
# cached generator is not the one we want.
if [ -f "${BUILD_3DS}/CMakeCache.txt" ] \
   && ! grep -q "^CMAKE_GENERATOR:INTERNAL=${GEN_NAME}$" "${BUILD_3DS}/CMakeCache.txt"; then
    echo "generator changed -> wiping ${BUILD_3DS#${ROOT}/}"
    rm -rf "${BUILD_3DS}"
    mkdir -p "${BUILD_3DS}"
fi
echo "generator: ${GEN_NAME}   compiler launcher: ${CCACHE_NAME}"

# Sub-second resolution matters now: with Ninja + a warm ccache both configure and
# the incremental build land well under 1 s, which whole-second $SECONDS reports as
# "0s". Prefer bash 5's EPOCHREALTIME, then python3, then fall back to whole seconds.
now_ms() {
    if [ -n "${EPOCHREALTIME:-}" ]; then
        printf '%s' "${EPOCHREALTIME%%[.,]*}${EPOCHREALTIME#*[.,]}" | cut -c1-13
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.time()*1000))'
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}
fmt_ms() { # start-ms -> "1.23s"
    local d=$(( $(now_ms) - $1 ))
    printf '%d.%02ds' $((d / 1000)) $(((d % 1000) / 10))
}

# ccache hit/miss delta across the full build (best-effort: field names differ
# across ccache majors, so we parse the machine-readable dump when available).
# Field names differ across ccache majors (cache_hit_direct vs direct_cache_hit,
# ...), so sum every spelling we know and fall back to 0.
ccache_stat() { # regex over stat names -> summed value, 0 if ccache absent
    command -v ccache >/dev/null 2>&1 || { echo 0; return; }
    ccache --print-stats 2>/dev/null \
        | awk -v re="$1" '$1 ~ re { n += $2 } END { print n + 0 }'
}
CC_HIT0="$(ccache_stat '^(direct_cache_hit|preprocessed_cache_hit|cache_hit_direct|cache_hit_preprocessed)$')"
CC_MISS0="$(ccache_stat '^cache_miss$')"

T0="$(now_ms)"
if ! cmake -S "${ROOT}" -B "${BUILD_3DS}" \
        "${CMAKE_EXTRA[@]:+${CMAKE_EXTRA[@]}}" \
        -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
        -DGDX_PLATFORM_3DS=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE=Release > "${BUILD_3DS}/ci-configure.log" 2>&1; then
    T_CONFIGURE="$(fmt_ms "$T0")"
    tail -20 "${BUILD_3DS}/ci-configure.log" >&2
    record "3ds configure" FAIL "see build-3ds/ci-configure.log"
    BUILD_OK=0
else
    T_CONFIGURE="$(fmt_ms "$T0")"
    record "3ds configure" PASS "${GEN_NAME}, launcher=${CCACHE_NAME}, ${T_CONFIGURE}"
    T0="$(now_ms)"
    if cmake --build "${BUILD_3DS}" -j"${JOBS}" > "${BUILD_3DS}/ci-build.log" 2>&1; then
        T_FULL="$(fmt_ms "$T0")"
        echo "build OK (-j${JOBS}, ${T_FULL})"
    else
        T_FULL="$(fmt_ms "$T0")"
        echo "FAIL: 3DS build — last lines:" >&2
        tail -30 "${BUILD_3DS}/ci-build.log" >&2
        BUILD_OK=0
    fi
    CC_HIT1="$(ccache_stat '^(direct_cache_hit|preprocessed_cache_hit|cache_hit_direct|cache_hit_preprocessed)$')"
    CC_MISS1="$(ccache_stat '^cache_miss$')"

    # Incremental timing: touch the biggest carve TU and rebuild. This also
    # re-proves the stale-object trap is gone — with the sources vendored there
    # is no patch step to desynchronise mtimes any more.
    T_INCR="skipped"
    if [ "$BUILD_OK" -eq 1 ] && [ "$RUN_INCREMENTAL" -eq 1 ] && [ -f "${ROOT}/${INCR_TOUCH}" ]; then
        touch "${ROOT}/${INCR_TOUCH}"
        T0="$(now_ms)"
        if cmake --build "${BUILD_3DS}" -j"${JOBS}" > "${BUILD_3DS}/ci-incr.log" 2>&1; then
            T_INCR="$(fmt_ms "$T0")"
            if ! grep -qiE 'interpreter\.(cpp|o)' "${BUILD_3DS}/ci-incr.log"; then
                echo "WARN: incremental build did not mention ${INCR_TOUCH##*/} — stale-object trap?" >&2
            fi
        else
            tail -20 "${BUILD_3DS}/ci-incr.log" >&2
            record "3ds incremental build" FAIL "see build-3ds/ci-incr.log"
            BUILD_OK=0
        fi
    fi
    echo "[time] configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} gen=${GEN_NAME} launcher=${CCACHE_NAME} jobs=${JOBS}"
    if [ "$CCACHE_NAME" != "off" ]; then
        echo "[ccache] hits=+$((CC_HIT1 - CC_HIT0)) misses=+$((CC_MISS1 - CC_MISS0)) over the full build"
        record "3ds build timing" PASS "configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} ccache +$((CC_HIT1 - CC_HIT0))h/+$((CC_MISS1 - CC_MISS0))m"
    else
        record "3ds build timing" PASS "configure=${T_CONFIGURE} full=${T_FULL} incr=${T_INCR} (no ccache)"
    fi
    GAME_3DSX="${BUILD_3DS}/port/3ds/G-Diffuser-3DS.3dsx"
    HARNESS_3DSX="${BUILD_3DS}/port/3ds/harness/gdx3ds-dl-tests.3dsx"
    if [ "$BUILD_OK" -eq 1 ] && [ -f "$GAME_3DSX" ]; then
        record "build: G-Diffuser-3DS.3dsx" PASS "$(du -h "$GAME_3DSX" | cut -f1 | tr -d ' ')"
    else
        record "build: G-Diffuser-3DS.3dsx" FAIL "missing artifact or build error (build-3ds/ci-build.log)"
        BUILD_OK=0
    fi
    if [ "$BUILD_OK" -eq 1 ] && [ -f "$HARNESS_3DSX" ]; then
        record "build: gdx3ds-dl-tests.3dsx" PASS "$(du -h "$HARNESS_3DSX" | cut -f1 | tr -d ' ')"
    else
        record "build: gdx3ds-dl-tests.3dsx" FAIL "missing artifact or build error (build-3ds/ci-build.log)"
    fi
fi

# ---------------------------------------------------------------- emu smoke --
lock_mtime_age() { # seconds since the lock dir's mtime (BSD stat)
    local m; m="$(stat -f %m "$AZAHAR_LOCK" 2>/dev/null || echo 0)"
    echo $(( $(date +%s) - m ))
}

acquire_azahar_lock() {
    local waited=0
    while ! mkdir "$AZAHAR_LOCK" 2>/dev/null; do
        if [ "$(lock_mtime_age)" -gt "$AZAHAR_LOCK_STALE_SECONDS" ]; then
            echo "emulator lock stale ($(lock_mtime_age)s old) — stealing"
            rmdir "$AZAHAR_LOCK" 2>/dev/null || true
            continue
        fi
        if [ "$waited" -ge "$AZAHAR_LOCK_WAIT_MAX_SECONDS" ]; then
            return 1
        fi
        echo "emulator lock held (${AZAHAR_LOCK}) — waiting ${AZAHAR_LOCK_POLL_SECONDS}s (${waited}/${AZAHAR_LOCK_WAIT_MAX_SECONDS}s)"
        sleep "$AZAHAR_LOCK_POLL_SECONDS"
        waited=$((waited + AZAHAR_LOCK_POLL_SECONDS))
    done
    HOLDING_LOCK=1
    return 0
}

if [ "$RUN_EMU" -eq 1 ]; then
    banner "Azahar boot smoke (${EMU_BOOT_SECONDS}s)"
    EMU_OK=1
    if [ "$BUILD_OK" -ne 1 ]; then
        record "emu boot smoke" FAIL "skipped: 3DS build failed"
        EMU_OK=0
    elif [ ! -x "$AZAHAR_BIN" ]; then
        record "emu boot smoke" FAIL "Azahar not found at ${AZAHAR_BIN} (install from GitHub releases)"
        EMU_OK=0
    elif [ ! -f "${AZAHAR_SDMC}/3ds/gdiffuser/fzerox.o2r" ]; then
        record "emu boot smoke" FAIL "no fzerox.o2r on the virtual SD — run tools/prebake and stage to ${AZAHAR_SDMC}/3ds/gdiffuser/"
        EMU_OK=0
    elif ! grep -q '^log_filter=\*:Debug' "$AZAHAR_CFG" 2>/dev/null; then
        record "emu boot smoke" FAIL "qt-config.ini log_filter is not *:Debug — heartbeat invisible (docs/3DS-HARDWARE.md, Building from source)"
        EMU_OK=0
    fi

    if [ "$EMU_OK" -eq 1 ]; then
        if ! acquire_azahar_lock; then
            record "emu boot smoke" FAIL "could not acquire ${AZAHAR_LOCK} within ${AZAHAR_LOCK_WAIT_MAX_SECONDS}s"
        else
            # Kill any leftover instances; stale processes share the log file and
            # corrupt the evidence (docs/research/m1-boot-debug.md, step 3).
            pkill -9 -x azahar 2>/dev/null || true
            sleep 2
            if pgrep -x azahar >/dev/null 2>&1; then
                record "emu boot smoke" FAIL "stale azahar process would not die (macOS IOKit wedge? see docs/3DS-HARDWARE.md troubleshooting)"
            else
                # Launch through `open -a`, NOT the bundle's inner binary. Running
                # Contents/MacOS/azahar directly starts a process that sits idle at
                # ~0% CPU and never boots the title: no azahar_log.txt output, no
                # sdmc log.txt, no frames — so the heartbeat probe below always
                # reported "not advancing". `open -a` launches it through LaunchServices
                # the way the shared emulator protocol does, and it boots normally.
                open -a "$AZAHAR_APP" --args "$GAME_3DSX"
                echo "azahar launched (open -a ${AZAHAR_APP}); booting for ${EMU_BOOT_SECONDS}s..."
                sleep "$EMU_BOOT_SECONDS"
                # SIGTERM first (graceful exit flushes the log), then -9.
                pkill -x azahar 2>/dev/null || true
                sleep 3
                pkill -9 -x azahar 2>/dev/null || true
                sleep 1

                FRAMES="$(grep -a 'OutputDebugString' "$AZAHAR_LOG" 2>/dev/null \
                          | grep -aEo 'frame [0-9]+' | awk '{print $2}' || true)"
                DISTINCT="$(printf '%s\n' "$FRAMES" | sort -nu | grep -c . || true)"
                MAXFRAME="$(printf '%s\n' "$FRAMES" | sort -n | tail -1)"
                FATALS="$(grep -ac '\[fatal\]' "$AZAHAR_LOG" 2>/dev/null || true)"
                echo "heartbeat: ${DISTINCT} distinct frames, max=${MAXFRAME:-none}; [fatal] lines: ${FATALS:-0}"
                if [ -z "$MAXFRAME" ] || [ "$DISTINCT" -lt 2 ]; then
                    record "emu boot smoke" FAIL "heartbeat not advancing (distinct=${DISTINCT}, max=${MAXFRAME:-none}) — see ${AZAHAR_LOG}"
                elif [ "${FATALS:-0}" -ne 0 ]; then
                    record "emu boot smoke" FAIL "${FATALS} [fatal] line(s) in the Azahar log"
                else
                    record "emu boot smoke" PASS "heartbeat reached frame ${MAXFRAME}, 0 [fatal] lines"
                fi
            fi
            release_lock
        fi
    fi
fi

summary
exit "$FAILED"
