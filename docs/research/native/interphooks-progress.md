# interphooks — the owned interpreter carries the Tier-2 hooks (progress)

Branch `native/interphooks`, worktree `~/code/gdx-3ds/interphooks`, off `native/main@21566ff`.
Brief: close integration-verify-2026-09-17 §5 — `[native] interp=1` dropped the track/fx
markers (no course), `machines` was inert under `interp=1`, `all=1` implied `interp=1`, and
`fx_sink=2` without `fxcensus=1` was a silent pass.

## State

| item | status | commit |
|---|---|---|
| 1. owned walker replays the track/fx markers (OP_NOOP → seam → the shared replays) | DONE, gate (a)(b)(c) PASS | `ab84166` |
| 2. machines under interp=1 (RegisterDlHook wired, lazy bake with the walker's resolver) | DONE, gate (a)(b) PASS | `4e9da35`, `4d26ee7`, `0b305a1` |
| 3. `all=1` = Tier-2 levers only; `interp` explicit; receipt prints the resolved state | DONE | `9d98faf` |
| 4. `fx_sink=2` coverage receipt unconditional in verify mode | DONE | `262ece7` |
| docs: 3DS-HARDWARE.md §5, STATUS.md | DONE | `7e5daf2` |
| gate (a) `interp=0 all=1` vs `interp=1 all=1` → divergent=0 | **PASS** (6002/6002, 8/8 SHOTs byte-identical) | |
| gate (b) `interp=1 track=2 machines_verify=1 fx_sink=2` → 0 mismatch, nonzero coverage ×3 | **PASS** (trk 70/70, fx 1631/1631, machines 27,213 draws / 1,173,396 tris) | |
| gate (c) race screenshot by eye | **PASS** (road, rails, boost pads, gantry, HUD all present) | |
| `tools/ci-3ds.sh --jobs 8` PASS | **PASS** at `0b305a1` | |

## Design: one replay body, two interpreters (the seam)

The track and fx marker replays and the shared exact/fast triangle path
(`gdx3ds_native_tri.cpp`) were written against `Fast::Interpreter` internals: `GfxSpVertex`
/ `GfxSpTri1`, `mRsp->loaded_vertices`, the RDP registers, the `[triloop]` packed-batch
latches and the `[perf-s7]` tri memo (`GdxTrackPackState`). The machine-mesh draw
(`gdx3ds_mesh.cpp`) went further: `BindMaterial` re-ran GfxSpTri1's state block against the
LUS `RenderingState` and `LatchVtxState` read the LUS frame-buffer/widescreen latches.

Duplicating any of that for `gdxn::Interp` would have meant a second copy of verified
bit-identical code. Instead every consumer now runs against `GdxInterpSeam`
(`port/3ds/gfx/gdx3ds_interp_seam.h`, LUS-free):

- **live register views** (`GdxSeamRegs`): raw pointers into the interpreter's own RSP/RDP
  mirrors — vertex cache, geometry/extra mode, fog, texture scale, prim/env/fog/blend/key
  colours, convert_k, lod fraction, first tile, textures_changed, othermodes, combine mode,
  prim depth, viewport flag. Both interpreters keep these "shaped like the LUS structs"
  (06-interp-plan §3.4); the adapters `static_assert` the `LoadedVertex`/`RGBA` layouts.
- **actions** (virtuals): `SpVertex(n, slot, hostVtx*)`, `SpModifyVertex`, `SpTexture`,
  `SetCombineRaw(w0,w1)`, `SetPrimColor`/`SetEnvColor` (the value-change-flush handlers),
  `Flush`, `PendingTris`, `SpTri` + `PopAppended` (batch adoption), `PackState`,
  `RejectVariant`, `TileUv`, and for machines `BindMaterial`, `LatchVtxState`,
  `ApplyDrawConstants`.
- **entry points**: `gdx3ds_track_marker_seam`, `gdx3ds_fx_marker_seam`,
  `gdx3ds_mesh_dl_hook_seam(seam, listPtr, resolver, user)`.

Adapters:

| | `gdx3ds_seam_lus.cpp` (Fast::Interpreter) | `n64/gdx_seam_owned.cpp` (gdxn::Interp) |
|---|---|---|
| SpVertex | `GfxSpVertex` | `TransformVertices` (SpVertex minus the operand resolve) |
| SpTri / adoption | `GfxSpTri1`; pop `mBufVboNumTris`/`mBufVboLen` | `SpTri`; pop `mBufNumTris` (the packed cursor is the pool's) |
| SetCombineRaw | `GdxTrackSetCombine` (RDP handler + s7 dirty mask) | `SetCombineWords` (the OP_SETCOMBINE decode) |
| PackState | `Interpreter::GdxTrackPackState` (s7 memo + `sGdxPack*`) | `mPack*` + `mTriMemo` (banked by `ResolveDrawState`, dropped at `Flush`) |
| BindMaterial | the former `gdx3ds_mesh.cpp` body, verbatim | `ResolveDrawState(false, ds)` — SpTri's state block factored out |
| LatchVtxState | the former body (LUS fb/widescreen latches) | mirrors owned `SpVertex` (`mCfg` widescreen/forceFixedAspect, `mVariant==2` FLX alpha) |
| who calls it | vendored `gfx_noop_handler_f3dex2` → `gdx3ds_track_marker`/`gdx3ds_fx_marker`; `gfx_dl_handler_common` → `gdx3ds_mesh_dl_hook` | `gdx_rt_glue.cpp` registers `OwnedNoopHook` (`RegisterNoopHook`, new) and `OwnedDlHook` (`RegisterDlHook`) on the Interp |

The owned walker's `OP_NOOP`/`OP_SPNOOP` (both bound to the same LUS handler in the F3DEX2
table) hand `c.w0, c.w1` to the hook; the hook dispatches `p=0x5A/0x5B` → track and
`0x5C/0x5D` → fx, the same arms and arguments as the vendored handler. Flush semantics are
the replays' own (unchanged): START in verify flushes the interpreter batch and arms the
stock hash; END flushes, restores the snapshot, replays into a `Gdx_NativeBatch`, discards
+ compares; in native mode END flushes only if tris are pending so the native batch starts
at the pool cursor. `Gdx_NativeBatchFlush` submits through `DrawPackedBatch` on the same
citro3d renderer the owned interpreter's `Flush` drives (`BackendC3D`).

Machines under the owned walker: there is no bridge conversion, so the G_DL hook meets the
game's own `D_800CDDB0[]` list pointer. `gdx3ds_mesh_dl_hook_seam` accepts either a mesh
stub (the bridge's redirect, LUS path) or a registered source pointer; the latter bakes
lazily with the resolver the caller passes — the owned one is `DlSrc::ResolveData` with
`preferPhysical` and full-payload readability, the walker's own G_VTX rules
(`GdxOwnedSeamResolveVtx`).

Owned-interpreter refactors (host harness `gdx_interp_host_tests` still ALL PASS):
`Interp::ResolveDrawState` (SpTri's state block; SpTri binds the same locals by reference
so the emission code is untouched), `TransformVertices`, `SetCombineWords`, `PendingTris`,
`RegisterNoopHook`, `FLUSH_HOOK`, the tri memo (`mTriMemo`/`mTriMemoValid`).

Vendored edit: `Interpreter::GdxTrackPackState` fills the neutral struct (`inputMap` +
`GdxSeamClip`); `MANIFEST.sha256` refreshed with `tools/vendor-lus.sh --manifest`.

## Config

`gdx3ds_native_lever` applies `all` only to `track, machines, fx_sink, fx_sprites, fx_hud`
(`gdx3ds_native_is_tier2`); `interp` is read as written. Boot receipt:
`[native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx= census trk= fx=
(resolved: all=tier2 only, interp explicit; ini latched at boot)`; the path line gained
`hooks=noop(track,fx)+dl(machines)` under `interp=1`.

## Known gaps

- `fx_hud` (the prim/env fold inside LUS's own 2D batches, `interpreter.cpp:709-712`) has no
  owned-walker equivalent: under `interp=1` it is inert (more draws, same pixels). Not part
  of this brief; noted so `interp=1 all=1` is read correctly — `fx=7` in the receipt means
  the lever is on, not that the owned 2D batcher folds.
- `interp_verify=1` (both interpreters on one job) with the Tier-2 levers on has not been
  exercised: both passes would replay the same marker into the backend's one
  `Gdx_NativeBatch` state; each pass opens and closes its own batches so it should be sound,
  but it is unmeasured.

## Evidence (2026-09-17, RetroArch lane, **software renderer**)

Build under test: `native/interphooks@262ece76`, `.3dsx` sha256
`4b697a81e2a8546b823fb28b23cbee79d2a9f0aa26da032044fc52704e261f78` (3,202,184 B), `.elf`
`1e067254ec6d6faf605cd6649e760198a8e6f864d48fbd3b4375847b95377482`. The two commits after
it (`4d26ee7` LUS-hook early return, `0b305a1` weak `D_800CDDB0` for the harness link) touch
no code the gate exercises on either path (`ci-3ds.sh` at `0b305a1`: `.3dsx` sha256
`0ce1bc4b0f7cd769bda7808bd14c7490be566048a353214e448b41ae30406806`, RESULT PASS).

Lane: `tools/dev/gdx-dev retro run`, one private instance at a time, canonical replay
`replays/race-mutecity-start.gdxrec` (6001 ticks, 8 SHOTs), `[debug] det=1 fh=1 fh_every=1
fh_start=0 renderthread=0 replay_quit=1 gputrace=0 platform=azahar`, `[stereo] enabled=1`,
`[net] enabled=0`, every `[native]` key passed explicitly. **Renderer: Software** — the Vulkan
and OpenGL cores host-crashed at guest frame ~39 on every boot today (`Unhandled
EXC_BAD_ACCESS` in RetroArch, lever-independent, machine load 20-60 from other agents'
processes), so `--gfx software` was pinned and boot retried until the heartbeat advanced.
Consequences: (1) wall time is not comparable to earlier RetroArch/Qt runs and no profile
was taken; (2) the frames are not comparable across lanes — `fhdiff` of today's runs against
the 2026-09-17 Azahar-Qt baseline S1 diverges from frame 1702 on **both** the LUS and the
owned path identically (`divergent=4275` for A and for B alike, frame 351 = `4ee0d16f` on
every run, the oracle constant), which is the renderer, not the code. Parity is same-lane,
same-renderer here, as PROTOCOL requires.

| run | id | `[native]` | wall_s | `[fh]` | SHOTs | `[rec] end` |
|---|---|---|---|---|---|---|
| A | `20260917-142616-A-lus-all` | `all=1 interp=0` (→ `machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1`, `path=lus`) | 1833.0 (deadline 1800 s; replay done at ~4 min) | 6002 | 8/8 | yes |
| B | `20260917-145809-B-owned-all` | `all=1 interp=1` (`path=owned hooks=noop(track,fx)+dl(machines)`) | 430.1 (deadline 420 s) | 6002 | 8/8 | yes |
| C | `20260917-150635-C-owned-verify` | `interp=1 machines=1 machines_verify=1 track=2 fx_sink=2 fx_sprites=1 fx_hud=1`, censuses 0 | 434.2 | 6002 | 8/8 | yes |

Boot receipts, verbatim:

```
A  [native] all=1 interp=0 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0 (resolved: all=tier2 only, interp explicit; ini latched at boot)
A  [native] interp=0 path=lus cpuclip=1 blendflush=1 verify=0 hooks=lus
B  [native] all=1 interp=1 machines=1 track=1 fx_sink=1 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0 (resolved: ...)
B  [native] interp=1 path=owned cpuclip=1 blendflush=1 verify=0 hooks=noop(track,fx)+dl(machines)
C  [native] all=0 interp=1 machines=1 track=2 fx_sink=2 fx_sprites=1 fx_hud=1 fx=7 census trk=0 fx=0 (resolved: ...)
C  [trk] init mode=2 census=0 ...   [fx] init mode=2 fold=1 census=0 ...   [native] machines=1 walk_only=0 verify=1
```

Item 3 is visible in A's receipt: `all=1 interp=0` resolves to `interp=0` (before this
branch it printed `interp=1`).

### (a) `interp=0 all=1` vs `interp=1 all=1`

```
fhdiff A B: compared=6002 divergent=0 known_transient=0 only_in_a=0 only_in_b=0 first_divergent=none last_common=6002
cmp shots: title mainmenu machsel race00 race01 race02 race03 race04 — all 8 SAME (byte-identical)
```

No `--known` needed. The 859/1537 in-race divergences of the 2026-09-17 report (first at
frame 3237, the race start) are gone. Both paths drew the machines natively: the `[native]
machines=1` windows sum to **27,213 draws in A and 27,213 in B**, peak window
`draws=1920 runs=10447 verts=211472 tris=189632 fails=0 fb=0 pool=0/0` in both; B's `[ninterp]`
windows report `unknown=0 dropped=0 runaway=0 rootrej=0`. Zero `marker with bad record`,
`gfxfail`, `bake FAIL` or `[fatal]` lines in either run.

### (b) the three verify modes under `interp=1`

Every coverage receipt printed with both census keys **off** (item 4 for fx; 28f588e for
track; machines always did):

| lever | coverage (sum over the 64-frame windows) | mismatches |
|---|---|---|
| `track=2` | 24 `[trk] replay(prev)` windows: **markers=70, vok=70, vbad=0, fb=0** | `[trk] verify MISMATCH` × **0** |
| `machines_verify=1` | 36 `[native] verify` windows: **draws=27,213, tris=1,173,396 compared bit-exact**, `triMismatch=0 countMismatch=0 matMismatch=0 auxDiff=0` | `[native] verify MISMATCH` × **0** |
| `fx_sink=2` | `[fx] agg2 stockdraws=1800 nativedraws=0 maxnative=0 markers=1631 tris=11744 fast=4907 exact=2806 fb=0 fold=p459/e0 split=p0/e459/t346 vok=1631 vbad=0` | `[fx] verify MISMATCH` × **0** |

(`nativedraws=0` / `draws=0` are correct in verify mode: hashed and discarded.) The
machines total matches the native draw count of (a) exactly (27,213), and the track/fx
totals match the 2026-09-17 LUS-path R3 figures to within the replay's effect-count noise
(R3: 70/70 track markers, 1,643 fx markers, 26,346 machine draws on the Qt lane). Verify
mode is pixel-clean too: `fhdiff A C: compared=6002 divergent=0`, all 8 SHOTs identical.

### (c) by eye

B's `race00_L.bmp` / `race01_L.bmp` (owned interpreter, every Tier-2 lever on): the Mute
City start grid with its cyan edge strips, the guard rails, the start gantry and the 30
machines on the grid; in `race01` the road surface, the rails, the far skyline and the
player machine with its boost trail. HUD (lap/position/time/energy, rival portraits, minimap,
speed readout) intact. This is the frame that was an empty pink cityscape under `all=1`
before.

## Gotchas

- `gdx-dev retro run` runs to its `--seconds` deadline even after `[rec] end` + `replay_quit`
  (the guest quits, RetroArch does not exit); the canonical replay completes in ~4 min on
  the software renderer, so `--seconds 420` is enough and 1800 just idles.
- zsh does not word-split `$VAR`: build `gdx-dev` argument lists as arrays (`"${A[@]}"`),
  and `ls $DIR/log-*.txt` before handing a glob to `grep` through a variable.
- Three concurrent `gdx-dev retro run` boots on this machine host-crash each other
  (`Unhandled EXC_BAD_ACCESS` in RetroArch at guest frame ~40, every renderer, the guest
  log healthy) — the same class interp-progress.md notes for a loaded lane. Run the gate
  sequentially.
- `friend class GdxOwnedSeam;` inside `namespace gdxn` befriends `gdxn::GdxOwnedSeam`; the
  adapter is global, so it is `friend class ::GdxOwnedSeam;` with a forward declaration.
- `gdx_dl.cpp` / `gdx_interp.h` are host-compiled by the harness: the hooks are function
  pointers registered from `gdx_rt_glue.cpp`, never symbol references.
