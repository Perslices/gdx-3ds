# The native renderer

A reference for the `native` branch: what it replaces, how it is switched on, how each piece is
checked, and what has actually been measured. Release notes and the honest list of what has *not*
been run are in [`NATIVE-RELEASE-NOTES.md`](NATIVE-RELEASE-NOTES.md).

---

## 1. What it replaces

Everything the stable build draws goes through libultraship's Fast3D interpreter
(`interpreter.cpp`), fed by a bridge (`port/n64_gfx_bridge.cpp`) whose only job is to widen the
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
(`gdx3ds_seam_lus.cpp`, `n64/gdx_seam_owned.cpp`), so the owned walker expands the same markers
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
| `all` | `0` | Master for the **Tier-2 levers only** (`machines`, `track`, `fx_sink`, `fx_sprites`, `fx_hud`, `ownfold`): `1` = each of those that is 0-or-absent reads as 1; an explicit `2` (verify) is kept. Never touches `interp`. |
| `interp` | `0` | `1` = the owned F3DEX2→citro3d interpreter draws the frame instead of LUS Fast3D + the bridge. Always explicit — `all` does not imply it. Carries the Tier-2 hooks, so it combines with every lever below. |
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
| `texinval` | `1` | Honour the game's queued texture-cache invalidations (the arena-rewind clear, the per-race minimap delete) on the owned path. |
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
| `ncensus` | `0` | `[ncensus]` batch-split attribution on the owned path. |
| `dspcensus` | `0` | Per-opcode dispatch census on the owned path. |
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
| `audio_stream` | `0` | `1` = stream the 10.25 MB audio sample bank out of the `.o2r` through a block cache instead of holding it resident. Saves about 10.25 MB of heap and cuts the boot preload from 4,573 ms to 1,703 ms. |
| `audio_stream_cache_kb` | `512` | Size of that cache. Measured hit rate at the default is 97%, over a working set of 72 blocks. |
| `audio_stream_block_kb` | `16` | Block size for it. |
| `audio_stream_verify` | `0` | `1` = serve every streamed read *both* ways and compare. A correctness tool; it is a byte comparison, not a listening test. |
| `tex_upload_kb` | `0` | `0` = the historical 4 MiB texture staging reservation. Any other value caps it, in KiB. Measured high-water across the whole replay corpus is 307,200 B on every replay, so `512` is ample; the decoders clamp and **log** if a value is set too small, and a CLAMP line means exactly that. |
| `disk_stream` | `0` | 64DD image handling for an Expansion Kit build. `0` = resident image (does not fit on 3DS), `2` = stream off the SD card with no resident buffer. There is no useful `1` on this platform. |
| `disk_stream_cache_kb` | `512` | Block cache for the streamed disk. Re-derived from a six-point sweep against a real disk: the whole span of the sweep is 384 KB of card reads, once, so capacity buys almost nothing above the default. |
| `disk_stream_block_kb` | `16` | Block size for it. |
| `disk_stream_report` | `512` | LEO reads between periodic `[diskstream]` receipts. |
| `ek_english` | `1` | Apply the baked English text pack over the mounted disk. Inert in a build where the pack was not baked, which is every default build; skipped automatically on an already-translated image. |
| `ek_fixed_aspect` | — | Killswitch for the fixed-4:3 fix that makes the course editor draw at all. Leave it alone. |
| `heapattr_top`, `membudget`, `membudget_top` | `0` | Heap-attribution and memory-budget censuses. `heapattr_top` needs a build configured with `-DGDX_3DS_HEAPATTR=ON`. |

Build options that go with them, all defaulting to the shipped value:

| option | default | what it does |
|---|---|---|
| `GDX_3DS_RDRAM_MB` | `16` | Size of the emulated RDRAM arena. Measured corpus-wide high-water occupancy is 3,038,864 B (18.1%); `8` is the recommendation and is what the Old 3DS result uses. |
| `GDX_3DS_SIM_O3DS` | `OFF` | Simulate the Old 3DS memtype-0 commit budget so a New 3DS or the emulator reproduces the real grant. **Hazard:** pinning a budget above the host console's real one panics in `crt0` with a black screen and no log. |
| `GDX_3DS_HEAPATTR` | `OFF` | Allocator interposition and the ranked `[heap-attr]` census. A measurement build (~2.13 MB of `.bss`). |
| `GDX_3DS_EXPANSION_KIT` | `OFF` | Compile the 64DD Expansion Kit. **Not additive** — it swaps the ROM subsystems for the disk ones and changes `#ifdef EXPANSION_KIT` behaviour in 65 translation units, so an Expansion Kit build and the shipped build are two different binaries, not two settings. Needs your own ~64.9 MB `.ndd` at runtime. |

---

## 3. How a lever is checked

Four independent gates, in increasing order of what they can see:

1. **Host tests.** `tools/ci-3ds.sh` builds and runs thirteen host test binaries covering the
   vertex packer, the format converters, the track templates, the owned interpreter, the atlas UV
   arithmetic, the config readers, the heap configuration, the LOD controller, the block cache and
   disk streamer, the `.o2r` stream reader, the MAME ↔ `.ndd` converter (30,288 checks plus a
   round-trip), the development CLI, and a machines harness that replays baked meshes against the
   interpreter's own output. It also verifies the vendored-libultraship manifest and applies the
   `decomp-*.patch` stack idempotently, failing on a patch file it does not know.
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

Two parity failures were found the first time the replay corpus widened beyond the canonical Mute
City replay — in paths that had passed every earlier gate, including one with 281,524 host-harness
checks and 1.2 M triangles of bit-exact verify behind it:

- `interp=1` diverged on the RECORDS course flyby. Both texture caches keyed CI textures on the
  palette's DRAM **address**, so a TLUT rewritten in place served whatever decode the entry was
  born with, and the two caches went stale at different moments. Fixed by `palhash` /
  `texhash_ci`.
- `machines=1` diverged on the 4P machine-settings screen. `BindMaterial` copied the triangle
  path's state block but not its **guard**: the interpreter returns before resolving a material
  when every triangle is screened out, so an invisible run left `textures_changed` armed and a
  later run reused a recycled texture id. Fixed by screening the run for a survivor first.

Both are fixed on this branch and all corpus replays report zero divergent frames.

The lesson is in the shape of it: one replay, on one course, in one emulator was never a
sufficient proof.

---

## 4. What is measured

See [`NATIVE-RELEASE-NOTES.md`](NATIVE-RELEASE-NOTES.md) §1 for the renderer's hardware numbers
and the conditions they were taken under, §4 for the Old 3DS heap result, and **§7 for everything
that has not been run** — which includes the audio, where nothing has ever listened.

---

## 5. The Expansion Kit

The 64DD Expansion Kit is compiled by `-DGDX_3DS_EXPANSION_KIT=ON` and is not the shipped
configuration. It boots on a console, reaches the main menu, draws three screens' worth of
vocabulary the shipped build cannot draw, and loads a playable course editor and machine creator
from a disk image streamed off the SD card. It can be baked with an English text pack so a
Japanese dump reads in English without replacing the dump.

It goes through the same renderer: `interp` and the Tier-2 levers apply to it unchanged, and the
main-menu frames are byte-identical between an Expansion Kit build and the shipped build except
for the two extra signs.

What it costs and what is still wrong with it — the audio, the editor's graphical glitches, the
boot time, and the fact that it does not fit an Old 3DS — is
[`NATIVE-RELEASE-NOTES.md`](NATIVE-RELEASE-NOTES.md) §2 and §7. The levers are §2.6 above.
