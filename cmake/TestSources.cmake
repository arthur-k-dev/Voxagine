# Unit test translation units, listed explicitly for the same reason
# EngineSources.cmake is: a glob would silently pick up the pre-port suite under
# UnitTesting/UnitTests/*.cpp, which still targets the deleted allocators and
# the Windows-only harness.
#
# Every file here is expected to build and pass on both configurations, with no
# GPU and no display - see UnitTesting/UnitTests/Source/Harness/VoxelWorldHarness.h
# for why the voxel write path is reachable without either.

set(VOXAGINE_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting/UnitTests/Source)

set(VOXAGINE_TEST_SOURCES
    ${VOXAGINE_TEST_DIR}/Harness/VoxelWorldHarness.cpp
    ${VOXAGINE_TEST_DIR}/Harness/VoxelWorldHarnessTest.cpp

    ${VOXAGINE_TEST_DIR}/Core/ECS/Systems/Physics/VoxelGridTest.cpp
    ${VOXAGINE_TEST_DIR}/Core/ECS/Systems/Physics/IntegrityCheckerTest.cpp
    ${VOXAGINE_TEST_DIR}/Core/ECS/Systems/Physics/ParticleLinkedListTest.cpp
    ${VOXAGINE_TEST_DIR}/Core/Platform/Rendering/VoxelBrickGridTest.cpp
)
