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

			m_Front.assign(m_Grid.GetBrickCount(), 0u);
			m_Back.assign(m_Grid.GetBrickCount(), 0u);

			m_Density.assign(m_Grid.GetFineCellCount(), 0u);
			m_DensityBack.assign(m_Grid.GetFineCellCount(), 0u);

			m_Grid.SetBuffers(m_Front.data(), m_Back.data());
			m_Grid.SetDensityBuffers(m_Density.data(), m_DensityBack.data());
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
					m_Grid.AddVoxel(false, uiX, uiY, uiZ, uiColor);
			}

			m_Grid.EndRegion(false, v3Min, v3Size);
		}

		/* Keeps the stand-in voxel buffer in step, so Validate has something to
		   check the counts against. */
		void Write(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
		{
			m_Words[ID(uiX, uiY, uiZ)] = uiColor;
			m_Grid.SetVoxel(ID(uiX, uiY, uiZ), uiColor);
		}

		uint32_t Validate() { return m_Grid.Validate(false, m_Words.data()); }

		/* The buffer the marcher binds holds the brick level and nothing else
		   (RENDERING_PLAN.md 7.1b route B), so it starts at element zero
		   again. */
		const uint32_t* Mirror() const { return m_Front.data(); }

		/* The staging side of the coverage texture: the finest level as one
		   RGBA texel a cell, alpha the occupied fraction and RGB the linear
		   albedo premultiplied by it (RENDERING_PLAN.md 7.3). */
		uint32_t Density(uint32_t uiCell) const { return (m_Density[uiCell] >> 24) & 0xFFu; }
		uint32_t Albedo(uint32_t uiCell) const { return m_Density[uiCell] & 0x00FFFFFFu; }

		static uint32_t DensityOf(uint32_t uiCount)
		{
			const uint32_t uiVolume = VoxelBrickGrid::LevelVolume(0);

			return (uiCount * 255u + uiVolume / 2u) / uiVolume;
		}

		/* What WriteDensityMirror should have produced for a cell of this
		   density holding voxels of this colour: the sRGB colour decoded to
		   linear, then scaled by the density. */
		static uint32_t PremultipliedOf(uint32_t uiColor, uint32_t uiDensity)
		{
			const uint32_t uiLinear = VoxelBrickGrid::PackLinearAlbedo(uiColor);

			uint32_t uiPacked = 0;

			for (uint32_t uiChannel = 0; uiChannel < 3; ++uiChannel)
			{
				const uint32_t uiValue = ((uiLinear >> (uiChannel * 8)) & 0xFFu) * uiDensity;

				uiPacked |= ((uiValue + 127u) / 255u) << (uiChannel * 8);
			}

			return uiPacked;
		}

	private:
		UVector3 m_v3Size;
		VoxelBrickGrid m_Grid;
		std::vector<uint32_t> m_Front;
		std::vector<uint32_t> m_Back;
		std::vector<uint32_t> m_Density;
		std::vector<uint32_t> m_DensityBack;
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

	grid->SetVoxel(static_cast<uint32_t>(uiVoxels), k_uiStone);
	grid->SetVoxel(0xFFFFFFFFu, k_uiStone);

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

VOXAGINE_CHECK(VoxelBrickGrid, EveryLevelHalvesTheOneBelowItAndKeepsItsEdge)
{
	BrickGrid grid;

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
	}

	/* The bricks are a level of this rather than a structure beside it. */
	CHECK_EQ(grid->GetBrickCount(), grid->GetLevelCellCount(VoxelBrickGrid::k_uiBrickLevel));
	CHECK_EQ(grid->GetGridSize().x, grid->GetLevelGridSize(VoxelBrickGrid::k_uiBrickLevel).x);

	/* The finest level is what the coverage texture's mip 0 has to hold, one
	   texel a cell. */
	CHECK_EQ(grid->GetFineCellCount(), grid->GetLevelCellCount(0));
	CHECK_EQ(grid->GetFineGridSize().x, grid->GetLevelGridSize(0).x);
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

		/* Only two levels leave the CPU, and they leave it differently: the
		   bricks as a count in the buffer the marcher binds, the finest as a
		   density byte staged for the coverage texture. */
		if (uiLevel == VoxelBrickGrid::k_uiBrickLevel)
			CHECK_EQ(grid.Mirror()[uiCellID], 1u);

		if (uiLevel == 0)
			CHECK_EQ(grid.Density(uiCellID), BrickGrid::DensityOf(1));
	}

	CHECK_EQ(grid.Validate(), 0u);
}

/* RENDERING_PLAN.md 7.3. The texel a bounce cone samples is the brick's albedo
   in linear light, scaled by how full the cell is - so a half-empty cell of a
   red wall reads as half as much red, and the box average that builds the
   coarser mips stays exact without knowing anything about coverage. */
VOXAGINE_CHECK(VoxelBrickGrid, TheFineTexelCarriesPremultipliedLinearAlbedo)
{
	BrickGrid grid;

	const uint32_t uiRed = 0xFF2040C0u;

	/* One voxel of a two-voxel-per-axis cell: LevelVolume(0) is 8, so this is
	   an eighth full and the colour arrives at an eighth of its strength. */
	grid.Write(9, 9, 9, uiRed);
	grid->FlushDirty();

	const UVector3& v3Fine = grid->GetLevelGridSize(0);

	const uint32_t uiCellID = (9u >> VoxelBrickGrid::k_uiFineShift)
		+ (9u >> VoxelBrickGrid::k_uiFineShift) * v3Fine.x
		+ (9u >> VoxelBrickGrid::k_uiFineShift) * v3Fine.x * v3Fine.y;

	const uint32_t uiDensity = BrickGrid::DensityOf(1);

	CHECK_EQ(grid.Density(uiCellID), uiDensity);
	CHECK_EQ(grid.Albedo(uiCellID), BrickGrid::PremultipliedOf(uiRed, uiDensity));

	/* Filling the rest of the cell leaves the colour alone and takes the
	   density to full, which is the only way the two can be told apart. */
	for (uint32_t uiZ = 8; uiZ < 10; ++uiZ)
	for (uint32_t uiY = 8; uiY < 10; ++uiY)
	for (uint32_t uiX = 8; uiX < 10; ++uiX)
		grid.Write(uiX, uiY, uiZ, uiRed);

	grid->FlushDirty();

	CHECK_EQ(grid.Density(uiCellID), 255u);
	CHECK_EQ(grid.Albedo(uiCellID), VoxelBrickGrid::PackLinearAlbedo(uiRed));

	/* And emptying it takes the premultiplied colour to black with it, rather
	   than leaving a lit cell where there is no longer anything to light. */
	for (uint32_t uiZ = 8; uiZ < 10; ++uiZ)
	for (uint32_t uiY = 8; uiY < 10; ++uiY)
	for (uint32_t uiX = 8; uiX < 10; ++uiX)
		grid.Write(uiX, uiY, uiZ, 0u);

	grid->FlushDirty();

	CHECK_EQ(grid.Density(uiCellID), 0u);
	CHECK_EQ(grid.Albedo(uiCellID), 0u);

	CHECK_EQ(grid.Validate(), 0u);
}

/* The decode has to be the same curve the shaders use, or the bounce arrives in
   a different colour space from everything it is added to. Endpoints are exact;
   mid grey is the value Color.hlsl's SrgbToLinear produces. */
VOXAGINE_CHECK(VoxelBrickGrid, TheAlbedoDecodeIsTheSrgbCurve)
{
	CHECK_EQ(VoxelBrickGrid::PackLinearAlbedo(0xFF000000u) & 0xFFu, 0u);
	CHECK_EQ(VoxelBrickGrid::PackLinearAlbedo(0xFFFFFFFFu) & 0xFFu, 255u);

	/* 0.5 sRGB is 0.2140 linear, which is 55 of 255. */
	CHECK_EQ(VoxelBrickGrid::PackLinearAlbedo(0xFF808080u) & 0xFFu, 55u);

	/* Channels do not bleed into each other, and they keep the voxel word's own
	   order: red is the *low* byte and the tag is the high one, which is the
	   layout an R8G8B8A8_UNORM texel wants anyway. */
	const uint32_t uiPacked = VoxelBrickGrid::PackLinearAlbedo(0xFF0000FFu);

	CHECK_EQ(uiPacked & 0xFFu, 255u);
	CHECK_EQ((uiPacked >> 8) & 0xFFu, 0u);
	CHECK_EQ((uiPacked >> 16) & 0xFFu, 0u);
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

			if (uiLevel == VoxelBrickGrid::k_uiBrickLevel)
			{
				CHECK_EQ(grid.Mirror()[uiCell], VoxelBrickGrid::k_uiBrickVolume)
					<< "cell " << uiCell;
			}

			/* Solid, so the density byte is saturated - and this is the check
			   that the count-to-byte conversion is exact at full occupancy
			   rather than one short of it. */
			if (uiLevel == 0)
				CHECK_EQ(grid.Density(uiCell), 255u) << "cell " << uiCell;
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
		{
			CHECK_EQ(grid->GetLevelCount(false, uiLevel, uiCell), 0u) << "level " << uiLevel;

			if (uiLevel == 0)
				CHECK_EQ(grid.Density(uiCell), 0u) << "cell " << uiCell;
		}
	}

	CHECK_EQ(grid.Validate(), 0u);
}

/* --- What reaches the coverage texture (RENDERING_PLAN.md 7.1b route B) ---- */

VOXAGINE_CHECK(VoxelBrickGrid, EveryDensityWriteIsCoveredByADirtyRegion)
{
	BrickGrid grid;

	/* Two bursts far apart, so the regions cannot be one box by accident, and
	   a run along x inside each so the merge is exercised. */
	for (uint32_t uiX = 4; uiX < 16; ++uiX)
		grid.Write(uiX, 5, 5, k_uiStone);

	for (uint32_t uiX = 20; uiX < 24; ++uiX)
		grid.Write(uiX, 17, 19, k_uiStone);

	grid->FlushDirty();

	std::vector<ImageRegion> regions;
	const bool bFull = grid->TakeDensityRegions(regions);

	/* The first take after a Flush is always "all of it" - the mirror has just
	   been repopulated wholesale - so this drains that one and measures the
	   next. */
	CHECK_TRUE(bFull);
	CHECK_TRUE(regions.empty());

	for (uint32_t uiX = 4; uiX < 16; ++uiX)
		grid.Write(uiX, 9, 5, k_uiStone);

	grid->FlushDirty();

	CHECK_FALSE(grid->TakeDensityRegions(regions));
	CHECK_FALSE(regions.empty());

	/* Every cell whose density is non-zero at the finest level and was written
	   by that second burst has to be inside one of the boxes. A box the
	   uploader never receives is stale ambient occlusion, which nothing about
	   the image or the counts would report. */
	const UVector3& v3Fine = grid->GetFineGridSize();

	uint32_t uiUncovered = 0;

	for (uint32_t uiX = 4; uiX < 16; ++uiX)
	{
		const UVector3 v3Cell(uiX >> VoxelBrickGrid::k_uiFineShift,
		                      9u >> VoxelBrickGrid::k_uiFineShift,
		                      5u >> VoxelBrickGrid::k_uiFineShift);

		bool bCovered = false;

		for (const ImageRegion& region : regions)
		{
			bCovered = bCovered ||
				(v3Cell.x >= region.m_uiX && v3Cell.x < region.m_uiX + region.m_uiWidth &&
				 v3Cell.y >= region.m_uiY && v3Cell.y < region.m_uiY + region.m_uiHeight &&
				 v3Cell.z >= region.m_uiZ && v3Cell.z < region.m_uiZ + region.m_uiDepth);
		}

		if (!bCovered)
			++uiUncovered;
	}

	CHECK_EQ(uiUncovered, 0u);

	/* And no box may name a cell that does not exist: the finest grid is a
	   ceil-div of a window that need not be a whole number of bricks. */
	for (const ImageRegion& region : regions)
	{
		CHECK_TRUE(region.m_uiX + region.m_uiWidth <= v3Fine.x);
		CHECK_TRUE(region.m_uiY + region.m_uiHeight <= v3Fine.y);
		CHECK_TRUE(region.m_uiZ + region.m_uiDepth <= v3Fine.z);
	}

	/* Taking them clears them; nothing is uploaded twice and nothing is left
	   behind to make the next audit report a backlog. */
	CHECK_FALSE(grid->HasPendingDensityRegions());
}

VOXAGINE_CHECK(VoxelBrickGrid, ASwapMakesTheWholeTextureStale)
{
	BrickGrid grid;

	std::vector<ImageRegion> regions;
	grid->TakeDensityRegions(regions);

	grid.Write(9, 9, 9, k_uiStone);
	grid->FlushDirty();

	CHECK_TRUE(grid->HasPendingDensityRegions());

	/* The texture holds one buffer's densities, and the swap makes that the
	   wrong buffer whatever either mirror has been kept current with - so the
	   pending boxes are worthless and the answer is "all of it". */
	grid->Swap();

	CHECK_TRUE(grid->TakeDensityRegions(regions));
	CHECK_TRUE(regions.empty());
}
