# G-Diffuser 3DS — native renderer fork: where it stands

One page, for a human. Written 2026-09-17 against `native/main@999523f7`
(3dsx sha256 `b0ff58cfc33f273c8d14368a4d2e510fdd65d96442bdaccadc9b780995a40a6f`, the build
staged on the Desktop card). Some evidence below was taken on `native/main@19c0fce9`
(3dsx `838d31d1…`); the two hash-match frame for frame on the canonical replay, so it
carries. Evidence for every claim here: `integration-verify-2026-09-17.md`.

## What the fork is

G-Diffuser is a PC port of the F-Zero X decomp; `feat/3ds-hwaudio` is the Nintendo 3DS
port of it, and it works — New 3DS, 60 fps in the normal case, audio, stereo 3D, sleep
and power-off all hardware-verified. Everything it draws goes through **libultraship's
Fast3D interpreter** (`interpreter.cpp`, 10,466 lines), fed by a **bridge**
(`port/n64_gfx_bridge.cpp`, 12,353 lines) whose only job is to widen the game's 32-bit
`Gfx` words into the 64-bit words LUS wants. Every triangle in the game is transformed
and lit on the ARM11 CPU, one display-list command at a time.

The **native fork** (branch `native/main`, worktree `~/code/gdx-3ds/native`) replaces
that, piece by piece, with code that writes PICA vertex buffers directly — and keeps the
old path alive beside it. It is a *hybrid* renderer by design, not a rewrite: the menus,
the course editor, the ending and the long tail of one-off screens will stay on an
interpreter forever.

Two tiers:

- **Tier 1 — own the interpreter.** `[native] interp` swaps LUS's `interpreter.cpp` and
  the bridge for a purpose-built F3DEX2→citro3d walker (`port/3ds/gfx/n64/`). Same
  pixels, no conversion pre-pass.
- **Tier 2 — bypass the interpreter for the three things the race spends its frame on.**
  `track` replays the course tessellator's vertex stream straight into a VBO;
  `machines` draws the 30 racer meshes from baked, retained lists; `fx_sink` /
  `fx_sprites` / `fx_hud` batch the effect sprites and the HUD.

Every lever is an ini killswitch, every lever defaults to **0** = the exact prior path,
and each one has a verify mode that draws the stock path and compares the native path's
packed vertex bytes against it in-process, bit for bit.

**The two tiers combine** (`native/interphooks`). `track` and `fx_sink` work by making the
game emit a marker where its geometry commands used to be, and `machines` by redirecting a
registered list at `G_DL`; the replays behind those hooks are written against one
interpreter seam (`gdx3ds_interp_seam.h`) with an adapter per interpreter, so the owned
walker expands the markers and draws the meshes through the same code the LUS interpreter
does. `interp=1` with every Tier-2 lever is pixel-identical to `interp=0` with the same
levers — see `interphooks-progress.md`.

Supporting tracks built the ground the above stands on: `vendor` (the LUS carve is now
vendored source under `port/3ds/vendor/lus/`, so 38 of the 48 patches are gone),
`oracle` (true frame capture, `[fh]` per-frame hashes, `.gdxrec` tick-exact record and
replay, determinism pins), `devcli` (`tools/dev/gdx-dev` — the whole emulator lane in one
command), `hwlink` (a TCP link to the console), `refbuild`, `residuals`, `residuals2`
(run-to-run nondeterminism in texture-id recycling, and the atlas UV audit) and `atlasuv`
(the atlas view clamp limit).

## The `[native]` ini keys

All in `sd:/3ds/gdiffuser/gdiffuser.ini`, section `[native]`, read **once at boot**
(changing one needs a restart). Every default is the prior behaviour.

| key | default | what it does |
|---|---|---|
| `all` | 0 | Master for the **Tier-2 levers only** (`machines`, `track`, `fx_sink`, `fx_sprites`, `fx_hud`): 1 = each of those that is 0-or-absent reads as 1; an explicit `2` (verify) is kept. Never touches `interp`. |
| `interp` | 0 | 1 = the owned F3DEX2→citro3d interpreter draws the frame instead of LUS Fast3D + the bridge. Always explicit (`all` does not imply it). Carries the Tier-2 hooks, so `interp=1` combines with any of the levers below. |
| `interp_verify` | 0 | 1 = run BOTH interpreters over every job and compare the packed batches field by field (`[nverify]`). Slow; a correctness tool. |
| `cpuclip` | 1 | The owned interpreter clips triangles on the CPU the way LUS does. 0 = let the PICA clip (fewer draws, different pixels — the tunnel A/B). |
| `blendflush` | 1 | The owned interpreter flushes its batch before a blend-alpha change, so alpha compare uses the register in force at the primitive (LUS parity). |
| `machines` | 0 | 1 = the 30 racer meshes draw from baked retained lists (150 lists = 30 slots × 5 LODs), one indexed draw per material run. |
| `machines_walk_only` | 0 | 1 = replay the baked mesh back through the interpreter's own vertex/triangle path. A stepping stone, kept as a bisect. |
| `machines_verify` | 0 | 1 = per draw, the walk pass is drawn and captured, the native pass is captured with GPU submit suppressed, and the triangle streams are compared bit-exact. |
| `machines_mask` | `ffffffff` | Hex per-slot mask (bit *i* = machine slot *i*, 30 slots) — bisect which machine breaks. |
| `machines_lod_mask` | `1f` | Hex per-LOD mask (bit *l* = LOD *l*+1). |
| `track` | 0 | 1 = the course tessellator's vertex loads and index templates are recorded and replayed into a native batch, one draw per material run. 2 = verify (stock drawn, native replayed into a discarded batch and hash-compared per run). |
| `trkcensus` | 0 | 1 = `[trk]` per-frame census. The `[trk] replay(prev) … vok=/vbad=` coverage totals print in verify mode regardless (28f588e). |
| `fx_sink` | 0 | 1 = the six effect sprite ring classes (debris, sparks, smoke, explosions, force field, fall explosions) are recorded at the game-side macro and replayed as native per-class batches. 2 = verify. |
| `fx_sprites` | 0 | With `fx_sink`: fold prim/env colour changes into the vertex colour where the backend carries that input per vertex, so a colour change stops splitting the draw. |
| `fx_hud` | 0 | The same fold rule applied inside the interpreter's own 2D batches — HUD digits, stars, markers stop splitting on every prim colour. |
| `fxcensus` | 0 | 1 = `[fx]` per-class census. The `[fx] agg2 … vok=/vbad=` coverage totals print in verify mode regardless (interphooks). |

Boot receipt (one line, always, every value RESOLVED): `[native] all= interp= machines=
track= fx_sink= fx_sprites= fx_hud= fx=<bitmask> census trk= fx= (resolved: all=tier2 only,
interp explicit; ini latched at boot)`, plus the interpreter path line `[native] interp=
path=lus|owned cpuclip= blendflush= verify= hooks=`. The DBG tab shows the same state on its
`NATIVE` and `NATIVE FX` rows.

Related `[debug]` keys the harness uses and a player should leave alone: `det`,
`fh`, `fh_every`, `fh_start`, `input_replay`, `input_record`, `replay_quit`, `gputrace`.
`[net] enabled` (the hwlink dev link) must stay 0 for any normal run.

## Hardware-verified vs emulator-only

**Blunt answer: none of the native levers has ever run on hardware.** Every parity,
verify and profile number in this fork is from Azahar (Qt lane or the libretro core under
RetroArch), on a loaded Mac, with a JIT. Emulator milliseconds are relative only.

| thing | status |
|---|---|
| The base port (`feat/3ds-hwaudio`): 60 fps, audio, stereo, APT sleep/HOME/power-off, flat heap | **hardware-verified** (round 4/5 sessions; med 51 / p95 60 / max 60 fps) |
| `track`, `machines`, `fx_sink`, `fx_sprites`, `fx_hud`, all five together | emulator-only: pixel-identical to all-off over 6002 frames on two builds, three verify modes clean, +10.6 % in the 30-car window. Cards staged, never run. |
| `interp` **alone** | emulator-only: pixel-identical to all-off over 4101 frames (title → menus → race), per-batch differential 0 mismatches over 13,440 tasks, 50-minute soak. Card staged, never run. |
| `interp` **together with the Tier-2 levers** | emulator-only (`native/interphooks`): `interp=1 all=1` vs `interp=0 all=1` pixel-identical over the canonical replay; the three verify modes under `interp=1` report zero mismatches with nonzero coverage. `fx_hud` is inert under `interp=1` (no fold in the owned 2D batches; pixel-neutral). See `interphooks-progress.md`. |
| `trectbatch=1` (the atlas) matching `trectbatch=0` | emulator-only, but now **clean**: 6002/6002 frames and all 8 SHOTs identical on `999523f`, where the `residuals2` sub-texel shift used to differ. |
| Stereo (3D slider) interaction with any native lever | **unverified anywhere**: the emulator lane never activates the stereo right-eye pass, so the `[c3d] … anchor=` / `eyeR=` receipts never appear. |
| `cpuclip=0` on a tunnel course | **unverified**: the canonical replay's course has no tunnel. |
| Every emulator millisecond in every progress doc | relative; hardware is truth |

## The cards on the Desktop

Nothing here has touched the SD card. Each directory holds `G-Diffuser-3DS.3dsx`,
`G-Diffuser-3DS.elf` (symbols, for crash triage), `SHA256SUMS` and `TEST-PLAN.txt`.

| card | build | what it tests |
|---|---|---|
| `G-Diffuser-3DS-native1/` | `native/main@999523f7`, 3dsx `b0ff58cf…` (**current — refreshed 2026-09-17**) | The integration card. Runs A–H: every lever alone, all five Tier-2 levers together, the three verify modes for one lap, `interp` through every screen plus the tunnel `cpuclip` A/B, and a 30-minute soak with stereo (split in two, because the tiers cannot be soaked together). |
| `G-Diffuser-3DS-track/` | `native/track` | The course path alone: geometry popping, missing rails, tunnel joins, and the A/B frame rate. |
| `G-Diffuser-3DS-machines/` | `native/machines@fe318a2` | The retained machine meshes alone: bodies, liveries, LOD transitions, the machine-select screen, plus `machines_verify`. |
| `G-Diffuser-3DS-interp/` | `native/interp@75c532c` | The owned interpreter alone: every screen by eye against the stock path, the tunnel `cpuclip` A/B, and `[prof]` windows. |
| `G-Diffuser-3DS-fx/` | `native/fx@9a62855` | The effects and HUD levers alone: visual A/B of the six sprite classes, the in-process verify lap, the stereo-anchor check, the night-course star A/B. |
| `G-Diffuser-3DS-hwlink/` | `native/hwlink@2712205` | Not a renderer card: the TCP dev link (PING / RELOAD / PUT / log streaming) that would end the SD round trip. Needs `SETUP.md` first. |
| `G-Diffuser-3DS-rc1/` | `main@824c7ae` | The pre-fork release candidate, for reference — what "unchanged" looks like. |

If you only run one, run **`native1`**: it is the current build and its plan covers the
others' ground.

## Merge log — tracks into `native/main`

First-parent order, oldest first. The fork base is `feat/3ds-hwaudio@767b6c0`.

| commit | track | what merged |
|---|---|---|
| `9b8f332` | vendor | LUS carve vendored into `port/3ds/vendor/lus/` (Phase 0) — 38 lus patches deleted |
| `c5415b6` | hwlink | `[net]` console link + `tools/dev/hw.py` |
| `beff251`, `8882dee`, `83e0b6d` | devcli | `tools/dev/gdx-dev` — build, emu/retro lanes, prof, runs, sd, diff |
| `fec2b61` | refbuild | desktop reference build on this Mac |
| `9a7585e` | track | course GBI shim + native batch replay; the LUS marker hook folded into the vendored carve |
| `379a05f` | oracle | true frame capture, `[fh]` hashes, `.gdxrec` record/replay, determinism pins, `framediff`/`fhdiff` |
| `2611e0d` | machines | retained machine meshes + host harness; the LUS DL hook folded into the carve |
| `fce2dd1` | interp | owned F3DEX2 interpreter M0–M4 (default off); `[nverify]` side channels folded into the carve |
| `e18f5f2`, `87a12cf` | residuals2 | canonical texture-id recycling, pow2 pad gutter, atlas UV clamp audit + host tests, vendored-LUS manifest checked by CI |
| `b828cb4`, `19c0fce` | fx | effects sink + sprite batches, HUD prim/env fold, the `[native]` section reader (decomp patch 15) |
| `80b16a9` | devcli | a replay that does not finish is no longer a PASS (`[rec] end` + every SHOT required; the manifest carries `replay_complete`/`shots_seen`/`sha256`) — tools only |
| `999523f` | atlasuv | the view clamp limit in `AtlasFillTexParams`, so `trectbatch=1` matches `trectbatch=0` on HUD and course-name text |

`tools/ci-3ds.sh` applies 15 decomp patches (pure deltas, listed in `PATCH_LIST`), checks
the vendored-LUS manifest, runs 7 host test binaries and builds the `.3dsx`.

## Known open items

1. **Closed (`native/interphooks`): `interp` + the Tier-2 levers.** The owned walker's
   `OP_NOOP` now replays the track and fx markers and its `G_DL` hook draws the machine
   meshes, through the interpreter seam (`port/3ds/gfx/gdx3ds_interp_seam.h`, adapters
   `gdx3ds_seam_lus.cpp` / `n64/gdx_seam_owned.cpp`); `all=1` no longer implies `interp`.
   Evidence and residual gaps (the `fx_hud` fold has no owned-walker equivalent yet — it is
   pixel-neutral, so `interp=1 fx_hud=1` is merely inert) in `interphooks-progress.md`.
2. **Atlas sub-texel shift — looks closed, wants a hardware look.** With the `[debug]
   trectbatch` atlas on, glyph rects used to render ~0.16 texel to one side of the
   standalone path (782 px on the course-name frame, crisp-vs-smeared, horizontal only,
   one-directional; `residuals2-progress.md` §2). `native/atlasuv` (`999523f`) fixed the
   view clamp limit in `AtlasFillTexParams`, and on this build `trectbatch=1` and
   `trectbatch=0` are pixel-identical over the whole 6002-frame canonical replay with all 8
   SHOTs byte-identical. That is one replay on one emulator, and the original residual
   could always have been an Azahar sampler artefact, so the hardware check is still owed —
   but `trectbatch=0` is no longer a necessary workaround.
3. **The stereo anchor is unverified on the emulator lane.** The right-eye pass never
   activates under Azahar, so `[c3d] … anchor=` never prints and no native lever has ever
   been seen interacting with stereo-anchored draws. It is run H on the card.
4. **`interp` M5 is pending hardware.** The owned interpreter is correct and fast in the
   emulator, but it stays default-0 and the bridge and LUS both stay linked until a
   hardware run says the frame is right; deleting them is M5 and has not started. The
   tunnel `cpuclip=0` A/B also has no tunnel course in any committed replay.
5. **Verify modes pass silently without their census key.** `track=2` reports no coverage
   without `trkcensus=1`, and `fx_sink=2` reports none without `fxcensus=1` — in both
   cases the MISMATCH line is unconditional but the "how much was checked" totals are
   not, so a run with the census off looks like a clean pass and proves nothing.
   `machines_verify=1` is the exception: its counters print unconditionally.
6. **`docs/3DS-HARDWARE.md` §5 is stale.** It still says `interp` and `fx_hud` are "not
   merged / not built yet: no effect", and it does not list `machines_*`, `interp_verify`,
   `cpuclip` or `blendflush`. The table above is the current one.
7. **`gdx-dev` counts `[fatal-trace] atexit` as a fatal.** Every clean self-terminating
   run (`replay_quit=1`) emits that line on its normal exit path and is scored FAIL with
   nothing wrong (`tools/dev/gdxdev/emu.py:134`). Read the verdict detail, not the word.
8. **A `PASS` verdict does not mean the run finished.** The heartbeat check only sees what
   was logged before a core death. Cross-check `wall_s` and the `[fh]` count against the
   replay's expected frame or SHOT count before believing any run.
9. **Not built, by design:** explosion frame atlas, shadows as instanced quads, indexed
   draws for the track path, and the long-tail screens (course editor, EK, ending,
   records) have no replays and have never been exercised by any automated run.
