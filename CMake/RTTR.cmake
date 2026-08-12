# Provides the RTTR::Core target.
#
# Prefers an installed RTTR so offline and packaged builds work; otherwise
# fetches the v0.9.6 tag, which is the version the engine's headers were
# vendored from and the one verified against GCC 16 / Clang 22.

set(VOXAGINE_RTTR_TAG v0.9.6)

if(NOT VOXAGINE_FETCH_RTTR)
    find_package(rttr CONFIG REQUIRED)
    return()
endif()

find_package(rttr CONFIG QUIET)

if(rttr_FOUND AND TARGET RTTR::Core)
    message(STATUS "RTTR: using installed ${rttr_VERSION}")
    return()
endif()

message(STATUS "RTTR: fetching ${VOXAGINE_RTTR_TAG} from upstream")

include(FetchContent)

FetchContent_Declare(rttr
    GIT_REPOSITORY https://github.com/rttrorg/rttr.git
    GIT_TAG        ${VOXAGINE_RTTR_TAG}
    GIT_SHALLOW    TRUE
    # 0.9.6 sorts base classes with a comparator that is not a strict weak
    # ordering, which is UB in std::sort and aborts on iOS before main().
    # CMake/PatchRTTR.cmake explains it at length and is idempotent.
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   -DRTTR_SOURCE_DIR=<SOURCE_DIR>
                   -P ${CMAKE_CURRENT_LIST_DIR}/PatchRTTR.cmake
)

# RTTR 0.9.6 predates CMake 3.5 being dropped as a compatibility floor, so
# configuring it under CMake 4 fails outright without this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(BUILD_INSTALLER OFF CACHE BOOL "" FORCE)
set(BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_RUNTIME_LIBS OFF CACHE BOOL "" FORCE)

# iOS bundles do not currently copy RTTR's shared library into the app.
# Build RTTR statically on iOS so the executable has no reference to the
# build-machine path (and cannot abort at launch with DYLD "Library missing").
if(IOS OR (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    set(BUILD_RTTR_DYNAMIC OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(rttr)

# RTTR exposes the static target as Core_Lib, while Voxagine consistently links
# RTTR::Core. Keep that public name stable for the iOS static configuration.
if((IOS OR (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")) AND
   TARGET rttr_core_lib AND NOT TARGET RTTR::Core)
    add_library(RTTR::Core ALIAS rttr_core_lib)
endif()

# RTTR's own warnings are not ours to fix.
if(TARGET rttr_core)
    target_compile_options(rttr_core PRIVATE -w)

    # RTTR sets its own LIBRARY_OUTPUT_DIRECTORY, so librttr_core.so lands in
    # _deps/rttr-build/lib rather than wherever the outer build puts shared
    # libraries. On Android that is the difference between the .so being
    # packaged into the APK and the app dying on dlopen at startup - the
    # Android Gradle plugin collects from the output directory it set, and
    # nowhere else.
    if(ANDROID AND CMAKE_LIBRARY_OUTPUT_DIRECTORY)
        set_target_properties(rttr_core PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
    endif()

    # Its headers are reached as <rttr/...>; the generated version.h lands in
    # the build tree, so both directories have to be on the search path.
    target_include_directories(rttr_core SYSTEM INTERFACE
        $<BUILD_INTERFACE:${rttr_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${rttr_BINARY_DIR}/src>
    )
endif()
