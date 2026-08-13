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

# The file dialog backends on Apple platforms are Objective-C++ (.mm): AppKit's
# NSOpenPanel on macOS, UIKit on iOS. CMake picks OBJCXX from the extension but
# the language still has to be enabled, and enable_language() is only valid at
# directory scope - not inside the if(VOXAGINE_BUILD_EDITOR) block that adds
# them. Enabling it costs one extra compiler check at configure time.
if(APPLE AND VOXAGINE_BUILD_EDITOR)
    enable_language(OBJCXX)
endif()

# miniaudio's iOS backend includes AVFoundation/Foundation Objective-C
# declarations. Keep the implementation C everywhere else, but compile its
# single translation unit as Objective-C on iOS so the backend is usable.
if(VOXAGINE_IOS AND VOXAGINE_AUDIO_BACKEND STREQUAL "MINIAUDIO")
    enable_language(OBJC)
    set_source_files_properties(
        ${VOXAGINE_SOURCE_DIR}/External/miniaudio/miniaudio_impl.c
        PROPERTIES LANGUAGE OBJC)
endif()

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
# enable_testing(), not include(CTest). The module is CDash machinery: it adds
# twenty-eight custom targets - Continuous*, Nightly*, Experimental* - that
# submit builds to a dashboard this project does not have. Nothing here uses
# them, and they are not free: an IDE reading the CMake model turns every one
# into a run configuration, which buried the four real ones (Game, Editor and
# friends) under the auto-generated list. `ctest` and `add_test` need only this.
enable_testing()

# ---------------------------------------------------------------------------
# Vulkan backend
#
# No engine dependencies by design, so it can be built and run before the rest
# of Core compiles on Linux.
# ---------------------------------------------------------------------------
set(VOXAGINE_VULKAN_LOADER_SOURCE)
if(NOT VOXAGINE_IOS)
    list(APPEND VOXAGINE_VULKAN_LOADER_SOURCE
        ${VOXAGINE_SOURCE_DIR}/External/volk/volk.c)
endif()

add_library(voxagine_vulkan STATIC
    # volk: Vulkan is loaded at runtime rather than linked. Not a preference -
    # Android's libvulkan.so exports nothing past 1.1 and this renderer is
    # built on core-1.3 sync2 and dynamic rendering. VulkanAPI.h has the whole
    # reasoning; it is the only file that includes vulkan.h.
    ${VOXAGINE_VULKAN_LOADER_SOURCE}
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

    # NSOpenPanel/NSSavePanel, for the same reason. iOS gets UIKit and
    # Foundation from voxagine_link_vulkan already, so only macOS is named.
    if(APPLE AND NOT VOXAGINE_IOS AND VOXAGINE_BUILD_EDITOR)
        target_link_libraries(voxagine PRIVATE "-framework AppKit")
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

        # ARC, for the two Objective-C++ file dialog backends only.
        #
        # CMake compiles .mm under manual reference counting by default, which
        # is not what modern AppKit/UIKit code is written against and not what
        # these are: nfd_uikit.mm holds a __weak reference across a UIAlertAction
        # block, and __weak is an ARC construct - clang rejects it outright
        # under MRC rather than degrading. Under MRC the view controllers it
        # allocates would also have to be released by hand, in a file whose
        # objects outlive the call that made them.
        #
        # Scoped to these files rather than set on the target: the rest of the
        # library is C++ and unaffected either way, and a global -fobjc-arc
        # would silently change how miniaudio's Objective-C translation unit is
        # compiled on iOS.
        set_source_files_properties(
            ${VOXAGINE_SOURCE_DIR}/External/nativefiledialog/nfd_cocoa.mm
            ${VOXAGINE_SOURCE_DIR}/External/nativefiledialog/nfd_uikit.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
endif()

# Copies the shipped asset tree into an app bundle.
#
# The trees go in wholesale rather than as individual RESOURCE entries: they are
# ~95 MB across thousands of files and CMake's per-file resource handling would
# list every one of them.
#
# Named directories rather than all of Game/, which is what this used to copy.
# That swept the whole desktop working directory into the app - the game's own
# Source/ tree, both Optick DLLs, the Visual Studio .rc and .aps files, and
# SmallHouseV2, a directory of generated remainder models nothing references.
# None of it is reachable on a device, and MobileAssets then extracted a second
# copy of all of it at startup.
#
# Keep this list in step with MobileAssets' k_pAssetRoots and its loose files:
# anything shipped here but not extracted is dead weight, and anything extracted
# but not shipped is a missing-file failure at runtime.
#
# DESTINATION differs per platform and must match what SDL_GetBasePath returns
# there - the bundle root on iOS, Contents/Resources on macOS. The caller passes
# it because only the caller knows which platform it is building for.
function(voxagine_bundle_assets target destination)
    # UserSettings.vguser holds the editor's startup world, and only an editor
    # build has any use for it. Leaving it out is not harmless: with no user
    # settings the editor opens with no world at all, which is what a bundled
    # editor did until this list stopped being the game's list.
    set(VOXAGINE_LOOSE_ASSETS
        ${CMAKE_CURRENT_SOURCE_DIR}/Game/Settings.vgs
        ${CMAKE_CURRENT_SOURCE_DIR}/Game/ProjectSettings.vgps)

    if(VOXAGINE_BUILD_EDITOR)
        list(APPEND VOXAGINE_LOOSE_ASSETS
            ${CMAKE_CURRENT_SOURCE_DIR}/Game/UserSettings.vguser)
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Game/Content ${destination}/Content
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Game/Engine ${destination}/Engine
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VOXAGINE_LOOSE_ASSETS}
            ${destination}
        COMMENT "Copying the asset tree into the app bundle")
endfunction()

# ---------------------------------------------------------------------------
# The application
#
# Two targets, not one renamed target, and only ever one of them per configure.
#
#   BitBuster       - the game. A plain executable on desktop, libmain.so on
#                     Android, a .app on iOS.
#   VoxagineEditor  - the editor. A .app bundle on macOS and iOS; a plain
#                     executable named Voxagine everywhere else.
#
# It used to be one target called BitBuster with OUTPUT_NAME Voxagine flipped
# on for editor builds. That works while both are plain executables and stops
# working the moment one of them has to be a bundle: MACOSX_BUNDLE, the
# Info.plist, the bundle identifier and the resource layout are all target
# properties, and a target has one of each. Two apps need two targets.
#
# They are mutually exclusive rather than both built because EDITOR is defined
# PUBLIC on the voxagine library (see above) - the editor is not an extra
# executable over the same library, it is a different compilation of it. Having
# both in one build tree would mean a second 147-TU static library, which is
# what the editor/game preset split already buys without paying for it twice.
# ---------------------------------------------------------------------------
if(VOXAGINE_BUILD_ENGINE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/GameSources.cmake)

    if(VOXAGINE_BUILD_EDITOR)
        set(VOXAGINE_APP VoxagineEditor)
    else()
        set(VOXAGINE_APP BitBuster)
    endif()

    if(VOXAGINE_ANDROID)
        # An Android app has no executable of its own. SDLActivity dlopens the
        # libraries named by getLibraries() - "SDL3" then "main" - and calls
        # SDL_main in the second, so the game has to *be* libmain.so. The
        # OUTPUT_NAME is what makes the file name match what the Java loader
        # asks for; renaming it means overriding getLibraries() in the activity.
        add_library(${VOXAGINE_APP} SHARED ${VOXAGINE_GAME_SOURCES})
        set_target_properties(${VOXAGINE_APP} PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(${VOXAGINE_APP} ${VOXAGINE_GAME_SOURCES})
    endif()

    # The editor binary is called Voxagine, the game one BitBuster. On Android
    # neither is: the loader asks for libmain.so by name (above).
    if(VOXAGINE_BUILD_EDITOR AND NOT VOXAGINE_ANDROID)
        set_target_properties(${VOXAGINE_APP} PROPERTIES OUTPUT_NAME Voxagine)
    endif()

    target_include_directories(${VOXAGINE_APP} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Game/Source)
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
    target_link_libraries(${VOXAGINE_APP} PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,voxagine>"
        voxagine_warnings)

    # EDITOR itself is defined on the voxagine library above, PUBLIC, so both
    # the library and this executable see it.

    # -----------------------------------------------------------------------
    # Bundle identity.
    #
    # The editor is a separate application from the game and gets its own
    # identifier, display name and icon. Sharing an identifier would mean
    # installing one over the other on a device, and on macOS the two would
    # share Launch Services registration and preferences.
    if(VOXAGINE_BUILD_EDITOR)
        set(VOXAGINE_BUNDLE_ID "${VOXAGINE_IOS_BUNDLE_ID}.editor")
        set(VOXAGINE_BUNDLE_NAME "Voxagine Editor")
    else()
        set(VOXAGINE_BUNDLE_ID "${VOXAGINE_IOS_BUNDLE_ID}")
        set(VOXAGINE_BUNDLE_NAME "Bit Buster")
    endif()

    get_target_property(VOXAGINE_APP_EXECUTABLE ${VOXAGINE_APP} OUTPUT_NAME)

    if(NOT VOXAGINE_APP_EXECUTABLE)
        set(VOXAGINE_APP_EXECUTABLE ${VOXAGINE_APP})
    endif()

    # Substituted into Platforms/iOS/Info.plist.in. Both apps want landscape;
    # see the comment beside the key there.
    set(VOXAGINE_IOS_ORIENTATIONS
"        <string>UIInterfaceOrientationLandscapeLeft</string>
        <string>UIInterfaceOrientationLandscapeRight</string>")

    if(VOXAGINE_IOS)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Platforms/iOS/Info.plist.in
                       ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

        set_target_properties(${VOXAGINE_APP} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VOXAGINE_BUNDLE_ID}"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${VOXAGINE_IOS_MIN_VERSION}"
            # The UIDeviceFamily in Info.plist.in does *not* decide this on its
            # own: Xcode rewrites that key from TARGETED_DEVICE_FAMILY, which
            # defaults to iPhone. Leaving it unset shipped a bundle declaring
            # family [1] no matter what the template said, and iOS then ran the
            # game letterboxed in iPhone compatibility mode on iPad instead of
            # at the display's real resolution.
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        # Signing is the user's identity and their decision, and an unsigned
        # build still compiles and links - which is all CI could ever check, and
        # all the SideStore route ever needed, since that re-signs the IPA
        # itself. Installing on an attached device is the case that does need a
        # real identity, so set VOXAGINE_IOS_DEVELOPMENT_TEAM to a team ID and
        # build with `xcodebuild -allowProvisioningUpdates`, which then creates
        # and downloads the profile for this bundle ID on demand.
        if(VOXAGINE_IOS_DEVELOPMENT_TEAM)
            set_target_properties(${VOXAGINE_APP} PROPERTIES
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${VOXAGINE_IOS_DEVELOPMENT_TEAM}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
            )
        else()
            set_target_properties(${VOXAGINE_APP} PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            )
        endif()

        voxagine_bundle_assets(${VOXAGINE_APP} $<TARGET_BUNDLE_CONTENT_DIR:${VOXAGINE_APP}>)
    elseif(APPLE AND VOXAGINE_BUILD_EDITOR)
        # macOS: the editor is a double-clickable .app, which is the whole
        # point of this target existing separately.
        #
        # A bundle launched from Finder starts with the working directory set
        # to "/", so the engine's bare relative asset paths - "Content/...",
        # "Engine/Assets/Shaders/..." - resolve against the root of the disk
        # and every one of them fails. That is the same problem iOS has, and
        # Core/System/MobileAssets.cpp now answers it the same way for both:
        # find the asset root inside the bundle, prepare a writable copy of it,
        # and chdir there before anything reads a file.
        #
        # The game is deliberately *not* bundled here. It is still launched
        # from Game/ on desktop, still finds its assets relative to the working
        # directory, and nothing about that is broken.
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(VOXAGINE_MACOS_MIN_VERSION_KEY
"    <key>LSMinimumSystemVersion</key>
    <string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
        else()
            set(VOXAGINE_MACOS_MIN_VERSION_KEY "")
        endif()

        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Platforms/macOS/Info.plist.in
                       ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

        set_target_properties(${VOXAGINE_APP} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)

        # Contents/Resources, not Contents/: that is what SDL_GetBasePath
        # returns on macOS (CFBundleCopyResourcesDirectoryURL), where on iOS it
        # returns the bundle root. Matching SDL's answer on each platform is
        # what lets MobileAssets use one code path for both.
        voxagine_bundle_assets(${VOXAGINE_APP}
            $<TARGET_BUNDLE_CONTENT_DIR:${VOXAGINE_APP}>/Resources)
    endif()

    # Guarded because voxagine_add_shaders returns without creating the target
    # when dxc is missing, and add_dependencies on a target that does not exist
    # is a generate-time error. That is not hypothetical: it is what stopped CI
    # from building the engine at all, and CI has no Vulkan SDK. The executable
    # still links; it just cannot render until someone compiles the shaders.
    #
    # It also has to come *before* the POST_BUILD copy above can be correct:
    # the .spv files land in Game/Engine/Assets/Shaders and are copied from
    # there, so a bundle built without this dependency can capture the previous
    # build's shaders.
    if(TARGET voxagine_shaders_game)
        add_dependencies(${VOXAGINE_APP} voxagine_shaders_game)
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

        # **The gate that would have caught a level losing 8% of its geometry.**
        #
        # Every other audit in this tree checks *self-consistency* - the CPU voxel
        # against the mapping, the bricks against the bitmap, the pyramid against
        # its mirror, the codec against itself. A world that is uniformly missing
        # one chunk's models satisfies all of them, which is exactly what
        # happened: chunk streaming phase 4 dropped the camera chunk's large
        # renderers and every suite stayed green.
        #
        # Occupied voxels is an *absolute* count, and it is an exact integer -
        # machine-independent, identical in Debug and Release, reproducible run to
        # run (verified twice). So it can be a hard equality gate, and a checked-in
        # number is the only instrument here that can say "geometry went missing"
        # rather than "the pieces agree with each other".
        #
        # No --ui-script: gameplay has randomness, and the initial window is what
        # this is about. If the number legitimately changes - a level edit, a
        # placement fix - update it here and say why in the commit, exactly like
        # Tests/Baselines/perf.txt.
        add_test(NAME gpu_world_occupancy
            COMMAND BitBuster
                --hidden --size 320x180 --frames 3000
                --map Content/Worlds/Fishing_Village/Fishing_Village_Beat1.wld)
        set_tests_properties(gpu_world_occupancy PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Game
            # Two seconds is a *floor* now, not the trigger: the audit fires at
            # the first frame after it at which the world has stopped streaming
            # (RenderSystem::Render). It used to fire on the clock alone, which
            # first raced a 5.5 s run and measured nothing, and then - once
            # phase 9 bounded one renderer's stamp and spread the level's
            # geometry over more frames - read 1,400,089 at two seconds and
            # called a correct level a regression. The frame count is the margin
            # that lets it settle; the TIMEOUT is the real bound.
            ENVIRONMENT "VOXAGINE_VOXEL_AUDIT=2;VOXAGINE_AUDIO_NULL_DEVICE=1"
            PASS_REGULAR_EXPRESSION "2706535 active of 75497472"
            LABELS "gpu;integration;streaming"
            RUN_SERIAL TRUE
            TIMEOUT 180)

        # The same fixture in its VOXAGINE_GPU_STREAMING_ONLY mode: it walks the
        # player across a Beat2 chunk boundary through --ui-script and times the
        # window transition against a 16.7 ms per-frame ceiling.
        #
        # This is the primary instrument of Docs/CHUNK_STREAMING_PLAN.md. It
        # failed on master by about 9x (peak 150-154 ms in Release; see phase 0's
        # notes) and carried WILL_FAIL through the phases that drove it down:
        # 150 -> 102 (1) -> 94 (3) -> 27.9 (5) -> **9.8-10.2 ms (9)**. It passes
        # now, and the WILL_FAIL is off.
        #
        # A gate that is expected to fail is honest; deleting it until it passes
        # is how the hitch stayed unmeasured. If this starts failing again, the
        # peak it prints names the frame - and check the *occupancy* gate above
        # in the same breath, because phase 9's whole hazard is a green hitch
        # gate measured against a level that is not all there.
        #
        # --frames is a main-loop iteration count, not a wall clock: the fixture
        # exits itself when the transition closes, so this is set high enough not
        # to be the terminator in either config and the TIMEOUT is the real bound.
        add_test(NAME gpu_chunk_streaming_frame_budget
            COMMAND voxagine_gpu_destruction_stress
                --hidden --uncapped --size 640x360 --frames 200000
                --map Content/Worlds/Fishing_Village/Fishing_Village_Beat2.wld
                --ui-script "wait,forward-on" --ui-script-interval 120)
        set_tests_properties(gpu_chunk_streaming_frame_budget PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Game
            ENVIRONMENT "VOXAGINE_GPU_STREAMING_ONLY=1;VOXAGINE_AUDIO_NULL_DEVICE=1"
            LABELS "gpu;integration;streaming"
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
    # The streaming harness's synthetic worlds live beside the tests rather than
    # in the content pack: a failure should name a five-entity world, not a
    # thousand-entity one, and CI needs no shipped level to run them.
    target_compile_definitions(voxagine_tests PRIVATE
        VOXAGINE_TEST_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Game"
        VOXAGINE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures")

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
