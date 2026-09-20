#!/usr/bin/env bash
# Stage the hardware card for the machines track: ~/Desktop/G-Diffuser-3DS-machines/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HOME/Desktop/G-Diffuser-3DS-machines"
mkdir -p "$OUT"
cp "$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.3dsx" "$ROOT/build-3ds/port/3ds/G-Diffuser-3DS.elf" "$OUT/"
(cd "$OUT" && shasum -a 256 G-Diffuser-3DS.3dsx G-Diffuser-3DS.elf > SHA256SUMS)
cat > "$OUT/TEST-PLAN.txt" <<TP
G-Diffuser-3DS — machines track (native retained machine meshes), branch native/machines
commit $(git -C "$ROOT" rev-parse --short HEAD)   $(date)

Install: copy G-Diffuser-3DS.3dsx to sd:/3ds/gdiffuser/ (keep fzerox.o2r, gdiffuser.o2r).
Set in sd:/3ds/gdiffuser/gdiffuser.ini (merge keys, keep the rest):
    [debug]
    filelog = 1
    [native]
    machines = 1            ; 0 = legacy interpreter path (A/B toggle)
    machines_verify = 0     ; 1 = in-frame walk-vs-native triangle compare (slow; parity oracle)
    machines_walk_only = 0  ; 1 = mesh replayed through the interpreter (bisect walk vs T&L)
    machines_mask = ffffffff  ; hex slot mask, machines_lod_mask = 1f (hex LOD1..5)

A) Visual A/B (machines=0 then machines=1): machine select (cycle all characters incl.
   Super Falcon/Stingray/Cat), records/ghost screens, a 30-car GP race (tunnels, attack
   highlights, ghosts in Time Attack), GP-end cutscene. Expected: no visible difference.
B) Parity oracle: machines=1 + machines_verify=1, drive one lap. Expected in log.txt:
   "[native] verify ... triMismatch=0 countMismatch=0 matMismatch=0" every window.
   auxDiff>0 is expected (batch-split vertex-0 records).
C) Perf: [debug] gputrace=1 (inflates; compare like with like), machines=0 vs 1 on the
   same track: [prof] tri/vtx/dsp/br columns, [native] us=.. per frame, [c3d] dNat, and
   the [native] linfree=..K receipt (linear free KiB).
D) Stability: no [fatal], no "bake FAIL" lines, no vtx-failsafe lines; HOME/sleep and
   a second race after returning to the menu (lists rebuilt -> "gen=" advances).
Receipts to copy back: sd:/3ds/gdiffuser/log.txt.
TP
echo "staged: $OUT"; cat "$OUT/SHA256SUMS"
