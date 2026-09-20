# 08 — Track: tessellation straight into a PICA VBO

Tier 2 item 2 of `00-master-plan.md` (§5). Track name `track`, branch `native/track`,
killswitch `[native] track` (default 0), receipt `[trk]`.

## 1. Goal

Keep `course.c`'s spline tessellator (game logic, deterministic, ~1100 lines of
per-frame float work) and redirect only its **output**: instead of writing `Vtx` into
`GfxPool.courseVtxBuffer` and emitting GBI into `gfxBuffer` for the LUS interpreter to
re-walk, the chunk writers feed a native sink that transforms to clip space on the CPU
and issues **one PICA draw per material run** instead of one interpreter display-list
call per 8-vertex chunk. Nothing else in the frame changes; the rest still goes through
the bridge + interpreter.

## 2. Code facts

### 2.1 The track emits no triangles in C

This is the finding that shapes the whole design. Grepping `course.c` for `gSP2Triangles`
outside `Course_DrawModel` returns **zero hits**. The ten chunk-group functions
(`decomp/src/game/course.c:3643-4265`) emit only:

- `gSPVertex(sCourseDisp++, gCourseVtxPtr, 8, D_800F89BC->loadVtxIndex)` — 8 verts into
  RSP vertex-cache slots 0-7 or 8-15 (`course.c:3673, 3713, 3829, 3939, 4045, 4101, 4169, 4212, 4239, 4255`),
  or `16, 0` on the pair-priming path (`3634, 3725, 3991, 4127, 4225`).
- `gSPDisplayList(sCourseDisp++, D_800F89B8->unk_04 …)` — a pointer to one of **20 fixed
  index templates** in segment 8 (`course_track_gfx`), wired once in `func_800A4BAC`
  (`course.c:3296-3317`).
- `gSPModifyVertex(..., loadVtxIndex + n, G_MWO_POINT_ST, …)` — retroactively patches the
  ST of the *previously loaded* 8 slots (e.g. `course.c:3649-3668`).

I dumped all 20 templates out of `fzerox.o2r` (`course_track_gfx/D_8014{0F0,138,180,1C8,
210,268,2C0,308,350,398,3E0,430,480,4D0,520,580,5E0,640}`). Each is a `G_TRI2` run over the
16-slot cache, 5-8 commands (10-16 tris); ten of them interleave `G_RDPPIPESYNC` +
`G_SETCOMBINE` pairs mid-template (the tunnel/cylinder interior/exterior material split).
`D_80140F0` = 8 × TRI2 = 16 tris, `(2,10,11)(2,11,3)(3,11,12)…(1,10,2)` — a closed
8-segment ring bridging chunk N (slots 0-7) and N+1 (slots 8-15). `D_8014138` is the same
with the halves swapped (parity), `D_8014180/1C8` the reversed winding.

**Consequence:** the index stream is a 20-entry static table, decodable once at load. The
native path needs no index generation, only a pointer→template map.

### 2.2 Vertices

`Course_SetupChunkDefaultVtxPos` (`course.c:3455-3486`) and
`Course_SetupChunkPipeTunnelVtxPos` (`3427-3453`) write 8 `Vtx` and bump `gCourseVtxPtr`.
Positions come straight from `SegmentChunk.referencePos1..8`, which are **`s16[3]` absolute
world coordinates** (`decomp/include/unk_structs.h:351-358`), rounded from float in
`Course_ChunkCalculateCommonRoadReferencePos` (`course.c:1585-1632`). UVs come from
`chunk->{left,right,top,bottom}TextureCoord` (or the `…Correction` variants when
`TRACK_FLAG_80000000` is set) crossed with the 5-entry `D_800F8930[]` S table loaded from
the material descriptor (`course.c:3488-3630`). Colours are never written per frame: all
4096 `courseVtxBuffer` entries are set once to rgb=0, a=255 in `func_800A4BAC`
(`course.c:3325-3333`).

Buffers: `GfxPool.courseVtxBuffer[4096]`, `gfxBuffer[8193]` (`decomp/include/sys.h:86-87`);
rearmed per frame at `decomp/src/overlays/ovl_i2/race.c:189`.

### 2.3 State: two setup lists, decoded

`Course_Draw` emits `gSPDisplayList(D_8014008)` then `D_8014078` then fog/prim
(`course.c:4979-4994`). Decoded from the o2r:

- **`D_8014008`** — `GEOMETRYMODE clear=all`, then `set = 0x00A10405` =
  `G_CLIPPING|G_SHADING_SMOOTH|G_FOG|G_CULL_BACK|G_SHADE|G_ZBUFFER`;
  `G_TEXTURE off`; `PIPESYNC`; `SETOTHERMODE_L rendermode = 0xFA006220`;
  `SETCOMBINE 0xFC147E28 44FE793C`.
  `0x6220` = `FORCE_BL|ALPHA_CVG_SEL|CVG_DST_FULL|Z_UPD`, `ZMODE_OPA` — **`Z_CMP` is
  clear**: the track writes depth but does not test it. It is correct only because
  `Course_Draw` sorts chunk groups far→near (`course.c:5032-5052`). *Never reorder draws.*
  `0xFA00` = `GBL_c1/c2(G_BL_CLR_FOG, G_BL_A_SHADE, G_BL_CLR_IN, G_BL_1MA)` =
  `G_RM_FOG_SHADE_A` in both cycles — fog is a blend of fog colour by **shade alpha**.
- **`D_8014078`** — 8 `SETTILE` + 6 `SETTILESIZE`, tiles 0-3 and 4-7; `gSPTexture(…,
  D_800F89D4, G_ON)` picks base tile 1 or 5 (`course.c:4330-4334`).
- **`D_8014040`** (the `func_800A95B4` preview pass) is the same shape but
  `set = 0x00800405` — no `G_FOG`, no `G_SHADING_SMOOTH`, rendermode `0x00552078` =
  `G_RM_AA_ZB_OPA_SURF`. Out of scope: course-edit only (`191080.c:241`).

Material transitions are raw, gated on `sLastTrackShapeType` (`course.c:4325-4348`,
mirrored at `4553-4576`): `gDPLoadSync` + `gDPSetTextureImage(RGBA, 16b, width, texture)`
+ `gDPLoadBlock(tile, 0, 0, 2047, CALC_DXT(width,16b))`. Descriptors are
`D_800CF8C8[shape][type]` (`course.c:163-172`), 8 shapes × ≤3 types, so **≤24 distinct
track textures per course**, all RGBA16, 2048 texels each.

Per-frame animated state: `gDPSetPrimColor(0,0, 255-(frame*8)%256, 0,0,255)` — the rail
chevron strobe (`course.c:4988-4994`); `gSPFogPosition(sCourseFogStartDistance, 1000)`
(`4982`); `gDPSetFogColor` computed from `courseFogColors` with a tunnel-mouth fade
(`4996-5027`). All three are constant across the whole track pass.

### 2.4 The matrix

`Course_Draw` loads **no** matrix. `Background_Draw` runs immediately before it for the
same viewport (`race.c:195-231`) and loads
`gSPMatrix(&D_1000000.unk_20208[cameraIndex], G_MTX_LOAD|G_MTX_PROJECTION)`
(`ovl_i3/background.c:1090`) plus an identity modelview (`background.c:1100`). That Mtx is
`Camera_MatrixToMtx(&camera->projectionViewMtx, …)` (`camera.c:1253`). So the track's
transform is exactly `camera->projectionViewMtx`, available in C as an `MtxF`, and the
modelview is identity. Confirms "world space, no modelview".

### 2.5 Culling

CPU, in `Course_Draw`: depth `< 0` or `> sCourseFarRenderDistance` (= renderOrigin + 4500,
`course.c:3416-3417`), then NDC-x outside ±1 (±`gdxCullX` under ultrawide)
(`course.c:4696-4718`). Sets `chunk->drawState`. Chunk groups are then built by depth band
(`4919-4976`, ≤`SEGMENT_CHUNK_GROUP_COUNT`=64) and drawn furthest-first. A distance-based
chunk-skip LOD lives inside the group walk (`course.c:4293-4317`, `D_800F894C/8950`).

### 2.6 Per-frame counts

No committed `[cull]` capture exists — `port/3ds/patches/decomp-race-cull-diagnostics.patch`
already emits `[cull] cam=%d cnt=%d pass=%d neg=%d far=%d` (`course.c:4781-4786`) and
`[grp]` spans, but no log in `docs/research/` contains its output. **Measuring this is
milestone 0.** Bounds and the best proxies today:

- Structural: `SEGMENT_CHUNK_COUNT` 0x400, `gSegmentChunks[1025]`; the emitter stops at
  `gCourseVtxPtr >= courseVtxBuffer[0xFF0]` → **≤510 chunk writes/frame**, ≤64 groups.
- Hardware census, whole frame (`m1/docs/research/locked60-campaign.md:9-13`):
  `[race-dl] lists=70 cmds=1486 tri=222 trect=41`; opcodes `06 tri2=2.4-4.4 ms/143-270`,
  `01 vtx=0.6-1.2/28-79`, `F3 loadblock=1.9-2.4/144-199`.
  The `lists=70` is dominated by the track: every chunk draw is one `gSPDisplayList`.
- Working estimate to be confirmed at M0: **~50-70 chunk draws/frame**, ~400-1100
  track triangles, ~450-600 track verts loaded, and **4-15 material runs** (transitions of
  `shape|type` in far→near order).

### 2.7 Precision

`gputransform/docs/research/gputransform-report.md:138`: "PICA vertex uniforms are 24-bit
floats and the skyline quads span ±32000 units, so the composed-matrix path loses ~1px of
position precision"; on hardware it produced "venue-floor texture-map breaks"
(`m1/docs/research/RESUME-2026-09-02.md:32-38`). Track positions are `s16` absolute world
coordinates (§2.2), i.e. squarely in that range. **GPU transform of the track as-is would
reproduce the documented failure.** Camera-relative coordinates were never tried (grep of
both doc trees finds no mention).

### 2.8 Gadgets are not in scope

`course_gadgets.c` authors **no** per-frame vertices: landmine/jump/dash/terrain verts are
baked once into `D_80225800` at race init, decorations are static ROM DLs placed by a baked
`Mtx`. It runs as a separate pass right after `Course_Draw` (`race.c:196/197` …), emitting
`gSPFogPosition(980,1000)` (`course_gadgets.c:543`), `G_CULL_BACK` toggles (`541,
1050-1147`) and `G_RM_FOG_SHADE_A`-class modes (`551-553, 707-709`); no decal modes in
retail. Guard rails are *track shape* geometry, not gadgets. Gadgets belong with the
machines/static-mesh track — but this track must **leave interpreter state consistent** for
the pass that follows it.

## 3. Design

### 3.1 The shim (patch surface)

A new header `port/3ds/gfx/gdx3ds_track_shim.h`, included from `course.c` behind
`#if defined(PORT) && defined(GDX_NATIVE_TRACK)`, redefines ten macros to native calls:
`gSPVertex`, `gSPDisplayList`, `gSPModifyVertex`, `gSPTexture`, `gDPSetTextureImage`,
`gDPLoadBlock`, `gDPLoadSync`, `gDPSetPrimColor`, `gDPSetFogColor`, `gSPFogPosition`,
each swallowing its `Gfx*` argument. **The C bodies of all ten group functions, both chunk
walks and both setup paths stay byte-identical.**

Three guarded hook points: `#include` at the top; `gdxtrk_begin(cameraIndex, &sp60)` after
the matrix is composed in `Course_Draw`; `gdxtrk_end()` before its `return`. Plus
`#pragma push_macro`/`pop_macro` around `Course_DrawModel` (`course.c:4479-4507`), which
uses the real `gSPVertex`/`gSP2Triangles` for the course-select menu model. Estimated
decomp patch: **~50 lines**, one file. When `gdxtrk_begin` reports inactive the macros
forward to the real GBI ones, so the ini killswitch is exact at runtime.

### 3.2 Sink model: mirror the RSP vertex cache

`gSPModifyVertex` edits slots already loaded, so a flat append is wrong. The sink holds a
16-slot cache of `{s16 pos[3]; s16 st[2]; u8 rgba[4]}` mirroring the RSP's, plus a
`transformed[16]` shadow of clip-space `{x,y,z,w}` with a per-slot dirty bit.

- `gdxtrk_vertex(const Vtx* v, int n, int idx)` — copies `n` entries from the game's own
  `courseVtxBuffer` into slots `idx..idx+n-1` and transforms only those (positions of the
  other half are unchanged, so their clip-space values are reused).
- `gdxtrk_modify_vertex(int slot, u32 st)` — patches ST only; position shadow untouched.
- `gdxtrk_dl(const Gfx* p)` — looks `p` up in the load-time template map and emits that
  template's triangles.

Emission per template: append the 16 cache slots as 16 packed vertices, then append the
template's `3 × ntris` `u16` indices with the current base. **16 transformed verts and
48 indices per chunk instead of 48 fully-written vertices** — 864 B vs 2304 B of VBO
traffic, and 8 (not 48) `dp4×4` transforms per chunk since only the freshly loaded half is
new. A template carrying mid-list `SETCOMBINE` splits into two runs at that point.

### 3.3 Transform: CPU, and why

**Decision: CPU, plain `float`, matrix hoisted into 16 locals.** The 3DS ARM11 MPCore has
VFPv2 only — no NEON — so there is no vectorisation to buy; devkitARM emits scalar VFP
either way. Cost: 8 verts × 4 `dp4` = 128 mul + 96 add per chunk; at 70 chunks that is
~16k FLOPs/frame, well under 0.1 ms. Stereo is a backend replay of the same VBO
(§3.6), so this is paid once per eye-pair.

Against GPU T&L: (1) **precision** — §2.7, the track is exactly the geometry class that
broke; (2) **no headroom** — hardware `vtx=0.7 ms` for the *whole frame*, and
`locked60-campaign.md` records "GPU vertex transform is NOT a lever for this game — it is
shelved for good"; (3) **cost is elsewhere** — `dsp 6.5-7.3`, `br 3.2-3.8`, `tri 3.4-3.6`,
`drw 2.3-2.5`. This plan attacks command dispatch and draw count, not vertex math.

**Documented reopening path (not in scope):** translate to camera-relative on the CPU
before upload — write `p - eye` as the vertex position and hand the GPU
`projectionViewMtx · T(eye)`. `|p - eye| ≤ sCourseFarRenderDistance ≈ 5500` by
construction (the cull guarantees it), so float24's ~17-bit mantissa leaves ~4 fractional
bits, versus ~0 at ±32000. That is the standard remedy the shelved branch never tried, and
it would also unlock `GPU_SHORT` position attributes. Record it; do not build it.

**Parity nuance:** the interpreter transforms with the *fixed-point* `Mtx` (`unk_20208`,
s15.16 via `Camera_MatrixToMtx`). To keep the native path numerically comparable, take the
same matrix and convert it the way the interpreter does, rather than using
`camera->projectionViewMtx` directly — otherwise the native path is *more* accurate and
frame hashes differ for a reason that is not a bug.

### 3.4 Fog

Reproduce `Interpreter::GfxSpVertex` exactly
(`libultraship/src/fast/interpreter.cpp:3803-3818`):

```
fogRatio = (w <= 0.001f) ? -1.0f : clamp(z / w, -1.0f, 1.0f);
f        = clamp(fogRatio * fog_mul + fog_offset, 0.0f, 255.0f);   // stored as u8
```
`fog_mul`/`fog_offset` are the two halves of `gSPFogPosition`'s moveword
(`interpreter.cpp:5839-5844`). The factor goes into packed slot `out[9]`
(`gdx3ds_vbopack.h:63-68`), and the backend's combiner mapper already appends the
`GPU_INTERPOLATE` TexEnv stage keyed on `GPU_PRIMARY_COLOR`'s alpha
(`gfx_citro3d.cpp:1369-1385`) whenever the shader is created with the same combiner key
and `opt_fog`. Fog colour rides that stage's constant, refreshed per draw from the aux
record (`gfx_citro3d.cpp:1565-1572`). So fog is free if we drive the same shader program;
we must not fall into the depth-LUT path (`RESUME.md:42-49` — the track lives in
`d ∈ [0.95, 1.0]` and the LUT cannot discriminate it).

### 3.5 Batching and state

One `Gdx_NativeBatch` run per material change **in emission order**. A run ends on: a
`shape|type` transition (new `LoadBlock`), a `gSPTexture` tile change, a mid-template
`SETCOMBINE`, or the pass end. Runs are never merged across a gap and never sorted —
`Z_CMP` is off (§2.3).

State per run: bound texture id (our own `NewTexture`/`UploadTexture` ids from a load-time
address→id map over the ≤24 descriptors, bypassing `TextureCacheKey`),
`SetDepthTestAndMask(false, true)`, `SetUseAlpha(false)`, `SetZmodeDecal(false)`, the track
combiner via `LookupShader`/`CreateAndLoadNewShader`+`LoadShader`, and
`RefreshStageConstantsPacked` for prim/fog. UVs must be pre-scaled into padded texture
space (`gfx_citro3d.cpp:2529-2533`).

Backface culling: the track sets `G_CULL_BACK`, and today the interpreter culls per
triangle on the CPU while the backend holds `C3D_CullFace(GPU_CULL_NONE)`
(`gfx_citro3d.cpp:1008`; `tunnel-roof-cull-analysis.md`). The native path sets
`GPU_CULL_BACK_CCW` for its runs and restores `GPU_CULL_NONE` before returning, moving
the cull to the GPU for free. The portrait fixup's xy block has det = +1 so winding is
preserved, but this must be A/B'd on a tunnel and a cylinder (the known-sensitive shapes).

Two-stage VBO plan:
- **M2 (non-indexed)** reuses `VboPackBegin` + `DrawPackedBatch`
  (`gfx_citro3d.h:170-171`) with zero new backend code — 48 verts written per chunk into
  the per-frame 1 MiB `mVboPool` (`gfx_citro3d.cpp:526`, reset each `StartFrame`, ~76 KB
  needed). This alone removes every track `Gfx` command from the bridge and interpreter.
- **M4 (indexed)** adds `DrawPackedIndexed` inside `gfx_citro3d.cpp` (it must live in that
  TU: `sDispProjLast` is a file-static that any custom uniform upload has to invalidate)
  using `C3D_DrawElements(GPU_TRIANGLES, n, C3D_UNSIGNED_SHORT, idx)` — the first indexed
  draw in the tree; the index buffer must be `linearAlloc`'d.

### 3.6 Stereo

Nothing to do. The backend re-issues the same VBO per eye, changing only the `projection`
uniform and the target (`gfx_citro3d.cpp:2811-2821`). Call
`gdx3ds_stereo_set_draw_class(GDX3DS_STEREO_SCENE)` explicitly rather than relying on the
`bufVbo[3] == 1.0f` ortho heuristic. Viewport fold: if `mVpAffineActive`, the run's
matrix must fold `mVpSx/Sy/Tx/Ty` exactly as `gfx_citro3d.cpp:2757-2774` does — relevant
for 2-4P split screen, where `Course_Draw` runs once per viewport.

### 3.7 Shared native draw API

Both this track and the machines track need the same primitive. Proposed
`port/3ds/gfx/gdx3ds_nativebatch.h`:

```c
void Gdx_NativeBatchBegin(Gdx3dsStereoClass cls);
void Gdx_NativeBatchMaterial(const Gdx3dsNativeMaterial* m);  /* tex id, shader key,
                                                                 depth/blend/cull, prim,
                                                                 env, fog rgb+line */
float* Gdx_NativeBatchVerts(u32 nVerts);      /* returns writable packed slots, stride 12 */
u16*   Gdx_NativeBatchIndices(u32 nIndices);  /* NULL until M4 → caller emits expanded */
void   Gdx_NativeBatchFlush(void);            /* ends the current run, issues one draw */
void   Gdx_NativeBatchEnd(void);              /* restores cull/depth/uniform state */
```
Implemented in `gfx_citro3d.cpp` (same TU, for `sDispProjLast` and the private texture
table). The machines track consumes the identical API with a persistent, non-per-frame VBO
— which is the one extension it needs and this track does not (`mVboPool` is reset every
`StartFrame`, `gfx_citro3d.cpp:1151`).

## 4. Milestones

**M0 — Measure (0.5 d).** Arm the existing `[cull]`/`[grp]` diagnostics, add a `[trk]`
census on the stock path counting chunk draws, verts, triangles and `shape|type`
transitions per frame per viewport. *Verify:* one hardware and one Azahar capture of a
crowd lap; numbers land in the progress file. **This replaces every estimate in §2.6.**

**M1 — Template table + host test (1 d).** Decode the 20 segment-8 templates at course
load into `{u16 idx[48]; u8 ntris; u8 splitAt;}` and a pointer→template map. Commit the
decoded golden table. *Verify:* a host unit test in `port/tests/` (precedent:
`gfx_pack_tests.c`) that runs the decoder over the 20 committed blobs and asserts the exact
index arrays — the one genuinely host-testable piece, and the one most likely to break
silently.

**M2 — Sink + non-indexed draw (3 d).** The shim header, the 16-slot cache, the CPU
transform, fog, material runs, `VboPackBegin`/`DrawPackedBatch`. *Verify:* `[trk]`
receipts match M0's counts exactly on both paths; frame-hash parity on the recorded
replays with `[native] track` 0 vs 1 (masks only where the oracle already masks); tunnel
and cylinder A/B screenshots for the winding check.

**M3 — Hardware profile + gadget-pass hygiene (1 d).** Confirm the interpreter state the
gadget pass inherits is unchanged (it emits no geometry mode of its own for landmines /
jumps / effects — `course_gadgets.c` §2.8). *Verify:* HW `[prof]` window with `gputrace`
off, before/after, plus a 30-minute soak with 0 errors.

**M4 — Indexed draw (2 d).** `DrawPackedIndexed` + `C3D_DrawElements`; 16 verts + 48
indices per chunk. *Verify:* frame-hash parity unchanged vs M2; `[prof]` delta reported
separately so the indexed win is attributable.

**M5 — Tighten (optional, 1-2 d).** Frustum-y cull (the game tests only NDC-x); merging
adjacent same-material runs where depth ordering permits. *Verify:* parity must still hold;
if not, the merge is unsound and is dropped.

Total ~8-9 working days, matching the master plan's "Track: 2 weeks".

## 5. Perf target

Baseline (HW, `locked60-campaign.md:9-13`): `br 3.2-3.8, dsp 6.5-7.3, vtx 0.7,
tri 3.4-3.6, imp 1.2-1.8, drw 2.3-2.5`; `nD=110-123, nT=550-570, lists=70`.

The track pass leaves the bridge and interpreter entirely: ~50-70 `gSPDisplayList` + ~60
`gSPVertex` + ~400-500 `G_TRI2` executions per frame gone, ~40-60 backend draws collapsed
to 4-15.

- On today's LUS stack: **`br` −0.6 to −1.0, `dsp` −1.2 to −2.0, `tri` −0.8 to −1.4,
  `drw` −0.5 to −0.9** → **2.5-4.0 ms** off the render thread in a crowd frame.
- Stacked *after* Tier 1 (which already zeroes `br`): **1.8-3.0 ms**.

Claimed only as a range until M3 produces a `[prof]` window. gputrace state must be stated
with any number.

## 6. Risks

1. **`gSPModifyVertex` semantics.** If any template consumes a slot *before* a later
   `ModifyVertex` patches it, the snapshot-per-template model is right and a shared-vertex
   model would be wrong. Mitigated by snapshotting; costs VBO bytes, buys correctness.
2. **Winding / near-plane.** Moving the cull to the GPU and dropping CPU clipping is the
   tunnel-roof risk (`tunnel-roof-cull-analysis.md`; the gputransform branch saw "no
   wedges" once the PICA did the clipping, which is encouraging). A/B on tunnel +
   cylinder is a gate, not a nice-to-have.
3. **Painter's order.** Any batching that reorders across chunk groups is silently wrong
   because `Z_CMP` is off. Enforced by never sorting; assert in debug that run emission
   order equals group emission order.
4. **Split screen.** `Course_Draw` runs 2-4× per frame with viewport fold active; the fold
   must be folded into the run matrix or the native track lands full-screen.
5. **Matrix quantisation** (§3.3) — a parity trap, not a correctness one.
6. **No host harness for the tessellator.** `course.c` includes `global.h` + five `fzx_*`
   headers and needs `gSegmentChunks`, `gCourseInfos`, live camera state and asset symbols;
   it is not host-compilable in isolation. Parity rests on (a) the M1 host template test and
   (b) device `[trk]` receipts + frame hashes on both paths. Say so in every parity claim.
7. **Effect/landmine setup DLs** (`aSetupCourseEffectTextureDL`, `aSetupLandmineTextureDL`)
   are undecoded opaque blobs; if the native track leaves tile state different from the
   interpreter's, the gadget pass that inherits it breaks. M3 guards this.

## 7. Dependencies

- **Oracle (03) — hard.** Frame hashes and deterministic replays are the only parity
  mechanism available (see risk 6). Nothing here can be claimed verified without it.
- **Tier 1 interpreter — not required.** The sink bypasses the LUS path for the track only;
  the rest of the frame keeps using it. If Tier 1 lands first the gain shrinks (§5) but
  nothing needs rework.
- **Machines track — shared.** `Gdx_NativeBatch` (§3.7) is defined here; whichever track
  lands first implements it, the other consumes it. The one divergence to agree up front:
  machines need a persistent VBO, the track wants the per-frame pool.

## 8. First 10 commands

1. `<scratch>/native/bootstrap.sh track native/track` — worktree at `~/code/gdx-3ds/track`.
2. Create `docs/research/native/track-progress.md` (state / next step / gotchas); commit.
3. `mkdir -p tools/dev && python3 tools/dev/dump_track_dls.py` — extract the 20
   `course_track_gfx/D_8014*` blobs from `fzerox.o2r`, decode `G_TRI2`, and write
   `port/3ds/gfx/gdx3ds_track_templates.inc` (the golden table). Commit tool + table.
4. Write `port/tests/track_template_tests.c` asserting that table against a re-decode;
   wire into the existing host-test target; `ctest` green.
5. Add the `[trk]` census to stock `course.c` under `[debug] trkcensus` — chunk draws,
   verts, tris, `shape|type` transitions, per viewport. Commit.
6. Build 3dsx; take the Azahar lock; run a scripted crowd lap with `trkcensus=1` and
   `[cull]` armed; snapshot `log.txt` to `/tmp/track-art/`.
7. Record the M0 numbers in `track-progress.md`; replace §2.6's estimates.
8. Stage the same build to `~/Desktop/G-Diffuser-3DS-track/` with a TEST-PLAN for a
   hardware `[trk]` + `[prof]` baseline; note in the report that a human run is needed.
9. Write `port/3ds/gfx/gdx3ds_track_shim.h` (macros + `gdxtrk_*` prototypes) and
   `gdx3ds_track_sink.cpp` as a **no-op recorder**: counts and checksums, renders nothing.
   `[native] track=1` must give a black track with `[trk]` counters identical to stock —
   proving the shim captures exactly the intended call set before any rendering exists.
10. Land the decomp patch `decomp-native-track-shim.patch` (~50 lines) into
    `port/3ds/patches/` with its README entry and ordering; confirm the stale-object trap
    (touch `course.c`, verify the `.o` rebuilds); commit.
