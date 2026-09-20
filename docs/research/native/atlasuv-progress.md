# atlasuv — the `trectbatch` U shift, root-caused and fixed

Track: **atlasuv**, branch `native/atlasuv`, worktree `~/code/gdx-3ds/atlasuv`, forked from
`native/main` @ `87a12cf`. Input: `residuals2-progress.md` §2 ("the atlas-vs-standalone glyph-edge
delta is a sub-texel horizontal U shift; leading hypothesis: sub-texel UV resolution relative to the
bound texture extent"). Lane: **RetroArch** (`tools/dev/gdx-dev retro run`, private instance per run,
Bash sandbox disabled), probe `replays/probe3.gdxrec` (43 SHOT frames), `[fh]` via SHOT tokens with
`fh_every=0`.

## State — DONE, gate met

| item | status | commit |
|---|---|---|
| reproduce the 782-px residual A/B | done | — |
| `[debug] uvdiag` texcoord receipt | done | `ab9956a` |
| root cause | **found — it is not a precision effect** | — |
| fix + killswitch `[debug] viewclamp` | done | `ab9956a`, `bd58e51` |
| host test `gdx_gfx_atlas_uv_tests` 18 -> 32 checks | done | `ab9956a`, `bd58e51` |
| **gate: probe3 43 frames, `trectbatch=0` vs `=1`, `divergent=0`** | **met** | `bd58e51` |

`tools/ci-3ds.sh --jobs 8`: **RESULT: PASS** (7 host tests, `gdx_gfx_atlas_uv_tests === ALL PASS
(32 checks) ===`, vendor manifest 123 files, 14 decomp patches, both `.3dsx`).

---

## 1. The leading hypothesis was wrong, and provably so

residuals2 guessed the PICA resolves normalised UVs at a sub-texel precision **relative to the bound
texture's extent**, so a 512-wide page would quantise coarser than a 16-wide pad. That fails by two
orders of magnitude before any measurement: PICA varyings are fp24 (16-bit mantissa), so the worst
ULP of a page-relative `u` near 1.0 is `2^-17`, i.e. `512 * 2^-17 = 1/256` **texel**. The measured
shift is ~0.15 texel — 40x larger. The residual is not a precision effect at all.

## 2. What it actually is: a coordinate clamp applied to a vertex

**A `min()` on a rect's FAR VERTEX is not a tap clamp — it rescales the whole quad's UV
interpolation.** `AtlasFillTexParams` (and `DrawTriangles`' legacy repack) gated the `sky_clamp_fix`
content-edge limit on `tex.width < tex.padWidth`. For an atlas VIEW `padWidth` is the **512-wide
PAGE**, so the gate is trivially true and *every* view took a limit of `(cell - 0.5) / page`.

Its standalone twin does not. Every F-Zero X HUD glyph is **pow2 on both axes** (8x8, 16x32, 32x32),
so `width == padWidth`, **no limit is applied at all**, and the PICA's own `GPU_CLAMP_TO_EDGE` clamps
the **taps** at the texture edge.

So a 16-texel glyph's far vertex went `u = 16.0` (standalone) to `15.5` (view): the rect's texel span
was squeezed by 3% and the sampled content slid **right** by 0 at the left edge to **half a texel** at
the right edge. Horizontal only, because these rects span their cell's full width but only half its
height (`T` stops at 16 of a 32-row cell, nowhere near the limit) — exactly the horizontal-only,
right-biased, ~0.15-texel residual residuals2 measured.

### The receipt that showed it (`[debug] uvdiag=1` + the `atlasdiag` frame window)

probe3 f1701, the same course-name glyph quad (`xy=-0.2450,-0.6667,-0.1650,-0.5333`), `cs` = the
CONTENT-relative texel span the two paths are supposed to agree on:

```
trectbatch=0  [uvdiag] wh=16x32 pad=16x32   a=0,0    cs=0.000000,16.000000
trectbatch=1  [uvdiag] wh=16x32 pad=512x256 a=53,1   cs=0.000000,15.500000   <- the shear
```

Independent confirmation from the pixels, before any code was touched: a least-squares fit of
`B(x) = A(x - d)` over the f1703 diff bbox gives `d = +0.180` screen px (rms 16.2 -> 9.2), and the
best 5-tap horizontal kernel is `[-0.017, 0.190, 0.818, -0.012, 0.010]` — a pure rightward
sub-pixel slide, constant (`+0.12..+0.23`) across every 12-px band of the 122-px bbox, i.e. a phase
error, not a scale error. The glyph is 16 texels over 16 screen px, so half a texel at the far edge
averages to ~0.2 px over the quad. The leftover rms is the part a *uniform* shift cannot model: the
real error is a 0 -> 0.5 texel **ramp**.

## 3. The fix: a view's limit is whatever its STANDALONE twin's is

`trectbatch` is a pure performance lever, so the view has to reproduce the standalone path's limit
exactly — **including where that limit itself shears the quad**. Both applying a limit the standalone
does not, *and* omitting one it does, move pixels; the first commit here did the latter and traded the
glyph residual for a race-HUD one (see §4).

`gdx3ds_view_clamp_limit()` (in `port/3ds/gfx/gdx3ds_texclamp.h`, the one home for this contract) asks
what the **standalone pad** would have been — `gdx3ds_standalone_padded()`, the same `NextPow2`/min-8
`UploadTexture` uses:

| axis | standalone | view (fixed) |
|---|---|---|
| `extent < pow2 pad` | `(extent - 0.5) / padded` — shears the far vertex | `(extent - 0.5) / page` — the same shear, bit for bit |
| `extent == pow2 pad` (pow2 >= 8) | no limit; `GPU_CLAMP_TO_EDGE` clamps the taps | `extent / page`, the cell's far EDGE |

The pow2 case is exact because of the cell's 1-texel **replicated gutter**: below `extent - 0.5` the
limit is a no-op, and above it the view's two taps are the last content texel and its own gutter copy,
so the blend returns that texel — which is what `GPU_CLAMP_TO_EDGE` returns for the standalone. Using
the *edge* rather than dropping the limit also keeps a wild coordinate out of the NEXT cell on the
page, which no-limit did not.

Killswitch **`[debug] viewclamp=1`** (env `GDX_ATLAS_VIEW_CLAMP`) restores the pre-fix
`(extent - 0.5) / page` for every view. Receipt: `[atlasuv] viewclamp legacy=0 env=0x0 cfg=0`.

## 4. The wrong turn, kept here because it is the trap

Commit `ab9956a` dropped the view clamp entirely, reasoning that the gutter *is* the clamp. That fixed
every menu frame (500-1400 went from DIFF to SAME) and **broke the race HUD**: frames 1500-1701 went
from SAME to DIFF, a 1256-px delta in a `bbox=121,19-278,40` banner present in every race frame.

Cause: F-Zero X's race HUD strips are **padded, not pow2** (`304x3 -> 512x8`, `160x24 -> 256x32`), so
the standalone path *does* clamp them, and its clamp *does* shear — a 3-tall strip interpolates V over
`0..2.5`, not `0..3`. Receipt, probe3 f1690, same quad:

```
trectbatch=0            wh=160x24 pad=256x32  cs=0.000000,159.500000  ct=0.000000,23.500000
trectbatch=1, no limit  wh=160x24 pad=512x256 cs=0.000000,160.000000  ct=0.000000,24.000000
```

`bd58e51` narrowed the rule to "mirror the standalone" and both cases land.

## 5. Gate and runs

RetroArch lane, Vulkan, `replays/probe3.gdxrec`, `det=1 fh=1 fh_every=0 trifast=1 tmemfast=1
padgutter=1`. Cite the ARTIFACT sha256: `GDX3DS_BUILD_ID` is configure-time and reads
`native/atlasuv@87a12cf7` for every build below.

| run | artifact | ini | purpose | result |
|---|---|---|---|---|
| `tb0` / `tb1` | `4fd344d5…` | `trectbatch=0/1` | reproduce (pre-fix) | `compared=43 divergent=25 first=500` |
| `uv0` / `uv1` | `e77645e5…` | + `uvdiag=1 atlasdiag 1700-1706` | the texcoord receipt | root cause |
| `fix0` / `fix1` | `fc5106f8…` | view clamp DROPPED | first attempt | `divergent=29 first=1500` (glyphs fixed, race broken) |
| `g0` / `g1` | `09b381a3…` | view clamp at the cell EDGE | second attempt | `divergent=29 first=1500` |
| **`h0` / `h1`** | **`65e5eba7…`** | **the landed rule** | **gate** | **`compared=43 divergent=0`, all 43 SHOT BMPs byte-identical (`cmp`)** |
| `k1` | `65e5eba7…` | `trectbatch=1 viewclamp=1` | killswitch discriminator | reproduces the residual exactly (see below) |
| `p0` / `p1` | `65e5eba7…` | `trectbatch=1 viewclamp=0/1`, `gputrace=1` | cost of the fix | `total 44.10 -> 44.09 ms/f` |

### The killswitch is the discriminator

`k1` — the LANDED build with `[debug] viewclamp=1` — reproduces the residual exactly and nothing
else:

| comparison | result |
|---|---|
| `h0` (`trectbatch=0`) vs `k1` (`=1`, `viewclamp=1`) | `compared=43 divergent=25 first=500` — the original residual |
| f1703 `framediff` `h0` vs `k1` | `diff_pixels=782 max_delta=148 bbox=142,185-263,208` — residuals2's number |
| `tb1` (pre-fix build) vs `k1` (fixed build, clamp restored) | `compared=43 divergent=0` |

So the view clamp accounts for the whole 782-px delta: with it on, the fixed build is bit-identical
to the pre-fix one; with it off, bit-identical to `trectbatch=0`.

### Profile: unchanged

`p0` / `p1` = the landed build, `trectbatch=1`, `viewclamp=0` vs `=1`, **`gputrace=1` (stated — it
inflates: p50 21.6 fps vs 31.4 without)**, 790/789 `[prof]` windows, 0 error receipts either side.
`gdx-dev prof --ab p0 p1 --by-vtxn`:

```
[prof] mean ms/frame, all windows    total 44.10 -> 44.09   delta -0.00 (-0.0%)
  tri 25.93 -> 25.92   imp 2.10 -> 2.10   drw 0.55 -> 0.55   dsp 12.82 -> 12.82
bucket menu (nD 1-15)                total 12.75 -> 12.77   delta +0.02 (+0.1%)
bucket light (nD 16-49)              total 44.54 -> 44.54   delta -0.00 (-0.0%)
fps p10/p50/p90                      21.4/21.6/21.6 both
```

Expected: the change is two scalar comparisons and a `NextPow2` per texture unit at **batch begin**
(`AtlasFillTexParams`), never per vertex and never per fragment. `[trect2] atlas=93/0/2/0` in `p0`
confirms the lever was engaged.

Cross-build, `gputrace=0`, `[watchdog]` fps p10/p50/p90 — the fix and the `[uvdiag]` receipt cost
nothing, and `trectbatch`'s own win is intact:

| run | build | p10 / p50 / p90 |
|---|---|---|
| `tb1` | pre-fix, `trectbatch=1` | 31.4 / 31.6 / 31.6 |
| `h1` | landed, `trectbatch=1` | 31.4 / 31.4 / 31.6 |
| `tb0` | pre-fix, `trectbatch=0` | 29.4 / 29.6 / 29.6 |
| `h0` | landed, `trectbatch=0` | 29.4 / 29.6 / 29.6 |

**No `--known`.** Determinism controls, all `compared=43 divergent=0`: `tb0` vs `fix0` and `tb0` vs
`uv0` (same config, different builds — the fix cannot touch `trectbatch=0`, and it does not), and
`tb1` vs `uv1` (so the `[uvdiag]` logging itself moves no pixel).

f1703 `framediff`, `trectbatch=0` vs `=1`: pre-fix `diff_pixels=782 max_delta=148
bbox=142,185-263,208` (residuals2's number, exactly); post-fix `IDENTICAL`.

## 6. `[debug] uvdiag` — how to use it again

`uvdiag=1` plus a non-empty `atlasdiag_from`/`atlasdiag_to` window logs one line per batch of <= 12
vertices that uses texture unit 0:

```
[uvdiag] f=<frame> pg=<page,-1 standalone> id=<texId> wh=<WxH> pad=<padWxpadH> a=<ax,ay>
         xy=<x0,y0,x1,y1 screen bbox> cs=<min,max content texel S> ct=<..T> b=<u,v float bits> nv=<verts>
```

Pair two runs by `xy` — the lever cannot move a vertex position — and compare `cs`/`ct`.

**Caveat.** `a=` and therefore `cs`/`ct` come from the texture bound at DRAW time. In a merged atlas
batch that is the LAST view, so a line whose `cs` starts far negative (e.g. `-18.0`) is a batch whose
vertices belong to an earlier cell; only the batch whose last rect owns the binding reads correctly.
The 12-vertex cap keeps most merged batches out of the log for exactly this reason.

## 7. Gotchas (add to the lane's list)

- `gdx-dev retro run` has **no `--shots`** flag (that is `emu run`); the retro lane always collects
  them. The flag is an argparse error, so two runs died instantly and silently in a background pipe.
- Frame hashes on probe3 are **fully deterministic run-to-run** with `det=1` on this lane (three
  same-config cross-build pairs, `divergent=0`), so a single A/B pair is trustworthy — residuals2's
  "race SHOTs are not run-to-run deterministic" was an Azahar-Qt-lane observation.
- A `[uvdiag]`-style receipt is cheap to make useless: the first version capped batches at 12 vertices,
  which is every standalone rect but almost no merged atlas batch (36 lines vs 827). Pair the cap with
  the batch-merge behaviour you are diagnosing.
- `gdx-dev retro run` also has **no `--gputrace`** flag; it is `--ini gputrace=1`.
- The race HUD needed no eye inspection in the end: all 43 SHOT BMPs are **byte-identical** between
  `trectbatch=0` and `=1`, which is strictly stronger. `[trect2]`/`[c3d]` atlas census lines only
  appear with `gputrace=1` or `verbose=1`, so a plain gate run has no atlas receipt in its log — use
  the killswitch A/B to prove the lever was engaged instead.
- probe3 at load ~50-60 takes **310-370 s** wall (and ~420 s with `gputrace=1`); `--seconds 360` is a
  safe bound. Check `uptime`.
