# Native renderer — beta release notes

**Branch `native`. Beta. Every feature lever defaults to `0`, which is the byte-for-byte prior
renderer.** With no `[native]` section in your ini, a build from this branch behaves exactly like
a build from `main`. Nothing below is on unless you turn it on.

What it is, and every ini key, is in [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md).

---

## 1. The result

On a New 3DS, over the WiFi development link, running the canonical Mute City replay with input
pinned and the render thread on:

**`[native] all=1 interp=1` takes total CPU time per frame from 23.81 ms to 18.98 ms — −20.3%.**

| section | `all=0 interp=0` | `all=1 interp=0` | `all=1 interp=1` |
|---|---|---|---|
| dsp (display-list dispatch) | 9.52 | 10.45 (+9.8%) | **8.59 (−9.7%)** |
| vtx (vertex transform) | 1.10 | 0.49 (−55.6%) | 0.56 (−48.5%) |
| tri (triangle setup) | 7.66 | 6.05 (−21.0%) | **4.31 (−43.6%)** |
| imp (import / texture) | 1.73 | 1.74 (+0.4%) | 1.22 (−29.3%) |
| drw (draw submission) | 1.04 | 0.98 (−5.4%) | 0.90 (−13.0%) |
| oth | 2.75 | 3.00 (+9.4%) | 3.29 (+19.9%) |
| **total ms/frame** | **23.81** | **22.73 (−4.5%)** | **18.98 (−20.3%)** |

**Read the ratios, not the budget.** This pass runs with `[debug] gputrace=1`, which inflates
every millisecond. The columns are comparable to each other — same build, same replay, same
instrumentation — but 23.81 ms is *not* this game's frame budget and neither is 18.98.

**It is not a scene-mix artefact.** Sliced by draw-call count per frame, so like scenes are
compared with like, the improvement holds across the whole range and is strongest where it
matters:

| scene bucket | `all=0 interp=0` | `all=1 interp=0` | `all=1 interp=1` |
|---|---|---|---|
| menu | 13.45 | 13.42 (−0.3%) | 12.26 (−8.8%) |
| light | 22.97 | 22.46 (−2.3%) | 18.14 (−21.1%) |
| mid | 27.84 | 27.22 (−2.2%) | 24.63 (−11.5%) |
| **crowd (full 30-car field)** | **34.54** | 30.63 (−11.3%) | **27.35 (−20.8%)** |

**The owned interpreter is what pays.** The Tier-2 levers alone move dispatch the wrong way
(+9.8%) and buy −4.5% overall; adding `interp=1` turns dispatch around (−9.7%) and takes the
total to −20.3%.

**`n=1` per configuration on this pass.** The per-bucket table agreeing with the section totals,
and the independent frame-rate pass below agreeing on the ordering, are the cross-checks. A
repeat would still be worth having.

### Frame rate

A separate pass with no profiling instrumentation, three configurations × two repeats, in-race,
first 60 s after the race starts, 5 s beats, repeats pooled. 6/6 runs completed with zero fatal
receipts.

| config | min | p10 | p25 | p50 | p90 | mean |
|---|---|---|---|---|---|---|
| `all=0 interp=0` | 18.4 | 22.0 | 24.6 | 34.4 | 45.0 | **34.4** |
| `all=1 interp=0` | 21.4 | 27.8 | 30.6 | 34.6 | 43.2 | **35.4** |
| `all=1 interp=1` | 21.0 | 22.2 | 25.0 | 38.0 | 47.2 | **36.3** |

The mean moves from 34.4 to 36.3 — about 6%. **The floor moves more than the mean**, which on a
handheld is the part you feel: `all=1 interp=1` lifts the minimum by 2.6 fps, and `all=1
interp=0` lifts the minimum by 3.0 and p25 by 6.0 while giving back 1.8 at p90.

These figures are not comparable to any frame-rate numbers published for the stable branch: this
replay now captures far more screenshots than the one those were taken on, and a screenshot costs
frames. All three configurations pay it equally, so the comparison above is internally valid.

---

## 2. Also in this release

- **Two parity bugs found and fixed** when the replay corpus widened past its first course — a
  stale CI-texture decode after an in-place palette rewrite (`interp=1`, on the RECORDS course
  flyby), and a material-guard mismatch on retained machine meshes (`machines=1`, on the 4P
  machine-settings screen). Both are described in [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md) §3.
  All corpus replays now report zero divergent frames.
- **`ownfold`**, the owned interpreter's own prim/env colour fold — the counterpart of `fx_hud`,
  which is inert under `interp=1`. Its parity has been gated on one replay so far, not the whole
  corpus, and it has not been A/B'd on hardware.
- **Several silent-drop sites closed** in the texture cache and the vertex loader, each behind its
  own default-on killswitch (`texstalefix`, `vtxclamp`, `rawcopywhole`, `texinval`) so a
  regression can be bisected from the ini on the console.
- **Asymmetric hysteresis in the dynamic-LOD controller**, so it tracks load instead of hunting.
- **Stereo 3D with the native levers, on the emulator.** Both eyes, zero divergent frames for
  `all=0`, `all=1` and `all=1 interp=1`, with identical stereo-anchor receipts across all three.
  An earlier note that the emulator "never runs the right-eye pass" was wrong — the emulated 3D
  slider was simply sitting at zero.

---

## 3. What has *not* been established

This is the part that decides whether the beta is worth your time. It is written to be read
against the claims above, not instead of them.

**The crash fix is consistent with fixed. It is not confirmed.** A crash was reported roughly 30
seconds into the *second race of a Grand Prix* under `all=1 interp=1`. A cause was found and
fixed. Ten races were run on hardware on the day of this measurement — including two full races
under the exact lever set that produced the crash — with zero fatal receipts in any of them. But
none of those ten was the Grand Prix second race that produced the original dump. The
reproduction is the honest gate, and it has not been run.

**The reported "distance wobble / pop-in" is open.** What has been settled is that it is not the
native stack: 181 consecutive in-race frames are byte-identical between the stock interpreter and
the full native stack, the shipped course path performs no transform of its own, and the
dynamic-LOD controller has been shown to be tracking real load rather than oscillating. The two
surviving explanations — the game's own distant-track behaviour, and a hardware-only render
configuration no parity lane exercises — are separated by an experiment that needs a console and
has not been run.

**The 30-minute soak has not run.** Neither has the HOME-menu / sleep / lid-close suspend check
under the native levers. A HOME-menu hang was seen after one earlier hardware session; a
mechanism has been named and every suspend wait in the port is now bounded with a receipt, but
the sequence that produced the hang has not been re-run against the fix.

**Because of those three, `all=1 interp=1` is not the default and will not be until they pass.**
The bridge and the libultraship interpreter both stay linked; deleting them is a later milestone.

**Old 3DS / 2DS is not started.** Every measurement in this document is a New 3DS. The Old 3DS
work — folding the render thread into main, a memory diet, a 268 MHz budget — has not begun, and
no build from this branch has been run on one.

**Other standing limits:**

- `cpuclip = 0` is a bisect lever and diverges on tunnel interiors. Leave it at `1`.
- Verify modes need their matching census key or they report no coverage and a clean-looking pass
  proves nothing — see [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md) §2.3.
- The long-tail screens now have replays and pass parity, but the Expansion Kit build compiles and
  links without booting, and the credits screen has never been reached by an automated run.
- Parity is same-platform and same-render-thread-mode only. Every emulator millisecond anywhere in
  this project is relative; hardware is the only truth for frame time.

---

## 4. Trying it

Build exactly as for the stable branch — the native renderer is compiled in either way, and it is
the ini that decides whether it draws. See [`3DS-HARDWARE.md`](3DS-HARDWARE.md) §9 for the full
instructions, including the Windows path.

```ini
; sdmc:/3ds/gdiffuser/gdiffuser.ini
[native]
all = 1
interp = 1
```

Check the boot log or the DBG tab: if a lever you set is missing from the `[native]` receipt line,
the ini was not read. To go back, delete the section — there is no migration and nothing is
written to your save.

If you hit something, the useful report is the `[native]` receipt line, the build id from the STAT
tab, what you were doing, and — if you can get one — the log with `[debug] filelog = 1`.
