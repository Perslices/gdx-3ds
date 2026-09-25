# port/3ds/patches — decomp working-tree patches

Submodule changes are never committed inside the submodules (plan rule); they live here
as patch files applied to the working tree, and land in the respective forks at
integration sign-off.

**libultraship is no longer patched here.** Its 3DS carve is vendored at
`port/3ds/vendor/lus/` with all 38 `lus-*.patch` already folded in, so the 3DS build needs
neither the submodule nor the patch step. Those 38 patch files moved to
`port/3ds/vendor/lus/patches/` (frozen provenance; apply order in `ORDER.txt`) and their
per-patch write-ups moved with them into `port/3ds/vendor/lus/patches/README.md`. Start at
`port/3ds/vendor/lus/PROVENANCE.md`.

Apply after `git submodule update --init decomp torch`:

```sh
git -C decomp apply ../port/3ds/patches/decomp-ilp32.patch
git -C decomp apply ../port/3ds/patches/decomp-port-segment-bzero.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-dma-low-address.patch
git -C decomp apply ../port/3ds/patches/decomp-port-audio-specwait-yield.patch
git -C decomp apply ../port/3ds/patches/decomp-race-cull-diagnostics.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-sky-overscan-deadend-note.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-machineselect-gradient-coalesce.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-rom-audio-port.patch
git -C decomp apply ../port/3ds/patches/decomp-port-course-select-state-reset.patch
git -C decomp apply ../port/3ds/patches/decomp-port-rival-detail.patch
git -C decomp apply ../port/3ds/patches/decomp-native-track-shim.patch
git -C decomp apply ../port/3ds/patches/decomp-port-determinism-pins.patch
git -C decomp apply ../port/3ds/patches/decomp-native-machines-rebuilt.patch
git -C decomp apply ../port/3ds/patches/decomp-port-title-demo-audio-pin.patch
git -C decomp apply ../port/3ds/patches/decomp-native-fx-shim.patch
git -C decomp apply ../port/3ds/patches/decomp-3ds-mainprof-checkpoints.patch
```

`tools/ci-3ds.sh` applies exactly this list, idempotently, and FAILS if a `*.patch` turns up
in this directory that its `PATCH_LIST` does not know about.

## decomp-port-segment-bzero.patch (M1 boot debug)

`#ifndef PORT` guards on the two raw `bzero(SEGMENT_BSS_START(x), SEGMENT_BSS_SIZE(x))`
calls (`src/sys/sys_main.c` leo, `src/sys/rom/disk_drive.c` ovl_i11). Under PORT the
overlay BSS markers are 1-byte stubs in `port/gen/LinkStubs.c` that the linker places
arbitrarily; on the 3DS link `leo_BSS_END` (0x009C4070) lands BELOW `leo_BSS_START`
(0x009D1160), so the size goes negative (≈4 GB as `size_t`) and the bzero zeroes the
tail of the host `.bss` before spinning on unmapped writes at the image end — the M1
black-screen boot hang (docs/research/m1-boot-debug.md). Every other segment-BSS
consumer (`Dma_LoadOverlay`, `DiskDrive_LoadOverlay*`, `Dma_ClearRomCopy`,
`func_800742FC`) was already a PORT no-op or guarded; these two were only compiled
with `EXPANSION_KIT` off, which desktop never builds.


## decomp-ilp32.patch (M1 integration; stream E's blockers #1 and #2)

Prerequisites for ANY 32-bit (3DS/devkitARM) build of the decomp — see
docs/research/32bit-sweep.md "Deferred / filed upstream" items 1-4:

- **include/libc/stdint.h** — proper ILP32 branch: `intptr_t`/`uintptr_t` spelled
  `int`/`unsigned int` exactly like newlib (`__INTPTR_TYPE__`), and 32-bit
  `INTPTR_MIN/MAX`, `UINTPTR_MAX`. Previously every non-LP64 host fell into the
  LLP64 branch and got a 64-bit `uintptr_t` (truncating casts everywhere + hard
  conflicting-typedef errors against newlib's `<stdint.h>`).
- **include/libc/stddef.h / stdlib.h** — `ptrdiff_t` (`int`) and `wchar_t`
  (`unsigned int`) spelled to match newlib/GCC on ILP32; `long`-based spellings are
  size-identical but type-distinct and hard-error when a TU sees both headers.
- **include/PR/os_time.h** — on `GDX_PLATFORM_3DS`, `osGetTime`/`osSetTime` are
  aliased to `gdx3ds_osGetTime`/`gdx3ds_osSetTime`: libctru's os.o also exports an
  `osGetTime` (ms since 2000, unrelated semantics) and is always pulled into the link,
  so the port shim (port/3ds/lus_glue/gdx3ds_libultra.cpp) cannot define the colliding
  name. Desktop builds see the original names.
- **include/PR/gbi.h** —
  1. `_GFXW1_PTR` tags host pointers with `GDX_GFXW1_HOST_TAG` ("GDX1") in the spare
     high32 of the u64 `GfxW1` on 32-bit builds: a 32-bit host pointer is otherwise
     value-indistinguishable from a segment token (3DS heap 0x08xxxxxx == segment 8;
     static data 0x00xxxxxx == segment 0). port/n64_gfx_bridge.cpp's ProcessList
     recognizes exactly this tag as its host-pointer fast path (kGfxW1HostTag32 —
     keep the constants in sync).
  2. Static `gs*` initializer forms rewritten onto `_GFX_STATIC_PTR_INIT`: a 32-bit
     toolchain has NO relocation that stores a symbol address into a 64-bit field
     (GCC: "initializer element is not constant"), so on ILP32 the macro initializes
     a word-split `GwordsStatic32` union arm — address relocated into `w1lo`, tag as
     a plain constant in `w1hi`. On 64-bit hosts and N64 builds the macro expands to
     the original `{{ w0, _GFXW1_PTR(x) }}` shape (semantically identical).


## decomp-port-audio-specwait-yield.patch (M1 race freeze)

`Audio_SetSpec` waits for the audio-heap reset with a raw
`do {} while (!AudioThread_ResetComplete())` spin on the game fiber (host thread).
Under PORT the reset stages only advance on the dedicated audio thread
(`gdx_audio_produce_one_tick`), and on the 3DS kernel — and Azahar's faithful
emulation of it — a running thread is never preempted by an equal-priority thread
(the devkitARM thread shim inherits the creator's priority), so the spin starves
the audio thread forever: a permanent, svc-silent, single-core freeze. The PORT
branch (both `audio/rom` and `audio/disk` variants) yields 1 ms per iteration via
`port/n64_sched.c:gdx_audio_specwait_yield`, which also feeds the watchdog's
`spec=` counter. Desktop is unaffected in behavior (preemptive OS scheduling
already saved it) but gets the same polite wait.


## decomp-3ds-dma-low-address.patch (M1 race-entry freeze)

`Dma_PortRamPointer` (src/sys/dma.c) treats any address `< GDX_DMA_RDRAM_SIZE`
(16MB) as a raw RDRAM offset. On 64-bit hosts pointer width alone excludes real
pointers from that branch; on ILP32 it does not, and the 3DS process image maps
from 0x00100000 — EXE globals below 16MB (gCourseCtx at ~0x0038xxxx) had their
DMAs rerouted into the RDRAM shadow. Course data never arrived, the course
segment list never linked, and Course_SegmentLengthsInit spun forever on NULL
segment pointers — the mode+difficulty race-entry freeze and its ~18.5k/s
unmapped-access storm (which also tanked emulator fps). The patch makes the
branch prefer a registered host range (the EXE image is registered at boot) for
addresses >= 0x00100000, restoring the 64-bit semantics; unclaimed addresses
still resolve as RDRAM offsets, and addresses below 0x00100000 (unmapped on the
3DS) stay unambiguous.


## decomp-3ds-machineselect-gradient-coalesce.patch (SELECT-PERF — background gradient)

`MachineSelect_BackgroundDraw` (shared by the machine-select AND machine-settings /
accel-max-speed screens — exactly the two screens reported extra slow) paints its
blue background gradient as one `gDPPipeSync + gDPSetFillColor + gDPFillRectangle`
triplet PER SCANLINE: ~224 (240 borderless) single-row fill rects, ~700 Gfx commands
per frame that the bridge re-translates and the interpreter rasterises as 2 tris each
(with the full per-tri state resolution). No other menu screen does this — main menu
and course select emit zero fill rects, the title screen one. It is a pure
differential CPU tax on the machine screens.

The `>>3` quantisation collapses the 224 rows to at most ~8 distinct 5551 colours
(r∈{0,1}, g=0, b∈0..7), so the patch (GDX_PLATFORM_3DS-gated; desktop DL content
stays byte-identical) coalesces runs of identical fill colour into one tall fill
rect: ~8-9 rects instead of 224/240. Pixel-identical by construction — a multi-row
FILL rect is the exact union of its single-row rects, the colour math per row is
unchanged (including the borderless index clamp), and a PipeSync between same-colour
fills has no visual effect. Expected: machine-screen draws= / tris= in [c3d] drop by
~215 rects / ~430 tris per frame, build= falls correspondingly.


## decomp-race-cull-diagnostics.patch (C2/C3-COURSE-CULL)

Bounded, read-only-except-kill-switch, PORT-gated diagnostics that settled where race
geometry was lost:

- `[cull]` in Course_Draw (C2): per-64th-frame census of the chunk depth/NDC cull
  with its matrix operands and 4 sample chunks; 64 bursts max.
- `[cull2]` (C3): full concatenated-matrix row (m30/m31, col0/col1, p00/p11), a
  reclassification of every depth-in-range rejected chunk by WHICH NDC test rejected
  it, by-index samples with both NDC coordinates, and a near-band (w < 3200) dump —
  this proved every road-ahead chunk passes the cull (ndcx ~0.02 at 200-4400 units)
  and the rejects are geometrically correct side/behind chunks.
- `[grp]` (C3): the chunk-group tessellator walk (both DrawChunkGroup functions),
  logging each group and each emitted span on armed frames — proved the painter's
  order walk emits contiguous spans covering the whole visible course.
- `[rvcull]` in Racer_Draw (C2): racer visibility census with operands for up to 3
  culled-but-active rivals — rivals vis up to 29/30 at race start, gate healthy.
- **`gdx-nocull.txt` kill-switch** (C3, via `gdx_nocull_test_enabled()` in
  port/n64_sched.c): force-passes every depth-in-range chunk, bypassing the NDC
  tests. The discriminating experiment: the race frame was pixel-identical with the
  cull forced open, exonerating the cull end-to-end and (with `gdx-nofog.txt`)
  pinning the invisible road on the fog LUT misbinding fixed by
  lus-3ds-fog-exact-params.patch.

Kept applied: the logs are the fastest regression tripwire for this class. The census
family is OFF by default behind the `GDX_DIAG_CULL` Dev Tools gate (C4: an armed frame
emits ~75 formatted-float lines and the human measured the bursts costing fps); enable
via the env var or F1 > Dev Tools > "Course/racer cull census". No game state is
written unless the nocull file exists; desktop behavior identical (PORT-gated, logging
only, switches off by default).


## decomp-3ds-rom-audio-port.patch (AUDIO-CONTENT — rom-driver host port; silence root cause)

The 3DS build compiles the ROM-side audio driver (`src/audio/rom/`, selected by
`GDX_EXPANSION_KIT=OFF` in port/3ds/game/CMakeLists.txt) — a driver the desktop port
NEVER builds (desktop is EXPANSION_KIT=1 → `src/audio/disk/`), so it had received
none of the disk driver's PORT work. Result on device: HLE ran an empty ~27-command
mix every tick and `[audio-out]` showed `nz=0` forever. Four fixes, all mirrors of
the disk driver's existing PORT blocks:

- **`external.c`**: `Audio_Update` was stubbed `return` under PORT (stale
  "Audio_Init is skipped" rationale from pre-EK desktop bring-up). That killed the
  game's entire per-frame audio pump — engine SE, SE stacks, BGM state machines.
  Un-stubbed (guarded by `gAudioContextInitialized`), plus `Audio_SetOutMode` and
  `Audio_GuitarSeqStart` un-stubbed. One-shot `[audio-rom]` receipt on first pump.
- **`lib/thread.c`**: the little-endian AudioCmd union fix (`QueueCmdS8/U16` stored
  their payload at byte 3 via the console's big-endian shift; every
  `AUDIOCMD_CHANNEL_SET_IO` — including `Audio_SEStart`'s sfxId — read back 0), plus
  the cross-thread wiring: QueueCmd under `gdx_audio_ctx_lock`,
  `ScheduleProcessCmds`/CreateTask drain over the lock-free cmd ring, taskStartQueue
  post to the host atomic. Periodic `[audio-seq]` receipt (seq players + notesOn).
- **`lib/load.c`**: the in-place font "relocation" read the big-endian font image as
  host-endian and patched garbage pointers into it. Replaced with the disk driver's
  `gdx_fontconv` host-native conversion, adapted to the ROM font layout (no SFX
  array; instruments at u32[1+i]; Sample flags `codec:4 medium:2`). `[fontconv]`
  receipt per converted font.
- **`lib/seqplayer.c`**: `AudioSeq_PortConvertSeqEnvelope` for the layer/channel ENV
  ops (seq blob stays big-endian; envelopes must be converted, pointer-keyed cache).

Port-side counterpart (committed normally, not a patch): `[audio-hle]` receipts in
port/n64_audio_hle.c — per-run command count, ADPCM/ENVMIXER voice-op counters, and
a one-shot "FIRST NONZERO output" line naming the first real PCM leaving the DSP.

Note: `decomp-3ds-rom-audio-port.patch` was REGENERATED 2026-08-21 (audio
forensics) from the feat/3ds-audiocontent worktree's final audio state: the
originally committed 736-line patch was an earlier iteration and is NOT
sufficient for audible output (empty ~27-cmd mix, adpcm=0). The regenerated
1015-line patch (external.c, lib/{audio.h,heap.c,load.c,seqplayer.c,thread.c})
is the audible state, roundtrip-verified against the sanctioned stack. See
docs/research/audio-regression-forensics.md.


## decomp-port-course-select-state-reset.patch (TIMETRIAL — post-GP course-select input freeze)
`CourseSelect_Init`'s `sCourseSelectState = COURSE_SELECT_CUP_SELECT` reset is
`#ifdef EXPANSION_KIT` in the decomp: the retail US ROM relied on the course_select
OVERLAY re-DMA restoring the `.data` initializer on every entry, and the EK
re-release added the explicit reset. The port links overlays statically
(`Dma_LoadOverlay` does not restore initialized data), so on the 3DS build — the
only non-EK port build (`port/3ds/game/CMakeLists.txt` sets `GDX_EXPANSION_KIT OFF`;
desktop is EK=ON, which is why desktop never showed this) — a SECOND course-select
visit inherits the previous visit's state (`COURSE_SELECT_CONTINUE` after advancing
to machine select, `COURSE_SELECT_NEXT_COURSE_*` after a GP cup's next-course
screens). `CourseSelect_Update`'s state switch has no case for those values, while
every draw switch does: the screen keeps rendering (identical DL every frame,
`[gpu] md=10 draws≈141 tris≈3760` — the frozen fingerprint in the hardware log)
but ignores ALL input forever. Hardware-verified as the "TIME TRIAL entry wedges
after a GP cup" freeze (watchdog `stage=7 fiber=-1` was a red herring — normal
between-frames snapshot; the `[tt]` park dump added alongside this patch shows all
fibers healthy in that state). Fix: perform the reset under `PORT` as well.

## decomp-port-rival-detail.patch (RIVAL-DETAIL — crowd-frame CPU via earlier rival LOD)
The 30-machine crowd frames pay per-rival triangles + draws + texture switches the
game already knows how to shed: Racer_Draw natively tiers every machine by view
depth (machineLod 1..6 at 230/290/380/470/1500, shadow gate unk_2B2 at 800, effect
tier unk_2B3 at 400/900) and D_800CDDB0 holds six per-machine display lists down to
a far impostor. This patch makes those stock thresholds engage earlier for RIVALS
only, driven by a port hook `gdx_rival_detail_level()` (0 NATIVE / 1 REDUCED /
2 MINIMAL; 3DS: `[perf] rival_detail`, live row on the menu DISP tab, implemented in
port/3ds/gdx3ds_menu.c — latched from the INI even when the menu UI is disabled).
Mechanism: a pre-pass after the sp580 projection row build marks biased racers in
`sGdxRivalBiased[]`; in the main tier loop a biased rival's `temp_fa0` is stretched
`near + (d - near) * bias` before the threshold chain (REDUCED near=250 bias=2.5,
MINIMAL near=120 bias=6). Piecewise-multiplicative: exact native inside the near
band, scale-free beyond it, and ALL downstream thresholds (LOD tiers, shadow,
effects) inherit the one rewrite — no per-threshold constants to keep in sync.
Effective REDUCED: lod5 by ~470 raw, shadows off past ~470 (stock 800), effects
2/1 tiers at ~310/~510 (stock 400/900). MINIMAL: lod5+ past ~180, shadows off past
~233, effects at ~167/~250.
Safety rails: human machines (index < gNumPlayers) are NEVER biased; in MINIMAL the
nearest GDX_RIVAL_PROTECT_COUNT(5) rivals by |view depth| keep native treatment (you
fight your neighbours — a rival right behind the camera still counts as near); the
TIME_ATTACK ghost loop (<=3 machines, not a crowd) and the GP_END_CS/COURSE_EDIT
all-lod-1 paths are untouched; NATIVE (0) leaves every comparison on the stock
constants; non-PORT builds compile the original code. `temp_fa0` is not read after
the tier assignments (verified), so overwriting it in place is safe. Applies
independently of the other decomp patches (racer.c is otherwise untouched by the
stack); listed last of the decomp series.
Applied after `lus-cc-key-uninit-shader-id.patch`.
## decomp-native-track-shim.patch ([native] track — course tessellator output redirect)
Three guarded hunks in src/game/course.c, all under `#if defined(PORT) && defined(GDX_NATIVE_TRACK)`
(the 3DS game target defines it): `#include "gdx3ds_track_shim.h"` after the asset header,
`GDXTRK_BEGIN(cameraIndex)` in Course_Draw right before the two setup display lists, and
`GDXTRK_END()` before its return. The shim (port/3ds/gfx/gdx3ds_track_shim.h) redefines
gSPVertex / gSPDisplayList / gSPModifyVertex / gSPTexture / gDPLoadSync / gDPSetTextureImage /
gDPLoadBlock; the C bodies of the ten chunk-group functions, both chunk walks and both setup
paths are untouched. Inactive (`[native] track=0`, `trkcensus=0` — the default) every macro
forwards to port/3ds/game/gdx3ds_track_gbi.c, a TU that invokes the ORIGINAL macros under the
same gbi.h, so the emitted packets are byte-identical to the unpatched build. Active, the
vertex/template-DL/modify-vertex commands are recorded into a per-GfxPool op list and replaced by
one G_NOOP marker per material run that Interpreter::GdxTrackMarker (lus-3ds-native-track-marker
.patch) replays at that exact display-list position. Non-PORT builds compile the original code.
Applied last of the decomp series (course.c is otherwise touched only by
decomp-race-cull-diagnostics.patch, in different hunks).

## lus-3ds-native-track-marker.patch ([native] track — marker replay hooks)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.
Three `__3DS__`-gated additions so the port can replay the course tessellator's recorded
vertex/template ops at the exact display-list position the stock commands occupied:
(1) interpreter.h declares `Interpreter::GdxTrackMarker` (defined by the port in
port/3ds/gfx/gdx3ds_track_replay.cpp), `GdxTrackPackState` and `GdxTrackSetCombine`;
(2) the F3DEX2 G_NOOP handler forwards `p == 0x5A/0x5B` (never emitted by the game or LUS's
OPEN/CLOSE_DISPS 7/8) to GdxTrackMarker; (3) GdxTrackPackState is a read-only snapshot of the
file-static draw-state memo + packed-batch latches (sGfxS7TriMemo, sGdxPack*) the replay's fast
append reproduces, and GdxTrackSetCombine runs the RDP set-combine handler plus the dirty mask
gfx_step would OR for it. Nothing on the stock path changes; desktop builds see only the header
declarations. Applied last of the libultraship series (context is the fully patched
interpreter.cpp).
## decomp-port-determinism-pins.patch

ORACLE track (`docs/research/native/03-oracle-plan.md` §3, `oracle-progress.md`). Pure delta
against the full decomp stack above (it touches `src/sys/sys_gfx.c`, which `decomp-ilp32.patch`
also touches, so it is applied LAST of the decomp series). Three wall-clock sources become
pinnable through `port/gdx_determinism.c` accessors; with no pin every original expression
runs byte for byte:

| Site | Original | Pinned |
|---|---|---|
| `src/sys/sys_gfx.c` `Math_Rand1Init` / `Math_Rand2Init` | `osGetTime()` seeds (the old `#ifdef PORT` read `getenv("GDX_RAND_SEED1/2")`, dead on 3DS) | `gdx_det_rand_seed(1|2, &seed)` — `[debug] rand_seed1/2`; desktop keeps the env fallback inside the accessor |
| `src/sys/sys_gfx.c` boot-logo hold | `while (osGetTime()-t0 < 230 frames) gdx_yield();` — one VI tick per yield, so the tick count consumed is wall-clock dependent and every later tick-scheduled event drifts | `gdx_det_boot_hold_ticks()` > 0 -> exactly that many `gdx_yield()`s (`[debug] boot_hold_ticks`, 230 under `det=1`) |
| `src/audio/disk/lib/thread.c`, `src/audio/rom/lib/thread.c` `audioRandom *= osGetCount()` | hardware entropy (the disk lib already pinned it under `gdx_pcm_capture_active()`) | `gdx_det_audio_pin()` OR'd into the existing condition; the ROM lib gets the same block |

Killswitch: absent `[debug] det` / `rand_seed*` / `boot_hold_ticks` / `det_audio` keys the
accessors return "unpinned". Receipt: `[det] init det=.. seed1=.. seed2=.. boot_hold_ticks=..
audio=..` at boot.
## lus-renderthread-texcache-owner.patch (round 4, feat/3ds-balance)
Weak hook `gdx3ds_texcache_note_thread(kind)` at the top of Interpreter::TextureCacheClear /
Lookup / Delete / DeletePalette (3DS only). port/3ds/gdx3ds_renderthread.cpp counts calls that
reach the cache from any thread but the render thread while it owns the cache and reports
`[rt] ... texcacheMainMut=` (must stay 0). Pure delta on the full stack; no behavior change.

## lus-native-machines-dl-hook.patch ([native] machines — G_DL call-out)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.

One hunk in `src/fast/interpreter.cpp` `gfx_dl_handler_common`: after `SegAddr`, a weak
`gdx3ds_mesh_dl_hook(interpreter, subGFX)` (port/3ds/gfx/gdx3ds_mesh.cpp) draws a native
machine mesh when the target is one of the registry's stub lists and skips the stub; it
also sets `sGfxS7DirtyMask = 0xFF` because the native draw binds material state directly,
so the tri/rect draw-state memos must re-derive. `#ifdef __3DS__`, weak symbol: desktop
LUS and a tree without the mesh module are byte-identical. Pure delta on top of the full
libultraship stack above (applied last); trivial to fold into the vendored interpreter.

## decomp-native-machines-rebuilt.patch ([native] machines — list generation bump)

`src/game/racer.c`: the three sites that (re)build the static machine model lists
`D_800CDDB0[]` (`func_8008D3C4`, `func_8008D5F4`, `func_8008D97C`) call
`gdx_native_machine_lists_rebuilt()` (port/decomp_port.c, no-op off-3DS) so the mesh
registry re-bakes at the next G_DL. Applied last of the decomp series.

## lus-nverify-diag.patch (native/interp — per-batch verify side channels)

**Folded into the vendored carve** (`port/3ds/vendor/lus/`, see its PROVENANCE.md); the patch file lives in `port/3ds/vendor/lus/patches/` for provenance only.
3DS only, pure delta on the full stack. Two hooks gated on an inline `gdx3ds_verify_mode != 0`
test (false unless `[native] interp_verify=1`; the gate was a `gdx3ds_verify_active()` CALL
until 2026-09-23 -- with no LTO in the tree that was a real per-command and per-triangle cost
on the default `interp=0` path, audit finding 5): `gdx3ds_verify_note_cmd(w0,w1)` at the top of gfx_step
(command-stream differential against the owned walker) and `gdx3ds_verify_note_tri(&vt)` in
GfxSpTri1 just before packed emission (tile window / extents / tm / loaded sizes / othermode /
viewport for the mismatching batch). Zero behavior change; one predictable branch per command
when verify is off. Temporary: goes with the LUS interpreter at native M5. See
docs/research/native/interp-progress.md.
## decomp-port-title-demo-audio-pin.patch (RESIDUALS — rare boot/title divergence)

Pure delta on top of the stack above; independent of every other patch (it touches only
`src/audio/disk/external.c:func_807442E4` and its ROM-library twin
`src/audio/rom/external.c:func_800B84B8`, which no other patch edits).

**What it fixes.** Those two functions are the *only* place in the game where state the
renderer can see is advanced by the audio library's synthesis position rather than by a
tick count:

```c
temp_lo = (s32) note->synthesisState.samplePosInt / 100;
if ((temp_lo > 2100) && (temp_lo < 2110) && (gGameMode == GAMEMODE_FLX_TITLE) &&
    (gTitleDemoState == TITLE_DEMO_INACTIVE)) {
    gTitleDemoState = TITLE_DEMO_ACTIVE;   /* -> attract demo */
}
```

`Audio_Update()` runs on the game thread once per frame, but `samplePosInt` is advanced by
the dedicated audio producer (`port/gdx_audio_thread.cpp:AudioThreadMain`), which self-pumps
every 5 ms of WALL CLOCK and produces while the DSP ring is below its target — it is not
tied to the VI tick. Emulated, the game runs far slower than realtime, so `temp_lo` advances
by tens per game tick and the arming window is only 9 units wide: whether any tick lands
inside it — and therefore whether the attract demo starts, and on which frame — varies
between otherwise bit-identical replays. Measured (RetroArch lane, `det=1`, three runs of
the same replay): `temp_lo` at input tick 127 = 8 / 4 / 8, at tick 420 = 1340 / 1343 / 1350.
Six title-hold boots of the same replay produced four different frame-700..1300 hash
sequences. This is the "1 run in 7 leaves the logo hold early into the attract demo"
residual recorded in `docs/research/native/oracle-progress.md`.

**The pin.** Under `gdx_det_audio_pin()` (`[debug] det_audio`, implied by `[debug] det=1`)
the two `gTitleDemoState` writes are skipped, so the attract demo is never armed from audio
state and the title screen advances on tick count alone. Unpinned — i.e. all real play —
the expression is byte for byte the original, including the
`AUDIOCMD_GLOBAL_INIT_SEQPLAYER` call in the same function, which is deliberately NOT
gated (it is audio-internal and does not touch game state).

**Deliberately not done:** deriving `temp_lo` from a tick counter so the demo still fires at
a fixed tick. That would be the more faithful pin, but it changes what every existing
parity replay records (all of them assume the demo does not fire), so it belongs to a
separate change with its own re-recorded replays.

## decomp-3ds-mainprof-checkpoints.patch (MAINPROF, NEXT.md A8)

`GDX_MP(...)` phase markers inside BOTH of `src/sys/sys_gfx.c`'s per-VI frame functions —
`func_80067D64` (`D_800CCFE4 == 2`) and `func_80067E98` (`== 3`, the triple-buffered path the
game runs in for most of a session; the switch is at the bottom of that file) — plus the macro
block that defines them (`#ifdef PORT` only; off
PORT `GDX_MP` expands to nothing, exactly like the `GDX_CK` block it sits beside).

**Why the decomp and not the port.** `func_80067D64` IS the game thread's frame, and on the
3DS it runs inline on the main thread inside `gdx_vi_tick` (the VI `osSendMesg` dispatches
the fibers from host context — `docs/research/renderthread-audit.md` §8). The phases the
A8 main-thread profile has to separate — `Audio_Update`, `func_800690FC` (the game-mode
update), `Gfx_LoadSegments`, `func_80069698` (the DL build), `Segment_LoadAssets` + the
transition capture, and `Gfx_SetTask` — are plain sequential calls inside that one
function with no port-side boundary between them. There is nowhere else to stand.

**Cost and blast radius.** `GDX_MP(n)` is one call to `gdx_mprof_ck` (`port/n64_sched.c`,
defined for every platform; a no-op off 3DS), whose first statement tests the single int
`gdx3ds_mprof_active`. That int is 0 unless `gdiffuser.ini [debug] mainprof = 1`, which
defaults to 0. Nine calls per game frame, in family with the `gdx_diag_verbose()` call the
adjacent `GDX_CK` lines already make. No game state is read or written: the profiler only
reads `svcGetSystemTick`. Armed, the receipt is emitted from the HOST loop at a window
boundary, never from inside a game fiber, so the replay's tick order is untouched —
verified by `[fh]` parity, `docs/research/native/mainprof-progress.md`.

## decomp-3ds-disk-audio-ilp32.patch (B11 — Expansion Kit build, ILP32 argument passing)

`src/audio/disk/lib/load.c` declares two port shims with 64-bit parameter types:

```c
extern void* gdx_host_calloc(unsigned long long count, unsigned long long size);
extern void* gdx_rdram_persist_alloc_raw(unsigned long long size, unsigned long long align);
```

Both real definitions take `size_t` (`port/shims.c`, `port/decomp_port.c`). On LP64 desktop
`size_t` and `unsigned long long` are both 64 bits, so the spelling was harmless and nobody
noticed. On the ILP32 3DS build `size_t` is 32 bits, and ARM32 AAPCS passes each `u64`
argument in a **register pair** — so the callee reads its *second* parameter out of the high
half of the first. `gdx_rdram_persist_alloc_raw` therefore saw `align == 0`, computed
`base = (top - size) & ~(0u - 1u) == 0`, decided the arena was exhausted and called
`abort()`. That is the first hard stop of an `GDX_EXPANSION_KIT=ON` 3DS build: it dies at
the first soundfont conversion, around guest frame 180, with
`[rdram] FATAL: arena exhausted (persist)`.

This patch respells both externs as `unsigned int`, exactly as the ROM twin
`src/audio/rom/lib/load.c` already does (that file carries the same fix and the same
comment, applied when the ROM audio driver was ported in
`decomp-3ds-rom-audio-port.patch` — the disk twin was skipped because the 3DS has never
built it).

**Affects only `GDX_EXPANSION_KIT=ON`.** `audio/disk/` is excluded from the shipped
(EK=OFF) source list by `port/cmake/GdxGameSources.cmake`, so the patch changes no byte of
the default build.

## decomp-3ds-disk-audio-heap-ilp32.patch (B11 — Expansion Kit build, static audio heap)

`src/audio/disk/lib/audio_heap.c` sizes the disk audio library's static heap at
`0x2ECA00 * 4` (**12.2 MB**) under `PORT`, and says why:

> The pools are carved by N64-tuned spec sizes, but several cached structs carry 64-bit
> pointers on the host and outgrow their N64 footprints.

That is a **64-bit-host** headroom figure. The overflow it pays for is pointers doubling
from 4 bytes to 8, which does not happen on an ILP32 target. On the 3DS the `x4` takes
12.2 MB of BSS out of a ~49 MB application heap before a single allocation — by itself the
largest single reason an `GDX_EXPANSION_KIT=ON` 3DS build runs out of memory at boot. (For
scale: the ROM audio library's `gAudioHeap` is `0x3B000`, 236 KB.)

This patch makes the multiplier `x2` when `__SIZEOF_POINTER__ == 4`, leaving 2.9 MB of
headroom over the retail carve, and keeps `x4` on every 64-bit host so no desktop build
changes. Total EK BSS: 21.59 MB → 15.45 MB.

**Affects only `GDX_EXPANSION_KIT=ON`.** `audio/disk/` is excluded from the shipped (EK=OFF)
source list, so the patch changes no byte of the default build.
