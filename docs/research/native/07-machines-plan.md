# 07 — Machines: retained PICA meshes for the machine display lists (Tier 2, item 1)

Track: `machines`. Status: PLAN (no code). Reads: `00-master-plan.md` §2, §4.3, §5 Tier 2 item 1, §8.
All paths below are relative to the `m1` worktree unless prefixed.

## 1. Goal

Draw every in-race machine body natively (one indexed PICA draw per material run, CPU T&L in one
tight loop) instead of walking its F3DEX2 list through the interpreter each frame, with
frame-hash parity against the LUS path, behind `[native] machines=0/1`.

## 2. Code facts (what the master plan got right, and what it got wrong)

**Emitters.** `decomp/src/overlays/ovl_i9/machine_draw.c` (16,148 lines) defines **295**
`Gfx* Machine_Draw*` functions, not 392 (392 counts the prototypes at :32-181): 33 stock
characters x 5 LODs = 165 (`:4942` BlueFalconLod1 ... incl. SuperFalcon/Stingray/Cat), custom
parts Front 7x5 (`:961`), Rear 7x5, Wing 6x5-ish (`:3428`), `Machine_DrawCustom` (`:182`, wing
then rear then front), and 31 `Machine_DrawLoad*Textures` (`:16054`, `:15425` custom).
Opcode census (whole file): `gSP2Triangles` 5814, `gSPVertex` 1427 (all with v0=0, max 64),
`gSPTexture` 1369, `gDPPipeSync` 960, `gDPSetPrimColor` 795, `gDPSetCombineLERP` 494,
`gDPSetEnvColor` 394, `gSP1Triangle` 277, `gDPLoadMultiBlock[_4b]` 203 (**only** inside the
`Load*Textures` functions). Zero geometry-mode, render-mode, matrix or other-mode commands: the
body functions are pure straight-line `{texture tile, [prim], [env], combine, vertex, tris}*`
emitters. Five combiners only (`:271/108/56/43/16` sites = census modes #16-20 in
`combiner-census.md`, all `cycle2 = COMBINED*SHADE`).

**They do not run per frame.** `racer.c:2058 func_8008D3C4`, `:2096 func_8008D5F4`,
`:2179 func_8008D97C` run each function ONCE at race/menu load into static buffers
`D_800CDDB0[slot*6+lod]` (`racer.c:164`, 180 x `Gfx[126]` in `game/unk_gfx_segment.c:9`) plus
one texture-set list per slot `D_800CDD38[slot]` (`:156`). The race loop draws
`gSPDisplayList(D_800CDD38[idx]); gDPSetEnvColor(bodyRGB,255); gSPDisplayList(D_800CDDB0[idx*6+lod-1])`
(`racer.c:6125-6128`; ghosts `:6206-6209` with env alpha 160 and an explicit XLU render mode
`:6189`; attack highlight `:6023-6028` re-draws the same list with `gDPSetBlendColor`; menus
`:7191-7225`, `ovl_i4/machine.c:1596-1660` with viewports). LOD 6 = `D_7045098`, an empty list
(`racer.c:5788`, thresholds 230/290/380/470/1500 at `:5773-5785`; `decomp-port-rival-detail.patch`
only biases the distance). Per-frame cost today is therefore the interpreter walking ~40-60
commands and transforming ~3 vertices per triangle per visible machine, not the emitters.

**Colour args.** Stock character functions take `(Gfx*)` only; colours are literals. The 12
args exist only for custom parts (Front: decal/number/cockpitPrim/cockpitEnv RGB; Rear/Wing:
decal/number) and are baked into the slot's list at load from `sCustomMachineInfo` /
`MachineInfo` (`racer.c:2064-2078`, `:2103-2113`). The only per-frame colour is **ENV** (body
colour, alpha 255/160), consumed by every mode via `ENVIRONMENT`; some runs override env
(`Machine_DrawBlueFalconLod5` sets env 0,0,0 for tile 6) and later runs inherit it. Prim colour
is set by most runs before a PRIMITIVE mode but not all (e.g. `SuperCatLod5` sets it first;
`Rear0Lod1` second run) -- prim/env at list entry are **inherited interpreter state**.

**Runtime state reads.** Two, both build-time-only: `gGreyscaleMachinePart` (70 sites, EK-only
`#ifdef`, set from `machine_create/custom_machine_select.c:251,354`; substitutes prim colour
228,228,228 or a grey texture `D_8013C980`) and the anti-piracy block in `BlueFalconLod1`
(`:4948-4972`: `osRecvMesg(D_800DCAB0)`, `IO_READ`, `ROM_READ(0x510)`, bumps `D_800CE784`, may
zero `gBodyHealthValues`; port chokepoints `decomp_port.c:467 gdx_rom_read32`, `:484 gdx_io_read`
returns 0). EK is compiled in by default (`port/cmake/GdxGameSources.cmake:55`); the EK tree's
`machine_draw.c` differs from decomp's only by the `#ifdef` bodies (616 diff lines).

**Vertices/textures.** `gSPVertex` operands are segment-3 (`D_30xxxxx`, 324 sites,
`machine_custom_gfx.yaml`, seg `[0x03, 0x17B1E0]`) and segment-7 (`D_70xxxxx`, 1048 sites,
`machine_global_gfx.yaml`, seg `[0x07, 0x1E23F0]`) VTX symbols; 55 sites use `&D_x[62..64]`
offsets. In the port every symbol is a host array (`port/gen/AssetBindings.c:1651 u8 D_30040F8[0x140]`)
filled at boot by `GDiffuser_LoadAllAssets` from the o2r (`:5335` key
`machine_custom_gfx/D_30040F8`); o2r VTX entries are a 64-byte `OVTX` header, u32 count, then
16-byte host-endian `Vtx` (verified on `fzerox.o2r`). Lit vertices carry s8 normals in the
colour slot. Textures: 8 tiles per machine loaded by `Load*Textures` via `gDPLoadMultiBlock`
(RGBA16 32x32/16x16 for the emblem) and `_4b` (I4/IA4 16x16..32x64), fixed clamp/mirror/wrap per
axis, masks = log2(size), shift 0, no TLUTs; body runs select tiles 0-7 with
`gSPTexture(0xFFFF,0xFFFF,0,tile,G_ON)` (custom parts use the `MACHINE_TILE_CUSTOM_*` enum,
`fzx_machine.h:101`). `combiner-census.md` §2c's claim that stock machines are binary DLs is
wrong: `machine_models.yaml` (segment 9) is only the reflection pass `D_800CDAD8`
(`racer.c:7264`, GP-end cutscene and machine settings) which stays on the interpreter.

**Setup state (binary DLs, decoded from the o2r).** `D_303A6A8` (race) / `D_303A5F8`
(GP-end): clear all geometry modes then set
`ZBUFFER|SHADE|CULL_BACK|LIGHTING|TEXTURE_GEN|SHADING_SMOOTH|CLIPPING` (race adds TEXTURE_GEN),
`NUMLIGHTS 1`, `G_CYC_2CYCLE`, othermode_l `0xC8000230` = `Z_CMP|Z_UPD|CVG_DST_FULL` with
blender cycle 1 `(CLR_FOG, A_SHADE, CLR_IN, 1MA)`, bilerp, and a tile-0 load. **No `G_FOG`.**

**Lighting is the FLX trick, not plain Gouraud.** The race task runs `gspF3DFLX2_Rej`
(`sys/sys_gfx.c:174`). `racer.c:6073-6074` loads one directional light + ambient (dir `0x45^3`
or track-up*120 in tunnels; ambient 50..100), `:6108` DMAs `D_800CE3D0` (a 256-byte alpha LUT)
into DMEM 0x8B0, `:6109 gSPLookAtY` uploads the "alpha light" (`lightdir - camera.x`), `:6110`
sets the fog colour (`racerFogColors` or tunnel/pipe colours). LUS reproduces this exactly:
`interpreter.cpp:3623-3695` shade RGB = ambient + max(0, n.coeff/127)*light, `:3697-3711`
under `FZeroFlxReject` + `G_TEXTURE_GEN`: vertex alpha = `dmem[dma_io_dmem+128+clamp((int)dot(alphaLight_model, n))]`
(UVs stay from `tc`), `:3833-3834` alpha carried in `color.a`; `:5748-5770` the LookAtY slot
becomes the alpha light after `GfxSpDmaIo` (`:5786`). The blender's `CLR_FOG/A_SHADE` makes
`use_fog` true (`:4269`), and the backend maps it to a TexEnv INTERPOLATE(fogColour, prev,
primary.alpha) stage (`gfx_citro3d.cpp:1366-1384`; packed batches feed the factor through
`out[9]` and `Gdx3dsVboPackAux.fogRgb`, `gdx3ds_vbopack.h`). The per-vertex specular is what
"lit geometry" means for machines; there is no fog on them.

**Reject semantics.** The Rej ucode never clips: `interpreter.cpp:4007-4019` drops any triangle
with a vertex `w<=0`, `|x|>2w`, `|y|>2w`, or `z<-w`; backface cull is on CPU. Both must be
reproduced per triangle for parity.

**Custom machines / creator.** In-race custom machines are ordinary slot lists (parts baked by
`Machine_DrawCustom` at load). The Create Machine screens draw parts live every frame
(`machine_create_draw.c:658-665, 711-730`) and stay on the interpreter (long tail).

**Backend.** Packed PICA layout = 12 floats `pos4 uv0 rgba uv1` (`gdx3ds_vbopack.h`,
`shader.v.pica`: clip-space in, `projection` fixup uniform); stereo = per-eye re-issue of the
same VBO range with a different uniform (`gfx_citro3d.cpp:2775-2823`); viewport fold and DISP
modes live in the projection compose (`:2173`); `DrawTriangles` is arrays-only, 1 MiB linear
pool (`:525`). Matrix frame interpolation (`gdx_interp.h`) happens upstream in the bridge's
G_MTX rerouting, so anything that reads the interpreter's current modelview sees the lerped one.

## 3. Design

### 3.1 Hook point: intercept `G_DL` for registered lists, inside the interpreter

Because the game already bakes each machine into a static list, the runtime replacement is not
"replace the function body" but "replace the walk": a LUS patch in `gfx_dl_handler_common`
(`interpreter.cpp:8052`) checks the target pointer against a small registry (pointer -> mesh).
Hit: `GdxNativeMachineDraw(mesh, this)` reads the interpreter's current state -- MP matrix,
`current_lights`, `f3dflx_alpha_light`, `dmem`+`dma_io_dmem`, `prim/env/fog` colour, othermode,
geometry mode, tile descriptors -- and draws; the list is skipped. Miss: unchanged. This covers
every consumer for free and in-order (race body, ghosts with env alpha 160 + XLU mode, attack
highlight, menu/records/settings screens with viewports), keeps the shadow quads, crashed models
(`D_800CE080`, seg-7 binary), reflection pass and Create Machine on the interpreter, and needs
no decomp patch beyond a one-line generation bump in `func_8008D3C4/D5F4/D97C` so a rebuilt
slot invalidates its mesh. Killswitch `[native] machines=0` makes the registry lookup a no-op.
Receipt: `[native] machines=1 slots=N meshes=M draws=D verts=V us=T` every 64 frames.

### 3.2 Bake: lazy, on device, from the built list, at first use

First `G_DL` of an unregistered `D_800CDDB0[i]` pointer (generation-tagged) walks the 126-entry
list once with a ~150-line F3DEX2 walker (`G_TEXTURE, G_SETPRIMCOLOR, G_SETENVCOLOR,
G_SETCOMBINE, G_VTX, G_TRI1, G_TRI2, G_RDPPIPESYNC, G_ENDDL`; anything else = bake failure ->
fallback to interpreter + one log line). Output per (slot, lod):

- `runs[]`: `{tile, combineHi/Lo, prim: {kind: INHERIT|CONST, rgba}, env: {INHERIT|CONST},
  firstIndex, indexCount}` in emission order (xlu ordering is preserved because runs are drawn in
  order and the whole list runs at one othermode).
- `verts[]`: `{const Vtx* src, u16 count}` chunks referencing the o2r host arrays **in place**
  (no copy: pos s16, tc s16, normal s8 already host-endian); a chunk-relative `u16 indices[]`
  built from the tri commands. Total for 40 slots x 5 LODs: ~12k tris -> ~72 KB indices +
  ~2 KB material tables, regular heap. Nothing shipped in the repo, no offline resource.
- Textures: `(slot, tile) -> backend texture id + sampler cms/cmt + tile extents` resolved by
  running the slot's `D_800CDD38` list through the interpreter as today (it is ~10 commands)
  and calling `ImportTexture(unit, tile)` (`interpreter.cpp:2754`) for each tile the mesh uses,
  memoised per generation -- same texcache/atlas ids as the LUS path, so texture parity is by
  construction.

Bake cost: ~60 commands per LOD, 5 LODs x 30 slots ~ 9k commands, well under 5 ms, spread over
the first frames each LOD is seen. Memory: verts referenced in place; per-frame transient
vertices in the existing linear pool: ~20 B x ~3.6k visible verts (30 cars, mixed LODs; LOD1
avg 200 verts/101 tris, LOD5 avg 44/17) at 48 B packed = ~170 KB/frame, less than today's
3-verts-per-tri copies. Bake-time choice (boot vs offline): lazy on-device wins on every axis --
zero boot cost, no format to version, no derived data.

### 3.3 Runtime draw (per machine, per frame)

1. Compute light coefficients exactly as `CalculateNormalDir` does from the current modelview
   (once per draw), the model-space alpha light (`TransposedMatrixMul` + normalise), MP matrix.
2. One loop over the mesh's unique vertices (not per triangle): clip pos = MP x (x,y,z,1) with
   the same `AdjXForAspectRatio` branch the interpreter takes; rgb = ambient + lit; a = LUT
   lookup; uv = tc * 0xFFFF>>16 then the same tile normalisation the packed path applies
   (`AtlasFillTexParams` scale/clamp/offset, `gfx_citro3d.cpp:1907`); write 12 floats into the
   linear pool. The lighting/alpha/UV math is moved verbatim into a shared header
   `port/3ds/gfx/gdx3ds_vtx_math.h` and the interpreter patched to call it, so the two paths
   cannot drift.
3. Per triangle: reject test (`:4010-4019`) and CPU backface cull with the interpreter's
   winding rule; survivors append 3 x u16 to a per-draw index list in the linear pool.
4. Per run: set combiner/prim/env/tile through the interpreter's own material binding
   (`interpreter.cpp:4847-4856` `LookupOrCreateShaderProgram` + `Flush` + `LoadShader`,
   texture select) so the TexEnv program, constants and fog-blend stage are identical; then a
   new backend entry `gdx3ds_native_draw_indexed(vertsFloat*, nVerts, u16* idx, nIdx, aux)`
   that configures the same attribute layout and issues `C3D_DrawElements` inside the existing
   per-eye loop (factor `:2775-2823` into a helper). Mono/offscreen/stereo classification is
   unchanged because the projection compose is untouched.

Lighting choice: **CPU per-vertex** (justified: the FLX LUT alpha and the reject test need
per-vertex/per-tri CPU work anyway; ~4k verts/frame is tiny; PICA fragment lighting would be
per-fragment with different LUT semantics and break frame-hash parity; GPU T&L is a later
phase-2 item, and machine coordinates are small so the 24-bit-uniform issue seen on +/-32000
skyline quads in `gputransform-report.md` would not apply).

Prim/env INHERIT runs read the interpreter's current colours at hook time (the game sets env
right before the DL; prim may be the shadow pass's). Ghost alpha 160 comes through env.

### 3.4 Where it does NOT apply

Lists that fail to bake (unexpected opcode), `machineLod==6`, crashed models, Create Machine,
reflection pass, texrect/HUD. All fall back silently and are counted in the receipt.

## 4. Host differential harness

**Compile.** `machine_draw.c` builds on macOS with the game's defines plus ONE extra define:
the only SDK collision is `decomp/include/PR/os_libc.h:87 #include <strings.h>` pulling the
Darwin `string.h` (conflicting `memset`/`strchr`/`ssize_t` against `global.h:20` and
`libc/stdlib.h:21`). Verified (`cc -c` produces a 338 KB object, 0 errors):

```
cc -c -O1 -w decomp/src/overlays/ovl_i9/machine_draw.c -Idecomp -Idecomp/include -Idecomp/src -Iinclude \
  -DPORT=1 -DGDIFFUSER_PORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -DF3DEX_GBI_2=1 \
  -D_LANGUAGE_C=1 -DVERSION_US=1 -DASSET_VERSION=us -DASSET_REVISION=rev0 -DEXPANSION_KIT=1 \
  -DMFS_VERSION=1 -DBUILD_VERSION=VERSION_J -D_STRINGS_H_
```

Undefined symbols: 1372 VTX arrays, 96 textures, 9 EK grey textures, and
`D_800CE784 D_800DCAB0 func_i2_801039BC gBodyHealthValues gGreyscaleMachinePart gdx_io_read
gdx_rom_read32 osRecvMesg`. `port/gen/AssetBindings.c` also compiles with the same flags, so
the harness links it for the arrays and fills them from `fzerox.o2r` with a 60-line miniz
reader (`port/3ds/assets/third_party/miniz`, already used by `gdx3ds_fs_host_smoke`): key
`<yaml>/<symbol>`, skip 64 bytes + count, copy. Stubs for the 8 misc symbols. Host `Gfx` is 16
bytes (w1 pointer-width, `port/tests/gfx_pack_tests.c`); the walker must read `w1` as a pointer,
not a u32 -- the device walker is compiled with the same source, ILP32 makes it 8 bytes.

**Test A -- walker vs recorder (exactness).** For each of the 295 functions with the game's own
argument tables (stock: none; parts: sentinel colours 1..12 in every arg; `gGreyscaleMachinePart`
0 and 1): (1) run the function into a `Gfx[126]` buffer as `func_8008D3C4` does; (2) walk it with
the device walker -> mesh; (3) decode the same buffer with an independent Python decoder
(`tools/native/gfx_decode.py`, also used to read setup DLs from the o2r) -> triangle list with
per-triangle `(tile, combine, prim, env, vertex symbol+index)`; assert byte-equal after
canonicalisation. Also asserts: every list fits 126 entries, only the 9 expected opcodes occur,
`v0==0` always, no `G_DL`/matrix/geometry-mode inside, list count = 295.

**Test B -- colour attribution.** The recorder reports, per function, which prim/env values
are sentinel args vs literals, and which runs use PRIMITIVE/ENVIRONMENT before any set (the
INHERIT set). Expected: stock = literals only; parts = decal/number/cockpit args land exactly in
`gDPSetPrimColor`/`gDPSetEnvColor`; the INHERIT table is committed as a golden file and diffed.
Greyscale run diffs must be prim-colour/texture-only.

**Test C -- vertex math.** `gdx3ds_vtx_math.h` compiled on host against a double-precision
reference over 10k random (normal, light, ambient, alpha-light, LUT) tuples: shade rgb exact
(integer path), alpha LUT index exact, UV normalisation exact; plus the reject test on random
clip-space triangles vs the interpreter snippet copied verbatim.

All three run in `tools/ci-3ds.sh` next to the three existing host tests (`:325-341` pattern),
sub-second.

## 5. Device parity

Mechanism: frame hash (`[fh] frame=N crc=`, master plan §4.2, oracle track) on the recorded
replays with `[native] machines=0` vs `=1`, same build, same ini otherwise. Scenes: machine
select cycling all 33 characters (LOD1, viewports, `func_8009CCBC`), records/ghost screens
(env alpha 160), a 30-car GP race replay (all LODs, tunnels = alpha-light/ambient variants,
attack highlights), GP-end cutscene (fallback correctness, not native). Expected differences:
**none** (same shader ids, same texture ids, same per-vertex math, same reject/cull, same
order). Until `[fh]` lands: Azahar scanout SHOT diff at fixed ticks (existing autoinput).

Bisect on mismatch: `[native] machines_mask=<hex slot mask>`, `machines_lod_mask=<5 bits>`,
`machines_dump_frame=N` writes both paths' packed vertex+index streams and material tables for
one frame to the SD (`native-dump-<frame>.bin`) for a host diff; `machines_walk_only=1` runs
the mesh through the interpreter's own `GfxSpVertex/GfxSpTri1` (M2 stepping stone) to separate
"walk" from "T&L" regressions.

## 6. Perf target and measurement

Today (HW, crowd frame, render thread ~15 ms): `tri 4.2, vtx 1.0, drw 1.2, dsp 4.9` ms
(`[prof]` buckets, gputrace inflates). Machines are ~30 of the ~100 lists per frame and the
bulk of lit vertices. Target: `tri+vtx` for machines from ~2.5 ms to <0.8 ms (vertices written
once instead of 3x, no per-command dispatch, no per-tri pre-scan overhead), `drw` unchanged or
lower (one `C3D_DrawElements` per run, ~7 runs/machine as today). Measure: `[prof]` window on
the same replay frames before/after with gputrace off and on (both cited), the `[native]`
receipt (draws/verts/us per frame), `[c3d]` linear free KiB before/after (expect +100 KB
headroom from the smaller per-frame VBO use), `[gpu]` wP3D unchanged (no new GPU load).

## 7. Work breakdown

| M | Scope | Verification | Size |
|---|---|---|---|
| 0 | Worktree `native/machines`, ini `[native] machines` + masks, receipt line, progress file | builds, receipt visible, flag off = byte-identical hashes | 0.5 d |
| 1 | Host harness: `tools/native/machines_harness.sh`, walker as `port/3ds/gfx/gdx3ds_machine_walk.c` (host+device), o2r reader, Python decoder, tests A/B, golden INHERIT table | 295/295 pass, in ci-3ds.sh | 1.5 d |
| 2 | Device registry + lazy bake + `G_DL` hook patch + generation bump; `walk_only` mode replaying the mesh through `GfxSpVertex/GfxSpTri1` | frame-hash parity on all scenes (walk-only), bake receipts, no fallbacks in race | 2 d |
| 3 | `gdx3ds_vtx_math.h` shared with the interpreter (LUS patch), native T&L loop, reject/cull, indexed backend entry with per-eye loop, material binding through the interpreter | test C; frame-hash parity native vs LUS on all scenes; `[prof]` before/after | 3 d |
| 4 | Texture memo per (slot,tile,generation), INHERIT prim/env, ghosts/highlight/menus, Azahar soak, hardware card `~/Desktop/G-Diffuser-3DS-machines/` with TEST-PLAN | hardware hashes + perf window | 1 d |
| 5 | Patch consolidation (README order), docs, fallback audit, progress file final | ci green | 0.5 d |

Total ~8.5 agent days. Each milestone commits on `native/machines`.

## 8. Risks

- Another patch on LUS `interpreter.cpp` (hook + shared math); when Tier 1's own interpreter
  lands, the hook moves with it (the mesh/walker/backend entry are interpreter-agnostic).
- Prim/env INHERIT correctness depends on reading interpreter state at the hook -- covered by
  the golden table and the ghost/menu parity scenes.
- Exactly-degenerate triangles: CPU cull is kept, so no hardware-vs-CPU winding divergence.
- Texture identity: routing through `ImportTexture` keeps ids equal, but the texcache eviction
  path (`lus-3ds-texcache-*`) can rebind ids; memo must key on generation and re-resolve on
  miss (receipt counts re-resolves).
- Lazy bake on the render thread: heap-only, bounded (<126 commands), never linearAlloc.
- Old 3DS: unchanged memory shape (in-place vertices), smaller per-frame VBO use.
- Machine lists rebuilt mid-mode (records/ghosts at race load): covered by the generation
  one-liner; a missed site shows as a stale mesh in parity, bisected with `machines_mask`.

## 9. Dependencies

None on Tier 1: this runs on the LUS path first. Oracle track (`[fh]` + replay) for device
parity; until then SHOT diffs. Lexicon and CI patterns from `tools/ci-3ds.sh`. No ROM or o2r
in the repo; the host harness reads `fzerox.o2r` from the Azahar SD path or `$GDX_O2R`.

## 10. First 10 commands

```
1.  <scratch>/native/bootstrap.sh machines native/machines && cd ~/code/gdx-3ds/machines
2.  cc -c -O1 -w decomp/src/overlays/ovl_i9/machine_draw.c <flags above> -D_STRINGS_H_ -o /tmp/md.o && nm -u /tmp/md.o | wc -l
3.  cc -fsyntax-only -w port/gen/AssetBindings.c <same flags> -Iport      # host arrays for the harness
4.  unzip -l "$GDX_O2R" | grep -c 'machine_custom_gfx/\|machine_global_gfx/'   # 524 + 1088 entries
5.  python3 tools/native/gfx_decode.py "$GDX_O2R" machine_custom_gfx/D_303A6A8   # setup DL golden
6.  mkdir -p port/3ds/native tools/native && git commit --allow-empty -m "native/machines: M0 start"
7.  # write gdx3ds_machine_walk.c + harness main; run: tools/native/machines_harness.sh
8.  grep -n 'gfx_dl_handler_common' libultraship/src/fast/interpreter.cpp    # hook site (8052)
9.  cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-3ds --target G-Diffuser-3DS_3dsx
10. mkdir /tmp/azahar.lock && open -a /Applications/Azahar.app --args "$PWD/build-3ds/G-Diffuser-3DS.3dsx"   # walk_only parity run
```
