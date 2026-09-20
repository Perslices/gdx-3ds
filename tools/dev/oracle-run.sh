#!/usr/bin/env bash
# oracle-run.sh — one Azahar run under the oracle, lock-protocol compliant, artifacts snapshotted.
#
#   tools/dev/oracle-run.sh --name <run> [--3dsx <abs path>] [--replay <rec>] [--record <name>]
#                           [--every N] [--det 0|1] [--autoinput <file>] [--timeout <s>]
#                           [--set section.key=value ...] [--no-quit]
#
# What it does (PROTOCOL.md): waits for /tmp/azahar.lock (mkdir), keeps it touched, MERGES the
# oracle keys into the shared gdiffuser.ini (backed up and restored afterwards, so no key leaks
# into another agent's run), launches Azahar on the .3dsx, waits for "[rec] end" / "[oracle]
# quit" / the timeout while snapshotting sdmc artifacts every 25 s to $OUT (default
# /tmp/oracle-art/<run>/), quits Azahar (osascript, then TERM after 30 s — it is OUR instance),
# restores the ini + gdx-autoinput.txt, releases the lock. Outputs in $OUT: log.txt (sdmc
# filelog: every [fh]/[rec]/[det] receipt), azahar_log.txt, autotest/*_L.bmp, rec/*.gdxrec.
#
# ORACLE_LOCK_OWNED=1: the caller already holds /tmp/azahar.lock (and touches it); this run
# neither acquires nor releases it — back-to-back runs without a gap another agent could take.
#
# Parity check = two runs with the same --replay and --det 1, then:
#   tools/dev/fhdiff.py /tmp/oracle-art/A/log.txt /tmp/oracle-art/B/log.txt
set -euo pipefail

NAME=""; DSX=""; REPLAY=""; RECORD=""; EVERY=0; DET=1; AUTOINPUT=""; TIMEOUT=1500; NOQUIT=0; QUITTICK=0
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2;;
        --3dsx) DSX="$2"; shift 2;;
        --replay) REPLAY="$2"; shift 2;;
        --record) RECORD="$2"; shift 2;;
        --every) EVERY="$2"; shift 2;;
        --det) DET="$2"; shift 2;;
        --autoinput) AUTOINPUT="$2"; shift 2;;
        --timeout) TIMEOUT="$2"; shift 2;;
        --quit-tick) QUITTICK="$2"; shift 2;;
        --set) EXTRA+=("--set" "$2"); shift 2;;
        --no-quit) NOQUIT=1; shift;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done
[ -n "$NAME" ] || { echo "--name required" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ -n "$DSX" ] || DSX="$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.3dsx"
[ -f "$DSX" ] || { echo "no .3dsx at $DSX" >&2; exit 2; }
SD="$HOME/Library/Application Support/Azahar/sdmc"
INI="$SD/3ds/gdiffuser/gdiffuser.ini"
GLOG="$SD/3ds/gdiffuser/log.txt"   # the [debug] filelog sink (GDX3DS_FILELOG_PATH); the game rotates it to log-prev.txt
AZLOG="$HOME/Library/Application Support/Azahar/log/azahar_log.txt"
OUT="${OUT:-/tmp/oracle-art/$NAME}"
LOCK=/tmp/azahar.lock
HAVE_LOCK=0   # cleanup releases the lock ONLY if this script acquired it (never another agent's)
LAUNCHED=0    # cleanup quits Azahar ONLY if this script launched it
mkdir -p "$OUT"
STAMP="$(date +%s)"
BK="$OUT/ini.backup"
AIBK="$OUT/gdx-autoinput.backup"

log() { echo "[oracle-run $(date +%H:%M:%S)] $*"; }

# 1. lock
waited=0
if [ "${ORACLE_LOCK_OWNED:-0}" = "1" ] && [ -d "$LOCK" ]; then
    log "lock owned by the caller (ORACLE_LOCK_OWNED=1)"
    HAVE_LOCK=2
    TOUCHER=""
fi
until [ "$HAVE_LOCK" -ne 0 ] || mkdir "$LOCK" 2>/dev/null; do
    if [ "$waited" -eq 0 ]; then log "waiting for $LOCK (held by another agent)"; fi
    sleep 5; waited=$((waited + 5))
    if [ "$waited" -gt 7200 ]; then log "gave up waiting for the lock"; exit 3; fi
done
if [ "$HAVE_LOCK" -eq 0 ]; then
    HAVE_LOCK=1
    log "lock acquired"
    ( while true; do touch "$LOCK" 2>/dev/null || exit 0; sleep 20; done ) &
    TOUCHER=$!
fi

cleanup() {
    set +e
    if [ "$HAVE_LOCK" -eq 0 ]; then log "exiting without the lock: nothing to clean"; return; fi
    if [ "$NOQUIT" -eq 0 ] && [ "$LAUNCHED" -eq 1 ]; then
        if pgrep -x Azahar >/dev/null 2>&1 || pgrep -f Azahar.app >/dev/null 2>&1; then
            # The game normally exits itself (quit_at_tick / replay_quit) and Azahar then sits on its
            # main window: a plain quit works. While emulation is still running Azahar puts up a
            # close-confirmation dialog and ignores the quit, so escalate: TERM, then (once) KILL.
            log "quitting Azahar"
            osascript -e 'tell application "Azahar" to quit' >/dev/null 2>&1 || true
            for _ in $(seq 1 15); do pgrep -f Azahar.app >/dev/null 2>&1 || break; sleep 1; done
            if pgrep -f Azahar.app >/dev/null 2>&1; then log "quit ignored; SIGTERM"; pkill -TERM -f Azahar.app || true; fi
            for _ in $(seq 1 20); do pgrep -f Azahar.app >/dev/null 2>&1 || break; sleep 1; done
            if pgrep -f Azahar.app >/dev/null 2>&1; then log "SIGTERM ignored; SIGKILL (once)"; pkill -KILL -f Azahar.app || true; sleep 3; fi
        fi
    fi
    snapshot
    if [ -f "$BK" ]; then cp "$BK" "$INI"; log "ini restored"; fi
    if [ -n "$AUTOINPUT" ]; then
        if [ -f "$AIBK" ]; then cp "$AIBK" "$SD/gdx-autoinput.txt"; else rm -f "$SD/gdx-autoinput.txt"; fi
    fi
    if [ "$HAVE_LOCK" -eq 1 ]; then
        kill "$TOUCHER" 2>/dev/null || true
        rm -rf "$LOCK"
        log "lock released; artifacts in $OUT"
    else
        log "lock left to the caller; artifacts in $OUT"
    fi
}
snapshot() {
    cp "$GLOG" "$OUT/log.txt" 2>/dev/null || true
    cp "$AZLOG" "$OUT/azahar_log.txt" 2>/dev/null || true
    mkdir -p "$OUT/autotest" "$OUT/rec"
    cp "$SD"/autotest/*_L.bmp "$SD"/autotest/*_R.bmp "$OUT/autotest/" 2>/dev/null || true
    cp "$SD"/3ds/gdiffuser/rec/*.gdxrec "$OUT/rec/" 2>/dev/null || true
}
trap cleanup EXIT

# 2. ini merge (backup first)
cp "$INI" "$BK"
SETS=(--set debug.fh=1 --set debug.fh_every="$EVERY" --set debug.det="$DET" --set debug.platform=azahar
      --set debug.filelog=1 --set debug.replay_quit=1 --set debug.quit_at_tick="$QUITTICK")
if [ -n "$REPLAY" ]; then SETS+=(--set debug.input_replay="$REPLAY"); else SETS+=(--del debug.input_replay); fi
if [ -n "$RECORD" ]; then SETS+=(--set debug.input_record=1 --set debug.input_record_name="$RECORD"); else SETS+=(--del debug.input_record --del debug.input_record_name); fi
python3 "$HERE/inimerge.py" "$INI" "${SETS[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
if [ -n "$AUTOINPUT" ]; then
    [ -f "$SD/gdx-autoinput.txt" ] && cp "$SD/gdx-autoinput.txt" "$AIBK"
    cp "$AUTOINPUT" "$SD/gdx-autoinput.txt"
fi
if [ -n "$REPLAY" ] && [ ! -f "$SD/3ds/gdiffuser/rec/$REPLAY.gdxrec" ]; then
    if [ -f "$ROOT/replays/$REPLAY.gdxrec" ]; then mkdir -p "$SD/3ds/gdiffuser/rec"; cp "$ROOT/replays/$REPLAY.gdxrec" "$SD/3ds/gdiffuser/rec/"; log "staged replays/$REPLAY.gdxrec"; else log "replay $REPLAY not on SD nor in replays/"; exit 2; fi
fi
rm -f "$SD"/autotest/*_L.bmp "$SD"/autotest/*_R.bmp
if [ -n "$RECORD" ]; then rm -f "$SD/3ds/gdiffuser/rec/$RECORD.gdxrec"; fi
: > "$AZLOG" 2>/dev/null || true

# 3. launch + wait
log "launch $DSX (replay='$REPLAY' record='$RECORD' every=$EVERY det=$DET timeout=${TIMEOUT}s)"
open -a /Applications/Azahar.app --args "$DSX"
LAUNCHED=1
T0=$(date +%s); LAST=0
while true; do
    sleep 5
    NOW=$(date +%s); EL=$((NOW - T0))
    if [ $((NOW - LAST)) -ge 25 ]; then snapshot; LAST=$NOW; fi
    if grep -q "\[oracle\] quit\|\[apt\] shutdown" "$GLOG" 2>/dev/null; then
        log "run finished at ${EL}s: $(grep -m1 '\[rec\] end\|\[oracle\] quit\|\[apt\] shutdown' "$GLOG")"
        sleep 3; break
    fi
    if ! pgrep -f Azahar.app >/dev/null 2>&1; then log "Azahar exited at ${EL}s"; break; fi
    if [ "$EL" -ge "$TIMEOUT" ]; then log "timeout at ${EL}s"; break; fi
done
snapshot
log "receipts: $(grep -c '\[fh\] frame=' "$OUT/log.txt" 2>/dev/null || echo 0) [fh] lines, $(ls "$OUT/autotest" 2>/dev/null | wc -l | tr -d ' ') BMPs"
