# 03 — Frame oracle, input record/replay, determinism pins

Track: **oracle**. Planning only — nothing here is implemented. Companion to `00-master-plan.md`
§3 and §4.2. Line numbers cite `~/code/gdx-3ds/m1` at `feat/3ds-hwaudio`.

## Goal

Make "did this frame change?" answerable mechanically, on device, for frame N exactly:

1. A **true capture** — the citro3d colour target for frame N, after its GPU work completed —
   emitted as a BMP and as a **frame hash** receipt line.
2. **Input record/replay** (`.gdxrec`) so the same inputs can be replayed on emulator and
   hardware through one injector, and converted to the hwlink UDP path unchanged.
3. **Determinism pins** so `same .gdxrec + same seed + same build → identical [fh] sequence`,
   in-race, not just on menus.
4. Host tools (`framediff.py`, `fhdiff.py`) so nobody hand-rolls `cmp` again.

## Current state (what is wrong, precisely)

- **`<label>_scan.bmp` is stale and reads the wrong buffer.** `GdxDumpTopScanoutBmp`
  (`port/n64_gfx_bridge.cpp:8626`) calls `gfxGetFramebuffer(GFX_TOP, GFX_LEFT)` from inside
  `gdx_gfx_run` — *before* `C3D_FrameEnd` has display-transferred frame N. It captures the
  libctru scanout back buffer, which holds frame N-1 or N-2. Full verdict:
  `docs/research/reference-diff-2026-08-20.md` Part 1 ("every scanout-verified conclusion about
  a late-frame pass is suspect").
- **`<label>.bmp` (the frame mirror) is black in-game.** `GdxDumpNamedFrameIfRequested`
  (`port/n64_gfx_bridge.cpp:8700`) reads `gFrameMirrorFb`, a 320x240 interpreter framebuffer fed
  by `CopyFrameBuffer` (`:8772-8779`). In-game it returns black (RESUME.md B2). So *neither*
  existing SHOT image is frame N.
- The readback machinery itself is now sound: `ReadFramebufferToCPU`
  (`port/3ds/gfx/gfx_citro3d.cpp:3297-3417`) untiles via `C3D_SyncDisplayTransfer` and polls a
  two-word sentinel because `C3D_SyncDisplayTransfer` does **not** wait while a C3D frame is
  active (`:3344-3357`). That code is reusable; only its *source* and *call site* are wrong.
- **No hash, no receipt.** Parity today means copying BMPs off the SD card and running `cmp` by
  hand, per agent, every time.
- **No input recorder.** `gdx-autoinput.txt` (`port/input_bridge.c:275-386`) is capped at **64
  events**, drops a trailing block of `SHOT` lines at parse (RESUME.md:150-153), and `s_tick`
  (`:285`, incremented at `:385`) is a file-static — nothing else can see the tick.
- **In-race runs are not reproducible.** Control-vs-control SHOTs differ 3.0-13.3 kB
  (`docs/research/bridgecache-progress.md:206-210`, `docs/research/dynlod-progress.md:76-79`) —
  "timer digits + pack positions" — while menu/preview shots are byte-identical.

## Design

### 1. True capture

**Point.** A new `gdx3ds_oracle_after_endframe()` called immediately after `EndFrame()` in both
present paths: the render-thread callback `main_3ds.cpp:1010`
(`rtCb.endFrame = [] { sRtWindow->EndFrame(); }`) and the `rtMode == 0` branch (`:1109`). At that
point `C3D_FrameEnd(0)` (`gfx_citro3d.cpp:1192`) has queued frame N's command list and its
display transfer, and no frame is open. When armed, the hook calls `C3D_FrameSync()` (waits for
the queued frame) and then reads `mFramebuffers[0].target->frameBuf.colorBuf`.

**Why the target and not the scanout:** the target is what the GPU drew into this frame; the
scanout buffer is double-buffered and one transfer downstream, which is exactly the off-by-one
the current oracle has. Reading the target after `FrameEnd + FrameSync` is unambiguously frame N.

**Resolution: the full 400x240 target, not 320x240.** The main target is
`C3D_RenderTargetCreate(240, 400)` (`gfx_citro3d.cpp:952`, portrait 240x400 = landscape 400x240).
That is the real output extent, including the fixed-aspect widescreen columns the 320x240 mirror
throws away — and a resample to 320x240 hides exactly the sub-pixel/edge regressions a renderer
rewrite produces. 400x240x3 + 54 = **288,054 B** per BMP, matching the byte counts already quoted
in the progress docs. A `[debug] fh_scale=320` may downscale for eyeballing; hashes are always at
native extent.

**Right eye:** `[debug] fh_eye=L|R|both`. `Gdx3dsStereo::RightTarget()` (`gdx3ds_stereo.h:94`)
gives the second target; emitted as a separate `eye=R` receipt/file. Default `L`.

**Encode.** Reuse the untile+un-rotate body of `ReadFramebufferToCPU` factored into
`extern "C" int gdx3ds_gfx_capture_target(int eye, uint16_t* out, uint32_t w, uint32_t h)` —
same sentinel poll, minus the `mFrameActive` FrameSplit branch (no frame is open here). Output is
RGBA5551, row 0 = top.

**Hash.** CRC-32 (table-free bitwise; no crc32/xxhash exists in `port/` today) over the packed
192,000-byte RGBA5551 buffer. Receipt:

```
[fh] frame=1234 tick=1234 eye=L w=400 h=240 hash=8f2a41c9 tag=race00
```

`frame` is the host-loop counter (`main_3ds.cpp` `frameNum`), which advances exactly one VI tick
per iteration (`:1078`) — the same clock `.gdxrec` records against.

**Triggers.** (a) `SHOT <label>` from autoinput/`.gdxrec` → image + hash; (b) `[debug] fh_every=N`
→ hash only, every Nth frame, `tag=auto`; (c) `gdx_oracle_capture_request(const char* tag, int
flags)` — the entry point the hwlink track's net `CAPTURE` calls.

**Idle cost is zero.** The hook's first statement is
`if (sPendingTag == nullptr && sEvery == 0) return;` over two file-statics latched once at init.
No config read, no allocation, and critically **no `C3D_FrameSync`** unless armed — an
unconditional sync would serialize the pipeline and skew every perf number on the fork.

**Files.** `autotest/<tag>_L.bmp` (sdmc root, `mkdir` as today). Receipts to the existing log
sinks (`svcOutputDebugString` + filelog).

### 2. Input recorder / replay (`.gdxrec`)

One injector serves autoinput, `.gdxrec`, and (later) hwlink UDP. Extend `port/input_bridge.c`:
export the tick (`gdx_input_tick()`), and route both scripted sources through one
`gdx_input_inject(tick, &pad)` applied at `gdx_controller_poll`'s existing substitution point
(`:707-724`), before the digital-stick derivation — the property that makes scripted input flow
through the identical path as physical input.

**Format** — ASCII, one line per frame, line K after the header == tick K. No RLE (trivial UDP
conversion), no event cap, **streamed** with `fgets` one line per poll (O(1) memory; an 18,000-
frame race is ~400 kB on SD).

```
gdxrec 1
build 447ee52
seed1 0x1234abcd
seed2 0x0fedcba9
pins renderthread=1 rival_detail_auto=0 stereo=0 fh_every=0
# tick btn stickx sticky [touch tx ty] [SHOT label]
0 0000 0 0
1 0000 0 0
...
4600 8000 -34 12 SHOT race00
```

`btn` = 4 hex digits, the N64 mask already used by `input_bridge.c`. `stickx/y` decimal -80..80.
`touch` optional (3DS-only; the bottom-screen menu is the only consumer). **SHOT is a trailing
token on the frame's own line** — that structurally kills the RESUME.md:150 "trailing SHOT block
dropped" gotcha.

**Record:** `[debug] input_record=1` → `rec/<name>.gdxrec` (name from `[debug] input_record_name`,
default `rec0`), written from the same poll, after the physical read. **Replay:**
`[debug] input_replay=rec0` (wins over autoinput; logs `[rec] replay rec0 frames=N`, and
`[rec] end tick=N` at EOF, after which physical input returns).

**Hardware.** Prefer SD-side `.gdxrec` replay on hardware too — it is tick-exact, unlike UDP.
`tools/dev/gdxrec2ir.py` converts a `.gdxrec` to Rosalina InputRedirection 20-byte packets
(buttons inverted mask / touch / circle pad / C-stick, UDP 4950) for the hwlink track's
interactive lane; note in that tool's header that UDP replay is **not** tick-exact and is not a
parity oracle.

### 3. Determinism

Sources found (all `file:line`):

| # | Source | Effect | Pin |
|---|---|---|---|
| 1 | `decomp/src/sys/sys_gfx.c:712,716` `Math_Rand1Init(osGetTime(), …)`; `:782,786` `Math_Rand2Init` | Rival AI, effects, transition tile order — `Math_Rand1/2` used ~200x in `ovl_i3/cpu.c` (e.g. `:469,842,1327`), `ovl_i2/effects.c`, `ovl_i2/transition.c:312` | Env pin **exists** (`:692-715`, `:764-787`) but uses `getenv` — **dead on 3DS** (RESUME.md:143-147). New decomp patch: read the seed via a weak `extern "C"` accessor backed by `[debug] rand_seed1/2` |
| 2 | `decomp/src/sys/sys_gfx.c:753-760` boot-logo hold: `while (osGetTime()-t < 230 frames) gdx_yield();` | `gdx_yield` (`port/n64_sched.c:824-836`) returns to the host loop, which pumps **one VI tick per yield**. The tick count consumed by boot is wall-clock dependent, so every downstream tick-scheduled event lands at a different game frame. **This is why menus match byte-for-byte and races differ by "timer digits + pack positions"**: a shifted timeline is invisible on a static screen | Same patch: when the pin is on, count *yields* (frames), not milliseconds |
| 3 | `port/3ds/gdx3ds_dynlod.c:109,180` LOD tier from `svcGetSystemTick()` deltas | Changes rival geometry — `dynlod-progress.md:71-74` measured nD=125 (tier 0) vs 97 (tier 2) on the same frames | `[perf] rival_detail_auto=0` (exists) — required in every parity run's `pins` |
| 4 | `decomp/src/audio/disk/lib/thread.c:214` `audioRandom *= osGetCount()`; `:992`; ROM path `decomp/src/audio/rom/lib/thread.c:139` | Sequencer variance; audio-only, but unpinned | Pin at `:195-213` exists **only** under `gdx_pcm_capture_active()`; widen the condition to "capture armed OR determinism pin on" |
| 5 | `port/3ds/lus_glue/gdx3ds_libultra.cpp:59,65` `osGetTime`/`osGetCount` = `steady_clock` | Every decomp consumer of those is wall-clock on 3DS | — (root of 1, 2, 4) |
| 6 | `gfx_citro3d.cpp:1146` `Gdx3dsStereo::FrameBegin` polls the 3D slider | Analog physical input; changes the right-eye image and whether the second pass runs | `[debug] stereo=0` for parity runs, or record the slider in `.gdxrec` |
| 7 | `decomp/src/game/game.c:702-704` `osGetTime` spin; `sys_leo_dd.c:348-350`; `sys_fault.c:133` | Burn wall time, no `gdx_yield` → **no tick drift**. EK / DD / fault paths | none needed; documented as cleared |

**Cleared (checked, deterministic):** `decomp/src/sys/dma.c:206` yields every 32 kB — a
size-derived, not time-derived, tick count. `port/n64_vi.c:33,38` yields inside game busy-waits
that are tick-driven. `main_3ds.cpp:1078` runs exactly one `gdx_vi_tick` per host iteration, so a
dropped host frame never drops a game tick — that is what makes tick-scheduled input sound at all.
Bottom-screen overlays (`gdx3ds_fps_hud.c:90`, `gdx3ds_menu.c:820`) are `consoleInit(GFX_BOTTOM)`
only and cannot touch the top-screen hash.

**Decomp patch discipline.** One new file
`port/3ds/patches/decomp-port-determinism-pins.patch` (items 1, 2, 4), a pure delta, plus its
apply line in `port/3ds/patches/README.md` and in `tools/ci-3ds.sh`'s `decomp:` list
(`:162-171`), with a `## decomp-port-determinism-pins.patch` section in the README explaining it.
Off by default: with no `[debug] rand_seed1` key the original `osGetTime()` expressions run byte
for byte.

**Parity protocol.** A parity claim states: build SHA, `.gdxrec` name + SHA, `pins` line, and
platform. Then `same .gdxrec + same seed + same build + same platform → identical [fh] sequence`.
Tolerance is **zero** for menus today, and zero in-race is the gate after the pins land. If a
residual in-race delta survives the pins, fall back to masks — never to "looks close":

```
# race.mask — regions excluded from the exact compare
mask 400 240
rect 296 8 104 24 timer
rect 0 200 96 40 speed
```

`rect x y w h name`, `#` comments, coordinates in the 400x240 landscape frame, top-left origin.
`framediff.py --mask race.mask`. Masked regions are reported in the contact sheet so a masked
failure is never silently green.

### 4. Host tools

- `tools/dev/framediff.py a.bmp b.bmp [--mask f] [--perceptual T] [--sheet out.png]` — exit 0/1;
  prints differing-byte count, differing-pixel count, bbox of the diff, and per-mask breakdown;
  writes a 3-up contact sheet (A | B | amplified diff) only on failure. Stdlib + Pillow (already a
  CI dependency, commit a63d042).
- `tools/dev/fhdiff.py a.log b.log` — parses `[fh]` lines, reports first divergent frame, total
  divergent frames, and the tags around it. This is the cheap gate; images only on mismatch.

### 5. Emulator vs hardware

Azahar and PICA200 hardware will **not** produce identical hashes — different rasterizer, different
texture filtering rounding, different transfer paths. Parity is **always same-platform**:
Azahar-vs-Azahar, hardware-vs-hardware. Cross-platform comparison is a human eyeball on the
contact sheet, never a hash. Every receipt therefore carries the platform in the run's report
header, and `fhdiff.py` refuses (warns) when the two logs disagree on it.

## Work breakdown

| Step | Deliverable | Verification |
|---|---|---|
| O1 | `gdx3ds_gfx_capture_target()` factored out of `ReadFramebufferToCPU`; no behaviour change | Build; existing transition capture path still produces the same `transcap_*.bmp` |
| O2 | `gdx3ds_oracle_after_endframe()` + `[fh]` receipt + BMP writer; SHOT rerouted to it | Azahar: menu SHOT BMP is non-black and matches the LCD; two runs of the same autoinput give identical `[fh]` on menu ticks |
| O3 | `fh_every=N` hash-only path; idle-cost proof | `[prof]`/`[c3d]` windows with `fh_every=0` unchanged vs. pre-O2 build (same run script, gputrace off) |
| O4 | `[debug] input_record=1` → `.gdxrec` | Record a 600-frame menu session; file has 600 data lines; tick column strictly ascending |
| O5 | `.gdxrec` replay through the unified injector | Replay the O4 file; `[fh]` sequence identical to the recorded session's |
| O6 | `decomp-port-determinism-pins.patch` (+README, +ci-3ds.sh) | Patch applies idempotently; with no ini keys the build's `[fh]` sequence is unchanged |
| O7 | Twin-run in-race gate | Same `.gdxrec` + `rand_seed1/2` + `rival_detail_auto=0`, two Azahar runs: `fhdiff.py` reports 0 divergent frames through a full lap. If not, bisect with `fh_every=1` to the first divergent frame and add the source to the table |
| O8 | `framediff.py` + `fhdiff.py` + mask format | Unit-check on synthetic BMPs (identical / 1-pixel diff / diff inside a mask) |
| O9 | `gdxrec2ir.py` + docs | Packet bytes checked against the InputRedirection layout; hardware lane owned by the hwlink track |

## Risks

- **`C3D_FrameSync()` in the capture path stalls the pipeline** for the captured frame. Accepted:
  it only runs when armed, and a capture frame's timing is not a perf sample. Must be stated in
  the receipt so nobody profiles a capture window.
- **Residual in-race nondeterminism after the pins.** Mitigation is O7's `fh_every=1` bisect; the
  fallback is masks, which is a weaker but still mechanical gate.
- **Reading `target->frameBuf.colorBuf` after `FrameEnd`** assumes citro3d does not swap the
  target's colour buffer (it does not — the target is a fixed off-screen RGBA8 allocation,
  `gfx_citro3d.cpp:952`). Verify once on hardware, not just Azahar, since transfer timing differs.
- **SD write cost** — 288 kB per BMP on a shared virtual SD. Hash-only is the default; images only
  on SHOT or on demand.
- **Decomp patch churn**: the pin patch touches `sys_gfx.c`, which `decomp-ilp32.patch` also
  touches. Order it last in the `decomp:` list and check the stale-object trap (PROTOCOL.md).

## Estimate

O1-O3 ~1.5 days, O4-O5 ~1 day, O6-O7 ~1.5 days (O7 is emulator-run-bound, ~40 min per race run),
O8-O9 ~0.5 day. **~4.5 days**, plus one hardware confirmation pass for the capture point.

## First 10 commands

```sh
SCRATCH=<scratch>
$SCRATCH/native/bootstrap.sh oracle native/oracle
cd ~/code/gdx-3ds/oracle && tools/ci-3ds.sh
sed -n '3297,3420p' port/3ds/gfx/gfx_citro3d.cpp          # the readback body to factor out
sed -n '1186,1200p' port/3ds/gfx/gfx_citro3d.cpp          # EndFrame / C3D_FrameEnd
sed -n '1005,1015p;1100,1115p' port/3ds/main_3ds.cpp      # both EndFrame call sites
sed -n '275,390p' port/input_bridge.c                     # autoinput injector to unify
grep -n "Math_Rand1Init\|Math_Rand2Init\|gdx_yield" decomp/src/sys/sys_gfx.c
DEVKITPRO=/opt/devkitpro cmake -S . -B build-3ds -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds --target G-Diffuser-3DS_3dsx -j8
mkdir /tmp/azahar.lock && open -a /Applications/Azahar.app --args "$PWD/build-3ds/port/3ds/G-Diffuser-3DS.3dsx"
```
