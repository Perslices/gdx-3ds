#!/usr/bin/env bash
# verifymatrix.sh — run the native fork's verify-mode matrix over the canonical replay.
#
# Why: `[native] interp_verify=1` (both interpreters over every job, packed batches compared)
# had never been run together with the Tier-2 verify levers (`track=2`, `machines_verify=1`,
# `fx_sink=2`), nor with `all=1`. This script is that matrix, so any branch can re-run it.
#
#   tools/dev/verifymatrix.sh                      # every cell, in order
#   tools/dev/verifymatrix.sh a b c                # only those cells
#   tools/dev/verifymatrix.sh --list
#   tools/dev/verifymatrix.sh --summary            # re-print the table from existing run dirs
#
# Cells (see docs/research/native/verifymatrix-progress.md):
#   base       all levers off                       — the [fh] reference
#   interp     interp=1                             — non-verify reference for (a)
#   interpall  interp=1 all=1                       — non-verify reference for (b)/(c)
#   a          interp=1 interp_verify=1
#   b          interp=1 interp_verify=1 all=1
#   c          interp=1 interp_verify=1 track=2 machines_verify=1 fx_sink=2   (all four)
#   dtrack     interp=1 track=2
#   dmach      interp=1 machines=1 machines_verify=1
#   dfx        interp=1 fx_sink=2 fx_sprites=1
#   e          interp=1 cpuclip=0                   — a real behaviour switch, divergence expected
#   fwalk      interp=1 machines=1 machines_walk_only=1
#   fmask      interp=1 machines=1 machines_mask=55555555   (odd slots only)
#   flod       interp=1 machines=1 machines_lod_mask=01     (LOD1 only)
#
# Every [native] and every run-control [debug] key is passed EXPLICITLY on every run:
# gdx-dev merges --ini into the instance ini and keys persist across runs
# (integration-verify-2026-09-17 finding 6).
#
# quit_at_tick is pinned to 0 on every run. The shared Azahar SD's ini is another agent's
# working file: it arrived tonight carrying `quit_at_tick = 1200`, which silently ended a Qt
# run at frame 1167 and still scored PASS, because a deliberate quit counts as complete.
#
# trectbatch is pinned to 0 on every run. Under the texrect atlas the two verify passes see
# different atlas cell placement and [nverify] reports UV/state diffs that are an artefact of
# verify mode, not of the interpreter (interp-progress.md, "Gotchas").
set -uo pipefail

# VM_ROOT lets a copy of this script (e.g. one kept out of the way while the repo copy is
# edited — bash re-reads a running script from disk) still find the worktree it tests.
ROOT="${VM_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
DEV="$ROOT/tools/dev/gdx-dev"
THREEDSX="$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.3dsx"
REPLAY="race-mutecity-start"
# Software, for the whole matrix, on purpose. [fh] parity is same-lane AND same-renderer
# (interphooks-progress.md: a renderer change diverges both paths identically from frame 1702),
# so a cell that silently fell back to another renderer could not be compared with the rest --
# and Vulkan host-crashes this core at guest frame ~39 whenever the machine is loaded, which it
# is. Pass --gfx to override.
GFX="software"
# LANE: "retro" (no singleton, N agents at once) or "azahar" (the Qt singleton behind
# /tmp/azahar.lock). The Qt lane is where dense `fh_every=1` capture over 6002 frames is
# validated, and it is the one that works when the retro lane is full of other agents'
# instances -- which is when azahar-libretro host-crashes at the first heavy frame.
LANE="retro"
# CAPTURE DENSITY. Corrected 2026-09-23 (menus4145): the paragraph that used to sit here said
# the retro lane ran "with SHOT-level capture only" and that DENSE=1 was how a cell got all
# 6002 frame hashes. That was wrong in the direction that hides bugs. `[debug] fh_start`
# defaults to 0 (port/3ds/gdx3ds_oracle.c:30,322), and this script has always passed
# `debug.fh_every=1`, so EVERY cell here has always captured densely; `--dense` only re-stated
# the default and could not change a thing. Density is therefore pinned explicitly on every
# run now, so it is never inherited from a seeded ini (the Qt lane's shared SD keeps keys
# across runs -- integration-verify-2026-09-17 finding 6) and never has to be inferred from a
# comment. FH_START=0 is dense; a positive value suppresses the boot window, which is the only
# thing the old azahar-libretro "dense kills the core at frame ~75" probe was ever avoiding.
#
# Why this matters beyond bookkeeping: SHOT-level sampling is what let `menus-longtail` frame
# 4145 hide for weeks behind a `compared=24 divergent=0` claim (menus4145-progress.md).
FH_START=0
ATTEMPTS=3      # outer retries per cell (the core dies at boot under load)
SECONDS_VERIFY=3600
SECONDS_PLAIN=1800
TRACKNAME="verifymatrix"
RUNROOT="/tmp/gdx-runs"
ONLY_SUMMARY=0

# Order is priority order: the B7 question (a, c and their references) first, then the
# single-lever cells, then the bisect levers, then the corpus. The lane is shared and a run
# can be lost to another agent's instances, so the cells that answer the brief run first.
CELLS=(base interp a c interpall b vtrack vmach vfx cnohud dhud cfixed dtrack dmach dfx e
       fwalk fmask flod cmenus cgp2 ctunnel c4p cexplo)

# cell -> the [native] keys that differ from the all-zero default.
cell_native() {
    case "$1" in
    base)      echo "" ;;
    interp)    echo "interp=1" ;;
    interpall) echo "interp=1 all=1" ;;
    a)         echo "interp=1 interp_verify=1" ;;
    b)         echo "interp=1 interp_verify=1 all=1" ;;
    c | cfixed | cmenus | cgp2 | ctunnel | c4p | cexplo)
               echo "interp=1 interp_verify=1 track=2 trkcensus=1 machines=1 machines_verify=1 fx_sink=2 fx_sprites=1 fx_hud=1 fxcensus=1" ;;
    dhud)      echo "interp=1 interp_verify=1 fx_hud=1" ;;
    vtrack)    echo "interp=1 interp_verify=1 track=2 trkcensus=1" ;;
    vmach)     echo "interp=1 interp_verify=1 machines=1 machines_verify=1" ;;
    vfx)       echo "interp=1 interp_verify=1 fx_sink=2 fx_sprites=1 fxcensus=1" ;;
    cnohud)    echo "interp=1 interp_verify=1 track=2 trkcensus=1 machines=1 machines_verify=1 fx_sink=2 fx_sprites=1 fxcensus=1" ;;
    dtrack)    echo "interp=1 track=2 trkcensus=1" ;;
    dmach)     echo "interp=1 machines=1 machines_verify=1" ;;
    dfx)       echo "interp=1 fx_sink=2 fx_sprites=1 fxcensus=1" ;;
    e)         echo "interp=1 cpuclip=0" ;;
    fwalk)     echo "interp=1 machines=1 machines_walk_only=1" ;;
    fmask)     echo "interp=1 machines=1 machines_mask=55555555" ;;
    flod)      echo "interp=1 machines=1 machines_lod_mask=01" ;;
    *)         echo "__UNKNOWN__" ;;
    esac
}

# The corpus (native/replays, merged): the four verify modes are run on every replay, not
# only the canonical one. The machfix result is the reason — the 4P material bug was invisible
# to a frame hash unless the scene also recycled a texture id, and machines_verify had never
# run outside the two scenes it was built for.
cell_replay() {
    case "$1" in
    cmenus)  echo "menus-longtail" ;;
    cgp2)    echo "race-gp2-sector" ;;
    ctunnel) echo "tunnel-silence-records" ;;
    c4p)     echo "race-4p-vs" ;;
    cexplo)  echo "explosion-pileup" ;;
    cnohud)  echo "$REPLAY" ;;
    cfixed)  echo "$REPLAY" ;;
    *)       echo "$REPLAY" ;;
    esac
}

# race-4p-vs needs all four controller ports reported.
cell_extra_ini() {
    case "$1" in
    c4p) echo "debug.pads=4" ;;
    *)   echo "" ;;
    esac
}

cell_seconds() {
    case "$1" in
    a | b | c | c[a-z]* | dhud | v*) echo "$SECONDS_VERIFY" ;;
    *)         echo "$SECONDS_PLAIN" ;;
    esac
}

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# Full explicit key set. Defaults first, then the cell's overrides win (inimerge keeps the last).
build_ini_args() {
    local cell="$1" overrides k v
    local -a args=()
    local defaults=(
        "native.all=0" "native.interp=0" "native.interp_verify=0" "native.cpuclip=1"
        "native.blendflush=1" "native.machines=0" "native.machines_verify=0"
        "native.machines_walk_only=0" "native.machines_mask=ffffffff"
        "native.machines_lod_mask=1f" "native.track=0" "native.trkcensus=0"
        "native.fx_sink=0" "native.fx_sprites=0" "native.fx_hud=0" "native.fxcensus=0"
        "debug.det=1" "debug.fh=1" "debug.fh_every=1" "debug.quit_at_tick=0"
        "debug.renderthread=0" "debug.replay_quit=1" "debug.gputrace=0"
        "debug.platform=azahar" "debug.trectbatch=0" "debug.tmemfast=1" "debug.trifast=1"
        "stereo.enabled=1" "net.enabled=0"
    )
    defaults+=("debug.fh_start=$FH_START")  # pinned, never inherited (see FH_START above)
    overrides="$(cell_native "$cell")"
    # A cell override replaces the default entry for the same key.
    local -a keep=()
    local d dk okeys=""
    for k in $overrides; do okeys="$okeys ${k%%=*}"; done
    for d in "${defaults[@]}"; do
        dk="${d#native.}"; dk="${dk%%=*}"
        case "${d}" in
        native.*)
            if [[ " $okeys " == *" $dk "* ]]; then continue; fi ;;
        esac
        keep+=("$d")
    done
    for d in "${keep[@]}"; do args+=(--ini "$d"); done
    for k in $overrides; do args+=(--ini "native.$k"); done
    for k in $(cell_extra_ini "$cell"); do args+=(--ini "$k"); done
    printf '%s\n' "${args[@]}"
}

# The azahar-libretro core host-crashes at guest frame ~40 when the machine is loaded, on every
# renderer and independently of the build (interp-progress.md note 5, interphooks gotchas). It is
# a lane fault, not a result, so a cell is retried; the last attempt pins Software, which is the
# renderer that survived a loaded host in the interphooks gate.
run_cell() {
    local cell="$1" secs; secs="$(cell_seconds "$cell")"
    local -a ini=()
    while IFS= read -r line; do ini+=("$line"); done < <(build_ini_args "$cell")
    echo "=== cell $cell : [native] $(cell_native "$cell") ==="
    local att rc=1 t0 t1 gfx
    for att in $(seq 1 "$ATTEMPTS"); do
        gfx="$GFX"
        echo "    attempt $att/$ATTEMPTS lane=$LANE gfx=${gfx:-auto}  uptime: $(uptime)"
        t0=$(date +%s)
        if [ "$LANE" = "azahar" ]; then
            "$DEV" --track "$TRACKNAME" --run-root "$RUNROOT" emu run "$THREEDSX" --lane azahar \
                --name "vm-$cell" --replay "$(cell_replay "$cell")" --shots --seconds "$secs" "${ini[@]}"
        else
            local -a gfxarg=()
            [ -n "$gfx" ] && gfxarg=(--gfx "$gfx")
            "$DEV" --track "$TRACKNAME" --run-root "$RUNROOT" retro run "$THREEDSX" \
                --name "vm-$cell" --replay "$(cell_replay "$cell")" "${gfxarg[@]}" --seconds "$secs" "${ini[@]}"
        fi
        rc=$?
        t1=$(date +%s)
        echo "    cell $cell attempt $att rc=$rc wall_s=$((t1 - t0))"
        [ "$rc" = "0" ] && break
    done
    echo "    cell $cell rc=$rc wall_s=$((t1 - t0))"
    return $rc
}

# --- receipt extraction -------------------------------------------------------------------
cell_rundir() {
    ls -1d "$RUNROOT/$TRACKNAME"/*-"vm-$1" 2>/dev/null | tail -1
}

report_cell() {
    local cell="$1" dir log
    dir="$(cell_rundir "$cell")"
    if [ -z "$dir" ]; then echo "$cell: NO RUN"; return; fi
    # retro lane writes log-<runid>.txt, the Qt lane writes log.txt
    log="$(ls -1 "$dir"/log-*.txt "$dir"/log.txt 2>/dev/null | tail -1)"
    if [ -z "$log" ]; then echo "    NO GUEST LOG"; return; fi
    echo "--- $cell  ($(basename "$dir"))"
    [ -f "$dir/RESULT" ] && sed 's/^/    /' "$dir/RESULT"
    grep -h "^\[native\] all=\|\[native\] interp=" "$log" 2>/dev/null | head -2 | sed 's/^/    /'
    # interp_verify
    awk '/\[nverify\] tasks=/{t=$0} END{if(t!="")print "    nverify: " t}' "$log" 2>/dev/null
    python3 "$ROOT/tools/dev/verifymatrix_report.py" "$log"
}

# --- main ---------------------------------------------------------------------------------
declare -a WANT=()
while [ $# -gt 0 ]; do
    case "$1" in
    --list) printf '%s\n' "${CELLS[@]}"; exit 0 ;;
    --summary) ONLY_SUMMARY=1 ;;
    --gfx) GFX="$2"; shift ;;
    --lane) LANE="$2"; shift ;;
    --dense) FH_START=0 ;;                 # kept: it is the default, and says so
    --fh-start) FH_START="$2"; shift ;;    # >0 suppresses the boot window; 0 = dense
    --3dsx) THREEDSX="$2"; shift ;;
    --replay) REPLAY="$2"; shift ;;
    --run-root) RUNROOT="$2"; shift ;;
    --track) TRACKNAME="$2"; shift ;;
    -h | --help) usage; exit 0 ;;
    -*) echo "unknown flag $1" >&2; exit 2 ;;
    *) WANT+=("$1") ;;
    esac
    shift
done
[ ${#WANT[@]} -eq 0 ] && WANT=("${CELLS[@]}")

if [ "$ONLY_SUMMARY" = "1" ]; then
    for c in "${WANT[@]}"; do report_cell "$c"; done
    exit 0
fi

fails=0
for c in "${WANT[@]}"; do
    [ "$(cell_native "$c")" = "__UNKNOWN__" ] && { echo "unknown cell $c" >&2; exit 2; }
    run_cell "$c" || fails=$((fails + 1))
done

echo
echo "======== MATRIX SUMMARY ========"
for c in "${WANT[@]}"; do report_cell "$c"; done
echo
echo "[fh] parity (fhdiff, L eye), verify cell vs its non-verify reference:"
pair() {
    local A B da db
    da="$(cell_rundir "$1")"; db="$(cell_rundir "$2")"
    [ -z "$da" ] || [ -z "$db" ] && return
    printf '  %-22s ' "$1 vs $2"
    python3 "$ROOT/tools/dev/fhdiff.py" "$(ls -1 "$da"/log-*.txt "$da"/log.txt 2>/dev/null|tail -1)" \
        "$(ls -1 "$db"/log-*.txt "$db"/log.txt 2>/dev/null|tail -1)" --quiet
}
pair base interp
pair interp a
pair interpall b
pair interpall c
pair interp dtrack
pair interp dmach
pair interp dfx
pair interp e
pair interp fwalk
pair interp fmask
pair interp flod
echo "cells failed: $fails"
exit $((fails > 0))
