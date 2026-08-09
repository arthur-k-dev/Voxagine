# Provides the SDL3::SDL3 target.
#
# Two acquisition routes, and which one is right is decided by the toolchain
# rather than by preference:
#
#  - **Desktop**: find_package, falling back to pkg-config. This is what the
#    tree has always done and it stays the default, because a distro SDL3 is
#    already built, already correct, and does not cost a minute of configure
#    time.
#  - **Cross-compiling** (Android, iOS): build SDL3 from source with the same
#    toolchain as everything else. find_package and pkg-config only ever see
#    the *host*, so on a cross build they either fail or - much worse - succeed
#    and hand an arm64 link an x86-64 library.
#
# VOXAGINE_FETCH_SDL3 forces the source build anywhere. VOXAGINE_SDL3_SOURCE_DIR
# points it at an existing checkout instead of cloning, which is how the Android
# Gradle project shares one SDL3 tree between the native build (this file) and
# the Java side (org.libsdl.app, which has to match the .so exactly).

set(VOXAGINE_SDL3_TAG "release-3.2.20" CACHE STRING
    "SDL3 tag to build from source when cross-compiling")
set(VOXAGINE_SDL3_SOURCE_DIR "" CACHE PATH
    "Existing SDL3 checkout to build instead of cloning")

if(NOT DEFINED VOXAGINE_FETCH_SDL3)
    if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
        set(VOXAGINE_FETCH_SDL3 ON)
    else()
        set(VOXAGINE_FETCH_SDL3 OFF)
    endif()
endif()

if(NOT VOXAGINE_FETCH_SDL3)
    find_package(SDL3 QUIET)

    if(NOT SDL3_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
    endif()

    return()
endif()

message(STATUS "SDL3: building ${VOXAGINE_SDL3_TAG} from source for ${CMAKE_SYSTEM_NAME}")

include(FetchContent)

# Android needs SDL as a *shared* library and that is not a preference either:
# SDLActivity reaches the native side through JNI, and JNI_OnLoad plus the
# registered natives have to live in a .so the Java loader can dlopen by name.
# A static SDL linked into libmain.so leaves org.libsdl.app with nothing to
# bind to.
if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

if(VOXAGINE_SDL3_SOURCE_DIR)
    if(NOT EXISTS "${VOXAGINE_SDL3_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "VOXAGINE_SDL3_SOURCE_DIR=${VOXAGINE_SDL3_SOURCE_DIR} has no CMakeLists.txt")
    endif()

    FetchContent_Declare(SDL3 SOURCE_DIR ${VOXAGINE_SDL3_SOURCE_DIR})
else()
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        ${VOXAGINE_SDL3_TAG}
        GIT_SHALLOW    TRUE
    )
endif()

FetchContent_MakeAvailable(SDL3)

# SDL's own warnings are not ours to fix, and this tree builds with -Wall
# -Wextra as errors-in-spirit.
foreach(sdl_target SDL3-shared SDL3-static)
    if(TARGET ${sdl_target})
        target_compile_options(${sdl_target} PRIVATE -w)

        # Same reason as RTTR: the Android Gradle plugin packages what it finds
        # in the output directory it nominated, and a libSDL3.so left in
        # _deps/sdl3-build is a dlopen failure at startup, not a build error.
        if(ANDROID AND CMAKE_LIBRARY_OUTPUT_DIRECTORY)
            set_target_properties(${sdl_target} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        endif()
    endif()
endforeach()

# SDL3's CMake already defines SDL3::SDL3 as an alias for whichever of
# shared/static it built, so there is nothing to alias here.
if(NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 was built but did not define SDL3::SDL3")
endif()
