# port/3ds/probes/drawprobe -- PROBE, NOT PART OF THE GAME BUILD (o3ds60 design input).
# A citro3d submission micro-benchmark for the Old 3DS: draw-call overhead, uniform upload,
# state changes, PICA vertex/fill throughput, CPU matrix math, streaming VBO writes.
# STANDALONE project: not referenced by port/3ds/CMakeLists.txt or tools/ci-3ds.sh. Build:
#   cmake -S port/3ds/probes/drawprobe -B <out-of-tree dir> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#   cmake --build <that dir>
# Produces:
#   DrawProbe.cia       000400000FF3DA00  the game's gdiffuser.rsf (SystemMode 64MB etc.), with
#                                         title/product code/UniqueId edited; installs next to
#                                         the game and next to HomeProbe (0xff3d6/0xff3d7)
#   drawprobe.3dsx/.elf                   the same program, for hbmenu
#   drawprobe-lane.3dsx                   EMULATOR ONLY: also mirrors its lines plus a
#                                         [watchdog] heartbeat into sdmc:/3ds/gdiffuser/log.txt
#                                         so `gdx-dev retro run` sees it boot, and quits by
#                                         itself when done. Never ship it on a card.
cmake_minimum_required(VERSION 3.13)
project(drawprobe C)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "drawprobe needs the devkitPro 3DS toolchain")
endif()

set(PROBE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(PROBE_PACKAGING "${PROBE_REPO}/port/3ds/packaging")
find_program(PROBE_MAKEROM makerom HINTS "${PROBE_REPO}/tools/3ds-bin" REQUIRED)
find_program(PROBE_BANNERTOOL bannertool HINTS "${PROBE_REPO}/tools/3ds-bin" REQUIRED)

execute_process(COMMAND git -C "${PROBE_REPO}" rev-parse --short HEAD
                OUTPUT_VARIABLE PROBE_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROBE_GIT)
    set(PROBE_GIT "nogit")
endif()
string(TIMESTAMP PROBE_STAMP "%Y%m%d-%H%M%S")
set(PROBE_BUILD_ID "${PROBE_GIT}-${PROBE_STAMP}")

ctr_add_shader_library(drawprobe_shader drawprobe.v.pica)
dkp_add_embedded_binary_library(drawprobe_shader_bin drawprobe_shader)

function(add_drawprobe_elf elf laneLog)
    add_executable(${elf} drawprobe.c)
    target_compile_definitions(${elf} PRIVATE DRAWPROBE_BUILD_ID="${PROBE_BUILD_ID}")
    if(laneLog)
        target_compile_definitions(${elf} PRIVATE DRAWPROBE_LANE_LOG=1)
    endif()
    target_compile_options(${elf} PRIVATE -Wall -O2)
    target_link_libraries(${elf} PRIVATE drawprobe_shader_bin citro3d ctru m)
endfunction()

set(PROBE_TITLE "G-Diffuser DrawProbe")
set(PROBE_SMDH "${CMAKE_CURRENT_BINARY_DIR}/drawprobe.smdh")
ctr_generate_smdh(OUTPUT "${PROBE_SMDH}" NAME "${PROBE_TITLE}"
                  DESCRIPTION "citro3d cost probe (o3ds60)"
                  AUTHOR "G-Diffuser Project" ICON "${PROBE_PACKAGING}/icon.png")

# The card build: .elf + .3dsx + .cia of ONE program.
add_drawprobe_elf(drawprobe OFF)
ctr_create_3dsx(drawprobe SMDH "${PROBE_SMDH}")

# The emulator lane build: .3dsx only.
add_drawprobe_elf(drawprobe-lane ON)
ctr_create_3dsx(drawprobe-lane SMDH "${PROBE_SMDH}")

set(PROBE_BANNER "${CMAKE_CURRENT_BINARY_DIR}/drawprobe.bnr")
add_custom_command(
    OUTPUT "${PROBE_BANNER}"
    COMMAND "${PROBE_BANNERTOOL}" makebanner -i "${PROBE_PACKAGING}/banner.png"
            -a "${PROBE_PACKAGING}/banner.wav" -o "${PROBE_BANNER}"
    DEPENDS "${PROBE_PACKAGING}/banner.png" "${PROBE_PACKAGING}/banner.wav"
    VERBATIM)

# The game's rsf, keeping every system setting (SystemMode 64MB on the Old 3DS), with only
# the identity edited so it installs beside the game.
file(READ "${PROBE_PACKAGING}/gdiffuser.rsf" rsf)
string(REGEX REPLACE "Title( +): G-Diffuser" "Title\\1: ${PROBE_TITLE}" rsf "${rsf}")
string(REGEX REPLACE "ProductCode( +): CTR-P-GDXF" "ProductCode\\1: CTR-P-DPRB" rsf "${rsf}")
string(REGEX REPLACE "UniqueId( +): 0xff3d5" "UniqueId\\1: 0xff3da" rsf "${rsf}")
if(NOT rsf MATCHES "UniqueId +: 0xff3da" OR NOT rsf MATCHES "ProductCode +: CTR-P-DPRB")
    message(FATAL_ERROR "drawprobe: the gdiffuser.rsf identity edits did not apply")
endif()
set(PROBE_RSF "${CMAKE_CURRENT_BINARY_DIR}/drawprobe.rsf")
file(WRITE "${PROBE_RSF}" "${rsf}")

set(PROBE_CIA "${CMAKE_CURRENT_BINARY_DIR}/DrawProbe.cia")
add_custom_command(
    OUTPUT "${PROBE_CIA}"
    COMMAND "${PROBE_MAKEROM}" -f cia -o "${PROBE_CIA}" -target t -exefslogo
            -elf "$<TARGET_FILE:drawprobe>" -rsf "${PROBE_RSF}" -icon "${PROBE_SMDH}"
            -banner "${PROBE_BANNER}"
    DEPENDS drawprobe "${PROBE_SMDH}" "${PROBE_BANNER}" "${PROBE_RSF}"
    VERBATIM)
add_custom_target(drawprobe-cia ALL DEPENDS "${PROBE_CIA}")
