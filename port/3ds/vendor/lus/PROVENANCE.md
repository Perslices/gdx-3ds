# port/3ds/vendor/lus — provenance

Vendored copy of the libultraship subset the **3DS** build compiles, with every 3DS
patch already folded in. The `libultraship/` submodule is no longer needed to
configure or build the 3DS target; it stays in the tree for the desktop build only.

## Upstream

| | |
|---|---|
| Repository | `https://github.com/Zorkats/libultraship.git` (`.gitmodules` `submodule.libultraship.url`) |
| Commit | `7bca0e22ed8fd28f761a8631741c163e6f4b51e5` |
| License | MIT — verbatim copy in `LICENSE`, also `LICENSES/MIT-libultraship.txt` at the repo root |

## What is here

| Path | Contents |
|---|---|
| `src/` | the **36** carve sources listed in `port/3ds/gfx/CMakeLists.txt` (no globbing) |
| `include/` | the **83** headers (84 paths — one is the `filesystemtools/../StringHelper.h` alias) the 3DS build actually opens, measured from the baseline build's `*.o.d` depfiles |
| `cmake/cvars.cmake` | verbatim; `include()`d for the `CVAR_*` name macros |
| `HEADERS.txt` | the measured closure list, input to `tools/vendor-lus.sh` |
| `MANIFEST.sha256` | sha256 of every file above (`shasum -a 256 -c MANIFEST.sha256`) |
| `patches/` | the 38 `lus-*.patch`, **frozen** — provenance only, see below |
| `patches/ORDER.txt` | their apply order |

Not vendored, deliberately:

* `libultraship/src/` as an include directory. Zero headers resolved from it in the
  baseline depfiles, so the `-I…/libultraship/src` flag was dropped with the move.
* Everything else under `libultraship/` — ~206 header files and the whole desktop
  runtime. A missing header is a fatal compile error, never silent breakage, so a
  measured closure is safe to carve; if an `#ifdef`-hidden include ever surfaces it
  is one line in `HEADERS.txt` plus a `--regen`.
* `port/3ds/gfx/third_party/` (nlohmann, BS_thread_pool, tinyxml2) — not LUS code,
  stays where it is.

## Patches: folded, and the one exclusion

All 38 `lus-*.patch` in `patches/ORDER.txt` order are **already applied** to the files
here. They touch 12 files; 11 are inside the carve or the closure and are folded in.
The twelfth is **excluded**:

    src/libultraship/window/gui/GfxDebuggerWindow.cpp
      — hunk from patches/lus-tmem-span-store.patch (the `LoadedTextureAt()` accessor
        conversion). Desktop-only file; never compiled on 3DS, not in the carve, not in
        the closure. Its hunk is intentionally NOT folded here. The patch file keeps it
        so the desktop lane can still apply the stack against the submodule.

## Source of truth

From the vendoring commit onward **this tree is the source of truth**. `patches/` is
frozen history: it is no longer applied by `tools/ci-3ds.sh` (whose `PATCH_LIST` is now
the `decomp-*.patch` stack only), and it will rot the moment the vendored sources are
edited. Fix bugs by editing `src/`/`include/` here, not by adding patches.

## Regenerating / auditing

`tools/vendor-lus.sh` (needs the `libultraship/` submodule checked out, so: desktop
lane or a local clone — never the 3DS CI):

```sh
tools/vendor-lus.sh --closure build-3ds   # re-derive HEADERS.txt from depfiles
tools/vendor-lus.sh --regen               # rebuild this tree from the pin + patches
tools/vendor-lus.sh --verify              # regen into a temp dir and diff (receipt)
tools/vendor-lus.sh --manifest            # recompute MANIFEST.sha256
```

`--verify` passed at the vendoring commit. It is expected to fail once anyone edits the
vendored tree — that is the documented consequence of the source-of-truth switch above,
which is why it is a local audit and not a CI gate.

## Equivalence receipt (Phase 0 gate)

Recorded in `docs/research/native/vendor-progress.md`: `compile_commands.json` diff,
per-TU preprocessed-output sha256 for all 36 carve TUs + the two harness mains,
per-object `.text` sha256, `.elf` symbol list, and an Azahar SHOT comparison against
the pre-vendoring build. Every vendored file was additionally cross-checked
byte-for-byte against a second, independently patched checkout of the same pin: 122/122 identical.

## Post-vendoring folds

Patches written against the submodule by tracks that branched before the vendoring, folded
into this tree at merge time (`git apply --directory=port/3ds/vendor/lus`) and kept in
`patches/` for provenance:

- `lus-3ds-native-track-marker.patch` (native/track, 2026-09-13): `GdxTrackMarker` replay hook
  in `src/fast/interpreter.cpp` + `include/fast/interpreter.h`.
- `lus-native-machines-dl-hook.patch` (native/machines, 2026-09-13): weak `gdx3ds_mesh_dl_hook`
  call-out in `gfx_dl_handler_common` (`src/fast/interpreter.cpp`), `__3DS__` only.
- `lus-nverify-diag.patch` (native/interp, 2026-09-13): `gdx3ds_verify_note_cmd/_tri` side channels
  in `src/fast/interpreter.cpp`; folded by 3-way file merge (base = carve at 9b8f332).
  Temporary until native M5. **Edited in the vendored tree 2026-09-23 (native/nverifygate,
  audit finding 5):** the gate is now an inline `gdx3ds_verify_mode != 0` test on the global
  exported from `port/3ds/gfx/gfx_citro3d.cpp`, not a `gdx3ds_verify_active()` call -- there
  is no LTO in the tree, so the call was a real per-command and per-triangle cost on the
  DEFAULT `interp=0` path. The `.patch` file keeps the original text for provenance.
