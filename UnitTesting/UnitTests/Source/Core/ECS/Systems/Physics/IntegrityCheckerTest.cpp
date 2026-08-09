#include <gtest/gtest.h>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Harness/VoxelWorldHarness.h"

namespace
{
	const UVector3 k_v3Size(48, 24, 48);
	const UVector3 k_v3ChunkSize(16, 24, 16);

	const uint32_t k_uiStone = 0xFF808080u;

	/* Drains the checker until it stops producing work, so a test never depends
	   on the per-tick visit budget. */
	std::vector<std::vector<uint64_t>> RunToCompletion(IntegrityChecker& checker)
	{
		std::vector<std::vector<uint64_t>> results;

		for (uint32_t i = 0; i < 4096; ++i)
		{
			const size_t uiBefore = results.size();
			checker.Process(IntegrityChecker::VISIT_BUDGET_PER_TICK, results);

			/* Process returns as soon as there is nothing pending, so a pass
			   that neither reported nor consumed anything means it is done. The
			   loop bound is a runaway guard, not the exit condition. */
			if (results.size() == uiBefore && i > 0)
				break;
		}

		return results;
	}
}

/* D11: both open-coded copies of this hash truncate float to uint16_t, so a
   negative coordinate wraps rather than being rejected. Pinned here as it is,
   not as it should be - phase 2 consolidates the two copies and this test is
   what says the consolidation did not change the answer for the positive
   coordinates everything actually uses. */
TEST(IntegrityChecker, HashRoundTripsPositiveCoordinates)
{
	for (uint32_t uiX = 0; uiX < 1024; uiX += 37)
	for (uint32_t uiY = 0; uiY < 256; uiY += 11)
	for (uint32_t uiZ = 0; uiZ < 1024; uiZ += 53)
	{
		const Vector3 v3Position(uiX, uiY, uiZ);
		const Vector3 v3Back = IntegrityChecker::HashToPosition(IntegrityChecker::PositionToHash(v3Position));

		EXPECT_EQ(v3Back, v3Position);
	}
}

TEST(IntegrityChecker, AGroundedStructureIsNotAnIsland)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);
	world.FillBox(UVector3(10, 1, 10), UVector3(6, 8, 6), k_uiStone, 1);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	std::vector<uint64_t> seeds { IntegrityChecker::PositionToHash(Vector3(12, 5, 12)) };
	checker.EnqueueBulk(seeds);

	EXPECT_TRUE(RunToCompletion(checker).empty());
}

TEST(IntegrityChecker, AFloatingStructureIsAnIsland)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);

	/* Starts at y = 5 with nothing under it. The checker calls a voxel grounded
	   when y - 1 == 0, i.e. when it sits directly on the ground layer. */
	world.FillBox(UVector3(20, 5, 20), UVector3(4, 4, 4), k_uiStone, 1);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	std::vector<uint64_t> seeds { IntegrityChecker::PositionToHash(Vector3(21, 6, 21)) };
	checker.EnqueueBulk(seeds);

	const std::vector<std::vector<uint64_t>> islands = RunToCompletion(checker);

	ASSERT_EQ(islands.size(), 1u);
	EXPECT_EQ(islands[0].size(), 4u * 4u * 4u);
}

TEST(IntegrityChecker, CuttingASupportTurnsAStructureIntoAnIsland)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);

	/* A single column from the ground up to a slab. Remove the column and the
	   slab is unsupported - the whole point of the check. */
	world.FillBox(UVector3(30, 1, 30), UVector3(1, 6, 1), k_uiStone, 1);
	world.FillBox(UVector3(28, 7, 28), UVector3(5, 1, 5), k_uiStone, 1);

	IntegrityChecker grounded;
	grounded.SetVoxelGrid(&world.Grid());

	std::vector<uint64_t> seeds { IntegrityChecker::PositionToHash(Vector3(30, 7, 30)) };
	grounded.EnqueueBulk(seeds);

	EXPECT_TRUE(RunToCompletion(grounded).empty());

	for (uint32_t uiY = 1; uiY < 7; ++uiY)
		world.Clear(30, uiY, 30);

	IntegrityChecker cut;
	cut.SetVoxelGrid(&world.Grid());

	std::vector<uint64_t> cutSeeds { IntegrityChecker::PositionToHash(Vector3(30, 7, 30)) };
	cut.EnqueueBulk(cutSeeds);

	const std::vector<std::vector<uint64_t>> islands = RunToCompletion(cut);

	ASSERT_EQ(islands.size(), 1u);
	EXPECT_EQ(islands[0].size(), 5u * 5u);
}

TEST(IntegrityChecker, ResetDropsPendingWork)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);
	world.FillBox(UVector3(20, 5, 20), UVector3(4, 4, 4), k_uiStone, 1);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	std::vector<uint64_t> seeds { IntegrityChecker::PositionToHash(Vector3(21, 6, 21)) };
	checker.EnqueueBulk(seeds);
	checker.Reset();

	EXPECT_TRUE(RunToCompletion(checker).empty());
}
