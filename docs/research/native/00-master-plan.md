# Native renderer fork: plan, dev-loop upgrade, LUS deprecation scope

Date: 2026-09-07. Basis: four research passes over mainline `feat/3ds-hwaudio`
(LUS surface, decomp emission census, dev-loop inventory, external hardware-loop
research). Numbers marked (HW) are from the round-4/5 hardware profiles; (est) are
estimates.

## 0. One-paragraph verdict

The 3DS build compiles exactly 36 LUS files (15,043 lines): the Fast3D interpreter
(10,466), the o2r resource system (~3,000), and utilities. Everything else LUS
(window, audio, input, config, gui) is already replaced by `port/3ds/lus_glue`.
"Deprecating LUS for graphics" therefore means replacing one file, `interpreter.cpp`,
plus the 12,353-line bridge that exists only to feed it. That is a bounded job with a
proven reference (sm64_3ds `gfx_pc.c`, ~2.5k lines). It buys roughly a third of the
render thread. The bigger prize, a native race path (retained machine meshes, track
tessellated straight into a VBO, sprite batches for effects/HUD), is the only route to
Old 3DS and is design work, not translation. Neither should start before the dev loop
is fixed: today there is no trustworthy frame oracle, no network path to the console,
no desktop reference on this Mac, and every hardware iteration is a human SD round trip.
Fix the loop first (about 2 weeks), then fork.

## 1. What LUS graphics actually is on 3DS (inventory)

| Piece | Lines | Role | Fate in the fork |
|---|---|---|---|
| `libultraship/src/fast/interpreter.cpp` | 10,466 | F3DEX2 + RDP + OTR opcode interpreter, RSP T&L on CPU, TMEM/texcache, combiner keys, flush to backend | Replaced (Tier 1) |
| `port/n64_gfx_bridge.cpp` | 12,353 | Widens the game's 32-bit Gfx into LUS's 64-bit `F3DGfx`, resolves segments/assets, memos, diagnostics | Deleted with the interpreter; a native interpreter reads the game's Gfx directly |
| `port/3ds/gfx/gfx_citro3d.cpp` | 3,492 | Our citro3d backend: combiner to TexEnv, texture upload, packed VBO, stereo, viewport fold | Kept; becomes a library the native paths call |
| Fast3D factories/types | 1,435 | Parse torch's o2r formats (DisplayList v0 with OTR opcodes, Vertex, Texture, Matrix, Light) | Kept or replaced by a 300-line reader |
| Ship resource/archive/binarytools/utils | 3,088 | Mount o2r via miniz, load by path/hash | Kept (or replaced by direct miniz reads) |
| 48 patches | 38 lus + 10 decomp | 36 touch `interpreter.cpp` | Vanish when the file is vendored |

The interpreter handles 21 F3DEX2 SP opcodes, 30 RDP opcodes, and ~10 OTR opcodes that
F-Zero X's o2r actually uses (`G_SETTIMG_OTR_FILEPATH`, `G_VTX_OTR_FILEPATH`,
`G_DL_OTR_FILEPATH`, `G_MTX_OTR`, `G_PUSHCD`, `G_MARKER`, and the wide variants). The
backend contract is 41 virtuals in `gfx_rendering_api.h`; a native renderer bypasses
it rather than satisfying it.

## 2. How the game draws (census, decomp/src)

- Zero static `Gfx[]` arrays anywhere. Every command is an immediate-mode emit into
  `GfxPool.gfxBuffer[8193]`. Static/dynamic is a property of the operands, not the
  commands. This is why the bridge output cache was a no-go (87% host-built).
- Machines: `ovl_i9/machine_draw.c`, 392 `Machine_Draw*Lod*` functions (5 LOD tiers),
  11,530 macro sites (71% of the tree), zero `gSPMatrix`. Vertices are ROM segment
  symbols (static). Livery is 12 color args that land in `gDPSetPrimColor` /
  `gDPSetEnvColor`. Five 2-cycle combiner modes live only here; all already mapped.
- Track: procedural. `course.c` tessellates the spline into `courseVtxBuffer[4096]`
  every frame, 8 verts per chunk, ten shape-specific chunk-group emitters, material
  switch via raw `gDPSetTextureImage` + `gDPLoadBlock` gated on `sLastTrackShapeType`.
  World-space vertices, no matrices, per-vertex shade, no RSP lighting. CPU cull:
  depth 5500 and NDC x.
- Effects: `racer.c` writes `effectsVtxBuffer[2048]`, 211 sites, 4-5 verts per sprite.
- HUD: pure texrects (24 in `hud.c`). Sky: tiny, per-frame verts.
- Matrix stack unused (6 pushes tree-wide). ~90-100 modelview loads/frame in a 30-car
  race, one per machine per slot.
- Lighting: single directional + ambient (`NUMLIGHTS_1`), machines and menus only.
- Textures: 2,678 in the yaml set, RGBA16-dominant with a large I4 population; palette
  swaps in background sprites and HUD; UV scrolling via vertex tc, not tile offsets.
- Long tail on the interpreter: `menus.c` 357 sites, course_edit ~500, EK, ending,
  records, machine creator.

Consequence: machine meshes are bakeable once (straight-line emitters over static
vertex data, colors as parameters). Track is a per-frame vertex stream that can go
straight into a PICA VBO. Effects and HUD are sprite batches. Everything else stays on
an interpreter forever; the fork is a hybrid renderer, not a total replacement.

## 3. The dev loop today, and why it blocks the fork

Facts from the inventory:

- Hardware: SD card sneakernet. No `3dslink`, no socket, no netloader anywhere in the
  tree (`soc:U` is in the RSF ACL, unused). Results come back as `log.txt`, Luma dump
  files, and the user's eyes.
- Visual oracle: `autotest/<label>_scan.bmp` reads a stale back buffer (1-2 frames
  late, not the target citro3d drew into); the `<label>.bmp` mirror is black in-game.
  Recorded verdict: "every scanout-verified conclusion about a late-frame pass is
  suspect". Never fixed.
- Determinism: tick-scheduled autoinput gives byte-identical menu/preview shots, but
  in-race shots differ 3-19 KB control-vs-control (timer digits, rival order). No
  input recorder, no frame stepping, no committed diff tool (each agent re-did it by
  hand with `cmp`).
- Desktop reference: none on macOS (link fails on ~1,190 SDK header collisions).
  README supports Windows/Linux only.
- Emulator: Azahar Qt only, one instance, lock in `/tmp`, agents have killed each
  other's runs and deleted each other's artifacts. All `/tmp` helper scripts are gone.
- Build: Makefile generator; patch dance is a full reverse+forward stack apply;
  stale-object trap documented.
- Host tests: three sub-second unit tests; the "harness" is a 36 s on-device scene
  runner with hand-written region predicates, never run by CI.

A renderer rewrite is a differential-testing problem (new path must match old path
pixel-for-pixel, then beat it on time). None of the above can adjudicate that today.

## 4. Dev-loop upgrade (do this first, ~2 weeks)

### 4.1 Hardware over WiFi (removes the SD round trip)

All of this is verified against current libctru / Luma3DS 13.4 / hbmenu source.

1. Push: `3dslink -a <3ds-ip> -s G-Diffuser-3DS.3dsx`. hbmenu's netloader (press Y
   once) receives it, writes `sdmc:/3ds/<name>.3dsx`, launches it, and injects the
   host IP as an argv entry (`__3dslink_host`).
2. Logs: in the app, `socInit()` then `link3dsStdio()` (port 17491). With `3dslink -s`
   the Mac receives stdout live. Route the filelog sink there too.
3. Zero-touch relaunch: Luma exposes the `hb:ldr` port. hbmenu's own loader is
   `svcConnectToPort("hb:ldr")`, `HBLDR_SetTarget(path)`, `HBLDR_SetArgv`, exit. Our app
   opens a TCP listener, receives the next `.3dsx`, writes it to SD, calls SetTarget on
   itself, exits. After the first Y press per power-on, the console reloads builds on
   its own. This is the single highest-leverage item.
4. Input injection: Rosalina InputRedirection, UDP 4950, 20-byte packet (12 bytes HID:
   buttons inverted mask / touch / circle pad; 4 bytes C-stick; 4 bytes special:
   HOME, POWER, POWER-long). It patches HID shared memory, so it drives homebrew.
   Enable once per boot in the Rosalina menu. A Python driver replays recorded laps.
5. Capture: app-side. After the render thread's DP-done for frame N, read back the
   citro3d target (not the scanout), encode, send on a second TCP socket to
   `__3dslink_host`. Exact pixels, no CFW streamer needed. (NTR-HR + headless
   Chokistream image-sequence output is the fallback for continuous video.)
6. Perf: log `C3D_GetDrawingTime()`, `C3D_GetProcessingTime()`, `C3D_GetCmdBufUsage()`
   alongside `[prof]` over the stdout link. There are no PICA hardware counters;
   citro3d's timers are the state of the art.
7. Crashes: Rosalina GDB stub, `target extended-remote <ip>:4000`, scripted
   `gdb -batch -x` for backtraces. Enable once per boot; disable before shutdown.

What still needs a human: power on, one Y press in hbmenu, two Rosalina toggles
(InputRedirection, debugger), driving one reference lap per course for the replay
files, and subjective calls (feel, judder, stereo comfort). No remote power-on exists.
Console stays docked on the charger with auto-sleep off.

### 4.2 A frame oracle that is actually true

- Fix capture: encode on the render thread after DP-done, from the target texture.
  Add a frame hash (`[fh] frame=N crc=...`) so parity can be checked without moving
  images at all; images only on mismatch.
- Commit `tools/dev/framediff.py`: exact compare with region masks (HUD timer, rival
  portraits), perceptual threshold, contact sheet on failure.
- Input recorder: dump tick + pad state per frame on hardware (`[rec]`), replay via the
  existing tick-scheduled autoinput on both emulator and hardware. Verify the race is
  deterministic given inputs (check RNG seeding; the audio harness already pins
  `GDX_RAND_SEED1/2`). If it is, in-race parity becomes exact instead of "3-19 KB off".

### 4.3 A ground-truth renderer on this Mac

- Linux desktop build in Docker (README supports Linux) with Mesa llvmpipe + xvfb.
  Same o2r, same inputs, same `gdx_request_frame_dump`. This is the authoritative
  cross-check `harness/EXPECTED.md` already names but nobody has. Compare at 320x240.
- Host differential harness for the native paths: compile `machine_draw.c` (+ `gbi.h`
  + a recording Gfx buffer) on the host, no GPU. Record the triangle set each
  `Machine_Draw*Lod*` emits, compare against the baked mesh. Sub-second, deterministic,
  runs per commit. The three existing host tests prove partial host compiles work; this
  avoids the 1,190-error full desktop link.

### 4.4 Emulator lane without the singleton

- RetroArch + Azahar libretro core, driven over UDP 55355 (`FRAMEADVANCE`,
  `SCREENSHOT`, `PAUSE_TOGGLE`, `RESET`); multiple instances with separate configs.
  Azahar `.ctm` movie record/replay is the deterministic-input primitive if Qt is kept.
  Emulator numbers stay relative-only; hardware is truth.

### 4.5 Build hygiene

- Vendor the 36-file LUS carve into `port/3ds/vendor/` with all patches applied
  (see Phase 0). The libultraship submodule leaves the 3DS build; 38 patches become
  code; the stale-object trap and the reverse+forward dance go away.
- Ninja + ccache. Record full and incremental build times in CI output.
- One CLI, `tools/dev/gdx-dev`: `push`, `run <replay> [--capture]`, `diff a b`,
  `prof <log>`, `gdb`. Agents stop rewriting `/tmp` scripts that evaporate.

## 5. Deprecating LUS for graphics: two tiers

### Tier 1: own interpreter (bounded, mechanical)

Replace `interpreter.cpp` + bridge with a purpose-built F3DEX2-to-citro3d interpreter
in `port/3ds/gfx/`, modeled on sm64_3ds `gfx_pc.c`, reusing our backend, packed VBO,
combiner mapper, texcache, atlas.

Scope:
- Reads the game's 8-byte Gfx directly (ILP32 pointers already fit; the widening
  pre-pass and the whole address-space heuristic class disappear).
- 21 SP + ~25 RDP + ~10 OTR opcodes. Flat switch, no handler tables, no shared_ptr or
  weak_ptr per command, no per-command asset lookups (resolve OTR paths once per list
  and memo by pointer).
- RSP T&L on CPU: transform, one directional light, fog factor, texcoord gen. Drop CPU
  clipping; PICA clips in hardware (near-plane behaviour to verify on the tunnel).
- TMEM: keep our span store / same-content skip / value gates as designed, in ~600 lines
  instead of patches.
- Keep the o2r archive + factories for asset loading (or a 300-line direct reader).

Size (est): 4-6k new lines, 3-5 weeks. Verification: frame-hash parity against the
vendored LUS path on the recorded replays, on emulator and hardware.

Gain (HW, render thread, crowd frame, ms): today dsp 4.9, tri 4.2, br 3.1 (now brMain
on core 0), drw 1.2, vtx 1.0, imp 0.7, about 15 total. Tier 1 (est): br 0, dsp 2-3,
tri ~3, rest same, about 8-9. Not enough for Old 3DS alone. Maintenance gain is real:
36 patches on a 10k-line upstream file become owned code.

### Tier 2: native race path (the actual remake)

Hybrid: native fast paths for what the race draws; Tier 1 interpreter for the long tail
(menus, course editor, EK screens, ending, records).

1. Machines. Bake each `Machine_Draw*Lod*` once at load: run it through a recording
   interpreter with sentinel livery colors, collect triangles per material run, detect
   which prim/env slots the 12 livery args reach, emit a retained mesh (linearAlloc VBO,
   PICA layout) with material slots. Runtime replaces the function body with
   `Gdx_DrawMachine(character, lod, mtx, livery)`: a handful of `C3D_DrawArrays` with
   per-material TexEnv constants. Lighting: one directional light, either CPU per
   vertex (30 cars x ~200 verts, cheap) or PICA fixed-function lighting. Textures
   pre-uploaded per machine set. Stereo: second draw with the other projection, no
   re-walk. Risk: emitters that branch on runtime state (damage, boost flame, shadow
   variant); classify the 392 functions by script, bake per variant.
2. Track. Keep the tessellator (game logic, deterministic), point `gCourseVtxPtr` at a
   PICA-layout ring VBO, transform on CPU into clip space in one tight loop, one draw
   per material run instead of per 8-vert chunk. GPU transform is a stretch goal only
   (the shelved gputransform branch documents the 24-bit precision failure on
   ±32000-unit coordinates).
3. Effects: `gEffectsVtxPtr` sprites become a sprite batch (one draw per texture).
4. HUD: texrects to a 2D batcher over the existing atlas.
5. Sky, shadows, boost tint, transitions (the one framebuffer readback): case by case.
6. Viewport fold, border modes, stereo anchor, dynlod: carried over as-is.

Verification: host differential harness for baked meshes (exact triangle-set match),
frame-hash parity on the replays for the integrated path with masks where the native
path is intentionally different (none expected), hardware profile per milestone.

Size (est): machines 2-3 weeks, track 2, effects+HUD 1-2, integration/stereo/menu
viewports 1-2: 6-9 weeks agent time, $10-15k at the observed $330/active day. Gain
(est): render thread 4-6 ms in crowds. That is the Old 3DS-class budget.

### Old 3DS, stated plainly

Tier 2 is necessary, not sufficient. Old 3DS also needs: render thread folded back
into the main loop (no spare core), 268 MHz (4x less CPU than New 3DS at 804 MHz
plus L2), 64 MB app region (memory diet: o2r-resident audio, texture set per race,
no 42 MB ELF). Plan it as a separate milestone after Tier 2 parity on New 3DS.

## 6. Phases and gates

| Phase | Weeks | Deliverable | Gate |
|---|---|---|---|
| 0 Vendor | 0.5 | LUS carve vendored, patches folded, submodule out of the 3DS build, Ninja/ccache | Bit-identical 3dsx behaviour, frame-hash parity on title/select/preview |
| 1 Loop | 2 | 3dslink push + self-reload, stdout link, input redirection driver, true capture + frame hash, framediff, recorder/replay, gdx-dev CLI, Docker Linux reference build | One command runs a recorded lap on hardware and returns log + hashes + diffs with no human touch |
| 2 Tier 1 | 3-5 | Own interpreter, bridge deleted | Parity on all replays (emu + HW), render thread ≤ 9 ms crowd (HW), 0 errors over a 30-min soak |
| 3 Machines | 2-3 | Bake pipeline + host harness + native machine draw | Triangle-set match for all 392 emitters, parity on replays, HW profile |
| 4 Track | 2 | Direct-VBO tessellation | Parity, HW profile |
| 5 Effects/HUD/rest | 1-2 | Sprite batches, HUD batcher, leftovers | Parity, HW profile, locked 60 with stereo and FULL rival detail |
| 6 Old 3DS | tbd | Main-loop render, memory diet | Boots and races on Old 3DS |

Gates are hard: no phase merges on "looks fine in emu". Each phase lands behind an ini
killswitch like every lever before it.

## 7. Fork mechanics

- New worktree `~/code/gdx-3ds/native`, branch `native/main` off mainline. Phase 0 is
  the first commit. Patches README is retired for LUS; decomp patches stay (10).
- Public repo: `native` branch, README section "Native renderer (in progress)";
  releases keep shipping from `main` until Phase 5 parity, then v1.0 from native.
- Research briefs per phase in `docs/research/native/`, progress files as before.
- Agent protocol unchanged plus: every claim of parity cites a frame-hash line and a
  replay id; every perf claim cites a hardware `[prof]` window with `gputrace` state.

## 8. Risks

- `hb:ldr` self-relaunch from a running 3dsx is untested (hbmenu does exactly this, so
  it should work; verify on day one).
- WiFi drops, console sleep, HOME during a run: add a watchdog that re-arms netloader
  and a run-level retry.
- Race determinism under replay may need RNG pinning in the decomp (small patch).
- Machine emitters with runtime branches (bake per variant).
- Near-plane clipping without CPU clip (tunnel roof is the known sensitive spot).
- Track precision if GPU transform is ever attempted (documented failure).
- Scope creep into the long tail: the interpreter stays for menus by design.
