# G-Diffuser 3DS — Release Notes

## v1.0.0 — 2026-10-19

The first release to support the **Old 3DS / 2DS** family, and the first to include the
**64DD Expansion Kit** (Course Edit, Create Machine, and the DD cups) on console.

### Highlights

- **Old 3DS / 2DS support.** The previous release required a New 3DS. This one runs on
  the original Old 3DS and 2DS too, with its own memory and performance tuning.
- **64DD Expansion Kit on console**, for New 3DS players who own their own Japanese 64DD
  disk dump. Streamed from the SD card — there is no room to hold the whole ~65 MB disk
  in memory on either console. Old 3DS Expansion Kit support and fan-translated disk
  images are not part of this release; see [Known issues](#known-issues).
- **A new bottom-screen dashboard ("DASH")** during races, with three HUD layouts and an
  idle screen between races.
- **A native 3DS renderer** that redraws part of each frame itself instead of going
  through the general-purpose interpreter the first release used for everything. It runs
  by default on both consoles, tuned per console, with no ini editing needed (see
  [Performance](#performance) below).
- **A New 3DS attract-demo freeze fixed.** See [What changed](#what-changed-since-v090)
  below for the technical detail; this matters for default, out-of-the-box play now that
  the native renderer runs there too.
- **Old 3DS audio crackle greatly reduced** by a resampler fix. Some faint crackle can
  still occur in a minority of sessions; see [Known issues](#known-issues).

### Downloads

Download the file for your console from the [latest release](https://github.com/cruxxxxxx/gdx-3ds/releases/latest).
Each release includes a `SHA256SUMS` file so you can check your download.
Each build also comes as a ready-to-unzip `G-Diffuser-3DS-<BUILD>-v1.0.0.zip`, laid out like an SD card.

| File | Console | Content | Who it's for |
|---|---|---|---|
| `G-Diffuser-3DS-O3DS.3dsx` / `.cia` | Old 3DS, Old 2DS | F-Zero X | Old-3DS-family owners who just want the game |
| `G-Diffuser-3DS-N3DS.3dsx` / `.cia` | New 3DS, New 2DS XL | F-Zero X | New-3DS-family owners who just want the game |
| `G-Diffuser-3DS-N3DS-EK.3dsx` / `.cia` | New 3DS, New 2DS XL | F-Zero X + Expansion Kit | New-3DS owners with their own 64DD disk dump |

Pick the build for your console; add `-EK` only if you have your own 64DD F-Zero X
Expansion Kit disk dump and want the course editor, machine creator, and DD cups — the
Expansion Kit is New 3DS only in this release (see [Known issues](#known-issues)). An
Old 3DS build also runs on a New 3DS, but a New 3DS build does **not** fit an Old
3DS's memory — don't install it there. Vanilla and `-EK` builds can both be installed
side by side; they share one ini, archives, and saves folder on the SD card.

### Installing, and upgrading from v0.9.0

First time installing? See `docs/3DS-HARDWARE.md` for the full setup walkthrough.

**Using the Expansion Kit build (new install or upgrade)?** It needs its own 64DD disk
image, placed at `sdmc:/3ds/gdiffuser/media/baserom.jp.ek.ndd` — the retail Japanese
dump; a fan-translated image is not supported in this release (see
[Known issues](#known-issues)) — see `docs/3DS-HARDWARE.md` §6 for the full layout. Not
sure which disk-image layout your dump is in? Check the file size:
about 71 MB (70,627,520 bytes) is MAME's layout — convert it first with
`tools/dev/mame2ndd.py tondd yourfile.mame baserom.jp.ek.ndd` (needs Python 3 on your PC;
no 3DS build toolchain needed — see Tooling below). About 65 MB (64,931,840 bytes) is
already this port's layout — just rename and copy it as-is. These are the only two sizes
to expect; anything else is a bad or truncated dump, not a third valid layout.

Already running v0.9.0? Here's what's different for you:

- **You still need your own game archives.** Nothing in any download contains game
  data. Bake `fzerox.o2r` and `gdiffuser.o2r` from your own F-Zero X ROM with
  `tools/prebake` (see `docs/3DS-HARDWARE.md`) and copy them to
  `sdmc:/3ds/gdiffuser/`, same as before.
- **Your existing `gdiffuser.ini` keeps working.** We are not aware of any ini key being
  renamed or removed since v0.9.0; anything new simply uses its default in an old ini file.
- **Your save carries over.** The save file format hasn't changed since v0.9.0
  (`sdmc:/3ds/gdiffuser/saves/fzerox.sav`) — a save written by an earlier build still
  loads correctly in this one. (v0.9.0 was New-3DS-only, so there's no old Old-3DS save
  to carry over.)
- **The Expansion Kit is new.** See the disk-image note above; vanilla and `-EK` installs
  share the same ini, archives, and saves folder.
- Settings you changed in-game are written back to `gdiffuser.ini` automatically, same
  as before.

### Performance

The native renderer is **on by default** on both consoles in this release. You don't need
to write any ini lines for performance — with no `gdiffuser.ini`, or no `[perf] preset` key
in one, `preset` resolves to `auto`, which picks the tuned configuration for your console at
startup:

- **New 3DS:** the `n60` lever set — native renderer, native race renderer, and frame-pacing
  tuning, with the render thread off.
- **Old 3DS:** `o3ds-auto` — the same tuning, minus a texture-cache headroom setting the New
  3DS has the RAM for, plus three Old-3DS memory/CPU safety levers, plus the existing
  automatic detail scaling in crowded races.

An explicit `[perf] preset` key in `gdiffuser.ini` always wins over the automatic choice.
On a New 3DS, you can set `preset = n60` by hand to force that same lever set explicitly.
**Don't do this on an Old 3DS:** `n60` applies the New 3DS's texture-cache headroom without
the Old 3DS memory/CPU safety levers that `auto` gives an Old 3DS for free, and it isn't a
supported or tested configuration there. `preset = off` is an escape hatch back to the
pre-native-renderer defaults, on either console:

```ini
[perf]
preset = off    # bare stock defaults; no native renderer
```

### What changed since v0.9.0

**Renderer**

- A native F3DEX2 (the N64's graphics-command format) interpreter and native draw paths
  for the track, the racer models, the bottom-screen dashboard, and the particle effects
  (sparks, smoke, explosions, force field), replacing libultraship's general-purpose
  interpreter for those parts of the frame. On by default on both consoles; see
  [Performance](#performance) for the per-console lever sets and how to turn it off.
- A long list of frame-pacing fixes that came out of chasing stutter on real hardware:
  the GPU command queue, display-list batching, and the stereoscopic 3D draw order were
  all reworked.

**Old 3DS**

- First-ever support for the Old 3DS / 2DS family. Runs with an 8 MiB RDRAM arena (vs.
  16 MiB on New 3DS) and streams some data that New 3DS keeps fully loaded in memory.
- Automatic racer-detail scaling in crowded races, tuned specifically for the Old 3DS's
  slower CPU.
- An audio resampler fix that removes most of the audio crackle reported on Old 3DS
  hardware.
- A save-file corruption bug fixed: on real SD cards (unlike most emulators), overwriting
  an existing save could silently fail, so only your very first save ever took effect.
  Saves, ghosts, and 64DD save sidecars now write safely every time.

**New 3DS**

- Fixed a reproducible freeze in the title screen's attract-mode demo loop (surfaces with
  the native renderer on, which is the default now), caused by a GPU command buffer that
  was too small for the demo's full 30-racer field.
- The same save-write fix as Old 3DS, above.

**Expansion Kit**

- Runs on console for the first time, on both Old 3DS and New 3DS: the build boots and the
  Expansion-Kit-only menu signs draw, confirmed on real hardware. The course editor, machine
  creator, and the Expansion Kit's own cups have been verified with a mounted disk image in
  the emulator; a hardware pass with an actual disk image mounted hasn't been done yet for
  this release (see `docs/STATUS.md`). **Only the New 3DS Expansion Kit build (`N3DS-EK`) is
  released in v1.0.0** — on Old 3DS, 64DD courses are jittery, the disk music breaks up, and
  the editors are very slow, so the Old 3DS Expansion Kit build isn't shipped this time; see
  [Known issues](#known-issues).
- The ~65 MB disk image is streamed from the SD card rather than held in memory — neither
  console has room to hold it whole.
- Source builds can now bake in an optional English text pack for the Expansion Kit's
  Japanese-only menus, generated from your own fan-translated disk dump with
  `tools/gen_ek_text_pack.py` before building. This is **not** in any of the three
  standard downloads above — it needs a translated image you supply and a from-source
  build. Your own disk dump stays the authority for everything except the on-screen
  words.
- Without a disk image, the Expansion Kit build still boots and plays the normal game;
  only the Expansion-Kit-only menu entries stay greyed out.

**Bottom screen**

- A new in-race dashboard ("DASH") showing live race information, with three HUD
  placement modes (classic top-screen HUD, split across both screens, or everything on
  the bottom screen), cycled from the mode button or a SELECT long-press.
- An idle view (checkerboard background, G-Diffuser logo) when not racing.
- In 2–4 player split-screen, the HUD is always on the top screen.

**Audio**

- The Old 3DS crackle fix above.
- Save-related audio stalls reduced: save writes now happen off the main thread.

**Tooling**

- `tools/build-3ds.sh` — for anyone building from source: one command builds any of the
  four targets (or all of them), producing both the `.3dsx` (Homebrew Launcher) and
  `.cia` (HOME menu) file for each, plus a `SHA256SUMS` file, straight into a `dist/3ds/`
  folder (see `docs/BUILDING.md`).
- A WiFi development link (3dslink + a small host tool), for anyone building from
  source, for iterating without repeatedly copying files to an SD card.
- `tools/dev/mame2ndd.py` — a bit-faithful converter between the two 64DD disk-image
  layouts (MAME's and this port's). A standalone script, no build toolchain needed —
  for players too: if your Expansion Kit disk dump is in MAME's layout, run this on it
  first (see the Expansion Kit install note above).

### Known issues

- A rare speed-readout spike (~826 KPH) can appear briefly at the finish line.
- Brief audio gaps are possible during some loading moments.
- **Old 3DS has a tight memory ceiling.** The console's real memory budget is thinner
  than most emulators simulate; an unusually large asset load during a race can run the
  Old 3DS out of memory. Rare in normal play; more likely with non-default content.
- Not every mode has been hardware-tested on every console. Coverage is good but not
  exhaustive — see `docs/STATUS.md` for the current state per feature and console.
- A small amount of audio crackle can still occur on Old 3DS in some sessions, even with
  the resampler fix above. The underlying cause of the fix is confirmed; a second source,
  worse on Expansion Kit builds, is still being tracked down.
- **The Expansion Kit is New 3DS only for now.** An Old 3DS Expansion Kit build boots, but
  64DD courses are jittery, the disk music breaks up, and the course editor, machine
  creator, and menus are very slow — so it isn't released this time (`tools/build-3ds.sh
  o3ds-ek` still builds it from source, as an experimental, unsupported target).
- **Fan-translated Expansion Kit disk images (`baserom.translated.ek.ndd`) are not
  supported.** The game identifies the disk by an exact whole-image checksum, and a
  translation patched onto a different dump doesn't match it; on hardware, a translated
  image froze at boot instead of reaching the menu. Use the retail Japanese
  `baserom.jp.ek.ndd` dump for now — English support is planned for a later release.
- Stereoscopic 3D plays but isn't yet a locked 60 fps on either console; see
  `docs/STATUS.md` for the current state.

### Measured performance

Hardware figures below are from Old 3DS consoles running the native renderer's `o3ds-auto`
lever set — the same one this release now applies by default (see
[Performance](#performance) above) — in races against the game's own AI field (not
WiFi-connected multiplayer).

| Mode | Scene | Excluding log-flush stalls | Whole run, including them |
|---|---|---|---|
| Grand Prix, 2D | Full 30-racer field, in-race | **57.3 fps mean** (4.6% of frames repeated) | 51.9 fps mean |
| Time Attack, 2D | Single racer | Not yet measured on hardware for this release | — |
| Grand Prix, stereoscopic 3D | Full field, 3D slider on | Not yet separately confirmed on hardware for this release | — |

("Log-flush stalls" are brief pauses while buffered debug-log lines get written to the SD
card; the hardware run this table is sourced from ran extra diagnostic logging on top of
normal play to get these numbers, so the "excluding them" column is the more representative
figure for ordinary play.)

New 3DS hardware frame-rate figures for the native renderer (also on by default there, via
the `n60` lever set) have not been captured separately yet — see `docs/STATUS.md`. The New
3DS has comfortably more CPU headroom than the Old 3DS at every setting tested so far.

### Thanks

To the F-Zero X decompilation and 64DD research this port is built on, to the libultraship
project this builds on for the PC side of G-Diffuser, and to everyone who tested beta
builds on real hardware along the way. The full story of how this port was built is in the
[post-mortem](postmortem/).

---

## Previously: v0.9.0 — 2026-09-04

The first public release. New 3DS / New 2DS XL only, built entirely on libultraship's
general-purpose interpreter (no native renderer, no Old 3DS support, no Expansion Kit on
console).
