# wobble — progress (distant-vertex wobble + pop-in audit, native paths vs stock)

Track: `wobble`, branch `native/wobble`, worktree `~/code/gdx-3ds/wobble` (off `native/main@1328487`).
Protocol: `tools/native/PROTOCOL.md`. Hardware observation that started this (New 3DS,
`native/main@b367f42`, run G `[native] all=1 interp=1 cpuclip=1`): "some objects in the distance
had vertex wobbling", "maybe a bit more pop-in", not sure if pre-existing; run E (`all=1 interp=0`)
drew no remark. Every emulator parity check says these paths are pixel-identical to stock, but
Azahar emulates the PICA with host floats, so the audit asks what reaches the GPU per path.

## State (2026-09-20)

- Audit done (below). Result: **no attribute-format, shader or uniform-route difference exists
  between any native path and the stock packed-VBO path** — every draw of every path goes through
  one `DrawTriangles` tail with one `AttrInfo` and one `BufInfo`. The wobble, if path-dependent,
  is not a GPU-input-format effect. Ranked hypotheses in §3; the top two are testable on the card.
- Fix commit `c237e65` (parity-neutral by construction — receipts + inter-draw padding only):
  `[dynlod]` receipt now prints under `interp=1` (it never did), `[ninterp]` carries
  `rej= cull= emit= packref= full=`, and the native index tail is padded to 16 bytes.
- Emulator parity vs `b367f42`: **clean on both lanes** (`interp=0` and `interp=1`: 6002/6002, 8/8 SHOTs byte-identical), §5.
- Card staged: `~/Desktop/G-Diffuser-3DS-wobble/` (§6). The SD card was not touched. Next step: the user runs it.

## 1. What reaches the PICA, per path (the audit)

Everything below was read from the code on this branch; line numbers are for `c237e65`.

### 1.1 Vertex attribute formats — identical for every path

| item | where | value |
|---|---|---|
| attribute loaders | `port/3ds/gfx/gfx_citro3d.cpp:1439-1444` (Init, once) | v0 `GPU_FLOAT x4` pos, v1 `GPU_FLOAT x2` uv0, v2 `GPU_FLOAT x4` rgba, v3 `GPU_FLOAT x2` uv1 |
| buffer binding | `gfx_citro3d.cpp:3413-3415` (`DrawTriangles`, every draw) | one `BufInfo_Add(dst, 48 B stride, 4 attrs, 0x3210)` |
| stride | `gfx_citro3d.cpp:579` `kOutStrideFloats = 12` = `GDX3DS_VBOPACK_STRIDE` (`gdx3ds_vbopack.h:45`) | pos4 + uv0 2 + rgba4 + uv1 2, all float32 |
| fixed vertex scaling / GPU_SHORT / GPU_BYTE | nowhere | never used; the PICA's fixed-point attribute conversion is not on any path |

Who writes into that layout:

- stock LUS `[triloop]` packed batch: `VboPackBegin` (`gfx_citro3d.cpp:3528`) hands the cursor to
  the vendored interpreter, `DrawPackedBatch` (`:3556`) → `DrawTriangles`.
- owned interpreter (`interp=1`): `port/3ds/gfx/n64/gdx_tri.cpp:529-717` writes the same 12 floats
  (`out[0..3]` = clip x, y, `(z+w)/2`, w; UVs through the same `FminExact`/pow2-rescale
  expressions; colour through `mByteToUnit`), `Flush` (`gdx_tri.cpp:47`) → `DrawPackedBatch`.
- machines (`machines=1`): `gdx3ds_mesh.cpp` emits through `gdx3ds_vtx_math.h` (a verbatim
  transcription of `GfxSpVertex`/`GfxSpTri1`) into `NativeMeshBegin`'s cursor, then
  `NativeMeshDraw` (`gfx_citro3d.cpp:3612`) copies u16 indices after the vertices and calls the
  SAME `DrawTriangles` with `mNativeActive` set; only the submit lambda differs
  (`C3D_DrawElements(GPU_TRIANGLES, nIdx, C3D_UNSIGNED_SHORT, idx)` vs `C3D_DrawArrays`,
  `:3343-3349`). citro3d toggles GEOSTAGE_CONFIG's element mode around the indexed draw itself.
- track / fx (`Gdx_NativeBatch*`, `gfx_citro3d.cpp:4320-4450`): vertices written at the pool cursor,
  `Gdx_NativeBatchFlush` → `DrawPackedBatch` → `DrawTriangles`. `Gdx_NativeBatchIndices` returns
  NULL (no indexed track draws).

So: same types, same count, same order, same stride, same pool (`mVboPool`, one `linearAlloc`,
`:1463`), same cache flush (`FlushPendingVbo`, `:1642`, one range `[flushed, offset)` before every
`C3D_FrameEnd`/`C3D_FrameSplit`, `:1654/:3981/:4052`; the native index tail lies inside that range
because `NativeMeshDraw` advances the cursor by `nVerts*12 + idxFloats`).

### 1.2 Vertex shader and uniforms — identical for every path

`port/3ds/gfx/shader.v.pica` is the only vertex shader: 4 `dp4` with `projection[4]` + pass-through.
The uniform is chosen per draw inside `DrawTriangles` (`gfx_citro3d.cpp:3416-3505`): fixup matrix →
MENU DISP scene/UI variant → `[vpfix]` viewport affine → stereo eye matrices. None of that reads
which path produced the batch; the only per-path input is `gdx3ds_stereo_set_draw_class`, which
the track/fx batch sets (`:4423`) and which the bridge and owned walker never set (grep: the only
caller outside the backend is `Gdx_NativeBatchFlush`), so LUS and owned draws classify by the same
`bufVbo[3] == 1.0f` heuristic. Depth: `C3D_DepthMap(true, -1, 0 | decalBias)` (`:1455/:2684`) for all.

### 1.3 CPU-side precision — identical, and float24 cannot make clip-space wobble

- Quantisation: LUS's `GfxSpVertex` and the owned `TransformVertices` (`gdx_rsp.cpp:136-327`) and the
  machines' `gdx3ds_vtx_transform` (`gdx3ds_vtx_math.h:122-236`) all run the same float32
  expressions in the same order (s16 model coords × the s15.16-derived float MP matrix, UV
  `(tc*scale)>>16`, fog clamp, `clip_rej`/`clip_out` masks). Nobody quantises screen positions:
  the N64's own s15.2 screen fixed-point is not reproduced by LUS, and the native paths do not add
  one. There is no path with a fixed-point intermediate.
- `cpuclip=1` (`gdx_tri.cpp:380-438`) is the LUS 7-plane Sutherland–Hodgman clip, plane for plane
  (`ClipDistance` `:109`, fan re-entry with `w` clamped to `kMinClipW`), including the far plane
  `w - z`; far vertices are treated exactly as in LUS. `cpuclip=0` is the documented
  different-pixels A/B and was not in run G.
- PICA float24 on clip-space attributes (the gputransform precedent, `feat/3ds-gputransform`
  `docs/research/gputransform-report.md`): a float24 has a 16-bit mantissa, relative step 2^-16.
  For a clip vertex with |x| ≤ w the NDC error is ≤ 2·2^-16 ≈ 3e-5, i.e. ≤ 0.006 px on the 400-px
  axis and ≤ 0.004 px on the 240-px axis, for ANY w (the error is relative, not absolute). That
  is two orders of magnitude under a sub-pixel. The gputransform case wobbled because it fed
  MODEL-space ±32000 coordinates through a composed matrix held in float24 UNIFORMS (absolute
  error ≈ 32000·2^-16 ≈ 0.5 unit per product); no native path enters that regime — all four feed
  clip-space floats and a 0/±1/0.5 fixup matrix, exactly like stock.
- ARM11 VFPv2 vs Azahar's dynarmic: both are IEEE single with chained (non-fused) `fmacs`, so the
  CPU-side values the emulator parity compared are the values the hardware computes.

### 1.4 What the emulator parity does NOT cover (this is where a hardware-only difference can hide)

- Every parity/verify run in this fork ran with `[debug] renderthread=0` (homefix-progress.md
  §Verified; interphooks lane; the oracle's determinism pin). Hardware runs the pipelined render
  thread (`[debug] renderthread` default 1, `gdx3ds_menu.c:379`, `gdx3ds_renderthread.cpp:297`):
  logic(N+2) on main overlaps render(N+1) on core 2. The N64 contract (everything the DL references
  is double-buffered or written after DP-done; `renderthread-audit.md` §3/§4) covers the game's
  data for BOTH interpreters — the bridge copies vertices only for big-endian static sources
  (`n64_gfx_bridge.cpp:6853/8297`, `isBig`), so LUS reads pool vertices live too — but the owned
  walker resolves operands on the render thread (`gdx_dlsrc.cpp:44` through the bridge's resolver
  seam) where LUS resolved them on main at submit. Nothing DL-referenced was found that main
  rewrites during logic(N+2); the track/fx records are per-pool-parity (`gdx3ds_track_record.h:1-5`,
  `gdx3ds_fx_record.h:1-4`); machine lists rebuild only at game init / mode change
  (`game.c:181/682/712`), never mid-race. Still: **the thread interleaving was never exercised by
  any parity run**, which is why the card includes a `renderthread=0` isolation run.
- The stereo right-eye pass never activates under Azahar (STATUS.md), so slider-on behaviour of
  any lever is unverified; the card asks for the slider OFF on the wobble runs.
- Only the canonical Mute City replay is covered; other courses exercise other DL classes.

### 1.5 dynlod — what it measures, and how the native paths move it

- Timer: `gdx3ds_dynlod_task_begin/end` wrap `gdx_gfx_job_run` on the render thread
  (`gdx3ds_renderthread.cpp:205-216`; `n64_sched.c:1190` when the thread is off) — the interpreter
  walk + native machine T&L + draw submission, CPU only, no GPU wait (`C3D_FrameBegin` is in BEGIN).
  The native paths do not remove anything from that window; they shorten it.
- Controller (`gdx3ds_dynlod.c:131-173`): tier 0..2 = rival detail NATIVE/REDUCED/MINIMAL
  (`gdx3ds_menu.c:243`); `effective = max(floor, tier)`; RAISE (less detail) when the 4-frame
  average > hi (15.0 ms), LOWER only when a whole 30-frame block averages < lo (12.0 ms), dwell 8.
  Cheaper frames therefore lower the tier toward the floor = MORE detail. Direction is right.
- Level ≠ 0 stretches every rival's view depth beyond `gdxRivalNear` by `gdxRivalBias` (2.5×, or
  6× beyond 120 at MINIMAL; `decomp-port-rival-detail.patch`), so the game's LOD tiers
  230/290/380/470/1500, shadow 800 and effects 400/900 engage that much earlier. A tier CHANGE
  therefore pops every distant rival between LODs at once.
- Machine LOD selection under `machines=1` is the game's: the hook finds the list the game passed
  to `G_DL` (`gdx3ds_mesh.cpp:965-1034`, `Find(src)` → registry entry `slot`/`lod`), so LOD choice,
  thresholds and the dynlod bias are untouched; `machines_lod_mask=1f` only decides which of the
  5 LODs draw natively vs through the interpreter.
- Receipt gap (fixed in `c237e65`): the `[dynlod]` line was printed only from the bridge's
  `[race-dl]` census inside `GdxGfxJobRun` (`n64_gfx_bridge.cpp:10765` … `:10900`), which the owned
  walker never enters (`gdx_ninterp_job_run` takes the job first, `:12174-12190`). Run G's log
  cannot contain a `[dynlod]` line; run E's can.

## 2. Path-specific differences actually found

1. **Attribute-buffer base alignment after a machine draw** (`gfx_citro3d.cpp:3617`, before the fix):
   the u16 index tail was rounded to 4 bytes, so every later draw's `BufInfo` base in the same frame
   was 4/8/12-byte aligned, where the stock 48-byte-stride appends are always 16-byte aligned.
   Azahar's vertex loader reads any byte address. No PICA alignment requirement is documented and
   run E (machines=1) drew no remark, so this is hygiene, not the cause — but it was the only
   difference in what the fetcher is handed. Fixed: tail padded to 16 bytes (`NativeIdxFloats`).
2. **Receipt blind spots under `interp=1`**: no `[dynlod]` line; no visibility of refused packed
   batches (`packRefused`, `gdx_tri.cpp:533`) — the owned walker DROPS a batch the backend refuses
   (pool nearly exhausted, `VboPackBegin` `:3539-3541`) where LUS falls back to its legacy path.
   That is a real, path-specific pop-in mechanism when the 1 MiB pool (`:582`) runs hot; it now
   prints as `packref=` on the `[ninterp]` line (0 expected).
3. Nothing else: draw order and batch boundaries were compared per batch by `[nverify]`
   (13,440 tasks, 0 mismatches) and the three verify modes; the GPU state setters are shared.

## 3. Ranked hypotheses

1. **dynlod tier oscillation now that the render task is cheap enough to cross the 12 ms LOWER
   line** (explains "more pop-in" AND "distant objects wobbling" = distant rivals flipping LOD in
   lockstep with the tier). On hardware the LUS build measured 14-15 ms in crowd windows
   (`renderthread-audit.md` §8), which can RAISE on spikes (>15 for 4 frames) but can never LOWER
   (needs a 30-frame block < 12): the tier ratchets to MINIMAL and stays — stable, just reduced.
   With `interp=1` the same window lands in the 12-15 band: raise on a spike, lower 30 frames
   later, raise again — every transition re-tiers every distant rival by 2.5-6×. Evidence needed:
   `[dynlod] … raises=a/b lowers=c/d` per 64-frame window on hardware (now printed under interp=1);
   card run P (`rival_detail=0`, `rival_detail_auto=0`) removes it entirely. Fix if confirmed:
   longer LOWER block / asymmetric dwell in `gdx3ds_dynlod.c` (not changed blind).
2. **Render-thread interleaving that no parity run exercised** (`renderthread=0` on every lane).
   The owned walker resolves and reads operands during render(N+1) while main runs logic(N+2);
   the audit finds every referenced range double-buffered or fenced, but that is a reading of the
   design, not a run. Card run R (`all=1 interp=1 renderthread=0`) isolates it: if the wobble
   vanishes with the thread off, it is a race, not the GPU.
3. **Pre-existing, path-independent** (the user is unsure it is new): the stock runs S/M/T/I on the
   card, same course and spot, settle it. Candidates if all runs wobble: the game's own far-LOD /
   tessellation behaviour, or 24-bit depth z-fighting on distant decals (`sDecalBias`), neither
   of which any lever changes.
4. **Stereo slider on** during run G: the right-eye pass is unverified for every lever; classes are
   heuristic (`bufVbo[3]==1`) for both interpreters, so no difference is expected, but it is
   unexercised. Card: slider OFF for the A/B, one slider-ON run at the end.
5. **Attribute-buffer alignment** after machine draws (§2.1) — fixed in this build regardless.
6. **Course content outside the canonical replay** (legacy F3D vertex lists, `G_BRANCH_Z` LOD
   branches — both transcribed identically, `gdx_dl.cpp:238-258` vs `interpreter.cpp:8437`): would
   also differ in the emulator on that course; not hardware-only.
7. Ruled out with numbers: PICA float24 attributes/uniforms on clip-space floats (§1.3);
   fixed-point attribute conversion (no path uses `GPU_SHORT/BYTE`); attribute count/order/stride;
   a per-path vertex shader or uniform route; `cpuclip=1` far-plane handling.

## 4. Commits

- `c237e65` fix(wobble): `[dynlod]` receipt under interp=1, `[ninterp] rej/cull/emit/packref/full`,
  16-byte native index tail.

## 5. Verified

Lane: `tools/dev/gdx-dev retro run` (RetroArch + azahar-libretro, Vulkan, New3DS model), canonical
replay `race-mutecity-start.gdxrec`, ini `det=1 fh=1 fh_every=1 fh_start=0 renderthread=0
replay_quit=1 gputrace=0 platform=azahar stereo.enabled=1 net.enabled=0 native.all=1`; binary =
the card's 3dsx (sha256 `5ede5845…`, code `c237e65`, build-id string still `1328487`: baked at
configure). Base = `native/main@b367f42` run `homefix/20260920-135714-base-1` (sha `bced3f34…`,
same lane/ini, `native.interp=0`). Tries: A passed on try 4, B on try 5 (the known
lever-independent boot SIGBUS under load ≈ 30-50; every failed try died before frame 40).

| run | ini delta | result |
|---|---|---|
| `wobble/20260920-175011-wobA-4` (`interp=0`) vs base | none | `fhdiff: compared=6002 divergent=0 only_in_a=0 only_in_b=0 last_common=6002`; RESULT PASS frame=5980, 8/8 SHOTs, 0 fatals |
| `wobble/20260920-175520-wobB-5` (`interp=1`, i.e. run G's levers) vs base | `native.interp=1` | `fhdiff: compared=6002 divergent=0 … last_common=6002`; RESULT PASS frame=5998, 8/8 SHOTs, 0 fatals; all 8 SHOT BMPs byte-identical to the base's |
| A vs B | — | `compared=6002 divergent=0` |

Receipts from B (interp=1): `[native] interp=1 path=owned cpuclip=1 blendflush=1 verify=0
hooks=noop(track,fx)+dl(machines)`; every `[ninterp]` window `rej=0 cull=0 emit=224640
packref=0 full=640` (packed batches refused: 0 — the pool never ran hot on this replay);
`[race-dl]` lines: 0 in B (the bridge census never runs under the owned walker — confirms the
receipt gap), 44 in A; `[dynlod]` lines: 92 in B (all from the new receipt), 45 in A.

**Not verified, and why:** the `[dynlod]` controller itself — every replay run pins it off
(`main_3ds.cpp:987`: `gdx3ds_dynlod_set_auto(0)` under `.gdxrec` replay because the tier depends
on `svcGetSystemTick` deltas), so B's lines read `auto=0 … n=0`. **No parity run in this fork has
ever exercised dynlod**, on either interpreter; combined with the missing receipt under `interp=1`
that makes hypothesis 1 both untested and previously unobservable. Also not verified: the
render-thread interleaving (all lanes `renderthread=0`), stereo right-eye, any course but Mute City,
and of course hardware — the card carries all of those.

## 6. Card

`~/Desktop/G-Diffuser-3DS-wobble/`: `G-Diffuser-3DS.3dsx` (sha256 `5ede5845ffe4b1480dad78d6941c53e1ce182f288160124934386797d601277c`),
`G-Diffuser-3DS.elf` (`97fdf65a…`), `SHA256SUMS`, `TEST-PLAN.txt`. Runs, same course / spot / stretch,
slider OFF, restart between runs (ini latched at boot):
S stock (`all=0 interp=0`) · M `machines=1` only · T `track=1` only · I `interp=1` only ·
G `all=1 interp=1` (the reported run) · P = G + `[perf] rival_detail=0 rival_detail_auto=0`
(pop-in check; DBG tab RIVAL DETAIL row reads NATIVE) · R = G + `[debug] renderthread=0`
(thread isolation) · Z = G with the slider ON. Report per run: wobble none/machines/track/buildings,
pop-in vs S; send `log.txt` per run and paste 5-10 consecutive `[dynlod]` lines from G and from S.
The SD card was not touched.

## Next step

User runs the card. Decode: wobble in G/I but not S/M/T → owned-interpreter specific, then R
splits thread race (gone in R) from something else; wobble in all runs → pre-existing
(hypothesis 3); pop-in gone in P + `[dynlod]` in G showing both raises and lowers per window →
hypothesis 1, fix = asymmetric dwell / longer LOWER block in `gdx3ds_dynlod.c` (change it with the
numbers, not before).

## Gotchas

- zsh expands `====`/`--include=*.c` — quote them.
- `gdx-dev retro run` on Vulkan dies at boot ~1 in 3 (SIGBUS class, lever-independent); the loop
  script retries. `[fh]` hashes are renderer-specific: compare Vulkan against Vulkan.
- The `decomp` submodule shows modified after `tools/ci-3ds.sh` (patch stack applied) — never
  `git add` it.
