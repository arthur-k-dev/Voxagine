#include "Framework/Check.h"

#include "Core/Particles/ParticleLanding.h"
#include "Harness/VoxelWorldHarness.h"

namespace
{
	const UVector3 k_v3Size(32, 24, 32);
	const UVector3 k_v3ChunkSize(16, 24, 16);

	const uint32_t k_uiStone = 0xFF808080u;
}

VOXAGINE_CHECK(ParticleLanding, PrefersTheCellDirectlyAbove)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);

	int32_t iX = 0;
	int32_t iY = 0;
	int32_t iZ = 0;

	REQUIRE_TRUE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 10, 0, 10, iX, iY, iZ));

	CHECK_EQ(iX, 10);
	CHECK_EQ(iY, 1);
	CHECK_EQ(iZ, 10);
}

VOXAGINE_CHECK(ParticleLanding, StepsAsideWhenTheCellAboveIsTaken)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);
	world.Set(10, 1, 10, k_uiStone, 1);

	int32_t iX = 0;
	int32_t iY = 0;
	int32_t iZ = 0;

	REQUIRE_TRUE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 10, 0, 10, iX, iY, iZ));

	/* Somewhere in the neighbourhood, empty, and not the taken cell. */
	CHECK_FALSE(iX == 10 && iY == 1 && iZ == 10);
	CHECK_LE(std::abs(iX - 10), 1);
	CHECK_LE(std::abs(iZ - 10), 1);
	CHECK_FALSE(world.Bricks().IsOccupied(world.VoxelID(
		static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ))));
}

VOXAGINE_CHECK(ParticleLanding, FailsWhenThereIsNowhereToGo)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	/* Solid everywhere the search can reach. */
	world.FillBox(UVector3(8, 8, 8), UVector3(8, 8, 8), k_uiStone, 1);

	int32_t iX = 0;
	int32_t iY = 0;
	int32_t iZ = 0;

	CHECK_FALSE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 11, 11, 11, iX, iY, iZ));
}

/* A landing at the very edge must not resolve to a cell outside the window -
   that is a write the batch would reject, so the debris would silently vanish
   rather than settle one voxel over. */
VOXAGINE_CHECK(ParticleLanding, StaysInsideTheWindow)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);

	int32_t iX = 0;
	int32_t iY = 0;
	int32_t iZ = 0;

	REQUIRE_TRUE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 0, 0, 0, iX, iY, iZ));
	CHECK_TRUE(ParticleLanding::IsInside(k_v3Size, iX, iY, iZ));

	REQUIRE_TRUE(ParticleLanding::Resolve(
		world.Bricks(), k_v3Size,
		static_cast<int32_t>(k_v3Size.x) - 1, 0, static_cast<int32_t>(k_v3Size.z) - 1,
		iX, iY, iZ));

	CHECK_TRUE(ParticleLanding::IsInside(k_v3Size, iX, iY, iZ));

	/* Directly under the ceiling there is no cell above, so it must find one
	   beside rather than resolving out of bounds. */
	world.FillBox(UVector3(0, k_v3Size.y - 1, 0), UVector3(4, 1, 4), k_uiStone, 1);

	CHECK_TRUE(ParticleLanding::Resolve(
		world.Bricks(), k_v3Size, 1, static_cast<int32_t>(k_v3Size.y) - 1, 1, iX, iY, iZ) == false ||
		ParticleLanding::IsInside(k_v3Size, iX, iY, iZ));
}

VOXAGINE_CHECK(ParticleLanding, IsDeterministic)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);
	world.Set(10, 1, 10, k_uiStone, 1);

	int32_t iFirstX = 0, iFirstY = 0, iFirstZ = 0;
	int32_t iSecondX = 0, iSecondY = 0, iSecondZ = 0;

	REQUIRE_TRUE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 10, 0, 10, iFirstX, iFirstY, iFirstZ));
	REQUIRE_TRUE(ParticleLanding::Resolve(world.Bricks(), k_v3Size, 10, 0, 10, iSecondX, iSecondY, iSecondZ));

	CHECK_EQ(iFirstX, iSecondX);
	CHECK_EQ(iFirstY, iSecondY);
	CHECK_EQ(iFirstZ, iSecondZ);
}

VOXAGINE_CHECK(ParticleLanding, OccupancyMatchesTheGrid)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.Set(5, 6, 7, k_uiStone, 1);

	CHECK_TRUE(ParticleLanding::IsOccupied(world.Bricks(), k_v3Size, 5, 6, 7));
	CHECK_FALSE(ParticleLanding::IsOccupied(world.Bricks(), k_v3Size, 5, 7, 7));

	/* Out of bounds reads as empty, never as occupied - a particle leaving the
	   window must not read as having hit something. */
	CHECK_FALSE(ParticleLanding::IsOccupied(world.Bricks(), k_v3Size, -1, 6, 7));
	CHECK_FALSE(ParticleLanding::IsOccupied(world.Bricks(), k_v3Size,
		static_cast<int32_t>(k_v3Size.x), 6, 7));
}
