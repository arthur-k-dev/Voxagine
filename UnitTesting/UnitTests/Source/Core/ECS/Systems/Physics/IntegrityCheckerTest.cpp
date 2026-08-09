#include <gtest/gtest.h>

#include <algorithm>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Voxels/VoxelEditBatch.h"
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


/* Phase 4. The memo is what stops an explosion's thousands of seeds each
   re-flooding the same structure: the first seed classifies it, the rest are
   answered without a visit.

   A long beam held up at one end, deliberately: a seed inside a *tower* finds
   the ground within a few steps and the memo saves little, which is the shape
   that makes this measurable. The expensive walks are the ones where support is
   far away, and those are exactly the ones a burst produces thousands of seeds
   on. */
TEST(IntegrityChecker, TheMemoAnswersRepeatedSeedsWithoutWalking)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);
	world.FillBox(UVector3(1, 10, 20), UVector3(46, 3, 3), k_uiStone, 1);  /* the beam */
	world.FillBox(UVector3(1, 1, 20), UVector3(1, 9, 3), k_uiStone, 1);    /* its one support */

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	/* From the far end, so the walk crosses the whole beam before it finds the
	   support. */
	std::vector<uint64_t> first { IntegrityChecker::PositionToHash(46u, 11u, 21u) };
	checker.EnqueueBulk(first);

	RunToCompletion(checker);

	const uint64_t uiFirstVisits = checker.GetStats().uiVisits;
	EXPECT_GT(uiFirstVisits, 200u);

	/* A hundred more seeds along the same beam. Without the memo each one would
	   cross it again. */
	std::vector<uint64_t> more;

	for (uint32_t i = 0; i < 100; ++i)
		more.push_back(IntegrityChecker::PositionToHash(2u + (i % 45), 10u + (i % 3), 20u + (i % 3)));

	checker.EnqueueBulk(more);

	RunToCompletion(checker);

	const uint64_t uiExtraVisits = checker.GetStats().uiVisits - uiFirstVisits;

	/* A hundred seeds must not cost anything like a hundred walks. Bounded
	   generously - what matters is the shape, not the constant. */
	EXPECT_LT(uiExtraVisits, uiFirstVisits);

	/* Two ways the memo pays, and both matter. A seed landing exactly on a
	   classified voxel is answered without starting a walk at all; one landing
	   beside the first walk's path starts a walk that meets the memo a step or
	   two in and stops there. The first walk records the path it took, not the
	   whole beam, so most of the saving is the second kind. */
	const IntegrityChecker::Stats& stats = checker.GetStats();

	EXPECT_GT(stats.uiSeedsSkippedByMemo + stats.uiMemoHits, 40u);
}

/* Ledger D6's consumer half: a seed is deduplicated against everything queued,
   not only against the batch it arrived in. */
TEST(IntegrityChecker, SeedsAreDeduplicatedAgainstThePendingQueue)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	const uint64_t uiSeed = IntegrityChecker::PositionToHash(5u, 5u, 5u);

	std::vector<uint64_t> batchA { uiSeed, uiSeed, uiSeed };
	std::vector<uint64_t> batchB { uiSeed };

	checker.EnqueueBulk(batchA);
	checker.EnqueueBulk(batchB);

	EXPECT_EQ(checker.GetStats().uiSeedsOffered, 4u);
	EXPECT_EQ(checker.GetStats().uiSeedsDeduplicated, 3u);
}

/* The memo has to notice a write it was not told about. This is the defect the
   first version of phase 4 shipped in the gauntlet: callers invalidated, the
   gauntlet was not a caller, and it found 20 islands where it should have found
   100. The grid's write generation is polled instead. */
TEST(IntegrityChecker, AWriteThroughTheBatchInvalidatesTheMemo)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);
	world.FillBox(UVector3(20, 1, 20), UVector3(1, 8, 1), k_uiStone, 1);
	world.FillBox(UVector3(18, 9, 18), UVector3(5, 1, 5), k_uiStone, 1);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	/* Classified as grounded, and remembered. */
	std::vector<uint64_t> seeds { IntegrityChecker::PositionToHash(20u, 9u, 20u) };
	checker.EnqueueBulk(seeds);

	EXPECT_TRUE(RunToCompletion(checker).empty());

	/* Cut the column out from under the slab, through the batch - which is what
	   destruction does and what the checker is never told about. */
	{
		VoxelEditBatch batch(world.MakeEditTarget());

		for (uint32_t uiY = 1; uiY < 9; ++uiY)
			batch.Clear(Vector3(20.f, static_cast<float>(uiY), 20.f));
	}

	std::vector<uint64_t> after { IntegrityChecker::PositionToHash(20u, 9u, 20u) };
	checker.EnqueueBulk(after);

	const std::vector<std::vector<uint64_t>> islands = RunToCompletion(checker);

	ASSERT_EQ(islands.size(), 1u);
	EXPECT_EQ(islands[0].size(), 5u * 5u);
}

/* The oracle, which is the phase 4 acceptance instrument: the exhaustive
   pre-phase-4 walk and the memoised one must classify identically. */
TEST(IntegrityChecker, TheMemoisedAnswerMatchesTheExhaustiveOne)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillGround(k_uiStone);
	world.FillBox(UVector3(4, 1, 4), UVector3(10, 18, 10), k_uiStone, 1);   /* grounded block */
	world.FillBox(UVector3(24, 8, 24), UVector3(6, 6, 6), k_uiStone, 2);    /* floating block */
	world.FillBox(UVector3(36, 1, 36), UVector3(1, 10, 1), k_uiStone, 3);   /* column */
	world.FillBox(UVector3(34, 11, 34), UVector3(5, 2, 5), k_uiStone, 3);   /* its slab */

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	/* Cut the column, so the slab becomes an island the checker has to find. */
	{
		VoxelEditBatch batch(world.MakeEditTarget());

		for (uint32_t uiY = 1; uiY < 11; ++uiY)
			batch.Clear(Vector3(36.f, static_cast<float>(uiY), 36.f));
	}

	/* Seed everything occupied, which is far more than destruction would - the
	   point is that the memoised walk gives the same answer whatever it is
	   asked. */
	std::vector<uint64_t> seeds;

	for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < k_v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
	{
		if (world.Grid().GetCell(uiX, uiY, uiZ).IsActive())
			seeds.push_back(IntegrityChecker::PositionToHash(uiX, uiY, uiZ));
	}

	checker.EnqueueBulk(seeds);

	std::vector<uint64_t> found;

	for (const std::vector<uint64_t>& island : RunToCompletion(checker))
		found.insert(found.end(), island.begin(), island.end());

	std::sort(found.begin(), found.end());
	found.erase(std::unique(found.begin(), found.end()), found.end());

	/* Now the same question, exhaustively, from scratch. */
	std::vector<uint64_t> oracle;
	std::vector<uint64_t> island;

	for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < k_v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
	{
		if (!world.Grid().GetCell(uiX, uiY, uiZ).IsActive())
			continue;

		if (checker.ClassifyExhaustive(
			Vector3(static_cast<float>(uiX), static_cast<float>(uiY), static_cast<float>(uiZ)), island))
		{
			oracle.insert(oracle.end(), island.begin(), island.end());
		}
	}

	std::sort(oracle.begin(), oracle.end());
	oracle.erase(std::unique(oracle.begin(), oracle.end()), oracle.end());

	EXPECT_EQ(found.size(), 6u * 6u * 6u + 5u * 2u * 5u);
	EXPECT_EQ(found, oracle);
}
