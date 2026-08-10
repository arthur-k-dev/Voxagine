cmake_minimum_required(VERSION 3.21)

project(Voxagine
    VERSION 0.1.0
    DESCRIPTION "Voxagine engine and Bit Buster"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Options
#
# VOXAGINE_BUILD_ENGINE defaults off so a plain configure stays offline-capable
# (no RTTR fetch). Use the game/editor presets in CMakePresets.json to build
# BitBuster.
# ---------------------------------------------------------------------------
option(VOXAGINE_BUILD_BRINGUP "Build the Vulkan bring-up target" ON)
option(VOXAGINE_BUILD_ENGINE  "Build the engine library" OFF)
option(VOXAGINE_BUILD_EDITOR  "Build the game with the editor (EDITOR define)" ON)
option(VOXAGINE_FETCH_RTTR    "Fetch RTTR from upstream instead of find_package" ON)
option(VOXAGINE_CHECK_RTTR    "Compile the reflection TU to prove RTTR works" OFF)
set(VOXAGINE_AUDIO_BACKEND "MINIAUDIO" CACHE STRING
    "Audio backend: MINIAUDIO (vendored, all platforms) or NONE (silent)")
set_property(CACHE VOXAGINE_AUDIO_BACKEND PROPERTY STRINGS MINIAUDIO NONE)
option(VOXAGINE_ENABLE_OPTICK "Enable Optick profiling" OFF)
option(VOXAGINE_BUILD_TESTS   "Build the gtest suite (needs VOXAGINE_BUILD_ENGINE)" OFF)
option(VOXAGINE_BUILD_GPU_TESTS "Build the opt-in headless GPU integration test" OFF)
option(VOXAGINE_PROFILE_DEFAULT "Default the frame profiler on in an optimised build" OFF)
set(VOXAGINE_ASSET_VERSION "dev" CACHE STRING
    "Stamped into the extracted mobile asset tree; a change re-extracts it")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(VOXAGINE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Voxagine/Source)

# ---------------------------------------------------------------------------
# Platform selection. Must come before the dependencies, which it decides how
# to acquire (a cross toolchain cannot use the host's SDL3).
# ---------------------------------------------------------------------------
include(CMake/Platforms.cmake)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if(NOT VOXAGINE_IOS)
    find_package(Vulkan REQUIRED)
endif()

# RTTR 0.9.6 is what this engine was written against, and its reflection macros
# are load-bearing for save/load and the editor's property inspector. It used
# to live in External/rttr as headers only, with no sources and only a Windows
# .lib to link - which is why nothing here could build on Linux. That copy is
# gone; includes are plain <rttr/...> and resolve to whatever this provides.
#
# Only pulled in when something actually needs it, so the default bring-up
# build stays offline-capable.
if(VOXAGINE_BUILD_ENGINE OR VOXAGINE_CHECK_RTTR)
    include(CMake/RTTR.cmake)
endif()

include(CMake/SDL3.cmake)

# ---------------------------------------------------------------------------
# Warning set shared by our own targets. Vendored code is included as SYSTEM
# so its warnings stay out of the way.
# ---------------------------------------------------------------------------
# MSVC defines _DEBUG automatically in debug configurations and the engine
# leans on it in ~90 places (debug draw, validation, asserts). GCC does not, so
# a Debug build here would silently behave like a release one.
add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)


add_library(voxagine_warnings INTERFACE)
target_compile_options(voxagine_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wno-unused-parameter>
)

# Tests are one target, built by VOXAGINE_BUILD_TESTS further down. There used
# to be two standalone executables here as well - a worker-count test and a
# DateTime test, each with its own main - which ran without the engine and so
# without VOXAGINE_BUILD_TESTS. Both are checks in the one suite now; a second
# way to write a test is a way that gets forgotten.
include(CTest)

# ---------------------------------------------------------------------------
# Vulkan backend
#
# No engine dependencies by design, so it can be built and run before the rest
# of Core compiles on Linux.
# ---------------------------------------------------------------------------
add_library(voxagine_vulkan STATIC
    # volk: Vulkan is loaded at runtime rather than linked. Not a preference -
    # Android's libvulkan.so exports nothing past 1.1 and this renderer is
    # built on core-1.3 sync2 and dynamic rendering. VulkanAPI.h has the whole
    # reasoning; it is the only file that includes vulkan.h.
    ${VOXAGINE_SOURCE_DIR}/External/volk/volk.c
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKAllocator.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKCommandEngine.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKDescriptorLayout.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKDevice.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKPassBindings.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKResource.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKSwapchain.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKUploadBuffer.cpp
    # Not Vulkan-specific, but VKCommandEngine.cpp reports into it and it has
    # no engine dependencies of its own (unlike most of Core/Platform/Rendering,
    # it deliberately doesn't include pch.h) - see EngineSources.cmake, which
    # does NOT list this file, to avoid compiling it into both static libraries
    # and hitting a duplicate-symbol link error in editor/game.
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/FrameProfiler.cpp
)

target_include_directories(voxagine_vulkan PUBLIC ${VOXAGINE_SOURCE_DIR})
target_link_libraries(voxagine_vulkan PRIVATE voxagine_warnings)
voxagine_link_vulkan(voxagine_vulkan)

# Vendored, and generated code at that.
set_source_files_properties(${VOXAGINE_SOURCE_DIR}/External/volk/volk.c
    PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------
include(CMake/Shaders.cmake)

# SPIR-V lands next to the HLSL it came from, because the engine asks for
# shaders by asset path - "Engine/Assets/Shaders/Particles.vs" - and appends
# .spv. Keeping them in the build tree would mean the runtime could not find
# them. They are build output, so .gitignore excludes *.spv.
set(VOXAGINE_SHADER_DEFINES "")

# **No shader is compiled differently per platform any more, and that is worth
# knowing rather than rediscovering.** VOXAGINE_MOBILE used to be passed to dxc
# so Defines.hlsl could halve the two cone-trace step counts on a phone; the
# render quality those knobs stood in for is a runtime setting now
# (CameraData.hlsl's renderQuality), so the same .spv serves every platform and
# the player - not the build - decides what it costs.
#
# That closes the hazard CMake/Shaders.cmake describes at length: the shader
# output path is not platform-qualified, so whichever preset configured last
# used to decide what every other preset's asset root picked up. With no
# per-platform defines there is nothing to differ. Re-introducing one brings the
# hazard back with it, so prefer a uniform in the constant buffer.


voxagine_add_shaders(voxagine_shaders_game
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Game/Engine/Assets/Shaders
    OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Game/Engine/Assets/Shaders
    DEFINES ${VOXAGINE_SHADER_DEFINES}
)

# ---------------------------------------------------------------------------
# Bring-up target: SDL3 window + Vulkan clear screen
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_BRINGUP)
    add_executable(voxagine_bringup
        ${VOXAGINE_SOURCE_DIR}/Bringup/VulkanBringup.cpp
    )

    target_link_libraries(voxagine_bringup
        PRIVATE voxagine_vulkan SDL3::SDL3 voxagine_warnings
    )
endif()

# ---------------------------------------------------------------------------
# Engine library
#
# Off by default so a plain configure stays offline-capable, not because
# anything is blocking it any more - the engine, the game and the editor all
# build and run. Audio is miniaudio (vendored, no SDK), file dialogs are
# editor-only (see VOXAGINE_BUILD_EDITOR below), and Optick's vendored headers
# still reference a Windows-only OptickCore.lib so USE_OPTICK stays 0.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_ENGINE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/EngineSources.cmake)

    add_library(voxagine STATIC ${VOXAGINE_ENGINE_SOURCES})

    # The vendored STB .c files open with #include "pch.h", so they are only
    # valid as C++. The old build compiled every file in the project as C++
    # with a forced PCH; CMake picks the language from the extension.
    set_source_files_properties(
        ${VOXAGINE_SOURCE_DIR}/External/STB/image_DXT.c
        ${VOXAGINE_SOURCE_DIR}/External/STB/image_helper.c
        ${VOXAGINE_SOURCE_DIR}/External/STB/stb_image_aug.c
        PROPERTIES LANGUAGE CXX
    )

    # Vendored sources are compiled straight into the voxagine target (see
    # EngineSources.cmake), so they inherit -Wall -Wextra from voxagine_warnings
    # like everything else - unlike External/*.h, which is only ever reached by
    # #include and stays out of the way on its own. -w keeps our own warning set
    # meaningful without patching code that isn't ours to fix.
    set_source_files_properties(
        ${VOXAGINE_SOURCE_DIR}/External/STB/image_DXT.c
        ${VOXAGINE_SOURCE_DIR}/External/STB/image_helper.c
        ${VOXAGINE_SOURCE_DIR}/External/STB/stb_image_aug.c
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_demo.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_draw.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_dropdown.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_stdlib.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_widgets.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imguizmo/ImGuizmo.cpp
        ${VOXAGINE_SOURCE_DIR}/External/imguizmo/ImSequencer.cpp
        ${VOXAGINE_SOURCE_DIR}/External/teenypath/teenypath.cpp
        PROPERTIES COMPILE_OPTIONS "-w"
    )

    target_include_directories(voxagine PUBLIC ${VOXAGINE_SOURCE_DIR})

    target_link_libraries(voxagine
        PUBLIC voxagine_vulkan SDL3::SDL3 RTTR::Core
        PRIVATE voxagine_warnings
    )

    # GetOpenFileName, for the editor's file dialogs.
    if(WIN32)
        target_link_libraries(voxagine PRIVATE comdlg32)
    endif()

    if(VOXAGINE_ANDROID)
        target_link_libraries(voxagine PRIVATE android log)
    endif()

    # Stamped into the extracted asset tree so an app update re-extracts rather
    # than silently running the previous version's content. Gradle passes the
    # app's versionName; a plain CMake build gets "dev", which re-extracts only
    # when the tree is missing.
    target_compile_definitions(voxagine PUBLIC
        VOXAGINE_ASSET_VERSION="${VOXAGINE_ASSET_VERSION}")

    if(VOXAGINE_PROFILE_DEFAULT)
        target_compile_definitions(voxagine PUBLIC VOXAGINE_PROFILE_DEFAULT)
    endif()

    # No target_precompile_headers here on purpose.
    #
    # Every engine TU already starts with #include "pch.h". CMake's PCH support
    # force-includes its own cmake_pch.h on top of that, so headers reached
    # both ways get pulled in twice and #pragma once does not save them -
    # moodycamel's concurrentqueue.h then reports every declaration as a
    # redefinition of itself. Adopting CMake's PCH means removing the explicit
    # includes from all 147 TUs first; not worth doing mid-port.

    # Audio. The backend is additive rather than a swap: NullAudioContext is
    # always compiled and AA_NONE still selects it at runtime, so a silent run
    # needs no rebuild.
    if(VOXAGINE_AUDIO_BACKEND STREQUAL "MINIAUDIO")
        target_sources(voxagine PRIVATE ${VOXAGINE_MINIAUDIO_SOURCES})
        target_compile_definitions(voxagine PUBLIC VOXAGINE_MINIAUDIO)

        # 4 MB of vendored single-header C, like the STB sources above.
        set_source_files_properties(
            ${VOXAGINE_SOURCE_DIR}/External/miniaudio/miniaudio_impl.c
            ${VOXAGINE_SOURCE_DIR}/External/miniaudio/stb_vorbis.c
            PROPERTIES COMPILE_OPTIONS "-w"
        )

        # miniaudio's backends dlopen their audio libraries at runtime (ALSA,
        # PulseAudio, PipeWire), so there is nothing to find_package here - but
        # it does need the C runtime pieces that do that.
        if(UNIX AND NOT APPLE AND NOT ANDROID)
            find_package(Threads REQUIRED)
            target_link_libraries(voxagine PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
        endif()
    elseif(NOT VOXAGINE_AUDIO_BACKEND STREQUAL "NONE")
        message(FATAL_ERROR
            "VOXAGINE_AUDIO_BACKEND must be MINIAUDIO or NONE, got '${VOXAGINE_AUDIO_BACKEND}'")
    endif()

    if(VOXAGINE_ENABLE_OPTICK)
        target_compile_definitions(voxagine PUBLIC USE_OPTICK=1)
    else()
        target_compile_definitions(voxagine PUBLIC USE_OPTICK=0)
    endif()

    # PUBLIC, and on the library: Editor.cpp and the #ifdef EDITOR branch of
    # Application::Run are both library sources, so defining this only on the
    # executable compiled the whole editor out.
    #
    # The sources are conditional too, which they were not before: Editor.cpp
    # has no #ifdef EDITOR around its body, so a game build compiled the entire
    # editor - and through FileBrowser, the desktop file dialog. See the
    # VOXAGINE_EDITOR_SOURCES comment in EngineSources.cmake.
    if(VOXAGINE_BUILD_EDITOR)
        target_sources(voxagine PRIVATE ${VOXAGINE_EDITOR_SOURCES})
        target_compile_definitions(voxagine PUBLIC EDITOR)
    endif()
endif()

# ---------------------------------------------------------------------------
# Bit Buster
#
# EDITOR selects the editor build; the old .sln expressed this as separate
# *Editor and *Game configurations.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_ENGINE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/GameSources.cmake)

    if(VOXAGINE_ANDROID)
        # An Android app has no executable of its own. SDLActivity dlopens the
        # libraries named by getLibraries() - "SDL3" then "main" - and calls
        # SDL_main in the second, so the game has to *be* libmain.so. The
        # OUTPUT_NAME is what makes the file name match what the Java loader
        # asks for; renaming it means overriding getLibraries() in the activity.
        add_library(BitBuster SHARED ${VOXAGINE_GAME_SOURCES})
        set_target_properties(BitBuster PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(BitBuster ${VOXAGINE_GAME_SOURCES})
    endif()

    target_include_directories(BitBuster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Game/Source)
    # WHOLE_ARCHIVE, and it is load-bearing rather than tidiness.
    #
    # RTTR registrations are static initializers in an anonymous namespace, so
    # a translation unit whose only job is reflection exports nothing anyone
    # calls - and a linker pulls an object out of a static archive only when
    # something references a symbol in it. Core/Settings.cpp is exactly that
    # shape: 51 lines, one RTTR_REGISTRATION block and one out-of-line setter
    # nothing calls. It was silently dropped from every build, so the type had
    # zero registered properties and JsonSerializer applied none of them.
    #
    # Nothing reported it. FromJsonFile returns true once the file *opens*,
    # FromJson logs only when "Data"/"Type" are missing, and get_properties()
    # on an unregistered type is empty rather than invalid - so Settings.vgs
    # parsed, matched, iterated zero properties and left every value at its
    # compiled-in default. FrameLimit, EnableVSync, Fullscreen, FXAA, shadows,
    # ResolutionScale and the window size were all inert for the whole port.
    #
    # Any future reflection-only TU has the same shape, which is why this is
    # fixed at the link rather than by giving Settings.cpp an anchor symbol.
    target_link_libraries(BitBuster PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,voxagine>"
        voxagine_warnings)

    # EDITOR itself is defined on the voxagine library above, PUBLIC, so both
    # the library and this executable see it.

    if(VOXAGINE_IOS)
        # NEVER BUILT. Written without Xcode or a Mac anywhere in reach, so
        # treat all of this as a first draft - Docs/MOBILE_PORT_LOG.md says so at
        # more length. The pieces are conventional; none is verified.
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Platforms/iOS/Info.plist.in
                       ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

        set_target_properties(BitBuster PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.voxagine.bitbuster"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${VOXAGINE_IOS_MIN_VERSION}"
            # Signing is the user's identity and their decision. An unsigned
            # build still compiles and links, which is all CI could ever check.
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
        )

        # The content tree goes into the bundle wholesale rather than as
        # individual RESOURCE entries: it is ~100 MB across thousands of files
        # and CMake's per-file resource handling would list every one of them.
        # MobileAssets then copies it out of the bundle on first launch,
        # because the bundle is read-only and this engine writes through the
        # same relative paths it reads.
        add_custom_command(TARGET BitBuster POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/Game
                $<TARGET_BUNDLE_CONTENT_DIR:BitBuster>
            COMMENT "Copying the content tree into the app bundle")
    endif()

    # Guarded because voxagine_add_shaders returns without creating the target
    # when dxc is missing, and add_dependencies on a target that does not exist
    # is a generate-time error. That is not hypothetical: it is what stopped CI
    # from building the engine at all, and CI has no Vulkan SDK. The executable
    # still links; it just cannot render until someone compiles the shaders.
    if(TARGET voxagine_shaders_game)
        add_dependencies(BitBuster voxagine_shaders_game)
    endif()

    # A separate executable on purpose: this is a renderer integration test,
    # not part of Tests/Framework's CPU-only suite. Its fixture owns the test
    # application and drives real destruction; VoxApp remains production-only.
    if(VOXAGINE_BUILD_GPU_TESTS AND NOT VOXAGINE_BUILD_EDITOR)
        set(VOXAGINE_GPU_TEST_SOURCES ${VOXAGINE_GAME_SOURCES})
        list(REMOVE_ITEM VOXAGINE_GPU_TEST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/Game/Source/main.cpp)

        add_executable(voxagine_gpu_destruction_stress
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Rendering/DestructionSyncStress.cpp
            ${VOXAGINE_GPU_TEST_SOURCES})
        target_include_directories(voxagine_gpu_destruction_stress PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Game/Source)
        target_link_libraries(voxagine_gpu_destruction_stress PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,voxagine>"
            voxagine_warnings)

        if(TARGET voxagine_shaders_game)
            add_dependencies(voxagine_gpu_destruction_stress voxagine_shaders_game)
        endif()

        add_test(NAME gpu_destruction_sync_stress
            COMMAND voxagine_gpu_destruction_stress
                --hidden --uncapped --size 640x360 --frames 6000
                --map Content/Worlds/Fishing_Village/Fishing_Village_Beat1.wld)
        set_tests_properties(gpu_destruction_sync_stress PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Game
            LABELS "gpu;integration"
            RUN_SERIAL TRUE
            TIMEOUT 180)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
#
# One target, three modes: `checks`, `scenarios` and `perf`. See Tests/README.md
# for what each is for and how to add a case to it.
#
# It links the engine library and runs with no GPU and no display, which is what
# lets CI cover it - see Tests/Harness/VoxelWorldHarness.h for why the voxel
# write path is reachable without either.
#
# Not linked WHOLE_ARCHIVE, unlike BitBuster: the tests do not read Settings.vgs
# and do not need the reflection-only translation units dragged in. If a test
# ever does need RTTR registrations, it needs the same treatment and the comment
# on BitBuster explains why.
#
# No GTest dependency any more. The assertion macros in Tests/Framework/Check.h
# are a few dozen lines and cover what the suite used, and one registry that can
# hold checks, scenarios, invariants and benchmarks is worth more here than a
# third-party one that can only hold the first.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_TESTS)
    if(NOT VOXAGINE_BUILD_ENGINE)
        message(FATAL_ERROR "VOXAGINE_BUILD_TESTS needs VOXAGINE_BUILD_ENGINE=ON")
    endif()

    include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/TestSources.cmake)

    add_executable(voxagine_tests ${VOXAGINE_TEST_SOURCES})

    target_include_directories(voxagine_tests PRIVATE ${VOXAGINE_TEST_DIR})
    target_link_libraries(voxagine_tests PRIVATE voxagine voxagine_warnings)

    # A few checks want a real shipped asset rather than a synthetic one - the
    # audio decode check, which exists to prove the vendored Vorbis wiring is
    # still compiled in, cannot be written against a made-up file. The game's
    # content root is passed in rather than resolved relative to the CWD,
    # because ctest runs the binary from the build directory.
    target_compile_definitions(voxagine_tests PRIVATE
        VOXAGINE_TEST_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Game")

    # Assertions about one unit at a time. Seconds.
    add_test(NAME checks COMMAND voxagine_tests checks)

    # Every scenario against every invariant, twice each for determinism. Under
    # a minute in Release and several times that in Debug, which is the price of
    # running the real pipeline over real worlds rather than a mock of it.
    add_test(NAME scenarios COMMAND voxagine_tests scenarios)

    # The benchmarks, gated on their machine-independent work counters. Timings
    # are printed but cannot fail the run: the baseline file holds only the
    # exact counts, precisely so this test means the same thing on a CI runner
    # as it does on a workstation. Framework/Benchmark.h has the reasoning.
    add_test(NAME perf COMMAND voxagine_tests perf
        --baseline ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Baselines/perf.txt)
endif()

# ---------------------------------------------------------------------------
# Reflection check
#
# Core/Settings.cpp is the smallest real engine TU that exercises RTTR the way
# the rest of the engine does: an enum registration plus member-pointer and
# getter/setter properties. Compiling it proves the RTTR dependency is usable
# on this platform without building the whole engine. Off by default because
# it needs the network.
# ---------------------------------------------------------------------------
if(VOXAGINE_CHECK_RTTR AND NOT VOXAGINE_BUILD_ENGINE)
    add_library(voxagine_reflection_check OBJECT
        ${VOXAGINE_SOURCE_DIR}/Core/Settings.cpp
    )

    target_include_directories(voxagine_reflection_check PRIVATE ${VOXAGINE_SOURCE_DIR})
    target_link_libraries(voxagine_reflection_check PRIVATE RTTR::Core)
endif()

message(STATUS "Voxagine: bring-up=${VOXAGINE_BUILD_BRINGUP} engine=${VOXAGINE_BUILD_ENGINE} rttr-check=${VOXAGINE_CHECK_RTTR}")
