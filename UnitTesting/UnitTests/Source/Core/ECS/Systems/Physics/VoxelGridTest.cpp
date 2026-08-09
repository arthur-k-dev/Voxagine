#include <gtest/gtest.h>

#include "Harness/VoxelWorldHarness.h"

namespace
{
	const UVector3 k_v3Size(48, 24, 48);
	const UVector3 k_v3ChunkSize(16, 24, 16);

	const uint32_t k_uiStone = 0xFF808080u;
}

/* GetChunk is the grid's one bulk reader and it exists in two near-duplicate
   implementations picked by an alignment test (D10). Whichever one runs, it has
   to agree with GetVoxel voxel for voxel - and the phase 1 consolidation is
   only safe because this says so first. */
TEST(VoxelGrid, GetChunkAgreesWithGetVoxel)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
	for (uint32_t uiY = 0; uiY < k_v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
	{
		if (((uiX * 7 + uiY * 13 + uiZ * 29) % 5) == 0)
			world.Set(uiX, uiY, uiZ, k_uiStone | uiX, static_cast<uint16_t>(1 + (uiX % 3)));
	}

	/* Origins deliberately on and off a chunk boundary, and sizes that do and
	   do not fit inside one chunk - which is what selects between the two
	   implementations. */
	const Vector3 origins[] = {
		Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(15, 0, 15),
		Vector3(16, 0, 16), Vector3(14, 3, 30), Vector3(31, 5, 2),
	};

	const Vector3 sizes[] = { Vector3(1, 1, 1), Vector3(4, 4, 4), Vector3(9, 5, 9), Vector3(17, 2, 3) };

	for (const Vector3& v3Origin : origins)
	{
		for (const Vector3& v3Size : sizes)
		{
			const uint32_t uiCount =
				static_cast<uint32_t>(v3Size.x) *
				static_cast<uint32_t>(v3Size.y) *
				static_cast<uint32_t>(v3Size.z);

			std::vector<Voxel*> voxels(uiCount, nullptr);
			std::vector<uint16_t> slots(uiCount, 0);

			ASSERT_TRUE(world.Grid().GetChunk(voxels.data(), v3Origin, v3Size, true, slots.data()))
				<< "origin " << v3Origin.x << "," << v3Origin.y << "," << v3Origin.z;

			for (uint32_t uiZ = 0; uiZ < static_cast<uint32_t>(v3Size.z); ++uiZ)
			for (uint32_t uiY = 0; uiY < static_cast<uint32_t>(v3Size.y); ++uiY)
			for (uint32_t uiX = 0; uiX < static_cast<uint32_t>(v3Size.x); ++uiX)
			{
				const uint32_t uiIndex =
					uiX +
					uiY * static_cast<uint32_t>(v3Size.x) +
					uiZ * static_cast<uint32_t>(v3Size.x) * static_cast<uint32_t>(v3Size.y);

				const uint32_t uiWorldX = static_cast<uint32_t>(v3Origin.x) + uiX;
				const uint32_t uiWorldY = static_cast<uint32_t>(v3Origin.y) + uiY;
				const uint32_t uiWorldZ = static_cast<uint32_t>(v3Origin.z) + uiZ;

				if (uiWorldX >= k_v3Size.x || uiWorldY >= k_v3Size.y || uiWorldZ >= k_v3Size.z)
					continue;

				const VoxelCell cell = world.Grid().GetCell(uiWorldX, uiWorldY, uiWorldZ);

				ASSERT_NE(voxels[uiIndex], nullptr)
					<< "origin " << v3Origin.x << "," << v3Origin.y << "," << v3Origin.z
					<< " size " << v3Size.x << "," << v3Size.y << "," << v3Size.z
					<< " at " << uiWorldX << "," << uiWorldY << "," << uiWorldZ;

				EXPECT_EQ(voxels[uiIndex]->Color, cell.GetColor())
					<< "origin " << v3Origin.x << "," << v3Origin.y << "," << v3Origin.z
					<< " size " << v3Size.x << "," << v3Size.y << "," << v3Size.z
					<< " at " << uiWorldX << "," << uiWorldY << "," << uiWorldZ;

				EXPECT_EQ(slots[uiIndex], cell.GetSlot())
					<< "origin " << v3Origin.x << "," << v3Origin.y << "," << v3Origin.z
					<< " at " << uiWorldX << "," << uiWorldY << "," << uiWorldZ;
			}
		}
	}
}

TEST(VoxelGrid, OutOfBoundsReadsReportNothing)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	EXPECT_EQ(world.Grid().GetVoxel(k_v3Size.x, 0, 0), nullptr);
	EXPECT_EQ(world.Grid().GetVoxel(0, k_v3Size.y, 0), nullptr);
	EXPECT_EQ(world.Grid().GetVoxel(0, 0, k_v3Size.z), nullptr);

	EXPECT_FALSE(static_cast<bool>(world.Grid().GetCell(k_v3Size.x, 0, 0)));
	EXPECT_FALSE(static_cast<bool>(world.Grid().GetCell(0, k_v3Size.y, 0)));
	EXPECT_FALSE(static_cast<bool>(world.Grid().GetCell(0, 0, k_v3Size.z)));
}

/* Rule 4: a slot is a stable identity for the life of the world. Two things
   follow and both are relied on elsewhere - the same entity always gets the
   same slot back, and no slot is ever handed to a second entity. */
TEST(VoxelGrid, OwnerSlotsAreStableAndUnique)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelGrid& grid = world.Grid();

	const uint16_t uiFirst = grid.AcquireOwnerSlot(1001);
	const uint16_t uiSecond = grid.AcquireOwnerSlot(1002);

	EXPECT_NE(uiFirst, VoxelOwnerVolume::k_uiNoOwnerSlot);
	EXPECT_NE(uiSecond, VoxelOwnerVolume::k_uiNoOwnerSlot);
	EXPECT_NE(uiFirst, uiSecond);

	EXPECT_EQ(grid.AcquireOwnerSlot(1001), uiFirst);
	EXPECT_EQ(grid.FindOwnerSlot(1001), uiFirst);
	EXPECT_EQ(grid.ResolveOwnerSlot(uiFirst), 1001u);

	/* An entity that has never stamped anything must not consume a slot just by
	   being asked about. */
	EXPECT_EQ(grid.FindOwnerSlot(9999), VoxelOwnerVolume::k_uiNoOwnerSlot);
	EXPECT_EQ(grid.ResolveOwnerSlot(VoxelOwnerVolume::k_uiNoOwnerSlot), 0u);

	/* The reserved slot never names an entity, and is never handed out. It can
	   still arrive from chunk data encoded before phase 3 deleted the particle
	   claims that used it (rule 4). */
	EXPECT_EQ(grid.ResolveOwnerSlot(VoxelOwnerVolume::k_uiReservedSlot), 0u);

	for (uint64_t uiEntity = 2000; uiEntity < 2064; ++uiEntity)
		EXPECT_NE(grid.AcquireOwnerSlot(uiEntity), VoxelOwnerVolume::k_uiReservedSlot);
}

TEST(VoxelGrid, WorldToGridIsTheInverseOfGridToWorld)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelGrid& grid = world.Grid();

	grid.SetWorldOffset(Vector3(64.f, 0.f, 128.f));

	for (uint32_t uiZ = 1; uiZ < k_v3Size.z; uiZ += 7)
	for (uint32_t uiY = 1; uiY < k_v3Size.y; uiY += 5)
	for (uint32_t uiX = 1; uiX < k_v3Size.x; uiX += 7)
	{
		const Vector3 v3Grid(uiX, uiY, uiZ);
		const Vector3 v3Back = grid.WorldToGrid(grid.GridToWorld(v3Grid), true);

		EXPECT_EQ(v3Back, v3Grid);
	}

	grid.SetWorldOffset(Vector3(0.f));
}
