#include "Framework/Check.h"

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
		explicit BrickGrid(const UVector3& v3Size = k_v3Size)
			: m_v3Size(v3Size)
		{
			m_Grid.Resize(v3Size);

			/* Sized for the whole coverage pyramid: the levels share one
			   buffer, so a mirror sized to the bricks alone is overrun by the
			   first write to any other level. */
			m_Front.assign(m_Grid.GetPyramidElementCount(), 0u);
			m_Back.assign(m_Grid.GetPyramidElementCount(), 0u);

			m_Grid.SetBuffers(m_Front.data(), m_Back.data());
			m_Grid.Flush();

			m_Words.assign(static_cast<size_t>(v3Size.x) * v3Size.y * v3Size.z, 0u);
		}

		VoxelBrickGrid& operator*() { return m_Grid; }
		VoxelBrickGrid* operator->() { return &m_Grid; }

		uint32_t ID(uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
		{
			return uiX + uiY * m_v3Size.x + uiZ * m_v3Size.x * m_v3Size.y;
		}

		/* The bulk path the chunk streamer uses, over a whole region. */
		void WriteRegion(const UVector3& v3Min, const UVector3& v3Size, uint32_t uiColor)
		{
			m_Grid.BeginRegion(false, v3Min, v3Size);

			for (uint32_t uiZ = v3Min.z; uiZ < v3Min.z + v3Size.z; ++uiZ)
			for (uint32_t uiY = v3Min.y; uiY < v3Min.y + v3Size.y; ++uiY)
			for (uint32_t uiX = v3Min.x; uiX < v3Min.x + v3Size.x; ++uiX)
			{
				m_Words[ID(uiX, uiY, uiZ)] = uiColor;

				if ((uiColor >> 24) != 0)
					m_Grid.AddVoxel(false, uiX, uiY, uiZ);
			}

			m_Grid.EndRegion(false, v3Min, v3Size);
		}

		/* Keeps the stand-in voxel buffer in step, so Validate has something to
		   check the counts against. */
		void Write(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
		{
			m_Words[ID(uiX, uiY, uiZ)] = uiColor;
			m_Grid.SetVoxel(ID(uiX, uiY, uiZ), (uiColor >> 24) != 0);
		}

		uint32_t Validate() { return m_Grid.Validate(false, m_Words.data()); }

		/* The bricks no longer start at element zero, so a mirror read has to
		   go through the level offset the shader also derives. */
		const uint32_t* Mirror() const
		{
			return m_Front.data() + m_Grid.GetLevelOffset(VoxelBrickGrid::k_uiBrickLevel);
		}

		const uint32_t* LevelMirror(uint32_t uiLevel) const
		{
			return m_Front.data() + m_Grid.GetLevelOffset(uiLevel);
		}

	private:
		UVector3 m_v3Size;
		VoxelBrickGrid m_Grid;
		std::vector<uint32_t> m_Front;
		std::vector<uint32_t> m_Back;
		std::vector<uint32_t> m_Words;
	};

	const uint32_t k_uiStone = 0xFF808080u;
}

VOXAGINE_CHECK(VoxelBrickGrid, OnlyOccupancyTransitionsMoveTheCount)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(9, 9, 9));

	grid.Write(9, 9, 9, k_uiStone);
	CHECK_EQ(grid->GetCount(false, uiBrick), 1u);

	/* A different colour on an already-occupied voxel is not a transition -
	   rule 3's alpha byte is a rendererState tag, so it is never simply 1. */
	grid.Write(9, 9, 9, 0x7F102030u);
	CHECK_EQ(grid->GetCount(false, uiBrick), 1u);

	grid.Write(9, 9, 9, 0u);
	CHECK_EQ(grid->GetCount(false, uiBrick), 0u);

	/* Clearing an already-empty voxel must not underflow. */
	grid.Write(9, 9, 9, 0u);
	CHECK_EQ(grid->GetCount(false, uiBrick), 0u);

	CHECK_EQ(grid.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, TheMirrorFollowsTheCount)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(1, 2, 3));

	/* The mirror is written by the flush rather than by the write, so a count
	   and its mirror are equal at every point the GPU can observe them and
	   nowhere in between. */
	grid.Write(1, 2, 3, k_uiStone);
	grid->FlushDirty();
	CHECK_EQ(grid.Mirror()[uiBrick], 1u);

	grid.Write(1, 2, 3, 0u);
	grid->FlushDirty();
	CHECK_EQ(grid.Mirror()[uiBrick], 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, CountsEveryVoxelOfABrick)
{
	BrickGrid grid;

	const uint32_t uiBrick = grid->VoxelToBrick(grid.ID(8, 8, 8));

	for (uint32_t uiZ = 8; uiZ < 16; ++uiZ)
	for (uint32_t uiY = 8; uiY < 16; ++uiY)
	for (uint32_t uiX = 8; uiX < 16; ++uiX)
		grid.Write(uiX, uiY, uiZ, k_uiStone);

	CHECK_EQ(grid->GetCount(false, uiBrick), VoxelBrickGrid::k_uiBrickVolume);
	CHECK_EQ(grid.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, OutOfRangeVoxelsAreIgnored)
{
	BrickGrid grid;

	const uint64_t uiVoxels = static_cast<uint64_t>(k_v3Size.x) * k_v3Size.y * k_v3Size.z;

	grid->SetVoxel(static_cast<uint32_t>(uiVoxels), true);
	grid->SetVoxel(0xFFFFFFFFu, true);

	CHECK_FALSE(grid->IsOccupied(static_cast<uint32_t>(uiVoxels)));
	CHECK_EQ(grid.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, TheHashSeesTheCountsAndTheBits)
{
	BrickGrid a;
	BrickGrid b;

	CHECK_EQ(a->Hash(false), b->Hash(false));

	b.Write(3, 4, 5, k_uiStone);
	CHECK_NE(a->Hash(false), b->Hash(false));

	a.Write(3, 4, 5, k_uiStone);
	CHECK_EQ(a->Hash(false), b->Hash(false));

	/* Colour alone is not brick state, so the brick hash must not see it - the
	   voxel words are hashed separately by the harness. */
	b.Write(3, 4, 5, 0xFF010203u);
	CHECK_EQ(a->Hash(false), b->Hash(false));
}

/* --- Coverage pyramid (RENDERING_PLAN.md 7.1b) ---------------------------- */

VOXAGINE_CHECK(VoxelBrickGrid, TheLevelsAreLaidEndToEndFromTheFinest)
{
	BrickGrid grid;

	uint32_t uiExpected = 0;

	for (uint32_t uiLevel = 0; uiLevel < VoxelBrickGrid::k_uiPyramidLevels; ++uiLevel)
	{
		const uint32_t uiCell = VoxelBrickGrid::LevelSize(uiLevel);
		const UVector3& v3Level = grid->GetLevelGridSize(uiLevel);

		/* Ceil-div, so a window that is not a whole number of cells keeps its
		   edge - the shader's PyramidLevelGridSize does the same. */
		CHECK_EQ(v3Level.x, (k_v3Size.x + uiCell - 1) / uiCell) << "level " << uiLevel;
		CHECK_EQ(v3Level.y, (k_v3Size.y + uiCell - 1) / uiCell) << "level " << uiLevel;
		CHECK_EQ(v3Level.z, (k_v3Size.z + uiCell - 1) / uiCell) << "level " << uiLevel;

		CHECK_EQ(grid->GetLevelCellCount(uiLevel), v3Level.x * v3Level.y * v3Level.z);
		CHECK_EQ(grid->GetLevelOffset(uiLevel), uiExpected) << "level " << uiLevel;

		uiExpected += grid->GetLevelCellCount(uiLevel);
	}

	CHECK_EQ(grid->GetPyramidElementCount(), uiExpected);

	/* The bricks are a level of this rather than a structure beside it. */
	CHECK_EQ(grid->GetBrickCount(), grid->GetLevelCellCount(VoxelBrickGrid::k_uiBrickLevel));
	CHECK_EQ(grid->GetGridSize().x, grid->GetLevelGridSize(VoxelBrickGrid::k_uiBrickLevel).x);
}

VOXAGINE_CHECK(VoxelBrickGrid, OneVoxelIsCountedOnceAtEveryLevel)
{
	BrickGrid grid;

	grid.Write(9, 9, 9, k_uiStone);

	/* Everything but the bricks is built by FlushDirty, once a frame, from
	   what the writes marked - so a test that reads a level directly has to
	   stand where the GPU stands. */
	grid->FlushDirty();

	for (uint32_t uiLevel = 0; uiLevel < VoxelBrickGrid::k_uiPyramidLevels; ++uiLevel)
	{
		uint32_t uiTotal = 0;

		for (uint32_t uiCell = 0; uiCell < grid->GetLevelCellCount(uiLevel); ++uiCell)
			uiTotal += grid->GetLevelCount(false, uiLevel, uiCell);

		CHECK_EQ(uiTotal, 1u) << "level " << uiLevel;

		/* And in the cell the voxel actually falls in, which is the half a
		   total cannot catch: a level addressed with another level's grid
		   dimensions still counts one voxel once, just in the wrong place. */
		const uint32_t uiShift = VoxelBrickGrid::k_uiFineShift + uiLevel;
		const UVector3& v3Level = grid->GetLevelGridSize(uiLevel);

		const uint32_t uiCellID = (9u >> uiShift)
			+ (9u >> uiShift) * v3Level.x
			+ (9u >> uiShift) * v3Level.x * v3Level.y;

		CHECK_EQ(grid->GetLevelCount(false, uiLevel, uiCellID), 1u) << "level " << uiLevel;
		CHECK_EQ(grid.LevelMirror(uiLevel)[uiCellID], 1u) << "level " << uiLevel;
	}

	CHECK_EQ(grid.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, ACoarseCellSumsTheFineOnesUnderIt)
{
	BrickGrid grid;

	/* One whole brick, so every level from the brick up must read 64 and the
	   level below it must read 8 in each of its eight cells. */
	for (uint32_t uiZ = 8; uiZ < 12; ++uiZ)
	for (uint32_t uiY = 8; uiY < 12; ++uiY)
	for (uint32_t uiX = 8; uiX < 12; ++uiX)
		grid.Write(uiX, uiY, uiZ, k_uiStone);

	grid->FlushDirty();

	for (uint32_t uiLevel = VoxelBrickGrid::k_uiBrickLevel; uiLevel < VoxelBrickGrid::k_uiPyramidLevels; ++uiLevel)
	{
		const uint32_t uiShift = VoxelBrickGrid::k_uiFineShift + uiLevel;
		const UVector3& v3Level = grid->GetLevelGridSize(uiLevel);

		const uint32_t uiCellID = (8u >> uiShift)
			+ (8u >> uiShift) * v3Level.x
			+ (8u >> uiShift) * v3Level.x * v3Level.y;

		CHECK_EQ(grid->GetLevelCount(false, uiLevel, uiCellID), 64u) << "level " << uiLevel;
	}

	for (uint32_t uiCell = 0; uiCell < grid->GetLevelCellCount(0); ++uiCell)
	{
		const uint32_t uiCount = grid->GetLevelCount(false, 0, uiCell);

		if (uiCount != 0)
			CHECK_EQ(uiCount, 8u) << "level 0 cell " << uiCell;
	}

	CHECK_EQ(grid.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelBrickGrid, TheBulkRegionPathMaintainsEveryLevel)
{
	/* Cell-aligned in every axis, which the window and the far field both are
	   in practice - EndRegion marks a straddling cell fully occupied, and at
	   the coarse levels that is 32^3 voxels of ambient occlusion asserted from
	   one voxel of evidence. */
	BrickGrid grid(UVector3(32, 32, 32));

	grid.WriteRegion(UVector3(0, 0, 0), UVector3(32, 32, 32), k_uiStone);
	grid->FlushDirty();

	for (uint32_t uiLevel = 0; uiLevel < VoxelBrickGrid::k_uiPyramidLevels; ++uiLevel)
	{
		for (uint32_t uiCell = 0; uiCell < grid->GetLevelCellCount(uiLevel); ++uiCell)
		{
			CHECK_EQ(grid->GetLevelCount(false, uiLevel, uiCell), VoxelBrickGrid::LevelVolume(uiLevel))
				<< "level " << uiLevel << " cell " << uiCell;

			CHECK_EQ(grid.LevelMirror(uiLevel)[uiCell], VoxelBrickGrid::LevelVolume(uiLevel))
				<< "level " << uiLevel << " cell " << uiCell;
		}
	}

	CHECK_EQ(grid.Validate(), 0u);

	/* And emptying it again through the same path returns every level to zero,
	   rather than leaving the coarse ones holding what the fine ones lost. */
	grid.WriteRegion(UVector3(0, 0, 0), UVector3(32, 32, 32), 0u);
	grid->FlushDirty();

	for (uint32_t uiLevel = 0; uiLevel < VoxelBrickGrid::k_uiPyramidLevels; ++uiLevel)
	{
		for (uint32_t uiCell = 0; uiCell < grid->GetLevelCellCount(uiLevel); ++uiCell)
			CHECK_EQ(grid->GetLevelCount(false, uiLevel, uiCell), 0u) << "level " << uiLevel;
	}

	CHECK_EQ(grid.Validate(), 0u);
}
