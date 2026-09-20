# integration-verify — every native lever on at once, and `interp` on top (2026-09-17)

Second combined-lever gate. The first (`integration-verify-2026-09-13.md`) covered `track`
and `machines` alone. Since then `native/fx` merged (effects sink, sprite batches, HUD
prim/env fold) and brought the `[native]` section reader with its `all` master,
`native/interp`'s M2 residuals were root-caused and fixed, and `native/atlasuv` landed the
view clamp-limit fix. This checks all of it together.

**Verdict: PASS on the Tier-2 levers and on `interp` — and FAIL on the two combined.**

- All five Tier-2 levers on together (`track machines fx_sink fx_sprites fx_hud`) are
  **pixel-identical to the all-off baseline on all 6002 frames** of the canonical replay,
  all 8 SHOT captures byte-identical, in-race HUD intact by eye. Confirmed on two builds.
- The three verify modes run together (`track=2 machines_verify=1 fx_sink=2`) report
  **zero mismatches** with **nonzero coverage on all three**, and are themselves
  pixel-identical to the baseline.
- `interp=1` **on its own** is pixel-identical to the baseline over the 4101 frames that
  cover title → menus → race start → mid-race, all 5 SHOTs in that span byte-identical.
- **`[native] all=1` produces a broken frame.** With `interp=1` *and* the Tier-2 levers on,
  the **course geometry is missing for the whole race** — the machines fly over the empty
  city backdrop. 859 of the 4101 compared frames diverge, every one of them `mode=1`
  (in race), starting at frame 3237, the first frame of the race. Deterministic, and
  identical on both builds tested. §5.

The two halves of the fork are each correct; they must not be switched on at the same time
until the owned interpreter carries the Tier-2 hooks. `all=1` is currently the one setting
a user should not use.

## Builds

Two, because `native/main` moved twice while this was in flight. Cite the **artifact
sha256**, not the build id: `GDX3DS_BUILD_ID` is baked at configure time.

| | build A | build B |
|---|---|---|
| commit | `native/main@19c0fce9` | `native/main@999523f7` |
| `.3dsx` sha256 | `838d31d18071219b20958df41c2e32e239b8bbdaeb5af0cf0460c783b981708f` (3,194,496 B) | `b0ff58cfc33f273c8d14368a4d2e510fdd65d96442bdaccadc9b780995a40a6f` (3,196,584 B) |
| `.elf` sha256 | `82d9f1754a62ee22674f1f6f0e2b9286b344c9c52dcd7b032d565a9a23beb185` | `fff7ca1c64c946c4a7905d93e2a3df4034dde9bee8b6dcaad9b9da5977e28ff5` |
| what it adds | the `fx` merge (patch 15) | `+ 80b16a9` (devcli, tools/docs only) `+ 999523f` (`native/atlasuv`: the view clamp limit in `AtlasFillTexParams`) |

Build B is what the card ships and what STATUS.md describes. The `19c0fce..80b16a9` delta
is `tools/dev/` and `docs/` only — no `port/`, `decomp/` or vendored-LUS change — so build
A's game code is `80b16a9`'s game code as well.

**Cross-build control:** the all-off baseline on build A and on build B hash-match on all
6002 frames (`fhdiff R1 S1: compared=6002 divergent=0`), so every build-A result below
carries to build B unchanged. That is expected here rather than a check of the atlasuv
work: the shared Azahar ini had `[debug] trectbatch = 0` (another session's key), and with
the atlas off the fix has nothing to change. Runs T1/T2 repeat the headline A/B with
`trectbatch=1`.

## Runs

Two lanes. Parity and verify on the **Azahar Qt lane** (`gdx-dev emu run --lane azahar`),
one at a time, because that is the lane the oracle validated `fh_every=1` on over 6002
frames and dense capture is what kills the libretro core. Profiles on the **RetroArch
lane**, three private instances concurrently, because an A/B must see the same host noise
and that lane has no singleton.

Common to every run: New3DS profile, `platform=azahar`, the canonical replay
`replays/race-mutecity-start.gdxrec` (6001 ticks, 8 SHOTs; title → menus → 30-machine GP
Mute City from tick 3226, retire/results at 4895), `[debug] det=1 renderthread=0`,
`[stereo] enabled=1` (slider 0), `[net] enabled=0`. **Every `[native]` key was passed
explicitly on every run**, including the ones meant to be 0 — gdx-dev merges `--ini` into
the shared ini and the keys persist; R1's merge log shows it arriving with another
session's `interp=1 machines=1 track=1 interp_verify=1`.

### Parity / verify (Azahar Qt, `fh=1 fh_every=1`, `gputrace=0`)

| run | id | build | `[native]` levers | wall_s | `[fh]` frames | SHOTs | `[rec] end` |
|---|---|---|---|---|---|---|---|
| R1 baseline | `20260917-110757-R1-baseline` | A | all 0 | 1141 | 6002 | 8/8 | `tick=6001 lines=6001` |
| R2 Tier-2 | `20260917-112728-R2-allon-nointerp` | A | `track=1 machines=1 fx_sink=1 fx_sprites=1 fx_hud=1` | 821 | 6002 | 8/8 | `tick=6001 lines=6001` |
| R3 verify | `20260917-114128-R3-verify` | A | `track=2 trkcensus=1 machines=1 machines_verify=1 fx_sink=2 fx_sprites=1 fx_hud=1 fxcensus=1` | 861 | 6002 | 8/8 | `tick=6001 lines=6001` |
| R4 `all=1` | `20260917-120445-R4-all-interp` | A | `all=1 interp=1` (`path=owned`) | 841 | 6002 | 8/8 | `tick=6001 lines=6001` |
| S1 baseline | `20260917-122115-S1-baseline` | B | all 0 | 821 | 6002 | 8/8 | `tick=6001 lines=6001` |
| S2 Tier-2 | `20260917-123456-S2-tier2` | B | the five Tier-2 levers | 821 | 6002 | 8/8 | `tick=6001 lines=6001` |
| S3 interp only | `20260917-124837-S3-interponly` | B | `interp=1`, Tier-2 all 0 | 581 | 4101 | 5/5 in span | `quit_at_tick=4100` |
| S4 `all=1` | `20260917-125817-S4-all` | B | `all=1 interp=1` | 581 | 4101 | 5/5 in span | `quit_at_tick=4100` |
| T1 baseline, atlas | `20260917-130815-T1-baseline-atlas` | B | all 0, `trectbatch=1` | 820.9 | 6002 | 8/8 | `tick=6001` |
| T2 Tier-2, atlas | `20260917-132156-T2-tier2-atlas` | B | the five Tier-2 levers, `trectbatch=1` | 801.0 | 6002 | 8/8 | `tick=6001` |
| U1 interp+track | `20260917-133522-U1-interp-track` | B | `interp=1 track=1` | 500.7 | 3401 | — | `quit_at_tick=3400` |
| U2 interp+mach+fx | `20260917-134343-U2-interp-mach-fx` | B | `interp=1 machines=1 fx_sink=1 fx_sprites=1 fx_hud=1` | 500.6 | 3401 | — | `quit_at_tick=3400` |

S3/S4/U1/U2 stop deliberately at a tick (the race start and the first in-race SHOT are all
the isolation needs), so gdx-dev scores them FAIL on the new completion check — see
"Findings", item 7. Every other run is a full replay and was checked against the replay
itself, not just against the verdict word: 6002 `[fh] frame=` receipts (6001 ticks + 1),
all 8 SHOT labels captured, the `[rec] end tick=6001 lines=6001` receipt present,
`stale=1` and `mark=miss` both zero, and a wall time consistent with a full replay rather
than an early core death.

Lever activation is on the record — the boot receipt of each run:

```
R1/S1  [native] all=0 interp=0 machines=0 track=0 fx_sink=0 fx_sprites=0 fx_hud=0 fx=0 census trk=0 fx=0
R2/S2  [native] all=0 interp=0 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0
R3     [native] all=0 interp=0 machines=1 track=2 fx_sink=2 fx_sprites=1 fx_hud=1 fx=7 census trk=1 fx=1
S3     [native] all=0 interp=1 machines=0 track=0 fx_sink=0 fx_sprites=0 fx_hud=0 fx=0 census trk=0 fx=0
R4/S4  [native] all=1 interp=1 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0
R1-R3/S1-S2  [native] interp=0 path=lus   cpuclip=1 blendflush=1 verify=0
R4/S3/S4     [native] interp=1 path=owned cpuclip=1 blendflush=1 verify=0
```

### Profile (RetroArch, three concurrent private instances, `gputrace=1`, **no capture**)

| run | id | levers | windows | `[rec] end` | `[fh]` |
|---|---|---|---|---|---|
| Q1 | `20260917-113655-Q1-alloff` | all 0 | 93 | yes | 0 |
| Q2 | `20260917-113655-Q2-tier2` | the five Tier-2 levers | 93 | yes | 0 |
| Q3 | `20260917-113655-Q3-all` | `all=1` (`path=owned`) | 93 | yes | 0 |

Build A, `gputrace=1` recorded in all three manifests (`prof --ab` refuses a mismatched
pair), wall 1508 s each, zero error receipts, `RESULT PASS … complete=True`. **Zero `[fh]`
lines in any profiled window** — captures are readback-stalled and must never appear in
one. To get that *and* a completion check, the three runs used a SHOT-stripped copy of the
canonical replay (staging a `.gdxrec` that has SHOTs now arms `[debug] fh=1`); the button
and stick stream is byte-for-byte the committed file's, only the `SHOT` trailers are gone.

## (1)+(2) Baseline vs all five Tier-2 levers

```
fhdiff R1 baseline  vs  R2 Tier-2   (build A)
compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002

fhdiff S1 baseline  vs  S2 Tier-2   (build B)
compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002
```

All 8 SHOT BMPs (288,054 B each) are **byte-identical** (`cmp`) in both pairs: `title`,
`mainmenu`, `machsel`, `race00`, `race01`, `race02`, `race03`, `race04`.

No `--known` was needed; the strict comparison passes, frame 1702 included.

**The oracle's absolute frame-351 gate is back.** Every run here hashes `4ee0d16f` at frame
351 — the constant recorded on `native/oracle@c3021cfb` — and frame 1702 is `mode=10`
(course select), as the oracle documented. The 2026-09-13 report found `5fe9ecac` /
`mode=7` on `2611e0d2` and concluded the gate no longer transferred to `native/main`; on
these builds it does. Item 3 of that report's "next steps" can be closed as "no re-baseline
needed".

The native paths really did draw the compared frames. R2's `[native]` windows sum to
**26,346 native machine draws, 2,130,980 triangles, 134,521 material runs, `fails=0`,
`fb=0`, `pool=0/0`** — nothing fell back to the stock path and no bake failed. (`track` and
`fx` print their per-frame totals only under their census keys, off in R2; R3 is the same
lever set with the censuses on and reports them — next section.)

## (3) The three verify modes together

R3 runs `track=2` (stock drawn, native replayed into a discarded batch and hash-compared
per run), `machines=1 machines_verify=1` (walk pass drawn and captured, native pass
captured with GPU submit suppressed, triangles compared bit-exact) and `fx_sink=2` (the
same shape for the effect sprite runs) over the whole replay, with both census keys on.

| lever | coverage | mismatches |
|---|---|---|
| `track=2` | 23 census windows, **70 markers / 70 runs verified, `vok=70 vbad=0`**, `fb=0` | `[trk] verify MISMATCH` × **0** |
| `machines_verify=1` | 39 windows, **26,346 draws / 1,172,615 triangles compared bit-exact** | `triMismatch=0 countMismatch=0 matMismatch=0 auxDiff=0` on every window; `[native] verify MISMATCH` × **0** |
| `fx_sink=2` | **1,643 markers verified, `vok=1643 vbad=0`**, 11,808 triangles replayed (4,717 fast / 3,065 exact), `fb=0` | `[fx] verify MISMATCH` × **0** |

Run-wide receipts:

```
[native] verify draws=55 tris=3818 triMismatch=0 countMismatch=0 matMismatch=0 auxDiff=0
         (cumulative triMismatch=0; auxDiff excludes the per-vertex inputs, so it must be 0)
[fx] agg2 stockdraws=1888 nativedraws=0 maxnative=0 markers=1643 tris=11808 fast=4717 exact=3065
         fb=0 fold=p446/e0 split=p0/e446/t423 vok=1643 vbad=0
[fx] hud fold=p53584/e2099 (interpreter batches kept across prim/env changes, [native] fx_hud)
[trk] replay(prev) markers=1 runs=1 vtx=15 verts=128 tris=240 fast=68 exact=34 draws=0 comb=0 fb=0 vok=1 vbad=0
```

(`nativedraws=0` / `draws=0` are correct in verify mode: the native batch is hashed and
discarded rather than submitted.)

Verify mode is also pixel-clean:

```
fhdiff R1 baseline vs R3 verify : compared=6002 divergent=0 ... last_common=6002
fhdiff R2 Tier-2   vs R3 verify : compared=6002 divergent=0 ... last_common=6002
```

**Both census keys are still load-bearing.** `track=2` reports no coverage without
`[native] trkcensus=1` (the 2026-09-13 report's finding 5), and `fx_sink=2` reports none
without `[native] fxcensus=1` — `[fx] agg2`, which carries `vok=`/`vbad=`, sits inside the
`sCensus && (sBeginCalls & 63u) == 1u` gate (`port/3ds/gfx/gdx3ds_fx_sink.c:289`). Both
MISMATCH lines are unconditional, so a census-off verify run looks like a clean pass and
proves nothing about how much was checked. `machines_verify=1` is the exception: its
counters print unconditionally.

## (4) `interp` — alone, and on top of the Tier-2 levers

```
fhdiff S1 baseline vs S3 interp=1 only
compared=4101 divergent=0 known_transient=0 only_in_a=1901 only_in_b=0 first_divergent=none last_common=4101

fhdiff S1 baseline vs S4 all=1 (interp + Tier-2)
compared=4101 divergent=859 known_transient=0 only_in_a=1901 only_in_b=0 first_divergent=3237 last_common=4101

fhdiff R1 baseline vs R4 all=1 (build A, full replay)
compared=6002 divergent=1537 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=3237 last_common=6002

fhdiff R2 Tier-2   vs R4 all=1 (build A, full replay)
compared=6002 divergent=1537 ... first_divergent=3237
```

(`only_in_a=1901` is S1's frames past S3/S4's deliberate stop at tick 4100, not a
divergence.)

`interp=1` on its own is **identical** — 4101 frames covering title, every menu, the course
intro card, the race start and a minute of racing, and the five SHOTs in that span
(`title`, `mainmenu`, `machsel`, `race00`, `race01`) are byte-identical to the baseline's.
That is a materially stronger result than the `interp` track's own evidence, which was
SHOT byte-compares on the RetroArch lane; this is every frame.

`all=1` diverges, and only in race. The divergent frames of R4's full run fall in four
runs, all `mode=1`:

```
3237-4079 (843 frames)   4086-4110 (25)   4118-4135 (18)   4141-4791 (651)
```

Frame 3237 is the first frame of the race (the replay enters at tick 3226). Everything
before it — boot, title, main menu, mode select, course select, machine select, machine
settings, the course intro card — and everything after 4791 (retire, results, records) is
identical. The divergent hashes are the same on build A and build B
(`7361e9a8, 810602fb, 36258603, 79e98a3c` at frames 3237-3240 in both), so this is
deterministic, not a transient.

SHOT BMPs, R1 vs R4: `title`, `mainmenu`, `machsel`, `race02`, `race03`, `race04`
**identical**; `race00` and `race01` **differ**.

```
framediff race00  diff_pixels=29648  max_delta=239  bbox=15,16-384,223
framediff race01  diff_pixels=44223  max_delta=255  bbox=15,77-384,223
```

Contact sheets (`tools/dev/framediff.py --sheet`, not committed — they are BMP-derived
frames of the game) show what it is, unambiguously: **the course is not drawn.** In
`race00` the baseline has the grey Mute City start grid with its cyan edge strips, the
guard rails and the start gantry; the `all=1` frame has the 30 machines sitting on the
empty pink cityscape with no road under them at all. In `race01` the baseline has the road
surface, rails and the yellow/green boost pads; the `all=1` frame has a flat pink grid
where the road should be. The HUD, the rival portraits, the lap/position/time readouts, the
speed readout, the minimap and the machines themselves are all correct in both.

## (5) Root cause: `interp=1` drops the Tier-2 markers

The Tier-2 levers hook the **vendored LUS** interpreter, and nothing else:

- `track` and `fx_sink` do not draw. They make the game emit a `G_NOOP` marker
  (`p=0x5A`, `w1` = the recorded run) **in place of** the real `gSPVertex`/`gSPDisplayList`
  commands, and the geometry is replayed from LUS's `G_NOOP` handler
  (`Interpreter::GdxTrackMarker`, and the fx equivalent).
- `machines` needs the **bridge** (`port/n64_gfx_bridge.cpp`, `case kOpDl`) to redirect a
  registered `D_800CDDB0[]` list to a stub, plus LUS's `gfx_dl_handler_common` calling
  `gdx3ds_mesh_dl_hook`.
- `fx_hud` is the fold rule inside LUS's own 2D batches
  (`port/3ds/vendor/lus/src/fast/interpreter.cpp:709-712`).

Under `interp=1` the owned walker runs instead and none of those exist:

- `OP_NOOP` is a bare `return 0` (`port/3ds/gfx/n64/gdx_dl.cpp:179-186`) — no marker
  dispatch. The recorded run is therefore **dropped**, and because the shim already
  replaced the real GBI with the marker, the geometry is gone. That is the missing course.
- The owned walker does have a `mDlHook` seam (`gdx_dl.cpp:197`, `gdx_interp.h:306-309`),
  but **`RegisterDlHook` is never called anywhere in the tree**, so the machines hook is
  unwired; and the prepare pass skips `ConvertRoot` under `interp=1`, so the bridge never
  registers a stub either. `machines` is therefore merely inert, not destructive — which
  is why the machines still draw in the broken frames.

Receipts agree. Q3 (`all=1`, `path=owned`) and Q2 (the same five levers, `path=lus`) over
the same replay:

```
Q2 (interp=0)  [native] machines=1 lists=150 baked=70 ... draws=64 walk=0 runs=512 ... fb=0
Q3 (interp=1)  — no [native] machines= receipt anywhere in the run (it is silent when the
                 lever draws nothing)
Q3 (interp=1)  [ninterp] tasks=64 lists=1728 cmds=175232 vtx=125376 tri=224640 rect=6912 ...
```

Isolation runs U1/U2 pin the attribution to a single lever. Both run to tick 3400, which
covers the race start:

```
fhdiff S1 baseline vs U1 (interp=1 track=1)
compared=3401 divergent=165 first_divergent=3237
   frame=3238 A=26546ce4 B=810602fb    frame=3239 A=1d956455 B=36258603    frame=3240 A=dfa9e086 B=79e98a3c

fhdiff S1 baseline vs U2 (interp=1 machines=1 fx_sink=1 fx_sprites=1 fx_hud=1)
compared=3401 divergent=0 first_divergent=none
```

U1's divergent hashes are **the same hashes S4 and R4 produce** (`810602fb`, `36258603`,
`79e98a3c` at frames 3238-3240), and U1's `race00_L.bmp` is **byte-identical to `all=1`'s**
— so `track=1` with `interp=1` accounts for the entire defect on its own. U2 is clean on
every frame and every SHOT in its span: `machines` and the three fx levers are inert under
`interp=1`, not destructive.

One caveat on U2: its window ends at the race start, which is effect-light. `fx_sink`'s
marker has the same shape as `track`'s — the sprite vertex/triangle ops live in the record,
not in the stream — so by construction the six effect-sprite classes should vanish under
`interp=1` too; it simply was not observed here because the isolation window barely draws
them. Treat `fx_sink=1 interp=1` as suspect, not as cleared.

**The `all` master makes this the default way to turn things on.**
`port/3ds/gfx/gdx3ds_native_cfg.c:33-36` applies `all=1` to every lever *including*
`interp`, and an explicit `interp=0` does not survive it (a lever that reads 0 becomes 1).
So `[native] all=1` always means `interp=1`, and it simultaneously sets `track=1` and
`fx_sink=1`, which is exactly the combination that loses the course. The boot receipt
prints `all=1 interp=1 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7` with no
warning, and there is no single key that means "every Tier-2 lever, stock interpreter" —
R2's configuration has to be spelled out by hand.

Suggested fixes, in order of cost:

1. **Make the combination impossible.** When `interp=1` wins, force `track`, `fx_sink`,
   `fx_sprites`, `fx_hud` and `machines` to 0 in `gdx3ds_native_cfg.c` and say so in the
   boot receipt (`… (track/fx forced 0: interp=1 has no marker hook)`). One-line-ish, and
   it turns a broken frame into a documented restriction.
2. **Take `interp` out of the `all` master** — it is the one lever that changes *which
   renderer runs*, not *what it does* — or add `all_tier2`. Then `all=1` means the Tier-2
   set and is safe.
3. **Wire the hooks**: dispatch the track/fx marker from the owned walker's `OP_NOOP`, and
   call `RegisterDlHook(gdx3ds_mesh_dl_hook, …)` plus whatever bridge registration the
   machines bake needs. That is real work and belongs to the `interp` track's M5, not here.

Until one of those lands, `[native] all=1` should be treated as "do not use", and the
hardware card says so.

## (6) Profile A/B — `gputrace=1`, race replay, 93 frame-aligned windows each

Three private RetroArch instances run **concurrently** so all three see the same host noise
(load average 55-70 throughout; other agents' emulators were on the machine).
`tools/dev/gdx-dev prof --ab <A> <B> --by-vtxn`, mean ms/frame, delta = B − A, positive =
A is faster. The `crowd` bucket (`nD ≥ 100`) is the 30-car race window.

### Q2 Tier-2 levers vs Q1 all off

```
[prof] bucket: crowd (nD 100-inf)
section   Q2 Tier-2   Q1 all off   delta      pct
br             3.51         4.38   +0.87   +24.7%
dsp           16.23        15.37   -0.86    -5.3%
vtx            0.76         2.01   +1.25  +164.0%
tri           10.35        13.12   +2.77   +26.8%
imp            2.63         2.53   -0.10    -4.0%
drw            2.93         2.92   -0.01    -0.4%
mtx            0.04         0.04   -0.00    -0.6%
total         37.01        40.92   +3.91   +10.6%

all windows   33.33        37.05   +3.73   +11.2%
mid  (50-99)  18.74        20.79   +2.05   +10.9%
light(16-49)  20.33        21.11   +0.78    +3.8%
menu (1-15)   20.77        20.89   +0.12    +0.6%

fps        p10   p50   p90
Q2        18.4  21.4  49.8
Q1        13.6  18.6  46.8
```

The shape is what the levers claim: `vtx` collapses 2.01 → 0.76 ms and `tri` 13.12 → 10.35
in the crowd window, `br` falls because the bridge converts fewer commands, and `dsp` rises
0.86 ms as cost moves into the marker replay and the retained walker. Menu windows are a
wash, as they must be — no Tier-2 lever is on the menu path.

### Q3 `all=1` (owned interpreter) vs Q2 Tier-2 levers

```
[prof] bucket: crowd (nD 100-inf)
section   Q3 all=1   Q2 Tier-2    delta      pct
br            0.04        3.51   +3.47  +8886%
dsp          11.16       16.23   +5.07   +45.4%
vtx           2.91        0.76   -2.14   -73.8%
tri          14.49       10.35   -4.14   -28.6%
imp           2.04        2.63   +0.59   +29.1%
drw           2.92        2.93   +0.02    +0.5%
mtx           0.21        0.04   -0.18   -82.2%
total        33.87       37.01   +3.13    +9.2%

all windows  30.00       33.33   +3.32   +11.1%
```

### Q3 `all=1` vs Q1 all off

```
[prof] bucket: crowd (nD 100-inf)
section   Q3 all=1   Q1 all off   delta       pct
br            0.04         4.38   +4.34   +10894%
dsp          11.16        15.37   +4.21    +37.7%
vtx           2.91         2.01   -0.90    -30.9%
tri          14.49        13.12   -1.37     -9.4%
imp           2.04         2.53   +0.49    +24.0%
drw           2.92         2.92   +0.00     +0.1%
total        33.87        40.92   +7.04    +20.8%
```

**Caveat that has to be stated: Q3 was not drawing the course** (§4/§5). Its numbers are a
real measurement of the owned interpreter, but on a frame that is missing geometry, so the
`vtx`/`tri` figures flatter it and the comparison against Q2 is not apples to apples.
`br` → **0.04 ms** is the one number that is unaffected and it is the headline: the
bridge's conversion pre-pass, 4.34 ms/frame in the crowd window, disappears entirely.

Read together the tables say the two halves win in *different* sections — `interp` kills
`br` and `dsp`, Tier-2 kills `vtx` and `tri` — so combining them ought to be close to
additive, and today it cannot be done at all. That is the prize §5 is about.

Zero error receipts in all three runs. Emulator + JIT numbers on a loaded host with
`gputrace` on: **only the deltas mean anything**, and none of these absolutes is a hardware
figure.

## (7) The atlas configuration

Every run above had `[debug] trectbatch = 0` — the shared Azahar ini carried it, so the
`[trectbatch]` atlas was off. The lever comparisons are unaffected (both sides of every
A/B share the state), but `trectbatch` defaults to **1**, so those runs are not the
shipping configuration. T1/T2 repeat the headline A/B with the atlas on, on build B:

```
fhdiff T1 baseline(trectbatch=1)  vs  T2 Tier-2(trectbatch=1)
compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002
```

All 8 SHOT BMPs are byte-identical as well, so **the headline result holds in the default
atlas configuration**: the five Tier-2 levers are pixel-identical to all-off with
`trectbatch=1` exactly as with `trectbatch=0`.

One more thing falls out of it. On build B the **atlas and the standalone path now agree**:

```
fhdiff S1 baseline (trectbatch=0)  vs  T1 baseline (trectbatch=1)
compared=6002 divergent=0 ... last_common=6002    (all 8 SHOTs byte-identical too)
```

That is the `residuals2` "atlas sub-texel shift" (25 of 43 probe frames differing, a
~0.16-texel one-directional U shift on glyph rects) closed by `native/atlasuv` — at least
on this replay, on this emulator. It still wants a hardware look, since the residual could
always have been an Azahar sampler artefact.

## (8) Screenshots by eye

R2 (`race01_L.bmp`, frame 4001, `mode=1`, in race, all five Tier-2 levers on): lap counter
`LAP 1/3`, position `29/30`, `TIME 00'05"77`, the ENERGY bar, all six rival portraits down
the left edge (1ST-6TH), the green `2ND` position marker and the `755 km/h` readout are all
present and correctly placed. Track surface, guard rails, the yellow/green boost pads, two
rival craft and the player craft with its boost flame all draw, and the figure-eight course
minimap is intact. **The combined-lever HUD corruption does not reproduce.**

R2 `machsel_L.bmp` (`SELECT MACHINE`, all 30 machine thumbnails, the 1P portrait and the
BODY/BOOST/GRIP bars) and `race03_L.bmp` (the `1: MUTE CITY / FIGURE EIGHT` intro card over
the F-ZERO JACK CUP crest and the character collage) are both correct. All 8 of R2's SHOTs
are byte-identical to R1's, so inspecting R2 inspects the baseline too.

R4 (`all=1`): the HUD, portraits, readouts, minimap and machines are correct; the course is
absent (§4). R4's `title`, `mainmenu`, `machsel`, `race02`, `race03` and `race04` are
byte-identical to the baseline's, and S3 (`interp=1` alone) is byte-identical on every SHOT
in its span — so the owned interpreter draws every menu and the race correctly; only the
combination is broken.

## Findings for the lane and the tools

**1. `[native] all=1` loses the course geometry.** §4/§5. The one finding of substance.

**2. `fx_sink=2` gives a silent pass without `fxcensus=1`**, the same shape as the
`track=2` / `trkcensus=1` finding from 2026-09-13. Both MISMATCH lines are unconditional;
both coverage totals sit behind the census gate.

**3. The 2026-09-13 report's "attract demo" profile runs were not the attract demo.** That
report's P1A/P1B are described as free-running attract-demo runs with no replay; their logs
open with `[rec] replay armed … frames=6001`, because the RetroArch lane at the time
inherited `input_replay` from the seeded ini. They were replay runs, which is why they had
`crowd`/`mid`/`menu` buckets. The lane now strips run-control keys from the seed, and a
genuinely input-free run on this build **never leaves `mode=0`**: three 300 s runs reached
36,000+ guest frames with `md=0` on every `[gpu]` receipt, ~97 texrect draws per frame, no
`Course_Draw`/`Racer_Draw` activity at all — and all three lever configurations therefore
measured identical to two decimal places. If a profile needs a crowd window, stage the
replay; do not assume the attract demo starts on its own.

**4. `gdx-dev emu run` waits out `--seconds` even after the guest has exited.**
`replay_quit=1` ends the *guest*, but Azahar keeps its window and the harness's loop only
breaks when the emulator process dies (`tools/dev/gdxdev/emu.py:266-274`). R1 was given
`--seconds 2400`, finished its replay at ~1140 s, and would have idled another 21 minutes.
The runs here used a scratch watcher that kills Azahar once the guest logs `[oracle] quit
requested`. Worth having in `emu run` itself: once a staged replay's completion receipts
are all in, stop.

**5. `[fatal-trace] atexit` still fails a clean run's fatal census.**
`tools/dev/gdxdev/emu.py:134` folds `fatal-trace` into `fatals`, and every `replay_quit=1`
run emits `[fatal-trace] atexit: process exiting via exit()` on its normal exit path. Every
run here carries exactly one and is otherwise clean. (`prof.py` already classifies it
benign; `judge` does not.) Carried over from the 2026-09-13 report, finding 4.

**6. The shared Azahar ini arrives dirty, and that includes `[debug] trectbatch`.** R1's
merge log shows `[native] interp: 1 -> 0`, `machines: 1 -> 0`, `track: 1 -> 0`,
`interp_verify: 1 -> 0` — another session's keys — and `trectbatch = 0` was sitting in the
file from a previous session and was *not* on the list gdx-dev strips, so eight runs were
made in the non-default atlas configuration before anyone noticed (§7). Pass every key the
result depends on, every run, including `[debug] trectbatch`.

**7. `quit_at_tick` fails the new completion check.** A run that stops deliberately at a
tick has no `[rec] end` and is missing the SHOTs past that tick, so `judge` reports
`FAIL replay did not complete`. The check is right to demand completion of a *full* replay;
it should treat an explicit `quit_at_tick` the way it treats `replay_quit` and score
against the SHOTs scheduled *before* that tick.

**8. The RetroArch lane carried nine 300-1500 s runs today with zero core deaths**, at load
average 55-70, three instances at a time, including three concurrent with an Azahar Qt
capture run. The 2026-09-13 report's "the retro lane is not a parity lane while the host is
busy" still holds for dense `[fh]` capture, but for profile and completion runs it was
solid.

## Deviations from the brief

- **Run (2) could not be `[native] all=1` with `interp` at 0.** The `all` master sets
  `interp` too and an explicit `interp=0` does not survive it (§5), so R2/S2 spell the five
  Tier-2 levers out by hand. R4/S4 (`all=1 interp=1`) are the brief's run (4).
- **Run (4) is not "near-identical" to run (2).** It diverges on 1537 of 6002 frames and
  the cause is a real bug, not a transient — §4, §5, with the contact-sheet analysis.
- **Extra runs.** S3 (`interp=1` alone) and U1/U2 were added to isolate the bug, T1/T2 to
  repeat the headline A/B in the default atlas configuration, and S1/S2 to re-establish the
  headline on build B after `native/atlasuv` merged mid-task.
- **Lane.** Parity and verify ran on the Azahar Qt lane, not RetroArch: dense `[fh]`
  capture is what kills the libretro core, and the Qt lane is where `fh_every=1` over 6002
  frames is validated. The profile A/B stayed on RetroArch as asked.
- **The profile A/B uses the race replay, not the attract demo** — finding 3; a genuine
  attract-demo run has no 30-car window on this build.
- **The profile runs used a SHOT-stripped copy of the canonical replay** (scratch, not
  committed) so no `[fh]` capture could land in a profiled window while still getting a
  completion check. The input stream is byte-identical to the committed file's.
- **`fhdiff --known 1702` was never needed**; every comparison that should pass, passes
  strictly.

## Card staged for the user

`~/Desktop/G-Diffuser-3DS-native1/` refreshed from **build B** — `G-Diffuser-3DS.3dsx`
(sha256 `b0ff58cf…`), `G-Diffuser-3DS.elf` (`fff7ca1c…`), `SHA256SUMS` (verified with
`shasum -a 256 -c`), `TEST-PLAN.txt`. **The SD card was not touched.**

`TEST-PLAN.txt` spells out every `[native]` key and eight runs: **A** all levers off,
**B** `track`, **C** `machines`, **D** the fx levers, **E** all five Tier-2 levers
together, **F** the three verify modes for one lap (with both census keys and the exact log
lines to look for), **G** the owned interpreter through every screen by eye plus the tunnel
`cpuclip=1/0` A/B on a course that has a tunnel, **H** a 30-minute soak with `[stereo]
enabled=1`. `[net] enabled` is 0 throughout and the plan says so. Because of §5 the plan
tells the user up front that `interp` and the Tier-2 levers must not be on together and
that `all=1` is the one setting to avoid, run G is an `interp`-only run, and H is split into
H1 (Tier-2, the more likely shipping configuration) and H2 (interp).

## What is still not verified

- **Anything on hardware.** Every number here is Azahar. No native lever has ever run on a
  3DS.
- **Stereo.** `[stereo] enabled=1` was set on every parity run, but the emulator never
  activates the right-eye pass, so no `[c3d] … anchor=` / `eyeR=` receipt appeared in any
  run and no lever has been seen interacting with a stereo-anchored draw.
- **`cpuclip=0`.** The canonical replay's course has no tunnel.
- **The long tail.** Course editor, Expansion Kit screens, ending and records have no
  replay and were not exercised.
- **`interp` past tick 4100.** S3 stops there; the full-replay `interp`-only parity run
  (retire, results, records with the owned interpreter) has not been made. R4 shows those
  screens identical *with* the Tier-2 levers on, which is weaker evidence for the same
  thing.

---

# Profile on GPU renderer (b367f42) — 2026-09-17

§6 above profiled Q1/Q2/Q3 on `native/main@19c0fce9`, where **Q3 (`all=1`) was not drawing
the course** — its `vtx`/`tri` were measured on a frame missing geometry, so the Q3 columns
were never apples-to-apples. `native/interphooks` has since merged (`b367f42`), the owned
walker carries the track/fx marker and machines hooks, and its gate (a) proved
`interp=0 all=1` vs `interp=1 all=1` **pixel-identical on 6002 frames, 8/8 SHOTs
byte-identical** (`interphooks-progress.md`). That gate ran on the **software** renderer, so
no profile existed for the fixed stack. This is that profile, on the **Vulkan** renderer,
and it is the first `all=1 interp=1` measurement taken on a frame that draws the course.

## Headline — crowd bucket (`nD >= 100`), mean ms/frame

| config | `[native]` | total |
|---|---|---|
| (1) all off | all 0 | **56.03** |
| (2) Tier-2 | `all=1 interp=0` | **41.63** |
| (3) owned | `all=1 interp=1 cpuclip=1` | **30.21** |

(3) is **1.85x** (1) and **1.38x** (2) in the 30-car window — and unlike §6's Q3, on a
correct frame.

## Runs

Build `native/main@b367f42` (`GDX3DS_BUILD_ID` = `native/main@b367f420`), already built.
**`.3dsx` sha256 `bced3f34d5cd029df041a51894a759a7517cd78e94424b5c375eed20e1c9ed9a`**
(3,202,240 B), `.elf` `efe4271cb0e6868aaf6a81da9116a8c1b884ef96346b0e852dee088d877c9d9d`.

Lane: `tools/dev/gdx-dev retro run --gfx vulkan`, **one instance at a time** (each run's
driver was confirmed exited before the next launched), unique private instance per run.
**Renderer: Vulkan** — pinned, and it booted on **attempt 1 of 3 on all six runs**
(`manifest.attempts` = `[{attempt:1, gfx:'Vulkan', early_crash:false, ok:true}]`); no
OpenGL or software fallback was needed, so these numbers are directly comparable with §6's
RetroArch/Vulkan Q-runs and *not* with `interphooks-progress.md`'s software-renderer runs.

Ini, identical on every run bar the `[native]` lever set:
`[debug] det=1 gputrace=1 replay_quit=1 renderthread=0` plus gdx-dev's measurement set
(`console=1 filelog=1 filelog_max_kb=4096 verbose=1`). **`gputrace=1` on all six**
(top-level in every manifest; `prof --ab` refuses a mismatched pair) — **the millisecond
values below are inflated and are not comparable with a `gputrace=0` run.** The retro lane
strips `[debug]` and `[native]` from the seeded ini (33 keys dropped, logged per run) and
every `[native]` key was additionally passed explicitly.

Replay: the canonical `replays/race-mutecity-start.gdxrec` with its 8 `SHOT` trailers
stripped and nothing else changed — `sed -E 's/[[:space:]]+SHOT[[:space:]]+[A-Za-z0-9_]+[[:space:]]*$//'`,
6014 lines in and out, the button/stick stream byte-for-byte the committed file's. Staging
a `.gdxrec` that carries SHOTs arms `[debug] fh=1` (`tools/dev/gdxdev/replay.py:200-204`)
and a readback-stalled capture must never land in a profile window. **Zero `[fh] frame=`
lines in any of the six runs**, confirmed by grep.

| run | id | `[native]` | wall_s | guest frame | `[rec] end` | `[prof]` | errors |
|---|---|---|---|---|---|---|---|
| P1 | `20260917-151940-P1-alloff` | all 0 | 64.4 | 5982 | `tick=6001 lines=6001` | 93 | 0 |
| P2 | `20260917-152124-P2-tier2` | `all=1 interp=0` | 64.4 | 5910 | `tick=6001 lines=6001` | 93 | 0 |
| P3 | `20260917-152249-P3-all-interp` | `all=1 interp=1 cpuclip=1` | 62.4 | 5924 | `tick=6001 lines=6001` | 93 | 0 |
| P1b | `20260917-152526-P1b-alloff` | all 0 | 64.4 | 5984 | yes | 93 | 0 |
| P2b | `20260917-152631-P2b-tier2` | `all=1 interp=0` | 62.4 | 5910 | yes | 93 | 0 |
| P3b | `20260917-152736-P3b-all-interp` | `all=1 interp=1 cpuclip=1` | 62.4 | 5928 | yes | 93 | 0 |

Every run is a **completed replay**, not merely a live heartbeat: `RESULT PASS …
complete=True shots=0/0`, the `[rec] end tick=6001 lines=6001` receipt, and
`[oracle] quit requested: [rec] end with replay_quit=1`. Error census (`[fatal]`
`[fatal-trace]` `[gfxfail]` `[gdl-bad]` `[gdl-miss]` `[datafail]` `[texdiag]` `bad_alloc`):
**none, on all six.**

Boot receipts, verbatim:

```
P1  [native] all=0 interp=0 machines=0 track=0 fx_sink=0 fx_sprites=0 fx_hud=0 fx=0 census trk=0 fx=0 (resolved: all=tier2 only, interp explicit; ini latched at boot)
P1  [native] interp=0 path=lus cpuclip=1 blendflush=1 verify=0 hooks=lus
P2  [native] all=1 interp=0 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0 (resolved: ...)
P2  [native] interp=0 path=lus cpuclip=1 blendflush=1 verify=0 hooks=lus
P3  [native] all=1 interp=1 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0 (resolved: ...)
P3  [native] interp=1 path=owned cpuclip=1 blendflush=1 verify=0 hooks=noop(track,fx)+dl(machines)
```

`hooks=noop(track,fx)+dl(machines)` is the interphooks merge, and P3's machines receipt is
the proof it is load-bearing — §6's Q3 had **no `[native] machines=` receipt at all**
(the lever drew nothing under `interp=1`); P3 has one, with real draws:

```
P2  [native] machines=1 lists=150 baked=30 bakes=30/30 fails=0 draws=0    walk=0 runs=0     verts=0      tris=0      fb=0 pool=0/0
P3  [native] machines=1 lists=150 baked=30 bakes=30/30 fails=0 draws=1920 walk=0 runs=10447 verts=211472 tris=189632 cull=85123 fb=0 pool=0/0
P3  [ninterp] tasks=64 lists=441 (wide=64 n64=377) cmds=41420 rect=10813 tmem=5859 batches=331 unknown=0 dropped=0 runaway=0 ovf=0 rootrej=0
```

### Host load, and why it does not move these numbers

`uptime` at each launch (10 cores): P1 `4.68 11.84 18.28`, P2 `19.24 15.73 19.09`,
P3 `22.57 18.48 19.86`, P1b `12.93 19.32 20.21`, P2b `43.46 26.30 22.72`,
P3b `60.09 34.29 25.97`. `gdx-dev retro status` reported **no other active instance** at
any point and `ps` showed no other emulator; the load is the run's own RetroArch (heavily
threaded) plus ~3-4 of background desktop apps.

The repeat pass exists because of that spread, and it settles it: the `[prof]` section
timers are **guest-clock** section timers, and they reproduce to **within 0.05 ms** across
a pass taken at load 4.7 and a pass taken at load 60.

| crowd total | pass A | pass B | delta |
|---|---|---|---|
| (1) all off | 56.03 | 56.01 | 0.02 |
| (2) Tier-2 | 41.63 | 41.60 | 0.03 |
| (3) owned | 30.21 | 30.18 | 0.03 |

fps, which *is* derived from host wall time (`[watchdog]` beats / 5 s), reproduces too:
p10/p50 identical in all three pairs, p90 within 0.4. Guest frame counts likewise
(5982/5984, 5910/5910, 5924/5928). Every table below quotes **pass A**.

## Buckets and the race band

`prof --by-vtxn` buckets **each run independently** by that run's own `nD`, so the crowd
bucket is not a fixed set of frames: the Tier-2 levers cut draws/frame, and P2 lands 14
windows in `crowd` where P1 and P3 land 16. The crowd rows are therefore the campaign's
A/B convention but not frame-aligned. The **race band** — the fixed frame range
3226-4895, the replay's race entry to retire — is, at **26 windows / 1664 frames in all
three runs**, and it is the apples-to-apples row.

### Crowd bucket (`nD >= 100`) — mean ms/frame

| | wins | frames | br | dsp | vtx | tri | imp | drw | mtx | oth | **total** | nD/f | vtxN |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| (1) all off | 16 | 1024 | 8.52 | 21.96 | 5.47 | 14.15 | 1.46 | 3.55 | 0.11 | 0.81 | **56.03** | 154.7 | 3791 |
| (2) Tier-2 | 14 | 896 | 5.96 | 27.10 | 0.29 | 1.81 | 1.81 | 3.72 | 0.12 | 0.82 | **41.63** | 148.1 | 211 |
| (3) owned | 16 | 1024 | 0.03 | 20.02 | 0.52 | 2.78 | 1.81 | 3.65 | 0.54 | 0.85 | **30.21** | 153.7 | 0 |

### Race band (frames 3226-4895, 26 windows / 1664 frames each) — mean ms/frame

| | br | dsp | vtx | tri | imp | drw | mtx | oth | **total** | nD/f | vtxN |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (1) all off | 5.58 | 13.47 | 1.61 | 7.24 | 1.12 | 1.44 | 0.09 | 1.19 | **31.74** | 70.4 | 1007 |
| (2) Tier-2 | 4.85 | 13.96 | 0.74 | 5.41 | 1.16 | 1.21 | 0.09 | 0.89 | **28.30** | 54.7 | 566 |
| (3) owned | 0.03 | 10.61 | 1.01 | 6.26 | 1.04 | 1.48 | 0.39 | -0.15 | **20.67** | 70.3 | 0 |

### All four buckets, per config

```
(1) all off                                                                (2) Tier-2
bucket              wins frames  br   dsp    vtx  tri    imp  drw  mtx  oth   total    bucket              wins frames  br   dsp    vtx  tri    imp  drw  mtx  oth   total
menu (nD 1-15)      11   705    0.71  8.29  0.00  1.77  3.91 0.13 0.00 -0.90  13.91    menu (nD 1-15)      11   705   0.71  8.26  0.00  1.78  3.91 0.13 0.00 -0.85  13.94
light(nD 16-49)     60   3840   2.44 13.45  1.06 12.26  2.57 0.54 0.02  0.34  32.67    light(nD 16-49)     63   4032  2.42 13.44  0.89 11.55  2.48 0.54 0.02  0.18  31.52
mid  (nD 50-99)      6   384    6.90 15.78  1.91  7.00  1.11 1.67 0.12  1.35  35.85    mid  (nD 50-99)      5   320   6.33 17.34  0.73  4.07  1.27 1.80 0.13  1.30  32.96
crowd(nD 100-inf)   16   1024   8.52 21.96  5.47 14.15  1.46 3.55 0.11  0.81  56.03    crowd(nD 100-inf)   14   896   5.96 27.10  0.29  1.81  1.81 3.72 0.12  0.82  41.63

(3) owned (all=1 interp=1 cpuclip=1)
bucket              wins frames  br   dsp    vtx  tri    imp  drw  mtx  oth   total
menu (nD 1-15)      11   705    0.03  8.55  0.00  2.29  3.42 0.13 0.00 -1.88  12.53
light(nD 16-49)     60   3840   0.04 10.13  1.04 13.50  2.03 0.53 0.07 -0.56  26.78
mid  (nD 50-99)      6   384    0.03 11.59  1.03  5.10  1.09 1.72 0.52  0.81  21.89
crowd(nD 100-inf)   16   1024   0.03 20.02  0.52  2.78  1.81 3.65 0.54  0.85  30.21
```

### Draws per frame (`nD`, mean)

| bucket | (1) all off | (2) Tier-2 | (3) owned |
|---|---|---|---|
| crowd (`nD>=100`) | 154.7 | 148.1 | 153.7 |
| race band (3226-4895) | 70.4 | 54.7 | 70.3 |

The Tier-2 levers are the only configuration that **reduces** draws — the batcher folds
runs (70.4 → 54.7 in the race band, -22%). The owned interpreter does not batch further:
its win is that it never pays the bridge, not that it submits less.

## The three A/B tables

`tools/dev/gdx-dev prof --ab <A> <B> --by-vtxn`, mean ms/frame, delta = B − A,
**positive = A is faster**. Crowd bucket. `gputrace=1` on both sides of every pair.

### (1) vs (2) — `prof --ab P2-tier2 P1-alloff`

```
[prof] bucket: crowd (nD 100-inf)
section   (2) Tier-2   (1) all off    delta       pct
br              5.96          8.52    +2.57    +43.1%
dsp            27.10         21.96    -5.14    -19.0%
vtx             0.29          5.47    +5.18  +1763.7%
tri             1.81         14.15   +12.34   +681.9%
imp             1.81          1.46    -0.36    -19.7%
drw             3.72          3.55    -0.17     -4.4%
mtx             0.12          0.11    -0.01     -9.0%
oth             0.82          0.81    -0.01     -0.8%
total          41.63         56.03   +14.40    +34.6%

all windows    31.04         34.68    +3.64    +11.7%
mid  (50-99)   32.96         35.85    +2.88     +8.8%
light(16-49)   31.52         32.67    +1.15     +3.7%
menu (1-15)    13.94         13.91    -0.03     -0.2%

fps        p10   p50   p90
(2)       19.2  22.0  52.6
(1)       14.0  19.4  49.2
```

The shape §6 predicted, harder: `vtx` 5.47 → 0.29 and `tri` 14.15 → 1.81 in the crowd
window — the bake replays geometry the interpreter no longer transforms or assembles — and
`br` falls 8.52 → 5.96 because the bridge converts fewer commands. `dsp` **rises 5.14 ms**,
the cost moving into the marker replay and the retained walker; that is the trade, and it
is still a 14.40 ms net win. Menu windows are a wash (-0.03 ms), as they must be: no
Tier-2 lever is on the menu path.

### (2) vs (3) — `prof --ab P3-all-interp P2-tier2`

```
[prof] bucket: crowd (nD 100-inf)
section   (3) owned   (2) Tier-2     delta        pct
br             0.03         5.96     +5.92   +17546.6%
dsp           20.02        27.10     +7.08      +35.4%
vtx            0.52         0.29     -0.23      -43.9%
tri            2.78         1.81     -0.97      -34.8%
imp            1.81         1.81     -0.00       -0.0%
drw            3.65         3.72     +0.06       +1.7%
mtx            0.54         0.12     -0.41      -76.7%
oth            0.85         0.82     -0.03       -3.2%
total         30.21        41.63    +11.43      +37.8%

fps        p10   p50   p90
(3)       21.8  27.6  58.6
(2)       19.2  22.0  52.6
```

**This is the comparison §6 could not make.** Q3's `vtx`/`tri` flattered it because the
course was missing; P3 draws it (gate (a), and the `machines draws=1920` receipt above),
and the owned interpreter still wins 11.43 ms — now with `vtx`/`tri` moving the *wrong*
way (+0.23, +0.97 against it, the owned walker doing work the LUS bake had already
folded) and the entire win coming from `br` 5.96 → **0.03** and `dsp` 27.10 → 20.02.
`drw` is flat (+0.06) — both configurations submit the same draws to citro3d.

### (1) vs (3) — `prof --ab P3-all-interp P1-alloff`

```
[prof] bucket: crowd (nD 100-inf)
section   (3) owned   (1) all off     delta        pct
br             0.03          8.52     +8.49   +25155.6%
dsp           20.02         21.96     +1.94       +9.7%
vtx            0.52          5.47     +4.95     +944.6%
tri            2.78         14.15    +11.37     +409.7%
imp            1.81          1.46     -0.36      -19.7%
drw            3.65          3.55     -0.10       -2.9%
mtx            0.54          0.11     -0.42      -78.8%
oth            0.85          0.81     -0.03       -4.0%
total         30.21         56.03    +25.82      +85.5%

fps        p10   p50   p90
(3)       21.8  27.6  58.6
(1)       14.0  19.4  49.2
```

The full-stack number: **25.82 ms/frame off the crowd window, 1.85x**, and p50 fps
19.4 → 27.6 (+42%) over the whole run. `br` → 0.03 ms is the headline and the one figure
no caveat touches — the owned walker does not use the bridge at all.

## Reading notes and caveats

- **`gputrace=1` inflates every millisecond here.** It is on in all six manifests so the
  three configs are mutually comparable; none of these values is a frame-budget figure.
  Emulator ms are relative — hardware is truth, and no native lever has run on a 3DS.
- **`vtxN` is 0 for config (3) and is not a regression.** The counter is the LUS
  interpreter's loaded-vertex census; the owned walker reports its own totals in
  `[ninterp]` (which likewise shows `vtx=0 tri=0` — its geometry goes through the seam's
  `SpVertex`/`SpTri`, not the walker's own op counters). Do not read `vtxN` across
  `interp`. Config (2)'s 3791 → 211 collapse *is* meaningful: same interpreter, fewer
  vertices reaching it.
- **`oth` is a residual** (window total minus the named sections) and can go negative
  (config (3)'s menu bucket, -1.88; its race band, -0.15). It is not a section.
- **The crowd bucket is not frame-aligned across configs** (14/16/16 windows) because
  `--by-vtxn` buckets each run by its own `nD`. The race band is; both are given.
- **`cpuclip=0` is still unmeasured** — it is pinned to 1 in config (3) here, and the
  canonical replay's course has no tunnel, which is the case that lever exists for.
- Pixel parity for these configurations is **not** re-established by this profile
  (`fh=0` throughout, deliberately). It rests on interphooks gate (a): `interp=0 all=1` vs
  `interp=1 all=1`, 6002 frames divergent=0, 8/8 SHOTs byte-identical — on the software
  renderer. **A GPU-renderer parity run for `all=1 interp=1` has not been made.**
