# port/3ds/probes/homeprobe -- the Old 3DS HOME-freeze bisect title (NEXT.md C4j,
# docs/research/native/o3dshome2-progress.md). STANDALONE project: not part of the game build
# or tools/ci-3ds.sh. Build:
#   cmake -S port/3ds/probes/homeprobe -B <out-of-tree dir> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#   cmake --build <that dir>
# Produces two CIAs of the SAME ELF logic under distinct title IDs:
#   HomeProbe-OurRsf.cia  000400000FF3D600  the game's gdiffuser.rsf + the game's banner/icon
#   HomeProbe-FbiRsf.cia  000400000FF3D700  that rsf with FBI's (buildtools make_base) defaults:
#                                           SystemModeExt Legacy, CpuSpeed 268MHz, no L2 cache
cmake_minimum_required(VERSION 3.13)
project(homeprobe C)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "homeprobe needs the devkitPro 3DS toolchain")
endif()

set(PROBE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(PROBE_PACKAGING "${PROBE_REPO}/port/3ds/packaging")
find_program(PROBE_MAKEROM makerom HINTS "${PROBE_REPO}/tools/3ds-bin" REQUIRED)
find_program(PROBE_BANNERTOOL bannertool HINTS "${PROBE_REPO}/tools/3ds-bin" REQUIRED)

set(PROBE_BANNER "${CMAKE_CURRENT_BINARY_DIR}/homeprobe.bnr")
add_custom_command(
    OUTPUT "${PROBE_BANNER}"
    COMMAND "${PROBE_BANNERTOOL}" makebanner -i "${PROBE_PACKAGING}/banner.png"
            -a "${PROBE_PACKAGING}/banner.wav" -o "${PROBE_BANNER}"
    DEPENDS "${PROBE_PACKAGING}/banner.png" "${PROBE_PACKAGING}/banner.wav"
    VERBATIM)

# One variant = one ELF (its log file name is baked in) + one rsf + one SMDH + one CIA.
function(add_probe_variant tag title uniqueId rsfEdits)
    set(elf homeprobe-${tag})
    add_executable(${elf} homeprobe.c)
    target_compile_definitions(${elf} PRIVATE HOMEPROBE_TAG="${tag}")
    if(DEFINED PROBE_AUTOMODE)
        # Emulator smoke only (-DPROBE_AUTOMODE=0..3): skip the button prompt. Never ship it.
        target_compile_definitions(${elf} PRIVATE HOMEPROBE_AUTOMODE=${PROBE_AUTOMODE})
    endif()
    target_compile_options(${elf} PRIVATE -Wall -O2)
    target_link_libraries(${elf} PRIVATE citro3d ctru m)

    file(READ "${PROBE_PACKAGING}/gdiffuser.rsf" rsf)
    string(REGEX REPLACE "Title( +): G-Diffuser" "Title\\1: ${title}" rsf "${rsf}")
    string(REGEX REPLACE "ProductCode( +): CTR-P-GDXF" "ProductCode\\1: CTR-P-HPRB" rsf "${rsf}")
    string(REGEX REPLACE "UniqueId( +): 0xff3d5" "UniqueId\\1: ${uniqueId}" rsf "${rsf}")
    foreach(edit IN LISTS rsfEdits)
        string(REPLACE "=>" ";" pair "${edit}")
        list(GET pair 0 from)
        list(GET pair 1 to)
        string(REGEX REPLACE "${from}" "${to}" rsf "${rsf}")
    endforeach()
    set(rsfOut "${CMAKE_CURRENT_BINARY_DIR}/${elf}.rsf")
    file(WRITE "${rsfOut}" "${rsf}")

    set(smdh "${CMAKE_CURRENT_BINARY_DIR}/${elf}.smdh")
    ctr_generate_smdh(OUTPUT "${smdh}" NAME "${title}" DESCRIPTION "G-Diffuser HOME bisect probe"
                      AUTHOR "G-Diffuser Project" ICON "${PROBE_PACKAGING}/icon.png")
    set(cia "${CMAKE_CURRENT_BINARY_DIR}/HomeProbe-${tag}.cia")
    add_custom_command(
        OUTPUT "${cia}"
        COMMAND "${PROBE_MAKEROM}" -f cia -o "${cia}" -target t -exefslogo -elf "$<TARGET_FILE:${elf}>"
                -rsf "${rsfOut}" -icon "${smdh}" -banner "${PROBE_BANNER}"
        DEPENDS ${elf} "${smdh}" "${PROBE_BANNER}" "${rsfOut}"
        VERBATIM)
    add_custom_target(${elf}-cia ALL DEPENDS "${cia}")
    ctr_create_3dsx(${elf} SMDH "${smdh}") # emulator smoke only; the card ships the CIAs
endfunction()

add_probe_variant(OurRsf "HOME Probe OurRsf" 0xff3d6 "")
add_probe_variant(FbiRsf "HOME Probe FbiRsf" 0xff3d7
    "SystemModeExt( +): 124MB=>SystemModeExt\\1: Legacy;CpuSpeed( +): 804MHz=>CpuSpeed\\1: 268MHz;EnableL2Cache( +): true=>EnableL2Cache\\1: false")
