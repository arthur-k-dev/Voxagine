# Which platform is being built, and what that implies.
#
# Before the mobile port there was exactly one platform branch in the whole
# build (`comdlg32` on WIN32), because there was exactly one shape of target:
# a desktop executable that finds its assets next to the working directory.
# Android and iOS break both halves of that, so the differences get named here
# once rather than being rediscovered at each site.
#
# Sets, for use elsewhere:
#   VOXAGINE_ANDROID / VOXAGINE_IOS   - exactly one, or neither
#   VOXAGINE_MOBILE                   - either of the above
# and defines VOXAGINE_MOBILE on every target that links voxagine, so C++ can
# ask the same question. Prefer that over __ANDROID__/TARGET_OS_IOS at call
# sites: what the code almost always means is "is this a phone", not "is this
# Bionic".

set(VOXAGINE_ANDROID OFF)
set(VOXAGINE_IOS OFF)

if(ANDROID)
    set(VOXAGINE_ANDROID ON)
elseif(IOS OR (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    set(VOXAGINE_IOS ON)
endif()

if(VOXAGINE_ANDROID OR VOXAGINE_IOS)
    set(VOXAGINE_MOBILE ON)
    add_compile_definitions(VOXAGINE_MOBILE)
else()
    set(VOXAGINE_MOBILE OFF)
endif()

if(VOXAGINE_ANDROID)
    add_compile_definitions(VOXAGINE_ANDROID)
elseif(VOXAGINE_IOS)
    add_compile_definitions(VOXAGINE_IOS)
endif()

# ---------------------------------------------------------------------------
# The device floor, decided once and written down here because three separate
# things have to agree with it: the NDK's ANDROID_PLATFORM, Gradle's
# minSdkVersion, and Xcode's IPHONEOS_DEPLOYMENT_TARGET.
#
# **The renderer requires Vulkan 1.3.** VKDevice asks for VK_API_VERSION_1_3 and
# the swapchain uses core-1.3 synchronization2 (vkCmdPipelineBarrier2,
# vkQueueSubmit2) with no fallback path, so a device without it fails at device
# creation rather than degrading. Docs/MOBILE_PORT_PLAN.md rejected building a
# reduced-feature path for older hardware as disproportionate for a small indie
# title, and that decision is what these numbers encode.
#
#   Android: API 31 (Android 12). Vulkan 1.3 shipped January 2022; Android 13
#            is where it became common and Android 14 is where it is close to
#            universal on anything with a discrete GPU vendor's driver. 31 is
#            the floor that can *run* it at all; the store listing should say
#            Android 13. Note this is a compile floor - a device on API 31 with
#            a Vulkan 1.1 driver still fails at startup, and phase 4 is where
#            that failure is made to say so clearly.
#   iOS:     16.0. MoltenVK's Vulkan 1.3 support needs a recent Metal feature
#            set, and 16.0 is also the floor where Apple's own tooling stops
#            arguing about bitcode.
set(VOXAGINE_ANDROID_MIN_API 31 CACHE STRING "Android minSdkVersion / ANDROID_PLATFORM")
set(VOXAGINE_IOS_MIN_VERSION "16.0" CACHE STRING "IPHONEOS_DEPLOYMENT_TARGET")

set(VOXAGINE_IOS_BUNDLE_ID "com.voxagine.bitbuster" CACHE STRING
    "CFBundleIdentifier for the iOS app")

# Defaults to the copy vendored in External/, so `cmake --preset ios` configures
# with no extra arguments. Overridable for a Vulkan SDK install elsewhere.
set(VOXAGINE_MOLTENVK_DIR "${CMAKE_CURRENT_LIST_DIR}/../External/MoltenVK/MoltenVK"
    CACHE PATH "MoltenVK install to link the iOS build against")
# Empty means "build unsigned", which is what CI and the SideStore route want.
# Set it to a ten-character Apple team ID to sign for an attached device; see
# the signing block in CMakeLists.txt.
set(VOXAGINE_IOS_DEVELOPMENT_TEAM "" CACHE STRING
    "Apple development team ID to sign the iOS app with, or empty for unsigned")

if(VOXAGINE_MOBILE)
    message(STATUS "Voxagine: building for mobile "
        "(android=${VOXAGINE_ANDROID} ios=${VOXAGINE_IOS})")

    # The editor is explicitly out of scope for mobile - it is a mouse and
    # keyboard tool with desktop file dialogs. Catch an attempt to build it
    # here rather than in a wall of compile errors.
    if(VOXAGINE_BUILD_EDITOR)
        message(FATAL_ERROR
            "VOXAGINE_BUILD_EDITOR is not supported on mobile. "
            "See Docs/MOBILE_PORT_PLAN.md, 'Rejected / out of scope'.")
    endif()

    if(VOXAGINE_BUILD_BRINGUP AND VOXAGINE_ANDROID)
        # voxagine_bringup has its own main() and would want its own APK to be
        # runnable. It is still worth *compiling* as a cheap check that the
        # Vulkan backend cross-compiles, so this is a note rather than an error.
        message(STATUS "Voxagine: voxagine_bringup will compile but has no APK to run in")
    endif()
endif()

# ---------------------------------------------------------------------------
# Vulkan.
#
# find_package(Vulkan) on the NDK toolchain finds the sysroot's headers and
# libvulkan.so, because CMAKE_FIND_ROOT_PATH points at the sysroot - so the
# normal path works and there is deliberately no special case here. On iOS the
# loader is MoltenVK and there is no system Vulkan at all, which is why that
# one *does* need a hand: see VOXAGINE_MOLTENVK_DIR below.
function(voxagine_link_vulkan target)
    if(VOXAGINE_IOS)
        if(NOT VOXAGINE_MOLTENVK_DIR)
            message(FATAL_ERROR
                "iOS needs VOXAGINE_MOLTENVK_DIR pointing at a MoltenVK install "
                "(the Vulkan SDK for macOS ships one). There is no system Vulkan "
                "on iOS; MoltenVK is the loader and the ICD.")
        endif()

        target_include_directories(${target} SYSTEM PUBLIC ${VOXAGINE_MOLTENVK_DIR}/include)

        # MoltenVK's older SDK layout shipped dylib/iOS/libMoltenVK.dylib;
        # current official iOS archives ship an XCFramework containing the
        # arm64 static library instead. Support both layouts so a build does
        # not silently depend on a deleted /tmp SDK extraction.
        if(EXISTS ${VOXAGINE_MOLTENVK_DIR}/dylib/iOS/libMoltenVK.dylib)
            set(VOXAGINE_MOLTENVK_LIBRARY ${VOXAGINE_MOLTENVK_DIR}/dylib/iOS/libMoltenVK.dylib)
        elseif(EXISTS ${VOXAGINE_MOLTENVK_DIR}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a)
            set(VOXAGINE_MOLTENVK_LIBRARY
                ${VOXAGINE_MOLTENVK_DIR}/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a)
        else()
            message(FATAL_ERROR "No iOS MoltenVK library found under ${VOXAGINE_MOLTENVK_DIR}")
        endif()

        target_link_libraries(${target} PUBLIC
            ${VOXAGINE_MOLTENVK_LIBRARY}
            "-framework Metal" "-framework IOSurface" "-framework QuartzCore"
            "-framework Foundation" "-framework UIKit")
    else()
        target_link_libraries(${target} PUBLIC Vulkan::Vulkan)
    endif()
endfunction()
