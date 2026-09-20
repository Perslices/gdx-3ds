# residuals2 — progress (four bounded items off the residuals track's "Not resolved" list)

Track: **residuals2**, branch `native/residuals2`, worktree `~/code/gdx-3ds/residuals2`, forked from
`native/main` @ `b7d4907`. Inputs: `residuals-progress.md` §"Not resolved / next" items 2, 3, 4 and 6.
Lane: **RetroArch** (`tools/dev/gdx-dev retro run`, private instance per run, no lock, **Bash sandbox
disabled**), probe `replays/probe3.gdxrec` (1811 ticks, 43 SHOT frames), `[fh]` via SHOT tokens with
`fh_every=0`.

## State

| Item | Status | Commit |
|---|---|---|
| 1. canonicalise `free_texture_ids` in `TextureCacheDelete` / `TextureCacheDeletePalette` | **done, gate met** | `ffd38b4` |
| 2. standalone pow2 padding replicates the edge texel on clamped axes | **landed; the residual it was aimed at is NOT this mechanism — see §2** | `faf6d05`, `3031a22` |
| 3. `AtlasFillTexParams` clamp limit + receipt | **done — the reported bug is not a bug; receipt landed** | `fbd0f3f` |
| 4. `MANIFEST.sha256` regenerated, and now checked by CI | **done** | `b5ae862` |

`tools/ci-3ds.sh --jobs 8`: **PASS** (see §5).

---

## 1. Recycled texture ids are now address-independent within a screen too

`TextureCacheClear()` was sorted by the residuals track; the other two producers of
`free_texture_ids` were not:

- `TextureCacheDelete` (`interpreter.cpp`) walks ONE hash bucket and `push_back`s in chain order —
  several tiles of the same source address share a bucket.
- `TextureCacheDeletePalette` walks the WHOLE map, i.e. `unordered_map` order.

`TextureCacheKey::Hasher` is `addr ^ (addr >> 5)` over the texture's raw GUEST pointer, and the guest
heap is not pinned run to run (the core-2 asset preload shifts it — residuals §"Not resolved" 1), so
both orders are run-dependent. `free_texture_ids` is consumed LIFO, so the order decides which
recycled id the next import receives; with the `[trectbatch]` atlas the id decides the CELL, the cell
decides `uOff`/`uScale`, and those decide the sampling phase of every rect drawn from it.

Fix: one shared `GdxCanonicalizeFreeTextureIds()` (whole-vector descending sort, `__3DS__` only) used
by all three producers, called only when the batch actually freed something. Cost: one sort of at most
`TEXTURE_CACHE_MAX_SIZE` u32 per freeing batch (mode changes, buffer refreshes, palette restages) —
never per texture, never on the steady draw path.

### Gate (met)

`replays/probe3.gdxrec`, 43 SHOT frames, three private RetroArch instances, artifact
sha256 `e11607c3…` (`3031a22`'s tree), `det=1 trectbatch=1 trifast=1 tmemfast=1 fh_every=0 padgutter=1`:

```
fhdiff R1 R2: compared=43 divergent=0 known_transient=0 first_divergent=none last_common=1810
fhdiff R1 R3: compared=43 divergent=0 ...
fhdiff R2 R3: compared=43 divergent=0 ...
```

**No `--known`.** An earlier 3-run set on the item-1-only build (`W-1/2/3`) was also 3/3 clean.
Cross-build control: `R1` vs `Q1` (a run on the previous build) `compared=43 divergent=0`, so nothing
in items 2–4 moved a pixel in the reference configuration.

---

## 2. The atlas-vs-standalone glyph-edge delta is NOT the pow2 padding

The brief (and residuals §"Not resolved" 3) says the 782-px course-name delta is the standalone path
`memset(0)`ing its pow2 padding so `GPU_LINEAR` fades clamped edges into black, while the atlas writes
a replicated gutter. **That diagnosis does not survive measurement.** The fix was implemented anyway
(it is right on its own terms, see below), and then falsified as the cause:

### The fix that was landed

`UploadTexture` now writes the same 1-texel replicated gutter the atlas has always written
(`kAtlasGutter`): the padding column at `x == width` and the padding row at `y == height`, plus their
corner, take the edge texel on **clamped axes**; non-clamped axes get their gutter **zeroed** (a
recycled id keeps its `C3D_Tex` when the padded extent is unchanged, so "leave it alone" would serve
the previous tenant's gutter). WRAP/MIRROR axes are deliberately untouched — on this hardware such an
axis folds at the PADDED period (`docs/research/shadow2-mirror-period-audit.md`), so its pad texels
belong to the wrap image, not to an edge clamp. The atlas evict copy-out writes the gutter too
(`3031a22`), so a view that loses its page does not lose its gutter with it.

To know the clamp mode at upload time, `TextureCacheLookup` now seeds a fresh slot's sampler from its
own cache KEY (`key.cms`/`key.cmt`, `__3DS__` only) instead of WRAP/WRAP — the decode calls
`UploadTexture` immediately after the miss, so the old `0,0` seed meant the backend saw WRAP on every
first upload. `cms`/`cmt` are part of the cache key, so a tile wanting different modes gets a
different entry.

Killswitch `[debug] padgutter=0` (env `GDX_PAD_GUTTER_OFF`). Receipts: `[padgutter] init on=… env=… cfg=…`,
a per-upload `[padgutter] n=… WxH -> padWxpadH clampU=… clampV=… rep=… zero=…` for the first 8 padded
uploads (then every 1024th), and a cumulative `padGut=<paddedUploads>/<replicatedAxes>/<zeroedAxes>`
suffix on the `[c3d]` line.

### The receipt's own finding: F-Zero X barely has padded textures

One probe3 run to guest frame **19044** (`det=1 trectbatch=0 padgutter=1`, artifact `7ff29c21…`) logs
exactly **eight** padded uploads, every one of them `320x240 -> 512x256` with `G_TX_CLAMP` on both axes
(`rep=16 zero=0`) — the framebuffer-sized captures. Every other texture the run uploads is already
pow2 on **both** axes, so there is no padding to bleed from in the first place. That is the real reason
the A/Bs below are all `divergent=0`, and it is worth carrying into any future work on the pow2-padding
bug family (sky clamp, livery clamp, shadow mirror period): on this game the family is nearly empty,
and the sky's 64x1 strip is one of the few members — it is not in probe3's route.

### Why it changes nothing here (three A/Bs, 43 frames each)

| A/B | result |
|---|---|
| `trectbatch=1`, `padgutter=0` vs `=1` (`P0` vs `P1`) | `compared=43 divergent=0` |
| `trectbatch=0`, `padgutter=0` vs `=1` (`T0` vs `T1`) | `compared=43 divergent=0` |
| `trectbatch=0 sky_clamp_fix=0`, `padgutter=0` vs `=1` (`SK0` vs `SK1`) | `compared=43 divergent=0` |

The padding is simply never sampled on this probe. `port/tests/gfx_atlas_uv_tests.cpp` shows why: the
`sky_clamp_fix` limit `(extent - 0.5) / padded` puts the last reachable sample exactly on the last
content texel's CENTRE, where the bilinear weight on the first padding texel is **0**. The gutter is a
hardening/consistency fix (it makes the standalone path structurally identical to the atlas one, and
covers the eviction copy-out), not the cause of the residual.

### What the residual actually is

`T1` (standalone) vs `Q1` (atlas), same build, same replay: **25 of 43 frames differ, first at f500**.
`framediff` on the course-name frame reproduces the residuals track's number exactly:

```
f1703  T1 vs Q1: diff_pixels=782 max_delta=148 bbox=142,185-263,208
f500   T1 vs Q1: diff_pixels=667 max_delta=173 bbox=141,15-267,35
```

Per-pixel analysis of the f1703 delta (`T1` = standalone, `Q1` = atlas):

- **769 of the 781 differing pixels** have the ATLAS value inside the STANDALONE's *horizontal*
  3-tap range `[min,max](x-1, x, x+1)`; the 12 that do not are off by one RGB5 step (8 levels).
- The same test on the *vertical* 3-tap range explains only **464**.
- The atlas value is closer to the standalone's **right** neighbour 490 times vs the **left**
  neighbour 248 times.

So the atlas render is the standalone render resampled with a **sub-texel U shift, horizontally only,
biased in one direction** — roughly 0.2 screen px (≈0.16 texel) estimated from partial-coverage edges.
It is *not* an edge fade to black: the standalone side is the CRISP one and the atlas side the smeared
one, the opposite of the residuals track's reading of the zoomed image.

The CPU side is provably not responsible: `gfx_atlas_uv_tests` drives the real packers and shows the
standalone and the atlas view resolve to the **same content-relative texel coordinate** for every
sample (`worstDelta = 0.0`). The difference is therefore downstream of the packed UV, in the texture
unit. Leading hypothesis, consistent with every observation: **the normalised UV's sub-texel
resolution is relative to the bound texture's extent**, and an atlas view is bound to a 512-wide PAGE
while its standalone twin is bound to a 16–64-wide pad — 8–32x fewer sub-texel bits for the same UV,
and a truncation (not round-to-nearest) would explain the one-directional bias. Not proven here.

**Next step for whoever owns `trectbatch`:** dump the packed UVs and the resolved texel coordinate for
one course-name rect under both configurations (a temporary receipt in `gdx3ds_uv_pack` / the packed
append), and confirm on hardware — this could equally be an Azahar sampler artefact. Until then
`trectbatch=0` remains a non-reference configuration, as the residuals track already recommended.

---

## 3. `AtlasFillTexParams` — the reported bug is not a bug

Report: "computes `wrapClampU/V` from the page extent without adding `uOff` — latent bug for clamped
atlas views." **Adding `uOff` there would CREATE a bug.** All four packers that emit PICA UVs —
`gdx3ds_uv_pack` (mesh / `[trifast]`), the interpreter's packed append
(`vendor/lus/src/fast/interpreter.cpp:5260` and `:5363`), `gdx_tri.cpp:591`, and `DrawTriangles`'
legacy repack — evaluate

```
uv_final = min(uv_normalised * uScale, clampLimit) + uOff
```

The **min comes first**, so the limit lives in PRE-OFFSET space: for a view `uScale` is `cell/page` and
`(cell - 0.5) / page` is the cell's own last-texel centre. `DrawTriangles` even says so in a comment
("clamp limits stay in un-offset space"); nothing near `AtlasFillTexParams` did, which is why it read
as a bug.

Landed instead:

- `port/3ds/gfx/gdx3ds_texclamp.h` — one home for the limit formula (`gdx3ds_pad_clamp_limit`) and the
  min-then-offset contract; `AtlasFillTexParams` and the legacy repack call it (expression-identical).
- `port/tests/gfx_atlas_uv_tests.cpp` → host exe `gdx_gfx_atlas_uv_tests`, wired into `tools/ci-3ds.sh`.
  18 checks over the REAL production code, including an explicit **negative control**: fold `uOff` into
  the limit and the sample escapes the cell *and* its gutter (page texel 77.5 instead of 44.5 for a
  12-texel cell at `ax=33`). That is the receipt that would have caught the change.

It also pins the two facts §2 rests on: a clamped standalone axis lands exactly on the last content
texel's centre (bilinear weight 0 on the padding), and an atlas-eligible rect can reach at most texel
`extent` — one past the content — so a 1-texel gutter is exactly wide enough on both paths.
Undershoot is bounded by the eligibility window plus that gutter, so views need no lower clamp.

---

## 4. Vendored LUS manifest

`tools/vendor-lus.sh --manifest` (NOT `--verify`: that re-derives the tree from the libultraship
submodule at the pin and is *expected* to fail from the vendoring commit onward, since the vendored
tree is the source of truth — see `PROVENANCE.md`). Fixed two entries that predate this track —
`include/fast/interpreter.h` (hash did not match the file) and `PROVENANCE.md` (absent entirely) — plus
this track's own `src/fast/interpreter.cpp` edits. `shasum -a 256 -c` is clean over 122 files.

`tools/ci-3ds.sh` now verifies it right after preflight (plain `shasum -c` plus a check that no file is
missing from the manifest; no submodule, no network): `vendor manifest PASS 123 files match`. Editing a vendored file stays fine and expected —
regenerate in the same commit. **This is an addition beyond the brief**: it is what stops the manifest
going stale a third time, but it will FAIL any branch that edits `port/3ds/vendor/lus/` without
regenerating, so revert the CI hunk if that is not wanted.

---

## 5. Runs and receipts

RetroArch lane, Vulkan, `gputrace=0`, `replays/probe3.gdxrec`. **Cite the artifact sha256, not the build
id**: `GDX3DS_BUILD_ID` is baked at CONFIGURE time, so `P/T/Q` (`3f956e68…`) and `R/SK` (`e11607c3…`)
both report `native/residuals2@b5ae8626` despite being different binaries.

| run | ini | purpose | result |
|---|---|---|---|
| `W-1/2/3` | `trectbatch=1` | item-1 gate on the item-1-only build | 3/3 pairs `divergent=0` |
| `R1/R2/R3` | `trectbatch=1 padgutter=1` | item-1 gate on the final build | 3/3 pairs `divergent=0` |
| `P0` / `P1` | `trectbatch=1`, `padgutter=0/1` | padgutter A/B (atlas) | `divergent=0` |
| `T0` / `T1` | `trectbatch=0`, `padgutter=0/1` | padgutter A/B (standalone) | `divergent=0` |
| `SK0` / `SK1` | `trectbatch=0 sky_clamp_fix=0`, `padgutter=0/1` | padgutter A/B with the UV clamp off | `divergent=0` |
| `T1` vs `Q1` | `trectbatch=0` vs `=1` | the residual | 25/43 frames differ, first f500 |
| `GUT` / `GUT2` | `trectbatch=1` / `=0`, `padgutter=1` | `[padgutter]` receipt | 8 padded uploads, all 320x240 clamped |

Artifacts: `P/T/Q` = `3f956e68e2…`; `R/SK` = `e11607c34e…`; `GUT` = `5ba131a0fc…`; `GUT2` = `7ff29c2130…`.

`tools/ci-3ds.sh --jobs 8`: **RESULT: PASS** — 7 host tests (including the new `gdx_gfx_atlas_uv_tests`,
`=== ALL PASS (18 checks) ===`), the new `vendor manifest` step (123 files), 14 decomp patches, both
`.3dsx` built.

## 6. Gotchas hit (add to the lane's list)

- **A `retro run` VERDICT of PASS does not mean the run completed.** Three runs crashed at guest frame
  ~433 (`Unhandled EXC_BAD_ACCESS`, in the console log, 8 s wall) and every one of them was recorded
  `PASS heartbeat reached frame 432, 0 [fatal] lines` — the heartbeat check only sees what was logged
  before the core died. **Cross-check `wall_s` and the `[fh]` count** (`grep -c '\[fh\] frame='` should
  equal the replay's SHOT count, 43 for probe3) before believing any probe3 run.
- The same three runs were fine on the same build minutes later at a lower load average. Load 60+ is
  where this machine starts killing azahar-libretro during the TITLE→MAIN_MENU transition. Check
  `uptime` first, as the residuals track already said, and prefer 2 concurrent instances over 3.
- **`--seconds` only bounds the run; it does not pace it.** probe3 finished in ~100 s at load ~30 and
  needed 420 s at load ~65. 240 s is a reasonable default.
- `replay_quit=1` is unnecessary for `[fh]` gates and only buys the false-negative FAIL the residuals
  track documented. Leave it off.
