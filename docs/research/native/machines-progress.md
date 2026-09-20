# machines — progress (native retained machine meshes)

Track: `machines`, branch `native/machines`, worktree `~/code/gdx-3ds/machines`.
Plan: `docs/research/native/07-machines-plan.md` (in the `native` worktree; not yet on native/main).

## State (2026-09-12)

- M0-M3 done; M4 emulator evidence collected on the RetroArch lane (below). Remaining:
  hardware card verdict, patch consolidation at merge (native/main moved on: vendored LUS
  carve, devcli, hwlink — the hook must be re-applied to port/3ds/vendor/lus at merge).

## Emulator evidence (RetroArch + Azahar libretro core, `gdx-dev retro run`, New3DS model,
   Vulkan; instances m1/m2/m3, private SDs; replay /tmp/machines-art/mx-replay.txt staged
   as gdx-autoinput.txt because `retro run` ignores --replay)

Build 9f9d969a (commit be5e3ae) unless noted. Three concurrent runs, same replay:
A0 `[native] machines=0`, C `machines=1`, B `machines=1 machines_walk_only=1`.
SHOT scanout BMPs (288054 B, 400x240) byte-compared:

| SHOT (tick) | scene | C native vs A0 | B walk vs A0 |
|---|---|---|---|
| mx_title (350) | title | IDENTICAL | IDENTICAL |
| mx_machsel (2300) | machine select, LOD1 in a viewport | IDENTICAL | IDENTICAL |
| mx_machsel2 (2500) | machine select, next character | IDENTICAL | IDENTICAL |
| mx_race00 (4700) | race, driving | IDENTICAL | IDENTICAL |
| mx_card (3300) | course intro card | 3808 px differ | 4426 px differ |
| mx_race01 (5300) | race | 380 px, rows 79-108 cols 161-237 | 393 px, same box |
| mx_race02 (6100) | race | 391 px, rows 82-103 cols 162-238 | 394 px, same box |

The card/race01/race02 differences appear identically for the interpreter's own walk-only
path, in the same small centre box (HUD text), i.e. the known in-race run-to-run
nondeterminism (03-oracle-plan: timer digits / pack positions), not machine geometry.

**In-frame verify (`machines_verify=1`, run D/D2/D3):** every draw runs the interpreter's
own GfxSpVertex/GfxSpTri1 path (drawn + captured) and then the native path (captured, GPU
submit suppressed); packed triangle streams compared bit-exact per triangle, plus shader
ids and bound texture ids. Over the 30-car attract demo (draws=1920/window, ~105k tris per
64-frame window), machine select and the race: `triMismatch=0 countMismatch=0
matMismatch=0` in every window, 0 `verify MISMATCH` lines. (`auxDiff` in D/D2 was the
interpreter's static aux record keeping stale entries past numInputs; masked in D3.)

**Profile ([prof], gputrace=1, emulator = relative only, same lane, 30-car attract demo
windows frame 2113-2561, ms per frame over 64 frames):**

| | br | dsp | vtx | tri | imp | drw | sum |
|---|---|---|---|---|---|---|---|
| legacy (E, frame 2113) | 10.35 | 25.81 | 8.11 | 20.00 | 1.57 | 4.31 | 70.2 |
| native (F, frame 2113) | 5.61 | 29.32* | 0.00 | 0.78 | 1.89 | 4.42 | 42.0 |

`*` dsp includes the native draw (`[native] us=21.7/f`, which itself nests imp+drw).
So in lane terms the 30 machines cost ~50 ms (vtx+tri+their dispatch+bridge) on the
interpreter and 21.7 ms natively; the bridge no longer converts 30 x 126 commands
(br 10.35 -> 5.61). Emulator only — hardware numbers await the card.

## Host harness results (tools/native/machines_harness.sh, `--out /tmp/machines-art/harness`)

- Emitters: 263 body functions (33 stock x 5 + Front 7x5 + Rear 7x5 + Wing 5x5 + 3 WingNone)
  + 30 texture lists; `Machine_DrawCustom` / `Machine_DrawLoadCustomTextures` are composites
  taking args and are exercised through their parts. The plan's "295" was the prototype count.
- Test A (walker vs `tools/native/gfx_decode.py`): noek 11905 tris / 263 lists, ek (grey 0+1)
  23810 tris / 526 lists — byte-identical canonical streams. 11905 = census 5814x2 + 277.
- Chunks 1427 (= gSPVertex sites), 1372 VTX symbols all resolved from fzerox.o2r, max 97
  commands per list, max 263 vertices per list, 0 lists with trailing state.
- Test B golden: `port/tests/golden/machines_inherit_{noek,ek}.txt` — 794 ENV-inherit runs
  (body colour), **0 PRIM-inherit runs** (every PRIMITIVE-consuming run sets prim first; the
  plan's "prim not always set" is wrong for the built lists), custom-part args land in
  prim/env as `ARG` where expected.
- Test C: 20k vertices (8700 alpha-LUT, 3000 texgen, 3700 fog), 20k triangles (reject /
  all-out / cull / 7-plane clip + fan), 40k UV samples, 281524 checks, 0 failures, with
  -ffp-contract=off.

## Device implementation (port/3ds/gfx/gdx3ds_mesh.cpp)

- Registry: 150 lists (30 slots x LOD1-5) from `D_800CDDB0[]`, pointer hash -> entry, one
  8-byte stub `F3DGfx` (G_ENDDL) per entry. Bridge (`n64_gfx_bridge.cpp` kOpDl) redirects a
  registered + baked host list to its stub; the interpreter hook draws it.
- Bake at bridge lookup (walker; vertex pointers through the adapter's own kOpVtx rules —
  `ResolveVtxForMesh`: host-pointer tag, placeholder/BSS-alias exceptions, segment
  resolver, readability), vertices IN PLACE in the loaded asset-segment image (already
  host-endian: AssetBindings.c kind-3 Vtx fixup). Heap per mesh: runs (36 B each) +
  chunks (20 B) + 6 B/tri + 1 B/vertex.
- Draw: `LatchVtxState` (MP, lights incl. the interpreter's lights_changed recompute, FLX
  alpha light, LUT, texgen, fog, AdjX/STRETCH branch), T&L all vertices once per draw with
  the texture scale recorded per chunk, then per run: `ApplyRunState` (interpreter setters
  in emission order), `BindMaterial` (GfxSpTri1 draw-state replica), reject/clip/cull,
  pack, one `C3D_DrawElements`. Fallback per run to `WalkRun` (GfxSpVertex/GfxSpTri1).
- Backend: `NativeMeshBegin/Draw` reserve verts + u16 indices in the per-frame linear pool;
  `DrawTriangles` tail shared (IssueDraw lambda: DrawElements vs DrawArrays), stereo per-eye
  loop untouched. `dNat=` on the `[c3d]` line.
- Memory: indices/runs ~150 KB worst case for all 150 lists (nothing shipped), scratch
  32 KB (1024 loaded vertices) + 12 KB index scratch; verify mode allocates 2 x 3.2 MB
  capture buffers lazily (verify only).

## Final state (2026-09-13)

- Hardware card staged: `~/Desktop/G-Diffuser-3DS-machines/` (3dsx sha 34377272..., elf,
  SHA256SUMS, TEST-PLAN.txt with the A/B toggles). Binary = commit 05ae967 build; boot-checked
  on the RetroArch lane (run G-bootcheck: PASS, frame 29506, 0 [fatal]).
- Runs F3/D3 (two instances launched 15 s apart right after a killed run) both stopped
  logging at guest t=4 s ("heartbeat not advancing") — a lane glitch, not the build: the same
  binary boots and runs on m1 (G-bootcheck). Their stage-timer receipts (`tnl/bind/emit/draw`)
  therefore have no emulator numbers yet; the hardware run will provide them.
- The SD card was not touched.

## Design deltas vs the plan (with reasons)

1. **Hook point is split bridge + interpreter.** The plan hooks `gfx_dl_handler_common` on the
   game's list pointer. On this port the game's host-built `Gfx` lists never reach the
   interpreter as-is: `port/n64_gfx_bridge.cpp` converts every sub-list into a per-task
   `ConvertedList` and the interpreter's `G_DL` target is that transient buffer. So:
   - the bridge (`case kOpDl`) asks `gdx3ds_mesh_bridge_lookup(hostPtr)`; a registered
     (and successfully baked) `D_800CDDB0[]` list is redirected to a stable 1-command stub
     list (`G_ENDDL`) owned by the mesh registry, and the 126-command conversion is skipped;
   - the interpreter patch (`lus-native-machines-dl-hook.patch`) is one call-out in
     `gfx_dl_handler_common`: `gdx3ds_mesh_dl_hook(interp, subGFX)`; a hit draws the mesh
     from the interpreter's live state and skips the (empty) stub.
   Bake happens at bridge-lookup time, so a failed bake leaves the list on the legacy path
   with the ORIGINAL list (the interpreter cannot consume a raw game `Gfx` list).
2. **Interpreter vertex math is not rewired to the shared header.** The user asked for a tiny
   hook patch; `gdx3ds_vtx_math.h` is a verbatim transcription of `GfxSpVertex`/`GfxSpTri1`
   math and test C (host) + device parity guard it instead. Rewiring is a follow-up once the
   vendor carve lands.
3. **Backend entry** is `gdx3ds_mesh_backend_*` (indexed `C3D_DrawElements` inside the
   existing DrawTriangles per-eye tail) rather than `Gdx_NativeBatch` — to be unified with
   the track agent's implementation at merge (see 08-track-plan.md §3.7).
4. Generation bump: `gdx_native_machine_lists_rebuilt()` (port/decomp_port.c, all platforms)
   called from the three rebuild sites in racer.c via `decomp-native-machines-rebuilt.patch`.

## Ini keys ([native] section)

| key | default | meaning |
|---|---|---|
| machines | 0 | 1 = draw registered machine lists natively |
| machines_walk_only | 0 | 1 = replay the baked mesh through GfxSpVertex/GfxSpTri1 (M2 stepping stone) |
| machines_mask | ffffffff (hex) | slot mask (bit i = machine slot i, 30 slots) |
| machines_lod_mask | 1f (hex) | LOD mask (bit l = LOD l+1) |
| machines_verify | 0 | 1 = per draw: walk pass drawn + captured, native pass captured with GPU submit suppressed, triangles compared bit-exact (`[native] verify ...` receipt) |

## Receipts

- `[native] machines=1 [walk_only=1|verify=1] lists=150 baked=N bakes=w/t fails=F draws=D walk=W runs=R
  verts=V tris=T rej=.. out=.. cull=.. clip=.. fan=.. fb=.. pool=.. us=U/f linfree=..K` every 64
  frames (silent when idle). `fb` = runs replayed through the interpreter (bind fallback),
  `pool` = backend refusals.
- `[native] bake slot=S lod=L cmds=.. runs=.. chunks=.. verts=.. tris=.. us=.. gen=..` per bake
  (first 160), `[native] bake FAIL ...` on walker refusal (list stays on the interpreter).
- `[native] verify draws=.. tris=.. triMismatch=.. countMismatch=.. matMismatch=.. auxDiff=..`
  (verify mode; `auxDiff` counts vertex-0 aux records that legitimately differ when the
  interpreter split a run into several batches).
- `[c3d] ... dNat=N`: indexed mesh draws in the window.

## Gotchas

- zsh: `echo ====` is a command expansion; use quoted separators.
- The VTX host arrays in AssetBindings.c are declared full-size but the bridge treats every
  asset-segment symbol as a PLACEHOLDER (`IsAssetPlaceholderPointer`): the bytes the
  interpreter reads live in the loaded segment image (`EnsureAssetSegmentImage`). The
  harness fills the arrays lazily from per-symbol o2r entries instead; the device baker
  resolves through the adapter.
- `fast/f3dex2.h` needs the interpreter's OPCODE macro; do not include it from port code.
- Backend statics live in `Fast::{anonymous}`; the extern "C" shims must qualify `Fast::`.
- Some lists emit `gSPTexture(..., G_OFF)` before shade-only runs: the walker records the on
  bit (GfxSpTexture ignores it).
- Shared SD `autotest/` holds other agents' BMPs: our labels are `mx_*` only.
