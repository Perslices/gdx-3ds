#!/usr/bin/env bash
# gdx-refbuild — configure + build the desktop G-Diffuser inside the reference container.
#
# Runs INSIDE the image (see Dockerfile). /src is the repo bind-mounted read-only; /build is a
# named Docker volume so nothing lands in the host tree and build-3ds/ is never touched.
#
#   docker run --rm -v <repo>:/src:ro -v gdx-refbuild:/build gdx-ref gdx-refbuild [--target T] [-j N]
#
# GDX_EXPANSION_KIT is OFF: the 3DS port is cartridge-only (port/3ds/game/CMakeLists.txt sets the
# same option), so the reference must compile the same source set or it is not a reference. It is
# also forced — the Expansion Kit needs a 64DD disk image and IPL ROM that this build has no copy of.
set -euo pipefail

SRC=${GDX_REF_SRC:-/src}
BUILD=${GDX_REF_BUILD:-/build}
TARGET=G-Diffuser
JOBS=$(nproc)
FRESH=0

while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        --fresh)  FRESH=1; shift ;;
        *) echo "gdx-refbuild: unknown argument $1" >&2; exit 2 ;;
    esac
done

if [ "$FRESH" = 1 ]; then
    rm -rf "${BUILD:?}"/*
fi

mkdir -p "$BUILD"

t0=$(date +%s)
cmake -S "$SRC" -B "$BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DGDX_EXPANSION_KIT=OFF
t1=$(date +%s)
cmake --build "$BUILD" --target "$TARGET" --parallel "$JOBS"
t2=$(date +%s)

echo "[gdx-refbuild] configure: $((t1-t0))s  build: $((t2-t1))s  total: $((t2-t0))s  jobs=$JOBS"
find "$BUILD" -maxdepth 3 -name "$TARGET" -type f -perm -u+x -print
