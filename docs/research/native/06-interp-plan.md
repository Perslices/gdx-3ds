# 06 — Tier 1: an owned F3DEX2-to-citro3d interpreter (track "interp")

Date: 2026-09-07. Planning only; nothing implemented. All paths are relative to the
mainline worktree `~/code/gdx-3ds/m1` unless stated. Line numbers are from that tree.

## 1. Goal

Replace `libultraship/src/fast/interpreter.cpp` (10,466 lines, 36 patches) and
`port/n64_gfx_bridge.cpp` (12,353 lines) with ~5k owned lines under
`port/3ds/gfx/n64/` that read the game's own `Gfx` stream, keep our citro3d backend
(`port/3ds/gfx/gfx_citro3d.cpp`) unchanged, and produce frame-hash-identical output
behind `[native] interp=1`. Success gate (master plan §6): parity on all replays (emu +
HW), render thread ≤ 9 ms in a crowd frame (HW), 0 errors over a 30-min soak.

## 2. Current-path facts

### 2.1 Command widths and the ILP32 truth

The brief's "8-byte Gfx" premise is wrong for the game side. Under `PORT` the decomp's
`Gfx` is 16 bytes on every host: `Gwords { u32 w0; u64 w1 }` (`decomp/include/PR/gbi.h:1715-1746`),
padded on ILP32 (`GwordsStatic32 {w0,_pad,w1lo,w1hi}` :1754-1759). `decomp-ilp32.patch`
tags every host pointer with `GDX_GFXW1_HOST_TAG` = "GDX1" in `w1hi` (:1739-1741);
segment tokens and value operands have `w1hi == 0`. LUS's `F3DGfx` is
`{uintptr_t w0, w1}` = 8 bytes on ILP32 (`libultraship/include/fast/lus_gbi.h:1161-1182`).
So on 3DS the bridge *narrows* 16 → 8 and turns the tag into a pointer/segment decision
(`port/n64_gfx_bridge.cpp:6418-6426`).

Three command-source classes exist (`CommandStrideForSource` :4742-4757,
`CommandSourceIsBigEndian` :4862-4876):

| Class | Memory | Stride/endian | Registered by |
|---|---|---|---|
| Host-built (GfxPool `D_1000000`, RDRAM arena) | recompiled C | 16 B, LE, tagged w1 | `gdx_register_host_wide_command_range` (`port/decomp_port.c:1380`) |
| Segment-image GFX ranges (setup_gfx, hud_gfx, machine_models, course gfx, EK) | verbatim ROM slice from `segment_blob/*`, bswap32-fixed in place by `gdx_fixup_asset_segment_image` (`port/gen/AssetBindings.c:9194`) | 8 B, host-endian | `gdx_register_asset_segment_command_ranges` (:9221) → `gHostN64CommandRanges` (bridge :8239) |
| Untagged raw ranges | whole segment images (`gRawN64Ranges`, bridge :1454) | 8 B, big-endian, sniffed by `IsLikelyBigEndianDisplayList` | :1455 |

Placeholder symbols (`D_3000510`, `aSetupBoosterDL`; 2,526 rows in `sAssetSegmentMap`,
`AssetBindings.c:4689-7217`) are BSS stubs holding no data; the bridge resolves them
into the loaded segment image via `gdx_lookup_asset_segment[_interior]` (:7267/:7299)
→ `ResolveGeneratedAssetStub` (bridge :1918) → `EnsureAssetSegmentImage` (:1230-1463).
`IsAssetPlaceholderPointer` (:1199) forces the tagged pointer to be *not* used verbatim.

### 2.2 The o2r reality (this deletes the whole OTR opcode family)

fzerox.o2r (3,610 entries) holds 203 `DisplayList` resources (28 distinct opcodes, all
f3dex2, every one prefixed by `G_MARKER`), 1,588 `Vertex`, 1,596 `Texture` (RGBA16 992,
I4 451, CI4 55, CI8 46, I8 32, IA8 13, IA4 5, IA16 2), 0 `Matrix`, 0 `Light`
(census script `<scratch>/native/interp-census/census.py`). **None of the 203 DLs is
ever executed**: `GDiffuser_LoadAsset` (`port/AssetLoader.cpp:28`) has zero callers,
`gF3DAssetRanges` is never populated (bridge :625, :1610), and the bridge emits exactly
one OTR opcode, `G_SETTIMG_OTR_FILEPATH` (0x25, :464, :7073-7077), solely to give the
LUS texcache an immutable string identity (:7024-7026). The texel bytes themselves are
present in the segment image (`CopyRawTextureBytes` :961-975 already reads them raw),
so a new interpreter reads N64-format texels at the host pointer and needs neither the
LUS resource manager nor any `*_OTR_*` handler. The o2r archive stays only as a blob
source (`GDiffuser_LoadArchiveFileBytes` :92, `port/gdx_segment_source.c:294`).

Runtime macro census (`decomp/src`, 70 unique): gSP2Triangles 5,892, gSPVertex 1,501,
gSPTexture 1,410, gDPPipeSync 1,388, gDPSetPrimColor 1,198, gDPSetCombineLERP 525,
texrects 273, gSPMatrix 99, gSPModifyVertex 88, gDPLoadTLUT 27,
gSPSetExtraGeometryMode 31, gSPLoadUcodeL 12, gSPLineW3D 10, gSPPopMatrix 6.

### 2.3 What the LUS interpreter does per frame

- `Run()` (`interpreter.cpp:10016-10143`): `SpReset` (9745), backend frame begin +
  clear (10051-10058), exec-stack walk via `gfx_step` (9681-9743, 256-slot
  `sFlatDispatch`), final `Flush` (10093). One pass per call.
- `GfxSpVertex` (3512-3840): MVP hoisted (3524), transform, `AdjXForAspectRatio`,
  UV scale, lighting with `CalculateNormalDir` (3316) for `current_num_lights-1`
  lights + 2 lookat, texgen (3690-3745; FZeroFlx alpha-light from `dmem`), `clip_rej`
  (L/R/B/T/far, 3755-3773), 7-bit `clip_out` (3780-3806), fog `z/w*mul+off` → `color.a`
  (3813-3840).
- `GfxSpTri1` (3904-5605): reject-variant screen (4013-4025), Sutherland-Hodgman
  clipping on 7 planes (w ≥ 1e-5, ±x, ±y, ±z; 4032-4121), CPU cull (4133-4185),
  state diffs (4221-4259), combiner key (4261-4420), memoized tile/import
  (4487-4801), shader select (4846-4862), packed-VBO write (4896-5306), `Flush` at
  256 tris (5600). `Flush` (663-693) → `gdx3ds_vbopack_draw`.
- TMEM: `tmem[4096]` mirror + 512 per-word `LoadedTexture` views with span store
  (h:294-358), `GfxDpLoadBlock` 6507-6719, `GfxDpLoadTile` 6721-6893, same-content
  skip 6481-6505, `[tmem2]` fast teardown 6140.
- Texcache (h:414-418): address-hashed map + LRU, 1024 entries, key = addr, palette
  addrs, fmt/siz/palette, sizes, cms/cmt/masks, `tmem_content_hash` (FNV over exactly
  the decoded span, 2965-3020) or resource path hash. Decoders `ImportTexture*`
  1771-2752 into `mTexUploadBuffer` → `UploadTexture`.
- Combiner: `ColorCombinerKey` (h:92-100) → `GenerateCC` (827-1063) → `(id0,id1)`;
  the backend decodes them with `gfx_cc_get_features` (10358).

### 2.4 Backend contract (kept as-is)

`Fast::GfxRenderingAPIC3D` is a public class (`Gdx3ds_GetCitro3dRenderer()`,
`gfx_citro3d.cpp:3467`); the new interpreter calls it directly. Per-frame order (cpp
lines): `StartFrame` 1126 → `ClearFramebuffer` 3274 → per batch `LoadShader`/
`CreateAndLoadNewShader` 1593/1610 (input LUS `(id0,id1)`; `MapCombiner` 1324 → ≤4
TexEnv stages), `SelectTexture` 1649 + `UploadTexture` 1950 (RGBA32 in, Morton, pow2
pad, 1024 max) + `SetSamplerParameters` 2048, depth 2101, decal 2110, `SetUseAlpha`
2293, alpha threshold 2322, `SetViewport` 2173 (fold 2198-2223), `SetScissor` 2240,
prim depth 2317, `gdx3ds_fog_note_params` 168 → `VboPackBegin` 2855 (12-float stride,
256-tri max, 1 MiB per-frame bump pool) → `DrawPackedBatch` 2884 → `EndFrame` 1186.
Stereo is per-draw dual emission inside `DrawTriangles` (2775-2827), no re-run. Cull is
`GPU_CULL_NONE` (994): culling is the interpreter's job. Everything runs on the render
thread (`gdx3ds_renderthread.h:3-21`).

### 2.5 Bridge and render thread per frame

Render thread (`port/3ds/gdx3ds_renderthread.cpp`): the game fiber submits one TASK per
frame into a ring (:150-170), snapshotting `gSegments` (:164); `[debug] renderthread`
(:297, receipt :299) selects core-2 execution. In bridgemain mode the main thread runs
`gdx_gfx_job_prepare` (:403) — the 16→8 conversion pre-pass — and the render thread runs
`gdx_gfx_job_run` (:206) under the job's segment view (`gdx_gfx_segment_view_set`) with
`gdx3ds_dynlod_task_begin/end` around it; `gdx_gfx_job_release` (:277) merges walk-time
segment claims back (`sTaskSegsOut`). Fences: `gdx3ds_rt_fence` (:484) and
`gdx3ds_rt_fence_dma` (:496, ahead-mode RDRAM writer). One task in flight.

Bridge: `GdxGfxJobRun` (:10540) converts via `ProcessList` (:6257) with memoized
`TryResolveAddress` (:5000) and the segment mirror into `mSegmentPointers` (:2964), then
`interp->Run` pass 0 (:11518, always, t=1). The second `Run` (:11593) executes only when
`interpActive`, i.e. env `GDX_INTERP_P0/P1` (`port/gdx_interp.cpp:96`; not an ini key,
unset on 3DS) — so the shipped 3DS build is single-pass. Epilogue: `gHostFrameGfxTaskRan`
(:11599, VI-fallback guard), `GdxUpdateFrameMirror` (:11646), transition-capture ticks
force t=1 (:11490-11495).

### 2.6 What must survive bridge deletion

Game-facing behaviour that lives in the bridge today and moves, not dies:
segment-image loading + first-claim `gSegments` store (:1230-1463), placeholder-stub
resolution (:1918), range registrations (:8233-8290; callers in `decomp_port.c` and
`AssetBindings.c` stay), `gNativeRgba16Ranges` host-order texel classification (:961-975),
the DMA-range fence for the render thread (`GdxRtFence`, :1451), the F3DEX2 variant
switch marker (`F3DEX2_VARIANT_SWITCH_MARKER` 0x47445800, `interpreter.h:616`) emitted
for `gSPLoadUcodeL`, `G_LINE3D` handling (bridge rewrites to `OTR_G_LINE3D_GDX`),
VI fallback present, transition capture (`ReadFramebufferToCPU`), frame mirror, and
the `[prof]`/`[c3d]` receipts. Bridge-only glue that dies: narrowing, `MakeLusGfx` (:497),
conversion caches/epochs, the o2r key rewrite (:7042-7077), persistent Vtx/texture
copies (:2675, :2803), address-class heuristics, memos and their diagnostics.

## 3. Design

### 3.1 Files (`port/3ds/gfx/n64/`, all host-compilable except `gdx_rt_glue.cpp`)

| File | Role | est. lines |
|---|---|---|
| `gdx_gbi.h` | field decode over `w0/w1`, othermode/geometry bits (no LUS headers) | 300 |
| `gdx_dlsrc.cpp/.h` | command-source table: ranges (wide/host-N64/raw-BE), placeholder-stub lookup, `gSegments` snapshot; `Resolve(w1lo,w1hi) → {ptr, class}` | 400 |
| `gdx_dl.cpp` | walker: 32-deep call stack, per-list reader (stride/endian fixed at list entry), flat `switch`, `G_LOAD_UCODE` variant marker | 500 |
| `gdx_rsp.cpp/.h` | matrices (11-deep), lights (33), lookat, viewport, fog line, `G_VTX` T&L, MODIFYVTX, BRANCH_Z, LINE3D | 600 |
| `gdx_rdp.cpp/.h` | othermode, colors, combine word, tiles[8], scissor, fill/prim depth, key/convert (stored, unused) | 350 |
| `gdx_tmem.cpp/.h` | 4 KiB mirror, span store, same-content skip, value gates for SETTILE/SETTILESIZE, TLUT staging | 500 |
| `gdx_texcache.cpp/.h` | content-keyed cache + N64→RGBA32 decoders lifted from `ImportTexture*` | 900 |
| `gdx_cc.cpp/.h` | `GenerateCC` + `gfx_cc_get_features` lifted verbatim (the backend's `MapCombiner` consumes `CCFeatures`), combiner pool | 400 |
| `gdx_tri.cpp` | per-tri: reject screen, cull, state diff → flush, packed-VBO write with aux, texrect/fillrect quads, atlas arm | 600 |
| `gdx_interp.cpp/.h` | `GdxInterp` instance: `RunFrame(root, snapshot)`, `Flush`, killswitch latch, receipts | 300 |
| `gdx_rt_glue.cpp` | render-thread job callback: picks LUS or owned path, frame epilogue calls | 200 |
| `port/tests/gdx_interp_host_tests.cpp` + `gdx_mock_backend.h` | host harness (§6) | 600 |

Total ≈ 5,650 lines, in the master plan's 4-6k band.

### 3.2 Command reader and address resolution

Each list entry resolves its source class once (`gdx_dlsrc`): wide lists read
`{w0 @0, w1lo @8, w1hi @12}`; host-N64 lists read 8-byte `{w0, w1}`; raw-BE lists
`bswap32` both words (kept until the yaml fixup table is complete; BE-sniffed lists
are receipt-counted).
Operand resolution is a 3-way branch, not a heuristic: `w1hi == GDX1` → host pointer
unless it lies in the placeholder-stub map (binary search, 2,526 rows, already
generated) → segment-image pointer; `w1hi == 0` → `gSegments[w1>>24] + (w1 & 0xFFFFFF)`
(the decomp's own table, `port/decomp_port.c:636`); 8-byte lists are always segment
tokens. Vtx data referenced from a raw-BE list is read through a bswap16 accessor
chosen once per `G_VTX` (replaces `MakePersistentVtxCopy`). Texels are read raw;
`gNativeRgba16Ranges` membership selects the host-order decoder variant.

### 3.3 Opcode set (flat `switch`, 47 cases)

SP (F3DEX2, 20): NOOP, MTX, POPMTX, MOVEMEM (viewport, light, lookat, FZeroFlx alpha
light), MOVEWORD (numlight, fog, segment), TEXTURE, VTX, MODIFYVTX, DL, ENDDL,
GEOMETRYMODE, TRI1, TRI2, QUAD, LINE3D, SETOTHERMODE_L/H, CULLDL (no-op as today),
RDPHALF_1, BRANCH_Z, LOAD_UCODE (variant marker only: Standard / Reject / FZeroFlxReject,
`SetF3dex2Variant` 726), DMA_IO. RDP (25): SETTIMG, SETTILE, SETTILESIZE, LOADBLOCK,
LOADTILE, LOADTLUT, SETCOMBINE, SETPRIMCOLOR, SETENVCOLOR, SETFOGCOLOR, SETBLENDCOLOR,
SETFILLCOLOR, SETPRIMDEPTH, SETSCISSOR, RDPSETOTHERMODE, TEXRECT, TEXRECTFLIP, FILLRECT,
SETCIMG, SETZIMG, SETKEYR/GB, SETCONVERT, and the four syncs (no-ops; pipesync is
already a no-op on 3DS). Port (2): EXTRAGEOMETRYMODE (0x3a, 31 game sites), the LINE3D
form the bridge currently rewrites. OTR family: none (§2.2). Unknown opcode: count +
advance, one log line per opcode per boot.

### 3.4 State

`GdxRsp` / `GdxRdp` mirror LUS's structs where parity depends on them (matrix stack
11, `MAX_VERTICES` 64 + 4 reserved slots, lights 33, `texture_tile[8]`, `tmem[4096]`,
othermode u64, `combine_mode` u64). `GdxRenderState` is the backend-facing diff set
(§3.9). One `GdxInterp` instance owned by the render thread; no globals except
killswitch latches and counters.

### 3.5 Vertex pipeline

Per `G_VTX`: MVP hoisted to a local (`lus-vtx-mtx-hoist`), transform, widescreen
x-adjust, UV × scale >> 16, lighting (ambient + `num_lights-1` directional; positional
branch kept for `G_LIGHTING_POSITIONAL` parity), texgen linear/spherical, FZeroFlx
alpha-light LUT, fog `clamp(z/w)*mul+off` into alpha with the `w <= 0.001 → -1` rule,
`clip_rej` (5 planes, for the trivial-reject AND test) — all in float32 exactly as
today, so the 24-bit-uniform precision failure of the GPU-T&L branch (skyline quads
spanning ±32000 units shifted ~1 px; `gputransform-report`, "Visual A/B") does not
apply: we still hand PICA clip-space float attributes, which is what LUS does now.

**No CPU clipping.** Rationale: (1) PICA clips in hardware; the GPU-transform
experiment ran 220,989 hardware-clipped draws and the tunnel analysis records that the
GPU path "gets true hardware clipping instead (proper near-plane straddler clip, no
wedges)" (`docs/research/tunnel-roof-cull-analysis.md:101-108`); (2) the suspected
tunnel-roof defect is *in* the CPU clipper's plane-5/w≤0 handling against reversed
depth (:115-121), so removing it is at worst neutral; (3) sm64_3ds `gfx_pc.c` (~1,850
lines) never clips — `clip_rej` AND-reject only — and ships. Fallback: `[native]
cpuclip=1` ports the Sutherland-Hodgman clipper (~150 lines, same `x + w < 0` plane
spelling as `lus-traffic-vtx-clipmask`) for A/B if a replay shows a near-plane diff.
The reject-variant 2× box drop (4013-4025) is N64 microcode semantics, not clipping,
and stays.

### 3.6 TMEM

Same model as the patched LUS, written once: 4 KiB byte mirror bounded by a readable
limit; a span table indexed by TMEM word with `{base, words, kind, LoadedTexture}` and
a 64-entry live-span list; overlap teardown removes whole spans (`tmem2` fast path is
the *only* path; the per-word legacy walk is not carried); same-content skip compares
the derived load record against the live span (applies to every source now, since all
sources are immutable segment images or game buffers guarded by content hash); SETTILE
and SETTILESIZE compare-before-write and never dirty on bit-identical re-asserts;
`textures_changed[unit]` is armed only by real changes, TLUT loads, and cache evictions
(`lus-crowd2-tilestate-value-gate`). LOADTILE keeps the 8-byte-aligned row stride.

### 3.7 Texcache

Keyed by content, never by identity: `{addr, fmt, siz, tile w/h, line bytes, cms/cmt,
masks, palette addr, force_opaque, content_hash}` where `content_hash` is FNV-1a folded
4 bytes at a time over *exactly* the decoded span (RGBA16: mask/CLAMP/NOMASK-bounded
extent; fonts: recorded load extent) plus the palette bytes for CI. 1024-entry LRU;
eviction nulls the bound unit and re-arms `textures_changed`; decoders lifted from
`ImportTexture*` with the tall-atlas NOMASK bound. Upload path is the backend's
`SelectTexture` + `UploadTexture` + `SetSamplerParameters`, unchanged. Ownership: the
cache lives inside `GdxInterp` on the render thread; `gdx3ds_texcache_note_thread`'s
`texcacheMainMut=0` audit is kept as a receipt.

### 3.8 Combiner

`GenerateCC`/`gfx_cc_get_features` move verbatim into `gdx_cc.cpp` so the backend's
`MapCombiner` sees identical `(id0,id1)` and `CCFeatures`; the key is value-initialised
(`lus-cc-key-uninit-shader-id`), `G_CCMUX_SHADE_ALPHA` is a generic input
(`lus-3ds-shade-alpha-ccmux`), pool is a small open-addressed table with a
`prev` fast path. The `[debug] shader stack` (`G_PUSH_SHADER`) is dropped — never
emitted on this port.

### 3.9 Flush policy

A batch splits only on: combiner/shader change; texture (re)bind or sampler change on a
used unit; depth test/mask; decal; viewport; scissor; `use_alpha`; value change of a
per-batch constant sampled from vertex 0 or aux (prim color+lod, env, fog line, fog
color, blend alpha, prim depth); framebuffer ops; 256 tris. Explicit non-splits:
syncs, bit-identical SETTILE/SETTILESIZE/SETCOMBINE re-asserts, same-content loads,
GEOMETRYMODE (cull is per-tri on CPU), `G_TEXTURE` unless `first_tile` changes,
MTX/VTX/MOVEMEM. This is the union of today's flush sites (interpreter.cpp 1073, 4235-4254,
4554-4590, 4758, 4853, 4859, 5602, 5840/5872, 6940, 6959, 6983, 8200, 8946). The
draw-state memos are not carried: with a flat switch and value-gated state they reduce
to one "dirty since last tri" byte.

### 3.10 Texrect and atlas

`G_TEXRECT`(+2×RDPHALF_1), `TEXRECTFLIP`, `FILLRECT` build NDC quads from U10.2 with
the COPY-mode `dsdx>>2`/edge+1 rule, fixed rect viewport (memoized per framebuffer
geometry), `geometry_mode=0`, two rect tris with `is_rect` skipping clip/cull; fillrect
Z-clear detection → `ClearDepthRegion`. Atlas: the trectbatch protocol verbatim
(`gdx3ds_atlas_arm` before an eligible clamp/mask-0 upload, keep the open packed batch
when the resolved texture is a view on the bound page, `gdx3ds_vbopack_refresh`, view
offset added after clamp), behind the same `[debug] trectbatch` key.

### 3.11 Interpolation

Single pass. The P0/P1 interpolation machinery is env-gated, off in the 3DS build
(§2.5), and P2 loop decoupling never landed, so the owned path drops it. If sub-tick
interpolation is ever wanted it belongs in `gdx_rsp` at `G_MTX` time (prev/cur matrix
lerp), not in a second walk.

### 3.12 Stereo, viewport fold, border modes

Unchanged: all in the backend, driven by the same calls (`SetViewport` fold,
ortho-w heuristic, `gdx3ds_stereo_set_draw_class`, `SetCurrentPrimDepth` for anchored
rects). The interpreter emits exactly what LUS emits.

### 3.13 Render-thread integration

Same three-phase job shape, same ring, same fences. `prepare` (main) shrinks to
capturing `{root Gfx*, ucode variant, gSegments[16] snapshot, ranges version}` — no
conversion, so `brMain` goes to 0 and the bridgemain wait disappears. `run` (render
thread) calls `GdxInterp::RunFrame(job)` inside the existing `dynlod_task_begin/end`
bracket; first-claim segment-image loads during the walk still go through the fenced
`gSegments` store and are merged at `release` exactly as today. Frame epilogue order
(VI-fallback flag, frame mirror, transition capture) is preserved by `gdx_rt_glue.cpp`
calling the same functions the bridge calls now.

### 3.14 Receipts and profiling

`[prof]` sections keep their indices (BR=0 RUN=1 VTX=2 TRI=3 IMP=4 DRW=5 MTX=6,
`gdx3ds_gpu_prof.h`) with BR reading 0 on the owned path; per-opcode ticks
(`gdx3ds_prof_op_ticks[256]`) kept; `[c3d]` texImp/texMiss/texDel and `[tmem]`
counters kept; new `[ninterp]` line every 64 frames: lists walked by class, opcodes,
tris, batches, flush reasons histogram, BE-sniffed lists, unknown opcodes.

### 3.15 Killswitch

`[native] interp=0|1` (default 0 until M4 parity), latched per task in the job run
callback; receipt at boot `[native] interp=<v> path=<lus|owned> cpuclip=<v>`.
`[native] interp_verify=1` runs both paths on the same job with the backend in
record-only mode and compares per-batch hashes (§6), emitting `[nverify] frame=N
batches=a/b first_mismatch=i`. `[native] cpuclip`, `[debug] trectbatch`, `triloop`
retained.

## 4. Invariants from the 36 interpreter patches (checklist)

TMEM/texture: (1) SETTIMG accepts any pointer ≥ 0x00100000; (2) NOMASK axes bound the
RGBA16 decode by SetTileSize, bound only shrinks; (3) one span per load, byte-identical
views, overlap removes whole spans; (4) same-content load is a no-op with no generation
bump; (5) diag gates latch env once; (6) whole-span teardown (`tmem2`); (7) livery
receipt hooks; (8) `std::min<uint32_t>` spellings (moot). Texcache: (9) hash exactly
the decoded bytes, 4-byte FNV; (10) hash span bound == decode bound (CLAMP‖NOMASK);
(11) no address aliasing after recycling — content hash is the identity; (12) SETTIMG
memo — obsolete; (13) cache is render-thread-only. Combiner: (14) SHADE_ALPHA ccmux is
a generic input; (15) key value-initialised, pool finite. T&L: (16) clip mask uses the
`x + w < 0` spelling (`cpuclip=1` only); (17) geometry diagnostics gated, counters on.
Matrices: (18) MP hoisted per VTX batch. Flush: (19) prim/env flush on value change
only; (20) fog line noted at append time, flush on change, `exact=0` for `G_BL_CLR_BL`,
`f(d)=(d*2*mul+(off-mul))/255`; (21) pipesync no-op, sampled registers flush at their
own mutation site; (22)(23) memo whitelists — subsumed; (24) SETTILE/SETTILESIZE value
gates, evictions arm `textures_changed`; (25) packed emission bit-identical (uint8/255
table, pow2 reciprocal, shifts as 2^-k, `fminf` NaN policy); (26) vertices written once
in the 12-float layout, aux from vertex 0, no mid-batch param change; (27) dispatch
precedence — a switch; (28) no shared_ptr; (29) no per-task heap churn. Texrect: (30)
rect memo — subsumed; (31) rect viewport recomputed only on fb geometry change;
(32) `[trect]` counters; (33) atlas protocol as §3.10. Diagnostics: (34) `[prof]`
exclusive ledger, fixed indices; (35) per-opcode ticks; (36) `[tri2]` counters.

## 5. Milestones

| M | Deliverable | Verification | Size |
|---|---|---|---|
| M0 Skeleton | `gdx_gbi.h`, `gdx_dlsrc`, walker with counting handlers, killswitch + `[ninterp]` receipt, render-thread dispatch | `interp=1` walks a title→race replay with 0 unknown opcodes and 0 BE-sniffed lists (Azahar); opcode histogram matches the bridge | 1.1k lines, 3 days |
| M1 Menus | RDP state, TMEM, texcache + decoders, `gdx_cc`, texrect/fillrect, ortho tris, flush classes | `[fh]` parity vs `interp=0` on title / mode / machine select / course preview replays (emu); host tests for TMEM + decoders vs device goldens | 2.2k, 6 days |
| M2 Race | matrices, lights, fog, texgen, `G_VTX`, cull, packed VBO + aux, variant marker, MODIFYVTX, BRANCH_Z, LINE3D | parity on one GP race replay (emu) with HUD masks; `cpuclip=1` A/B on the tunnel course; HW build staged | 1.5k, 6 days |
| M3 Long tail + soak | EK screens, course editor, ending, records, DMA_IO alpha light, transition capture, VI fallback epilogue | parity on all replays (emu + HW), 30-min soak 0 errors, heap flat | 0.5k, 4 days |
| M4 Perf | value gates, dirty byte, atlas, `[prof]` targets; flip default `interp=1` | HW `[prof]` crowd window: br 0, dsp ≤ 3, tri ≤ 3, total ≤ 9 ms, gputrace off | 0.3k, 4 days |
| M5 Delete | remove bridge + LUS interpreter from the 3DS build (vendor track owns the carve), keep archive/blob reader | bit-identical `[fh]` before/after deletion; build with no `fast/` sources | −22k, 2 days |

Menus render first because they are texrect-dominated and need no T&L. Total ≈ 25
working days, inside the 3-5 week estimate.

## 6. Verification plan

1. **Frame-hash parity (oracle track)**: same replay, `interp=0` vs `interp=1`, `[fh]`
   per frame on emulator and hardware; images only on mismatch via `framediff.py` with
   HUD/rival masks until race determinism is pinned.
2. **Batch-level differential on device** (`interp_verify=1`): the LUS path already
   funnels every draw through `DrawPackedBatch`/`DrawTriangles`; a record-only backend
   mode hashes `(shader ids, bound texture content hashes, sampler, depth/decal/alpha,
   viewport, scissor, aux, VBO floats)` per batch into a ring; the owned path runs the
   same job and compares. First mismatching batch + opcode index is the bug locator.
   This answers "is the old interpreter host-compilable": it is not (it needs
   `Ship::Context`, `ResourceManager`, CVars, `GfxDebugger`, spdlog; interpreter.cpp:20-55),
   so goldens are produced on device and compared there or dumped to SD.
3. **Host harness** (`port/tests/gdx_interp_host_tests.cpp`, built like
   `gfx_pack_tests.c` with `cc -DPORT=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1
   -I decomp/include`, `tools/ci-3ds.sh:325-329`): feeds hand-built `Gfx` arrays
   (both 16-byte tagged and 8-byte forms) into the walker with a mock backend, asserts
   packed VBO floats, texcache keys, TMEM spans, flush reasons; also replays SD-dumped
   batch goldens from step 2 for a few frames. Sub-second, in CI.
4. **Hardware `[prof]`**: crowd frame window, gputrace off, targets in M4.

## 7. Risks

- **Near plane without CPU clipping**: evidence is indirect (§3.5); mitigated by the
  `cpuclip=1` fallback and the tunnel replay in M2's gate.
- **Precision**: unchanged from today (float32 CPU T&L, float attributes); the 24-bit
  failure was uniform-side. Watch the skyline quads anyway in the parity masks.
- **Texcache ownership across threads**: cache and TMEM live in the render-thread
  instance; the main thread only produces the job. Segment images are append-only
  first-claim stores fenced by `GdxRtFence`, so pointers captured at prepare stay valid.
- **ILP32 address class**: the GDX1 tag removes the heuristic for wide lists, but
  placeholder stubs sit in BSS at real addresses and must be intercepted before use;
  the stub map is generated and complete (2,526 rows). Untagged raw-BE lists remain a
  sniffed class until the fixup yaml is complete — receipt-counted.
- **Combiner fidelity**: lifting `GenerateCC` verbatim keeps the 20 census modes
  (`combiner-census.md:3-4`, 5 two-cycle) bit-identical; any rewrite is out of scope.
- **Hidden bridge behaviour** (venue bank aliasing, EK fills): M0's census and M3's
  long tail are the net.

## 8. Dependencies on other tracks

- **vendor** (Phase 0): the LUS carve must be vendored with patches folded before M0
  so the old path is stable during A/B and M5 is a deletion, not a patch dance.
- **oracle** (Phase 1): `[fh]` frame hash, replays per screen and one race,
  `framediff.py`, 3dslink push for the HW gates. M1 can start on Azahar autoinput shots.
- **decomp/assets**: completeness of `sAssetFixups` GFX/VTX kinds (drives the raw-BE
  class to zero); no game-code changes otherwise.

## 9. First 10 commands

```
<scratch>/native/bootstrap.sh interp native/interp
cd ~/code/gdx-3ds/interp && mkdir -p port/3ds/gfx/n64 && git log --oneline -3
grep -n "add_library(gdx3ds_gfx" -A30 port/3ds/gfx/CMakeLists.txt        # where n64/ registers
sed -n 1715,1775p decomp/include/PR/gbi.h                               # Gfx + GDX1 tag
sed -n 4742,4760p port/n64_gfx_bridge.cpp; sed -n 8233,8290p port/n64_gfx_bridge.cpp   # range tables → gdx_dlsrc
sed -n 9681,9743p libultraship/src/fast/interpreter.cpp                 # gfx_step to replace
sed -n 3512,3840p libultraship/src/fast/interpreter.cpp > <scratch>/native/interp/vtx_ref.cpp   # T&L reference
sed -n 827,1063p libultraship/src/fast/interpreter.cpp                  # GenerateCC to lift
python3 <scratch>/native/interp-census/census.py                        # o2r census fixture
cc -O1 -DPORT=1 -DGDIFFUSER_PORT=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -Idecomp/include port/tests/gfx_pack_tests.c -o /tmp/gp && /tmp/gp   # host-harness template
```
