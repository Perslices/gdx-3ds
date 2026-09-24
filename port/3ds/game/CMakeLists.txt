# port/3ds/game/ — M1 integration: the decomp game + port bridge layer compiled for 3DS.
#
# Reuses port/cmake/GdxGameSources.cmake (the same source list + filters the desktop
# gdiffuser_game object library builds from) so the two builds cannot drift. Everything
# here is 3DS-only; the host GDX_PLATFORM_3DS stub build skips this directory's targets.
#
# Two OBJECT libraries (objects always fully link, so decomp<->bridge symbol cycles and
# static-archive ordering never bite):
#   gdx3ds_game — decomp game C + the port game-core TUs (n64_sched, decomp_port, ...),
#                 compiled with the desktop's exact define/flag set + GDX_PLATFORM_3DS.
#   gdx3ds_glue — the port bridge/glue TUs from the desktop G-Diffuser executable list,
#                 minus the desktop-only surfaces (SDL/ImGui/Discord/first-boot GUI/crash
#                 handler); see the keep/drop table in docs/research/m1-link-status.md.

if(NOT NINTENDO_3DS)
    return()
endif()

# 3DS MVP is cartridge-content only (docs/research/3ds-port-plan.md §3 stream D / §7);
# the 64DD Expansion Kit sources stay desktop-only until post-MVP. Must be set before the
# include() so the shared filter logic picks the rom-side subsystems (CMP0077: option()
# honors the pre-set normal variable).
#
# B11 (docs/research/native/ekbuild-progress.md): GDX_3DS_EXPANSION_KIT is the opt-in that
# flips it. It is OFF by default and the SHIPPED build is the OFF one -- EK=ON is not an
# additive flag, it swaps sys/rom -> sys/disk, audio/rom -> audio/disk and save_buffer.c ->
# dd_save.c and changes #ifdef EXPANSION_KIT behaviour in 65 decomp TUs, so an EK build is a
# different game binary, not the shipped one plus two overlays.
option(GDX_3DS_EXPANSION_KIT
       "Compile the 64DD Expansion Kit (course editor / machine creator) into the 3DS build -- EXPERIMENTAL, needs a ~64.9 MB .ndd disk image at runtime" OFF)
if(GDX_3DS_EXPANSION_KIT)
    message(STATUS "G-Diffuser-3DS: GDX_3DS_EXPANSION_KIT=ON -- Expansion Kit build (NOT the shipped configuration)")
    set(GDX_EXPANSION_KIT ON)
else()
    set(GDX_EXPANSION_KIT OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/GdxGameSources.cmake")

# ---------------------------------------------------------------------------------
# cxd4 RSP interpreter (LLE audio) — same vendored sources as the desktop lib.
# ---------------------------------------------------------------------------------
add_library(gdx3ds_cxd4_rsp STATIC
    "${GDX_PORT_DIR}/rsp/cxd4/gdx_rsp_driver.c"
    "${GDX_PORT_DIR}/rsp/cxd4/su.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/add.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/divide.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/logical.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/multiply.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/select.c"
    "${GDX_PORT_DIR}/rsp/cxd4/vu/vu.c")
target_include_directories(gdx3ds_cxd4_rsp PUBLIC "${GDX_PORT_DIR}/rsp/cxd4")
target_compile_definitions(gdx3ds_cxd4_rsp PRIVATE SP_EXECUTE_LOG=1 _CRT_SECURE_NO_WARNINGS=1)
target_compile_options(gdx3ds_cxd4_rsp PRIVATE -w -fno-strict-aliasing -fwrapv)

# ---------------------------------------------------------------------------------
# gdx3ds_game — decomp game sources (desktop parity flags; see port/CMakeLists.txt for
# the rationale on every flag). The fiber backend is NOT added here: gdx_fiber_3ds.c
# already lives in gdx3ds_os (stream B).
# ---------------------------------------------------------------------------------
if(GDX_3DS_EXPANSION_KIT)
    # EK=ON supplies the real leoBootID (port/n64_leo.c) and the ucode text markers
    # (port/gen/EkLinkStubs.c), so the EK=OFF stand-ins must NOT be compiled too.
    set(GDX3DS_EK_STUBS "")
    set(GDX3DS_EK_SOURCES ${GDX_GAME_EK_PORT_SOURCES})
else()
    set(GDX3DS_EK_STUBS gdx3ds_us_linkstubs.c)
    set(GDX3DS_EK_SOURCES "")
endif()

add_library(gdx3ds_game OBJECT ${GAME_SOURCES} ${GDX_GAME_CORE_SOURCES}
    ${GDX3DS_EK_STUBS}      # EK=OFF stand-ins for n64_leo.c/EkLinkStubs.c symbols
    ${GDX3DS_EK_SOURCES}    # EK=ON: n64_leo.c, gen/EkAssetBindings.c, gen/EkLinkStubs.c, gdx_ek_disk_overrides.c
    gdx3ds_livery_diag.c    # SHIP-LIVERY-2 [livery] game-state receipts (weak-called by gfx)
    gdx3ds_track_gbi.c      # [native] track: real GBI emitters behind gdx3ds_track_shim.h
    gdx3ds_fx_gbi.c         # [native] fx: real GBI emitters behind gdx3ds_fx_shim_begin.h
)

target_include_directories(gdx3ds_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${GDX_PORT_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
    "${CMAKE_CURRENT_SOURCE_DIR}/../gfx" # [native] track shim header (course.c, decomp-native-track-shim.patch)
)

target_compile_definitions(gdx3ds_game PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    ${GDX_GAME_DEFS}
    VERSION_US=1
    ASSET_VERSION=us
    ASSET_REVISION=rev0
    GDX_PLATFORM_3DS=1
    GDX_NATIVE_TRACK=1     # course.c includes gdx3ds_track_shim.h (runtime-gated: [native] track)
    GDX_NATIVE_FX=1        # racer.c includes gdx3ds_fx_shim.h (runtime-gated: [native] fx_sink)
)

if(GDX_3DS_EXPANSION_KIT)
    # Same triplet the desktop gdiffuser_game uses (port/CMakeLists.txt:30). MFS_VERSION/
    # BUILD_VERSION are the 64DD filesystem's own build selectors, not a game region: the
    # US text/font behaviour stays on VERSION_US above.
    target_compile_definitions(gdx3ds_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

# Same UB-hardening/legacy-quieting as the desktop gdiffuser_game (see port/CMakeLists.txt).
target_compile_options(gdx3ds_game PRIVATE -w -Werror=return-type -fcommon
    -fno-strict-aliasing -fwrapv
    -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types
    -Wno-error=implicit-function-declaration)

# ---------------------------------------------------------------------------------
# gdx3ds_glue — the port bridge layer (desktop G-Diffuser executable TU list, minus
# desktop-only files). Dropped TUs and why:
#   main.cpp                          -> replaced by port/3ds/main_3ds.cpp
#   gdx_discord.cpp                   -> Discord RPC dropped on 3DS
#   gdx_gui.cpp gdx_menu*.cpp gdx_console_log.cpp ui/UIWidgets.cpp gdx_imgui_nav.cpp
#   gdx_ghost_window.cpp gdx_input_viewer.cpp gdx_fps_overlay.cpp
#                                     -> ImGui enhancement/debug UI dropped on 3DS
#   enhancements/tuning/BoostDuration.cpp -> registers itself with the ImGui menu registry
#   gdx_firstboot.cpp gdx_firstboot_gui.cpp gdx_extract_launch.cpp gdx_dump_launch.cpp
#                                     -> first-boot Torch extraction moved to tools/prebake (PC)
#   disk_buffer.cpp + EK gen TUs      -> GDX_EXPANSION_KIT=OFF on 3DS (MVP)
# ---------------------------------------------------------------------------------
add_library(gdx3ds_glue OBJECT
    "${GDX_PORT_DIR}/enhancements/events/GameEvents.cpp"   # decomp racer.c references GameEvents_FireOnBoostStart
    "${GDX_PORT_DIR}/input_bridge.c"
    "${GDX_PORT_DIR}/gdx_input_script.c"
    "${GDX_PORT_DIR}/gdx_input_rec.c"     # ORACLE: .gdxrec record/replay
    "${GDX_PORT_DIR}/gdx_determinism.c"   # ORACLE: determinism pins
    "${GDX_PORT_DIR}/n64_gfx_bridge.cpp"
    "${GDX_PORT_DIR}/gdx_interp.cpp"
    "${GDX_PORT_DIR}/n64_gfx_convert.cpp"
    "${GDX_PORT_DIR}/gdx_vi_convert.c"
    "${GDX_PORT_DIR}/n64_audio_hle.c"
    "${GDX_PORT_DIR}/gdx_audio_lle.c"
    "${GDX_PORT_DIR}/gdx_audio_thread.cpp"
    "${GDX_PORT_DIR}/gdx_audio_capture.c"
    "${GDX_PORT_DIR}/gdx_frame_pacer.c"
    "${GDX_PORT_DIR}/shims.c"
    "${GDX_PORT_DIR}/AssetLoader.cpp"
    "${GDX_PORT_DIR}/resource/ResourceFactories.cpp"
    "${GDX_PORT_DIR}/rom_buffer.cpp"
    "${GDX_PORT_DIR}/sram_buffer.cpp"
    "${GDX_PORT_DIR}/disk_savefile.cpp"
    "${GDX_PORT_DIR}/gdx_ghost_io.c"
    "${GDX_PORT_DIR}/gdx_perf.cpp"
    "${GDX_PORT_DIR}/gdx_dev_gates.c"
    "${GDX_PORT_DIR}/gdx_workshop.cpp"
    "${GDX_PORT_DIR}/mio0_wrap.c"
    "${GDX_PORT_DIR}/gdx_stall.c"
    "${GDX_PORT_DIR}/gdx_segment_source.c"
    "${GDX_PORT_DIR}/../torch/lib/libmio0/mio0.c"
)

if(GDX_3DS_EXPANSION_KIT)
    # The desktop executable's EK glue (port/CMakeLists.txt:171): the disk-image loader plus
    # the fan-translation string/override layers n64_leo.c and the EK overlays call into.
    target_sources(gdx3ds_glue PRIVATE
        "${GDX_PORT_DIR}/disk_buffer.cpp"
        "${GDX_PORT_DIR}/gen/EkTranslatedOverrides.c"
        "${GDX_PORT_DIR}/gdx_ek_strings.c"
        "${GDX_PORT_DIR}/gen/EkTranslatedStrings.c"
        gdx3ds_ek_stubs.cpp)   # 3DS stand-ins for the dropped gdx_extract_launch.cpp helpers
endif()

target_include_directories(gdx3ds_glue PRIVATE
    "${GDX_PORT_DIR}"                       # "resource/ResourceFactories.h" etc.
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${GDX_PORT_DIR}/../torch/lib/libmio0"  # mio0.h + utils.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"  # gdx3ds_* contract headers
)
# The carve's include set (lus_stubs -> third_party -> LUS include/src) comes via the
# gdx3ds_lus_carve INTERFACE include dirs (gfx/CMakeLists.txt exports them).
target_link_libraries(gdx3ds_glue PRIVATE gdx3ds_lus_carve gdx3ds_cxd4_rsp)

target_compile_definitions(gdx3ds_glue PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    GDX_PLATFORM_3DS=1
    F3DEX_GBI_2=1
    $<$<BOOL:${GDX_3DS_EXPANSION_KIT}>:EXPANSION_KIT=1>
)

target_compile_options(gdx3ds_glue PRIVATE
    -Wall -Wno-unused-parameter
    -fno-strict-aliasing -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>
)
set_target_properties(gdx3ds_glue PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)
