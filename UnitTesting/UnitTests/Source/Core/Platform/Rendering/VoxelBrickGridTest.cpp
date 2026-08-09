#include <gtest/gtest.h>

#include <vector>

#include "Core/Platform/Rendering/VoxelBrickGrid.h"

namespace
{
	/* Deliberately not a multiple of the brick size in y, so the straddling-
	   brick rules in Begin/EndRegion are actually reached. */
	const UVector3 k_v3Size(32, 20, 32);

	class BrickGrid
	{
	public:
		BrickGrid()
		{
			m_Grid.Resize(k_v3Size);

			m_Front.assign(m_Grid.GetBrickCount(), 0u);
			m_Back.assign(m_Grid.GetBrickCount(), 0u);

			m_Grid.SetBuffers(m_Front.data(), m_Back.data());
			m_Grid.Flush();

			m_Words.assign(static_cast<size_t>(k_v3Size.x) * k_v3Size.y * k_v3Size.z, 0u);
		}

		VoxelBrickGrid& operator*() { return m_Grid; }
		VoxelBrickGrid* operator->() { return &m_Grid; }

		uint32_t ID(uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
		{
			return uiX + uiY * k_v3Size.x + uiZ * k_v3Size.x * k_v3Size.y;
		}

		/* Keeps the stand-in voxel buffer in step, so Validate has something to
		   check the counts against. */
		void Write(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
		{
			m_Words[ID(uiX, uiY, uiZ)] = uiColor;
			m_Grid.SetVoxel(ID(uiX, uiY, uiZ), (uiColor >> 24) != 0);
		}

		uint32_t Validate() { return m_Grid.Validate(false, m_Words.data()); }

		const uint32_t* Mirror() const { return m_Front.data(); }

	private:
		VoxelBrickGrid m_Grid;
		std::vector<uint32_t> m_Front;
		std::vector<uint32_t> m_Back;
		std::vector<uint32_t> m_Words;
	};

	const uint32_t k_uiStone = 0xFF808080u;
}

TEST(VoxelBrickGrid, OnlyOccupancyTransitionsMoveTheCount)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(9, 9, 9));

	grid.Write(9, 9, 9, k_uiStone);
	EXPECT_EQ(grid->GetCount(false, uiBrick), 1u);

	/* A different colour on an already-occupied voxel is not a transition -
	   rule 3's alpha byte is a rendererState tag, so it is never simply 1. */
	grid.Write(9, 9, 9, 0x7F102030u);
	EXPECT_EQ(grid->GetCount(false, uiBrick), 1u);

	grid.Write(9, 9, 9, 0u);
	EXPECT_EQ(grid->GetCount(false, uiBrick), 0u);

	/* Clearing an already-empty voxel must not underflow. */
	grid.Write(9, 9, 9, 0u);
	EXPECT_EQ(grid->GetCount(false, uiBrick), 0u);

	EXPECT_EQ(grid.Validate(), 0u);
}

TEST(VoxelBrickGrid, TheMirrorFollowsTheCount)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(1, 2, 3));

	grid.Write(1, 2, 3, k_uiStone);
	EXPECT_EQ(grid.Mirror()[uiBrick], 1u);

	grid.Write(1, 2, 3, 0u);
	EXPECT_EQ(grid.Mirror()[uiBrick], 0u);
}

TEST(VoxelBrickGrid, CountsEveryVoxelOfABrick)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(8, 8, 8));

	for (uint32_t uiZ = 8; uiZ < 16; ++uiZ)
	for (uint32_t uiY = 8; uiY < 16; ++uiY)
	for (uint32_t uiX = 8; uiX < 16; ++uiX)
		grid.Write(uiX, uiY, uiZ, k_uiStone);

	EXPECT_EQ(grid->GetCount(false, uiBrick), VoxelBrickGrid::k_uiBrickVolume);
	EXPECT_EQ(grid.Validate(), 0u);
}

TEST(VoxelBrickGrid, OutOfRangeVoxelsAreIgnored)
{
	BrickGrid grid;

	const uint64_t uiVoxels = static_cast<uint64_t>(k_v3Size.x) * k_v3Size.y * k_v3Size.z;

	grid->SetVoxel(static_cast<uint32_t>(uiVoxels), true);
	grid->SetVoxel(0xFFFFFFFFu, true);

	EXPECT_FALSE(grid->IsOccupied(static_cast<uint32_t>(uiVoxels)));
	EXPECT_EQ(grid.Validate(), 0u);
}

TEST(VoxelBrickGrid, TheHashSeesTheCountsAndTheBits)
{
	BrickGrid a;
	BrickGrid b;

	EXPECT_EQ(a->Hash(false), b->Hash(false));

	b.Write(3, 4, 5, k_uiStone);
	EXPECT_NE(a->Hash(false), b->Hash(false));

	a.Write(3, 4, 5, k_uiStone);
	EXPECT_EQ(a->Hash(false), b->Hash(false));

	/* Colour alone is not brick state, so the brick hash must not see it - the
	   voxel words are hashed separately by the harness. */
	b.Write(3, 4, 5, 0xFF010203u);
	EXPECT_EQ(a->Hash(false), b->Hash(false));
}
