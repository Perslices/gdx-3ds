# fx — progress (effects sink, HUD/2D batcher, integration scaffolding)

Branch `native/fx`, worktree `~/code/gdx-3ds/native-fx` (the path `~/code/gdx-3ds/fx` was already
taken by an old `feat/3ds-fx` worktree that the auto-mode classifier refused to remove, so the
bootstrap was reconstructed by hand at `native-fx`; run directories use `--track fx`). Plan:
`09-fx-integration-plan.md`. Protocol: `<scratch>/native/PROTOCOL.md` (absent from the scratchpad
at session start — the commit trailers and the `native/<track>:` subject prefix were taken from
the sibling tracks' commits). Killswitches `[native] fx_sink` (0 off / 1 native / 2 verify),
`[native] fx_sprites` (fold), `[native] fx_hud`; census `[native] fxcensus=1`; receipts `[fx]`.

## State

- M0 census: DONE in the build (`[fx] cam=…` sampled every 64th Racer_Draw + `[fx] agg` run-wide
  max/total per class); numbers below.
- M1/M2 effects sink + per-class native batches: DONE (f084e44), decomp patch
  `decomp-native-fx-shim.patch` (14th, pure delta), marker replay via the shared
  `gdx3ds_native_tri.cpp`; fold lever `fx_sprites`.
- M3 HUD/2D batcher: see "HUD" below.
- M5 integration scaffolding: DONE (f084e44): `[native]` reader with `all`, boot receipt, DBG rows
  22/24, docs §5.
- Parity: see "Runs".
- Profile: see "Runs".

## Design as built (deviations from 09-fx-integration-plan.md, with reasons)

1. **No PICA-layout ring; the game's `effectsVtxBuffer` stays authoritative and the interpreter's
   own `GfxSpVertex` transforms it at the marker.** Plan §3.1 wanted `gEffectsVtxPtr` retargeted
   at a linearAlloc ring in GPU layout. The stores are world-space integers; the clip-space
   transform needs the interpreter's MP matrix, which only exists on the render thread when the
   list is walked (renderthread=1), so a game-thread PICA ring would have had to reproduce the
   interpreter's transform (the exact parity trap the track plan abandoned, track deviation 2).
   The record therefore holds `Vtx*` pointers (like the track's `courseVtxBuffer` pointers) and
   the vertex walk happens once, in the interpreter, at the marker. Plan risk 4 (ring aliasing,
   `course.c:4545`) disappears with it.
2. **Bracket-scoped shim (push_macro/pop_macro), not a whole-file macro override.** course.c's
   shim redefines the GBI for the whole file; racer.c has ~500 macro sites outside the effects
   loops (machines, shadows, markers, countdown) that must stay byte-identical AND zero-cost, so
   the patch includes `gdx3ds_fx_shim_begin.h` / `_end.h` (preprocessor-only) inside
   `Racer_Draw` around the six ring classes. Outside the bracket the stock macros compile.
3. **Runs, not classes, are the draw grain; the per-class draw count is what the receipts
   report.** The class setup lists and the explosion frame `gDPLoadTextureBlock`s stay real GBI
   (the bridge translates raw texture pointers; replaying them from the interpreter side would
   need the bridge's `TranslateDataPointer`), each preceded by a marker. Debris, flying sparks
   and the pit force field are one run (one draw) per class. Collision sparks, smoke and fall
   explosions split on the per-sprite `gSPTexture` LOD tile (a tile change re-resolves the
   texture through `GfxSpTri1`, the exact path). Explosions split per sprite on the frame load
   (the plan's one-atlas-page frames are the follow-up: 8 frames of 32x32 IA8 = one 32x256
   strip; parity needs row 0 identical across frames because the s=31.97 edge sample wraps into
   the next row under REPEAT — checkable at build time, not built yet).
4. **prim/env fold is decided from the backend's stage-constant table, not from the combiner
   alone.** The citro3d backend routes exactly one combiner input per channel through the vertex
   colour attribute (`prg.vtxColorInput/vtxAlphaInput`) and binds every other input as a TexEnv
   stage constant from vertex 0 — which is why the stock `GfxDpSetPrimColor/EnvColor` flush on a
   value change. `Gdx_NativeBatchConstInputs()` (new, gfx_citro3d.cpp) reports the inputs that
   are stage constants; a prim (env) change folds iff no constant reads PRIM (ENV) in either
   channel and the alpha channel really is per-vertex (`wantVtxAlpha && !fogFactorToAlpha`).
   The fast path already evaluates the register per vertex at append time, so a folded batch's
   VBO bytes equal the stock path's; the split reasons are counted (`split=p/e/t`).
5. **Verify mode snapshots the registers too.** The track's verify only snapshots the vertex
   cache; an effects run mutates prim/env/prim_lod/first_tile/texture_scaling as well, so the
   START marker snapshots those and END restores them before the replay.
6. **The shared tri replay was extracted, not copied.** `gdx3ds_native_tri.{h,cpp}` holds the
   track's `DerivePrep/FastTri/ExactTri/Tri/FlushNative` verbatim; the track replay now calls
   them. `[native] track=2` verify covers the refactor.
7. **`[native] all=1` is applied through one reader** (`gdx3ds_native_cfg.c`): a lever that is
   0/absent reads as 1, an explicit 2 (verify) is kept. Track and machines readers were routed
   through it (one line each); interp adopts it when it merges.

## Runs (RetroArch + Azahar libretro lane, `gdx-dev retro run`, private instances fx/fx2/fx3)

Every run: build `native/fx@04ea5a80`+fh_start (22c616c), replay
`replays/race-mutecity-start.gdxrec` (6001 ticks: title → menus → 30-machine GP Mute City race
from tick 3226, retire/results at 4895), `[debug] det=1 platform=azahar renderthread=0`,
`[stereo] enabled=1` (slider 0 on the emulator), `[native] track=1 machines=1 interp=0` as the
baseline the coordinator asked for, `fxcensus=1`. Instance inis normalised to one canonical file
(see gotchas: the seeded inis had drifted, `trectbatch=0` on one of them).

### M0 census — this replay (`[fx] agg` at the last sampled window, run par8-sink1, 1473
Racer_Draw calls = attract demo + menus + the race band)

| class | max sprites / call | max stock draws / call | total sprites | total stock draws |
|---|---|---|---|---|
| explosions | 5 | 5 | 640 | 640 |
| machine debris | 60 | 1 | 3840 | 64 |
| flying sparks | 32 | 1 | 4864 | 212 |
| collision sparks | 4 | 4 | 746 | 746 |
| smoke | 2 | 2 | 138 | 138 |
| pit force field / fall explosions | 0 | 0 | 0 | 0 |

"stock draws" = state-change groups under the interpreter's split-on-value-change rule (the
sink counts them with `fx_sink=0`). The shape the plan predicted holds: debris and flying
sparks are already one draw per class on the stock path; explosions, collision sparks and
smoke are one draw per sprite (frame load / prim+env / LOD tile per sprite). This replay's
race band is effect-light (the player parks and retires); the attract demo (4P split screen,
30 AI machines) supplies most of the sprites. Max over the run: 7 stock draw groups in one
bracket.

## Gotchas

- `gdx-dev retro run --replay <file.gdxrec>`: the guest under the RetroArch lane has the SD ROOT
  as cwd (the .3dsx is loaded from the build dir), so `rec/<name>.gdxrec` must be at
  `<sdmc>/rec/`; devcli staged it under `3ds/gdiffuser/rec/` only → `[rec] replay open failed`
  and an attract-demo run that looks healthy. Fixed in `tools/dev/gdxdev/replay.py` (stages both).
- **The RetroArch/Azahar-libretro core crashes (host `Unhandled EXC_BAD_ACCESS`, garbage pc) under
  dense frame capture**: every `fh_every=1` run (4/4) and 5 of 6 `fh_every=2` runs died within
  ~200 guest frames, with the replay, with an autoinput script and with no input at all, under
  Vulkan and OpenGL alike, with `fx_sink` 0 or 1 (the baseline died too, so it is the lane, not
  the sink). `fh_every=0` runs never died (5/5). The crash follows the FIRST captures wherever
  they start: with `[debug] fh_start=300` (added for this) four concurrent `fh_every=2` runs all
  died at frame ~252-300, i.e. within a few captures; runs that survive their first ~50
  captures run to the end (`fh_every=2`: 5144 frames; `fh_every=8`: 3 of 5 whole replays). The
  probability rises with concurrent instances (three of mine plus other agents' were on the
  GPU). Parity captures are therefore taken ONE instance at a time with a retry loop
  (`<scratch>/fx/retry.sh`: a crashed attempt costs ~1 min) at `fh_every=2 fh_start=300`;
  verify (`fx_sink=2`) and profile runs do not capture and are unaffected. The Qt lane
  (oracle, every frame) is the strict alternative when its lock is free (it was not: another
  agent held it all session). Speed: `fh_every=0` ~126 guest fps, `fh_every=8` ~40-100 fps,
  `fh_every=1` ~1 fps with two instances.
- `[debug] replay_quit=1` ends the run through `exit()`, which prints `[fatal-trace] atexit:
  process exiting via exit()` — gdx-dev counts it as a `[fatal]` line and marks the run FAIL.
  Read the verdict detail, not the word.
- `[c3d]` (and its `anchor=` suffix) only prints with `[debug] gputrace=1`; parity runs
  (gputrace=0) have none. The suffix also needs the stereo pass active on the emulator.
- Two runs on the same instance back to back: the first harness's teardown kills the second
  run's RetroArch (~10 s in, guest log just stops, verdict "PASS heartbeat reached frame 134").
  Wait for the VERDICT file before reusing an instance.
- The run dir's guest log is `log-<instance>.txt`, not `log.txt`.
- zsh: pass `gdx-dev` argument lists as arrays (`"${ARR[@]}"`); `$VAR` does not word-split.
- `tools/ci-3ds.sh` after adding a patch to PATCH_LIST: the whole-stack gate refuses (N applied,
  N+1 listed); `git -C decomp checkout -- .` then re-run (oracle gotcha).

### Parity — `[fh]` hashes, replay `race-mutecity-start`, `fh_every=2 fh_start=300` (2860 hashed
frames of 6002: every second frame from 300, boot/title excluded), one instance at a time

| A | B | compared | divergent | note |
|---|---|---|---|---|
| fx_sink=0 (par2-sink0, 110053) | fx_sink=1 (par2-sink1, 111105) | 2860 | **0** | identical, 1702 matched too |
| fx_sink=0 (par2-sink0) | fx_sink=1 fx_sprites=1 fx_hud=1 (par2-all, 105035) | 2860 | 1 + known 1702 | frame 1418 = the first frame of course select (mode 7→10), the same first-frame-of-a-screen shape as the oracle's 1702 texrect transient; attributed below by the single-lever legs |

### Verify — in-process VBO-byte proof (`fx_sink=2`, fx_sprites=1, fx_hud=1), no capture

| run | scene | sampled `vok/vbad` (every 64th Racer_Draw) | `[fx] verify MISMATCH` | errors |
|---|---|---|---|---|
| verify-demo 105023 / verify-rep 105539 | the replay (the seeded ini still carried input_replay) | 29/0 each | 0 | 0 |
| demo-verify 110428 | attract demo, 32,574 frames, 43,713 Racer_Draw calls | 120/0 | 0 | 0 |

### Profile — replay, `gputrace=1`, `fh_every=0`, 93 [prof] windows each, one instance (fx3),
sequential (prof-base 105023, prof-fx 105440 = sink+sprites+hud, prof-hud 105857 = hud only)

Whole run (`gdx-dev prof --ab`): total 31.22 → 31.08 (fx) / 31.12 (hud) ms/frame, `drw` 1.10 →
1.01 both; every other bucket within ±0.03. Race band only (frames 3227-4895, 26 windows):

| | nD draws/frame | drw ms | br | tri | imp | sum of buckets |
|---|---|---|---|---|---|---|
| base | 70.4 | 1.45 | 5.27 | 5.40 | 1.16 | 27.77 |
| fx (sink+sprites+hud) | 54.1 | 1.19 | 5.21 | 5.38 | 1.16 | 27.37 |
| hud only | 54.1 | 1.19 | 5.32 | 5.39 | 1.16 | 27.54 |

The 16 draws/frame come from the `fx_hud` fold (HUD/marker rects with per-rect prim colour);
the effects sink is a wash on this replay: its race band has almost no sprites in the sampled
windows (the player parks and retires) and the sink's per-run draws equal the stock groups
where prim+env both vary (explosions: `fold=p3 split=e3`). Emulator+JIT numbers, gputrace on:
only the deltas mean anything; hardware is the truth (card staged).

### Frame 1418 (fx_hud) — found, attributed, fixed

The first `fx_hud=1` capture legs differed from `fx_hud=0` on exactly one frame, 1418 (the first
frame of course select), deterministically (two runs each: hud0 `ce47ea31`, hud1 `bf92d6e0`;
1416/1420 identical). `replays/probe1418.gdxrec` (the canonical replay + `SHOT f1416/f1418/f1420`)
gave BMPs: `framediff` hud0 vs hud1 → 1252 px, max delta 173, bbox (121,200)-(278,221) = the
course-select text box; hud0 vs hud0b and hud1 vs hud1b identical. Cause: with the fold the batch
was non-empty at the glyph rects' texture change, so the import took the `[trectbatch]` deferred
view-merge path, which the oracle already showed is not pixel-transparent; on the stock path the
prim flush had emptied the batch and the import ran plain. Fix (5a9b668): a batch that absorbed
a fold never takes the deferred import (`sGdxPackFolded`), so the split moves from the colour
change to the texture change and the import path stays the stock one. Re-probed on the merged
build (native/fx@8bce9948): hud0 / hud1 / all → `ce47ea31`, all three f1418 BMPs identical.

## After the native/main merge (fce2dd1: machines + interp) — build native/fx@8bce9948

CI gate `tools/ci-3ds.sh`: PASS (14 decomp patches incl. decomp-native-fx-shim, interp host
tests, machines harness, 3dsx 3.3 MB). Interp's lever now reads through the `all` master too.

### Verify, whole run (`[fx] agg2`, in-process VBO-byte proof, no capture)

- demo-verify 162201 (attract demo, 29,713 frames, 40,577 Racer_Draw calls, fx_sink=2
  fx_sprites=1 fx_hud=1): **markers=3732 vok=3732 vbad=0**, `verify MISMATCH`=0, fb=0; 9582 tris
  replayed (3588 fast / 6127 exact — the exact share is the per-run first tri + the clipper on
  4 viewports), split=t892 (LOD tile changes: collision sparks / pit force field), fold 0
  (those classes' prim/env do not change inside a run). Stock draw groups 4633 vs the
  brackets' native draws (discarded in verify).
- the interpreter `fx_hud` fold over that run: 228,921 prim / 33,716 env changes absorbed.

### Profile, replay, `gputrace=1`, `fh_every=0`, 93 frame-aligned [prof] windows each, one
instance sequential (prof-base 161342 / prof-fx 161759 = sink+sprites+hud / prof-hud 162216)

Whole run: total 31.37 → 31.27 (fx) / 31.32 (hud); `drw` 1.10 → 1.03 both; other buckets ±0.02.
Race band (frames 3227-4895, 26 windows): nD 70.4 → 54.7 draws/frame (both fx and hud), drw
1.46 → 1.20 ms, bucket sum 27.22 → 26.87 (fx) / 26.95 (hud). Whole-run native draws 1757 vs
1800 stock groups (`[fx] agg2`): the sink removes 43 draws over the replay — explosions still
split per sprite on the env register (`fold=p3 split=e3`), collision sparks/smoke on the LOD
tile; the per-frame maximum was 7 native draws for the bracket.

### Profile, attract demo (no replay; det=1 gives the same timeline: both runs 5518 stock
draw groups), `gputrace=1`, 193 frame-aligned windows, one instance sequential
(demo-prof-base 162633 / demo-prof-fx 163008 = sink+sprites+hud)

total 26.72 → 26.56 ms/frame (−0.6 %), `drw` 1.01 → 0.90 (−11 %), br/dsp/tri/imp/vtx within
±0.03; fps p50 35.6 → 37.0. Native draws 5464 vs 5518 stock groups; fold 142,629 prim / 6,118
env in the interpreter batches. Emulator+JIT with gputrace on: deltas only; hardware pending.

## Final build — native/fx@9a62855a (native/main b7d4907 merged: machines + interp + residuals,
15 decomp patches, CI gate PASS 2026-09-17)

### Profile (`gputrace=1`, replay, 93 frame-aligned [prof] windows, fx3 sequential:
prof4-base 100917 / prof4-fx 101334 = sink+sprites+hud / prof4-hud 101750)

Whole run: total 31.40 → 31.28 (fx) / 31.31 (hud) ms/frame; `drw` 1.10 → 1.03 both; other
buckets ±0.05. Race band (frames 3227-4895, 26 windows): nD 70.5 → 54.8 draws/frame (fx and
hud alike), drw 1.46 → 1.20 ms, bucket sum 27.28 → 26.90 (fx) / 26.94 (hud). Native draws 1757
vs 1800 stock groups over the replay; max 7 native draws per bracket.
Attract demo (demo4-base 102205 / demo4-fx 102539, det=1 → both 8914 stock draw groups, 379/381
windows, frame-aligned): total 26.09 → 25.96 (−0.5 %), drw 0.90 → 0.81 (−11 %), br/dsp/tri/imp
±0.02; native draws 8828 vs 8914; fold 216,357 prim / 8,910 env in the interpreter batches.
(devcli's new early-crash retry relaunched prof4-base once: console-1 crashed at boot, console-2
ran the whole replay.)

### Parity — final build native/fx@9a62855a, `[fh]` hashes, replay race-mutecity-start,
`fh_every=2 fh_start=300` (2860 hashed frames of 6002), `[native] track=1 machines=1 interp=0`
baseline, one instance (fx2) sequential with `<scratch>/fx/retry.sh` (boot-residual and
crash attempts rejected by the mode-7@46x / `[rec] end` / lever-line / log-mtime checks)

| leg (vs par4-sink0 100937) | run | compared | divergent | `[fx] agg2` |
|---|---|---|---|---|
| fx_sink=1 | par4-sink1 102033 | 2860 | **0** | native 1757 / stock 1800, fb=0 |
| fx_sink=1 fx_sprites=1 | par4-spr1 103103 | 2860 | **0** | fold p459, split e459/t346, fb=0 |
| fx_hud=1 | par4-hud1 104132 | 2860 | **0** | interpreter fold p54130/e2182 |
| fx_sink=1 fx_sprites=1 fx_hud=1 | par4-all 105202 | 2860 | **0** | native 1757, fold p51883/e1969 |

`fhdiff --known 1702` on every leg: divergent=0, known_transient=0 (1702 matched in every pair
on this build). Zero `[fx] marker with bad record` / gfxfail / gdl-bad lines in any run.

## What awaits hardware

- The card at `~/Desktop/G-Diffuser-3DS-fx/` (TEST-PLAN.txt A-F): visual A/B of the effect
  classes, the in-process verify lap (`fx_sink=2`), the stereo-anchor check (`anchor=` needs
  the 3D slider and the stereo pass, which the emulator lane never activates: no `eyeR=`/`anchor=`
  suffix appeared in any [c3d] line here), the night-course star A/B, the `[prof]` A/B with
  gputrace on and the fps run with it off. Every ms above is emulator+JIT.
- The plan's final gate (locked 60, stereo, FULL rival detail, 30-min soak) is a hardware run.

## Not built / deviations to note

- Explosion frame atlas (plan §3.1): not built. Both prim and env vary per explosion sprite
  and the packed layout carries one colour per vertex, so an atlas would not merge them either
  (`split=e`); the remaining win there is the per-sprite frame import, which needs a wrap-safe
  8-frame strip (row 0 identical across frames) and the unexercised `Gdx_NativeBatchMaterial`.
- Shadows as instanced quads (plan §3.1/3.4): not built (they are per-racer matrix draws, not in
  the six ring classes the brief named); attack highlight untouched (machines track).
- fx_hud is the generic fold rule inside the interpreter's own batches, not a separate native
  texrect path: it removes the per-prim-colour draw splits (stars, HUD digits, markers) and the
  prim-depth split stays, but the per-rect import lookup residue the plan targeted is untouched
  (`imp` unchanged in every A/B).
- Transitions, sky: untouched (plan §3.3, brief).
- `tools/dev/gdxdev/replay.py`: my root-copy block is now a no-op after devcli's absolute-path
  fix (same destination); left in place, harmless.
