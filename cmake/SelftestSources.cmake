# The selftest's translation units, listed explicitly like the engine's and the
# unit tests'.
#
# Scenarios and invariants are self-registering, so adding one is adding a file
# here and nothing else - no runner change, no header to edit. That cheapness is
# the point: every defect the first play session found was a scenario that did
# not exist.

set(VOXAGINE_SELFTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting/Selftest)

set(VOXAGINE_SELFTEST_SOURCES
    ${VOXAGINE_SELFTEST_DIR}/Main.cpp
    ${VOXAGINE_SELFTEST_DIR}/SelftestRegistry.cpp
    ${VOXAGINE_SELFTEST_DIR}/SelftestWorld.cpp

    ${VOXAGINE_SELFTEST_DIR}/Invariants/CoreInvariants.cpp

    ${VOXAGINE_SELFTEST_DIR}/Scenarios/Common.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/DiagonalScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/DynamicScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/EdgeScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/FloatingScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/ProtectedScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/RandomScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/StackedScenario.cpp
    ${VOXAGINE_SELFTEST_DIR}/Scenarios/TowersScenario.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/UnitTesting/UnitTests/Source/Harness/VoxelWorldHarness.cpp
)
