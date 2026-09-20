# port/3ds/net/ — track `hwlink`: the hardware dev link (SOC, log link, control listener,
# hb:ldr self-relaunch). Register new sources HERE, never in port/CMakeLists.txt.
#
# 3DS-only: there is no host equivalent of soc:U / hb:ldr, and nothing in the host stub
# build calls into this. The host configure therefore gets an empty INTERFACE target so
# `target_link_libraries(... gdx3ds_net)` in the parent stays unconditional.
if(NINTENDO_3DS)
    add_library(gdx3ds_net STATIC gdx3ds_net.c)
    # gdx3ds_config.h + gdx3ds_filelog.h come through gdx3ds_os's PUBLIC include dirs.
    target_link_libraries(gdx3ds_net PRIVATE gdx3ds_os)
    # Same configure-time build id the FPS-HUD bakes (branch@hash of HEAD at the last
    # cmake configure): PING and the "[net] link up" line report it, which is how
    # tools/dev/hw.py reload proves a NEW process came back rather than the old one.
    set_source_files_properties(gdx3ds_net.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_BUILD_ID=\"${GDX3DS_GIT_BRANCH}@${GDX3DS_GIT_HASH}\"")
else()
    add_library(gdx3ds_net INTERFACE)
endif()

target_include_directories(gdx3ds_net INTERFACE .)
