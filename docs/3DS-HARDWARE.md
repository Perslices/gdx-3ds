# Running G-Diffuser on a Nintendo 3DS

A player's guide: pick a build, get the assets onto an SD card, launch it, and tune the
settings that matter. This page covers real hardware. The same prebaked archives (§3) also
work with the Azahar emulator; running Azahar itself isn't covered in this release's docs.

## 1. What you need

| What | Why |
|---|---|
| **A Nintendo 3DS, 2DS, New 3DS, or New 2DS XL** | Any of them works — see §2 for which build to pick. |
| **Luma3DS >= 10.1.1** (custom firmware, boot9strap) | Homebrew execution, signature patches for the test-signed `.cia`, and audio support new enough for this game's sound to work. Follow <https://3ds.hacks.guide> if the console is stock. |
| **A way to run homebrew**: the Homebrew Launcher — reached from the Rosalina menu (always available under Luma) or a HOME-menu shortcut — for `.3dsx`, or **FBI** to install a `.cia` | Either works — see §5 for the install steps. If you don't already have FBI, get it from Universal-Updater or <https://github.com/Steveice10/FBI/releases>. |
| **An SD card with a few hundred MB free** | The game's archives are tens of MB; an Expansion Kit disk image is ~65 MB; saves and logs are small. |
| **`sdmc:/3ds/dspfirm.cdc`** (a DSP firmware dump) | Required for any audio. See §7 — without it the game runs, but silent. |
| **A PC with Python 3.8 or newer installed** | Needed once, to prebake your game assets — see §3. |
| **The `gdx-extract` tool** (downloaded separately) | A small PC-side extractor the prebake script drives — see §3 for where to get it. |
| **Your own F-Zero X (US) ROM**, pre-baked on a PC | The console never touches the ROM itself; see §3. |
| **(Optional) a 64DD F-Zero X Expansion Kit disk image**, if you want the Expansion Kit build | See §6. |

## 2. Pick a build

G-Diffuser 3DS ships as four builds:

| Build | Console | Content | File names |
|---|---|---|---|
| **O3DS** | Old 3DS / 2DS | F-Zero X | `G-Diffuser-3DS-O3DS.3dsx` / `.cia` |
| **N3DS** | New 3DS / New 2DS XL | F-Zero X | `G-Diffuser-3DS-N3DS.3dsx` / `.cia` |
| **O3DS-EK** | Old 3DS | F-Zero X + Expansion Kit | `G-Diffuser-3DS-O3DS-EK.3dsx` / `.cia` |
| **N3DS-EK** | New 3DS | F-Zero X + Expansion Kit | `G-Diffuser-3DS-N3DS-EK.3dsx` / `.cia` |

A few things worth knowing before you pick:

- Old vs. New 3DS is a compile-time memory layout choice, not a quality setting — the console
  model is detected at startup either way, so an Old-3DS build still runs at full New-3DS
  settings if launched on a New 3DS. The reverse isn't true: **don't run the New 3DS build on
  an Old 3DS** — it assumes more RAM than an Old 3DS has, and is expected to fail at or soon
  after launch (see §12).
- The Expansion Kit (EK) adds the 64DD course editor (COURSE EDIT) and machine creator (CREATE
  MACHINE), using the retail F-Zero X 64DD disk. It's a separate build of the whole game, not an
  add-on — see §6 for what you need to supply and what happens if you don't.
- Vanilla and Expansion Kit builds install side by side (different HOME-menu titles, different
  save files) and share one SD-card folder for everything else — so if you're unsure, download
  both builds for your console; they share settings and don't conflict.

If a build isn't available as a ready download, anyone with the devkitPro toolchain can build
all four from source instead (§14).

## 3. Prebake the game's assets on a PC (required, every build)

The 3DS never reads your ROM directly, and nothing in the repository or the release download
contains game data. Instead, you convert your own legally-owned F-Zero X ROM into two archive
files **once, on a PC**, and copy them to the SD card. The same two archives work for every one
of the four builds above. You'll also need the `gdx-extract` tool for your PC's OS — download it
from the project's latest release page (see `tools/prebake/README.md` step 2) and pass its path
as `--extractor`:

```sh
python3 tools/prebake/prebake.py --rom /path/to/fzerox-us-rev0.z64 --extractor /path/to/gdx-extract
```

Full requirements, the Windows walkthrough, and troubleshooting: `tools/prebake/README.md`.

## 4. SD card layout

```
sdmc:/
├── 3ds/
│   ├── G-Diffuser-3DS-<BUILD>.3dsx       (if using the Homebrew Launcher — §5)
│   ├── dspfirm.cdc                       (§7 — DSP firmware dump, required for audio)
│   └── gdiffuser/
│       ├── fzerox.o2r                    (from §3 — REQUIRED; without it the game won't boot)
│       ├── gdiffuser.o2r                 (from §3 — always generate both together; without it
│       │                                  the game still boots but engine/UI assets are missing)
│       ├── gdiffuser.ini                 (optional settings — §9-§10; the game writes this itself)
│       ├── media/
│       │   └── baserom.jp.ek.ndd         (Expansion Kit builds only — §6; or baserom.translated.ek.ndd)
│       ├── saves/                        (created by the game; back this up)
│       │   ├── fzerox.sav
│       │   ├── fzerox-ek.sav             (Expansion Kit builds)
│       │   ├── baserom.jp.ek.ndd.gdd     (Expansion Kit disk sidecars — see §6)
│       │   └── baserom.jp.ek.ndd.dcrc    (same two sidecars, see §6)
│       ├── log.txt                       (vanilla build's log — §13)
│       ├── log-EK.txt                    (Expansion Kit build's log)
│       └── crash.txt / crash-EK.txt      (only if `[debug] crashdump = 1`, default off — §10)
```

`<BUILD>` is whichever of O3DS / N3DS / O3DS-EK / N3DS-EK you downloaded. The vanilla and
Expansion Kit builds read and write the same `gdiffuser/` folder, so there's only ever one of
each shared file even if you have both builds installed. The `.gdd`/`.dcrc` sidecar names above
are exact examples for the Japanese retail disk — they always take your disk image's full
filename (including its `.ndd` extension) and add `.gdd` or `.dcrc` to the end of it.

## 5. Install: `.3dsx` or `.cia`

Both come from the same build and share saves, settings, and logs — use either one, or both.

**`.3dsx` — Homebrew Launcher** (simplest; just a file copy):

1. Copy `G-Diffuser-3DS-<BUILD>.3dsx` to `sdmc:/3ds/`.
2. Open the Homebrew Launcher (any entry point works under Luma — the Rosalina menu's, or a
   HOME-menu shortcut) and start it from the list.

**`.cia` — installed HOME-menu title** (lets you launch it like any other game):

1. Copy `G-Diffuser-3DS-<BUILD>.cia` anywhere on the SD card.
2. On the console, open **FBI** → browse to the file → **Install CIA**.
3. The game appears on the HOME menu. The vanilla build is named "G-Diffuser"; the Expansion
   Kit build is named "G-Diffuser EK" and shows an "EXPANSION KIT" banner, so they're easy to
   tell apart if you install both. Uninstall later via FBI → **Titles** if you ever want to.

The `.cia` is test-signed, which is standard for homebrew — Luma's signature patches let it
install and run like a normal title. It will not install on a stock (non-CFW) console.

## 6. Expansion Kit: supplying the disk

No prebake step for this one — copy your disk image to the SD card as-is.

The O3DS-EK and N3DS-EK builds need a 64DD F-Zero X disk image to unlock the course editor and
machine creator. You supply your own; nothing in the repository or the download contains one.

Rename your dump to exactly one of these two names and put it at the matching path — the game
looks for both and prefers the English one:

- `sdmc:/3ds/gdiffuser/media/baserom.translated.ek.ndd` — the fan English translation (preferred
  when found)
- `sdmc:/3ds/gdiffuser/media/baserom.jp.ek.ndd` — the Japanese retail disk (the only retail
  version of this disk) — it works out of the box, no setting to change.

**Playing without a disk image works fine.** The Expansion Kit build still boots and plays the
normal game — COURSE EDIT and CREATE MACHINE on the main menu just don't open. The log records
`[leo] no disk image found; Expansion Kit modes stay disabled`, which is expected, not an error,
if you haven't supplied a disk yet.

Either must be the full, untrimmed 64DD image, about 64.9 MB. A smaller file (anything under
60 MB) is rejected outright as truncated — that usually means a bad or partial download/dump.

**Streamed, not loaded into RAM.** The disk is read directly off the SD card as you play, not
copied into memory, so it costs almost no extra RAM on either console.

**First boot is slower.** The very first time a disk image is mounted, the game reads the whole
file once to fingerprint it — budget about a minute. It saves that fingerprint next to your
saves, e.g. `sdmc:/3ds/gdiffuser/saves/baserom.jp.ek.ndd.dcrc` — your disk image's exact
filename with `.dcrc` appended — so every later boot skips the wait. If you ever need to force
a re-check, delete that exact file.

Two sidecar files live next to your saves — they're not the same thing:

| File | What it is | Safe to delete? |
|---|---|---|
| **`.dcrc`** | The one-time disk fingerprint described above. | Yes, any time — the game just re-checks the disk and writes a fresh one on the next boot. |
| **`.gdd`** | Your own Expansion Kit edits: custom courses from COURSE EDIT, machines from CREATE MACHINE, and anything else that writes to the disk during play. The disk image itself (your `.ndd`) is never modified, so this file is where that content actually lives. | No — **back it up along with `saves/`**. Delete it (or lose it) and your custom courses and machines are gone, even though the disk image is untouched. |

## 7. Audio: dumping `dspfirm.cdc` (one-time, every console)

The audio backend needs the console's own DSP firmware, which Nintendo does not ship as a
downloadable file — you dump it from your own console once:

1. Install and launch **DSP1** (homebrew; Universal-Updater, or
   <https://github.com/zoogie/DSP1/releases>).
2. Press **B** ("Dump DSP firmware"). It writes `sdmc:/3ds/dspfirm.cdc`. Exit.

**Symptom without it:** the game boots and plays normally but is completely silent.

## 8. Controls

Slide/drift is N64 **Z** (left) and **R** (right) — N64 **L** is never read in-race. **Boost** is
a fresh N64 **B** press, available after lap 1. **Brake** is N64 **C-DOWN**, held.

| 3DS input | N64 input | Effect |
|---|---|---|
| Circle Pad | Control Stick | steer |
| A | A | accelerate |
| B | B | boost (after lap 1); back out of menus |
| Y | B | same as boost (a duplicate — see `y_maps_to_b` in §10) |
| X | Z | slide/drift left |
| L | Z | slide/drift left |
| R | R | slide/drift right |
| ZL / ZR *(New 3DS)* | B | boost |
| C-Stick *(New 3DS)* | C buttons | camera; C-Stick down also brakes |
| D-Pad | D-Pad | menus; steers too if the IN tab's D-PAD setting is FULL or RAMP |
| START | START | pause / confirm |

Attacks work as on the original N64 game: double-tap L or R (or X) for a side attack, or hold
one of L/R and double-tap the other for a spin attack. Holding L and R together cancels a drift.
ZL/ZR and the C-Stick only exist on New 3DS; on Old 3DS, boost stays on B/Y and slide-left on X.
Old 3DS has no C-Stick, so there is no default **Brake** button at all on that console — open
the IN tab (§9.1) and bind Brake to a button before you play.

**Every one of these is remappable.** The bottom screen's IN tab (§9) lets you tap any game
action and press a new 3DS button for it, independently of the table above.

## 9. The bottom screen

Outside the Homebrew Launcher or FBI menus, the bottom screen is G-Diffuser's own console. It has
two faces:

**During a race — DASH.** A live race readout (lap, position, speed, energy). Tap the small
**MODE** chip, or press **SELECT**, to cycle through three layouts:

1. classic — every readout stacked on the top screen, same as the original game
2. split — some readouts move to the bottom screen
3. all-on-bottom — the whole HUD moves to the bottom screen, top screen stays clean

In 2–4 player races the HUD always stays on the top screen (the bottom screen doesn't have room
to share four players' worth of readouts).

**Outside a race — the idle view.** A checkerboard backdrop with the G-Diffuser logo, so the
bottom screen doesn't sit blank at menus.

Either view has a **DEBUG** button in the bottom-left corner that hands the bottom screen over to
the settings console described below (one of that console's own tabs happens to also be named
**DBG** — don't confuse the two). Tapping the tab bar's **DASH**/**RACE** entry (or holding
**SELECT**) brings DASH/the idle view back.

**The settings console.** A row of tabs across the top:

| Tab | What it's for |
|---|---|
| **STA** | A quick status readout (memory headroom, build id) — nothing to change here. |
| **DSP** | Display (display settings — not to be confused with the dspfirm.cdc DSP firmware from §7): border mode and rival detail — see §9.1. |
| **3D** | Stereoscopic 3D depth, if you use the 3D slider — see §9.1. |
| **AUD** | Master volume. |
| **IN** | Steering feel, D-Pad steering, and button remapping — see §9.1. |
| **LG** | A scrollable copy of the on-device log, handy without pulling the SD card. |
| **AB** | Build / version information. |

(A **DBG** tab with low-level renderer toggles also exists, for diagnosing problems rather than
everyday play — see `DIAGNOSTICS.md` if you're asked to use it.)

### 9.1 Settings worth knowing about

- **Border mode (DSP tab):** four choices — AUTHENTIC (the original's letterboxed borders),
  **FULL-BLEED** (the default; the scene fills the whole 400×240 screen), ZOOM, and HYBRID.
  Full-bleed and hybrid can show a sliver of empty sky at the very edges in some scenes, since
  the original backdrops were painted for the narrower 4:3 view — authentic mode doesn't have
  this, at the cost of the black bars it was always drawn with.
- **Rival detail (DSP tab):** NATIVE / REDUCED / MINIMAL / LOW (NATIVE by default) — simplifies
  distant rival machines for more fps in big fields. An AUTO toggle on the same tab raises the
  cut automatically in a crowded race and relaxes it when the field thins out; whatever you pick
  by hand is treated as the floor it won't go below.
- **Stereoscopic 3D (3D tab):** off by default. There's no console-UI toggle to turn it on —
  set `[stereo] enabled = 1` in `gdiffuser.ini` (§10) and relaunch. (The experimental `n60`
  performance preset, §10, turns this on as a side effect — check it if 3D shows up
  unexpectedly.) Once enabled, on New 3DS, depth (IOD) and convergence become live steppers on
  the 3D tab while the 3D slider is up. Slider down (or `enabled = 0`) renders identically to 2D.
- **Steering feel (IN tab):** circle-pad deadzone, range, and response curve, plus whether the
  D-Pad can steer (off / full / ramps up while held) — all four are live steppers with a pad
  readout so you can see the effect immediately.
- **Button remapping (IN tab):** tap any action in the list, then press the 3DS button you want
  it bound to; a RESET DEFAULTS option is on the same tab.

Everything above except `[stereo] enabled` is saved back to `gdiffuser.ini` automatically — you
never have to hand-edit the file unless you want to set something before first boot, or turn on
3D. See §10 for the ini keys directly.

## 10. `gdiffuser.ini` — the keys worth setting by hand

Lives at `sdmc:/3ds/gdiffuser/gdiffuser.ini`. It's plain text (`#`/`;` start comments, section
and key names aren't case-sensitive), and you can omit it entirely — everything below has a
sensible default. Most of these sync automatically from the console UI (§9.1); the rest — and
anything you want to set before first boot — go here by hand.

```ini
[input]
deadzone = 16      ; circle-pad units ignored around centre
range = 145         ; circle-pad units mapped to full stick deflection
curve = 0           ; 0 linear, 1 soft, 2 softer
dpad_steer = 0      ; 0 off, 1 full (D-pad = full stick), 2 ramp (grows while held)
y_maps_to_b = 1      ; 3DS Y also boosts, alongside B (so A+Y accelerate+boost is comfortable)

[display]
border_mode = 1     ; 0 authentic, 1 full-bleed (default), 2 zoom, 3 hybrid

[audio]
master_volume = 100 ; 0-100

[stereo]
enabled = 0          ; OFF by default; set to 1 and reboot to turn on 3D — not a console-UI toggle
iod = 12             ; depth, 0-40 px; default 12; live on the 3D tab once enabled above
convergence = 0.25   ; 0.00-0.90; default 0.25; live on the 3D tab once enabled above

[perf]
preset = auto        ; auto | o3ds | n60 — see note below
draw_distance = 100  ; percent, 40-100
rival_detail = 0     ; 0 native, 1 reduced, 2 minimal, 3 low (floor for the AUTO toggle)
rival_detail_auto = 1 ; 1 = auto-raise the cut in crowded races (default); 0 = always use rival_detail as a fixed level

[debug]
console = 0          ; 1 = open the settings console on the bottom screen at boot
filelog = 0           ; 1 = mirror the boot/warning/crash log to sdmc:/3ds/gdiffuser/log.txt
filelog_max_kb = 1024 ; log size cap (1-8192); logging stops at the cap with a marker
crashdump = 0        ; 1 = write a crash dump (crash.txt / crash-EK.txt) if the game faults;
                     ; default off — see §13 for bug reports
```

**A note on `[perf] preset`.** `auto` (the default) quietly applies a reduced-detail profile
*only on Old 3DS* — the New 3DS default is the stock, full-detail settings either way. There's
also an experimental `n60` profile for a 60 fps New 3DS race. It hasn't crashed in testing, but a
steady 60 fps isn't confirmed on hardware yet — try it, don't rely on it. `n60` also turns on
stereo 3D (`[stereo] enabled = 1`) as part of its bundled defaults — set `enabled = 0` explicitly
afterward if you don't want that. An explicit `[perf]` key you set yourself always wins over
whatever the preset would have picked.

A few Expansion-Kit-only and diagnostic-only ini keys aren't reproduced here, since normal play
never needs them — see `NATIVE-RENDERER.md` §2 / `DIAGNOSTICS.md` if a bug report ever asks you
to set one.

## 11. Known issues

- A rare, cosmetic speed-readout spike (briefly showing an impossible number, around 826 KPH) can
  appear right at a finish line. It's a display glitch, not a race-affecting bug.
- Short audio gaps can occur during loading transitions.
- The Old 3DS has a firm memory ceiling — some heavy content may be tight on headroom. If
  something crashes only on Old 3DS and not New 3DS, that's the most likely reason.
- Hardware testing has covered the normal range of play, but not every mode on every console
  model yet — if you hit something that looks wrong, see §12.

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Instant exit back to the Homebrew Launcher, or "no archive mounted" | `fzerox.o2r` missing or misplaced (`gdiffuser.o2r` missing alone won't cause this — it only logs a warning) | Check `sdmc:/3ds/gdiffuser/` has both files; re-run the prebake tool (§3) — it refuses to produce a bad archive. |
| Boots but completely silent | no DSP firmware dump, or Luma3DS older than 10.1.1 (see §1) | §7 — DSP1, press B; update Luma3DS if it's old. |
| `.cia` won't install in FBI | stock console, or Luma not installed | Luma3DS is required — §1. |
| EK build's COURSE EDIT / CREATE MACHINE won't open | no Expansion Kit disk image found | §6 — check the path and exact file name; look for `[leo] no disk image found` in the log. |
| Disk image present but still not detected | wrong filename | §6 — rename it to exactly `baserom.jp.ek.ndd` or `baserom.translated.ek.ndd`. |
| First boot of an EK disk is slow (~1 min) | expected — the one-time disk fingerprint pass | Nothing to fix; later boots are fast. |
| EK disk image rejected as truncated | the `.ndd` is incomplete (under 60 MB) | Re-dump/re-download it; it should be about 64.9 MB. |
| Crashes / black screen right after launch | wrong build for the console (ran the New 3DS build on an Old 3DS) | Use the O3DS/O3DS-EK build instead — its memory layout assumes more RAM than an Old 3DS has — §2. |
| No 3D even with the slider up | 3D is off by default and needs an ini edit | Set `[stereo] enabled = 1` in `gdiffuser.ini` (§10) and relaunch — there's no in-game toggle for this one. |
| Blank/frozen screen, or a hang | — | Set `[debug] filelog = 1`, reproduce, then pull `log.txt` (or `log-EK.txt`) — §13. |
| Luma shows its red/pink exception screen | a crash in the game or port code | Photograph the screen (it shows the PC/LR registers), then follow §13 for a bug report. |

## 13. Logs and bug reports

Logs live at `sdmc:/3ds/gdiffuser/log.txt` (vanilla build) or `log-EK.txt` (Expansion Kit
build), and are only written when `[debug] filelog = 1` is set (§10) — set it before
reproducing a problem. The file is capped at `filelog_max_kb` (1024 KB by default) and is
overwritten fresh each boot.

If you're filing a bug report, please include:

1. **Console + firmware**: model (Old 3DS/2DS, New 3DS/New 2DS XL), Luma version.
2. **Which build**: O3DS / N3DS / O3DS-EK / N3DS-EK, and `.3dsx` or `.cia`.
3. **How far it got**: menus, a race, a specific mode — and the last line of `log.txt` if it
   stopped partway.
4. **The whole log file** (`log.txt` or `log-EK.txt`) — it's small enough to attach in full.
5. **A photo of Luma's exception screen**, if one appeared.
6. **A photo or description of anything visually wrong**, if that's the issue.
7. **If you turned on `[debug] crashdump = 1` before the crash**, also attach `crash.txt`/
   `crash-EK.txt` (see §10; full setup, and the Old-3DS caveat if no file appears, are in
   `DIAGNOSTICS.md`).

## 14. Building from source

You don't need this to play — it's only for building the console binary yourself instead of
downloading it. One command, once devkitPro is installed, builds and names the output exactly
like a downloaded release:

```sh
tools/build-3ds.sh <o3ds|n3ds|o3ds-ek|n3ds-ek|all>
```

Full toolchain setup and step-by-step instructions: `docs/BUILDING.md`.
