# port/3ds/vendor/citro3d -- vendored citro3d 1.7.1 (PROVENANCE.md).
#
# Compiled with the flags devkitPro's prebuilt libcitro3d.a (citro3d 1.7.1-2) was built with,
# read from its DW_AT_producer:
#   -mword-relocations -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm -march=armv6k -g -O2
#   -ffunction-sections -fdata-sections -fomit-frame-pointer -fno-math-errno
# plus the Makefile's -DNDEBUG=1 -D__3DS__ -DCITRO3D_BUILD, C17 (gcc 13's default -std=gnu17).
# The configuration flags (CMAKE_C_FLAGS_<CONFIG>) are replaced, not appended to, so a Debug or
# MinSizeRel game build does not change this library. It is NOT in GDX3DS_CODEGEN_TARGETS: the
# o3ds codegen profile never touches it. -Werror is dropped (gcc 16 warns where 13 did not);
# warnings do not change code.

add_library(gdx3ds_citro3d STATIC
    source/attribs.c
    source/base.c
    source/buffers.c
    source/drawArrays.c
    source/drawElements.c
    source/effect.c
    source/fog.c
    source/framebuffer.c
    source/gas.c
    source/immediate.c
    source/light.c
    source/lightenv.c
    source/lightlut.c
    source/mtxstack.c
    source/proctex.c
    source/renderqueue.c
    source/tex3ds.c
    source/texenv.c
    source/texture.c
    source/uniforms.c
    source/gdx_submit.c   # GDX (c5submit): the P3a diet, verify and profile (gdx/c3d_gdx.h)
    source/maths/mtx_fromquat.c
    source/maths/mtx_inverse.c
    source/maths/mtx_lookat.c
    source/maths/mtx_multiply.c
    source/maths/mtx_multiplyfvec3.c
    source/maths/mtx_multiplyfvec4.c
    source/maths/mtx_ortho.c
    source/maths/mtx_orthotilt.c
    source/maths/mtx_persp.c
    source/maths/mtx_perspstereo.c
    source/maths/mtx_perspstereotilt.c
    source/maths/mtx_persptilt.c
    source/maths/mtx_rotate.c
    source/maths/mtx_rotatex.c
    source/maths/mtx_rotatey.c
    source/maths/mtx_rotatez.c
    source/maths/mtx_scale.c
    source/maths/mtx_translate.c
    source/maths/mtx_transpose.c
    source/maths/quat_crossfvec3.c
    source/maths/quat_fromaxisangle.c
    source/maths/quat_frommtx.c
    source/maths/quat_frompitchyawroll.c
    source/maths/quat_lookat.c
    source/maths/quat_multiply.c
    source/maths/quat_pow.c
    source/maths/quat_rotate.c
    source/maths/quat_rotatex.c
    source/maths/quat_rotatey.c
    source/maths/quat_rotatez.c
)
# n60draw: PUBLIC, so every TU that links the vendored library compiles against its headers (-I is
# searched before devkitPro's -isystem copy). They are the prebuilt package's headers verbatim except
# c3d/uniforms.h, whose setters mark C3Dgdx_unifTouch (the NRR direct emitter's uniform hint): a
# setter compiled against the stock copy would write a uniform behind the hint.
target_include_directories(gdx3ds_citro3d PUBLIC include)
# gdx/: the diet's control surface for the port (c3d_gdx.h) and the host-tested scan helpers.
target_include_directories(gdx3ds_citro3d PUBLIC gdx)
target_compile_definitions(gdx3ds_citro3d INTERFACE GDX3DS_VENDOR_CITRO3D=1)
target_compile_definitions(gdx3ds_citro3d PRIVATE NDEBUG=1 __3DS__ CITRO3D_BUILD)
target_compile_options(gdx3ds_citro3d PRIVATE
    -marm -O2 -g -fdata-sections -fomit-frame-pointer -fno-math-errno
    -Wall -Wno-sizeof-array-div
)
set_target_properties(gdx3ds_citro3d PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)
# Replace the build-type flags (-O2 -g -DNDEBUG in Release) for this target only: the options
# above are the complete set. CMake has no per-target CMAKE_C_FLAGS_<CONFIG>, so the directory
# scope (this subdirectory holds only this target) is where they are cleared.
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${cfg} "")
endforeach()
