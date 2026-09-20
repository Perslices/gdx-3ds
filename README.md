# G-Diffuser 3DS — F-Zero X on the Nintendo 3DS

A homebrew port of the **F-Zero X decompilation** to the New Nintendo 3DS / New 2DS,
built on the G-Diffuser PC port. Runs natively on console hardware with stereoscopic
3D, full audio, and a touch-screen settings menu.

> **This is a fan-made, educational software-preservation project.**
> See the [Legal](#legal) section before downloading or building anything.

**Download:** [latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest) (`.3dsx` for the Homebrew Launcher, `.cia` for the HOME menu). You still need your own cartridge dump, converted on your PC; see [Building & installing](#building--installing).

> **You are on the `native` branch.** It carries everything `main` does plus an optional
> native PICA200 renderer that bypasses the CPU display-list interpreter — all of it off by
> default. Beta builds are tagged `native-v*`; the stable build is on `main` and tagged `v*`.
> See [Native renderer (beta)](#native-renderer-beta) for what it does and what is still unproven.

## On hardware

<p align="center">
  <img src="docs/media/gameplay-race.gif" width="420" alt="F-Zero X Grand Prix running on a New Nintendo 3DS; the bottom screen shows the debug menu reading 59.8 fps">
  <img src="docs/media/gameplay-handheld.gif" width="420" alt="Handheld footage of a race on the New 3DS at 60 fps">
</p>

<table>
  <tr>
    <td><a href="docs/media/title.jpg"><img src="docs/media/title.jpg" alt="F-Zero X title screen on the New 3DS, debug menu on the bottom screen"></a></td>
    <td><a href="docs/media/menu.jpg"><img src="docs/media/menu.jpg" alt="Select Mode screen on the New 3DS"></a></td>
  </tr>
  <tr>
    <td><a href="docs/media/character1.jpg"><img src="docs/media/character1.jpg" alt="Select Machine screen on the New 3DS"></a></td>
    <td><a href="docs/media/character2.jpg"><img src="docs/media/character2.jpg" alt="Blue Falcon machine stats on the New 3DS"></a></td>
  </tr>
</table>

Full-quality clips: [Grand Prix](https://cruxxxxxx.github.io/gdx-3ds/media/gameplay-race.mp4) ·
[handheld](https://cruxxxxxx.github.io/gdx-3ds/media/gameplay-handheld.mp4). The bottom screen in every shot is the
port's own debug menu (STAT tab): frame rate, heap, build id.

## Author's notes

Hello reader. This is the only section of this project written by a real human. Isn't that kind of neat? Well, I guess it depends on how you feel about all of these changes in our lives. In this section, I'll quickly detail my thoughts on this 100% AI generated port, its reflection on the direction of software development, and my own experience in using Claude Code to create it.

---

Awhile back I saw [Chris Lewis make a post about using Claude](https://blog.chrislewis.au/using-coding-agents-to-decompile-nintendo-64-games/) to assist in the Snowboard Kids 2 decompilation. It got me thinking: what a perfect use-case for a coding agent. It's a long running, tedious, endeavor of classification and definition.

I realized that one day I'd like to port F-Zero X to 3DS using a coding agent when it was decompiled/ported. So when [G-Diffuser](https://github.com/Zorkats/G-Diffuser) port was released based on the [Inspectredc Decompilation](https://github.com/inspectredc/fzerox), I gave it a shot. I pulled a bunch of adjacent repositories & documentation together, fed it to Claude using deep research, read the master plan and gave it the OK to spin up a bunch of subagents. For 3 weeks, I babysat and assumed a role somewhere between a Product Owner and a QA tester.

1. Claude suggests an implementation strategy to either fix a bug or improve performance.
2. I approve the suggestion, push back, or do research/implore research if we seem to hit a dead end.
3. Claude works and validates in Azahar emulator.
4. Claude provides a build and I test on 3DS.
5. I report back and provide debug logs. Loop continues.

This continued until it was 'done'. It still totally blows my mind that after 24 days I can play F-Zero X in 3D at a near solid 60fps.

---

Since my first experience using ChatGPT in 2022, I was filled with total existential dread. I saw my livelihood threatened, as this skill I had dedicated my life to became immediately devalued. Since then, I've used LLM chatbots & coding tools extensively and I've oscillated between feelings of fascination, fear, humility, intrigue, reward, and emptiness.

This project revealed to me how much things have changed. The complexity of this project cannot be overstated: this is a marvel. This is nothing short of a miracle. And it was done by a machine, with no human intervention on the code. I never read a line. I never made a true technical decision.

I learned how to program 15 years ago with the dream of contributing projects like this, and since then I've come to a stark realization: while I COULD plausibly port F-Zero X to the 3DS by hand, the time in my life IS finite, and when weighed against other life pursuits such as making an original game, it would not serve me well. I'm sure the journey would make me a better programmer. I'm sure I would learn so much about graphics rendering, address-space heuristics, the 3DS architecture, edge device profiling, etc. And I wholeheartedly thank & admire everyone whose work this is built off of, many of you are nameless but overwhelmingly appreciated.

The (terrifying) fact is: this would've taken me a few thousand hours of my life. For professionals, this might've cost nearly 80x to make (actually, even more, because Claude is currently subsidized).

So to any naysayers, feel free to not play. I understand. The ethics of LLMs are muddy and their incoming impact on society is understated. This project, in my opinion, serves as proof of the latter. However, to anyone who has ever dreamed of F-Zero X 3DS, enjoy.

## Post-mortem

The full story of the port — every step from research to a native-60 stereoscopic build in 24 days, the road to 60 fps round by round, what fought back, what is new, how the agent fleet was run, and what it cost — is published as a small site:

**https://cruxxxxxx.github.io/gdx-3ds/** (source under `docs/postmortem/`, also available as a PDF there).

## Repository layout

| Path | What |
|---|---|
| `port/3ds/` | The 3DS port: citro3d renderer, stereo, ndsp audio, input, touch menu, lifecycle, render thread, profilers, packaging |
| `port/3ds/patches/` | Pure-delta patch stack applied over the untouched `decomp/` submodule — 15 `decomp-*.patch`, apply order in its README |
| `port/3ds/vendor/lus/` | The slice of libultraship the console compiles, vendored in-tree with every 3DS patch folded in (`PROVENANCE.md`, `MANIFEST.sha256`). The `libultraship` submodule is desktop-only. |
| `port/3ds/gfx/n64/` | The **native renderer**: the owned F3DEX2→citro3d interpreter and the Tier-2 native paths (course, machines, effects, HUD) |
| `port/` | The G-Diffuser PC port this builds on (see `README-G-Diffuser.md`) |
| `tools/prebake/` | Converts *your* ROM into the archives the console reads; `tools/ci-3ds.sh` applies the patches and builds |
| `docs/3DS-HARDWARE.md` | Toolchain, SD layout, install, troubleshooting |
| `docs/research/` | Working notes, briefs, measurements and verdicts written during development |
| `docs/research/native/` | The native-renderer fork's plans, per-track progress docs and verification runs (`STATUS.md` is the one-page summary) |
| `docs/postmortem/` | The post-mortem site and its generator |

---

## Features

- **Native play on New 3DS / New 2DS** — native 60 Hz pacing, 59.6 fps median over a
  full Grand Prix on hardware, with an automatic RIVAL DETAIL tier that steps down in
  heavy 30-machine packs (manual override in the touch menu)
- **Stereoscopic 3D** — real depth via the 3D slider, with adjustable strength and
  convergence from the touch menu
- **Full audio** — music and sound effects through the 3DS DSP
- **Bottom-screen touch menu** — display modes (including a full-screen borderless
  mode), 3D tuning, volume, controller remapping, a log viewer, debug toggles, and
  a screen-off battery saver
- **Proper system integration** — HOME menu, sleep mode, and clean power-off
- **Multi-core rendering** — the display-list renderer runs one frame ahead on the New
  3DS's spare core (DBG tab "RENDER THR"; relaunch to change)
- **Circle Pad tuning** — deadzone, range and response curve steppers with a live pad
  readout in the INPUT tab, so the analog feel can be matched to an N64 stick; optional
  D-pad steering (full or ramped)
- **Heap watch** — an optional threshold trigger that writes a memory census to the SD
  log during long sessions (DBG tab; see `docs/3DS-HARDWARE.md`)
- **Two install formats** — Homebrew Launcher `.3dsx` or an installable `.cia`

## Native renderer (beta)

> **This branch (`native`) is a beta.** Every lever below defaults to `0`, which is the
> byte-for-byte prior renderer. Nothing here is on unless you turn it on, and nothing here
> has been proven on hardware yet — read "Hardware status" before you trust a frame.

### What it is

Everything the stable build draws goes through libultraship's Fast3D interpreter
(`interpreter.cpp`), fed by a bridge whose only job is to widen the game's 32-bit `Gfx`
words into the 64-bit words that interpreter wants. Every triangle in F-Zero X is
transformed and lit on the ARM11 CPU, one display-list command at a time.

The native renderer replaces that, piece by piece, with code that writes PICA200 vertex
buffers directly — and keeps the old path alive beside it. It is a **hybrid** renderer by
design, not a rewrite: the menus, the course editor, the ending and the long tail of
one-off screens stay on an interpreter.

Two tiers, which combine:

- **Tier 1 — own the interpreter** (`interp`). A purpose-built F3DEX2→citro3d walker
  (`port/3ds/gfx/n64/`) replaces libultraship's interpreter *and* the widening bridge.
  Same pixels, no conversion pre-pass.
- **Tier 2 — bypass the interpreter for the three things a race spends its frame on.**
  `track` replays the course tessellator's vertex stream straight into a VBO; `machines`
  draws the 30 racer meshes from baked, retained display lists; `fx_sink` / `fx_sprites` /
  `fx_hud` batch the effect sprites and the HUD.

Each lever has a verify mode that draws the stock path and compares the native path's
packed vertex bytes against it in-process, bit for bit, and each prints a boot receipt.

**Measured effect (emulator, relative):** the five Tier-2 levers together are +10.6 % in
the 30-car window; the whole stack is 1.85× on the emulator's crowd frame. Emulator
milliseconds are relative only — see below.

### `[native]` ini keys

In `sdmc:/3ds/gdiffuser/gdiffuser.ini`, section `[native]`, **read once at boot** (changing
one needs a restart). Every default is the prior behaviour. Full descriptions in
`docs/3DS-HARDWARE.md` §5.

| key | default | what it does |
|---|---|---|
| `all` | `0` | Master for the **Tier-2 levers only** (`machines`, `track`, `fx_sink`, `fx_sprites`, `fx_hud`): `1` = each of those that is 0-or-absent reads as 1; an explicit `2` (verify) is kept. Never touches `interp`. |
| `interp` | `0` | `1` = the owned F3DEX2→citro3d interpreter draws the frame instead of LUS Fast3D + the bridge. Always explicit — `all` does not imply it. Carries the Tier-2 hooks, so it combines with any lever below. |
| `interp_verify` | `0` | `1` = run BOTH interpreters over every job and compare the packed batches field by field (`[nverify]`). Slow; a correctness tool. |
| `cpuclip` | `1` | The owned interpreter clips triangles on the CPU the way LUS does. `0` = let the PICA clip (fewer draws, different pixels). |
| `blendflush` | `1` | The owned interpreter flushes its batch before a blend-alpha change, so alpha compare uses the register in force at the primitive (LUS parity). |
| `machines` | `0` | `1` = the 30 racer meshes draw from baked retained lists (150 lists = 30 slots × 5 LODs), one indexed draw per material run. |
| `machines_walk_only` | `0` | `1` = replay the baked mesh back through the interpreter's own vertex/triangle path. A bisect step. |
| `machines_verify` | `0` | `1` = per draw, compare the walk pass and the native pass triangle streams bit-exact. |
| `machines_mask` | `ffffffff` | Hex per-slot mask (bit *i* = machine slot *i*, 30 slots) — bisect which machine breaks. |
| `machines_lod_mask` | `1f` | Hex per-LOD mask (bit *l* = LOD *l*+1). |
| `track` | `0` | `1` = the course tessellator's vertex loads and index templates are recorded and replayed into a native batch, one draw per material run. `2` = verify. |
| `trkcensus` | `0` | `1` = `[trk]` per-frame census. **Set this with `track=2`** — without it a verify run prints no coverage totals and a clean-looking pass proves nothing. |
| `fx_sink` | `0` | `1` = the six effect sprite ring classes (debris, sparks, smoke, explosions, force field, fall explosions) are recorded at the game-side macro and replayed as native per-class batches. `2` = verify. |
| `fx_sprites` | `0` | With `fx_sink`: fold prim/env colour changes into the vertex colour where the backend carries that input per vertex, so a colour change stops splitting the draw. |
| `fx_hud` | `0` | The same fold rule inside the interpreter's own 2D batches — HUD digits, stars, markers stop splitting on every prim colour. Inert (pixel-neutral, no gain) under `interp=1`. |
| `fxcensus` | `0` | `1` = `[fx]` per-class census. **Set this with `fx_sink=2`**, same reason as `trkcensus`. |

A boot receipt names every resolved value once:

```
[native] all= interp= machines= track= fx_sink= fx_sprites= fx_hud= fx=<bitmask> census trk= fx=
         (resolved: all=tier2 only, interp explicit; ini latched at boot)
[native] interp= path=lus|owned cpuclip= blendflush= verify= hooks=
```

The DBG tab shows the same state on its `NATIVE` and `NATIVE FX` rows. If a lever you set
is missing from that line, the ini was not read — check the path and the section name.

To try the whole thing:

```ini
[native]
all = 1
interp = 1
```

### Hardware status — read this

Being honest about what has and has not been proven:

| thing | status |
|---|---|
| The base port (the `main` branch): 60 fps, audio, stereo, APT sleep/HOME/power-off, flat heap | **hardware-verified** — median 51 / p95 60 / max 60 fps over multiple sessions |
| `track`, `machines`, `fx_sink`, `fx_sprites`, `fx_hud`, and all five together | **emulator-verified**: pixel-identical to all-off over 6002 frames on two builds, three verify modes clean, +10.6 % in the 30-car window |
| `interp` alone | **emulator-verified**: pixel-identical to all-off over 4101 frames (title → menus → race), per-batch differential 0 mismatches over 13,440 tasks, 50-minute soak |
| `interp` together with the Tier-2 levers | **emulator-verified**: `interp=1 all=1` vs `interp=0 all=1` pixel-identical over the canonical replay; the three verify modes report zero mismatches with nonzero coverage |
| Any native lever **on real hardware** | **two sessions so far** (Tier-2 alone, then the full stack), both 0 errors. Not a verdict yet: no A/B frame-time numbers, and the open items below are from those runs. |
| Stereo (3D slider) interacting with any native lever | **unverified anywhere** — the emulator never activates the right-eye pass, so the `[c3d] … anchor=` receipt has never been seen with a lever on |
| `cpuclip=0` on a course with a tunnel | **unverified** — no committed replay has a tunnel |
| Every millisecond in every progress doc | **emulator, relative**. Hardware is the only truth for frame time. |

**Open items** (tracked in `docs/research/native/NEXT.md`):

1. A HOME-menu hang seen after one hardware session; the step is being narrowed with an
   `[apt]` trace.
2. A reported "distant wobble / more pop-in" on hardware, not reproduced in the emulator;
   being A/B'd lever by lever, with the dynamic-LOD hysteresis as the leading suspect.
3. Stereo, the tunnel `cpuclip` A/B, a hardware `[prof]` A/B of the full stack, and a
   30-minute soak are all still owed before any lever can default to on.
4. `fx_hud` has no equivalent fold in the owned walker yet, so `interp=1 fx_hud=1` is
   merely inert.
5. The long-tail screens (course editor, Expansion Kit, ending, records, machine creator)
   have no replays and have never been exercised by an automated run.

Because of 1–3, **every lever stays default-0 and the old renderer stays linked.** Deleting
the bridge and the LUS interpreter is a later milestone, gated on hardware.

### Building it

Identical to the stable branch — the native renderer is compiled in either way, it is the
ini that decides whether it draws:

```sh
git clone https://github.com/cruxxxxxx/gdx-3ds.git
cd gdx-3ds && git checkout native
git submodule update --init decomp fzerox-expansion-kit torch

export DEVKITPRO=/opt/devkitpro
tools/ci-3ds.sh --jobs 8
```

`tools/ci-3ds.sh` is the whole gate and is idempotent: it verifies the vendored-LUS
`MANIFEST.sha256`, applies the 15 `decomp-*.patch` (and fails loudly on a patch file it
does not know about), builds and runs 7 host test binaries, then configures and builds
`build-3ds/port/3ds/G-Diffuser-3DS.3dsx` — picking up Ninja and ccache automatically when
they are on PATH. Add the `.cia` with
`cmake --build build-3ds --target G-Diffuser-3DS-cia`.

**The 3DS lane does not need the `libultraship` submodule.** The 36 sources and 83 headers
the console compiles are vendored in-tree at `port/3ds/vendor/lus/` with every 3DS patch
already folded in; `libultraship` is desktop-only and `torch` is the PC-side extractor's
source. Full instructions, including the Windows path, in
`docs/3DS-HARDWARE.md` §9.

Asset conversion is unchanged and still runs on your PC from your own ROM dump — nothing
in this build embeds game data.

## Requirements

| What | Why |
|---|---|
| New Nintendo 3DS / New 2DS | The port targets the New3DS CPU (804 MHz + L2). Old 3DS is unsupported. |
| [Luma3DS](https://3ds.hacks.guide) custom firmware | Homebrew execution (and sigpatches for the `.cia` route) |
| **Your own F-Zero X (USA) ROM** | **No game data is included or downloadable — you must dump the cartridge you own.** Assets are converted on your PC (see below); the ROM itself never goes on the console. |
| `dspfirm.cdc` DSP firmware dump | Required for audio; dump it on your own console with the DSP1 homebrew app |

## Building & installing

1. **Get the port**: download `G-Diffuser-3DS.3dsx` or `G-Diffuser-3DS.cia` from the
   [latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest) (checksums in
   `SHA256SUMS`), or build it yourself: devkitPro + CMake, `tools/ci-3ds.sh` applies the
   patch stack and builds; step by step in `docs/3DS-HARDWARE.md`, "Building from source".
2. **Convert your game assets on your PC**: download `gdx-extract-<your OS>.zip` from the
   same release, then `python3 tools/prebake/prebake.py --rom <your dump>.z64 --extractor <gdx-extract>`
   produces the `fzerox.o2r` / `gdiffuser.o2r` archives (Python 3 only, no compiler; Windows
   walkthrough in `tools/prebake/README.md`). This step runs entirely on your computer, with
   your legally dumped F-Zero X (USA, rev 0) ROM.
3. **Copy to SD**: the archives go in `sdmc:/3ds/gdiffuser/`, the `.3dsx` in
   `sdmc:/3ds/` (or install the `.cia` with FBI).
4. Launch from the Homebrew Launcher or HOME menu. Full details, SD layout, and
   troubleshooting: **`docs/3DS-HARDWARE.md`**.

## Controls

| 3DS | Action |
|---|---|
| Circle Pad | Steer (deadzone / range / curve tunable in the INPUT tab) |
| D-pad | Steer, when DPAD is set to FULL or RAMP in the INPUT tab (off by default) |
| A | Accelerate |
| B / Y / ZL / ZR | Boost |
| L / R | Slide-drift left / right |
| C-stick ↓ | Brake |
| C-stick | Camera |
| START | Pause |
| Touch screen | Settings menu |

All bindings are remappable from the touch menu (INPUT tab).

## Legal

- **Not affiliated with, endorsed by, or sponsored by Nintendo.** Nintendo, F-Zero,
  Nintendo 3DS, and Nintendo 64 are trademarks of Nintendo Co., Ltd. All original
  game content is © Nintendo.
- **This project contains no game assets, no ROM, and no copyrighted Nintendo
  material**, and none is available from this project in any form. It is a port of
  independently produced, matching decompiled source code. To play, you must supply
  a ROM image dumped from a cartridge you legally own, and the asset conversion
  happens on your own computer.
- **Educational purpose**: this project exists for the study of software
  preservation, compiler/runtime engineering, and console homebrew development.
  It is non-commercial and must never be sold, bundled with game data, or
  distributed with pre-converted assets.
- If you enjoy F-Zero, support the rights holder — buy official releases.

## Credits

- **[inspectredc's F-Zero X decompilation](https://github.com/inspectredc/fzerox)** —
  the matching decomp this port is built on, and the work of every decomp contributor
- **[G-Diffuser](https://github.com/Zorkats/G-Diffuser)** — the PC port foundation (libultraship-based runtime)
- **[libultraship](https://github.com/Kenix3/libultraship)** (Kenix3 & contributors) —
  the N64 runtime/renderer framework
- **[devkitPro](https://devkitpro.org)** — toolchain; **citro3d** (fincs) — PICA200
  GPU library; **libctru** (smea & contributors)
- **[Azahar](https://azahar-emu.org)** — 3DS emulator used during development
- The sm64-port / Ship of Harkinian communities, whose public engineering informed
  many techniques in this port

## Known issues

- Heavy 30-machine packs can dip below 60 fps (use RIVAL DETAIL: REDUCED/MINIMAL)
- A rarely visible tunnel-roof rendering quirk on one course
- Debug toggles (GPUTRACE, VERBOSE) cost frame time and audio smoothness; leave them off to play

On this branch, see also the open items under [Native renderer (beta)](#native-renderer-beta) — they apply only when a `[native]` lever is set.

Bug reports: include `sdmc:/3ds/gdiffuser/log.txt` (enable `filelog` in the DBG menu). If a `[native]` lever was on, quote the two `[native]` boot receipt lines with it.
