# port/3ds/packaging — SMDH icon, HOME-menu banner, CIA spec

You do not need any of this to install the game — the release already ships
pre-built `.3dsx`/`.cia` files for every console/content combination; this
page is only for building your own CIA from source (see `docs/BUILDING.md`).

Inputs for the `.3dsx` metadata (always built) and the optional installable `.cia`
(target `G-Diffuser-3DS-cia`, gated on makerom + bannertool being findable).

## Files

| File | What | Source |
|---|---|---|
| `icon.png` | 48x48 SMDH icon (HBL list + HOME menu) | `assets/branding/gdiffuser-icon.png` downscaled (ImageMagick) |
| `banner.png` | 256x128 HOME-menu banner image (regular build) | `assets/branding/gdiffuser-logo-menu.png` centered on `#101018` |
| `banner-ek.png` | HOME-menu banner for the **Expansion Kit** build (`GDX_3DS_EXPANSION_KIT=ON`): `banner.png` with the logo raised and an "EXPANSION KIT" caption under it | generated (Pillow, its bundled Aileron font, SIL OFL) |
| `banner.wav` | banner audio: 0.25 s of silence (deliberately quiet) | generated (Python `wave`, 22050 Hz mono s16) |
| `gdiffuser.rsf` | makerom spec; see header comment for the choices | Steveice10 buildtools `template.rsf`, inlined |

Licensing: `icon.png`/`banner.png`/`banner-ek.png` are derivatives of this repository's
own branding art (`assets/branding/`), same license as the repo; nothing
Nintendo-derived is in this directory.

## SMDH metadata (baked into the `.3dsx`, and the `.cia`)

Regular build:

- Short title: `G-Diffuser`
- Long description: `G-Diffuser — F-Zero X decompilation port`
- Publisher: `G-Diffuser Project`

Expansion Kit build (`GDX_3DS_EXPANSION_KIT=ON`): same publisher; short title
becomes `G-Diffuser EK` (from the EK build's default `GDX_3DS_TITLE_TAG=EK`),
long description `G-Diffuser — F-Zero X Expansion Kit (64DD)`, and the
`banner-ek.png` banner above instead of `banner.png` — matching the Title IDs
table below.

Wired in `port/3ds/CMakeLists.txt` via devkitPro's `ctr_generate_smdh`/`ctr_create_3dsx`
(smdhtool + 3dsxtool, both ship with 3ds-tools) — the icon is shared; the
description and banner switch on the CMake option.

## Title IDs

Regular and Expansion Kit builds install as **separate HOME-menu titles**, so
both can be installed side by side (they share one SD folder — ini, archives,
`media/`, `saves/`):

| Build | Title ID | HOME-menu name | Save file |
|---|---|---|---|
| Regular (`GDX_3DS_EXPANSION_KIT=OFF`, the default) | `000400000FF3D500` | G-Diffuser | `saves/fzerox.sav` |
| Expansion Kit (`GDX_3DS_EXPANSION_KIT=ON`) | `000400000FF3D600` | G-Diffuser EK | `saves/fzerox-ek.sav` |

These IDs (and the HOME-menu name) come from `GDX_3DS_CIA_UNIQUE_ID` /
`GDX_3DS_TITLE_TAG`, two CMake cache variables that default to the values
above — or to an EK-flavored default the moment `GDX_3DS_EXPANSION_KIT=ON` is
set.

- Overridable via `GDX_3DS_CIA_UNIQUE_ID` / `GDX_3DS_TITLE_TAG` (e.g. for a
  one-off bisect build installed alongside the regular one, under its own
  title and log file name).
- CMake refuses an Expansion Kit build configured with the regular title ID
  outright, since that would install over the shipped game.
- No separate title ID per console model (Old 3DS vs. New 3DS) — only
  vanilla-vs-Expansion-Kit gets its own title; which memory profile a given
  binary was built for doesn't change what it installs as.

## CIA tools (local, never committed)

`makerom` and `bannertool` are not part of devkitPro's pacman set. Drop them in
`tools/3ds-bin/` (gitignored) or anywhere on `PATH`; CMake finds either location.

- **makerom** (Project_CTR, v0.19.0 tested): grab the platform binary from
  <https://github.com/3DSGuy/Project_CTR/releases> (macOS arm64/x86_64, Linux,
  Windows builds provided) and `chmod +x`.
- **bannertool** (v1.2.3, from the maintained carstene1ns fork — the original
  Steveice10 repo is gone): <https://github.com/carstene1ns/3ds-bannertool>.
  Linux/Windows release binaries exist; on macOS build from source
  (`cmake -S . -B build && cmake --build build`). Note: clang rejects a
  zero-initialized VLA in `source/3ds/lz11.cpp:115`; change `u8 pad[padLength]`
  to `u8 pad[4]` (its maximum) to build.

Then, from a tree configured as in `docs/BUILDING.md`:

```sh
cmake --build build-3ds --target G-Diffuser-3DS-cia
```

produces `build-3ds/port/3ds/G-Diffuser-3DS.cia`. Install with FBI on hardware
(Luma3DS) or `File > Install CIA` / `-i` in an emulator. It installs as
**000400000FF3D500** ("G-Diffuser"), or **000400000FF3D600** ("G-Diffuser EK")
for a tree configured with `GDX_3DS_EXPANSION_KIT=ON` — see Title IDs above.
`tools/build-3ds.sh` (the one-command build for all four console/content
combinations) does this step for you and names the result per target; see
`docs/BUILDING.md`.

`.3dsx` packaging needs no local tools at all — only the `.cia` is gated on
makerom/bannertool being found.
