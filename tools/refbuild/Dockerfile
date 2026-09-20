# G-Diffuser reference desktop renderer (track: refbuild).
#
# A linux/arm64 image that builds and runs the *desktop* G-Diffuser binary headlessly under
# Xvfb + Mesa llvmpipe, so an agent can produce 320x240 autotest BMPs from the same o2r and the
# same scripted inputs the 3DS port reads. See docs/research/native/04-refbuild-plan.md.
#
# Build (from the repo root):
#   docker build --platform linux/arm64 -t gdx-ref tools/refbuild
#
# The dependency set, the pinned versions and the "build them from source" decision are copied
# from .github/workflows/release-linux.yml, which is what actually ships. If the reference
# renderer were built against jammy's own SDL2/spdlog/libzip it would be a fourth renderer
# variant rather than the released one. Deltas from that workflow, all deliberate:
#   * aarch64 tarball/toolchain (the host is Apple Silicon; a native build is minutes, qemu
#     x86-64 would be ~10x). Nothing in the CMake sets -march/-msse — the only SSE code,
#     port/rsp/cxd4/vu/vu.h, is gated on ARCH_MIN_SSE2 which the build never defines.
#   * Mesa llvmpipe + Xvfb + xauth added: the workflow only builds, this image also RUNS.
#   * libzip keeps BUILD_TOOLS=OFF as in the workflow; the imported-target errors that
#     jammy's libzip-dev config throws (zipcmp/zipmerge/ziptool) do not arise from a
#     self-built one.
FROM --platform=linux/arm64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Pinned exactly as release-linux.yml pins them.
ARG CMAKE_VERSION=3.31.6
ARG SDL2_VERSION=2.30.9
ARG TINYXML2_VERSION=10.0.0
ARG SPDLOG_VERSION=1.14.1
ARG LIBZIP_VERSION=1.10.1

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-12 g++-12 ninja-build pkg-config \
      git ca-certificates wget \
      python3 python3-yaml python3-pil \
      zlib1g-dev libbz2-dev nlohmann-json3-dev libpng-dev \
      libgl1-mesa-dev libopengl-dev libegl1-mesa-dev libglew-dev \
      libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
      libxinerama-dev libxss-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdrm-dev libgbm-dev \
      libgl1-mesa-dri libglu1-mesa mesa-utils xvfb x11-utils xauth \
 && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-12 CXX=g++-12

# Jammy ships CMake 3.22; libultraship/CMakeLists.txt demands 3.24.
RUN wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz" \
 && mkdir -p /opt/cmake \
 && tar -xzf "cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz" -C /opt/cmake --strip-components=1 \
 && rm -f "cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz"
ENV PATH=/opt/cmake/bin:$PATH

RUN set -eux; cd /tmp; \
    wget -q "https://www.libsdl.org/release/SDL2-${SDL2_VERSION}.tar.gz"; \
    tar -xzf "SDL2-${SDL2_VERSION}.tar.gz"; \
    cmake -S "SDL2-${SDL2_VERSION}" -B build-sdl2 -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF; \
    cmake --build build-sdl2 --target install; \
    \
    wget -q -O tinyxml2.tar.gz "https://github.com/leethomason/tinyxml2/archive/refs/tags/${TINYXML2_VERSION}.tar.gz"; \
    tar -xzf tinyxml2.tar.gz; \
    cmake -S "tinyxml2-${TINYXML2_VERSION}" -B build-tinyxml2 -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -Dtinyxml2_BUILD_TESTING=OFF; \
    cmake --build build-tinyxml2 --target install; \
    \
    wget -q -O spdlog.tar.gz "https://github.com/gabime/spdlog/archive/refs/tags/v${SPDLOG_VERSION}.tar.gz"; \
    tar -xzf spdlog.tar.gz; \
    cmake -S "spdlog-${SPDLOG_VERSION}" -B build-spdlog -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DSPDLOG_BUILD_SHARED=OFF \
      -DSPDLOG_FMT_EXTERNAL=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF; \
    cmake --build build-spdlog --target install; \
    \
    wget -q -O libzip.tar.gz "https://github.com/nih-at/libzip/archive/refs/tags/v${LIBZIP_VERSION}.tar.gz"; \
    tar -xzf libzip.tar.gz; \
    cmake -S "libzip-${LIBZIP_VERSION}" -B build-libzip -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_OPENSSL=OFF \
      -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF; \
    cmake --build build-libzip --target install; \
    ldconfig; \
    rm -rf /tmp/*

# No GPU is exposed to the container; llvmpipe is the only renderer that will honour the
# core-profile context gfx_sdl2.cpp asks for (it advertises GL 4.5 core under Xvfb).
ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    SDL_AUDIODRIVER=dummy \
    XDG_RUNTIME_DIR=/tmp/xdg
RUN mkdir -p /tmp/xdg && chmod 700 /tmp/xdg

COPY build.sh /usr/local/bin/gdx-refbuild
COPY run.sh   /usr/local/bin/gdx-refrun
RUN chmod +x /usr/local/bin/gdx-refbuild /usr/local/bin/gdx-refrun

WORKDIR /src
ENTRYPOINT ["/bin/bash","-lc"]
