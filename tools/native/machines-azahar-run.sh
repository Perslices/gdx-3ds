#!/usr/bin/env bash
# machines-azahar-run.sh <label> <ini-keys-file> [seconds]
# Lock-protocol Azahar run for the machines track: merges the given "[section] key=value"
# fragment into the shared virtual-SD gdiffuser.ini (other keys preserved), installs the
# machines autoinput script, boots the machines build, snapshots log + SHOT BMPs to
# /tmp/machines-art/<label>/ every 25 s, and stops after [seconds] (default 420).
# Lock: mkdir /tmp/azahar.lock (poll), touch <=25 s, rm on exit. Never kills another
# agent's Azahar: we only launch while holding the lock.
set -euo pipefail
LABEL="$1"; KEYS="$2"; SECS="${3:-420}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SD="$HOME/Library/Application Support/Azahar/sdmc"
INI="$SD/3ds/gdiffuser/gdiffuser.ini"
ART="/tmp/machines-art/$LABEL"
BIN="$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.3dsx"
mkdir -p "$ART"
# ---- lock
while ! mkdir /tmp/azahar.lock 2>/dev/null || pgrep -xi azahar >/dev/null; do
    if [ -d /tmp/azahar.lock ] && ! pgrep -xi azahar >/dev/null && [ -z "$(find /tmp/azahar.lock -mmin -2)" ]; then
        echo "stale lock (untouched >2 min, no azahar): taking it"; rm -rf /tmp/azahar.lock; continue
    fi
    rmdir /tmp/azahar.lock 2>/dev/null || true  # we may have created it while azahar still runs
    echo "waiting for /tmp/azahar.lock / azahar exit ($(date +%T))"; sleep 15
done
trap 'rm -rf /tmp/azahar.lock' EXIT
# ---- ini merge (python: keep every other key/section, set ours)
python3 - "$INI" "$KEYS" <<'PY'
import sys, re
ini, keys = sys.argv[1], sys.argv[2]
lines = open(ini).read().split("\n")
want = {}
sec = None
for l in open(keys):
    l = l.strip()
    if not l or l.startswith(";"): continue
    m = re.match(r"\[(\w+)\]", l)
    if m: sec = m.group(1); continue
    k, v = [x.strip() for x in l.split("=", 1)]
    want.setdefault(sec, {})[k] = v
out = []; cur = None; seen = {}
for l in lines:
    m = re.match(r"\s*\[(\w+)\]", l)
    if m:
        if cur in want:
            for k, v in want[cur].items():
                if k not in seen.get(cur, set()):
                    out.append(f"{k} = {v}")
        cur = m.group(1); out.append(l); continue
    m = re.match(r"\s*(\w+)\s*=", l)
    if m and cur in want and m.group(1) in want[cur]:
        out.append(f"{m.group(1)} = {want[cur][m.group(1)]}"); seen.setdefault(cur, set()).add(m.group(1)); continue
    out.append(l)
if cur in want:
    for k, v in want[cur].items():
        if k not in seen.get(cur, set()): out.append(f"{k} = {v}")
for s, kv in want.items():
    if s != cur and not any(re.match(rf"\s*\[{s}\]", l) for l in lines):
        out.append(f"[{s}]"); out += [f"{k} = {v}" for k, v in kv.items()]
open(ini, "w").write("\n".join(out))
PY
cp "$INI" "$ART/gdiffuser.ini"
# ---- autoinput: title -> machine select (SHOT machsel) -> race (SHOTs race00/01/02)
cat > "$SD/gdx-autoinput.txt" <<'AI'
ticks
350 SHOT mx_title
400 START
800 A
1100 A
1400 A
1700 A
2000 A
2300 A
2300 SHOT mx_machsel
2450 RIGHT
2500 SHOT mx_machsel2
2600 A
2900 A
3200 A
3300 SHOT mx_race00
3300 A 3700
3300 STICK_LEFT 20
3700 STICK_RIGHT 20
4000 SHOT mx_race01
4400 STICK_LEFT 30
5000 SHOT mx_race02
5500 STICK_RIGHT 30
6000 SHOT mx_race03
AI
cp "$SD/gdx-autoinput.txt" "$ART/"
rm -f "$SD"/autotest/mx_*.bmp
: > "$SD/3ds/gdiffuser/log.txt"
sha256sum "$BIN" | tee "$ART/build.sha256"
open -a /Applications/Azahar.app --args "$BIN"
START=$(date +%s)
while [ $(( $(date +%s) - START )) -lt "$SECS" ]; do
    sleep 25
    touch /tmp/azahar.lock
    cp "$SD/3ds/gdiffuser/log.txt" "$ART/log.txt" 2>/dev/null || true
    cp "$SD"/autotest/mx_*.bmp "$ART/" 2>/dev/null || true
    if ! pgrep -x azahar >/dev/null && ! pgrep -x Azahar >/dev/null; then sleep 5; if ! pgrep -xi azahar >/dev/null; then echo "azahar exited"; break; fi; fi
done
pkill -xi azahar || true
sleep 3
cp "$SD/3ds/gdiffuser/log.txt" "$ART/log.txt" 2>/dev/null || true
cp "$SD"/autotest/mx_*.bmp "$ART/" 2>/dev/null || true
echo "run $LABEL done: $(ls "$ART" | tr '\n' ' ')"
