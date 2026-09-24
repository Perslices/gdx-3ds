#!/usr/bin/env bash
# covdrift.sh — the SHOT schedules' drift check (NEXT.md B48).
#
# THE PROBLEM. Every corpus schedule in replays/*.gdxrec is DERIVED: a greedy set cover of the
# features a `[debug] cov=1` census saw that replay draw, plus the mode-anchor rules and a
# --max-gap floor (coverage2-progress.md). A derived schedule is a claim about the replay
# CONTENT it was derived from. Change the content — re-record a replay, extend one, land a new
# feature family, change what a screen draws — and the schedule silently stops covering it,
# with no symptom at all: the gate keeps printing `divergent=0` over a shrinking sample.
# The hand-placed schedules this replaced covered 49-80% and printed `divergent=0` for months.
#
#   tools/dev/covdrift.sh                     # census + assert every corpus replay
#   tools/dev/covdrift.sh race-mutecity-start # only those replays
#   tools/dev/covdrift.sh --list
#   tools/dev/covdrift.sh --summary           # re-assert from existing run dirs, no guest runs
#   tools/dev/covdrift.sh --rederive          # on failure, print the derive command to fix it
#
# WHY THIS IS NOT IN tools/ci-3ds.sh. That is the fast host gate: ~5 s of build plus host tests,
# no emulator, no ROM. This check needs one guest run per replay (~60-100 s each on the retro
# lane, ~10 min for the corpus), a RetroArch install, a libretro core, and fzerox.o2r — an
# asset that must never be committed (PROTOCOL.md). Bolting it on would turn a 5 s gate into a
# 10 minute one, make every host-only change wait for an emulator, and make the gate
# unrunnable anywhere the ROM is absent. It is a per-release check, and it lives beside
# tools/dev/verifymatrix.sh, which is the same shape for the same reason.
#
# ci-3ds.sh does carry the cheap HALF of this: `schedule invariants`, a static check over
# replays/ that costs milliseconds and needs nothing. It catches the failure B47 actually was
# — a .gdxs claiming its placement is derived while the .gdxrec still held hand-placed shots —
# and every "someone hand-edited a SHOT back in" regression. What it cannot do is compare a
# schedule against CONTENT, because content only exists when the game runs. That is this file.
#
# TWO ASSERTIONS, NOT ONE. Feature coverage must be 100%, and the worst unsampled gap must
# stay within the floor the schedules were derived at. The second is not redundant: extend a
# replay by 1000 ticks into a screen it already visits and coverage stays at 100% while the
# sampling rate halves. `tunnel-silence-records` is the live example — its feature vocabulary
# saturates at frame 1851 and it then runs 2800 more frames of the same vocabulary.
#
# PIN THE ARTIFACT. Every run here uses ONE copy of the .3dsx, staged once into the run root
# and sha256-checked on every run. coverage2 had to throw away a whole gate because a CI
# rebuild changed the baked build id mid-run and the later cells were a different binary.
#
# Every [native] and run-control [debug] key is passed EXPLICITLY on every run, zeros included:
# gdx-dev --ini writes through to a persistent per-instance ini, so a lever omitted is a lever
# inherited (explosions-progress.md).
set -uo pipefail

ROOT="${CD_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
DEV="$ROOT/tools/dev/gdx-dev"
SCHED="$ROOT/tools/dev/covsched.py"
THREEDSX="$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.3dsx"
# Software on purpose, as everywhere else in this fork: the retro lane falls back
# Vulkan->OpenGL->Software on a boot crash, and a census that fell back would be a census of a
# different renderer's draw stream. PROTOCOL.md, "Pin the renderer for any comparison".
GFX="software"
SECONDS_RUN=600
ATTEMPTS=3          # the azahar-libretro core host-crashes at boot under load; that is a lane
                    # fault, not a result, so a replay is retried before it is called FAIL.
REQUIRE=100         # the coverage floor, in percent
MAXGAP=300          # must match the --max-gap the schedules were derived at
TRACKNAME="covdrift"
RUNROOT="/tmp/gdx-runs"
ONLY_SUMMARY=0
REDERIVE=0

# The corpus. Seven replays: the six coverage2 derived plus explosion-pileup (B47).
REPLAYS=(race-mutecity-start menus-longtail race-gp2-sector tunnel-silence-records
         race-4p-vs race-practice-mc-silence explosion-pileup)

# race-4p-vs walks a different menu entirely with the default pads=1: the 3DS has one pad and
# the decomp gates VS BATTLE on the port count, so the cursor never lands on it (3DS-HARDWARE.md).
replay_extra_ini() {
    case "$1" in
    race-4p-vs) echo "debug.pads=4" ;;
    *)          echo "" ;;
    esac
}

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# --- the pinned artifact ------------------------------------------------------------------
PINDIR="$RUNROOT/$TRACKNAME/pin"
PINNED=""
PINSHA=""

pin_artifact() {
    if [ ! -f "$THREEDSX" ]; then
        echo "covdrift: no .3dsx at $THREEDSX — build first (tools/ci-3ds.sh --jobs 8)" >&2
        return 1
    fi
    mkdir -p "$PINDIR" || return 1
    PINNED="$PINDIR/G-Diffuser-3DS.3dsx"
    cp "$THREEDSX" "$PINNED" || return 1
    PINSHA="$(shasum -a 256 "$PINNED" | awk '{print $1}')"
    echo "pinned artifact $PINNED"
    echo "        sha256 $PINSHA"
    echo "        (every run below is this one binary; a rebuild mid-run cannot change it)"
}

# --- one replay ---------------------------------------------------------------------------
run_census() {
    local rep="$1" att rc=1 t0 t1
    local -a ini=(
        --ini debug.det=1 --ini debug.cov=1 --ini debug.fh=1 --ini debug.fh_every=0
        --ini debug.fh_start=0 --ini debug.replay_quit=1 --ini debug.quit_at_tick=0
        --ini debug.verbose=0 --ini debug.gputrace=0 --ini debug.filelog_max_kb=262144
        --ini debug.renderthread=1 --ini debug.bridgemain=1
        --ini debug.trectbatch=0 --ini debug.tmemfast=1 --ini debug.trifast=1
        # all=1 interp=1 is the widest draw stream the port produces, so the census sees the
        # most features; a schedule derived from a narrower run would under-cover the gate's.
        --ini native.all=1 --ini native.interp=1 --ini native.interp_verify=0
        --ini native.fxcensus=0 --ini native.fxceiling=0 --ini native.trkcensus=0
        --ini stereo.enabled=1 --ini net.enabled=0
    )
    local k
    for k in $(replay_extra_ini "$rep"); do ini+=(--ini "$k"); done
    echo "=== census $rep ==="
    for att in $(seq 1 "$ATTEMPTS"); do
        echo "    attempt $att/$ATTEMPTS gfx=$GFX  uptime: $(uptime)"
        t0=$(date +%s)
        # --track is passed to BOTH the driver and `retro run`, on purpose. `retro run`
        # declares its own --track into the same argparse namespace with default None, so the
        # DRIVER-level --track is silently overwritten and the run lands in the track named
        # after the worktree instead. Caught here when every run reported PASS and then
        # `NO RUN`; tools/dev/verifymatrix.sh has the same shape and the same silent loss.
        "$DEV" --track "$TRACKNAME" --run-root "$RUNROOT" retro run "$PINNED" \
            --track "$TRACKNAME" --run-root "$RUNROOT" \
            --name "cd-$rep" --replay "$rep" --gfx "$GFX" --seconds "$SECONDS_RUN" "${ini[@]}"
        rc=$?
        t1=$(date +%s)
        echo "    $rep attempt $att rc=$rc wall_s=$((t1 - t0))"
        [ "$rc" = "0" ] && break
    done
    return $rc
}

replay_rundir() { ls -1d "$RUNROOT/$TRACKNAME"/*-"cd-$1" 2>/dev/null | tail -1; }

# The assertion. Returns 0 clean, 1 drifted, 2 could not be checked at all — three outcomes,
# because "the run died" and "the schedule has drifted" are different facts and collapsing
# them into one FAIL is how a lane fault gets committed as a content change.
check_replay() {
    local rep="$1" dir log rec sha rc
    dir="$(replay_rundir "$rep")"
    if [ -z "$dir" ]; then echo "--- $rep: NO RUN"; return 2; fi
    log="$(ls -1 "$dir"/log-*.txt "$dir"/log.txt 2>/dev/null | tail -1)"
    if [ -z "$log" ]; then echo "--- $rep: NO GUEST LOG ($dir)"; return 2; fi
    rec="$ROOT/replays/$rep.gdxrec"
    if [ ! -f "$rec" ]; then echo "--- $rep: NO RECORDING at $rec"; return 2; fi
    echo "--- $rep  ($(basename "$dir"))"
    # The artifact the census actually ran, from the run's own receipt — not from what this
    # script believes it staged.
    sha="$(sed -n 's/.*sha256=\([0-9a-f]*\).*/\1/p' "$dir/RESULT" 2>/dev/null | tail -1)"
    [ -n "$sha" ] && echo "    artifact $sha"
    if [ -n "$PINSHA" ] && [ -n "$sha" ] && [ "${PINSHA:0:${#sha}}" != "$sha" ]; then
        echo "    ARTIFACT MISMATCH: run used $sha, this check pinned ${PINSHA:0:16}"
        return 2
    fi
    # A truncated census is not a passing census: it would report the features of the frames it
    # reached and call the rest covered by omission.
    if ! grep -q "replay complete  *True" "$dir/RESULT" 2>/dev/null &&
       ! grep -q "complete=True" "$dir/RESULT" 2>/dev/null; then
        echo "    INCOMPLETE REPLAY — census covers only part of the run, not asserting"
        return 2
    fi
    if ! grep -q "^\[cov\]" "$log"; then
        echo "    NO [cov] LINES — was debug.cov=1 applied? not asserting"
        return 2
    fi
    python3 "$SCHED" cover "$log" --gdxrec "$rec" \
        --require "$REQUIRE" --max-gap "$MAXGAP" | sed 's/^/    /'
    rc=${PIPESTATUS[0]}
    if [ "$rc" != "0" ] && [ "$REDERIVE" = "1" ]; then
        echo "    to fix, re-derive against this census and re-gate the replay:"
        echo "      python3 tools/dev/covsched.py derive $log \\"
        echo "          --rewrite replays/$rep.gdxrec /tmp/$rep.gdxrec && \\"
        echo "          mv /tmp/$rep.gdxrec replays/$rep.gdxrec"
    fi
    return $rc
}

# --- main ----------------------------------------------------------------------------------
declare -a WANT=()
while [ $# -gt 0 ]; do
    case "$1" in
    --list) printf '%s\n' "${REPLAYS[@]}"; exit 0 ;;
    --summary) ONLY_SUMMARY=1 ;;
    --rederive) REDERIVE=1 ;;
    --gfx) GFX="$2"; shift ;;
    --3dsx) THREEDSX="$2"; shift ;;
    --require) REQUIRE="$2"; shift ;;
    --max-gap) MAXGAP="$2"; shift ;;
    --seconds) SECONDS_RUN="$2"; shift ;;
    --attempts) ATTEMPTS="$2"; shift ;;
    --run-root) RUNROOT="$2"; shift ;;
    --track) TRACKNAME="$2"; shift ;;
    -h | --help) usage; exit 0 ;;
    -*) echo "unknown flag $1" >&2; exit 2 ;;
    *) WANT+=("$1") ;;
    esac
    shift
done
[ ${#WANT[@]} -eq 0 ] && WANT=("${REPLAYS[@]}")
PINDIR="$RUNROOT/$TRACKNAME/pin"

for r in "${WANT[@]}"; do
    case " ${REPLAYS[*]} " in *" $r "*) ;; *) echo "unknown replay $r" >&2; exit 2 ;; esac
done

if [ "$ONLY_SUMMARY" != "1" ]; then
    pin_artifact || exit 2
    for r in "${WANT[@]}"; do run_census "$r"; done
else
    # --summary asserts against run dirs that already exist. Read the sha back from the pinned
    # copy if it is still there, so the artifact check still applies.
    PINNED="$PINDIR/G-Diffuser-3DS.3dsx"
    [ -f "$PINNED" ] && PINSHA="$(shasum -a 256 "$PINNED" | awk '{print $1}')"
fi

echo
echo "======== SCHEDULE DRIFT: coverage >= ${REQUIRE}%, worst gap <= ${MAXGAP} frames ========"
drift=0
unchecked=0
declare -a DRIFTED=() UNCHECKED=()
for r in "${WANT[@]}"; do
    check_replay "$r"
    case $? in
    0) ;;
    1) drift=$((drift + 1)); DRIFTED+=("$r") ;;
    *) unchecked=$((unchecked + 1)); UNCHECKED+=("$r") ;;
    esac
done

echo
if [ "$drift" != "0" ]; then
    echo "DRIFTED (schedule no longer covers its replay): ${DRIFTED[*]}"
    echo "  Each replay above names its unsampled features. Re-derive it"
    echo "  (covsched.py derive --rewrite, --rederive prints the command) and re-run the"
    echo "  parity gate for that replay: a new schedule is a new set of hashes."
fi
[ "$unchecked" != "0" ] && echo "NOT CHECKED (lane fault or missing input, NOT a content verdict): ${UNCHECKED[*]}"
if [ "$drift" = "0" ] && [ "$unchecked" = "0" ]; then
    echo "RESULT: PASS — ${#WANT[@]} replay(s), every committed schedule still covers its replay"
    exit 0
fi
echo "RESULT: FAIL — drifted=$drift unchecked=$unchecked"
exit 1
