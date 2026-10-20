# The native renderer

What the native renderer replaces, how it is switched on, how each piece is checked, and what has
actually been measured on hardware. This is the renderer the v1.0.0 release ships with — every
individual lever below still defaults to `0` (the *old*, libultraship-interpreter-parity path) at
its own call site, but `[perf] preset` itself now defaults to `auto`, and `auto` resolves to a
built-in table that turns the native renderer and NRR on for your console model (§4). **A plain
`gdiffuser.ini` with no `[perf]`/`[native]` section at all still boots into the native renderer on
both consoles** — you do not need to write any ini lines for performance. The result numbers and
the honest list of what has *not* been run are in [`RELEASE-NOTES.md`](RELEASE-NOTES.md). For
where this fits in the rest of the port, see [`ARCHITECTURE.md`](ARCHITECTURE.md) §4.3.

---

## 1. What it replaces

Everything the old path draws goes through libultraship's Fast3D interpreter — vendored into this
tree as a frozen carve (`port/3ds/vendor/lus/`, see `ARCHITECTURE.md` §4.2) rather than linked from
the desktop submodule — fed by a bridge (`port/n64_gfx_bridge.cpp`) whose only job is to widen the
game's 32-bit `Gfx` words into the 64-bit words that interpreter wants. Every triangle in
F-Zero X is transformed and lit on the ARM11 CPU, one display-list command at a time.

The native renderer replaces that, piece by piece, with code that writes PICA200 vertex buffers
directly — and keeps the old path alive beside it. It is a **hybrid** renderer by design, not a
rewrite: the menus, the course editor, the ending and the long tail of one-off screens stay on an
interpreter.

Two tiers, which combine:

- **Tier 1 — own the interpreter** (`interp`). A purpose-built F3DEX2→citro3d walker
  (`port/3ds/gfx/n64/`) replaces libultraship's interpreter *and* the widening bridge. Same
  pixels, no conversion pre-pass.
- **Tier 2 — bypass the interpreter for the three things a race spends its frame on.** `track`
  replays the course tessellator's vertex stream straight into a VBO; `machines` draws the 30
  racer meshes from baked, retained lists (150 lists = 30 slots × 5 LODs), one indexed draw per
  material run; `fx_sink` / `fx_sprites` / `fx_hud` / `ownfold` batch the effect sprites and the
  HUD.

### How the tiers combine

Tier 2 works by making the game emit a **marker** where its geometry commands used to be
(`track`, `fx_sink`), or by redirecting a registered display list at `G_DL` (`machines`). The
replays behind those hooks are written against one interpreter seam
(`port/3ds/gfx/gdx3ds_interp_seam.h`) with an adapter per interpreter
(`gdx3ds_seam_lus.cpp`, `n64/gdx_seam_owned.cpp`), so the owned interpreter expands the same markers
and draws the same meshes through the same code the libultraship interpreter does. `all=1` does
**not** imply `interp=1`; the two are set independently and every combination is valid.

---

## 2. The `[native]` ini section

All keys live in `sdmc:/3ds/gdiffuser/gdiffuser.ini`, section `[native]`, and are **read once at
boot** — changing one needs a restart. Section and key names are case-insensitive. Every
*feature* lever defaults to `0` at its own call site, which is the exact prior path — but the
shipped `[perf] preset = auto` default (§4) sets most of them to `1` for you, so the game you
actually boot already has the native renderer on. These per-key defaults are what you fall back
to by setting an individual key explicitly, or by setting `preset = off`.

To try the whole thing by hand instead of relying on the shipped preset:

```ini
[native]
all = 1
interp = 1
```

### 2.1 Feature levers

| key | default | what it does |
|---|---|---|
| `all` | `0` | Master for the **Tier-2 levers only** (`machines`, `track`, `fx_sink`, `fx_sprites`, `fx_hud`, `ownfold`). `1` sets every one of those that is unset (`0` or absent) to `1`. A lever already explicitly set to `2` (verify) is left alone. Never touches `interp`. |
| `interp` | `0` | `1` = the owned F3DEX2→citro3d interpreter draws the frame instead of libultraship (LUS) Fast3D + the bridge. Always explicit — `all` does not imply it. Carries the Tier-2 hooks, so it combines with every lever below. |
| `machines` | `0` | `1` = the 30 racer meshes draw from baked retained lists, one indexed draw per material run. |
| `track` | `0` | `1` = the course tessellator's vertex loads and index templates are recorded and replayed into a native batch, one draw per material run. `2` = verify. |
| `fx_sink` | `0` | `1` = the six effect sprite ring classes (debris, sparks, smoke, explosions, force field, fall explosions) are recorded at the game-side macro and replayed as native per-class batches. `2` = verify. |
| `fx_sprites` | `0` | With `fx_sink`: fold prim/env colour changes into the vertex colour where the backend carries that input per vertex, so a colour change stops splitting the draw. |
| `fx_hud` | `0` | The same fold rule inside **libultraship's** 2D batches — HUD digits, stars, markers stop splitting on every prim colour. Inert under `interp=1`; `ownfold` is its counterpart there. |
| `ownfold` | `0` | The same prim/env fold inside the **owned** interpreter's own batches. Tier-2, so `all=1` turns it on; an explicit `ownfold = 0` still wins. |

### 2.2 Interpreter semantics (defaults are libultraship parity)

| key | default | what it does |
|---|---|---|
| `cpuclip` | `1` | The owned interpreter clips triangles on the CPU the way LUS does. `0` = let the PICA clip: fewer draws, **different pixels**, and it diverges on tunnel interiors. A bisect lever, not a speed setting. |
| `blendflush` | `1` | Flush the batch before a blend-alpha change, so alpha compare uses the register in force at the primitive (LUS parity). |
| `palhash` | `1` | Fold the live TLUT bytes into the CI texture-cache key, so a palette rewritten in place does not serve a stale decode. |
| `texhash_ci` | `1` | Content-key the CI4 / CI8 / RGBA32 classes (decoded source span plus the live TLUT) rather than keying on the palette's DRAM address alone. |
| `texinval` | `1` | Honour the game's queued texture-cache invalidations (the arena-rewind clear, the per-race minimap delete) on the owned interpreter. |
| `texstalefix` | `1` | An import whose decode bailed is no longer marked uploaded, so a later lookup cannot become a cache hit over a recycled id's old pixels. |
| `vtxclamp` | `1` | An over-range `G_VTX` loads its in-range prefix instead of dropping the whole command. |
| `rawcopywhole` | `1` | The bridge's persistent texture copy tracks what the last refresh actually filled, and is not clamped to the registered extent of the buffer the load starts in. |
| `timgmemo` | `1` | Memoise `G_SETTIMG` operand resolution. Not Tier-2: `all` has no say over it. |
| `dlfacts` | `1` | Memoise display-list facts in the bridge. Not Tier-2. |
| `machines_defer_free` | `1` | Retire retained mesh buffers on a deferred queue rather than freeing them under the draw. |

Each of these defaults **is** the fix. They are killswitches kept so a regression can be bisected
from an ini file on the console, not tuning knobs.

### 2.3 Verify and bisect

| key | default | what it does |
|---|---|---|
| `interp_verify` | `0` | `1` = run **both** interpreters over every job and compare the packed batches field by field (`[nverify]`). Slow; a correctness tool, not a play mode. Forces both 2D folds off so it compares like with like. |
| `machines_verify` | `0` | `1` = per draw, the walk pass is drawn and captured, the native pass is captured with GPU submit suppressed, and the triangle streams are compared bit-exact. Its counters print unconditionally. |
| `machines_walk_only` | `0` | `1` = replay the baked mesh back through the interpreter's own vertex/triangle path. A stepping stone, kept as a bisect. |
| `machines_mask` | `ffffffff` | Hex per-slot mask (bit *i* = machine slot *i*, 30 slots) — bisect which machine breaks. |
| `machines_lod_mask` | `1f` | Hex per-LOD mask (bit *l* = LOD *l*+1). |
| `tmemskip`, `tmemcls` | `0` | TMEM emulation bisect levels. |

`track = 2` and `fx_sink = 2` are the verify modes for those two levers: the stock path is drawn
and the native replay is hash-compared against it in-process.

> **Set the matching census key with a verify mode.** `track=2` reports no coverage without
> `trkcensus=1`, and `fx_sink=2` reports none without `fxcensus=1`. In both cases the MISMATCH
> line is unconditional but the "how much was checked" totals are not — so a verify run with the
> census off looks like a clean pass and proves nothing. `machines_verify` is the exception.

### 2.4 Censuses (diagnostics; no effect on what is drawn)

| key | default | what it does |
|---|---|---|
| `trkcensus` | `0` | `[trk]` per-frame course census. |
| `fxcensus` | `0` | `[fx]` per-class sprite/triangle/draw census. |
| `ncensus` | `0` | `[ncensus]` batch-split attribution on the owned interpreter. |
| `dspcensus` | `0` | Per-opcode dispatch census on the owned interpreter. |
| `fxceiling` | `0` | Measurement lever: report the draw count an ideal effect batching would reach. |

### 2.5 Boot receipt

Every resolved value is named once in the log and on the DBG tab's `NATIVE` / `NATIVE FX` rows:

```
[native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx=<bitmask> census trk= fx=
         (resolved: all=tier2 only, interp explicit; ini latched at boot)
[native] interp= path=lus|owned cpuclip= blendflush= verify= hooks=
```

If a lever you set is missing from that line, the ini was not read — check the path and the
section name.

Related `[debug]` keys the development harness uses and a player should leave alone: `det`, `fh`,
`fh_every`, `fh_start`, `input_replay`, `input_record`, `replay_quit`, `gputrace`, `renderthread`,
`cov`, `shots`. `[net] enabled` (the development link to a host) defaults off for a normal SD/CIA
launch and only defaults on when the console was launched through 3dslink's netloader — see
`docs/HW-DEV-LOOP.md`. Set it explicitly if you want to be sure either way.

### 2.6 Memory, disk and Expansion Kit levers

These are `[debug]`, not `[native]` — they are not renderer levers, but they are the keys the
Old 3DS result and the Expansion Kit depend on, and they are documented nowhere else.

| key | default | what it does |
|---|---|---|
| `audio_stream` | `0` | `1` = stream the 10.25 MB audio sample bank out of the `.o2r` through a block cache instead of holding it resident, saving that heap and shortening boot preload. |
| `audio_stream_cache_kb` | `1024` | Size of that cache; raised from an earlier `512` after testing showed the smaller size re-fetching too much. |
| `audio_stream_block_kb` | `16` | Block size for it. |
| `audio_stream_verify` | `0` | `1` = serve every streamed read *both* ways and compare. A correctness tool; it is a byte comparison, not a listening test. |
| `tex_upload_kb` | `0` | `0` = the historical 4 MiB texture staging reservation. Any other value caps it, in KiB; `512` sits above the highest staging footprint seen across the whole replay corpus. The decoders clamp and **log** if a value is set too small, and a CLAMP line means exactly that. |
| `disk_stream` | `2` on 3DS | 64DD image handling for an Expansion Kit build. 3DS defaults to streaming (`2` = stream off the SD card with no resident buffer); `0` (resident image) does not fit either console's memory and has to be set explicitly to use it. There is no useful `1` on this platform. The first mount of a given image pays a whole-image CRC pass (identity check) and caches the result in a `<name>.ndd.dcrc` sidecar next to the save, so later boots sample 256 KiB instead of re-reading the whole disk; the `[diskstream]` receipt names exactly why a boot did or didn't skip it. |
| `disk_stream_cache_kb` | `512` | Block cache for the streamed disk; testing against real hardware found no benefit from a larger cache. |
| `disk_stream_block_kb` | `16` | Block size for it. |
| `disk_stream_report` | `512` | LEO reads between periodic `[diskstream]` receipts. |
| `ek_english` | `1` | Apply the baked English text pack over the mounted disk. Inert in a build where the pack was not baked, which is every default build; skipped automatically on an already-translated image. |
| `ek_fixed_aspect` | `0` | Killswitch for the fixed-4:3 fix that makes the course editor draw at all. `1` restores the course editor's black-screen bug on 3DS. Leave it at `0`. |
| `heapattr_top`, `membudget`, `membudget_top` | `0` | Heap-attribution and memory-budget censuses. `heapattr_top` needs a build configured with `-DGDX_3DS_HEAPATTR=ON`. |

Build options that go with them, all defaulting to the shipped value:

| option | default | what it does |
|---|---|---|
| `GDX_3DS_RDRAM_MB` | `16` | Size of the emulated RDRAM arena; `8` is the Old 3DS recommendation, comfortably above measured peak usage. |
| `GDX_3DS_SIM_O3DS` | `OFF` | Simulate the Old 3DS memtype-0 commit budget so a New 3DS or the emulator reproduces the real grant. **Hazard:** pinning a budget above the host console's real one panics in `crt0` with a black screen and no log. |
| `GDX_3DS_HEAPATTR` | `OFF` | Allocator interposition and the ranked `[heap-attr]` census. A measurement build (~2.13 MB of `.bss`). |
| `GDX_3DS_EXPANSION_KIT` | `OFF` | Compile the 64DD Expansion Kit. **Not additive** — it swaps the ROM subsystems for the disk ones and changes `#ifdef EXPANSION_KIT` behaviour across dozens of translation units in both `decomp/` and `port/`, so an Expansion Kit build and the shipped build are two different binaries, not two settings. Needs your own ~64.9 MB `.ndd` at runtime. |
| `GDX_ALLOW_JP_INPUTS` | `ON` on 3DS | Accepts Japanese-region game data — specifically the retail Japanese 64DD Expansion Kit disk, the only retail EK disk that exists. On 3DS this is on by default (the desktop port defaults it off, since there it also gates a cartridge ROM path with a known audio bug that the 3DS never takes — the console never reads a cartridge ROM at all). |

---

## 3. The compact native race renderer (NRR)

The levers in §2 are the renderer's first generation. A second, more complete generation —
internally called the **NRR** (compact native race renderer) — sits alongside it and covers more
of the frame. Both generations share the same marker mechanism (a `G_NOOP` the game emits where a
draw call used to be, picked up by a hook in the owned interpreter) and the same requirement that
`interp=1` is what makes the owned interpreter walk the list at all; NRR adds *what* gets drawn at
each marker, not a new way of reaching one.

NRR renders by **category**, each gated by its own `[native] nrr_<name>=1` under a master
`[native] nrr=1`:

| Category | Lever | Covers |
|---|---|---|
| HUD | `nrr_hud` | The bottom-screen dashboard and HUD elements |
| Track | `nrr_trk` | The course surface |
| Machines | `nrr_mach` | The racer models |
| Sky | `nrr_sky` | Sky and backdrop |
| Trackside dressing | `nrr_dress` | Trackside scenery |
| Course decorations | `nrr_deco` | Course decorations; depends on `nrr_dress` |
| Shadows | `nrr_shd` | Machine/track shadows |
| Effects | `nrr_fx` | Particle and effect sprites |
| Self-test | `nrr_selftest` | Debug-only; never set in a shipped configuration |

Two things worth knowing before touching these:

- **Sequential host loop only, for now.** NRR's game-side snapshot is taken once per tick and
  consumed by the render side either synchronously (`renderthread=0`) or from the parallel render
  core (`renderthread=1`); the levers above are only validated with `renderthread=0`, which is why
  the shipped bundle in §4 also sets it explicitly.
- **A category reports 0 (drawn) or a counted fallback**, never silently does nothing — there is
  no legacy path left once a category is on, so a steady race reporting fallbacks is a real
  problem, not a quirk.

This generation is deep enough that it is not fully catalogued lever-by-lever in this document —
each category's own source file (`port/3ds/gfx/nrr/`, `port/3ds/gfx/n64/gdx_nrr_*.cpp`) is the
reference for what it specifically changes.

---

## 4. The shipped configuration: `[perf] preset`

Every lever in §2–§3 still defaults to `0` at its own call site — that part hasn't changed, and
it's what makes each one bisectable by hand. What changed for v1.0.0 is the ini's own default:
`[perf] preset` defaults to `auto`, not to being absent, and `auto` is resolved into a built-in
lever table the moment the console model is known (`APT_CheckNew3DS`, before any lever is read).
So **a stock `gdiffuser.ini` with no `[perf]`/`[native]` section now boots straight into the
native renderer and NRR** — there's nothing to add for performance on either console. Explicit
keys you *do* add still override the resolved preset's value for that key only.

```ini
[perf]
preset = auto    ; the default — you don't need to write this line yourself
```

- **New 3DS** → `auto` resolves to the exact table an explicit `preset = n60` would (below).
- **Old 3DS** → `auto` resolves to an internal table (`main_3ds.cpp` calls it `o3ds-auto`; it's
  not a name you type yourself) that is the *same* lever set as `n60`, minus the New-3DS-sized
  `tex_keep_kb` texture headroom, plus three Old-3DS-only levers the hardware-verified Old 3DS
  card needed: `audio_stream=1`, `tex_upload_kb=512` (both memory-fit, §2.6), and
  `syscore_pct_safe=1` (clamps a core-1 CPU-time request to a measured-safe ceiling — 80% was
  measured starving the audio/loading pipeline). The `[perf]` LOD auto-scaling discussed at the
  end of this section runs independently on top of this and is unchanged.
- **`preset = off`** is the escape hatch back to bare stock — every lever at its own compiled-in
  default, byte-identical to the pre-native build. Use it to rule the preset in or out when
  bisecting a regression.
- An explicit, non-`auto` `[perf] preset` you set yourself (`n60`, `off`, or anything else) is
  read exactly as typed; `auto`-resolution only fills in when the key is *absent* or literally
  `auto`.

Source: `port/3ds/gdx3ds_perf_preset.c` (model detection and the LOD-only side of `auto`/`o3ds`)
and `port/3ds/gdx3ds_preset_n60.c` (`kN60Preset`, `kO3dsAutoPreset` — the lever tables themselves,
with the full key-by-key rationale in their header comments).

On a New 3DS build, `preset = n60` (what `auto` resolves to) bulk-sets roughly sixty `[native]`,
`[debug]`, `[stereo]` and `[menu]` keys to the values this release ships with — every one of them
is still an ordinary ini key, so an explicit entry for any of them in your own `gdiffuser.ini`
overrides the preset's value for that key only. Grouped by what each batch is for:

- **Host loop and frame pacing** (`debug.renderthread=0`, `frameskip=race0` and its tuning,
  `pace_swapgate`/`pace_swapgate_3d`, `dl_hollow`, `overlap_kick`, and related keys) — runs the
  sequential host loop NRR needs (§3) and paces frame submission to hit 60 Hz.
- **Texture handling** (`tex_fast`, `tex_keep`, `tex_keep_kb=4096`, `tex_keep_floor_kb`) — a larger
  resident-texture budget than the stock path uses, sized for the New 3DS's spare RAM.
- **Audio and save** (`audio_io_unlock`, `audio_ctx_narrow`, `audio_simd`, `audio_rs_align`,
  `sram_async`, `filelog_race_quiet`) — the same audio-path and off-thread-save fixes called out in
  the release notes.
- **The renderer core**: `native.all=1`, `native.interp=1`, plus every Tier-2 lever from §2 and the
  retained-path variants of each (`track_retained`, `machines_retained`, `hud_retain`,
  `sky_retain`, `shadow_batch`, `fx_batch`, and siblings).
- **NRR**: `native.nrr=1` and every category lever from §3, plus `finish_mesh`, `prewarm`, and
  `zclear_elide`.
- **Stereo**: `stereo_interleave`, `stereo_noreflush`, `stereo.enabled=1` — the 3D slider is live
  under this preset (slider at 0 still renders byte-identical mono).

**Old 3DS reaches its 60 fps race through the same `[perf] preset` key, not a separate recipe** —
see the `auto` → `o3ds-auto` resolution above. It is not one you type by hand (there's no
documented `preset = o3ds-auto` for a player to write; `auto` is what gets you there), but it is
read from the same ini key as the New 3DS table, applied the same way, with an explicit key of
your own overriding it the same way.

`[perf] preset` also has a second, older meaning, unrelated to the renderer, that runs alongside
the resolution above rather than instead of it: the automatic draw-distance/detail-reduction
system for crowded races (`rival_detail`, `rival_tier_max`, the auto-detail band, `lod6_skip`).
`auto` (the default) selects its Old-3DS-tuned table on an Old 3DS and leaves a New 3DS alone;
`o3ds` forces that table on any model. That system is about *how much* gets drawn, not *how* — it
stacks with either renderer generation, and both systems are driven by the same `[perf] preset`
value without conflicting.

---

## 5. How a lever is checked

Four independent gates, in increasing order of what they can see:

1. **Host tests.** `tools/ci-3ds.sh` builds and runs a large and growing set of host test binaries
   covering the vertex packer, the format converters, the track templates, the owned interpreter,
   the atlas UV arithmetic, the config readers, the heap configuration, the LOD controller, the
   block cache and disk streamer, the `.o2r` stream reader, the MAME ↔ `.ndd` converter (30,288
   checks plus a round-trip), texture swizzling, audio CRCs, DSP kernels/SIMD, save handling, and
   more — the script itself is the current list. It also verifies the vendored-libultraship
   manifest and applies the `decomp-*.patch` stack idempotently, failing on a patch file it does
   not know.
2. **In-process verify modes.** Each lever can draw the stock path and compare the native path's
   packed vertex bytes against it, in the same frame, bit for bit — see §2.3. This catches
   differences that never reach a pixel.
3. **Frame hashes.** `[debug] det=1` pins the game's nondeterminism, `.gdxrec` replays drive tick-
   exact input, and `[fh]` emits a per-frame hash of the presented frame. Two runs are compared
   frame for frame; screenshots are captured on a schedule derived from a per-frame feature
   census (`[debug] cov=1`), so the schedule covers everything the replay actually draws rather
   than points somebody guessed.
4. **Hardware.** A console over a WiFi link, running the same replays.

**Parity is same-platform and same-render-thread-mode only.** The render thread presents each
game frame exactly one host frame later than the sequential renderer, so `renderthread=0` and
`renderthread=1` are two different executions, not two settings, and their frame hashes do not
match without a one-frame shift. The gate runs with the thread **on**, which is the shipping
default.

### What that caught

Widening the replay corpus past the original course found two real parity bugs that every
earlier gate had missed — one in texture-cache keying (`palhash`/`texhash_ci` above are the fix)
and one in a material-binding guard. Both are fixed in this release and all corpus replays report
zero divergent frames — see the post-mortem for the story.

---

## 6. What is measured

See [`RELEASE-NOTES.md`](RELEASE-NOTES.md)'s "Measured performance" section for the renderer's
hardware numbers and the conditions they were taken under, and its "Known issues" section for
what is still rough — including the Old 3DS memory ceiling and the audio gaps during loads.

---

## 7. The Expansion Kit

The 64DD Expansion Kit is compiled by `-DGDX_3DS_EXPANSION_KIT=ON` and is not the shipped
configuration. It boots on a console, reaches the main menu, draws three screens' worth of
vocabulary the shipped build cannot draw, and loads a playable course editor and machine creator
from a disk image streamed off the SD card. It can be baked with an English text pack so a
Japanese dump reads in English without replacing the dump.

It goes through the same renderer: `interp` and every lever in §2–§4 apply to it unchanged, and the
main-menu frames are byte-identical between an Expansion Kit build and the shipped build except
for the two extra signs. It now also fits the Old 3DS's 8 MiB RDRAM arena (an `o3ds-ek` build
boots, reaches the menus, and loads the course editor and machine creator with a streamed disk) —
but fitting in memory isn't the same as running well: on real Old 3DS hardware 64DD courses are
jittery, the disk music breaks up, and the editors are laggy, so `o3ds-ek` is experimental and not
released in v1.0.0 — see `ARCHITECTURE.md` §4.1 for the four console builds and
[`RELEASE-NOTES.md`](RELEASE-NOTES.md) Known issues.

What it costs and what is still wrong with it is in [`RELEASE-NOTES.md`](RELEASE-NOTES.md). The
disk-streaming levers are in §2.6 above.
