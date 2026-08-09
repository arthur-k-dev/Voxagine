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
option(VOXAGINE_ENABLE_FMOD   "Enable FMOD audio (needs the FMOD Linux SDK)" OFF)
option(VOXAGINE_ENABLE_OPTICK "Enable Optick profiling" OFF)
option(VOXAGINE_ENABLE_NFD    "Enable native file dialogs" OFF)
option(VOXAGINE_BUILD_TESTS   "Build the gtest suite (needs VOXAGINE_BUILD_ENGINE)" OFF)

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
# Dependencies
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# RTTR 0.9.6 is what this engine was written against, and its reflection macros
# are load-bearing for save/load and the editor's property inspector. It used
# to live in External/rttr as headers only, with no sources and only a Windows
# .lib to link - which is why nothing here could build on Linux. That copy is
# gone; includes are plain <rttr/...> and resolve to whatever this provides.
#
# Only pulled in when something actually needs it, so the default bring-up
# build stays offline-capable.
if(VOXAGINE_BUILD_ENGINE OR VOXAGINE_CHECK_RTTR)
    include(cmake/RTTR.cmake)
endif()

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
endif()

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

include(CTest)
if(BUILD_TESTING)
    add_executable(voxagine_job_manager_config_test
        UnitTesting/UnitTests/Source/Core/Threading/JobManagerConfigTest.cpp
    )
    target_include_directories(voxagine_job_manager_config_test PRIVATE ${VOXAGINE_SOURCE_DIR})
    target_link_libraries(voxagine_job_manager_config_test PRIVATE voxagine_warnings)
    add_test(NAME job_manager_worker_count COMMAND voxagine_job_manager_config_test)

    add_executable(voxagine_date_time_test
        UnitTesting/UnitTests/Source/Core/Utils/DateTimeTest.cpp
    )
    target_include_directories(voxagine_date_time_test PRIVATE ${VOXAGINE_SOURCE_DIR})
    target_link_libraries(voxagine_date_time_test PRIVATE voxagine_warnings)
    add_test(NAME date_time_utc_conversion COMMAND voxagine_date_time_test)
endif()

# ---------------------------------------------------------------------------
# Vulkan backend
#
# No engine dependencies by design, so it can be built and run before the rest
# of Core compiles on Linux.
# ---------------------------------------------------------------------------
add_library(voxagine_vulkan STATIC
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
target_link_libraries(voxagine_vulkan
    PUBLIC Vulkan::Vulkan
    PRIVATE voxagine_warnings
)

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------
include(cmake/Shaders.cmake)

# SPIR-V lands next to the HLSL it came from, because the engine asks for
# shaders by asset path - "Engine/Assets/Shaders/Particles.vs" - and appends
# .spv. Keeping them in the build tree would mean the runtime could not find
# them. They are build output, so .gitignore excludes *.spv.
voxagine_add_shaders(voxagine_shaders_game
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Game/Engine/Assets/Shaders
    OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Game/Engine/Assets/Shaders
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
# Off by default. Still blocked on:
#   FMOD    - proprietary, needs the Linux SDK downloaded by hand.
#   Optick  - vendored headers reference a Windows-only OptickCore.lib.
#   nfd     - needs the GTK backend built from upstream source.
#   teenypath - replaceable with std::filesystem.
# and on the Vulkan render context, managers and passes not existing yet.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_ENGINE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineSources.cmake)

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

    # No target_precompile_headers here on purpose.
    #
    # Every engine TU already starts with #include "pch.h". CMake's PCH support
    # force-includes its own cmake_pch.h on top of that, so headers reached
    # both ways get pulled in twice and #pragma once does not save them -
    # moodycamel's concurrentqueue.h then reports every declaration as a
    # redefinition of itself. Adopting CMake's PCH means removing the explicit
    # includes from all 147 TUs first; not worth doing mid-port.

    if(VOXAGINE_ENABLE_FMOD)
        # The real FMOD implementations replace the silent stubs, which define
        # the same symbols.
        list(REMOVE_ITEM VOXAGINE_ENGINE_SOURCES
            ${VOXAGINE_SOURCE_DIR}/Core/Platform/Audio/NullAudioContext.cpp
            ${VOXAGINE_SOURCE_DIR}/Core/Resources/Formats/NullSoundReference.cpp
        )
        target_sources(voxagine PRIVATE ${VOXAGINE_FMOD_SOURCES})
        target_compile_definitions(voxagine PUBLIC VOXAGINE_FMOD)
    endif()

    if(VOXAGINE_ENABLE_OPTICK)
        target_compile_definitions(voxagine PUBLIC USE_OPTICK=1)
    else()
        target_compile_definitions(voxagine PUBLIC USE_OPTICK=0)
    endif()

    if(VOXAGINE_ENABLE_NFD)
        target_compile_definitions(voxagine PUBLIC VOXAGINE_NFD)
    endif()

    # PUBLIC, and on the library: Editor.cpp and the #ifdef EDITOR branch of
    # Application::Run are both library sources, so defining this only on the
    # executable compiled the whole editor out.
    if(VOXAGINE_BUILD_EDITOR)
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
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GameSources.cmake)

    add_executable(BitBuster ${VOXAGINE_GAME_SOURCES})

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

    # Guarded because voxagine_add_shaders returns without creating the target
    # when dxc is missing, and add_dependencies on a target that does not exist
    # is a generate-time error. That is not hypothetical: it is what stopped CI
    # from building the engine at all, and CI has no Vulkan SDK. The executable
    # still links; it just cannot render until someone compiles the shaders.
    if(TARGET voxagine_shaders_game)
        add_dependencies(BitBuster voxagine_shaders_game)
    endif()
endif()

# ---------------------------------------------------------------------------
# Unit tests and the destruction gauntlet
#
# Both link the engine library and both run with no GPU and no display, which
# is what lets CI cover them - see UnitTesting/UnitTests/Source/Harness/
# VoxelWorldHarness.h for why the voxel write path is reachable that way.
#
# Not linked WHOLE_ARCHIVE, unlike BitBuster: these do not read Settings.vgs and
# do not need the reflection-only translation units dragged in. If a test ever
# does need RTTR registrations, it needs the same treatment and the comment on
# BitBuster explains why.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_TESTS)
    if(NOT VOXAGINE_BUILD_ENGINE)
        message(FATAL_ERROR "VOXAGINE_BUILD_TESTS needs VOXAGINE_BUILD_ENGINE=ON")
    endif()

    find_package(GTest REQUIRED)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestSources.cmake)

    add_executable(voxagine_tests ${VOXAGINE_TEST_SOURCES})

    target_include_directories(voxagine_tests PRIVATE ${VOXAGINE_TEST_DIR})
    target_link_libraries(voxagine_tests PRIVATE
        voxagine GTest::gtest GTest::gtest_main voxagine_warnings)

    add_test(NAME voxagine_unit_tests COMMAND voxagine_tests)

    add_executable(voxagine_gauntlet
        ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting/Gauntlet/Gauntlet.cpp
        ${VOXAGINE_TEST_DIR}/Harness/VoxelWorldHarness.cpp
    )

    target_include_directories(voxagine_gauntlet PRIVATE ${VOXAGINE_TEST_DIR})
    target_link_libraries(voxagine_gauntlet PRIVATE voxagine voxagine_warnings)

    # Runs the built-in script and fails on any representation disagreement.
    # The state hash is printed rather than asserted here: it is a property of
    # the build, and CI matrixes two of them.
    add_test(NAME voxagine_gauntlet COMMAND voxagine_gauntlet)

    # The scenario suite. Distinct from the gauntlet, which is one long scripted
    # run measured for cost: this asserts the destruction system's invariants
    # over several deliberately-shaped worlds plus a batch of randomised ones,
    # and exits non-zero on the first that fails. See UnitTesting/Selftest.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SelftestSources.cmake)

    add_executable(voxagine_selftest ${VOXAGINE_SELFTEST_SOURCES})

    # Scenarios and invariants register themselves from static initialisers, so
    # these have to be compiled straight into the executable rather than reached
    # through a static archive - a linker pulls an object out of an archive only
    # when something references a symbol in it, and nothing references these.
    # Same shape as Core/Settings.cpp; see the WHOLE_ARCHIVE note on BitBuster.
    target_include_directories(voxagine_selftest PRIVATE
        ${VOXAGINE_TEST_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting
        ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting/Selftest)
    target_link_libraries(voxagine_selftest PRIVATE voxagine voxagine_warnings)

    add_test(NAME voxagine_selftest COMMAND voxagine_selftest)
endif()

# ---------------------------------------------------------------------------
# Reflection check
#
# Core/Settings.cpp is the smallest real engine TU that exercises RTTR the way
# the rest of the engine does: an enum registration plus member-pointer and
# getter/setter properties. Compiling it proves the RTTR dependency is usable
# on this platform while the full engine target is still blocked on FMOD and
# the Vulkan render context. Off by default because it needs the network.
# ---------------------------------------------------------------------------
if(VOXAGINE_CHECK_RTTR AND NOT VOXAGINE_BUILD_ENGINE)
    add_library(voxagine_reflection_check OBJECT
        ${VOXAGINE_SOURCE_DIR}/Core/Settings.cpp
    )

    target_include_directories(voxagine_reflection_check PRIVATE ${VOXAGINE_SOURCE_DIR})
    target_link_libraries(voxagine_reflection_check PRIVATE RTTR::Core)
endif()

message(STATUS "Voxagine: bring-up=${VOXAGINE_BUILD_BRINGUP} engine=${VOXAGINE_BUILD_ENGINE} rttr-check=${VOXAGINE_CHECK_RTTR}")
