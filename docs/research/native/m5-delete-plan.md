# M5 — delete the bridge + the LUS interpreter (inventory, survivors, step plan)

Task B6 of `NEXT.md`. Read-only analysis at `native/main@d45fe63`; no code changed. Plan:
`06-interp-plan.md` §2.6, §M5. Every line number is from this tree.

## 1. Verdict

M5 is **3DS-only**. `port/n64_gfx_bridge.cpp` and `port/n64_gfx_convert.cpp` are compiled by the
desktop executable (`port/CMakeLists.txt:130,132,591,593`) and by two desktop test targets
(`:423-427`, `:369`). The deletion is therefore *a CMake removal from
`port/3ds/game/CMakeLists.txt:100-102` plus new 3DS-only files*, not a `git rm`. The desktop
keeps the bridge, the LUS submodule, and `gdx_gfx_convert_tests` unchanged; `tools/ci-3ds.sh:353-363`
compiles `n64_gfx_convert.cpp` standalone and keeps passing untouched.

## 2. What leaves the 3DS link

| Unit | lines | note |
|---|---|---|
| `port/3ds/vendor/lus/src/fast/interpreter.cpp` | 10,756 | 1 of the 36 carve sources |
| `src/fast/debug/GfxDebugger.cpp` | 54 | |
| `src/fast/resource/factory/{DisplayList,Vertex,Texture,Matrix,Light}Factory.cpp` | 1,355 | 1,187 of it DisplayListFactory |
| `src/fast/resource/type/{DisplayList,Vertex,Texture,Matrix,Light}.cpp` | 80 | |
| `include/fast/**` (22 headers) | 3,153 | only after step 5 de-LUSes the backend |
| `port/n64_gfx_bridge.cpp` (3DS TU) | 12,847 | ~3,700 of it relocated, see §3 |
| `port/n64_gfx_convert.cpp` + `.h` | 330 | narrow→wide converter, dead under `interp=1` |
| `port/gdx_interp.cpp` + `.h` | 602 | frame interpolation, dead (§4.1) |
| `port/gdx_dlsrc_bridge.h` | 138 | the seam disappears |
| `port/3ds/gfx/gdx3ds_seam_lus.cpp` | 472 | the LUS half of `GdxInterpSeam` |
| `port/3ds/gfx/gdx3ds_verify.h` | 77 | `[nverify]` needs two interpreters |
| `port/resource/ResourceFactories.cpp:18-22,146-157` | ~17 | the 5 `Fast::` registrations + includes |
| `port/3ds/harness/dl_tests_main.cpp` | 346 | LUS-only DL replay app; retire or reimplement |

Gross ≈ **30,200 lines** out of the 3DS build; ≈ 3,700 come back as new 3DS-only sources.
**Net ≈ −26,500** (the plan's "−22k" predates the interphooks seam and the harness).

Surviving carve: 24 of 36 sources (ship resource manager/loader/archives/binarytools/utils,
3,088 lines under `src/ship/resource` plus utils) — the o2r blob reader stays.

## 3. Survivors and where they go

The seam header `port/gdx_dlsrc_bridge.h` is already the exact contract; 53 bridge-exported
symbols have 3DS-side callers (measured by grepping `port/3ds`, `port/decomp_port.c`,
`port/gen`, `decomp/src`). Grouped by proposed new home:

| New file | est. lines | What moves (today's `file:line`) |
|---|---|---|
| `port/3ds/gfx/n64/gdx_ranges.cpp` | ~1,750 | `HostRange` tables `gHostRanges` / `gRawN64Ranges` / `gHostN64CommandRanges` / `gNativeRgba16Ranges` (bridge:595-720); `ReadableByteLimit` (:2646); registrations `gdx_register_host_range` (:8429), `_host_n64_command_range` (:8435), `_host_wide_command_range` (:8441), `_host_raw_n64_range` (:8455), `_host_pointer_stub` (:8477), `_n64_address_range` (:8491), `gdx_register_main_module_range` (:9567), `gdx_registered_host_capacity` (:9863), `gdx_resolve_registered_host_address` (:9951), `gdx_resolve_module_host_address` (:9989), `gdx_dma_report_short_dest` (:9939); DMA dirty-range table + `gdx_record_dma_load` (:863) under `sGdxDmaRangesLock` (:840); native-RGBA16 set/defer-clear (:9447/:9464) + `gdx_gfx_texels_are_native_rgba16` (:12655); `gdx_gfx_classify_host_ptr` (:12841) |
| `port/3ds/gfx/n64/gdx_assetseg.cpp` | ~1,150 | `IsAssetPlaceholderPointer` (:1216), `EnsureAssetSegmentImage` (:1234), `EnsureAssetSegmentForSymbol` (:1474), `ResolveGeneratedAssetStub` (:1929), `ResolveWideAssetStubPointer` (:2077), `EnsureSetupGfxSegment` (:2157), `IsRdramHostPointer` (:1542), `IsPortBssAliasPointer` (:1561), mode-segment-9 + venue-bank resolvers, `[seg3-verify]` CRC tripwire (:1110,:1422), C exports `gdx_ensure_asset_segment_for_symbol` (:9589), `gdx_wide_asset_resolve_enabled` (:9621), `gdx_resolve_wide_asset_pointer` (:9630), `gdx_boot_warm_asset_segments` (:9699), `gdx_load_venue_texture_segment` (:9740), `gdx_load_venue_building_texture` (:9829), `gdx_gfx_low32_needs_resolver` (:12663), `gdx_gfx_timg_is_resource` (:12648), `gdx_gfx_readable_bytes` (:12659). Callers stay put: `port/gen/AssetBindings.c:4690` (`sAssetSegmentMap`, 2,526 rows), `:9205` fixup, `:9232` range registration, `port/decomp_port.c:1393`, `port/gen/EkAssetBindings.c` |
| `port/3ds/gfx/n64/gdx_seg.cpp` | ~200 | `gSegments` epoch + render-thread view: `gdx_segment_epoch_begin/end` (:86/:90, called from `port/decomp_port.c:1312/1364`), `gdx_gfx_segment_view_set` (:142), `gdx_gfx_segment_claims_merge` (:150), `gdx_gfx_job_set_segment` (:12667), `GdxRtFence` wrapper (:164-171). **`gSegments` itself does not move** — it is `port/decomp_port.c:519`, and `port/gdx_segment_source.c` (517 lines, its own `LightLock` at :43-45, `gdx3ds_rt_fence_dma` at :298) is untouched by M5 |
| `port/3ds/gfx/n64/gdx_job.cpp` | ~350 | `GdxGfxJob` + `gdx_gfx_run` (bridge:10540 region), `gdx_gfx_job_prepare/run/release/release_before_walk/segments/ucode`, `gdx_gfx_job_epilogue`, `gdx_gfx_job_root_list` / `_resolve_list` / `_resolve_data` / `_resolve_timg_host` / `_ucode_variant`. Prepare shrinks to `{root, ucode, gSegments[16], ranges version}` — `ConvertRoot` is gone, `brMain` already reads 0. `port/3ds/gdx3ds_renderthread.cpp` (:206, :277, :403, :484, :496) keeps its three-phase shape byte-for-byte |
| `port/3ds/gfx/n64/gdx_present.cpp` | ~900 | `gdx_vi_present_fallback` (:9211-9447, 236 lines) + `GdxBindWindowFramebuffer` (:9150), `GdxPresentPathTrace` (:9037), `GdxDiagHoldTick` (:9069), `gHostFrameGfxTaskRan` guard; frame mirror `GdxUpdateFrameMirror` (:8978) + `SeedFramebufferQuad` (:8600); `gdx_request_frame_dump` (:8893, callers `port/input_bridge.c:397`, `port/gdx_input_script.c:443`), `GdxDumpNamedFrameIfRequested` (:8908), `gdx_gfx_post_run_capture` (:8940, caller `port/3ds/lus_glue/gdx3ds_fast3d_window.cpp:259`), `GdxReadTopScanoutRgba16` (:8799); **transition scanout capture** `gdx_read_current_framebuffer` (:12459) + `LogAndDumpTransitionCapture` (:12251) + `[capture-probe]` (:12371) + `GdxTransitionCapturePendingThisTick` (:3118) + `gdx_diag_note_transition_capture` (:9561). Port hooks: `decomp/src/overlays/ovl_i2/transition.c` calls `gdx_read_current_framebuffer`, `gdx_diag_note_transition_capture`, `gdx_defer_native_rgba16_texture_range_clear`, `gdx_set_native_rgba16_texture_range`; `decomp/src/sys/sys_main.c` calls `gdx_vi_present_fallback` and `gdx_register_n64_framebuffer`; `port/3ds/main_3ds.cpp` calls the fallback per host frame |
| `port/3ds/gfx/n64/gdx_cc.cpp` (append) | +~100 | `gfx_cc_get_features` (`vendor/lus/src/fast/interpreter.cpp:10648`) + `CCFeatures` and the `SHADER_*` enum (`include/fast/interpreter.h:49,107,130`). **This is the one hard coupling left in the kept backend**: `gfx_citro3d.h:35-36` includes `fast/interpreter.h` for these and `fast/backends/gfx_rendering_api.h` for its base class; `gfx_citro3d.cpp:2070` calls `gfx_cc_get_features` in `MapCombiner` |
| `port/3ds/gfx/n64/gdx_texcache.cpp` (append) | +~40 | `gdx_texcache_request_clear` (:9521) and `gdx_invalidate_texture_address` (:9530) — callers `port/decomp_port.c` and `decomp/src/overlays/ovl_i3/minimap.c`. The owned cache already lives in `GdxInterp` on the render thread (§4.5) |
| stays where it is | — | `gdx_gfx_mem_census` → fold into `[ninterp]`; `gdx_cxx_exception_name/_what` (:9878/:9909) → move to `port/shims.c` or `port/3ds/os/` (unrelated to gfx) |

Everything else in the seam header dies with the LUS path: `gdx_gfx_job_run_lus`,
`gdx_gfx_lus_state`, `gdx_gfx_lus_resync_backend`, `gdx_gfx_job_bridge_stats`,
`gdx_gfx_frame_config`, `gdx_gfx_frame_publish`. The last two need replacements: frame config
(dimensions, `rendersToFb`, `gameFb`, widescreen latches, filter) is latched today by
`Fast::Interpreter::StartFrame` via `gdx3ds_fast3d_window.cpp:233-238`, and `mGfxFrameBuffer`
is published by `Run()`'s epilogue. Both become fields of an owned `Gdx3dsWindow` replacing
`Fast3dWindow` (424 lines, ~150 of which are `mInterpreter->` forwarders).

## 4. The specific questions

**4.1 Interpolation — dead, confirmed.** `port/gdx_interp.cpp` activates only from `getenv`
(`GDX_INTERP_P0/P1/P2`, :96); `GDX_GATE_INTERP_P0` is a Bucket-B gate compiled out without
`GDX_DEV_TOOLS` (`port/gdx_dev_gates.h:126-134`), there is no `[native]`/`[debug]` ini key, and
devkitARM ships no environment. The second `interp->Run` (bridge:11593) has never executed on
3DS; `interp-progress.md` M4 measures a single pass. Delete `gdx_interp.cpp/.h`, the matrix
retention block (bridge:2968-3400) and the 24 `gdx_gfx_interp_*` accessors (:10034-10140, all
desktop-only callers) from the 3DS build.

**4.2 Texrect atlas + `trectbatch` — already in the backend.** `gdx3ds_atlas_arm`,
`gdx3ds_vbopack_refresh`, the shelf packer and the eviction telemetry are all in
`gfx_citro3d.cpp:646-2130` / `gdx3ds_vbopack.h:73-114`; the interpreter only arms. The owned
`gdx_tri.cpp` / `gdx_rect.cpp` already do. **No move, no risk.** `port/tests/gfx_atlas_uv_tests.cpp`
is unaffected.

**4.3 TMEM fast paths.** `[tmem2]` whole-span teardown, the value-gated SETTILE/SETTILESIZE and
the same-content skip exist independently in `port/3ds/gfx/n64/gdx_tmem.cpp` (468 lines). The
LUS copy (`interpreter.cpp:6140-6893`, `interpreter.h:294-358`) dies with no relocation.

**4.4 o2r resource path.** Keep `ResourceManager`/`ResourceLoader`/`Archive`/`O2rArchive`/
`BlobFactory`/binarytools — `GDiffuser_LoadArchiveFileBytes` (`port/AssetLoader.cpp:92`) feeds
`port/gdx_segment_source.c:252`, `port/gdx_audio_lle.c:543`, `port/disk_buffer.cpp`, and
`GDiffuser_LoadAssetBytes` feeds all 2,500+ `AssetBindings.c` loads. **Drop the five
`Fast::` factories and types**: `GDiffuser_LoadAsset` (`AssetLoader.cpp:28`) still has zero
callers, no `DisplayList` resource is ever executed, and the owned "resource-backed" texture
class (`gdx_gfx_timg_is_resource`, bridge:12648) is only a *name lookup*
(`gdx_lookup_asset_segment_o2r_key`) plus the whole-load gate — it reads the texels raw from
the segment image, never through `Fast::Texture`. Remove the 5 registrations at
`ResourceFactories.cpp:146-157`; `Blob` + `GenericArray` stay.

**4.5 Texcache ownership.** The owned cache is a member of `GdxInterp`, constructed and used
only on the render thread. Keep the audit receipt: `gdx3ds_texcache_note_thread`
(`port/3ds/gdx3ds_renderthread.cpp:554`) feeds `texcacheMainMut=` in the `[rt]` line (:514) and
must stay 0. Re-point its two call sites from the LUS `TextureCacheClear`/`Delete` (the
`lus-3ds-texcache-thread-audit` fold) to `gdx_texcache.cpp`'s equivalents.

**4.6 `[prof]` / `[profop]`.** Section indices are owned by `port/3ds/gfx/gdx3ds_gpu_prof.h:156-163`
(`BR=0 RUN VTX TRI IMP DRW MTX`), not by the bridge; `gdx3ds_prof_op_ticks[256]` likewise.
`gdx-dev prof --ab` parses those names. **Keep all seven indices and keep emitting BR=0** so
before/after A/B tables stay comparable. The bridge's own `gGdxBrOpTicks/Calls` (:349-352) and
the `[brop]` line die.

**4.7 Diagnostics.** `[race-dl]`, `[wide]`, `[brop]`, `[bcache-census]`, `[brfast]`, `[tri2]`,
`[trect]` are all bridge-walk or LUS-interpreter census lines (`bridge:10352-10933`,
`:6704-6741`): **drop them**, they measure code that no longer exists — `gdx_rt_glue.cpp:250`
already notes `[race-dl]` is silent under `interp=1`. **Keep** `[seg3-verify]` (source-integrity
tripwire on segment-3 images, moves with `gdx_assetseg.cpp`), `[transition]` + `[capture-probe]`,
`[vifallback]`, `[cadence]`, and the `[ninterp]` line that replaces them. `[c3d]`, `[rt]`,
`[trk]`, `[fx]`, `[dynlod]`, `[fh]` are outside the deletion.

**4.8 ILP32.** `decomp-ilp32.patch` (267 lines, touches `decomp/include/PR/gbi.h` +4 libc headers)
is **load-bearing after M5, not before**: the owned walker's whole address-class decision is
`w1hi == GDX_GFXW1_HOST_TAG` ("GDX1") vs `0` (`gbi.h:1739-1741`). It must never be reverted.

**4.9 The 15 decomp patches.** Only one references a bridge symbol —
`decomp-3ds-dma-low-address.patch` uses `gdx_register_main_module_range`,
`gdx_registered_host_capacity`, `gdx_resolve_registered_host_address`, all three survivors with
unchanged signatures. **No decomp patch needs editing**, and `PATCH_LIST`
(`tools/ci-3ds.sh:167-183`) stays at 15.

**4.10 Host tests.** `gdx_gfx_convert_tests` (`ci-3ds.sh:353`) compiles `n64_gfx_convert.cpp`
standalone with no bridge/LUS headers — it survives M5 verbatim because the desktop keeps the
file. `gdx_interp_host_tests` (:377) must grow the relocated resolvers: today the harness mocks
the seam; after M5 `gdx_ranges.cpp` / `gdx_assetseg.cpp` become host-compilable TUs in that
link line, which is a **net gain in coverage** (the resolver stack has never been host-tested).

## 5. Step plan, one gate each

| # | Step | Gate |
|---|---|---|
| 0 | Baseline: `interp=1 all=1` full replay corpus (canonical + B1/B3 long-tail), `[fh]` per frame, all SHOTs, `.3dsx` sha256 archived | the numbers M5 must reproduce bit-for-bit |
| 1 | Flip the default: `[native] interp=1` (task C1) and soak | A5/A6/A7 green, HOME clean, 30-min soak 0 errors |
| 2 | Move the resolver stack: create `gdx_ranges.cpp` + `gdx_assetseg.cpp` + `gdx_seg.cpp` as **pure code moves**, bridge keeps thin forwarders so both paths still work | `[fh]` divergent=0 on the full corpus, `interp=0` **and** `interp=1`; `interp_verify=1` 0 mismatches; `ci-3ds.sh` PASS |
| 3 | Move the job + present + capture code: `gdx_job.cpp`, `gdx_present.cpp`; owned `Gdx3dsWindow` replaces `Fast3dWindow`'s frame-config and `mGfxFrameBuffer` publish | SHOT byte-compare identical; every transition in the corpus captures; boot-logo + taskless frames still present |
| 4 | Lift `gfx_cc_get_features` + `CCFeatures` + `SHADER_*` into `gdx_cc.*`; repoint `gfx_citro3d.h:35`; `Gdx3ds_GetCitro3dRenderer()` returns `gdxn::Backend*` | host tests PASS; `[fh]` divergent=0; shader-id histogram identical |
| 5 | **Delete**: drop the 3 bridge TUs from `port/3ds/game/CMakeLists.txt:100-102`, the 12 `src/fast/*` sources + `include/fast/**` from `port/3ds/gfx/CMakeLists.txt:62-73`, the 5 factory registrations, `gdx3ds_seam_lus.cpp`, `gdx3ds_verify.h`, `gdx_dlsrc_bridge.h`, the harness app; refresh `MANIFEST.sha256` (`tools/vendor-lus.sh --manifest`); update `PROVENANCE.md` ("post-vendoring folds" section: the three `lus-*` hooks become history), `patches/README.md`, `STATUS.md`, `README-3DS.md` | `[fh]` bit-identical to step 0 on the whole corpus; **no `fast/` object in the link**; `ci-3ds.sh --jobs 8` PASS including the vendor-manifest check; `.3dsx` size and heap high-water recorded |
| 6 | Hardware soak + `[prof]` A/B | 30-min soak 0 errors, heap flat, `[prof]` sections within noise of step 1 |

Steps 2-4 are individually revertible and each leaves both paths switchable; only step 5 is
one-way. Do not collapse them.

**Estimate.** Step 2: 6 h (the resolver stack is 2,900 lines of accumulated fix history — move,
never retype). Step 3: 5 h. Step 4: 3 h. Step 5: 3 h. Gates dominate: ~4 emulator corpus runs
at ~40 min each plus one hardware session. **~3 working days of agent time plus one user-run
card**, matching the plan's M5 budget of 2 days once C1 has already paid for the soak.

## 6. Risks — what has only ever been verified through the LUS path

1. **The VI present fallback and the taskless/boot path.** `gdx_vi_present_fallback` (236 lines)
   reaches into `interp->mRendersToFb`, `mGameFb`, `mGfxFrameBuffer` and `CreateFrameBuffer`;
   the owned interpreter has never driven a frame with **no** GFX task. Boot logo, mode-change
   holds and every `[cadence]` hold frame are LUS-composed today. Highest risk in M5.
2. **The frame mirror.** `interp-progress.md` open item 2 records the LUS mirror rendering
   *black in game* while the owned mirror has content — i.e. the mirror path is already known
   to differ between the two interpreters, and `gdx_read_current_framebuffer` (the game's own
   transition snapshot, a **gameplay-visible** effect, not a diagnostic) reads it. Its port
   hook in `ovl_i2/transition.c` is exercised by exactly one screen class the corpus barely
   covers. Gate it explicitly with a transition-heavy replay before step 3 lands.
3. **De-LUSing the backend (step 4).** `gfx_citro3d.cpp` is 4,538 lines of verified, pixel-exact
   code whose header currently *is* `fast/interpreter.h`. Changing its base class and its
   `CCFeatures` source touches every shader path at once. Mitigate by making step 4 a pure
   header/namespace move with `static_assert`s on `CCFeatures` layout and a shader-id histogram
   compare, and by keeping it separate from step 5.

Lesser, but real: the long-tail screens (editor / EK / ending / records) still have no replays
(`interp-progress.md` M3) — B1 must land before step 5; `fx_hud` is inert under `interp=1`
(B2), so step 1's default flip changes draw counts; and `interp_verify` with Tier-2 levers on
is still unmeasured (`interphooks-progress.md` "Known gaps"), which is the harness M5 removes.
