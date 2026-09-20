# 04 — Reference desktop renderer on this Mac (track `refbuild`)

Status: PLAN ONLY (no implementation). Read-only probes + `docker --version` were run.
Basis: master plan §4.3 ("A ground-truth renderer on this Mac"), mainline `m1` at
`~/code/gdx-3ds/m1`.

## 1. Goal

A **desktop G-Diffuser build an agent can run unattended on this Mac**, fed the *same*
`fzerox.o2r` + `gdiffuser.o2r` the 3DS reads and the *same* scripted inputs, producing
320x240 BMPs that `framediff` can compare against the 3DS port's SHOTs. It is the only
oracle that can say "the native renderer is wrong" rather than "the two 3DS builds differ".

## 2. Findings

### 2.1 The capture path already exists and is already 320x240

- `port/n64_gfx_bridge.cpp:8773` — `gFrameMirrorFb = interp->CreateFrameBuffer(320, 240, 320, 240, 1, false)`.
  Every presented frame is GPU-blitted into this mirror (`:8776`).
- `port/n64_gfx_bridge.cpp:8690` `gdx_request_frame_dump(label)` → `:8714`
  `ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, ...)` → `autotest/<label>.bmp`, RGBA16.
  **Identical code and identical dimensions on desktop and 3DS** — the window size only
  affects how much detail survives the downscale, never the output geometry.
- Bulk sequence capture: `GDX_CAPTURE_FRAMES=<start>:<count>` (+ optional `GDX_CAPTURE_MODE=<GET_MODE>`)
  dumps `gdxcap_NNNNN.bmp`, parsed at `port/n64_gfx_bridge.cpp:8535-8548`. Zero cost when unset.
- Driving: `GDX_INPUT_SCRIPT` (`port/gdx_input_script.c:325`), a text script with
  `WAIT / PRESS / STICK / INPUT / WAITMODE / LOG / SHOT / QUIT` (format documented at
  `port/gdx_input_script.c:1-52`). `QUIT` exits cleanly → runs are bounded and headless.
  `input_bridge.c:232,371` is the 3DS-side tick-scheduled twin feeding the same sink.
- Determinism pins already proven by the audio harness: `GDX_RAND_SEED1/2`, `GDX_AUDIO_THREAD=0`
  (`tools/audio_pcm_harness/README.md`). `tools/audio_pcm_harness/run_scenario.py` is the exact
  launcher pattern to copy (JSON scenario → env → cwd=exe dir → wait → collect artifacts).

**Consequence: no engine work is needed for the reference. Only a Linux build and a runner.**

### 2.2 The o2r really is the same file

`tools/prebake/README.md`: the 3DS archives are produced by driving *the desktop
`gdx-extract` with the exact same arguments*, gated on the same golden SHA-256 / entry
count. So `~/Library/Application Support/Azahar/sdmc/3ds/gdiffuser/{fzerox,gdiffuser}.o2r`
(17.2 MB / 92 KB, present) can be copied into the reference run directory verbatim. The
"same o2r" premise in §4.3 holds; it is not an approximation.

### 2.3 Docker

- `docker --version` → **20.10.20** (client), Docker Desktop **4.13.0** (Oct 2022).
- **The daemon is not running**: `Cannot connect to the Docker daemon at unix://~/.docker/run/docker.sock`.
  The socket file exists (last live 2026-08-25), so Docker Desktop has run on this machine.
- No colima, no podman, no Rancher/Orbstack.
- Host: **arm64** (Apple Silicon), macOS 15.7.4, 10 cores, 32 GB RAM, 105 GiB free.
- Docker Desktop 4.13 predates macOS 15 by three OS releases. Treat "it starts" as
  unverified until step 1 of the work breakdown; the mitigation is `brew install colima docker`
  (colima is lighter, scriptable, and has no GUI login) or a Docker Desktop upgrade.

### 2.4 Dependencies (from `README.md:207-215` and `port/CMakeLists.txt`)

CMake 3.24+, Ninja, C++20 compiler, Python 3 **plus PyYAML and Pillow** (the asset
generators import them — cf. commit `a63d042`), SDL2, GLEW, zlib, bzip2, libzip,
nlohmann-json, tinyxml2, spdlog. Runtime adds Mesa (llvmpipe) + Xvfb. Torch is *not*
needed for `G-Diffuser` itself (it is a separate `gdx-extract` ExternalProject,
`CMakeLists.txt:44-48`), and we already have the o2r, so `--target G-Diffuser` avoids it.

### 2.5 arm64 is safe

No `-march`/`-msse` anywhere in the CMake. The only SSE code is `port/rsp/cxd4/vu/vu.h:18`,
gated on `ARCH_MIN_SSE2`, which the build never defines (`port/CMakeLists.txt:85-106`) —
the portable C path is what ships on every host today. Building `linux/arm64` natively is
therefore legitimate *and* arguably closer to the 3DS (ARM FP) than x86-64 would be.

### 2.6 The macOS blocker is ONE root cause, not 1190 independent ones

Reproduced here on a single TU (`clang -fsyntax-only decomp/src/sys/buffers.c`, game defines):

```
decomp/include/PR/os_libc.h:87:   #include <strings.h>        (PORT && !_WIN32)
  -> macOS SDK strings.h:64 includes string.h  [glibc's does NOT]
    -> sys/_types/_ssize_t.h:31  error: typedef redefinition ... vs decomp/include/libc/stdlib.h:21 (typedef int ssize_t)
    -> _string.h:88              error: conflicting types for 'strchr' vs decomp/include/libc/string.h:7
    -> secure/_string.h:234      #define memcpy(...) __memcpy_chk_func(...)  breaks decomp/include/libc/string.h:9
```

6 errors in one file x ~200 decomp TUs ≈ the reported 1190. `decomp/` is a git submodule
(`.gitmodules:7`), so the fix is a patch in the existing patch system (sibling of
`decomp-ilp32.patch`): make `os_libc.h:83` not pull `<strings.h>` on Darwin (declare
`bcopy/bcmp/bzero` with `size_t` there instead), and `#ifndef __APPLE__` the three
`libc/{stdlib,string}.h` declarations. **Bounded — likely under a day.** But the
`bridgecache-progress.md:196-206` note also lists *missing EK asset includes* and
`port/gdx_frame_pacer.c` as separate failures, and macOS SDL2 takes the Metal/GL-4.1-core
path rather than the Linux GL path — so a fixed macOS build would be a *third* renderer
variant, not the reference the release ships.

## 3. Chosen approach

**Docker, `linux/arm64`, Ubuntu 22.04, Xvfb + Mesa llvmpipe, run locally.** Justification:

- Linux is a *supported, released* target (`README.md:130`); its OpenGL/SDL2 Fast3D path is
  the one `harness/EXPECTED.md` means by "reference".
- Native arm64 build on 10 cores: minutes, not the ~10x of qemu x86-64 emulation.
- The o2r and the ROM stay on this disk. Nothing is uploaded and nothing can be committed.
- Latency: a diff cycle is a `docker run`, seconds after the first build.

**Fallback (in order):**
1. If Docker Desktop 4.13 will not start on macOS 15: `brew install colima docker`,
   `colima start --cpu 8 --memory 12 --disk 60`. Same Dockerfile, same commands.
2. If containers are a dead end entirely: the macOS decomp patch of §2.6 — bounded, but
   accept that it is a different GL backend and log that caveat on every diff.
3. **Rejected: GitHub Actions.** The reference run needs `fzerox.o2r` (17 MB of
   ROM-derived data) and the ROM itself. PROTOCOL.md forbids committing either, and an
   encrypted-secret artifact just moves game data to a third party for a job whose
   turnaround is minutes-per-iteration instead of seconds. The release repo's ubuntu-22.04
   job (`.github/workflows/release-3ds.yml:69-75`) stays what it is: an extractor/3DS build.

### 3.1 Geometry / fold matching

`gdiffuser.cfg.json` (path built at `port/main.cpp:666`) is seeded before the run:

| Key | Value | Why |
|---|---|---|
| `Window.Width` / `Window.Height` | 320 / 240 | 1:1 with the mirror FB — no resample in `CopyFrameBuffer`. If SDL/LUS refuses so small a window, use 640x480 (exact 2x) and let framediff box-filter. |
| `Window.Fullscreen.Enabled` | false | headless |
| `gEnhancements.Graphics.Widescreen` | **0** | default is **1** (`port/gdx_menu.cpp:469`) — the single most likely silent geometry mismatch |
| `gEnhancements.Graphics.WidescreenUI` / `UltrawideMode` / `RemoveBorders` / `HideRaceCurtain` | 0 | all change viewport or culling |
| `gInternalResolution` | 1.0 | `port/gdx_menu_registry.cpp:247,263` |
| `gMSAAValue` | 1 | llvmpipe MSAA is slow and changes pixels |
| frame interpolation | off | 3DS renders at the sim rate |

The 3DS's own fold (`docs/research/viewport-progress.md`: out-of-range viewport → folded
rect, 400x240) only fires on *sub-window* viewports (menus/UI), never in-race. So in-race
frames are directly comparable; menu frames need the fold applied to the reference, or a
region mask. Record which is which per scenario.

## 4. Container sketch

```dockerfile
FROM --platform=linux/arm64 ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake ninja-build build-essential pkg-config ca-certificates \
      python3 python3-yaml python3-pil \
      libsdl2-dev libglew-dev zlib1g-dev libbz2-dev libzip-dev \
      nlohmann-json3-dev libtinyxml2-dev libspdlog-dev \
      libgl1-mesa-dri libglu1-mesa xvfb x11-utils \
 && rm -rf /var/lib/apt/lists/*
ENV LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe MESA_GL_VERSION_OVERRIDE=4.1COMPAT
WORKDIR /src
ENTRYPOINT ["/bin/bash","-lc"]
```

Build (source bind-mounted read-only, build dir in a named volume so the host tree stays
clean and `build-3ds/` is never touched):

```sh
docker run --rm -v ~/code/gdx-3ds/native:/src:ro -v gdx-refbuild:/build gdx-ref \
  'cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
   cmake --build /build --target G-Diffuser --parallel 10'
```

Run (`/run` = a host dir holding the exe, the two o2r, the ROM, the seeded cfg, the script):

```sh
docker run --rm -v <run>:/run gdx-ref \
  'cd /run && xvfb-run -s "-screen 0 640x480x24" -a \
     env GDX_INPUT_SCRIPT=/run/script.txt GDX_RAND_SEED1=1 GDX_RAND_SEED2=2 \
         GDX_AUDIO_THREAD=0 SDL_AUDIODRIVER=dummy ./G-Diffuser'
```

Artifacts land in `<run>/autotest/*.bmp` on the host.

## 5. `tools/dev/refrun.py`

Modelled on `tools/audio_pcm_harness/run_scenario.py`. A scenario JSON declares
`{id, script, env, cfg_overrides, shots[], timeout_seconds}`. `refrun.py`:

1. builds the run dir under `<scratch>/refrun/<id>/` (never in the repo): hardlink the
   exe from the volume, symlink the two o2r + ROM from their real locations, write
   `gdiffuser.cfg.json` from the §3.1 table + overrides, write the input script;
2. `docker run` the command above with a hard timeout;
3. asserts every `SHOT <label>` in the script produced `autotest/<label>.bmp`, sha256s them,
   writes `manifest.json` (build sha, o2r sha, script sha, cfg, env, per-shot sha);
4. `--build` / `--rebuild` subcommands; `--emit-3ds-script` converts the script to the
   tick-scheduled `gdx-autoinput.txt` form (`port/input_bridge.c:232`) so both sides get
   provably the same inputs from one source file.

`refrun.py diff <refA> <run3ds>` defers to the framediff tool from master-plan §4.2.

## 6. Work breakdown

| # | Step | Verification |
|---|---|---|
| 1 | Get a daemon up (`open -a Docker`; else colima) | `docker run --rm alpine echo ok` |
| 2 | Write `tools/dev/refbuild/Dockerfile`, build image | `docker run --rm gdx-ref 'cmake --version && pkg-config --modversion sdl2 spdlog'` |
| 3 | GL smoke test | `xvfb-run glxinfo -B` reports llvmpipe with GL >= 4.1 (add `mesa-utils`) |
| 4 | Configure + build `G-Diffuser` | binary exists; record cold/warm build times |
| 5 | Stage a run dir; boot to title with a 2-line script (`WAITMODE 0`, `SHOT title`, `QUIT`) | `autotest/title.bmp` is 320x240 and **not** black |
| 6 | Seed cfg per §3.1; re-shoot | widescreen off visibly changes the image; two runs byte-identical |
| 7 | Determinism gate: same scenario x3 | identical sha256 for every SHOT |
| 8 | `refrun.py` + 3 scenarios (title, course select, one in-race lap) | `manifest.json` complete; re-run reproduces every sha |
| 9 | First real cross-check vs a 3DS SHOT from the same script | a written verdict per shot: match / known-fold / real divergence |
| 10 | Commit; write `refbuild-progress.md` | branch `native/refbuild`; no o2r/ROM in the diff (`git status` clean of them) |

## 7. Risks

- **R1 Docker 4.13 on macOS 15 fails to start.** → colima. Low cost either way.
- **R2 llvmpipe rejects GL 4.1 core** (`gfx_sdl2.cpp:364-367` requests core+forward-compatible).
  llvmpipe advertises 4.5, so this should pass; if the compat override fights it, drop
  `MESA_GL_VERSION_OVERRIDE` and let Mesa negotiate. Fallback: `libgl1-mesa-dri` + EGL
  surfaceless, or swap the LUS backend to SDL2 software.
- **R3 First-boot wizard blocks headless.** `port/main.cpp:640` `FirstBootRun`; ROM +
  o2r present should short-circuit it (`:649`). If it still prompts, script an
  `assets/extracted/generic.o2r` dev-tree layout (`port/main.cpp:503,654`) which is the
  documented skip path.
- **R4 320x240 window refused / HiDPI scaling.** Fall back to 640x480 + 2x box filter.
- **R5 In-race frames are not bit-identical even with pinned seeds** (`bridgecache-progress.md`
  records 3-19 KB control-vs-control on 3DS). Mitigate with menu/preview scenarios for exact
  parity and region-masked perceptual thresholds in-race; the input recorder from §4.2 is the
  real fix and is another track's job.
- **R6 arm64-vs-ARM11 float divergence** masquerading as renderer bugs. If a diff looks like
  1-LSB noise, re-run the same scenario under `--platform linux/amd64` (qemu, slow) — if the
  two hosts agree, the divergence is real.
- **R7 Scope creep into fixing macOS.** Do not start §2.6 unless step 1 fails twice.

## 8. Estimate

Steps 1-5: half a day if Docker cooperates, one day with a colima detour. Steps 6-8: one
day. Step 9 (the first honest cross-check, which is where the surprises live): one day.
**~2.5 days.** The macOS fallback adds ~1 day and produces a weaker oracle.

## 9. First 10 commands

```sh
open -a Docker; sleep 45; docker info | tail -5
docker run --rm alpine echo ok
mkdir -p ~/code/gdx-3ds/refbuild/tools/dev/refbuild     # after bootstrap.sh refbuild native/refbuild
$EDITOR ~/code/gdx-3ds/refbuild/tools/dev/refbuild/Dockerfile   # §4 sketch
docker build --platform linux/arm64 -t gdx-ref ~/code/gdx-3ds/refbuild/tools/dev/refbuild
docker run --rm gdx-ref 'cmake --version; pkg-config --modversion sdl2 glew spdlog libzip'
docker run --rm gdx-ref 'xvfb-run -a glxinfo -B | head'
docker run --rm -v ~/code/gdx-3ds/refbuild:/src:ro -v gdx-refbuild:/build gdx-ref 'cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release'
docker run --rm -v ~/code/gdx-3ds/refbuild:/src:ro -v gdx-refbuild:/build gdx-ref 'cmake --build /build --target G-Diffuser --parallel 10 2>&1 | tail -40'
docker run --rm -v gdx-refbuild:/build -v <scratch>/refrun/t0:/run gdx-ref 'cp /build/port/G-Diffuser /run/ && cd /run && xvfb-run -a env GDX_INPUT_SCRIPT=/run/script.txt ./G-Diffuser; ls -la autotest'
```
