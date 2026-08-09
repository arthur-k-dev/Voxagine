# HLSL -> SPIR-V via DXC.
#
# The port keeps the original HLSL rather than translating to GLSL; DXC's
# -spirv backend compiles all of it unmodified. No [[vk::binding]] annotations
# were needed.
#
# The shifts below are load-bearing, not cosmetic. HLSL's b/t/u/s register
# classes are independent namespaces but Vulkan has a single binding namespace
# per set, so without them a shader declaring b0 and u0 emits two descriptors
# at set 0 binding 0 - an invalid layout. They must stay in sync with
# VKBindings in Vulkan/VKShaderBindings.h.

find_program(DXC_EXECUTABLE NAMES dxc HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)

set(VOXAGINE_DXC_SHIFTS
    # DXC defaults to targeting vulkan1.0 and refuses to emit any SPIR-V
    # subgroup operation for it. VKDevice asks for VK_API_VERSION_1_3, so this
    # only tells the compiler what the runtime has always been. Raise it here,
    # not per shader - a mismatch between the two is invisible until a shader
    # fails to compile.
    #
    # Added for QuadReadAcrossX/Y in the soft shadow, which has since gone back
    # to per-pixel sampling; nothing uses a subgroup op right now. Kept because
    # the target env should match the device regardless, and because the phase 7
    # mip-pyramid build is a compute pass that will want wave intrinsics.
    -fspv-target-env=vulkan1.1
    -fvk-b-shift 0 0
    -fvk-t-shift 100 0
    -fvk-u-shift 200 0
    -fvk-s-shift 300 0
    # DX buffer layout, not std430: the engine memcpys tightly-packed C++
    # structs into structured buffers (StructuredVoxelBuffer is 32 bytes;
    # std430 pads its float3s to a 48-byte stride, which silently shifts
    # every element after [0]).
    -fvk-use-dx-layout
)

# voxagine_add_shaders(<target> SOURCE_DIR <dir> OUTPUT_DIR <dir> [DEFINES <macro>...])
#
# Compiles every *.vs.hlsl / *.ps.hlsl / *.cs.hlsl under SOURCE_DIR. Plain
# *.hlsl files are include-only (Camera.hlsl, Defines.hlsl, ...) and are
# compiled as part of whatever includes them.
#
# DEFINES is preprocessor macros (bare NAME or NAME=VALUE, dxc's -D) beyond
# the fixed shift/layout set above - VOXAGINE_MOBILE is the one caller today,
# so Defines.hlsl can branch cone step counts by platform.
#
# **OUTPUT_DIR sits inside the source tree and is not platform-qualified** -
# CLAUDE.md's reasoning is that the engine asks for a shader by asset path and
# expects the .spv to be sitting next to the .hlsl it was built from. That is
# fine as long as every configure produces the *same* bytes for the same
# input, which was true until DEFINES existed: a desktop configure and a
# mobile configure now legitimately want different bytes at the *same* path.
# Whichever preset was built last is what every other preset's asset root then
# picks up too, invisibly - `stageGameAssets` on the Android side reads
# whatever is currently sitting in Game/Engine/Assets/Shaders at Gradle time,
# not a mobile-specific copy. **Always rebuild shaders for the platform you are
# about to run before running it** - `cmake --build --preset <preset>` covers
# this, a stale incremental build does not.
function(voxagine_add_shaders TARGET_NAME)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;OUTPUT_DIR" "DEFINES" ${ARGN})

    set(VOXAGINE_DXC_DEFINES "")
    foreach(DEFINE ${ARG_DEFINES})
        list(APPEND VOXAGINE_DXC_DEFINES -D${DEFINE})
    endforeach()

    if(NOT DXC_EXECUTABLE)
        message(WARNING "dxc not found - skipping shader target ${TARGET_NAME}")
        return()
    endif()

    file(GLOB SHADER_VS ${ARG_SOURCE_DIR}/*.vs.hlsl)
    file(GLOB SHADER_PS ${ARG_SOURCE_DIR}/*.ps.hlsl)
    file(GLOB SHADER_CS ${ARG_SOURCE_DIR}/*.cs.hlsl)

    # Any .hlsl may be included by any other, and dxc gives us no depfile, so
    # treat the whole directory as the dependency set. Coarse but never stale.
    file(GLOB SHADER_INCLUDES ${ARG_SOURCE_DIR}/*.hlsl)

    set(SPIRV_OUTPUTS "")

    foreach(PROFILE_PAIR "vs_6_0;${SHADER_VS}" "ps_6_0;${SHADER_PS}" "cs_6_0;${SHADER_CS}")
        list(POP_FRONT PROFILE_PAIR PROFILE)

        foreach(SHADER ${PROFILE_PAIR})
            get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
            get_filename_component(SHADER_FULL ${SHADER} NAME)
            string(REGEX REPLACE "\\.hlsl$" ".spv" SPIRV_NAME ${SHADER_FULL})

            set(SPIRV_OUT ${ARG_OUTPUT_DIR}/${SPIRV_NAME})

            add_custom_command(
                OUTPUT ${SPIRV_OUT}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUTPUT_DIR}
                COMMAND ${DXC_EXECUTABLE}
                        -spirv -T ${PROFILE} -E main
                        ${VOXAGINE_DXC_SHIFTS}
                        ${VOXAGINE_DXC_DEFINES}
                        -Fo ${SPIRV_OUT}
                        ${SHADER}
                DEPENDS ${SHADER} ${SHADER_INCLUDES}
                COMMENT "DXC ${PROFILE} ${SHADER_FULL}"
                VERBATIM
            )

            list(APPEND SPIRV_OUTPUTS ${SPIRV_OUT})
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPIRV_OUTPUTS})

    list(LENGTH SPIRV_OUTPUTS SPIRV_COUNT)
    message(STATUS "Shaders: ${TARGET_NAME} -> ${SPIRV_COUNT} SPIR-V modules")
endfunction()
