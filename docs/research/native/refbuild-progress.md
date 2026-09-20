# refbuild — progress

Track `refbuild` (plan: `docs/research/native/04-refbuild-plan.md`, master plan 4.3).
Goal: a **desktop G-Diffuser an agent can run unattended on this Mac**, fed the same o2r and the
same scripted inputs as the 3DS port, producing 320x240 `autotest/*.bmp` that can be diffed
against a console SHOT.

Worktree `~/code/gdx-3ds/refbuild`, branch `native/refbuild`.

## State

- [x] Docker daemon: **Docker Desktop 4.13.0 / engine 20.10.20 starts fine on macOS 15.7.4.**
      `open -a Docker`, then poll `docker info`. No colima needed; nothing upgraded or removed.
- [x] Image `gdx-ref` (`tools/refbuild/Dockerfile`), linux/arm64, Ubuntu 22.04 + Xvfb + Mesa
      llvmpipe. `glxinfo -B` under Xvfb reports **llvmpipe, max core profile 4.5**, clearing the
      GL-4.1-core context `gfx_sdl2.cpp` asks for (plan risk R2 closed).
- [x] Desktop `G-Diffuser` builds and links (`-DGDX_EXPANSION_KIT=OFF`).
- [x] `tools/dev/refrun.py` end to end: stage -> run -> assert every SHOT -> manifest.
- [x] Title-screen BMP, and the run-to-run noise floor measured.
- [x] Race-start BMPs (7 shots, title through the starting grid).
- [x] `tools/dev/refdiff.py` + contact sheets.
- [x] Determinism gate: `title_0300` (PUSH START blink OFF) is **bit-identical across three
      runs**, sha256 `d75dbd5ba5ac02a5...`. Blink-ON frames agree 99.07-100 %.
- [x] **Cross-check vs a 3DS SHOT: 88.69 % of pixels EXACT, dE76 mean 2.02 (JND ~2.3).**
      Via the lock-free RetroArch lane, not Azahar Qt. Numbers and caveats below.

## Commands

Rebuild the image (~2 min warm, ~12 min cold — it builds pinned SDL2/tinyxml2/spdlog/libzip from
source, exactly as `.github/workflows/release-linux.yml` does):

```sh
cd ~/code/gdx-3ds/refbuild
docker build --platform linux/arm64 -t gdx-ref tools/refbuild
```

Build the game (source read-only, objects in the named volume `gdx-refbuild`):

```sh
python3 tools/dev/refrun.py build -j 5          # add --fresh to wipe the volume
```

Run a scenario headless and collect its BMPs + manifest:

```sh
python3 tools/dev/refrun.py list
python3 tools/dev/refrun.py run title
python3 tools/dev/refrun.py run race_start
python3 tools/dev/refrun.py run title --label title-b        # a second run, to compare
python3 tools/dev/refrun.py emit-3ds race_start -o /tmp/gdx-autoinput.txt
python3 tools/dev/refdiff.py A.bmp B.bmp --out-dir ~/gdx-refrun/_diffs
```

Run directories land under `$GDX_REF_RUNS` (default `~/gdx-refrun/<label>/`) and hold
`G-Diffuser`, both `.o2r`, `gdiffuser.cfg.json`, `script.txt`, `run.log`, `manifest.json` and
`autotest/*.bmp`. **Never inside the repo, and `fzerox.o2r` is never committed.**

## Measured

| | |
|---|---|
| Image build, cold / warm | ~12 min / ~2 min |
| Game build, incremental no-op | 6 s (2 s configure + 4 s) |
| Game build, cold, `--fresh -j 4` (incl. the `gdx-extract` ExternalProject and the FetchContent deps) | **472 s** (23 s configure + 449 s) |
| `run title` wall (5 shots, frame-paced) | 21 s |
| `run race_start` wall (7 shots, 4 WAITMODE gates) | 50 s |
| Reference run-to-run, `title_0300` | **bit-identical** (same sha256, 3 runs) |
| Reference run-to-run, `title_0600` (blink ON) | 99.07-100 % exact; residual is the PUSH START glyphs alone |

## The cross-check (plan step 9)

**Reference** `title_3ds` / `title_0600`, Widescreen=1 in a 400x240 window, llvmpipe.
**Console** `G-Diffuser-3DS.3dsx` from `native/main@beff251f`, run in the lock-free RetroArch
lane (`~/code/gdx-3ds/native/tools/dev/gdx-dev retro`), Vulkan, tick-600 SHOT.
**Same inputs** (zero, both sides) and **provably the same game data**: both SDs' `fzerox.o2r`
hash to `b320370703f71edab2dfdaab41db245decd7ad55c106522be3f20f6ab2212de5`.

| reference config | exact | within 1 RGB5 quantum | MAE | dE76 mean / p95 | layout \|dY\|>40 |
|---|---|---|---|---|---|
| **Widescreen=1, 400x240** (`title_3ds`) | **88.69 %** | 91.60 % | 2.55 | **2.02** / 12.49 | 2.17 % |
| Widescreen=0, 320x240 (the plan's 3.1 floor) | 28.46 % | 35.55 % | 51.55 | 36.05 / 98.09 | 44.09 % |

Where the residual 11.31 % lives: 2.91 % at exactly one RGB5 quantum (dither/rounding on the
title's orange gradient), 8.40 % beyond it — of which the blinking PUSH START glyphs are 1.25 %
of the frame, i.e. the same element that dominates the reference's own run-to-run noise.
A mean dE76 of 2.02 is **below the ~2.3 just-noticeable difference**: the two renderers draw the
same picture. Contact sheets and the console BMP are in
`~/gdx-refrun/title-3dsgeom/compare/`.

**The plan's 3.1 geometry floor is wrong for a console comparison, and by a factor of three.**
Widescreen is indeed "the single most likely silent geometry mismatch" — in the other direction.
The 3DS never *registers* `gEnhancements.Graphics.Widescreen` (`port/3ds/game/CMakeLists.txt`
excludes `gdx_menu*.cpp`; `gdx3ds_cvar_stub.cpp` returns the caller's default of 1), so the
console renders hor+ — which its own log confirms with `[wide] hit=` receipt lines. Its top
screen is 400x240 and the 320x240 mirror is a uniform 0.8 downscale of it (content box 48..352
on the scanout twin, 39..282 on the mirror). Both configurations now ship as separate scenarios,
`title` and `title_3ds`, so neither is silently "the" setting.

## Decisions / deviations from the plan

- **`tools/refbuild/` not `tools/dev/refbuild/`** for the container (the track brief says
  `tools/refbuild/`); the runner stays at `tools/dev/refrun.py` as planned.
- **Dependencies built from source, pinned, mirroring `.github/workflows/release-linux.yml`**
  rather than jammy's packages as the plan sketch had it. Jammy's `libzip-dev` ships a CMake
  config referencing `zipcmp`/`zipmerge`/`ziptool` binaries it does not install, and its
  `libtinyxml2-dev` ships no CMake config at all — but the real reason is that a reference
  renderer linked against different SDL2/spdlog/libzip versions than the release would be a
  fourth renderer variant instead of the released one.
- **CMake 3.31.6 from Kitware's aarch64 tarball**: jammy ships 3.22, libultraship needs 3.24.
- **`-DGDX_EXPANSION_KIT=OFF`.** Two load-bearing reasons: `port/3ds/game/CMakeLists.txt` sets
  the same option (the 3DS port is cartridge-only, so a reference built with the EK source set is
  not the same game), and this machine has no 64DD disk image or IPL ROM, so the EK build cannot
  boot here at all. Three pre-existing defects had to be fixed to make that configuration link
  and boot — see commit `a990ed2` (`port/gdx_linkstubs_desktop.c`, and an `EXPANSION_KIT` gate on
  FirstBootRun's disk/IPL requirement).
- **The run dir is staged as a development tree** (`assets/extracted/generic.o2r` hard-linked to
  the same `fzerox.o2r`). Without it the reference *quarantines the console's archive*: see the
  gotcha below.
- **Not `/tmp` for run dirs.** macOS reaps it; the first title run's BMPs were gone five days later.

## Gotchas

- **The console's `fzerox.o2r` cannot pass the desktop's golden gate, and that is correct.**
  `tools/prebake` re-STORES `audio_blob/audio_table` and `segment_blob/common_assets_compressed`
  uncompressed *after* the golden SHA-256 gate has passed, so the SD card's archive has the same
  entries, order, sizes and CRCs as the golden but a different container hash. A plain boot
  re-hashes it (`GdxExtractEnsureArchive` -> `ensureCartArchive`), renames it `fzerox.o2r.bad`,
  and then dies with `[rom] FATAL: no ROM file found`. Staging `assets/extracted/generic.o2r`
  makes the run dir a development tree, which short-circuits extraction entirely
  (`DevelopmentTreeProvidesArchive`); `findArchivePaths` still prefers `fzerox.o2r`, so the bytes
  actually mounted are the console's.
- **`WAITMODE 0` is a no-op.** `gGameMode` is `GAMEMODE_TITLE` (0) from the first frame, so a
  script that waits for the title returns instantly, at a black boot frame. The title art appears
  around poll **1100-1500**; the attract-mode demo race takes over around **1800**.
- **The boot timeline is wall-clock-coupled, not poll-counted.** The same script run twice reaches
  the title at different poll indices (a script with earlier SHOTs in it shifts later ones, because
  writing a BMP costs wall time). Enabling the 59.94 Hz frame pacer does **not** fix it. Consequence:
  gate on `gGameMode` with `WAITMODE` wherever a mode change exists, and never expect frame-exact
  reproducibility on the boot path. This is plan risk R5, and it bites in menus, not only in-race.
- **A timed-out `WAITMODE` CONTINUES.** It logs `[autotest] WAITMODE timeout (mode=<want>
  current=<cur>)` and runs the next command, so a run that never left the menus still produces
  plausible-looking shots with the right filenames. Grep `run.log` for `WAITMODE timeout` before
  citing any BMP. (`CONTRIBUTING.md` makes the same point about `LOG` markers.)
- **`gEnhancements.Graphics.Widescreen` defaults to 1, and the 3DS never registers it**
  (`port/3ds/game/CMakeLists.txt` excludes `gdx_menu*.cpp`, and the CVar stub returns the caller's
  default), so the console renders **hor+ widescreen** while `refrun.py`'s floor sets 0 (4:3
  pillarbox) per the plan. Any console comparison must say which side was configured how; the
  manifest records it.
- **`gdx_menu.cpp` force-sets `WidescreenUI` back on and calls `CVarSave()`** while
  `gdx.Migrations.WidescreenUiDefaultOn` is 0. The staged config pins that flag to 1, or the
  override is undone before the first frame. `FramePacing` has the identical migration trap.
- **`exec xvfb-run` hangs as PID 1** after the game exits — a run that had already written its BMP
  looked like a 300 s timeout. `run.sh` keeps bash as PID 1 and uses `stdbuf -oL`, because stdout
  into docker's pipe is block-buffered and a killed run otherwise loses its whole boot log.
- **Menu path** (nothing in the tree documented it): TITLE -START-> SELECT MODE -A-> class popup
  -A-> NOVICE -A-> cup -A-> Jack Cup -A-> Mute City -A-> SELECT MACHINE -A-> Blue Falcon -A->
  settings -A-> OK -A-> race. START only leaves the title; every "OK?" prompt takes A.

## Next steps

1. **In-race cross-check.** Only the title has been compared. `race_start`'s `race_grid` /
   `race_start` are the shots that matter next: the 3DS viewport fold cannot touch in-race
   frames (`docs/research/viewport-progress.md`), so they are the cleanest geometry evidence —
   but the console's plain `<label>.bmp` frame mirror is **black or garbage in-game** (observed
   here at tick 2400, and `tools/dev/replays/README.md` says the same), so the console side has
   to come from the `<label>_scan.bmp` scanout twin at 400x240, which needs `refdiff --align`.
2. **Console-side input parity.** `emit-3ds` produces the autoinput, but its WAITMODE -> fixed
   tick conversion has not been validated against a console run yet.
3. **`gdx-dev retro run` ignores `--replay`.** The flag is on the parser
   (`gdxdev/cli.py`) but `gdxdev/retro.py` never stages it — the manifest comes back
   `"replay": null` and the guest log has no `[input] autoinput script loaded` line. The
   probe here was staged by copying the script to the instance's private SD root
   (`<instance>/saves/Azahar/Azahar/sdmc/gdx-autoinput.txt`), which is what `emu run` does.
   Worth fixing in the devcli track.
