# Phase 0 / track "vendor" — vendor the LUS carve into `port/3ds/vendor/lus/`

Planning brief. Nothing here is implemented. Master plan: `00-master-plan.md` §1, §4.5, §6, §7.

## 1. Goal

Move the 36-file LUS carve **and its header closure** into `port/3ds/vendor/lus/` with all 38
`lus-*.patch` patches already folded in, so:

- the `libultraship/` submodule is not needed by the 3DS build (it stays for desktop),
- `tools/ci-3ds.sh`'s patch stack shrinks from 48 to the 10 `decomp-*.patch`,
- the reverse+forward patch dance and the stale-object trap disappear,
- the build switches to Ninja + ccache with recorded times.

Gate: **object-level equivalence**, not `.3dsx` sha (paths are compiled in). See §6.

## 2. Current state (evidence)

| Fact | Evidence |
|---|---|
| Carve = 36 explicit sources, no globbing | `port/3ds/gfx/CMakeLists.txt:60-107` |
| Carve root points at the submodule | `port/3ds/gfx/CMakeLists.txt:27`, guard at `:29` |
| CVar macros come from LUS's own cmake | `port/3ds/gfx/CMakeLists.txt:37` (`cmake/cvars.cmake`, 49 lines) |
| Include order `lus_stubs` → `third_party` → `include` → `src` | `port/3ds/gfx/CMakeLists.txt:51-56` |
| Harness duplicates the same root/order | `port/3ds/harness/CMakeLists.txt:16,20,33-37` |
| Consumers (`lus_glue`, `game/gdx3ds_glue`, `assets`) inherit includes transitively | `port/3ds/game/CMakeLists.txt:124-126`, `port/3ds/lus_glue/CMakeLists.txt:35` |
| `port/resource/ResourceFactories.cpp` **is** in the 3DS build | `port/3ds/game/CMakeLists.txt:104`; object exists at `m1/build-3ds/port/3ds/game/CMakeFiles/gdx3ds_glue.dir/__/__/resource/ResourceFactories.o` |
| Top-level already `return()`s for 3DS before the submodule check | `CMakeLists.txt:18-23` vs `:26` — so only `gfx`/`harness` need the submodule |
| ci preflight demands the submodule | `tools/ci-3ds.sh:109` |
| 48-entry PATCH_LIST, 38 of them `libultraship:` | `tools/ci-3ds.sh:123-171` |
| Unknown patch on disk = hard FAIL | `tools/ci-3ds.sh:176-183` — so the 38 files must be **moved**, not merely delisted |
| Generator is Unix Makefiles; no ccache | `native/build-3ds/CMakeCache.txt` `CMAKE_GENERATOR:INTERNAL=Unix Makefiles`; `which ccache` → empty; `ninja` present at `/opt/homebrew/bin/ninja` |
| Upstream pin | `git submodule status` → `7bca0e22ed8fd28f761a8631741c163e6f4b51e5 libultraship`; `.gitmodules` url `Zorkats/libultraship` |
| `m1/libultraship` is at that sha with exactly the 12 patched files dirty | `git -C ~/code/gdx-3ds/m1/libultraship status --porcelain` |

**Header closure (measured, not guessed).** Parsing every `*.o.d` under `m1/build-3ds` (which
covers `gfx`, `game`, `harness`, `lus_glue`, `assets`, `os`) and normalising the
`port/3ds/*/../../../libultraship/` prefix yields **84 header paths, all under
`libultraship/include/`** (46 `include/ship`, 22 `include/fast`, 16 `include/libultraship`;
one is the duplicate `filesystemtools/../StringHelper.h`, so 83 real files, ~14 k lines).
**Zero headers resolve from `libultraship/src/`** — that include dir is vestigial and can go.
The 9 stub headers actually shadowed are `imconfig.h`, `monocypher*.h`, `SDL2/SDL.h`,
`ship/window/gui/Gui.h`, `spdlog/{spdlog,async,fmt/fmt}.h`, `zip.h` (`StormLib.h` is unused).
Carve sources total ~15 k lines / 724 KB.

**Patch fan-out.** The 38 `lus-*.patch` touch 12 files. Eleven are inside the carve or the
closure. The twelfth, `src/libultraship/window/gui/GfxDebuggerWindow.cpp`
(`port/3ds/patches/lus-tmem-span-store.patch:750`), is desktop-only and never compiled on 3DS —
its hunk is deliberately **not** folded in and must be recorded as such.

## 3. Design

### 3.1 Layout

```
port/3ds/vendor/lus/
  LICENSE            verbatim libultraship/LICENSE (MIT)
  PROVENANCE.md      upstream url+sha, 38 patch names in apply order, exclusions, regen cmd
  HEADERS.txt        the 83-path closure list (input to the vendor script)
  MANIFEST.sha256    sha256 of every vendored file
  cmake/cvars.cmake  verbatim
  include/…          83 patched headers (fast/, ship/, libultraship/)
  src/…              36 patched carve sources
  patches/           the 38 lus-*.patch, moved here, frozen, provenance only
```

`port/3ds/gfx/third_party/` (nlohmann, BS_thread_pool, tinyxml2) stays put — not LUS code.

### 3.2 Header-closure method

`tools/vendor-lus.sh` with three modes, committed so it is auditable:

- `--closure <build-dir>` — re-derives `HEADERS.txt` from `<build-dir>/**/*.o.d` exactly as
  measured above (grep `/libultraship/`, strip the `port/3ds/*/../../..` prefix, drop `.cpp`/`.c`,
  `sort -u`).
- *(default)* — checks `libultraship/` is clean at `7bca0e2`, `git apply`s the 38 patches in
  `port/3ds/patches/README.md` order, copies `cmake/cvars.cmake` + the 36 sources (read out of
  `port/3ds/gfx/CMakeLists.txt` so the lists cannot drift) + `HEADERS.txt`, writes
  `MANIFEST.sha256`, then `git checkout -f 7bca0e2` to restore the submodule.
- `--verify` — re-derives into a temp dir and diffs against the committed tree. This is the
  reproducibility receipt; it runs only where the submodule exists (desktop lane / local).

Closure-only (not the whole 206-file `include/` tree) is safe because a missing header is a
*fatal compile error*, never silent breakage; §7 risk 1 covers the `#ifdef`-hidden case.

### 3.3 CMake changes

- `port/3ds/gfx/CMakeLists.txt`: `GDX3DS_LUS_ROOT` → `${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lus`;
  delete the submodule `FATAL_ERROR` (`:29-31`); drop `"${GDX3DS_LUS_ROOT}/src"` from
  `GDX3DS_LUS_INCLUDES`. Source list, `F3DEX_GBI_2`, `-fexceptions -frtti`, `CXX_STANDARD 20`,
  `CXX_EXTENSIONS ON` and list *order* stay byte-identical.
- `port/3ds/harness/CMakeLists.txt:16,33-37`: same two substitutions.
- Nothing else changes: every other 3DS consumer gets includes through
  `gdx3ds_lus_carve`'s PUBLIC dirs.
- Desktop untouched: `CMakeLists.txt:37`, `port/CMakeLists.txt:354,466,666`, `.gitmodules`.

### 3.4 `tools/ci-3ds.sh`

- `:109` — drop the `libultraship/CMakeLists.txt` probe, keep `decomp/Makefile`.
- `:123-171` — remove the 38 `libultraship:` entries; `PATCH_LIST` becomes the 10 decomp ones.
  The 38 patch files move to `port/3ds/vendor/lus/patches/` in the same commit, or the
  unknown-patch guard at `:176` fails.
- `:370` configure — add `-G Ninja` when `command -v ninja`, and
  `-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache` when `command -v ccache`; fall back silently
  otherwise so the devkitpro container works before its `apt-get install -y ninja-build ccache`
  lands. Add `--no-ninja` / `--no-ccache` escape flags.
- Timing: bracket configure, full build and one incremental build (touch
  `port/3ds/vendor/lus/src/fast/interpreter.cpp`) with `SECONDS`; emit
  `[time] configure=… full=… incr=…` plus `ccache -s` deltas into the summary.
- `port/3ds/patches/README.md` shrinks to the 10 decomp patches + a pointer to the vendor tree.

### 3.5 Docs / licensing

`port/3ds/vendor/lus/LICENSE` (verbatim MIT) beside the code; root
`LICENSES/MIT-libultraship.txt` and `THIRD_PARTY_NOTICES.md:13` gain a line for the vendored 3DS
copy. `docs/3DS-HARDWARE.md` §9 steps 2-3 and `README-3DS.md:92` drop libultraship from the
3DS clone/patch instructions (`git submodule update --init decomp fzerox-expansion-kit`).

## 4. Work breakdown (each step ends with its own verification)

0. **Baseline.** Fresh worktree; `tools/ci-3ds.sh` (Makefiles, submodule patched) into
   `build-3ds-base`. Archive `compile_commands.json`, every `.o`, the `.elf`, the `.3dsx`, and an
   Azahar autoinput run's SHOT dumps. *Verify:* ci summary all-PASS; SHOT files non-empty.
1. **`tools/vendor-lus.sh` + `HEADERS.txt`.** No build wiring yet.
   *Verify:* `--closure build-3ds-base` reproduces the committed `HEADERS.txt` byte-for-byte;
   default mode leaves `libultraship/` clean (`git -C libultraship status --porcelain` empty).
2. **Materialise `port/3ds/vendor/lus/`** + LICENSE + PROVENANCE + MANIFEST; move the 38 patches.
   *Verify:* `--verify` passes; `MANIFEST.sha256 -c` passes; every vendored source's sha256 equals
   the corresponding file in `m1/libultraship` (independent second source).
3. **Repoint `gfx` and `harness` CMake.**
   *Verify (G1 flags):* configure `build-3ds-new`; normalise both `compile_commands.json`
   (strip source-root and `-I` roots) and `diff` — the only surviving delta must be the removal of
   `-I…/libultraship/src`.
4. **Preprocessor identity (G2).** For all 36 TUs + `linktest_main.cpp` + `dl_tests_main.cpp`,
   run the recorded command with `-E`, `sed` the two roots out, sha256.
   *Verify:* every hash equals baseline. This is the strongest available proof that the vendored
   sources+headers are the same compiler input.
5. **Codegen identity (G3/G4).** Build `build-3ds-new` with `CCACHE_DISABLE=1`.
   *Verify:* per-object `objcopy -O binary --only-section=.text` sha256 == baseline for all 36;
   `.rodata` deltas, if any, must be explained line-by-line as path strings (`strings` diff);
   `.elf` `.text` size and `nm -C --defined-only` symbol list identical.
6. **Behaviour (G5).** Azahar under the `/tmp/azahar.lock` protocol; `gdx-autoinput.txt`
   (`port/input_bridge.c:320`) driving title → machine select → course select with `SHOT` marks.
   *Verify:* SHOT dumps byte-identical to step 0; `log.txt` identical except the build-id sha
   (`port/3ds/CMakeLists.txt:78`); `tools/ci-3ds.sh --emu` PASS.
7. **`ci-3ds.sh` patch-stack surgery + preflight.**
   *Verify:* run on a checkout whose `libultraship/` is **deinitialised** — must PASS end to end;
   `PATCH_LIST` count == 10; unknown-patch guard still trips if a stray patch is dropped in.
8. **Ninja + ccache + timings.**
   *Verify:* one `-j1` Ninja build succeeds (flushes any missing shader/bin2s dependency), then
   `-j`; G3 re-run on the Ninja objects; record `configure/full/incremental` and `ccache -s`
   hit-rate for cold and warm runs in the progress file.
9. **Docs, notices, README, `patches/README.md`.**
   *Verify:* `grep -rn libultraship` over `port/3ds/`, `tools/ci-3ds.sh`, `docs/3DS-HARDWARE.md`
   returns only intentional prose; desktop `cmake -S . -B build-desktop` still configures.

## 5. Risks

1. **`#ifdef`-hidden includes** absent from the depfile closure (e.g. a Debug-only or
   `ENABLE_OPENGL` path). Fails loudly at compile; fix is one line in `HEADERS.txt`. Low.
2. **Dropping `-I…/libultraship/src`.** Depfiles prove nothing resolves there *in this config*;
   step 3's G1 diff and step 4's G2 make the claim per-TU rather than global. Low.
3. **ccache poisoning the gate.** `__FILE__` and absolute paths change under vendoring, so a
   stale hit would be catastrophic and invisible. Mitigation: every parity build in steps 3-6 runs
   `CCACHE_DISABLE=1`; ccache only lands in step 8, after the gate has passed.
4. **Ninja reorders the build** and exposes a latent missing dependency in
   `ctr_add_shader_library` / `dkp_add_embedded_binary_library`. Caught by the `-j1` pass.
5. **Two sources of truth.** Someone edits `vendor/lus/` while the moved patches rot.
   PROVENANCE must state plainly: from this commit the vendored tree is the source of truth and
   `patches/` is frozen history; `--verify` is expected to fail once the tree is edited, so it
   gets a `PROVENANCE.md`-recorded "verified at commit X" watermark rather than being run in CI.
6. **Container lacks ninja/ccache.** Handled by the `command -v` fallbacks; the release repo's
   workflow gets the `apt-get` line as a follow-up, not a prerequisite.
7. **Repo grows ~1.3 MB.** Accepted.

## 6. Estimate

Steps 0-2: 2 h. Steps 3-5 (the gate): 3 h, dominated by two clean builds. Step 6 (emu): 1-2 h
including lock contention. Steps 7-9: 2 h. **~9 h of agent time**, inside the master plan's
0.5-week Phase 0 budget. Steps 0 and 6 are the only ones needing the emulator.

## 7. First 10 commands

Run from a fresh worktree (`<scratch>/native/bootstrap.sh vendor native/vendor`). Never build in
`~/code/gdx-3ds/native` — a bootstrap build may be running there.

```sh
# 1  own worktree
<scratch> vendor native/vendor
cd ~/code/gdx-3ds/vendor
# 2  confirm the pin and that only the 12 expected files are patched
git submodule status; git -C libultraship status --porcelain
# 3  tools
which ninja ccache || brew install ccache
# 4  BASELINE build (Makefiles, submodule, no ccache) — step 0
export DEVKITPRO=/opt/devkitpro
tools/ci-3ds.sh 2>&1 | tee /tmp/vendor-baseline-ci.log
# 5  freeze the baseline artefacts
mkdir -p /tmp/vendor-art/base && cp build-3ds/compile_commands.json /tmp/vendor-art/base/ \
  && rsync -a --include='*/' --include='*.o' --include='*.elf' --include='*.3dsx' --exclude='*' build-3ds/ /tmp/vendor-art/base/build/
# 6  derive the header closure from the baseline depfiles (the §2 measurement, reproduced)
cat $(find build-3ds -name '*.o.d') | tr ' ' '\n' | grep '/libultraship/' \
  | sed "s|^$PWD/||; s|port/3ds/[a-z_]*/\.\./\.\./\.\./||; s|^libultraship/||" \
  | grep -vE '\.(cpp|c)$|\.o:$' | sort -u | tee /tmp/vendor-art/HEADERS.txt | wc -l   # expect 84
# 7  confirm no header resolves out of libultraship/src
grep -c '^src/' /tmp/vendor-art/HEADERS.txt    # expect 0
# 8  the 12 patch-touched files, and which are outside the carve+closure
grep -h '^+++ ' port/3ds/patches/lus-*.patch | sed 's|^+++ b/||' | sort -u
# 9  the exact carve list the vendor script must read back
sed -n 's|.*"\${GDX3DS_LUS_ROOT}/\(src/[^"]*\)".*|\1|p' port/3ds/gfx/CMakeLists.txt | tee /tmp/vendor-art/SOURCES.txt | wc -l  # expect 36
# 10 start the progress file the protocol requires
printf '# vendor track progress\n\nstate: baseline captured\nnext: write tools/vendor-lus.sh\n' > docs/research/native/vendor-progress.md
git add -A && git commit -m "docs(native): vendor track baseline + closure measurement"
```
