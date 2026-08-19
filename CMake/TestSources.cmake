# The test suite's translation units, listed explicitly for the same reason
# EngineSources.cmake is: a glob picks up whatever happens to be lying in the
# directory, and this tree has been burned by that before.
#
# One binary, `voxagine_tests`, with three modes - see Tests/README.md. It used
# to be three: a gtest suite, a scripted gauntlet and a scenario runner, each
# with its own main, its own output format and its own copy of the destruction
# tick loop. The copies had already drifted.
#
# Everything here builds and runs with no GPU and no display, which is what lets
# CI cover it - see Tests/Harness/VoxelWorldHarness.h for why the voxel write
# path is reachable without either.
#
# Checks, scenarios, invariants and benchmarks all register themselves from
# static initialisers, so these have to be compiled straight into the executable
# rather than reached through a static archive: a linker pulls an object out of
# an archive only when something references a symbol in it, and nothing
# references these. Same shape as Core/Settings.cpp; see the WHOLE_ARCHIVE note
# on BitBuster in CMakeLists.txt for what that cost when it went unnoticed.

set(VOXAGINE_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Tests)

set(VOXAGINE_TEST_SOURCES
    # The runner and its registries.
    ${VOXAGINE_TEST_DIR}/Framework/Main.cpp
    ${VOXAGINE_TEST_DIR}/Framework/Registries.cpp
    ${VOXAGINE_TEST_DIR}/Framework/Baseline.cpp

    # A whole voxel world with no GPU, and the destruction pipeline driven once.
    ${VOXAGINE_TEST_DIR}/Harness/VoxelWorldHarness.cpp
    ${VOXAGINE_TEST_DIR}/Harness/DestructionRun.cpp
    ${VOXAGINE_TEST_DIR}/Harness/WorldShapes.cpp

    # A whole chunk-streaming world with no GPU: a real World with no
    # RenderSystem, real Chunks, the real serializer, and the resident window
    # as two vectors behind IVoxelWindow.
    ${VOXAGINE_TEST_DIR}/Harness/StreamingHarness.cpp

    # A reflected entity that exists only here, so a cross-chunk link and
    # "did gameplay tick" are expressible at all - see StreamingProbe.h.
    ${VOXAGINE_TEST_DIR}/Harness/StreamingProbe.cpp

    # A .vox read without the engine's resource stack, so a test can place a
    # real model at a real transform - see the header.
    ${VOXAGINE_TEST_DIR}/Harness/VoxModelFile.cpp

    # Voxel storage: the grid, the brick grid, the harness that models both.
    ${VOXAGINE_TEST_DIR}/VoxelStorage/VoxelGridChecks.cpp
    ${VOXAGINE_TEST_DIR}/VoxelStorage/VoxelBrickGridChecks.cpp
    ${VOXAGINE_TEST_DIR}/VoxelStorage/WorldHarnessChecks.cpp
    ${VOXAGINE_TEST_DIR}/VoxelStorage/VoxelStoragePerf.cpp

    # The one voxel write path.
    ${VOXAGINE_TEST_DIR}/VoxelEditing/VoxelEditBatchChecks.cpp
    ${VOXAGINE_TEST_DIR}/VoxelEditing/VoxelEditingPerf.cpp

    # Destruction: the explosion, the invariants every run owes, and the
    # scenarios that put a world in front of them.
    ${VOXAGINE_TEST_DIR}/Destruction/SphericalDestructionChecks.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/DestructionPerf.cpp

    ${VOXAGINE_TEST_DIR}/Destruction/Invariants/RepresentationsAgree.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Invariants/IndestructibleGeometrySurvives.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Invariants/ParticlePoolIsSound.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Invariants/NothingIsLeftFloating.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Invariants/RunsAreReproducible.cpp

    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/SeveredTowers.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/IndestructibleTowers.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/SuspendedDecoration.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/DiagonalSupport.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/StackedPile.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/DynamicBody.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/WindowEdges.cpp
    ${VOXAGINE_TEST_DIR}/Destruction/Scenarios/RandomizedWorlds.cpp

    # Chunk streaming: the window commit transaction and what survives a slide.
    ${VOXAGINE_TEST_DIR}/Streaming/WindowCommitChecks.cpp
    ${VOXAGINE_TEST_DIR}/Streaming/ChunkReloadChecks.cpp
    ${VOXAGINE_TEST_DIR}/Streaming/ChunkUnloadChecks.cpp
    ${VOXAGINE_TEST_DIR}/Streaming/EntityStreamingChecks.cpp
    ${VOXAGINE_TEST_DIR}/Streaming/WorldStreamingChecks.cpp
    ${VOXAGINE_TEST_DIR}/Streaming/StreamingPerf.cpp

    # Connectivity: which geometry is still holding itself up.
    ${VOXAGINE_TEST_DIR}/Integrity/IntegrityCheckerChecks.cpp
    ${VOXAGINE_TEST_DIR}/Integrity/IntegrityPerf.cpp

    # The particle core, its landing resolution and its simulation cost.
    ${VOXAGINE_TEST_DIR}/Particles/ParticleCoreChecks.cpp
    ${VOXAGINE_TEST_DIR}/Particles/ParticleLandingChecks.cpp
    ${VOXAGINE_TEST_DIR}/Particles/ParticlesPerf.cpp

    # The renderer's CPU-side decisions - the parts that are reachable without
    # a device. The GPU integration fixture in the same directory is a separate
    # executable and is not part of this suite.
    ${VOXAGINE_TEST_DIR}/Rendering/ModelMeshUploadChecks.cpp
    ${VOXAGINE_TEST_DIR}/Rendering/VoxelStampChecks.cpp

    # Engine-wide primitives that are not about voxels at all.
    ${VOXAGINE_TEST_DIR}/Foundation/GameTimerChecks.cpp
    ${VOXAGINE_TEST_DIR}/Foundation/JobManagerConfigChecks.cpp
    ${VOXAGINE_TEST_DIR}/Foundation/ReferenceManagerChecks.cpp
    ${VOXAGINE_TEST_DIR}/Foundation/DateTimeChecks.cpp
    ${VOXAGINE_TEST_DIR}/Foundation/MathUtilsChecks.cpp
    ${VOXAGINE_TEST_DIR}/Foundation/AudioDecodeChecks.cpp
)
