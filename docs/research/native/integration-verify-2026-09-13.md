# integration-verify — both native levers on at once (2026-09-13)

Task: `native/main` had never been run with **both** native levers on together. `track` and
`machines` were each verified alone, on their own branches, and an earlier combined-lever
build had a bug that corrupted the in-race HUD. This is the combined-lever gate.

**Verdict: PASS.** With `[native] track=1` and `[native] machines=1` on together, the
canonical replay is **pixel-identical to the all-levers-off baseline on all 6002 frames**
(`fhdiff divergent=0`), all 8 SHOT captures are byte-identical, the in-race HUD is intact by
eye, and verify mode reports **zero mismatches** over 1.2 M triangles. Both levers on are also
**40.5 % faster** in the 30-car crowd bucket. The documented frame-1702 transient did not
occur at all.

## Builds

| | |
|---|---|
| Parity + verify + profile runs | `native/main@2611e0d2` (merge 2611e0d, track + machines + oracle) |
| Card staged for the user | `native/main@fce2dd1c` (merge fce2dd1, adds `native/interp`, default off) |
| Confirmation run on the staged build | `V1` — `fce2dd1c`, all-on, 6002/6002 frames identical to the `2611e0d2` baseline (below) |

The four evidence runs were taken on `2611e0d2`; `native/interp` merged while they were in
flight. `[native] interp` and `interp_verify` both default to `0`
(`port/3ds/gfx/n64/gdx_rt_glue.cpp:206-207`), so the interp merge cannot change the levers'
behaviour with the shipped defaults — and run `V1` re-runs the all-on configuration on
`fce2dd1c` itself and lands on the same 6002 hashes, which closes the gap rather than
assuming it away.

## Runs

Azahar Qt lane (`gdx-dev emu run`), New3DS profile, `platform=azahar`, replay
`replays/race-mutecity-start.gdxrec` (6001 ticks), `[debug] det=1 fh=1 fh_every=1
replay_quit=1`, `gputrace=0`, `renderthread=0`, `stereo=1`. Every run reached
`[rec] end tick=6001 lines=6001` and self-terminated.

| run | id | levers | frames | `stale=1` | `mark=miss` | real fatals |
|---|---|---|---|---|---|---|
| Q1 baseline | `20260913-103325-Q1-baseline` | `track=0 machines=0` | 6002 | 0 | 0 | 0 |
| Q2 all-on | `20260913-105001-Q2-allon` | `track=1 machines=1` | 6002 | 0 | 0 | 0 |
| Q3 verify | `20260913-111326-Q3-verify` | `track=2 machines=1 machines_verify=1` | 6002 | 0 | 0 | 0 |
| Q3b verify+census | `20260913-113655-Q3b-verify-census` | as Q3 plus `trkcensus=1` | 6002 | 0 | 0 | 0 |
| V1 all-on @fce2dd1c | `20260913-161232-V1-allon-fce2dd1` | `track=1 machines=1 interp=0` | 6002 | 0 | 0 | 0 |

"real fatals" excludes `[fatal-trace] atexit: process exiting via exit()`, which every clean
run emits on its normal exit path (see "Tooling findings" — the gdx-dev judge counts it as a
fatal and fails the run on it).

Lever activation is on the record in each run, not assumed:

```
Q2  [trk] init mode=1 census=0 templates(carve)=18/18 ... ops=5120 runs=256
Q2  [native] machines=1 walk_only=0 verify=0 mask=ffffffff lod_mask=1f (ini latched)
Q2  [native] machines registry: 150 lists (30 slots x 5 LODs)
Q2  [native] machines=1 lists=150 baked=93 bakes=0/116 fails=0 draws=64 walk=0 runs=448 ... fb=0 pool=0/0
```

`fails=0` and `fb=0` on every `[native]` window across the whole run: nothing fell back to the
stock path, so the native paths really did draw the frames that were compared.

## (1) vs (2) — fhdiff

```
Q1 baseline vs Q2 all-on
fhdiff: compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002
```

Every pair of the four `2611e0d2` runs is identical on all 6002 frames:

| pair | compared | divergent |
|---|---|---|
| Q1 vs Q2 | 6002 | 0 |
| Q1 vs Q3 | 6002 | 0 |
| Q1 vs Q3b | 6002 | 0 |
| Q2 vs Q3b | 6002 | 0 |

A per-frame comparison of the `mode=` token as well as the hash finds 0 frames differing in
either, so the two runs walk the same game-state timeline, not merely the same pixels.

### The shipped build (`fce2dd1c`)

`V1` re-runs the all-on configuration on the binary that is actually on the user's Desktop,
after `native/interp` merged:

```
V1 all-on @fce2dd1c  vs  Q1 baseline @2611e0d2
fhdiff: WARNING build mismatch: ...=native/main@fce2dd1c ...=native/main@2611e0d2
fhdiff: compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002

V1 all-on @fce2dd1c  vs  Q2 all-on @2611e0d2
fhdiff: compared=6002 divergent=0 ... last_common=6002
```

All 8 SHOT BMPs are byte-identical to Q2's as well, and `V1`'s `race01_L.bmp` was inspected by
eye: the in-race HUD is intact. So on the staged build, with `[native] interp` at its default
`0`, both levers on still produce exactly the all-off baseline frames — the interp merge is
inert at the shipped defaults and the combined-lever verdict carries to `fce2dd1c`.

**The expected frame-1702 exception did not occur.** The brief anticipated "identical except
frame 1702" from `oracle-progress.md`, and `fhdiff --known 1702` was not needed: the strict
gate passes. Two reasons, both benign:

- the 1702 transient is a *two-state* texrect/tmem fast-path artefact (`oracle-progress.md`
  "Frame 1702"), so it only shows when the two runs happen to land on opposite states;
- this build's menu timeline differs from `native/oracle@c3021cfb`. Frame 351's title hash is
  `5fe9ecac`, not the oracle's `4ee0d16f`, and frame 1702 is `mode=7` (main menu) here rather
  than `mode=10` (course select) — the boot hold resolves at a different tick, which shifts
  every later menu transition. **Q1, Q2, Q3, Q3b and V1 all share the same shifted timeline**
  (identical hash at frame 351 and everywhere else), so the lever comparison is sound; the
  shift is a property of the build, not of the levers.

The oracle's absolute frame-351 gate (`4ee0d16f`) therefore does not transfer to
`native/main`. A re-baseline of that constant on this branch is the follow-up, noted below.

## SHOT captures and the HUD

All 8 SHOT BMPs (`title`, `mainmenu`, `machsel`, `race00`..`race04`, 288,054 B each) are
**byte-identical** (`cmp`) between Q1, Q2, Q3 and Q3b.

Inspected by eye, `race01_L.bmp` from **Q2 (both levers on)**, frame 4001, `mode=1`, in-race:
lap counter `LAP 1/3`, position `29/30`, `TIME 00'04"22`, the ENERGY bar, all six rival
portraits down the left edge (1ST-6TH), the green `2ND` position marker, and the `783 km/h`
speed readout are all present and correctly placed; track, boost pads, rival craft and the
player craft all draw. **The combined-lever HUD corruption does not reproduce.**

(`race00_L.bmp` is the machine-settings screen, `mode=9` — the in-race HUD shot is `race01`.)

## (3) Verify modes

`track=2` (stock drawn + native recorded and compared) with `machines=1 machines_verify=1`
(walk pass drawn and captured, native pass captured with GPU submit suppressed, triangles
compared bit-exact), over the same full replay:

| counter | Q3 | Q3b |
|---|---|---|
| `[trk] verify MISMATCH` | 0 | 0 |
| `[trk]` sampled windows / runs / `vok` / `vbad` | census off | 29 / 73 / **73** / **0** |
| `[trk]` fallbacks in verified windows | — | 0 |
| `[native] verify` draws | 29,371 | 29,371 |
| `[native] verify` triangles | 1,206,950 | 1,206,950 |
| `triMismatch` / `countMismatch` / `matMismatch` / `auxDiff` | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

**Zero mismatch receipts, as expected.**

Q3b exists because the track lever's positive counters are gated: `[trk] verify MISMATCH` is
logged unconditionally, but the `[trk] replay(prev) ... vok=/vbad=` totals that prove *how
much* was checked are emitted only under `[native] trkcensus=1`
(`port/3ds/gfx/gdx3ds_track_sink.c:252`). Q3 was run with `trkcensus=0`, which made its track
half "no mismatch logged" with no coverage number; Q3b re-runs it with the census on. Worth
knowing for future verify runs: **`track=2` without `trkcensus=1` gives you a silent pass.**

The track sample is also modest — the census prints one window in 64 (`(sBeginCalls & 63u) ==
1u`), so a 6002-frame replay yields 29 windows and 73 verified runs, against the 13,878 the
track branch reported from a 69k-frame free-running attract session. 73/73 clean is real
evidence but it is not the same coverage; a long attract-mode verify run on `native/main` is
the cheap way to close that, and is listed below.

Verify mode is also pixel-clean: Q3 and Q3b hash-match Q1 on all 6002 frames.

## (4) Profile A/B — 30-car attract demo, `gputrace=1`

RetroArch lane, two private instances run **concurrently** so both see the same host noise,
no replay (free-running attract demo), `[debug] gputrace=1 det=1`, **oracle capture off
(`fh=0 fh_every=0`)** — `[fh]` frames are readback-stalled and must never appear in a
profiled window. 93 `[prof]` windows each, same build, `gputrace=1` recorded in both
manifests.

- A = `20260913-112811-P1B-allon` — `track=1 machines=1`
- B = `20260913-112808-P1A-alloff` — `track=0 machines=0`

`tools/dev/gdx-dev prof --ab <A> <B> --by-vtxn`, mean ms/frame (delta = B − A, positive =
all-on is faster):

```
[prof] mean ms/frame, all windows
section    A all-on    B all-off    delta     pct
br             3.06         3.70    +0.64   +21.1%
dsp           15.00        14.37    -0.63    -4.2%
vtx            0.69         1.75    +1.06  +154.6%
tri            8.46        10.95    +2.48   +29.3%
imp            2.48         2.44    -0.04    -1.6%
drw            1.10         1.07    -0.03    -2.5%
mtx            0.04         0.04    -0.00    -0.3%
oth            1.25         1.33    +0.08    +6.2%
total         32.08        35.66    +3.57   +11.1%

[prof] bucket: crowd (nD 100-inf)   <- the 30-car window
section    A all-on    B all-off    delta     pct
br             5.80         8.83    +3.03   +52.2%
dsp           24.90        21.76    -3.14   -12.6%
vtx            0.34         5.47    +5.13 +1494.0%
tri            2.34        14.16   +11.82  +506.0%
imp            1.66         1.45    -0.20   -12.4%
drw            3.67         3.54    -0.12    -3.4%
mtx            0.12         0.12    +0.00    +0.0%
oth            1.72         1.65    -0.06    -3.8%
total         40.55        56.98   +16.44   +40.5%

[prof] bucket: mid (nD 50-99)     total 33.77 vs 38.18   +4.41  +13.1%
[prof] bucket: light (nD 16-49)   total 32.78 vs 33.48   +0.70   +2.1%
[prof] bucket: menu (nD 1-15)     total 15.05 vs 15.12   +0.06   +0.4%

fps                p10   p50   p90
A all-on          18.0  22.0  48.4
B all-off         14.0  19.6  48.2

error receipts (A / B):  [fatal-trace]  1  1   (the normal-exit line; nothing else)
```

The two levers do exactly what they claim in the bucket that matters: in the crowd window
`vtx` collapses from 5.47 to 0.34 ms and `tri` from 14.16 to 2.34 ms, for **+16.44 ms/frame,
+40.5 %**. `dsp` rises 3.14 ms — cost moves into the display-list walker, which is the
expected shape for a retained path — and the net is still a large win. Menu-heavy windows are
a wash, as they should be: neither lever is on the menu path.

These are emulator + JIT numbers on a loaded host; **only the delta means anything**, and the
absolute ms/frame here must not be quoted as a hardware figure.

## Tooling and lane findings

Five things cost real time and are worth writing down. None of them is a fork bug except the
first, which is a `gdx-dev` bug.

**1. `gdx-dev --replay <x>.gdxrec` is a silent no-op in both lanes.** The guest opens the
replay as a path *relative to its cwd*, which is the **SDMC root** — `rec/<name>.gdxrec`
(`port/3ds/main_3ds.cpp:795`, `port/gdx_input_rec.c:189`). `gdxdev/replay.py rec_dir()`
stages it to `<sd>/3ds/gdiffuser/rec/` instead, so the guest logs

```
[rec] replay open failed: rec/race-mutecity-start.gdxrec
```

and the run proceeds with **no input at all** while the manifest still records the replay.
`tools/dev/oracle-run.sh:129` has it right (`$SD/rec/`), which is why the oracle branch never
hit this. Measured both ways: with the file at `<sd>/3ds/gdiffuser/rec/` the open fails; with
it at `<sd>/rec/` the guest logs `[rec] replay armed ... frames=6001`. Every run in this
document was staged to the SDMC root by a wrapper. The fix belongs in `rec_dir()` — it is one
line — but this agent was told not to commit code, so it is reported, not patched.

**2. RetroArch hangs on an invisible modal after it has been killed a few times.** Every
launch — `--version` included — stalled at 0.1 s of CPU with no log. `sample` showed the main
thread parked in `-[NSPersistentUIRestorer promptToIgnorePersistentStateWithCrashHistory:]`
→ `-[NSAlert runModal]`: macOS had accumulated crash history (the lane SIGKILLs RetroArch to
stop it) and was waiting on a dialog nobody can see. Cleared with

```sh
defaults write com.libretro.dist.RetroArch ApplePersistenceIgnoreState -bool YES
rm -rf "$HOME/Library/Saved Application State/com.libretro.dist.RetroArch.savedState"
```

This had already stopped another agent's runs dead before it stopped mine. It is worth putting
in `retro setup`.

**3. The RetroArch lane cannot carry the parity runs on this host.** The Azahar libretro core
dies with `SIGBUS EXC_ARM_DA_ALIGN` / `SIGSEGV` on its own `VulkanWorker` thread, inside
`azahar_libretro.dylib`, during boot — **with the levers off as well as on** (the all-off
baseline crashed at frame 35 and 39). It is not renderer-specific (OpenGL crashed too) and it
is not new: crash reports from 2026-09-12 23:27 and 2026-09-13 00:01 have the same faulting
thread, which is why the machines branch's `D3-verify` and `F3-prof1` runs failed. It
correlates with host load and with `fh_every=1` (a per-frame readback); the same replay with
`fh=0` completed cleanly. The parity runs therefore moved to the Azahar Qt lane, which is
where the oracle branch validated `fh_every=1` over 6002 frames, and only the profile A/B —
which must have `fh=0` anyway — stayed on the RetroArch lane. **This is an emulator defect,
not a port defect**, but it means the retro lane is not a parity lane while the host is busy.

**4. `[fatal-trace] atexit` fails every clean run.** `gdxdev/emu.py judge()` counts lines
beginning `[fatal`, and the normal exit path always emits `[fatal-trace] atexit: process
exiting via exit()`. Both profile runs were scored `FAIL 1 [fatal] line(s) in this run's
slice` with nothing whatsoever wrong. The `[fatal-trace]` prefix should be excluded from the
fatal census (it already has its own row in `prof --ab`'s error table).

**5. `[native] trkcensus=1` is required for `track=2` to report coverage** — see "(3) Verify
modes" above.

Also: the brief pointed at `<scratch>/native/PROTOCOL.md`, which does not exist anywhere on
this host (nor does any file named `PROTOCOL*`). Its conventions were followed as they are
quoted inside `tools/dev/README.md` and the branch progress docs — `gputrace` stated on every
perf claim, artifacts under the run directories, the Azahar lock respected, no ROM-derived
file committed.

## Deviations from the brief

- **Lane.** The brief asked for the RetroArch lane throughout. Parity runs (1)-(3) had to move
  to Azahar Qt for finding 3 above; the profile A/B (4) stayed on RetroArch as asked.
- **Concurrency.** Runs (1)-(3) are sequential, not concurrent — Azahar Qt is a singleton. The
  profile A/B pair is concurrent as asked. An earlier concurrent attempt at (1) and (2) on the
  RetroArch lane is what first exposed finding 3.
- **`fhdiff --known 1702`** was not needed; the strict comparison passes.
- **Q3b** was added because `track=2` reports no coverage without `trkcensus=1`.
- **V1** was added because `native/main` moved to `fce2dd1` mid-task and the staged card is
  built from it.

## Card staged for the user

`~/Desktop/G-Diffuser-3DS-native1/` — `G-Diffuser-3DS.3dsx`, `G-Diffuser-3DS.elf`,
`SHA256SUMS`, `TEST-PLAN.txt`. Built from `native/main@fce2dd1c`
(`G-Diffuser-3DS.3dsx` sha256 `bc823a6a1be76cad03960019f875e8ce911258a538d4680f4d116362115607f1`,
3,167,576 B). The SD card was **not** touched. `TEST-PLAN.txt` spells out the ini keys and seven runs: A all levers off, B `track=1`,
C `machines=1`, D both, E verify modes one lap, F 30-minute soak with both on and
`[stereo] enabled=1`, G `interp=1` through the menus and one race by eye. `[net] enabled` is
left at 0 throughout and the plan says so.

## Branch state at the time of writing

`native/main` advanced twice while this work was in flight: `fce2dd1` (merge `native/interp`)
and then `e5e8e68` (merge `native/devcli`, `adc45de fix(devcli): retro run no longer leaks
RetroArch processes`). `e5e8e68` touches only `tools/dev/` and `docs/` — no `port/`, `decomp/`
or `libultraship/` change — so the staged `fce2dd1c` binary's game code is identical to the
current HEAD's, and `V1` remains a run of the shipped game code.

Both tooling bugs reported above survive that merge and are still live on HEAD:
`gdxdev/replay.py rec_dir()` still returns `<sd>/3ds/gdiffuser/rec/`, and
`gdxdev/emu.py:134` still reads `fatals = errors.get("fatal", 0) + errors.get("fatal-trace", 0)`.

## Next steps

1. Fix `gdxdev/replay.py rec_dir()` to stage `.gdxrec` to the SDMC root (finding 1), and add a
   staging assertion so a failed `[rec] replay armed` fails the run instead of producing a
   silent no-input run.
2. Drop `[fatal-trace]` from `emu.py judge()`'s fatal census (finding 4).
3. Re-baseline the oracle's frame-351 title-hash gate for `native/main` — `5fe9ecac` on
   `2611e0d2`/`fce2dd1c`, not the `4ee0d16f` recorded on `native/oracle@c3021cfb` — and note
   in `oracle-progress.md` that frame 1702 is not the course-select frame on this branch.
4. Run a long attract-mode `track=2 trkcensus=1` session on `native/main` to get track-lever
   verify coverage comparable to the branch's own 13,878 runs (finding in "(3)").
5. Put the RetroArch persistent-state reset (finding 2) into `gdx-dev retro setup`, and record
   in `devcli-progress.md` that the retro lane is not a parity lane under load (finding 3).
