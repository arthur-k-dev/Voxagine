#include "Framework/Check.h"

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

VOXAGINE_CHECK(VoxelWorldHarness, StartsEmptyAndConsistent)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	CHECK_EQ(world.CountOccupied(), 0u);
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelWorldHarness, WriteReachesEveryRepresentation)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);

	/* CPU voxel and owner slot. */
	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);
	REQUIRE_TRUE(static_cast<bool>(cell));
	CHECK_EQ(cell.GetColor(), k_uiStone);
	CHECK_EQ(cell.GetSlot(), 3);

	/* Mapped word. */
	CHECK_EQ(world.Words()[world.VoxelID(5, 7, 9)], k_uiStone);

	/* Occupancy bitmap and brick count. */
	CHECK_TRUE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	CHECK_EQ(world.Bricks().GetCount(false, world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9))), 1u);

	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelWorldHarness, OverwritingDoesNotDoubleCount)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);
	world.Set(5, 7, 9, k_uiWood, 4);

	const uint32_t uiBrick = world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9));

	CHECK_EQ(world.Bricks().GetCount(false, uiBrick), 1u);
	CHECK_EQ(world.CountOccupied(), 1u);
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelWorldHarness, ClearRemovesTheVoxelFromEveryRepresentation)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 7, 9, k_uiStone, 3);
	world.Clear(5, 7, 9);

	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);
	CHECK_FALSE(cell.IsActive());
	CHECK_EQ(cell.GetSlot(), VoxelOwnerVolume::k_uiNoOwnerSlot);
	CHECK_EQ(world.Words()[world.VoxelID(5, 7, 9)], 0u);
	CHECK_FALSE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	CHECK_EQ(world.Bricks().GetCount(false, world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9))), 0u);
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelWorldHarness, OutOfBoundsWritesAreIgnored)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	const uint64_t uiBefore = world.Hash();

	world.Set(k_v3Size.x, 0, 0, k_uiStone, 1);
	world.Set(0, k_v3Size.y, 0, k_uiStone, 1);
	world.Set(0, 0, k_v3Size.z, k_uiStone, 1);
	world.Set(0xFFFFFFFFu, 0, 0, k_uiStone, 1);

	CHECK_EQ(world.Hash(), uiBefore);
	CHECK_EQ(world.CountOccupied(), 0u);
}

VOXAGINE_CHECK(VoxelWorldHarness, TheSameScriptHashesTheSameTwice)
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

	CHECK_EQ(build(), build());
}

VOXAGINE_CHECK(VoxelWorldHarness, TheHashSeesASingleVoxel)
{
	VoxelWorldHarness a(k_v3Size, k_v3ChunkSize);
	VoxelWorldHarness b(k_v3Size, k_v3ChunkSize);

	a.FillBox(UVector3(2, 2, 2), UVector3(4, 4, 4), k_uiStone, 1);
	b.FillBox(UVector3(2, 2, 2), UVector3(4, 4, 4), k_uiStone, 1);

	CHECK_EQ(a.Hash(), b.Hash());

	/* Colour, owner slot and occupancy each have to move the hash on their own,
	   or the net has a hole exactly where a phase might land a regression. */
	b.Set(3, 3, 3, k_uiWood, 1);
	CHECK_NE(a.Hash(), b.Hash());

	b.Set(3, 3, 3, k_uiStone, 2);
	CHECK_NE(a.Hash(), b.Hash());

	b.Set(3, 3, 3, k_uiStone, 1);
	CHECK_EQ(a.Hash(), b.Hash());

	b.Clear(3, 3, 3);
	CHECK_NE(a.Hash(), b.Hash());
}

/* A dynamic renderer's voxel: in the mapping, in the brick grid, and *not* in
   the physics grid. This is the asymmetry that shipped black particles - a
   colour read from the CPU voxel is zero here - and the harness has to be able
   to express it or no test can catch the next one. */
VOXAGINE_CHECK(VoxelWorldHarness, ADynamicVoxelExistsOnlyInTheMapping)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.SetDynamic(5, 7, 9, k_uiStone);

	CHECK_EQ(world.Words()[world.VoxelID(5, 7, 9)], k_uiStone);
	CHECK_TRUE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));

	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);
	REQUIRE_TRUE(static_cast<bool>(cell));
	CHECK_FALSE(cell.IsActive());
	CHECK_EQ(cell.GetColor(), 0u);
	CHECK_EQ(cell.GetSlot(), VoxelOwnerVolume::k_uiNoOwnerSlot);

	/* And the brick grid still agrees with the mapping, which is what the
	   marcher needs - the disagreement is only with the physics grid, which is
	   what the in-game sync audit reports as "occupied only on the GPU". */
	CHECK_EQ(world.Validate(), 0u);
}

/* Destruction reads the physics grid, so it does not see dynamic voxels at all
   - before or after the rewrite. Pinned, because "why did my character not get
   blown up" is a reasonable next question and this is the answer. */
VOXAGINE_CHECK(VoxelWorldHarness, ADynamicVoxelIsInvisibleToTheGrid)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.SetDynamic(5, 7, 9, k_uiStone);

	CHECK_EQ(world.CountOccupied(), 1u);
	CHECK_FALSE(world.Grid().GetCell(5, 7, 9).IsActive());
}
