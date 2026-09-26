# Native renderer — beta release notes

**Branch `native`. Beta. Every feature lever defaults to `0`, which is the byte-for-byte prior
renderer.** With no `[native]` section in your ini, a build from this branch behaves exactly like
a build from `main`. Nothing below is on unless you turn it on.

What it is, and every ini key, is in [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md).

---

## 0. What is new since the first beta notes

The first version of this page was written around the renderer alone. Four things have landed
since, and one of them changes what the branch is *for*:

1. **The 64DD Expansion Kit runs on real hardware.** It was filed here as compiling and linking
   "without booting". It boots, it reaches the main menu, it draws three things the shipped build
   cannot draw at all, and **the course editor and the machine creator load and are playable**,
   with the Expansion Kit's own cups racing. §2.
2. **The Expansion Kit can be played in English on a Japanese disk** — a 160-row text override
   baked into the build, applied over whatever image you mounted. No second disk image, and your
   own dump stays authoritative for everything except the words. §3.
3. **The shipped build fits an Old 3DS heap grant**, in a measured configuration that is not yet
   the default. Three cuts did it. No Old 3DS has ever run it. §4.
4. **A bit-faithful MAME ↔ `.ndd` converter**, with 30,288 checks in CI. §5.

And one thing got worse in the telling rather than better in fact: **the Expansion Kit's audio
crackles**, badly. §7 is the honest list and it has grown, not shrunk.

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

> **A number is true of the tree it was measured on.** Both tables above were taken in a single
> hardware session against an earlier state of this branch, before the Expansion Kit, disk
> streaming and memory work landed. Nothing in that work touches the shipped build's draw path —
> every byte of it is either `#ifdef EXPANSION_KIT`, or a default-off ini lever, or gated on a
> build option that defaults to the shipped value — and the shipped `.3dsx` has been shown
> byte-identical across several of those merges. But the *renderer* numbers have not been
> re-taken since, and that is the honest status of them.

---

## 2. The Expansion Kit runs on real hardware

F-Zero X's 64DD Expansion Kit — the course editor, the machine creator, and the extra cups —
was never released outside Japan and has never run on a 3DS before. It does now, on a console,
from the owner's own disk dump.

**What works, confirmed on a New 3DS:**

- The Expansion Kit build **boots** and reaches the main menu.
- It draws **three things the shipped build cannot draw at all**: the `COURSE EDIT` and
  `CREATE MACHINE` signs on the main menu, and the `EDIT CUP` plate on the RECORDS screen. All
  three are early-returned behind `#ifndef EXPANSION_KIT` in the game's own code, so no stable
  build has ever put them on screen.
- The **course editor and the machine creator load and are playable**, and the Expansion Kit's
  **DD cups race**.
- The disk is **streamed off the SD card**. There is no resident image: a 64DD dump is ~64.9 MB
  and the heap is smaller than that, so mounting one the obvious way was never possible. The
  measured working set of a whole boot-to-editor session is **89 distinct 16 KiB blocks, about
  1.4 MB**, served through a 512 KiB cache.
- 64DD **writes** (saved machines, saved courses) go to a journal beside the image. Your `.ndd`
  is never written to.

**It took one port fix, not a disk.** The Expansion Kit build had been filed as needing a 64DD
image to boot at all. It did not: the audio producer — a real OS thread, invisible to the game's
cooperative scheduler — held the audio context mutex across a blocking receive on a queue that
nothing was ever going to post to, because the disk-media load path had two early returns that
issue no transfer and signal no completion. The game fiber blocked on the same mutex and the
whole scheduler froze in the boot-logo window. Post-fix the build boots with **no disk image at
all**.

**Two things are wrong with it, and they are visible.**

- **The audio crackles.** See §7; this is the biggest open item on the branch.
- **The course editor has graphical glitches** — most obviously widget captions that are clipped
  until the widget is selected.

**Trying it** is §8. It is an opt-in build (`-DGDX_3DS_EXPANSION_KIT=ON`), it is not the shipped
configuration, and it needs your own disk dump.

---

## 3. The Expansion Kit in English

The Expansion Kit is Japanese-only, and "the course editor works" is as far as validation gets if
you cannot read the menus.

This branch can bake a **text override pack** into the build: 160 rows, 158,691 bytes, derived
from the LuigiBlood fan translation. At mount time it is applied *last*, over whatever image you
mounted, and it overwrites **only** the rows measured to be text-bearing. `CREATE`, `POINT`,
`FILE`, `ENTRY`, `EXIT`, `COURSE`, `SET`, `OPTION` and the node panel's `Size 0/64` all replace
kana in the course editor; the machine creator is translated too, including the three captions
the translation *reshaped* (`Machine Name` is 96×9 where the retail Japanese art is 48×12), which
a byte-for-byte copy could not have got right.

Why it is a build-time pack rather than "use a translated disk":

- The port already had a translated-disk seam, but it is gated on the **exact whole-image CRC-64**
  of one file. An `.ndd`'s whole-image checksum necessarily covers the per-medium defect map and
  the writable MFS area, so no two dumps of two physical discs can ever agree. Handing someone a
  translated image turns the seam on for *that one file*, not for them.
- A retail-Japanese dump contains no English bytes at all, and that is the dump the editors, the
  MFS volume with its saved machines, and the DD cups all come from.

So the pack supplies the missing source and nothing else. **Your own dump stays authoritative**
for drive presence, saves, courses, machines and audio; only the words on screen come from the
translation. Killswitch `[debug] ek_english` (default `1`); it is inert in a build where the pack
was not baked, which is every default build. The manifest of exactly which symbols a baked build
overwrites — names, sizes and capacities, no image bytes — is committed at
`port/gen/ek_text_pack_manifest.txt`. The pack itself is not: generating it needs a translated
image you supply.

---

## 4. Old 3DS: the shipped build now fits

Every number published for this port until now was a New 3DS, and the Old 3DS was formally out of
scope — it grants far less memory and the port did not fit.

**It fits now.** Against an Old 3DS memtype-0 heap grant of **24,576 KiB (25,165,824 B)**, the
shipped build in the configuration below reaches **`arena = 22,384,640 B`** — about 2.65 MiB of
headroom — and completes the canonical replay **49/49 screenshots, zero fatal receipts, zero
allocation failures**. A hardware heap reading taken in the same configuration puts live heap use
at **21,644,200 B**. Six of the seven corpus replays pass; the binding one misses by 484 KiB.

Three cuts got it there, and two of the three were not the ones anyone expected:

| cut | what it was | what it is now |
|---|---|---|
| The RDRAM arena | 16 MiB, one `calloc`, 37% of the heap — and **the high-water mark of a leak that was fixed years ago**. The arena never rewound when the constant was written; the rewind landed three commits later and the size never followed it. Measured high-water occupancy across the whole replay corpus: **3,038,864 B, 18.1%**. | `-DGDX_3DS_RDRAM_MB=8` — the Expansion Pak size, 2.6× the measured high-water |
| The audio sample bank | **10.25 MB** held resident for the life of the process. The game reads **1.14 MB of it per race**, and it is stored uncompressed in the `.o2r`, and the game never dereferences it — every byte is already a copy. | `[debug] audio_stream = 1` — streamed off the archive through a 512 KiB block cache, `hit% = 97` |
| A texture staging buffer | **4 MiB** reserved for the life of the process by the Fast3D interpreter's `Init`, sized from the maximum texture dimension. No memory census had ever named it, because every census in this project enumerates *owners*. Real high-water across all seven replays: **307,200 B — 320×240×4, the N64 framebuffer — byte-identical on every one.** 92.7% of the reservation was never touched. | `[debug] tex_upload_kb = 512` |

**None of those three is the default.** `GDX_3DS_RDRAM_MB` defaults to 16, `audio_stream` to 0 and
`tex_upload_kb` to 0 (= today's size), because turning them on by default is a decision that wants
an Old 3DS in the room. Each is individually parity-clean on the lane (`tex_upload_kb`, for one:
`compared=6002 divergent=0` lever-on vs lever-off).

**Read §7 before quoting any of this.** In particular: the grant was *simulated* on a New 3DS,
**no Old 3DS has ever run any build from this branch**, and the Expansion Kit build is a much
larger binary that does *not* fit.

What made the last cut findable was an allocator interposition that makes the heap **100%
attributable**: the reconciliation closes to the byte (tracked 25,884,104 against `mallinfo`
26,016,280, the difference exactly 4 bytes × 33,044 live blocks = newlib's chunk header), and
every allocation site is ranked and named. It is a measurement build (`-DGDX_3DS_HEAPATTR=ON`),
default off.

---

## 5. A MAME ↔ `.ndd` converter

64DD dumps exist in two layouts: the physical MAME layout and the logical `.ndd` layout this port
reads. `tools/dev/mame2ndd.py` converts either way, transcribed from LuigiBlood's
`leo64dd_python` reference, and it is **bit-faithful**: `tools/ci-3ds.sh` runs **30,288 checks**
plus a full round-trip on every build. It is stdlib-only and takes no image with it — you supply
your own dump.

`tools/dev/mkdisk64.py` generates a synthetic 64DD image, which is what made the disk path
testable without anyone's dump.

---

## 6. Also in this release

- **Two parity bugs found and fixed** when the replay corpus widened past its first course — a
  stale CI-texture decode after an in-place palette rewrite (`interp=1`, on the RECORDS course
  flyby), and a material-guard mismatch on retained machine meshes (`machines=1`, on the 4P
  machine-settings screen). Both are described in [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md) §3.
  All corpus replays report zero divergent frames.
- **The course editor drew nothing, and the reason was one flag.** Entering the editor publishes a
  "render this screen at a fixed 4:3" request whose implementation is a two-step pair that only
  closes with a desktop compositor. The 3DS has no compositor, so every draw went to an offscreen
  texture that nothing composited and framebuffer 0 was cleared black. One binary, two runs, one
  lever apart: 0 non-black pixels of 96,000 before, 47,246 of 96,000 after, and the frame closes
  against the editor's own declared geometry on all four edges.
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
  slider was simply sitting at zero. A later pass also settled that **stereo did not get more
  expensive** under the native levers.
- **Measurement infrastructure**, most of which exists because a previous claim turned out to rest
  on nothing: screenshot schedules derived from a per-frame feature census rather than hand-placed
  (and a note in the tool saying what 100% coverage does *not* mean — it cannot see a same-size
  texture swap); an audio ring-occupancy census that turns "crackly" into a number; a per-call
  audio-load census that attributes a blocking producer tick to its call site; universal
  `[alloc-fail]` receipts; and a ranked heap-residency census.

---

## 7. What has *not* been established

This is the part that decides whether the beta is worth your time. It is written to be read
against the claims above, not instead of them.

**The Expansion Kit's audio crackles.** Reduced, not fixed. The owner's words after the hardware
session were "the audio was crackling less but still very very crackly". It has been made a
number rather than an impression: the shipped build splices ~3,400 frames of silence into its
output over a replay; the Expansion Kit build splices ~30,500 — about **9×** — with a worst single
burst of 816 ms against 96 ms. The mechanism for the largest stall is named and one instance of it
is fixed (a single 0.87 s disk read, taken inline on the audio producer thread, in code written to
take one step per tick because a real 64DD would have DMA'd it in the background). It is not
closed. **And there is a little crackle on the shipped build too.**

**Nothing has ever listened to the audio.** No PCM has ever been hashed or compared by ear in any
gate in this project. The audio correctness evidence is a delivered-byte comparison — every
streamed read served both ways, `compared=3428 mismatches=0` — which is a diagnostic, not a proof
that the sound is right. Every audio claim on this branch is inferred.

**The crash fix is consistent with fixed. It is not confirmed.** A crash was reported roughly 30
seconds into the *second race of a Grand Prix* under `all=1 interp=1`. A cause was found and
fixed. Ten races were run on hardware on the day of that measurement — including two full races
under the exact lever set that produced the crash — with zero fatal receipts. But none of those
ten was the Grand Prix second race that produced the original dump, and that reproduction has not
been run.

**The 30-minute soak has not run.** Neither has the HOME-menu / sleep / lid-close suspend check
under the native levers. A HOME-menu hang was seen after one earlier hardware session; a mechanism
has been named and every suspend wait in the port is now bounded with a receipt, but the sequence
that produced the hang has not been re-run against the fix.

**Because of those, `all=1 interp=1` is not the default and will not be until they pass.** The
bridge and the libultraship interpreter both stay linked; deleting them is a later milestone.

**No Old 3DS has ever run any of this.** §4's grant is *simulated* on a New 3DS — a build option
that pins the commit budget to the Old 3DS memtype-0 split — plus one confirming heap measurement
on hardware. The simulation is faithful about the *budget*; it is not an Old 3DS. Its 268 MHz
clock, its smaller cache and its single core are not simulated at all, and no frame-rate claim of
any kind is made for an Old 3DS.

**The Expansion Kit does not fit an Old 3DS.** It is a different build of the whole game — the
option swaps out the ROM subsystems, the save path, and `#ifdef`-tested behaviour in 65 translation
units — and its heap is around 31.4 MB, well past the 24,576 KiB grant. §4 is about the shipped
build only.

**The Expansion Kit boots slowly.** A fix for the largest component of that is in, and it has not
been verified on hardware.

**The course editor's graphical glitches are open**, including widget captions clipped until the
widget is selected. The editor draws, and it is usable; it is not right.

**The reported "distance wobble / pop-in" is open.** What has been settled is that it is not the
native stack: 181 consecutive in-race frames are byte-identical between the stock interpreter and
the full native stack, the shipped course path performs no transform of its own, and the
dynamic-LOD controller has been shown to track real load rather than oscillate. The two surviving
explanations — the game's own distant-track behaviour, and a hardware-only render configuration no
parity lane exercises — are separated by an experiment that needs a console and has not been run.

**Other standing limits:**

- `cpuclip = 0` is a bisect lever and diverges on tunnel interiors. Leave it at `1`.
- Verify modes need their matching census key or they report no coverage and a clean-looking pass
  proves nothing — see [`NATIVE-RENDERER.md`](NATIVE-RENDERER.md) §2.3.
- Emulator evidence is weak evidence for the Expansion Kit specifically: the emulator tolerates
  unmapped accesses that console hardware faults on, and it time-slices both emulated cores onto
  one host thread, so its thread interleaving is not the console's.
- The credits screen has never been reached by an automated run.
- Parity is same-platform and same-render-thread-mode only. Every emulator millisecond anywhere in
  this project is relative; hardware is the only truth for frame time.

---

## 8. Trying it

### The renderer

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

### The Expansion Kit

**Experimental, and a separate build.** `GDX_3DS_EXPANSION_KIT=ON` is not an additive flag: it
produces a different binary of the whole game, not the shipped one plus two overlays. Do not use
it as your normal build.

You need your own 64DD disk dump. Nothing in this repository contains one, converts one for you
from thin air, or tells you where to find one.

```sh
cmake -S . -B build-3ds-ek -DGDX_PLATFORM_3DS=ON -DGDX_3DS_EXPANSION_KIT=ON
cmake --build build-3ds-ek
```

Put the image on the card beside the game as `baserom.jp.ek.ndd` (or
`baserom.translated.ek.ndd`), i.e. in `sdmc:/3ds/gdiffuser/`, and stream it:

```ini
[debug]
disk_stream = 2        ; stream off SD; there is no resident-image mode that fits
```

If your dump is in the physical MAME layout, `tools/dev/mame2ndd.py` converts it. Writes go to a
journal beside the image; the `.ndd` itself is opened read-only.

If you hit something, the useful report is the `[native]` receipt line, the build id from the STAT
tab, what you were doing, and — if you can get one — the log with `[debug] filelog = 1`.
