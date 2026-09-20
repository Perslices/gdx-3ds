# residuals — progress (the two oracle residuals: frame-1702 render transient, boot divergence)

Track: **residuals**, branch `native/residuals`, worktree `~/code/gdx-3ds/residuals`, forked from
`native/main` @ 2611e0d. Inputs: `oracle-progress.md` "What is not (two residuals…)" and
`oracle-evidence/`. Lane: **RetroArch** (`tools/dev/gdx-dev retro run`, private instances, no lock).

## State

| Step | Status |
|---|---|
| R1 reproduce both residuals on the RetroArch lane | done — A: 3 states in 3 runs; B: 4 sequences in 6 boots |
| R2 bisect `trectbatch` / `trifast` / `tmemfast` | done — **trectbatch alone** causes both (a) and (b) |
| R3 root-cause A | done — `Interpreter::TextureCacheClear()` recycles ids in `unordered_map` order |
| R4 fix A (sorted free list) | done — `port/3ds/vendor/lus/src/fast/interpreter.cpp:1102` |
| R5 verify A (>=3 runs, `fhdiff divergent=0`, no `--known`) | done — 43 sampled frames x 3 runs, 3/3 pairs clean |
| R6 root-cause B | done — `func_807442E4` / `func_800B84B8` arm the attract demo off the audio thread's synthesis position |
| R7 fix B (`det=1` pin, pure-delta decomp patch) | done — `decomp-port-title-demo-audio-pin.patch` |
| R8 verify B (>=6 boots, title hashes equal) | done — 6/6 boots identical on all 9 sampled frames |
| R9 side fix: `.gdxrec` paths were cwd-relative | done — `port/3ds/main_3ds.cpp:787` |

---

## Residual A — the frame-1702 transient AND the fast-path pixel delta

### Bisect (probe `replays/probe2.gdxrec`, 1811 ticks, 16 SHOT frames, `det=1`)

Each config compared over the same 16 sampled frames. `f1702` is the residual frame.

| config | 3 runs agree? | f1702 | f475/600/900/1200/1400/1703/1704/1750/1810 |
|---|---|---|---|
| all on (`ON-1..3`) | **no — 3 states** | `ad502622` / `757d7196` / `08e03849` | atlas values |
| `trectbatch=0` (`TB0-1..3`) | **yes** | `8331763c` | standalone values |
| `trifast=0` (`TF0-1`) | — | `ad502622` (an ON state) | **identical to ON** |
| `tmemfast=0` (`TM0-1`) | — | `08e03849` (an ON state) | **identical to ON** |
| all off (`OFF-1`) | — | `8331763c` | **identical to `trectbatch=0`** |

**`trifast` and `tmemfast` are pixel-transparent and are not involved in either symptom.**
`trectbatch` alone accounts for (a) the nondeterminism and (b) the steady pixel delta.
(Note `tmemfast=0` does NOT disable `lus-tmem-same-content-skip`: that is a compile-time
`kGdxTmemSameContentSkip` constant, `interpreter.cpp:6506`, not an ini lever. The `tmemfast`
key only gates `gdx_tmem2_fast_enabled`, the O(1) span teardown.)

### (b) What the steady delta IS — a real difference, and the atlas side is the correct one

`framediff ON-1/f1703 TB0/f1703` -> 782 px, max delta 148, bbox `142,185-263,208` = the whole
"1: MUTE CITY / FIGURE EIGHT" text (`oracle-evidence/f1703-atlas-vs-standalone.png`). Zoomed,
the difference is confined to glyph EDGES: the standalone path pads a non-pow2 texture to pow2
with `memset(0)` and lets GPU_LINEAR blend the last content texel with transparent black, so
edges fade; the atlas path writes a 1-texel replicated gutter around each cell, which is what
N64 `G_TX_CLAMP` actually does. So the fast path is not pixel-transparent, but it is *closer to
hardware* than the slow path — the remaining delta is the known pow2-padding bleed the
`sky_clamp_fix` wrapClamp hack only partially covers. Recommendation: do not "fix" this by
disabling the atlas; treat `trectbatch=0` as a non-reference configuration.

### (a) Root cause of the nondeterminism

`port/3ds/vendor/lus/src/fast/interpreter.cpp:1090` `Interpreter::TextureCacheClear()`:

```c
for (const auto& entry : mTextureCache.map) {          // std::unordered_map
    mTextureCache.free_texture_ids.push_back(entry.second.texture_id);
}
```

`TextureCacheKey::Hasher` (`include/fast/interpreter.h:212`) is `addr ^ (addr >> 5)` over the
texture's raw source pointer, so the walk order is a pointer-hash order. `free_texture_ids` is
consumed **LIFO** (`interpreter.cpp:1567`), so that order decides which id each later import
receives.

That was invisible while every texture owned its own pow2 `C3D_Tex`. With the `trectbatch` HUD
atlas the id decides which atlas **cell** a texture is packed into (`AtlasTryPlace`,
`port/3ds/gfx/gfx_citro3d.cpp:1696`), the cell decides the view's `uOff`/`uScale`
(`gfx_citro3d.cpp:1866-1874`), and `uOff`/`uScale` decide the sub-texel sampling phase of every
rect drawn from it. Different cell -> different bilinear weights at glyph edges -> different
pixels, on the first frame a glyph set is drawn.

**Evidence chain** (`[debug] atlasdiag_from`/`atlasdiag_to`, `[debug] texclrdiag=1`):

1. Full-run atlas traces of two runs (`atlasdiag_from=1 atlasdiag_to=1706`, 910 lines each) are
   identical up to **backend frame 460** — the TITLE -> MAIN_MENU transition, i.e. a
   `TextureCacheClear()`. There one run inserts an extra re-upload (`id=137`) into the
   50-strip HUD gradient burst that the other places later (`id=183`), and every placement
   after that lands on a different page/cell.
2. With `[texmissdiag]` on, the same source address `a=0x88ef498` is served by **id 188 in one
   run and id 2 in the other** — same key, same miss, different recycled id.
3. `[texclr]` receipts print the head of `free_texture_ids` before and after the clear:
   `pre=188,187,186,185,...` in one run vs `pre=190,189,188,187,...` in the other at the same
   clear. The pre-sort order is run-dependent; the post-sort order is not.
4. By frame 1701 the two runs' placements have diverged completely (page 6 @ ay=1 vs page 5 @
   ay=223), and frame 1702 — the first frame the course name is drawn — hashes differently.

### The fix (A)

`port/3ds/vendor/lus/src/fast/interpreter.cpp:1102`, `__3DS__` only:

```c
std::sort(mTextureCache.free_texture_ids.begin(), mTextureCache.free_texture_ids.end(),
          std::greater<uint32_t>());
```

Whole-vector descending sort (LIFO then pops ascending = "lowest free id first"), so the free
list is canonical regardless of map iteration order. Ids left over from an earlier clear must
take part, hence the whole vector and not just the appended tail. Cost: one sort of at most
`TEXTURE_CACHE_MAX_SIZE` u32 per cache clear (mode changes only). Behind nothing — it is a
bug fix.

### Verification (A)

`replays/probe3.gdxrec` — 1811 ticks, **43 SHOT frames**: every 100 frames to 1600, **every
frame 1690..1712**, plus 1750/1800/1810. Three runs, private RetroArch instances, same build,
`det=1 trectbatch=1 trifast=1 tmemfast=1`:

```
fhdiff VB-1 VB-2: compared=43 divergent=0 known_transient=0 first_divergent=none last_common=1810
fhdiff VB-1 VB-4: compared=43 divergent=0 ...
fhdiff VB-2 VB-4: compared=43 divergent=0 ...
```

**No `--known 1702`.** Before the fix the same probe gave three different f1702 hashes in three
runs (`ON-1..3` above).

---

## Residual B — the rare boot / title divergence

### Root cause

`decomp/src/audio/disk/external.c:2190` `func_807442E4()` and its ROM-library twin
`decomp/src/audio/rom/external.c:531` `func_800B84B8()`:

```c
temp_lo = (s32) note->synthesisState.samplePosInt / 100;
if ((temp_lo > 2100) && (temp_lo < 2110) && (gGameMode == GAMEMODE_FLX_TITLE) &&
    (gTitleDemoState == TITLE_DEMO_INACTIVE)) {
    gTitleDemoState = TITLE_DEMO_ACTIVE;     /* -> attract demo, game.c:434 */
}
```

These are the **only** writes to `gTitleDemoState` outside `game.c`'s own mode bookkeeping, and
they are the whole attract-demo trigger. `Audio_Update()` runs on the game thread once per
frame, but `samplePosInt` is advanced by the dedicated audio producer
(`port/gdx_audio_thread.cpp:249` `AudioThreadMain`), which self-pumps every **5 ms of wall
clock** and produces while the DSP ring is under target — nothing ties it to the VI tick.
Neither `det=1`'s seeds, the `boot_hold_ticks` pin nor `det_audio` (which only pinned
`audioRandom`) touched it.

Emulated, the game runs far slower than realtime, so `temp_lo` advances by tens per game tick
while the arming window is 9 units wide: whether any tick lands inside it — and hence whether
the attract demo starts at all, and on which frame — is run-dependent.

**Direct evidence** (temporary `[demoprobe]` instrumentation, three boots of the same replay,
`det=1`): at input tick 127 `temp_lo` = **8 / 4 / 8**; at tick 420 = **1340 / 1343 / 1350** —
the spread is already wider than the window, 1680 units before it is reached.

**Reproduction** (`replays/probetitle.gdxrec` — 1300 ticks of zero input so the run stays on
the title long enough for `temp_lo` to cross 2100; SHOTs at frames 400..1300). Six boots,
pre-fix:

| run | f700 | f800 | f1300 | mode from f800 |
|---|---|---|---|---|
| TP-1/2/3 | `0102495a` | `9284853e` | `241d9dc1` | 1 (attract demo) |
| TP-4 | **`6e3c9cc9`** | `89f9e888` | `2c3d5691` | 1 |
| TP-5 | `0102495a` | **`07e8dfe4`** | `4215fade` | 1 |
| TP-6 | **`f4abc8f9`** | `0326a7bc` | `0a3e3f7b` | 1 |

Four different timelines out of six boots: the demo starts, but at a run-dependent frame.
This is the same fault the oracle saw as "1 run in 7 left the logo hold ~8 frames early".

### The fix (B)

`port/3ds/patches/decomp-port-title-demo-audio-pin.patch` (pure delta, appended last; touches
only the two functions above, which no other patch edits). Under `gdx_det_audio_pin()`
(`[debug] det_audio`, implied by `det=1`) the two `gTitleDemoState` writes are skipped, so the
attract demo is never armed from audio state and the title screen advances on tick count alone.
Unpinned — all real play — the expression is byte for byte the original, including the
`AUDIOCMD_GLOBAL_INIT_SEQPLAYER` call in the same function, which is audio-internal and is
deliberately NOT gated. README section + `tools/ci-3ds.sh` `PATCH_LIST` updated (14 decomp
patches). `git -C decomp apply` / `apply --check -R` roundtrip verified clean.

Rejected alternative: driving `temp_lo` from a tick counter so the demo still fires at a fixed
tick. More faithful, but it changes what every existing parity replay records (they all assume
the demo does not fire), so it needs its own change with re-recorded replays.

### Verification (B)

Same `probetitle` replay, six boots, post-fix (`TF-1..6`): **all 9 sampled frames identical in
all six runs**, `mode=0` (TITLE) throughout — the demo never arms and the push-start blink
cycles deterministically (`2ddd3546` / `54767edf` / `140dd16b`, the 75-frame period).

---

## Side fix: `.gdxrec` replay paths were cwd-relative

`port/3ds/main_3ds.cpp:787`. `input_record` / `input_replay` built `"rec/<name>.gdxrec"`, a
path relative to the process cwd. Azahar Qt happens to set that to the gdiffuser directory;
the Azahar **libretro core does not**, so every `retro run --replay <x>.gdxrec` logged
`[rec] replay open failed: rec/x.gdxrec` and then ran on autoinput — i.e. a "parity gate" that
compared two non-replays. Both paths are now built from `gdx3ds_fs_base_path()`
(`sdmc:/3ds/gdiffuser/`), like every other file the port opens. Receipt after the fix:
`[rec] replay armed sdmc:/3ds/gdiffuser/rec/probe2.gdxrec frames=1811`.

## Debug receipts added (all off by default, zero cost)

| Key | Effect |
|---|---|
| `[debug] atlasdiag_from`, `atlasdiag_to` | `[atlasdiag]` line per atlas place / release / evict / page-alloc inside the backend-frame window; `[texmissdiag]` line per texture-cache miss in the same window. Diff two runs' traces to find where an atlas history forks. |
| `[debug] texclrdiag` | `[texclr] n=… pre=… post=…` at every `TextureCacheClear`: the head of `free_texture_ids` before and after the determinism sort. |
| (always) `[trectbatch] init on=… env=… cfg=… cfgloaded=…` | one line, proves which way the lever actually resolved (a run whose ini said 1 but whose pixels said 0 cost an hour before this existed — it turned out to be a stale log, see Gotchas). |

## Gotchas (RetroArch lane)

- **The Bash tool's sandbox silently breaks RetroArch.** A sandboxed launch hangs before it
  prints anything — even `RetroArch --version` hangs with 0% CPU. Every emulator command on
  this lane needs the sandbox disabled. This costs ~20 minutes if you do not know it.
- **`fh_every=N` is not usable on this lane.** The oracle's true-frame capture costs ~2.9 s per
  captured frame under the libretro core (vs ~8 ticks/s on Azahar Qt, ~20x), and two of three
  `fh_every=8` runs died with `Unhandled EXC_BAD_ACCESS` around frame 300. Use **SHOT-driven
  captures instead**: `SHOT <label>` tokens in the `.gdxrec` still emit a full
  `[fh] frame=… hash=…` receipt with `fh_every=0`, cost nothing between shots, and 43 of them
  in one run are reliable. A 1811-tick replay then takes ~3.5 min instead of ~85 min.
- **Always read `VERDICT`, never the SD `log.txt`, to decide a run happened.** When a RetroArch
  instance dies at startup the snapshotter copies the PREVIOUS run's `log.txt` and the numbers
  look perfectly plausible — one such stale log looked like a genuine third state of the
  frame-1702 residual and sent the investigation down a false trail for two batches. Cross-check
  `[bridge-init] registered EXE module range: … size=` against the current build.
- **`replay_quit=1` makes `gdx-dev retro run` report FAIL.** The clean self-terminating exit
  logs `[fatal-trace] atexit: process exiting via exit()`, which the `[fatal]` census counts.
  The run and its data are fine; `retro run`'s verdict is a false negative here.
- Three concurrent instances are reliable; instance `r3` crashed at startup twice under load.
- **Check `uptime` before trusting a FAIL.** Late in this session every probe3 run stalled at
  frame ~400 with `heartbeat not advancing`, including on the exact build that had completed
  the same probe twice earlier — the host was at load average 60 from other agents' emulators.
  A stalled guest under host starvation is indistinguishable from a regression in the run
  artifacts; re-run the last known-good build as a control before blaming your diff.

## Not resolved / next

1. **The upstream fork is the guest heap layout, and it is NOT pinned.** Traced with
   `atlasdiag_from=1 atlasdiag_to=462 texclrdiag=1 quit_at_tick=470`, three runs: two agreed,
   the third forked at backend **frame 402**, where the main-menu 296x4 gradient set is
   imported. Same 57 textures, same order, **same content hashes** (`h=37204b95, b3b1d385,
   6ba437ce, …` line for line), but at different guest addresses — `0xa64dd60, 0xa64e760,
   0xa64f120, …` in two runs and `0xa650340, 0xa650d40, 0xa651700, …` in the third, a constant
   `+0x25e0` shift, i.e. one earlier allocation of a different size. `TextureCacheKey::Hasher`
   is `addr ^ (addr >> 5)`, so a shifted address is a different bucket, which is what made the
   clear-time walk order differ. The shift itself comes from the guest heap, whose allocation
   history includes the core-2 asset preload (`[audio-blob] preload worker … 7/7 families
   resident in N ms`, `preload=running/core2` vs `done/core2` at the same frame) — the same
   wall-clock coupling as residual B. Pinning that would mean pinning the preload's completion
   order, which is a deliberate performance feature. The fix here is at the right layer
   instead: **the renderer must not be sensitive to guest heap addresses**, and after the
   sort it is not.
2. **Two other producers of `free_texture_ids` are not canonicalised**: `TextureCacheDelete`
   (`interpreter.cpp:1662`, walks one hash bucket) and `TextureCacheDeletePalette`
   (`interpreter.cpp:1715`, walks the whole map). Both `push_back` in map order. The whole-
   vector sort in `TextureCacheClear` erases any disorder they introduce at the next mode
   change, and the 43-frame gate over a full boot → course-select run is clean across six
   runs, so nothing observable is left — but within one screen the order dependence survives.
   The obvious follow-up is one `GdxCanonicalizeFreeTextureIds()` call at the end of each
   (guarded by "did this batch actually free anything"). It was written and built here but
   **not landed**: by the time it was ready the host was at load average 60 (several agents'
   emulators) and every probe3 run stalled at frame ~400 with "heartbeat not advancing" — on
   the committed build too — so it could not be held to the same gate as the rest of this
   work. Land it with a clean 43-frame x3 gate.
3. The atlas-vs-standalone glyph-edge delta (b) is a real rendering difference that should be
   closed from the STANDALONE side (pow2 padding should replicate the edge texel instead of
   `memset(0)`), which would make the two paths pixel-identical. Not done here: it touches
   every non-atlas texture in the game and needs its own A/B.
4. `AtlasFillTexParams` (`gfx_citro3d.cpp:1938`) computes `wrapClampU/V` from the PAGE extent
   without adding `uOff`, so the sky-clamp limit is wrong for atlas views. Not reachable for
   HUD text (those tiles take the shader clamp path), but it is a latent bug for any clamped
   view.
5. Residual B's pin suppresses the attract demo under `det=1` rather than making it fire
   deterministically — see the patch README for the trade.
6. `port/3ds/vendor/lus/MANIFEST.sha256` is stale on `native/main` for two entries that this
   track did not touch: `include/fast/interpreter.h` (hash does not match the file) and
   `PROVENANCE.md` (absent from the manifest entirely). `tools/vendor-lus.sh --manifest`
   rewrites both. Left alone here so this commit does not hide someone else's drift — only
   the `src/fast/interpreter.cpp` line is updated.
