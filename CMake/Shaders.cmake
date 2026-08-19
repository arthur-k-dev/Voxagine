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
# macOS installations often have shaderc/glslc before DXC. It accepts the
# same HLSL source and produces SPIR-V directly, so it is a real build
# fallback rather than silently shipping an app with no shader modules.
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS /usr/local/opt/shaderc/bin /opt/homebrew/opt/shaderc/bin)
find_program(SPIRV_OPT_EXECUTABLE NAMES spirv-opt HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
find_program(VOXAGINE_PYTHON3 NAMES python3 python)

# Captured here, at include time, because CMAKE_CURRENT_LIST_DIR/_FILE inside a
# function body resolve against whichever list file *calls* the function - which
# is CMakeLists.txt, not this directory. Using them directly below silently
# pointed the helper path and the rebuild dependency at the project root.
set(VOXAGINE_SHADERS_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(VOXAGINE_SHADERS_CMAKE_FILE ${CMAKE_CURRENT_LIST_FILE})

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

    # A warning rather than an error, and the difference matters: CI has neither
    # compiler and only wants to know the engine still builds, so failing the
    # configure here stopped every job at the configure step. The executable
    # still links without this target - CMakeLists.txt guards the
    # add_dependencies - it simply cannot render until someone compiles the
    # shaders.
    #
    # The case this used to guard, shipping an app whose .spv files were never
    # produced, is caught where it actually matters: Platforms/iOS/build-and-
    # deploy.sh counts SPIR-V modules against shader stages and refuses to
    # package a bundle that is missing any.
    if(NOT DXC_EXECUTABLE AND NOT GLSLC_EXECUTABLE)
        message(WARNING
            "Neither dxc nor glslc was found - skipping shader target ${TARGET_NAME}. "
            "The build will link but cannot render.")
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

            if(DXC_EXECUTABLE)
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
            else()
                if(PROFILE MATCHES "^vs_")
                    set(GLSLC_STAGE vert)
                elseif(PROFILE MATCHES "^ps_")
                    set(GLSLC_STAGE frag)
                else()
                    set(GLSLC_STAGE comp)
                endif()

                set(GLSLC_OUTPUT ${SPIRV_OUT})

                # Defines.hlsl's [[vk::image_format("rgba8")]] on the voxel
                # buffers is guarded by __spirv__, which only DXC defines - and
                # glslang would not implement the attribute anyway. Without it
                # the voxel buffers come out Rgba32f while their views are
                # RGBA8, and the whole voxel world decodes to garbage while
                # every other pass looks fine. There is no glslc flag or
                # spirv-opt pass for this, so the format is corrected in the
                # emitted binary. See CMake/FixVoxelImageFormat.py.
                if(NOT VOXAGINE_PYTHON3)
                    message(FATAL_ERROR
                        "python3 is required with glslc to correct the voxel buffer "
                        "image format (install python3, or use DXC).")
                endif()

                set(GLSLC_POST_COMMANDS "")

                # glslang's HLSL front end does not implement
                # NonUniformResourceIndex.  The UI pixel shader indexes its
                # texture array per sprite, so turn that one variable access
                # into a switch of literal descriptor accesses.  This is the
                # SPIRV-Tools portability pass intended for targets without
                # reliable non-uniform descriptor indexing (including
                # MoltenVK/Metal argument buffers).
                if(SHADER_FULL STREQUAL "UIRenderer.ps.hlsl")
                    if(NOT SPIRV_OPT_EXECUTABLE)
                        message(FATAL_ERROR "spirv-opt is required with glslc for UIRenderer.ps.hlsl (install SPIRV-Tools or DXC).")
                    endif()

                    set(GLSLC_OUTPUT ${SPIRV_OUT}.unoptimized)
                    # Prepended, not assigned: spirv-opt has to read the raw
                    # glslc output and write ${SPIRV_OUT}, and the image-format
                    # fix above then runs on that finished module.
                    set(GLSLC_POST_COMMANDS
                        COMMAND ${SPIRV_OPT_EXECUTABLE}
                                --replace-desc-array-access-using-var-index
                                # The replacement pass emits the literal-index
                                # switch but leaves the original dynamic sample
                                # as dead SPIR-V. Metal may still execute that
                                # argument-buffer access, so remove it here.
                                --eliminate-dead-code-aggressive
                                ${GLSLC_OUTPUT}
                                -o ${SPIRV_OUT}
                        COMMAND ${CMAKE_COMMAND} -E rm -f ${GLSLC_OUTPUT})
                endif()

                # Last, so it runs on the finished module in both cases - after
                # spirv-opt has written ${SPIRV_OUT} for UIRenderer.ps, and
                # directly on glslc's output for everything else.
                list(APPEND GLSLC_POST_COMMANDS
                    COMMAND ${VOXAGINE_PYTHON3}
                            ${VOXAGINE_SHADERS_CMAKE_DIR}/FixVoxelImageFormat.py
                            ${SPIRV_OUT})

                add_custom_command(
                    OUTPUT ${SPIRV_OUT}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUTPUT_DIR}
                    COMMAND ${GLSLC_EXECUTABLE}
                            -x hlsl -fshader-stage=${GLSLC_STAGE}
                            # HLSL register classes are independent. Without
                            # this, glslc ignores the UAV base below for an
                            # explicitly declared register(uN), colliding with
                            # b0 at Vulkan binding 0. MoltenVK then emits two
                            # Metal resources at the same index and every
                            # affected pipeline fails on iOS.
                            -fhlsl-iomap
                            # glslc's equivalent of dxc's -fvk-use-dx-layout,
                            # which the engine needs because it memcpys tightly
                            # packed C++ structs into structured buffers.
                            # Passing it explicitly is belt-and-braces rather
                            # than a fix: glslc already applies HLSL offset
                            # rules to HLSL input, and adding this produced
                            # byte-identical SPIR-V for all 24 modules. It is
                            # here so a future change to that default cannot
                            # silently repack the buffers.
                            -fhlsl-offsets
                            --target-env=vulkan1.2
                            -fubo-binding-base 0
                            -ftexture-binding-base 100
                            -fuav-binding-base 200
                            -fsampler-binding-base 300
                            -fauto-map-locations
                            ${VOXAGINE_DXC_DEFINES}
                            -o ${GLSLC_OUTPUT}
                            ${SHADER}
                    ${GLSLC_POST_COMMANDS}
                    # Compiler options are part of a shader's ABI. Rebuild
                    # modules when this file changes rather than keeping a
                    # stale SPIR-V binary that reflects old binding rules.
                    DEPENDS ${SHADER} ${SHADER_INCLUDES} ${VOXAGINE_SHADERS_CMAKE_FILE}
                    COMMENT "glslc ${PROFILE} ${SHADER_FULL}"
                    VERBATIM
                )
            endif()

            list(APPEND SPIRV_OUTPUTS ${SPIRV_OUT})
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPIRV_OUTPUTS})

    list(LENGTH SPIRV_OUTPUTS SPIRV_COUNT)
    message(STATUS "Shaders: ${TARGET_NAME} -> ${SPIRV_COUNT} SPIR-V modules")
endfunction()
