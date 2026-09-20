# interp track — progress (Tier 1: owned F3DEX2-to-citro3d interpreter)

Branch `native/interp`, worktree `~/code/gdx-3ds/interp`. Plan: `06-interp-plan.md`.
Killswitch: `[native] interp = 0|1` (default 0), `interp_verify`, `cpuclip`, `blendflush`. Receipts:
`[native] interp=… path=lus|owned`, `[ninterp] …` every 64 tasks, `[nverify] …`.

## State

| Milestone | Status | Evidence |
|---|---|---|
| M0 skeleton | code done, emu verification pending (lock contention) | builds; walker + histogram verify |
| M1 menus | DONE (parity evidence below) | dc8bb6d..7a7b831 |
| M2 race | DONE: race replay per-batch verify 0 mismatches (root cause + fixes below) | 8cc1661, d52ff8a |
| M3 long tail + soak | soak DONE (50 min, 0 errors, heap flat); long-tail screens NOT exercised (no replays) | |
| M4 profile + card | emu [prof] DONE (relative), card staged ~/Desktop/G-Diffuser-3DS-interp/; interp default stays 0 | 75c532c |

## Layout (what exists)

```
port/gdx_dlsrc_bridge.h          C seam: the bridge's resolver exported per job (see Deviations)
port/n64_gfx_bridge.cpp          + adapter Native* wrappers (public block before ConvertRoot),
                                 + extern "C" export block at EOF, + weak gdx_ninterp_job_run hook
                                 in gdx_gfx_run / gdx_gfx_job_run (3DS only, off unless interp=1)
port/3ds/gfx/n64/gdx_gbi.h       opcodes / fields / mode bits, F3D legacy opcodes, Cmd
port/3ds/gfx/n64/gdx_dlsrc.*     ListRef {base,limit,stride,isBig,isF3d}, ReadCmd, DlSrc (seam calls)
port/3ds/gfx/n64/gdx_interp.h    Rsp/Rdp/RenderState mirrors, Counters, class Interp
port/3ds/gfx/n64/gdx_dl.cpp      walker: 32-deep stack, RemapF3d, Exec() flat switch
port/3ds/gfx/n64/gdx_interp.cpp  RunFrame prologue/epilogue, SpReset, pure-state handlers, stubs
port/3ds/gfx/n64/gdx_backend.h   gdxn::Backend (LUS-free subset of GfxRenderingAPI + vbopack)
port/3ds/gfx/n64/gdx_rt_glue.cpp BackendC3D adapter, ini latch, receipts, gdx_ninterp_job_run
port/3ds/gfx/n64/gdx_log.h       gdxn_logf (svc + filelog on device)
port/3ds/gfx/n64/gdx_cc.*        GenerateCC verbatim + 256-slot open-addressed pool (prev fast path)
port/3ds/gfx/n64/gdx_tmem.cpp    SETTILE/SETTILESIZE value gates, span store (whole-span teardown),
                                 lazy views (LoadedTexAt), LOADBLOCK/LOADTILE/LOADTLUT, same-content skip
port/3ds/gfx/n64/gdx_texcache.cpp 1024-entry content-keyed LRU + RGBA16/32, IA4/8/16, I4/8, CI4/8 decoders
port/3ds/gfx/n64/gdx_rsp.cpp     MTX/POPMTX, G_VTX T&L (lighting, texgen, FLX alpha LUT, fog, clip flags),
                                 MOVEMEM (viewport/lights/lookat/FLX alpha light), DMA_IO, MODIFYVTX, LINE3D
port/3ds/gfx/n64/gdx_tri.cpp     SpTri: reject screen, CPU clip fan ([native] cpuclip, default 1), cull,
                                 state diff -> Flush, combiner key, tile extents/tm, sampler, shader, packed
                                 emission + aux; Flush -> DrawPackedBatch
port/3ds/gfx/n64/gdx_rect.cpp    viewport/scissor mapping, DrawRectangle/TextureRectangle/FillRectangle
port/3ds/gfx/gdx3ds_verify.h     [nverify] record/compare ring (hooks in gfx_citro3d.cpp)
port/3ds/gfx/CMakeLists.txt      add_library(gdx3ds_n64); port/3ds/CMakeLists.txt links it
```

## Deviations from the plan (with reasons)

1. **Address resolution is delegated, not rewritten (§3.2).** `gdx_dlsrc` calls
   `gdx_gfx_job_*` exports that wrap the adapter's private resolvers (`ResolveGuarded`,
   `TranslateDisplayListPointer` minus the enqueue, `ResolveWideAssetStubPointerFast`, the
   moveword segment case). Reason: the bridge's resolver stack (mode-owned segment 9, venue-bank
   aliases, port BSS aliases, EK N64 address ranges, RDRAM low32, registered host pointers,
   epoch seqlock) is the accumulated fix history; re-deriving it would re-open every bug it
   closed. It runs against the job's segment view on the render thread exactly as the bridge
   walk does. M5 moves the bodies. Cost: the bridge still converts at prepare (brMain) until
   M4 skips `ConvertRoot` under interp=1.
2. **Legacy F3D dialect (not in §3.3's 47 cases).** Segment-3 machine-part lists are F3D
   (0x01 MTX, 0x03 MOVEMEM, 0x04 VTX, 0x06 DL, 0xB6..0xBF). `RemapF3d` mirrors the bridge's
   isF3DSource arms. Dialect comes from the seam (`GdxAssetPointerDialect` / scan).
3. **Frame-level config is borrowed from the LUS Interpreter instance** (`gdx_gfx_frame_config`:
   dimensions, rendersToFb, gameFb, widescreen/fixed-aspect latches, texture filter) until M5.
   The LUS `StartFrame` still runs per frame from the window callback in both paths.
4. **The boot-logo after-clear hook** (`GDX_SEED_BOOT_LOGO`, env-gated, off on 3DS) is not
   carried.
5. **CPU clipping default ON** (`[native] cpuclip`, default 1): parity with LUS first; the
   plan's no-clip mode is the A/B (`cpuclip=0`) for the M2 tunnel gate.
6. **Legacy variable-stride emission not carried**: when `VboPackBegin` refuses (no shader,
   no frame, pool exhausted) the batch is dropped and counted (`packRefused`). LUS fell back
   to the repack path there; on this port the refusal only happens at pool exhaustion.
7. **Decode scratch is 256 KiB** (LUS: 4 MiB `max_tex_size^2*4`); decodes over that are
   counted (`texDecodeOverflow`) and skipped.
8. **"Resource-backed" class** = `gdx_gfx_timg_is_resource` (the bridge's o2r-key
   condition). Those take the legacy decode from the source bytes with a source-byte FNV as
   the cache identity (LUS used the archive path); raw sources hash the TMEM span exactly as
   LUS does. Risk: the o2r resource bytes vs the segment-image bytes must be identical for
   parity — the verify run decides.

## Opcode coverage

Legend: F = full (M0), S = state-only (M0, value recorded), C = counting stub, - = n/a.

| op | name | M0 | notes |
|---|---|---|---|
| 00/E0 | NOOP/SPNOOP | F | |
| 01 | VTX | C | [M2] T&L |
| 02 | MODIFYVTX | C | [M2] |
| 03 | CULLDL | F | no-op as LUS |
| 04 | BRANCH_Z | F | semantics = LUS; z from loaded vertices ([M2]) |
| 05/06/07 | TRI1/TRI2/QUAD | C | [M1] rect tris, [M2] 3D |
| 08 | LINE3D | C | live only inside an L3DEX2 section |
| 3A | EXTRAGEOMETRYMODE | F | |
| D6 | DMA_IO | C | [M2] FLX alpha light |
| D7 | TEXTURE | S | first_tile change arms textures_changed |
| D8 | POPMTX | C | [M2] |
| D9 | GEOMETRYMODE | S | |
| DA | MTX | C | [M2] |
| DB | MOVEWORD | S | numlight, fog (flush on change), segment (via seam) |
| DC | MOVEMEM | C | [M2] viewport/light/lookat |
| DD | LOAD_UCODE | F | variant + L3DEX2 line section; unknown load dropped |
| DE | DL | F | push/branch; DlHook for machines track |
| DF | ENDDL | F | |
| E1 | RDPHALF_1 | F | latches BRANCH_Z target (resolved list) |
| E2/E3 | SETOTHERMODE_L/H | S | |
| E4/E5 | TEXRECT/FLIP | C | consumes its two halves inline; [M1] |
| E6..E9 | syncs | F | no-ops |
| EA/EB/EC | KEY/CONVERT | S | stored, unused |
| ED | SETSCISSOR | S | [M1] aspect mapping |
| EE | SETPRIMDEPTH | S | |
| EF | RDPSETOTHERMODE | S | |
| F0 | LOADTLUT | C | [M1] |
| F1 | RDPHALF_2 | F | texrect trailer only |
| F2/F5 | SETTILESIZE/SETTILE | S | [M1] value gates |
| F3/F4 | LOADBLOCK/LOADTILE | C | [M1] TMEM |
| F6 | FILLRECT | C | [M1] |
| F7..FB | colors | S | prim/env/fog flush on value change |
| FC | SETCOMBINE | S | LUS color_comb/alpha_comb packing |
| FD | SETTIMG | S | token kept; resolution lazy at load time |
| FE/FF | SETZIMG/SETCIMG | S | resolved via seam |

## Invariants checklist (plan §4)

Unticked until the owning milestone lands: (1)-(8) TMEM [M1], (9)-(13) texcache [M1],
(14)-(15) combiner [M1], (16)-(18) T&L [M2], (19)-(21) flush [M1: 19, 21 partially — prim/env/
fog value gates and pipesync no-op are in place], (24)-(26) [M1], (27) dispatch is a switch [x],
(28) no shared_ptr [x], (29) no per-task heap churn [x — fixed arrays], (30)-(33) [M1],
(34)-(36) [M4].

## M1 parity evidence (2026-09-13)

Mechanism 1 — per-batch differential (`[native] interp_verify=1`, both interpreters on one job,
backend record/compare of {shader ids, texture content hashes + samplers, depth/decal/alpha,
prim depth, alpha threshold, viewport fold, scissor, aux, vertex count, packed VBO floats}):
`batch_mismatch_tasks=0` over 768 tasks (title + menus, replay menus.autoinput.txt, RetroArch
lane instance interp2, OpenGL, `[debug] trectbatch=0`). With the atlas ON the compare diverges
only through atlas cell placement (both paths' uploads share the pages), a verify-mode artifact.

Update (bb37e15): after fixing the verify-mode artifact (both interpreters share one backend;
each pass now re-applies its own cached backend state first), the differential is
`batch_mismatch_tasks=0` over 19,328 tasks (title -> menus -> race start, trectbatch=0,
RetroArch instance interp2, OpenGL, run verify12). Diagnostics kept behind interp_verify:
command-stream differential (LUS gfx_step vs owned walk), per-task RDP state snapshots
(start/end, both sides), per-tri othermode/viewport/walk side channel, batch-sequence dump.

Mechanism 2 — SHOT byte compare on the real configuration (atlas on, Azahar Qt lane,
`gdx-dev emu run --replay menus.autoinput.txt --shots`, runs 20260913-100935-qt-shotsL vs
20260913-101129-qt-shotsO): `cmp` on autotest/*.bmp: title/menu/mode/machine/course/race
`_scan.bmp` all SAME; `.bmp` mirrors SAME except course.bmp where the LUS mirror is all-black
(nonzero=0; the oracle plan's known "mirror black in-game" defect) and the owned mirror has
content. Receipts: `[native] interp=1 path=owned cpuclip=1`, `[ninterp]` unknown=0 dropped=0
runaway=0 rootrej=0 over every window; race-start windows carried vtx=125k tri=224k per 64
tasks, i.e. the 3D path was exercised and matched byte-for-byte on race_scan.bmp.

Lane notes: RetroArch lane launch-crashes ~50% of the time in the frontend (ozone context
reset "FS pagein error", macOS crash reports) and gdx-dev then parses the previous run's stale
SD log — scratch wrapper `<scratch>/native/interp/rr.sh` clears the SD log/BMPs, requires a
watchdog beat and retries. Vulkan instances hung with no guest output (killed only my own).

## M2 race evidence (2026-09-13, race.autoinput.txt = A-mash into the default GP race)

- Per-batch differential (interp_verify=1, trectbatch=0, RetroArch interp2 OpenGL, run
  raceverify2): 27,904 tasks, `batch_mismatch_tasks=13` (0.05%): 12x `state field=3` (unit-0
  texture identity) and 1x `field=7` (alpha-compare threshold). All 12 share one tri signature:
  3D draw through the LOAD tile as render tile (`t=7 ft=7`), 64x16 RGBA16, `tm=0`, loaded record
  `line=0 size=2048 full=0 orig=2048` (an interior/zero-line view), `sc=8000/FFFF`. No RSP/RDP
  state divergence at any task start/end (48-word snapshots), command streams identical.
  ROOT-CAUSED 2026-09-17, see "M2 residual root cause" below: the LUS side was wrong (o2r-by-name
  delivery truncated a two-symbol block load), and the field=7 task was a missing owned-side
  blend-alpha flush. Both fixed; re-verify = 0 mismatches.
- SHOT byte compare, real config (atlas on), owned vs LUS on the same RetroArch instance
  (runs raceShotsO / raceShotsL): trk1..trk4 `_scan.bmp` differ by 82 bytes each = 42 px in a
  16x4 patch at (139,184)-(154,187) (player-machine area, +-2 levels of shade). LUS-vs-LUS
  across two runs (raceShotsL vs raceverify2's LUS-drawn shots) is byte-identical, so the
  replay is deterministic and the 42 px are a real owned-vs-LUS residual (same class as above).
- `[native] cpuclip=0` (PICA-only clipping) A/B on the same replay: 2667 bytes (~890 px) per
  SHOT differ from cpuclip=1 and the per-frame draw count collapses 123 -> 22; cpuclip=1
  matches LUS's nD=123 exactly. The default course has no tunnel; the tunnel-roof A/B needs a
  tunnel course on hardware (TEST-PLAN step 3).

## M2 residual root cause (2026-09-17)

Method: `[nverify]` per-batch verify on the race replay (RetroArch lane, `gdx-dev retro run
--replay`), with the tri side channel extended to print, for the first mismatching batch, each
side's unit-0/unit-1 source address + bridge address class + FNV of the first 64 source bytes +
FNV of the 64 TMEM bytes at the render tile + the span base record (commit 8cc1661). The
detail line goes through `gdx3ds_verify_src_detail` on its own log line (filelog caps lines).

1. `field=3` (verify field index is 1-based: f[2] = texture unit 0 upload content hash), 12
   tasks, 3D draw through tile 7, 64x16 RGBA16, loaded view `line=0`. Evidence (run rv4):
   LUS unit-0 source = heap pointer (`cls=0`) whose first 64 bytes AND whose TMEM window hash to
   FNV(64 zero bytes) = 69691905; owned source = 0x0AC05948 = hostrange[19] (the decoded
   course_track_gfx segment image, 140544 B) + 0x7808 = `D_8007808`, real content. The span
   record on both sides is a 4 KB block whose interior word 256 is this tile: the course DL
   loads `D_8007008` + `D_8007808` (two stacked 64x16 RGBA16 textures, yaml offsets 0x7008 /
   0x7808) with ONE 4 KB LOADBLOCK and renders the second half through tile 7 at TMEM word 256.
   The bridge rewrote that SETTIMG to the o2r-by-name opcode (`course_track_gfx/D_8007008`, a
   2 KB resource); LUS's legacy resource decode then read 2 KB past `ImageData` (heap zeros).
   N64 semantics: the RDP reads the contiguous segment, i.e. D_8007808's bytes -- the OWNED side
   was right. Fix (bridge, shared file, minimal): `GdxO2rServesWholeLoad(raw, loadBytes)` --
   the o2r rewrite is taken only when the estimated load extent (LOADBLOCK/LOADTILE/TLUT scan,
   already computed for the copy path) fits the symbol's byte size (`gdx_lookup_asset_segment_
   sym_size`, added to port/gen/AssetBindings.c and tools/gen_asset_bindings.py); otherwise the
   raw-copy path reads the decoded image. The seam `gdx_gfx_timg_is_resource` takes the same
   load extent so the owned "immutable" class matches. The bridge comment already excluded
   RDRAM multi-tile atlases for exactly this reason; asset segments were assumed 1:1.
2. `field=7` (f[6] = alpha-compare threshold), 1 task at the race start (viewport 70,-90, FLX
   variant): both sides record blend alpha 0 at the tri, but LUS flushes the pending batch when
   SETBLENDCOLOR changes the alpha (`[blend-alpha-flush]`, the threshold is sampled at Flush) and
   the owned interpreter did not, so the owned batch was submitted with the NEW threshold. N64:
   alpha compare uses the register in force when the primitive is drawn -- LUS was right. Fix:
   `Interp::DpSetBlendColor` flushes on an alpha change (`[native] blendflush`, default 1).
3. Re-verify (rv20on, merged native/main b7d4907 = deterministic texture ids, trectbatch=0):
   `[nverify] tasks=13440 batch_mismatch_tasks=0` over 6,802 frames of the race replay
   (title -> menus -> GP race, SHOT ticks 5400/7200). rv14 (gate only, no blend flush):
   12,160 tasks, 1 mismatch (the field=7 task). Host harness: ALL PASS after the seam change.
4. NOT an interpreter defect, but a verify-harness hazard found on the way: `sCourseMinimapTex`
   (EXE .bss, 64x32 RGBA16 HUD rect, both sides read it live at 0x0043CAEC `cls=8`) is
   composed by the game thread every frame; when the lane is loaded (5+ RetroArch instances)
   the two verify passes of one task can straddle a recomposition and the run reports a
   texture-hash storm (rv19: 2,308/13,568, first 64 bytes zero on both sides = the blank top
   row, content differing below). Quiet lane: 0. Real runs read once, so this is a
   double-read artifact of verify mode; a sync render-thread mode for verify would remove it.
5. Lane notes: with other agents' RetroArch instances running (MoltenVK), my instance
   host-crashes (`Unhandled EXC_BAD_ACCESS`) at the first heavy frame regardless of build
   (verified on pristine fce2dd1, interp=0 too); rr.sh retries 4x and it passes when the lane
   is quiet. Emulator commands need the Bash sandbox off. gdx-dev merges `--ini` keys into the
   instance's ini and they PERSIST across runs (interp3 still carried `cpuclip=0` from the
   clip A/B), so every lever is passed explicitly now.

## M3 soak evidence

RetroArch instance interp3, OpenGL, interp=1, race replay then whatever the game does after
(run soak30, console at /tmp/interp-art/soak30/): 103,904 guest frames (~50 min), 0 [fatal],
0 host crashes, every `[ninterp]` window (1,476 of them) unknown=dropped=runaway=ovf=rootrej=0,
heapUsed 42,230,504 B from beat 209 to the end (hwm 42,230,528; boot 40.9 MB). Not exercised:
course editor, EK screens, ending, records, transitions beyond title->race (no replays exist).

## M4 profile (emulator, gputrace ON, relative only -- hardware is truth)

Same replay, same frames (9025-9153), nD=123 both:
  LUS   : br=2.87 dsp=17.77 vtx=1.87 tri=25.10 imp=2.32 drw=2.40 mtx=0.01  (~52.3 ms)
  owned : br=0.04 dsp=27.25 vtx=0    tri=0     imp=0    drw=2.40 mtx=0     (~29.7 ms)
(owned build of that run had no sub-sections; 75c532c adds VTX/TRI/IMP/MTX with LUS's indices.)
br=0 confirms the conversion pre-pass is gone. Hardware `[prof]` windows are TEST-PLAN step 6.

## Host harness

`port/tests/gdx_interp_host_tests.cpp` + `gdx_mock_backend.h` (wired into `tools/ci-3ds.sh`):
walk push/branch/ENDDL + F3D remap, GenerateCC ids, SETTIMG/SETTILE/LOADBLOCK/TEXRECT ->
RGBA16 upload + packed batch corners + cache hit on replay, MTX/VTX/TRI1 on a big-endian list.
Build line is in the file header (n64/ include dir FIRST: port/gdx_interp.h is a different
header). The mock seam maps GDX1-tagged low32 back to the range (64-bit host; exact on device).

## LUS-side verify hooks

`port/3ds/patches/lus-nverify-diag.patch` (last in tools/ci-3ds.sh PATCH_LIST) carries the two
`gdx3ds_verify_*` hooks in libultraship/src/fast/interpreter.cpp; the submodule itself is
never committed. Reapply with the stack (`tools/ci-3ds.sh`) on a fresh checkout.

## Open items / next steps

1. DONE (2026-09-17): tile-7 residual = o2r-by-name truncation on the bridge side; field=7 =
   missing owned blend-alpha flush. Race replay verify 0/13,440. SHOT byte re-compare on the real
   config (atlas on, cpuclip=1, blendflush=1; owned rv21O vs LUS rv23L, same instance): trk1_L /
   trk2_L byte-identical (0 of 288,054 bytes differ) -- the 42-px player-machine patch is gone.
2. LUS frame-mirror `course.bmp` is black while the owned mirror has content (scanout identical).
3. Long-tail screens (editor/EK/ending/records) need replays; none exist yet.
4. M5 (delete bridge + LUS) untouched by design; both paths stay switchable, default interp=0.
5. Hardware card at ~/Desktop/G-Diffuser-3DS-interp/ (TEST-PLAN.txt): parity + tunnel cpuclip
   A/B + [prof] windows are user-run.

## Gotchas

- zsh: `=====` on a command line is `=cmd` expansion; quote it.
- The bridge's adapter is private; the export block adds a public `Native*` wrapper block
  right before `ConvertRoot()`. Keep it a pure call-through.
- `gdx_ninterp_job_run` is weak in the bridge so desktop + the DL-test harness link.
- GdxGfxJobPrepare opens `GDX_PERF_SUB_GFXRUN`; when the owned path takes the job it must
  close it (`gdx_perf_sub_end`) — GdxGfxJobRun's scope guard is bypassed.
- The LUS flat dispatch's G_LOAD_UCODE handler sets the variant only (no fog reset).
- fzerox.o2r textures are OTEX v0 records: Flags=0, HByteScale/VPixelScale=1, ImageData = raw
  N64 texels. LUS decoded them by the legacy DRAM path; the `immutable` class here does the same
  from the segment image (same bytes).
- Under interp=1 (not verify) the prepare skips ConvertRoot; GdxGfxJobRun would early-return on
  converted==nullptr, so the owned hook MUST take every such job (it does unless no window yet).
- `[nverify]` under the atlas: expect UV/state diffs from cell placement; verify with
  trectbatch=0. The M0 histogram compare is receipt-only (bridge opCounts skip wide-cache hits).
- Weak `extern "C"` receipts must not live in host-compilable TUs (Mach-O refuses weak
  undefined at link); route through gdxn::Backend.

## Log

- 2026-09-07: worktree bootstrapped (48 patches, host tests PASS, .3dsx built); M0 files.
- 2026-09-12: resumed after a 5-day gap. OBJECT-library link fix (weak hook never pulled the
  archive member). M0 emu run was displaced by the track agent's run_azahar.sh taking the lock
  (my script now writes owner.json, releases only its own lock, and aborts if the lock changes
  hands). M1 core written and building (commits dc8bb6d, verify commit). Verify run queued.
- emu run script: <scratch>/native/interp/emu-run.sh <label> <secs> <autoinput> key=val...
  (merges [native] keys into the shared ini, snapshots to /tmp/interp-art/<label>-log.txt).
- 2026-09-13/17: M2 residual root-caused and fixed (bridge o2r whole-load gate + owned
  blend-alpha flush); race verify 0/13,440 (rv20on). WIP 8cc1661, killswitch d52ff8a. The
  vendored LUS now carries the verify hooks directly (no more lus patches); merged native/main
  b7d4907.
