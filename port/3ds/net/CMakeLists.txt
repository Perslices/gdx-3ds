# port/3ds/net/ — track `hwlink`: the hardware dev link (SOC, log link, control listener,
# hb:ldr self-relaunch). Register new sources HERE, never in port/CMakeLists.txt.
#
# 3DS-only: there is no host equivalent of soc:U / hb:ldr, and nothing in the host stub
# build calls into this. The host configure therefore gets an empty INTERFACE target so
# `target_link_libraries(... gdx3ds_net)` in the parent stays unconditional.
if(NINTENDO_3DS)
    add_library(gdx3ds_net STATIC
        gdx3ds_net.c
        # n60lead hwlink round: [net] enabled's default-resolution logic, split out to
        # port/gdx_net_enabled_default.c so it is host-testable (this TU is 3DS-only --
        # see port/tests/gdx_net_enabled_default_host_tests.c + tools/ci-3ds.sh).
        "${CMAKE_CURRENT_LIST_DIR}/../../gdx_net_enabled_default.c")
    # gdx3ds_config.h + gdx3ds_filelog.h come through gdx3ds_os's PUBLIC include dirs;
    # port/gdx_net_enabled_default.h is a bare top-level port/ header (same convention
    # gdx3ds_bottomhud.c uses for port/gdx_hud_placement.h).
    target_include_directories(gdx3ds_net PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../..")
    target_link_libraries(gdx3ds_net PRIVATE gdx3ds_os)
    # Same build-time build id the FPS-HUD bakes (branch@hash of HEAD, refreshed every
    # build -- see the parent CMakeLists.txt's gdx3ds_build_id_header target, hoisted above
    # this add_subdirectory() for exactly this): PING and the "[net] link up" line report
    # it, which is how tools/dev/hw.py reload proves a NEW process came back rather than
    # the old one.
    target_include_directories(gdx3ds_net PRIVATE "${GDX3DS_BUILD_ID_HEADER_DIR}")
    add_dependencies(gdx3ds_net gdx3ds_build_id_header)
else()
    add_library(gdx3ds_net INTERFACE)
endif()

target_include_directories(gdx3ds_net INTERFACE .)
