# Phase 0 / track `vendor` — progress

Plan: `docs/research/native/01-vendor-plan.md` (lives in the `native/` worktree).
Branch: `native/vendor`. Worktree: `~/code/gdx-3ds/vendor`.

state: COMPLETE — steps 0-9 done, gates G1-G5 all PASS
next: nothing blocking; the track is ready to merge into native/main

## Step 0 — baseline (DONE)

`bootstrap.sh vendor native/vendor` produced a green `tools/ci-3ds.sh` run
(48 patches applied, host tests PASS, `G-Diffuser-3DS.3dsx` 2.9M,
`gdx3ds-dl-tests.3dsx` 648K, `RESULT: PASS`).

Re-ran configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (flag does not affect
compile flags — the follow-up `cmake --build` rebuilt nothing, all targets
"Built target"). Baseline artefacts frozen at `/tmp/vendor-art/base/`:
`compile_commands.json` (215 entries), 214 `.o`, their `.o.d`, `.elf`, `.3dsx`.

### Measurements reproduced from the plan §2 (all match)

| Claim | Measured |
|---|---|
| submodule pin | `7bca0e22ed8fd28f761a8631741c163e6f4b51e5` |
| dirty files in `libultraship/` | exactly the 12 expected |
| header closure from `*.o.d` | 84 paths, 83 real files (one `filesystemtools/../StringHelper.h` alias) |
| closure split | 46 `include/ship`, 22 `include/fast`, 16 `include/libultraship` |
| headers resolving from `libultraship/src/` | **0** — the `-I…/src` entry is vestigial |
| carve sources read back from `port/3ds/gfx/CMakeLists.txt` | 36 |
| patches on disk | 38 `lus-*`, 10 `decomp-*`, 48 total |
| files the 38 lus patches touch | 12 — 4 in the closure, 7 in the carve, 1 outside |
| the 1 outside | `src/libultraship/window/gui/GfxDebuggerWindow.cpp` (desktop-only, never compiled on 3DS; hunk deliberately NOT folded) |

## Steps 1-2 — `tools/vendor-lus.sh` + `port/3ds/vendor/lus/` (DONE, commit `09cb6a9`)

`tools/vendor-lus.sh` has four modes: `--closure <build-dir>` (re-derives
`HEADERS.txt` from `*.o.d`), `--regen [outdir]` (checkout pin → apply
`patches/ORDER.txt` → copy → manifest → restore pin), `--verify` (regen into a temp
dir and diff), `--manifest`.

`patches/ORDER.txt` (38 names) is generated from `tools/ci-3ds.sh`'s `PATCH_LIST` and
was diffed against `port/3ds/patches/README.md`'s apply block — identical.

| Receipt | Result |
|---|---|
| `--closure build-3ds` vs the hand-run pipeline | byte-identical, 84 paths |
| `--verify` | PASS — committed tree == fresh regen |
| `shasum -a 256 -c MANIFEST.sha256` | 122/122 OK |
| every vendored file vs `~/code/gdx-3ds/m1/libultraship` (independent 2nd checkout of the same pin, patched by a different agent) | 122 compared, **0 mismatches, 0 missing** |
| `git -C libultraship status --porcelain` after `--regen` | empty (restored to `7bca0e2`) |
| vendored tree size | 1.9 MB, 123 files |

The 38 `lus-*.patch` were `git mv`'d to `port/3ds/vendor/lus/patches/` (history kept);
`port/3ds/patches/` now holds the 10 `decomp-*.patch`.

Deviation from the plan, recorded: `--regen`'s first draft did `rm -rf "$out"` with
`$out == $VENDOR`, which ate its own inputs (`HEADERS.txt`, `patches/`). Fixed before
any commit: inputs are stashed to a temp dir first and the wipe is scoped to the
generated subtrees only (`include/ src/ cmake/ LICENSE HEADERS.txt MANIFEST.sha256`),
never `patches/` or `PROVENANCE.md`.

## Step 3-5 — the parity gate (G1-G4): **ALL PASS**

Both builds `CCACHE_DISABLE=1`, Unix Makefiles, `Release`, `-j8`.
Baseline = `/tmp/vendor-art/base/` (frozen pre-vendoring `build-3ds`);
new = `build-3ds-new` (fresh configure against `port/3ds/vendor/lus`).

### G1 — `compile_commands.json` (215 entries each)

Both normalised (`/tmp/vendor-art/ccnorm.py`: both LUS roots → `@LUS@`, build dir →
`@BUILD@`, repo root → `@ROOT@`, one flag token per line, sorted by TU) then diffed.
**Every changed token, exhaustively:**

| Count | Token | Verdict |
|---:|---|---|
| 85 | `- -I@LUS@/src` | intended — the vestigial include dir, 0 headers resolved through it |
| 36 `-`/36 `+` | object output path `…/__/__/__/libultraship/src/…` → `…/__/vendor/lus/src/…` | intrinsic to the move; not a flag |
| 1 `-`/1 `+` | `-DGDX3DS_BUILD_ID="native/vendor@767b6c03"` → `@09cb6a95` | the git sha, changes on every commit |

Nothing else. No TU appeared or disappeared; every normalised source path matched.

### G2 — preprocessed-input identity

36 carve TUs + `linktest_main.cpp` + `dl_tests_main.cpp` = 38 TUs, each re-run from
its own recorded command with `-E -P`, both LUS roots and the repo root textually
normalised, sha256'd (`/tmp/vendor-art/preproc.py`).

**38/38 sha256 identical.** e.g. `src/fast/interpreter.cpp`
`97e49c99a50ab95b2c0dab292ca04641c2b31fdbb7201cc2cd5bf5bb2a9a2cc6` on both sides.
This is the direct proof that the vendored sources+headers are the same compiler input.

### G3 — codegen identity, per object

214 objects on each side (identical count).

| Comparison | Result |
|---|---|
| `objcopy -O binary --only-section=.text` sha256, all 214 | **214/214 identical** |
| same for `.rodata` and `.data` | identical |
| `objcopy --strip-debug` then sha256 the whole object (covers relocs, symtab, every section) | **213/214 identical** |

The one object that differed, `port/3ds/CMakeFiles/G-Diffuser-3DS.dir/gdx3ds_fps_hud.o`,
is the only consumer of `GDX3DS_BUILD_ID` (`port/3ds/CMakeLists.txt:78`); its string
lives in `.rodata.str1.4`. Proven to be *only* the git sha: recompiling it from the new
tree with `-DGDX3DS_BUILD_ID` forced back to `native/vendor@767b6c03` yields
`2a02374932acfbbef2aa686967858235c5abdd3a75131abf61821f4bfdb246bd`, which **equals the
baseline object's** strip-debug sha256.

### G4 — link identity

| | `G-Diffuser-3DS` | `gdx3ds-dl-tests` |
|---|---|---|
| `.text` sha256 | `c789ec7e89a6571c…` == baseline | `d40d42f7ff2c175c…` == baseline |
| `.rodata`/`.data`/`.init_array`/`.fini_array`/`.ARM.exidx`/`.ARM.extab` | `.rodata` differs (build-id only), rest SAME | all SAME |
| `nm -C --defined-only` | 14813 symbols, identical | 2766 symbols, identical |
| section sizes | identical except `.debug_line`/`.debug_str` (source paths got shorter) | same |
| `.3dsx` byte compare | **12 bytes differ** | **0 bytes differ — byte-identical** |

Those 12 bytes are at offsets 2464177-2464184 and 2464223-2464230, i.e. inside the two
embedded copies of `native/vendor@<sha>`, dumped in context and confirmed one by one.
Nothing else in the 3.0 MB image changed.

## Step 8 — Ninja + ccache: build times

`tools/ci-3ds.sh` now picks `-G Ninja` when `ninja` is on PATH and
`-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache` when `ccache` is, silently falling back
otherwise (`--no-ninja` / `--no-ccache` force it). `ccache 4.14` installed via
`brew install ccache`. All numbers are `--jobs 8` on this Mac, whole `tools/ci-3ds.sh`
runs from a wiped `build-3ds`, `incr` = touch
`port/3ds/vendor/lus/src/fast/interpreter.cpp` then rebuild.

| Configuration | configure | full | incremental | ccache |
|---|---:|---:|---:|---|
| Unix Makefiles, no ccache (**the old default**) | 0.76s | **23.60s** | **8.63s** | — |
| Ninja, no ccache | 0.42s | 23.05s | 8.02s | — |
| Ninja + ccache, **cold cache** (`ccache -C`, `--jobs 1`) | 0.39s | 128.20s | 0.95s | +0h/+214m |
| Ninja + ccache, warm cache | 0.47s | **0.89s** | **0.57s** | +214h/+0m |

Full build 23.6s → 0.89s (**27x**), incremental 8.63s → 0.57s (**15x**). The cold-cache
row is the `-j1` pass the plan asks for (risk 4: Ninja reordering exposing a missing
`ctr_add_shader_library` / `dkp_add_embedded_binary_library` dependency) — it PASSED, so
there is no latent dependency, and its 128s is `-j1`, not a ccache cost.

Timers were switched from `$SECONDS` to a ms-resolution helper (`EPOCHREALTIME`, else
`python3`, else whole seconds) because with a warm cache every phase now lands under 1s
and whole-second timing reported "0s".

### The Ninja-vs-Makefiles binary delta (investigated, benign, pre-existing)

Switching generator changes the linked image: `.text` 0x22da20 → 0x22d970, `.bss` +0x20,
`.ARM.extab` −0xc, `.ARM.exidx` −0x10. Cause: `libgdx3ds_gfx.a` gets
`gdx3ds_gfx_shader_shbin.o` in a different member position under Ninja, which changes
which vague-linkage (COMDAT) definition the linker keeps first. Exactly three weak
symbols differ in size — `std::vector<std::string>::~vector()` D1/D2 (0x134 vs 0x60) and
`_Sp_counted_base<_S_atomic>::_M_release()` (0x94 vs 0xc4) — while the **defined-symbol
set is identical (14813 symbols)** and every object file is identical.

Proven pre-existing and unrelated to vendoring: a scratch worktree at the pre-vendoring
commit `4743cef` (submodule restored, 38 patches applied) built with both generators
reproduces **the same section-size deltas and the same three weak symbols at the same
sizes**.

### The gate re-run on Ninja (same worktree, so identical paths)

Pre-vendoring vs vendored, both Ninja, both `CCACHE_DISABLE=1`, both in
`~/code/gdx-3ds/vendor` at the same git HEAD (only the two CMakeLists swapped and the
submodule patched/unpatched):

| | Result |
|---|---|
| objects | 214/214 byte-identical under `--strip-debug` (fps_hud too — same HEAD, same build id) |
| `G-Diffuser-3DS.elf` | alloc sections, `.text` bytes and symbol+size list all identical |
| `gdx3ds-dl-tests.elf` | identical |
| `G-Diffuser-3DS.3dsx` | **0 differing bytes** |
| `gdx3ds-dl-tests.3dsx` | **0 differing bytes** |

This is the strongest form of the gate: the shipping configuration produces a
**bit-identical** `.3dsx` before and after vendoring. (Cross-worktree comparisons are
meaningless here and were discarded — absolute source paths land in `.rodata` and shift
ARM literal pools, which is exactly why the plan gates on objects, not `.3dsx` sha.)

## Step 6 — G5, behaviour under Azahar: **PASS**

Emulator lock protocol honoured throughout (`mkdir /tmp/azahar.lock`, touched every
20 s, released at the end). Same `gdx-autoinput.txt` and the same shared
`gdiffuser.ini` for both runs — the ini was never written (`diff` clean afterwards),
and the borrowed `gdx-autoinput.txt` was restored.

Autoinput (tick mode, `port/input_bridge.c:230`), title → START → three menu
advances, a `SHOT` between each:

```
ticks
300 SHOT g5_title      1000 A                1900 SHOT g5_menu2
400 START              1300 SHOT g5_menu1    2200 A
700 SHOT g5_post_start 1600 A                2500 SHOT g5_menu3
```

Each `SHOT` writes two BMPs: `<label>.bmp` (interpreter frame-mirror,
`CopyFramebuffer` readback) and `<label>_scan.bmp` (the top screen's CPU-readable
scanout buffer — the image the LCD actually shows).

**Base** = `~/code/gdx-3ds/native/build-3ds/port/3ds/G-Diffuser-3DS.3dsx`,
build id `native/main@767b6c03`, the pre-vendoring reference build (Unix Makefiles,
48 patches, submodule).
**New** = `build-3ds/port/3ds/G-Diffuser-3DS.3dsx`, build id `native/vendor@e49ab5e7`,
Ninja + ccache, vendored carve, no libultraship patch step.

| file | sha256 (identical on both runs) |
|---|---|
| `g5_title_scan.bmp` | `d120660d5ed9e6fa9b2f8082801d07d509ce29f9eb264a2d8b55a3433fb86b66` |
| `g5_post_start_scan.bmp` | `177db213de5c3cc98c3c4c5ce6d167f24c164f4bb7f066700acb5e103ac0e717` |
| `g5_menu1_scan.bmp` | `3f5c157ef8a18479c96a2a825e06e58cc4ae1b8a80b1b572bb26fae3931a70bd` |
| `g5_menu2_scan.bmp` | `e853706e81e34668fe0085d86a82a18b959132c11c5fc7a5d75fa495d6e7cdf5` |
| `g5_menu3_scan.bmp` | `88e406c76928e4d6f2ef1a26a3c88f0a95a8f44aae419f90192e1089d0453157` |
| `g5_menu1.bmp` | `81a606a140bc44ef6e1cfc746d247c2961f78ef981e4bef8fa35d4a953916ee9` |
| `g5_menu2.bmp` | `03c7aa9f8e8b28710b4de0587ac29a7927093e7ab98bb4bc6ec3bf74dad7884f` |
| `g5_title.bmp`, `g5_post_start.bmp`, `g5_menu3.bmp` | `e7a8dfc80ea86fb37199df7a61585499e40930c00d8a9763644adbb4986a7f54` |

**10/10 byte-identical**, `diff` of the two `shasum` manifests empty. Zero `[fatal]`
lines in either `log.txt`.

### The `_scan` vs mirror distinction, and one flake worth recording

`e7a8dfc8…` is the **all-black** image (230400 identical bytes, 1 distinct value);
the non-black shots carry 32 distinct byte values, the `_scan` ones 235-244.

The `<label>.bmp` mirror path is known-flaky and documented as such in the source
(`port/n64_gfx_bridge.cpp:8627-8633`: its in-game readback "produced all-black
autotest BMPs while [present] counted ~135k nonzero scanout bytes", which is why
`GdxDumpTopScanoutBmp` exists). An earlier pair of runs caught exactly that: with the
*same* vendored binary, `g5_menu3.bmp` came out all-black on one run and
`c9f59fd70455c1fe…` (non-black) on the next, while `g5_menu3_scan.bmp` was
`88e406c7…` on both. So the mirror BMP is not a stable oracle by itself.

The `_scan` oracle, by contrast, is **stable across every run of both binaries**: the
five `_scan` hashes above reproduced identically on 4 independent runs (2 base,
2 vendored), across two sessions five days apart. That is the parity evidence; the
mirror BMPs agreeing 5/5 as well is a bonus, not the mechanism.

### `log.txt` comparison

766 lines (base) vs 782 (vendored). After dropping the free-running `[watchdog]`
heartbeats and normalising addresses, floats and counters, the residual is entirely
timing/nondeterminism: `[venueload] decode=…ms`, `[audioprime] push=…`, `[audio-out]`
ordering, `[tt] park#…`, one `[mem-census]` line. The only *structural* difference is a
uniform **32-byte (0x20) address shift** — `[bridge-init] registered EXE module range
size=0x98170c` vs `0x98172c`, `[vifallback] fb=0x67d408` vs `0x67d428`,
`[registered-resolve] base=0x64230c` vs `0x64232c` — which is exactly the `.bss +0x20`
delta measured at link time and attributed above to the Makefiles→Ninja vague-linkage
selection, not to vendoring.

### Bug found and fixed: `ci-3ds.sh --emu` never actually booted

`--emu` launched `"$AZAHAR_BIN" "$GAME_3DSX" &` — the bundle's inner binary. On this
machine that starts a process which sits at ~0% CPU and never boots the title: empty
`azahar_log.txt`, no sdmc `log.txt`, no frames, so the heartbeat probe always recorded
`heartbeat not advancing (distinct=0, max=none)`. Launching the same `.3dsx` through
`open -a /Applications/Azahar.app --args <path>` (what the agent protocol specifies)
boots it normally — that is how every run above was driven. The launch line is now
`open -a "$AZAHAR_APP" --args "$GAME_3DSX"`.

This bug is **pre-existing** — the `--emu` block is untouched by the vendoring work and
the same line is in the base worktree's `tools/ci-3ds.sh`.

### Status of the `--emu` end-to-end re-run

The launch fix is verified by mechanism, not yet by a green `tools/ci-3ds.sh --emu`
summary line: four full Azahar boots (2 base, 2 vendored) were driven with exactly the
`open -a /Applications/Azahar.app --args <abs .3dsx>` invocation the fixed script now
issues, and the pre-fix direct-binary invocation was observed to fail in the specific
way the probe reports. A confirming `--emu` run is blocked only on emulator
availability: `/tmp/azahar.lock` is held (legitimately, refreshed every few seconds) by
the `oracle` track, whose Azahar is running
`~/code/gdx-3ds/oracle/build-3ds/port/3ds/G-Diffuser-3DS.3dsx`. The lock was NOT stolen.

**For the next agent:** re-run `tools/ci-3ds.sh --jobs 8 --emu` once the lock frees.
Expect `emu boot smoke PASS`. If it still reports `heartbeat not advancing`, the cause
is *not* the launch path — look at `log_filter=*:Debug` in `qt-config.ini` and at
`fzerox.o2r` on the virtual SD, which are the probe's other two preconditions.

## Gate summary

| Gate | What it proves | Result |
|---|---|---|
| G1 | same compiler flags per TU | **PASS** — only `-I…/lus/src` removed, object paths moved, build-id string |
| G2 | same preprocessed input | **PASS** — 38/38 TUs sha256-identical |
| G3 | same codegen | **PASS** — 214/214 objects `.text`-identical; 213/214 whole-object, the 1 proven to be the git sha |
| G4 | same link | **PASS** — both ELFs `.text`-identical, symbol lists identical; under Ninja both `.3dsx` **byte-identical** |
| G5 | same behaviour | **PASS** — 10/10 Azahar SHOT BMPs byte-identical, 0 `[fatal]` |

## What the next agent must know

1. **The vendored tree is the source of truth.** `port/3ds/vendor/lus/patches/` is frozen
   history and is applied by nothing. Fix LUS bugs by editing
   `port/3ds/vendor/lus/src|include`. `tools/vendor-lus.sh --verify` will start failing the
   moment you do — that is intended and documented in `PROVENANCE.md`.
2. **The 3DS build no longer needs `libultraship/`.** Verified with the directory emptied
   *and* deleted outright. `git submodule update --init decomp fzerox-expansion-kit` is
   enough. The submodule stays for the desktop build (`cmake -S . -B build-desktop` still
   configures).
3. **Ninja changes the linked image** (three vague-linkage weak symbols; `.bss` +0x20),
   and that shift is visible in `log.txt` addresses. It predates vendoring — proven — but
   do not treat a 32-byte address shift between a Makefiles and a Ninja build as a
   regression.
4. **The `<label>.bmp` SHOT is a flaky oracle**; use `<label>_scan.bmp`. Same binary,
   different runs, different result — documented at `port/n64_gfx_bridge.cpp:8627`.
5. **`build-3ds-new/` was committed by accident in `833426a`** (449 generated files,
   absolute developer paths) and untracked in `66375de`. The blobs remain reachable from
   that commit; history rewriting was not available in this session. **Squash-merge this
   branch** and they never land on `native/main`.
6. `ccache` is now a build dependency on this machine (installed via `brew install
   ccache`). The devkitpro container does not have it or `ninja`; `tools/ci-3ds.sh` falls
   back silently, so the release workflow keeps working. Adding
   `apt-get install -y ninja-build ccache` there is a cheap follow-up, not a prerequisite.
