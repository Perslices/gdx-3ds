# port/3ds/ — Nintendo 3DS port aggregator (Phase 0 skeleton).
#
# Build wiring rule (docs/research/3ds-port-plan.md): this file and the stream
# subdirectories below are the ONLY places 3DS sources get registered. Streams
# never edit port/CMakeLists.txt; each owns exactly one subdirectory here.
#   gfx/    — stream A    os/     — stream B
#   audio/  — stream C    assets/ — stream D
#
# Cross-compile: cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DGDX_PLATFORM_3DS=ON
# The devkitPro toolchain defines NINTENDO_3DS and provides ctr_create_3dsx().

# Configure-time build id (branch@hash of HEAD when cmake last configured; a rebuild
# without reconfigure keeps the old id). Baked into the FPS-HUD's static row AND into
# port/3ds/net/'s "[net] link up build=..." / PING receipts, so an on-device report and a
# hw.py reload can both name the build they came from. Hoisted above add_subdirectory()
# because net/ consumes it.
execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE GDX3DS_GIT_BRANCH
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short=8 HEAD
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE GDX3DS_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GDX3DS_GIT_BRANCH)
    set(GDX3DS_GIT_BRANCH "unknown")
endif()
if(NOT GDX3DS_GIT_HASH)
    set(GDX3DS_GIT_HASH "nohash")
endif()

# --- C4g: RDRAM ARENA SIZE (-DGDX_3DS_RDRAM_MB=N, DEFAULT 16 = the shipped value) ---------
#
# port/n64_rdram.h's GDX_RDRAM_SIZE is the single largest row on the heap -- ONE 16 MB calloc,
# 37% of the shipped heap -- and 16 MB is not a console size: the N64 has 4 MB of RDRAM and
# 8 MB with the Expansion Pak. It is the PORT's arena (two bump cursors growing toward each
# other) and it is ALSO the top of the gfx bridge's bare-offset classification window, so it
# is a build-time knob with a guest receipt rather than an ini lever: `[rdram-res]` under
# `[debug] membudget=1` prints the occupancy this size has to cover.
#
# HAZARD, and it is the reason this defaults to 16: an undersized arena does NOT fail loudly.
# gdx_rdram_alloc_raw aborts, but the resolver narrowing does not -- an offset that used to
# resolve into the arena simply stops resolving, and the port's own failure mode under memory
# pressure is a hang with a frozen frame counter, not a crash (o3dssim-progress.md §3.1).
# Read [rdram-res] minSafeMB back from a run over the CONTENT you care about before lowering
# this, and read docs/research/native/rdramdiet-progress.md for what the corpus did and did
# not exercise.
set(GDX_3DS_RDRAM_MB "16" CACHE STRING
    "C4g: size of the decomp RDRAM arena in MiB (16 = the shipped default).")
math(EXPR GDX3DS_RDRAM_BYTES "${GDX_3DS_RDRAM_MB} * 1024 * 1024")
if(GDX3DS_RDRAM_BYTES LESS 1048576)
    message(FATAL_ERROR "GDX_3DS_RDRAM_MB=${GDX_3DS_RDRAM_MB} is not a usable arena size.")
endif()
# Reaches every TU that includes n64_rdram.h -- the port glue, the bridge AND the decomp game
# object library, all of which are created by the add_subdirectory() calls below. An unset
# knob defines the macro to exactly the value the header already had, so the default build is
# the same translation it was before the knob existed.
add_compile_definitions(GDX_RDRAM_SIZE_BYTES=${GDX3DS_RDRAM_BYTES}u)
if(NOT GDX_3DS_RDRAM_MB EQUAL 16)
    message(STATUS "G-Diffuser-3DS: RDRAM ARENA KNOB ACTIVE -- ${GDX_3DS_RDRAM_MB} MiB "
                   "(${GDX3DS_RDRAM_BYTES} B) instead of the shipped 16 MiB. Read [rdram-res] "
                   "size= back from the guest before believing it applied.")
endif()

# --- MEMPROBE (-DGDX_3DS_MEMPROBE=ON, DEFAULT OFF) ----------------------------------------
# Compile-time diagnostics for the memory tracks, OFF in every shipped build so the default
# .3dsx carries none of it:
#   * decomp_port.c  -- the RDRAM bump/persist high-water and the top-down non-zero scan that
#                       [rdram-res] reports (the scan is the honest occupancy measurement; it
#                       sees writes that never went through either allocator).
#   * n64_gfx_bridge.cpp -- the highest RDRAM offset any resolver ever admitted, which is the
#                       classification constraint on GDX_3DS_RDRAM_MB.
# Neither changes an allocation size, an allocation order or a frame; both are max-updates.
option(GDX_3DS_MEMPROBE
       "Compile the RDRAM occupancy + classification probes ([rdram-res] fields)" OFF)
if(GDX_3DS_MEMPROBE)
    add_compile_definitions(GDX_MEMPROBE=1)
    message(STATUS "G-Diffuser-3DS: MEMPROBE ACTIVE -- [rdram-res] reports real high-waters. "
                   "This is a measurement build, not the shipped one.")
endif()

add_subdirectory(gfx)
add_subdirectory(os)
add_subdirectory(audio)
add_subdirectory(assets)
add_subdirectory(net)     # hwlink: WiFi dev link (3DS-only inside)
add_subdirectory(game)      # M1 integration: decomp game + bridge layer (3DS-only inside)
add_subdirectory(lus_glue)  # M1 integration: LUS link-stub TUs (3DS-only inside)
if(NINTENDO_3DS)
    add_subdirectory(harness) # DL replay test app (gdx3ds-dl-tests.3dsx)
endif()

add_executable(G-Diffuser-3DS main_3ds.cpp)
target_include_directories(G-Diffuser-3DS PRIVATE include)
target_link_libraries(G-Diffuser-3DS PRIVATE gdx3ds_os gdx3ds_audio gdx3ds_assets)

if(NINTENDO_3DS)
    # Full game link: decomp + bridge objects, LUS carve + citro3d backend, the
    # miniz-backed libzip surface (O2rArchive), and the lus_glue link stubs
    # (Ship::Context / ConsoleVariable / Fast3dWindow replacements — see lus_glue/).
    target_compile_definitions(G-Diffuser-3DS PRIVATE GDX_PLATFORM_3DS=1)
    target_include_directories(G-Diffuser-3DS PRIVATE ..)   # port/: gdx_audio_thread.h, rom_buffer.h, resource/...
    target_sources(G-Diffuser-3DS PRIVATE
        $<TARGET_OBJECTS:gdx3ds_game>
        $<TARGET_OBJECTS:gdx3ds_glue>
        $<TARGET_OBJECTS:gdx3ds_lus_glue>
        $<TARGET_OBJECTS:gdx3ds_n64>   # [native] interp: owned interpreter (weak-hooked from the bridge)
    )
    target_link_libraries(G-Diffuser-3DS PRIVATE
        gdx3ds_gfx
        gdx3ds_lus_carve
        gdx3ds_zipshim
        gdx3ds_cxd4_rsp
        gdx3ds_os      # again after carve/glue: fiber + config symbols
        gdx3ds_net     # HWLINK: SOC log link + control listener ([net] enabled, DEFAULT OFF)
    )
    set_target_properties(G-Diffuser-3DS PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
    )

    # H-HARDWARE: SD file-log sink for the boot/watchdog/fatal tracers (svc debug goes
    # nowhere on real hardware). Integration-level like main_3ds.cpp, not a stream dir.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_filelog.c)

    # RENDER THREAD (LOCKED-60 Task H): core-2 render thread, [debug] renderthread killswitch.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_renderthread.cpp)

    # FPS-HUD: bottom-screen fps counter (gdiffuser.ini [debug] fps, DEFAULT ON).
    # Integration-level like gdx3ds_filelog.c. The HUD's static row bakes a
    # configure-time build id (branch@hash of HEAD when cmake last configured — a
    # rebuild without reconfigure keeps the old id) so on-device reports can name
    # the build they came from.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_fps_hud.c)
    set_source_files_properties(gdx3ds_fps_hud.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_BUILD_ID=\"${GDX3DS_GIT_BRANCH}@${GDX3DS_GIT_HASH}\"")

    # O3DS BUDGET: [mem-budget] grant receipt + [mem-res] ranked residency census
    # (gdiffuser.ini [debug] membudget, DEFAULT OFF). Diagnostic only; every call site
    # returns on one file-static load when the lever is off.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_membudget.cpp)

    # C4c CUT #1: the Fast3D decoded-texture staging reservation ([debug] tex_upload_kb,
    # DEFAULT 0 = today's size exactly). Fast::Interpreter::Init holds one
    # malloc(GetMaxTextureSize()^2 * 4) = 4,194,304 B for the life of the process, which the
    # C4c allocation census put at rank 3 of the whole heap and which no [mem-res] row names.
    # An ini lever rather than a build knob because Fast3D's Init runs long after
    # gdiffuser.ini is read. See port/3ds/gdx3ds_texupload.c.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_texupload.c)

    # O3DS GRANT KNOB (NEXT.md C4b / C4e) -- DEFAULT OFF, and off means byte-identical.
    #
    # libctru's __system_allocateHeaps() carves the malloc heap and the linear heap out of
    # the process commit budget from crt0, before main() and before any static constructor,
    # so NO ini lever can ever reach it. The only handle is the pair of weak symbols it reads
    # on entry; gdx3ds_heapcfg.c defines them strongly when -- and only when -- one of the two
    # cache variables below is non-zero. With both at 0 the TU defines no __ctru_* symbol and
    # contributes three constant-returning accessors for the [mem-budget] receipt.
    #
    #   -DGDX_3DS_SIM_AVAIL_KIB=N   C4e. Pin BOTH grants to what libctru would compute on a
    #                               console whose commit budget is N KiB. Azahar's "Old 3DS"
    #                               is memtype 2 (96 MB extended), so the emulator lane is
    #                               MORE generous than a real Old 3DS and cannot otherwise
    #                               reproduce the 64 MB memtype-0 constraint. This is how it
    #                               gets an unattended gate.
    #   -DGDX_3DS_SIM_O3DS=ON       shorthand for the memtype-0 budget: 54640 KiB =
    #                               55,951,360 B = the 64 MB APPLICATION region minus the
    #                               11,157,504 B of image+bss+slack that BOTH measured
    #                               profiles (memtype 6 and memtype 2) show for this .3dsx.
    #   -DGDX_3DS_LINEAR_HEAP_KIB=N C4b. Pin ONLY the linear grant and let libctru compute
    #                               heap = avail - linear, which is the branch with no 24 MiB
    #                               cap. This is the shape the cut would ship in.
    #
    # HAZARD: __system_allocateHeaps svcBreaks (USERBREAK_PANIC) if the PINNED heap+linear
    # exceeds the device's real budget, in crt0, with no log line and no [fatal] -- a black
    # screen. A GDX_3DS_SIM_AVAIL_KIB above the host console's own budget is that crash.
    option(GDX_3DS_SIM_O3DS
           "Simulate the Old 3DS memtype-0 (64 MB) commit budget so the emulator reproduces the real grant" OFF)
    set(GDX_3DS_SIM_AVAIL_KIB "0" CACHE STRING
        "C4e: simulate a commit budget of N KiB (0 = off). GDX_3DS_SIM_O3DS=ON sets 54640.")
    set(GDX_3DS_LINEAR_HEAP_KIB "0" CACHE STRING
        "C4b: pin __ctru_linear_heap_size to N KiB (0 = off, libctru's default split).")

    set(GDX3DS_SIM_AVAIL_KIB "${GDX_3DS_SIM_AVAIL_KIB}")
    if(GDX_3DS_SIM_O3DS AND GDX3DS_SIM_AVAIL_KIB EQUAL 0)
        set(GDX3DS_SIM_AVAIL_KIB 54640)   # 55,951,360 B -- see the host test's case 3
    endif()
    math(EXPR GDX3DS_SIM_AVAIL_BYTES "${GDX3DS_SIM_AVAIL_KIB} * 1024")
    math(EXPR GDX3DS_PIN_LINEAR_BYTES "${GDX_3DS_LINEAR_HEAP_KIB} * 1024")
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_heapcfg.c)
    set_source_files_properties(gdx3ds_heapcfg.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_SIM_AVAIL_BYTES=${GDX3DS_SIM_AVAIL_BYTES}UL;GDX3DS_PIN_LINEAR_BYTES=${GDX3DS_PIN_LINEAR_BYTES}UL")
    if(NOT GDX3DS_SIM_AVAIL_BYTES EQUAL 0 OR NOT GDX3DS_PIN_LINEAR_BYTES EQUAL 0)
        message(STATUS "G-Diffuser-3DS: HEAP KNOB ACTIVE -- simAvail=${GDX3DS_SIM_AVAIL_BYTES} B, "
                       "pinLinear=${GDX3DS_PIN_LINEAR_BYTES} B. This is NOT the shipped memory layout; "
                       "read [mem-budget] heapCfg=/heapGrant= back from the guest before believing it applied.")
    endif()

    # --- C4c: MALLOC INTERPOSITION + HEAP ATTRIBUTION -------------------------------------
    #
    # port/3ds/include/gdx3ds_heapattr.h carries the contract. Two knobs, deliberately
    # separate, because they trade off completely differently:
    #
    #   -DGDX_3DS_WRAP_MALLOC=ON   DEFAULT ON. The universal allocation-failure receipt.
    #                              `rdramdiet` landed [alloc-fail] at the C sites holding the
    #                              big blocks and stated the gap: the 7,138,708 B the census
    #                              could not attribute stayed silent. Under a tight grant this
    #                              port HANGS with a frozen frame counter rather than crashing
    #                              (o3dssim §3.1), which on a console is indistinguishable
    #                              from a deadlock by eye -- so a silent NULL is the most
    #                              expensive thing in the build. This makes every one of them
    #                              print its size AND its caller.
    #                              Cost: `malloc -> _malloc_r` becomes `__wrap_malloc ->
    #                              _malloc_r`, the SAME call depth (the wrapper REPLACES
    #                              newlib's thunk rather than wrapping it), plus one NULL
    #                              compare. OFF restores the original link exactly.
    #
    #   -DGDX_3DS_HEAPATTR=ON      DEFAULT OFF, requires the above. The ranked [heap-attr]
    #                              census: a live ptr -> (size, return address) map, so the
    #                              heap bytes [mem-res] cannot name get named. ~2 MB of .bss
    #                              and a lock on every allocation -- a MEASUREMENT build,
    #                              never a shipped one, and it does not fit a real memtype-0
    #                              console precisely because of that .bss.
    #
    # The wrap set is the `_r` PRIMITIVES plus the public entry points, not `malloc`/`free`
    # as C4c was originally specified: in devkitARM's newlib, 26 objects in libc.a call
    # `_malloc_r` directly against 17 that call `malloc`, so wrapping only the public names
    # would have measured a fraction of the C side and reported it as all of it.
    option(GDX_3DS_WRAP_MALLOC
           "Interpose newlib's allocator so every allocation failure prints [alloc-fail]" ON)
    option(GDX_3DS_HEAPATTR
           "Compile the live allocation map behind the [heap-attr] ranked census (measurement build)" OFF)
    if(GDX_3DS_HEAPATTR AND NOT GDX_3DS_WRAP_MALLOC)
        message(FATAL_ERROR
            "GDX_3DS_HEAPATTR=ON needs GDX_3DS_WRAP_MALLOC=ON: the map is populated by the "
            "wrappers, and without them it would print an empty table that reads like a "
            "measurement.")
    endif()
    # The TU is compiled either way so gdx3ds_membudget.cpp keeps a definition of the two
    # census entry points; with the option OFF everything inside it compiles away and no
    # --wrap reaches the linker, which is what makes "OFF is the original link" exact.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_mallocwrap.c)
    if(GDX_3DS_WRAP_MALLOC)
        set(GDX3DS_MALLOCWRAP_DEFS "GDX3DS_WRAP_MALLOC=1")
        target_link_options(G-Diffuser-3DS PRIVATE
            "LINKER:--wrap=_malloc_r" "LINKER:--wrap=_free_r" "LINKER:--wrap=_calloc_r"
            "LINKER:--wrap=_realloc_r" "LINKER:--wrap=_memalign_r"
            "LINKER:--wrap=malloc" "LINKER:--wrap=free" "LINKER:--wrap=calloc"
            "LINKER:--wrap=realloc" "LINKER:--wrap=memalign")
        if(GDX_3DS_HEAPATTR)
            list(APPEND GDX3DS_MALLOCWRAP_DEFS "GDX3DS_HEAPATTR=1")
            message(STATUS "G-Diffuser-3DS: HEAPATTR ACTIVE -- [heap-attr] ranked allocation "
                           "census compiled in (~2 MB of .bss + a lock per allocation). This is "
                           "a measurement build, not the shipped one.")
        endif()
        set_source_files_properties(gdx3ds_mallocwrap.c PROPERTIES COMPILE_DEFINITIONS
            "${GDX3DS_MALLOCWRAP_DEFS}")
    else()
        message(STATUS "G-Diffuser-3DS: WRAP_MALLOC OFF -- no universal [alloc-fail] receipt. "
                       "A C-side allocation failure will be silent again.")
    endif()

    # MENU: v1 bottom-screen touch menu (gdiffuser.ini [menu] enabled, DEFAULT ON).
    # Integration-level like the HUD/filelog TUs; shares the HUD's console + build id.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_menu.c)

    # DYNLOD (LOCKED-60 Task J): automatic rival-detail tier from the render task's wall
    # time ([perf] rival_detail_auto, DEFAULT ON; manual [perf] rival_detail is the floor).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_dynlod.c)
    # MAINPROF (NEXT.md A8): [mprof] main-thread section profiler, gdiffuser.ini
    # [debug] mainprof (DEFAULT OFF). Diagnostic only; every hook site is one int
    # load + branch when the gate is off.
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_mainprof.c)
    # ORACLE: true frame capture + [fh] receipts (docs/research/native/03-oracle-plan.md).
    target_sources(G-Diffuser-3DS PRIVATE gdx3ds_oracle.c)
    set_source_files_properties(gdx3ds_oracle.c PROPERTIES COMPILE_DEFINITIONS
        "GDX3DS_BUILD_ID=\"${GDX3DS_GIT_BRANCH}@${GDX3DS_GIT_HASH}\"")

    # H-HARDWARE: proper SMDH (48x48 icon + titles/publisher) instead of the devkitPro
    # placeholder. Assets + provenance/licensing: packaging/README.md.
    set(GDX3DS_SMDH "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.smdh")
    ctr_generate_smdh(
        OUTPUT "${GDX3DS_SMDH}"
        NAME "G-Diffuser"
        DESCRIPTION "G-Diffuser — F-Zero X decompilation port"
        AUTHOR "G-Diffuser Project"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon.png"
    )
    ctr_create_3dsx(G-Diffuser-3DS SMDH "${GDX3DS_SMDH}")

    # Optional installable CIA (target: G-Diffuser-3DS-cia). Gated on makerom +
    # bannertool, which devkitPro does not ship — see packaging/README.md for where
    # to get them (tools/3ds-bin/ is the gitignored drop point).
    find_program(GDX_MAKEROM_EXE makerom HINTS "${CMAKE_SOURCE_DIR}/tools/3ds-bin")
    find_program(GDX_BANNERTOOL_EXE bannertool HINTS "${CMAKE_SOURCE_DIR}/tools/3ds-bin")
    if(GDX_MAKEROM_EXE AND GDX_BANNERTOOL_EXE)
        set(GDX3DS_BANNER "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.bnr")
        set(GDX3DS_CIA "${CMAKE_CURRENT_BINARY_DIR}/G-Diffuser-3DS.cia")
        add_custom_command(
            OUTPUT "${GDX3DS_BANNER}"
            COMMAND "${GDX_BANNERTOOL_EXE}" makebanner
                -i "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.png"
                -a "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
                -o "${GDX3DS_BANNER}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/banner.wav"
            COMMENT "Building HOME-menu banner G-Diffuser-3DS.bnr"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${GDX3DS_CIA}"
            COMMAND "${GDX_MAKEROM_EXE}" -f cia -o "${GDX3DS_CIA}" -target t -exefslogo
                -elf "$<TARGET_FILE:G-Diffuser-3DS>"
                -rsf "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gdiffuser.rsf"
                -icon "${GDX3DS_SMDH}"
                -banner "${GDX3DS_BANNER}"
            DEPENDS
                G-Diffuser-3DS
                "${GDX3DS_SMDH}"
                "${GDX3DS_BANNER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gdiffuser.rsf"
            COMMENT "Building CIA G-Diffuser-3DS.cia (title 000400000FF3D500)"
            VERBATIM
        )
        add_custom_target(G-Diffuser-3DS-cia DEPENDS "${GDX3DS_CIA}")
    else()
        message(STATUS "G-Diffuser-3DS-cia target disabled: need makerom + bannertool "
                       "(see port/3ds/packaging/README.md)")
    endif()
endif()
