# port/3ds/vendor/citro3d — provenance

Vendored copy of citro3d, so the P3a submission diet (docs/research/native/c5submit-progress.md)
can live inside the library. Built by `CMakeLists.txt` here as `gdx3ds_citro3d`; the game links it
instead of devkitPro's prebuilt `libcitro3d.a` while `GDX_3DS_VENDOR_CITRO3D=ON` (the default).

## Upstream

| | |
|---|---|
| Repository | `https://github.com/devkitPro/citro3d.git` |
| Tag / commit | `v1.7.1` / `9f21cf7` ("libctru 1.7.1") |
| Installed package it replaces | devkitPro `citro3d 1.7.1-2` (`$DEVKITPRO/libctru/lib/libcitro3d.a`) |
| License | zlib — verbatim copy in `LICENSE` |

`include/` is byte-identical to the headers the package installed under
`$DEVKITPRO/libctru/include` (`diff -r` clean at vendoring time), so game code that includes
`<citro3d.h>` from the toolchain sees the same declarations the library is compiled against.
The source file list is the prebuilt archive's member list (50 objects, `ar t`).

## Build flags

Those of the prebuilt archive, read from its `DW_AT_producer`:
`-mword-relocations -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm -march=armv6k -g -O2
-ffunction-sections -fdata-sections -fomit-frame-pointer -fno-math-errno`, with the Makefile's
`-DNDEBUG=1 -D__3DS__ -DCITRO3D_BUILD` and gnu17. The one difference is the compiler: the package
was built with gcc 13.2.0; the vendored copy is built by the toolchain in use (gcc 16.1.0 at
vendoring time), the same compiler as the rest of the game.

## Local changes

Every change is marked `GDX (c5submit)` in the source. The commit that vendored the tree left it
verbatim; later commits are the diet (see c5submit-progress.md for the list).
