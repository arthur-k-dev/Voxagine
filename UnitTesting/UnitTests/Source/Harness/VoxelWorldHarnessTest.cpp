#include <gtest/gtest.h>

#include "Harness/VoxelWorldHarness.h"

namespace
{
	/* Small enough to hash in microseconds, chunked and brick-misaligned enough
	   to exercise the arithmetic: 48 is 6 bricks, the chunk is 16 wide, and the
	   height is not a multiple of the chunk size - which is exactly the real
	   layout's shape (square chunks in x/z, full height). */
	const UVector3 k_v3Size(48, 24, 48);
	const UVector3 k_v3ChunkSize(16, 24, 16);

	const uint32_t k_uiStone = 0xFF808080u;
	const uint32_t k_uiWood = 0xFF3060A0u;
}

TEST(VoxelWorldHarness, StartsEmptyAndConsistent)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	EXPECT_EQ(world.CountOccupied(), 0u);
	EXPECT_EQ(world.Validate(), 0u);
}

TEST(VoxelWorldHarness, WriteReachesEveryRepresentation)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);

	/* CPU voxel and owner slot. */
	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);
	ASSERT_TRUE(static_cast<bool>(cell));
	EXPECT_EQ(cell.GetColor(), k_uiStone);
	EXPECT_EQ(cell.GetSlot(), 3);

	/* Mapped word. */
	EXPECT_EQ(world.Words()[world.VoxelID(5, 7, 9)], k_uiStone);

	/* Occupancy bitmap and brick count. */
	EXPECT_TRUE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	EXPECT_EQ(world.Bricks().GetCount(false, world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9))), 1u);

	EXPECT_EQ(world.Validate(), 0u);
}

TEST(VoxelWorldHarness, OverwritingDoesNotDoubleCount)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);
	world.Set(5, 7, 9, k_uiWood, 4);

	const uint32_t uiBrick = world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9));

	EXPECT_EQ(world.Bricks().GetCount(false, uiBrick), 1u);
	EXPECT_EQ(world.CountOccupied(), 1u);
	EXPECT_EQ(world.Validate(), 0u);
}

TEST(VoxelWorldHarness, ClearRemovesTheVoxelFromEveryRepresentation)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);
	world.Clear(5, 7, 9);

	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);
	EXPECT_FALSE(cell.IsActive());
	EXPECT_EQ(cell.GetSlot(), VoxelOwnerVolume::k_uiNoOwnerSlot);
	EXPECT_EQ(world.Words()[world.VoxelID(5, 7, 9)], 0u);
	EXPECT_FALSE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	EXPECT_EQ(world.Bricks().GetCount(false, world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9))), 0u);
	EXPECT_EQ(world.Validate(), 0u);
}

TEST(VoxelWorldHarness, OutOfBoundsWritesAreIgnored)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	const uint64_t uiBefore = world.Hash();

	world.Set(k_v3Size.x, 0, 0, k_uiStone, 1);
	world.Set(0, k_v3Size.y, 0, k_uiStone, 1);
	world.Set(0, 0, k_v3Size.z, k_uiStone, 1);
	world.Set(0xFFFFFFFFu, 0, 0, k_uiStone, 1);

	EXPECT_EQ(world.Hash(), uiBefore);
	EXPECT_EQ(world.CountOccupied(), 0u);
}

TEST(VoxelWorldHarness, TheSameScriptHashesTheSameTwice)
{
	auto build = []()
	{
		VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

		world.FillGround(k_uiStone);
		world.FillBox(UVector3(10, 1, 10), UVector3(12, 12, 12), k_uiWood, 7);

		for (uint32_t i = 0; i < 64; ++i)
			world.Clear(10 + (i % 12), 1 + (i % 11), 10 + ((i * 5) % 12));

		return world.Hash();
	};

	EXPECT_EQ(build(), build());
}

TEST(VoxelWorldHarness, TheHashSeesASingleVoxel)
{
	VoxelWorldHarness a(k_v3Size, k_v3ChunkSize);
	VoxelWorldHarness b(k_v3Size, k_v3ChunkSize);

	a.FillBox(UVector3(2, 2, 2), UVector3(4, 4, 4), k_uiStone, 1);
	b.FillBox(UVector3(2, 2, 2), UVector3(4, 4, 4), k_uiStone, 1);

	EXPECT_EQ(a.Hash(), b.Hash());

	/* Colour, owner slot and occupancy each have to move the hash on their own,
	   or the net has a hole exactly where a phase might land a regression. */
	b.Set(3, 3, 3, k_uiWood, 1);
	EXPECT_NE(a.Hash(), b.Hash());

	b.Set(3, 3, 3, k_uiStone, 2);
	EXPECT_NE(a.Hash(), b.Hash());

	b.Set(3, 3, 3, k_uiStone, 1);
	EXPECT_EQ(a.Hash(), b.Hash());

	b.Clear(3, 3, 3);
	EXPECT_NE(a.Hash(), b.Hash());
}
