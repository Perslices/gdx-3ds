# The native renderer

What the native renderer replaces, how it is switched on, how each piece is checked, and what has
actually been measured on hardware. This is the renderer the v1.0.0 release ships with — the ini
levers below default to the *old* (libultraship-interpreter-parity) path, so a plain
`gdiffuser.ini` with no `[native]` section behaves exactly like the pre-native build. The result
numbers and the honest list of what has *not* been run are in
[`RELEASE-NOTES.md`](RELEASE-NOTES.md). For where this fits in the rest of the port, see
[`ARCHITECTURE.md`](ARCHITECTURE.md) §4.3.

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
*feature* lever defaults to `0`, which is the exact prior path.

To try the whole thing:

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
`cov`, `shots`. `[net] enabled` (the development link to a host) must stay `0` for any normal run.

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
| `disk_stream` | `2` on 3DS | 64DD image handling for an Expansion Kit build. 3DS defaults to streaming (`2` = stream off the SD card with no resident buffer); `0` (resident image) does not fit either console's memory and has to be set explicitly to use it. There is no useful `1` on this platform. |
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

Neither generation above is on by default — a stock `gdiffuser.ini` draws the whole frame through
the carved libultraship interpreter, as it always has. The way this release actually turns the
native path on is one key:

```ini
[perf]
preset = n60
```

On a New 3DS build, this bulk-sets roughly sixty `[native]`, `[debug]`, `[stereo]` and `[menu]`
keys to the values this release ships with — every one of them is still an ordinary ini key, so an
explicit entry for any of them in your own `gdiffuser.ini` overrides the preset's value for that
key only. Grouped by what each batch is for:

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

**Old 3DS does not use this same preset name for its equivalent configuration.** It reaches a
60 fps race through its own, separate recipe of default ini values appropriate to its tighter
memory budget (§2.6's `audio_stream`/`tex_upload_kb` among them), shipped as the Old 3DS build's
own defaults rather than as one `[perf] preset` value. For the exact key list, read
`port/3ds/gdx3ds_preset_n60.c`'s header comment and the Old 3DS CIA's shipped ini.

`[perf] preset` also has a narrower, older meaning, unrelated to the renderer: on its own (without
`n60`), it is the automatic draw-distance/detail-reduction system that kicks in for crowded races —
`auto` (the default) selects it automatically on an Old 3DS and leaves a New 3DS alone; `o3ds`
forces it on any model. That system is about *how much* gets drawn, not *how* — it stacks with
either renderer generation.

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
see `ARCHITECTURE.md` §4.1 for the four console builds.

What it costs and what is still wrong with it is in [`RELEASE-NOTES.md`](RELEASE-NOTES.md). The
disk-streaming levers are in §2.6 above.
