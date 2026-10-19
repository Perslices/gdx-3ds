# port/3ds/probes/r60probe -- PROBE, NOT PART OF THE GAME BUILD (r60 researchers A + F).
# ARM11 / memory-hierarchy micro-benchmarks for the Old 3DS: C3D per-draw cost vs a raw PICA
# command writer, command-block replay, FCRAM/linear/VRAM latency and bandwidth, I-cache
# footprint, VFP latency / short-vector mode / ARMv6 SIMD, svc + cache-flush costs, CP15
# performance counters (svcControlPerformanceCounter), and the core-1 (syscore) share.
# Then rendered tests (F): FrameEnd/heap-flush cost (FLUSH), raw-block -> C3D resync (RESYNC),
# frame replay by re-issue vs command-buffer copy+patch with CRC (T7), present cadence (T8).
# STANDALONE: not referenced by port/3ds/CMakeLists.txt or tools/ci-3ds.sh. Build:
#   cmake -S port/3ds/probes/r60probe -B <out-of-tree dir> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build <that dir>
# Produces r60probe.3dsx/.elf (hbmenu) and, when makerom + bannertool are found, R60Probe.cia
# (the game's rsf with the identity edited: UniqueId 0xff3db, beside DrawProbe's 0xff3da).
# Output: bottom-screen console + sdmc:/3ds/r60probe/r60probe.txt, one `[r60] ...` line per test.
cmake_minimum_required(VERSION 3.13)
project(r60probe C)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "r60probe needs the devkitPro 3DS toolchain")
endif()

set(PROBE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(PROBE_PACKAGING "${PROBE_REPO}/port/3ds/packaging")
find_program(PROBE_MAKEROM makerom HINTS "${PROBE_REPO}/tools/3ds-bin")
find_program(PROBE_BANNERTOOL bannertool HINTS "${PROBE_REPO}/tools/3ds-bin")

execute_process(COMMAND git -C "${PROBE_REPO}" rev-parse --short HEAD
                OUTPUT_VARIABLE PROBE_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROBE_GIT)
    set(PROBE_GIT "nogit")
endif()
string(TIMESTAMP PROBE_STAMP "%Y%m%d-%H%M%S")

# Sections R..T8 render with DrawProbe's machine shader (read in place, not copied).
ctr_add_shader_library(r60probe_shader "${CMAKE_CURRENT_SOURCE_DIR}/../drawprobe/drawprobe.v.pica")
dkp_add_embedded_binary_library(r60probe_shader_bin r60probe_shader)

add_executable(r60probe r60probe.c)
target_compile_definitions(r60probe PRIVATE R60PROBE_BUILD_ID="${PROBE_GIT}-${PROBE_STAMP}")
# -O2 like the game's hot code (the flags themselves are not under test here).
target_compile_options(r60probe PRIVATE -Wall -O2)
target_link_libraries(r60probe PRIVATE r60probe_shader_bin citro3d ctru m)

set(PROBE_TITLE "G-Diffuser R60Probe")
set(PROBE_SMDH "${CMAKE_CURRENT_BINARY_DIR}/r60probe.smdh")
ctr_generate_smdh(OUTPUT "${PROBE_SMDH}" NAME "${PROBE_TITLE}"
                  DESCRIPTION "ARM11 cost probe (r60)"
                  AUTHOR "G-Diffuser Project" ICON "${PROBE_PACKAGING}/icon.png")
ctr_create_3dsx(r60probe SMDH "${PROBE_SMDH}")

if(PROBE_MAKEROM AND PROBE_BANNERTOOL)
    set(PROBE_BANNER "${CMAKE_CURRENT_BINARY_DIR}/r60probe.bnr")
    add_custom_command(
        OUTPUT "${PROBE_BANNER}"
        COMMAND "${PROBE_BANNERTOOL}" makebanner -i "${PROBE_PACKAGING}/banner.png"
                -a "${PROBE_PACKAGING}/banner.wav" -o "${PROBE_BANNER}"
        DEPENDS "${PROBE_PACKAGING}/banner.png" "${PROBE_PACKAGING}/banner.wav"
        VERBATIM)
    file(READ "${PROBE_PACKAGING}/gdiffuser.rsf" rsf)
    string(REGEX REPLACE "Title( +): G-Diffuser" "Title\\1: ${PROBE_TITLE}" rsf "${rsf}")
    string(REGEX REPLACE "ProductCode( +): CTR-P-GDXF" "ProductCode\\1: CTR-P-R60P" rsf "${rsf}")
    string(REGEX REPLACE "UniqueId( +): 0xff3d5" "UniqueId\\1: 0xff3db" rsf "${rsf}")
    if(NOT rsf MATCHES "UniqueId +: 0xff3db" OR NOT rsf MATCHES "ProductCode +: CTR-P-R60P")
        message(FATAL_ERROR "r60probe: the gdiffuser.rsf identity edits did not apply")
    endif()
    set(PROBE_RSF "${CMAKE_CURRENT_BINARY_DIR}/r60probe.rsf")
    file(WRITE "${PROBE_RSF}" "${rsf}")
    set(PROBE_CIA "${CMAKE_CURRENT_BINARY_DIR}/R60Probe.cia")
    add_custom_command(
        OUTPUT "${PROBE_CIA}"
        COMMAND "${PROBE_MAKEROM}" -f cia -o "${PROBE_CIA}" -target t -exefslogo
                -elf "$<TARGET_FILE:r60probe>" -rsf "${PROBE_RSF}" -icon "${PROBE_SMDH}"
                -banner "${PROBE_BANNER}"
        DEPENDS r60probe "${PROBE_SMDH}" "${PROBE_BANNER}" "${PROBE_RSF}"
        VERBATIM)
    add_custom_target(r60probe-cia ALL DEPENDS "${PROBE_CIA}")
endif()
