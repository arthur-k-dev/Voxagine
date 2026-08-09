#include "pch.h"

#include "Core/Platform/Rendering/VoxelBrickGrid.h"

#include "Core/Platform/Rendering/FrameProfiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	uint32_t CeilDiv(uint32_t uiValue, uint32_t uiDivisor)
	{
		return (uiValue + uiDivisor - 1) / uiDivisor;
	}
}

void VoxelBrickGrid::Resize(const UVector3& v3WorldSize)
{
	/* Deliberately touches only CPU state: the mirrors belong to a Mapper the
	   caller has not resized yet, so the pointers in m_pGPU are about to be
	   freed. The caller resizes the mapper, calls SetBuffers, then Flush. */
	m_v3WorldSize = v3WorldSize;

	/* Level geometry first: every level is a ceil-div of the window by its own
	   cell size. The shader derives the brick level's the same way from
	   worldSize, and the texture's mips are halvings of level 0 - see
	   SetDensityBuffers. */
	bool bLevelsChanged = false;

	for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
	{
		const uint32_t uiCell = LevelSize(uiLevel);

		const UVector3 v3Grid(
			CeilDiv(v3WorldSize.x, uiCell),
			CeilDiv(v3WorldSize.y, uiCell),
			CeilDiv(v3WorldSize.z, uiCell)
		);

		const uint32_t uiCells = v3Grid.x * v3Grid.y * v3Grid.z;

		bLevelsChanged = bLevelsChanged || uiCells != m_uiLevelCells[uiLevel];

		m_v3LevelGrid[uiLevel] = v3Grid;
		m_uiLevelCells[uiLevel] = uiCells;
	}

	const uint32_t uiVoxelCount = v3WorldSize.x * v3WorldSize.y * v3WorldSize.z;
	const uint32_t uiWordCount = CeilDiv(uiVoxelCount, 64);

	/* The brick grid is a level of the pyramid rather than a structure beside
	   it, so it takes its size from one - k_uiBrickLevel's cell is
	   k_uiBrickSize by construction. */
	m_v3GridSize = m_v3LevelGrid[k_uiBrickLevel];
	m_uiBrickCount = m_uiLevelCells[k_uiBrickLevel];

	if (bLevelsChanged)
	{
		for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
		{
			const uint32_t uiCells = m_uiLevelCells[uiLevel];

			for (uint32_t i = 0; i < 2; ++i)
				m_pLevels[uiLevel][i] = uiCells > 0
					? std::unique_ptr<std::atomic<uint16_t>[]>(new std::atomic<uint16_t>[uiCells]())
					: nullptr;

			/* Only the brick level and above are marked; level 0 is rebuilt
			   from the brick containing it. */
			m_uiDirtyWords[uiLevel] = uiLevel >= k_uiBrickLevel ? CeilDiv(uiCells, 64) : 0;

			for (uint32_t i = 0; i < 2; ++i)
				m_pDirty[uiLevel][i] = m_uiDirtyWords[uiLevel] > 0
					? std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[m_uiDirtyWords[uiLevel]]())
					: nullptr;
		}
	}
	else
	{
		ZeroCounts();
	}

	ClearDirty();

	/* Sized off the voxel count rather than the brick count: a window can keep
	   the same number of bricks while holding a different number of voxels
	   only if it is not brick-aligned, but the two are separate allocations
	   either way and the bitmap is by far the larger of them. */
	if (uiWordCount != m_uiWordCount)
	{
		m_uiWordCount = uiWordCount;

		for (uint32_t i = 0; i < 2; ++i)
			m_pOccupancy[i] = uiWordCount > 0
				? std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[uiWordCount]())
				: nullptr;
	}
	else
	{
		ZeroOccupancy();
	}

	m_uiVoxelCount = uiVoxelCount;

	m_pGPU[0] = nullptr;
	m_pGPU[1] = nullptr;

	m_pDensity[0] = nullptr;
	m_pDensity[1] = nullptr;

	m_bDensityFull[0] = true;
	m_bDensityFull[1] = true;

	m_DensityRegions.clear();
	m_uiLastDensityBrick = UINT32_MAX;
}

void VoxelBrickGrid::SetBuffers(uint32_t* pFront, uint32_t* pBack)
{
	m_pGPU[m_uiFront] = pFront;
	m_pGPU[m_uiFront ^ 1u] = pBack;
}

void VoxelBrickGrid::SetDensityBuffers(uint8_t* pFront, uint8_t* pBack)
{
	/* Here rather than at file scope because k_uiFineVolume is private, and in
	   the class because a constexpr member function is not complete there. */
	static_assert(k_uiFineVolume == LevelVolume(0),
	              "the density divisor is not the finest level's cell volume");

	/* A count has to survive the round trip through a unorm byte, which it does
	   only while the cell is small enough that the quantization steps are wider
	   than one voxel. */
	static_assert(k_uiFineVolume <= 16,
	              "a level-0 cell holds more voxels than a unorm byte can count exactly");

	m_pDensity[m_uiFront] = pFront;
	m_pDensity[m_uiFront ^ 1u] = pBack;

	m_bDensityFull[0] = true;
	m_bDensityFull[1] = true;

	m_DensityRegions.clear();
	m_uiLastDensityBrick = UINT32_MAX;
}

void VoxelBrickGrid::Swap()
{
	m_uiFront ^= 1u;

	/* The texture holds one buffer's densities and the swap makes that the
	   wrong one, whatever either mirror has been kept current with. */
	m_bDensityFull[m_uiFront] = true;
	m_DensityRegions.clear();
	m_uiLastDensityBrick = UINT32_MAX;
}

void VoxelBrickGrid::RecordDensityRegion(uint32_t uiBuffer, uint32_t uiBrickID)
{
	if (uiBuffer != m_uiFront || m_pDensity[uiBuffer] == nullptr || m_bDensityFull[uiBuffer])
		return;

	if (m_DensityRegions.size() >= k_uiMaxDensityRegions)
	{
		m_bDensityFull[uiBuffer] = true;
		m_DensityRegions.clear();
		m_uiLastDensityBrick = UINT32_MAX;

		return;
	}

	/* Fine cells per brick axis: a brick is k_uiBrickSize voxels and a level-0
	   cell is LevelSize(0) of them. */
	const uint32_t uiPerAxis = k_uiBrickSize / LevelSize(0);

	const uint32_t uiBrickX = uiBrickID % m_v3GridSize.x;

	/* Extending the last box only works along x, and only while the run has
	   not wrapped into the next brick row. */
	if (!m_DensityRegions.empty() && uiBrickID == m_uiLastDensityBrick + 1 && uiBrickX != 0)
	{
		ImageRegion& last = m_DensityRegions.back();

		last.m_uiWidth = std::min(last.m_uiWidth + uiPerAxis,
		                          m_v3LevelGrid[0].x - last.m_uiX);

		m_uiLastDensityBrick = uiBrickID;

		return;
	}

	const uint32_t uiBrickY = (uiBrickID / m_v3GridSize.x) % m_v3GridSize.y;
	const uint32_t uiBrickZ = uiBrickID / (m_v3GridSize.x * m_v3GridSize.y);

	const UVector3& v3Fine = m_v3LevelGrid[0];

	ImageRegion region;
	region.m_uiX = uiBrickX * uiPerAxis;
	region.m_uiY = uiBrickY * uiPerAxis;
	region.m_uiZ = uiBrickZ * uiPerAxis;

	if (region.m_uiX >= v3Fine.x || region.m_uiY >= v3Fine.y || region.m_uiZ >= v3Fine.z)
		return;

	/* Clamped rather than assumed: the finest grid is a ceil-div of a window
	   that need not be a whole number of bricks, so the last brick of an axis
	   may own fewer than uiPerAxis cells. */
	region.m_uiWidth = std::min(uiPerAxis, v3Fine.x - region.m_uiX);
	region.m_uiHeight = std::min(uiPerAxis, v3Fine.y - region.m_uiY);
	region.m_uiDepth = std::min(uiPerAxis, v3Fine.z - region.m_uiZ);

	m_DensityRegions.push_back(region);
	m_uiLastDensityBrick = uiBrickID;
}

bool VoxelBrickGrid::TakeDensityRegions(std::vector<ImageRegion>& regions)
{
	regions.clear();
	m_uiLastDensityBrick = UINT32_MAX;

	if (m_bDensityFull[m_uiFront])
	{
		m_bDensityFull[m_uiFront] = false;
		m_DensityRegions.clear();

		return true;
	}

	regions.swap(m_DensityRegions);
	m_DensityRegions.clear();

	return false;
}

void VoxelBrickGrid::ZeroCounts()
{
	for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
	{
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (m_pLevels[uiLevel][i] == nullptr)
				continue;

			for (uint32_t uiCell = 0; uiCell < m_uiLevelCells[uiLevel]; ++uiCell)
				m_pLevels[uiLevel][i][uiCell].store(0, std::memory_order_relaxed);
		}
	}
}

void VoxelBrickGrid::ZeroOccupancy()
{
	for (uint32_t i = 0; i < 2; ++i)
	{
		if (m_pOccupancy[i] == nullptr)
			continue;

		for (uint32_t uiWord = 0; uiWord < m_uiWordCount; ++uiWord)
			m_pOccupancy[i][uiWord].store(0, std::memory_order_relaxed);
	}
}

bool VoxelBrickGrid::IsOccupied(bool bBack, uint32_t uiVoxelID) const
{
	const uint32_t uiIndex = Index(bBack);

	if (uiVoxelID >= m_uiVoxelCount || m_pOccupancy[uiIndex] == nullptr)
		return false;

	const uint64_t uiWord = m_pOccupancy[uiIndex][uiVoxelID >> k_uiWordShift].load(std::memory_order_relaxed);

	return ((uiWord >> (uiVoxelID & k_uiWordMask)) & 1ull) != 0ull;
}

void VoxelBrickGrid::ClearOccupancyRegion(uint32_t uiBuffer, const UVector3& v3Min, const UVector3& v3Size)
{
	std::atomic<uint64_t>* pWords = m_pOccupancy[uiBuffer].get();

	if (pWords == nullptr || m_uiVoxelCount == 0)
		return;

	const uint32_t uiLastX = std::min(v3Min.x + v3Size.x, m_v3WorldSize.x);
	const uint32_t uiLastY = std::min(v3Min.y + v3Size.y, m_v3WorldSize.y);
	const uint32_t uiLastZ = std::min(v3Min.z + v3Size.z, m_v3WorldSize.z);

	if (v3Min.x >= uiLastX)
		return;

	for (uint32_t uiZ = v3Min.z; uiZ < uiLastZ; ++uiZ)
	{
		for (uint32_t uiY = v3Min.y; uiY < uiLastY; ++uiY)
		{
			/* A row of the region is contiguous in the bitmap, so this clears
			   whole words wherever it can and masks only the two ends. */
			const uint32_t uiFirstBit = VoxelID(v3Min.x, uiY, uiZ);
			const uint32_t uiLastBit = uiFirstBit + (uiLastX - v3Min.x);

			uint32_t uiBit = uiFirstBit;

			while (uiBit < uiLastBit)
			{
				const uint32_t uiWord = uiBit >> k_uiWordShift;
				const uint32_t uiOffset = uiBit & k_uiWordMask;
				const uint32_t uiRun = std::min(64u - uiOffset, uiLastBit - uiBit);

				if (uiRun == 64)
				{
					pWords[uiWord].store(0, std::memory_order_relaxed);
				}
				else
				{
					const uint64_t uiMask = ((1ull << uiRun) - 1ull) << uiOffset;
					pWords[uiWord].fetch_and(~uiMask, std::memory_order_relaxed);
				}

				uiBit += uiRun;
			}
		}
	}
}

void VoxelBrickGrid::Flush()
{
	for (uint32_t i = 0; i < 2; ++i)
	{
		for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
		{
			const std::atomic<uint16_t>* pCounts = m_pLevels[uiLevel][i].get();

			if (pCounts == nullptr)
				continue;

			for (uint32_t uiCell = 0; uiCell < m_uiLevelCells[uiLevel]; ++uiCell)
				WriteMirror(i, uiLevel, uiCell, pCounts[uiCell].load(std::memory_order_relaxed));
		}
	}

	m_bDensityFull[0] = true;
	m_bDensityFull[1] = true;

	m_DensityRegions.clear();
	m_uiLastDensityBrick = UINT32_MAX;
}

void VoxelBrickGrid::ClearAll()
{
	ZeroCounts();
	ZeroOccupancy();

	for (uint32_t i = 0; i < 2; ++i)
	{
		if (m_pGPU[i] != nullptr)
			memset(m_pGPU[i], 0, m_uiBrickCount * sizeof(uint32_t));

		if (m_pDensity[i] != nullptr)
			memset(m_pDensity[i], 0, m_uiLevelCells[0]);
	}

	m_bDensityFull[0] = true;
	m_bDensityFull[1] = true;

	m_DensityRegions.clear();
	m_uiLastDensityBrick = UINT32_MAX;
}

void VoxelBrickGrid::BeginRegion(bool bBack, const UVector3& v3Min, const UVector3& v3Size)
{
	/* Before the early-out below: the bitmap is per voxel and does not depend
	   on there being any bricks, and the caller is about to overwrite this
	   region either way. */
	ClearOccupancyRegion(Index(bBack), v3Min, v3Size);

	if (m_uiBrickCount == 0)
		return;

	std::atomic<uint16_t>* pCounts = m_pLevels[k_uiBrickLevel][Index(bBack)].get();

	if (pCounts == nullptr)
		return;

	/* Bricks only. Level 0 is rebuilt from the occupancy bitmap this just
	   cleared, and the levels above are rebuilt as sums of their children, so
	   neither has anything to zero - see FlushDirty. */
	const UVector3 v3First = LevelCellRangeFirst(k_uiBrickLevel, v3Min);
	const UVector3 v3Last = LevelCellRangeLast(k_uiBrickLevel, v3Min, v3Size);

	for (uint32_t uiZ = v3First.z; uiZ <= v3Last.z; ++uiZ)
	{
		for (uint32_t uiY = v3First.y; uiY <= v3Last.y; ++uiY)
		{
			for (uint32_t uiX = v3First.x; uiX <= v3Last.x; ++uiX)
				pCounts[BrickID(uiX, uiY, uiZ)].store(0, std::memory_order_relaxed);
		}
	}
}

void VoxelBrickGrid::EndRegion(bool bBack, const UVector3& v3Min, const UVector3& v3Size)
{
	if (m_uiBrickCount == 0)
		return;

	const uint32_t uiBuffer = Index(bBack);
	std::atomic<uint16_t>* pCounts = m_pLevels[k_uiBrickLevel][uiBuffer].get();

	if (pCounts == nullptr)
		return;

	/* A brick straddling the region edge was only counted for the part of it
	   the caller rewrote, so its count is an undercount of what is actually
	   there - and an undercounted brick that reaches zero deletes visible
	   geometry. Chunk regions are brick-aligned in practice (chunk sizes and
	   the window height are multiples of 8), so this is a guard against
	   content that is not, and it fires per region rather than per frame.

	   Only the bricks need it. Level 0 comes from the bitmap, which is exact
	   at any alignment, and the coarse levels inherit whatever the bricks say
	   - so an overcount here reads as a slightly over-occluded cell rather
	   than as a 32^3 block of ambient occlusion asserted from one voxel. */
	const bool bAligned =
		(v3Min.x % k_uiBrickSize) == 0 && (v3Size.x % k_uiBrickSize) == 0 &&
		(v3Min.y % k_uiBrickSize) == 0 && (v3Size.y % k_uiBrickSize) == 0 &&
		(v3Min.z % k_uiBrickSize) == 0 && (v3Size.z % k_uiBrickSize) == 0;

	if (!bAligned)
		ReportUnalignedRegion(k_uiBrickLevel, v3Min, v3Size);

	const UVector3 v3First = LevelCellRangeFirst(k_uiBrickLevel, v3Min);
	const UVector3 v3Last = LevelCellRangeLast(k_uiBrickLevel, v3Min, v3Size);

	for (uint32_t uiZ = v3First.z; uiZ <= v3Last.z; ++uiZ)
	{
		for (uint32_t uiY = v3First.y; uiY <= v3Last.y; ++uiY)
		{
			for (uint32_t uiX = v3First.x; uiX <= v3Last.x; ++uiX)
			{
				const uint32_t uiBrickID = BrickID(uiX, uiY, uiZ);

				if (!bAligned)
				{
					const bool bInside =
						uiX * k_uiBrickSize >= v3Min.x && (uiX + 1) * k_uiBrickSize <= v3Min.x + v3Size.x &&
						uiY * k_uiBrickSize >= v3Min.y && (uiY + 1) * k_uiBrickSize <= v3Min.y + v3Size.y &&
						uiZ * k_uiBrickSize >= v3Min.z && (uiZ + 1) * k_uiBrickSize <= v3Min.z + v3Size.z;

					if (!bInside)
						pCounts[uiBrickID].store(static_cast<uint16_t>(k_uiBrickVolume), std::memory_order_relaxed);
				}

				/* Marked rather than pushed: the mirror write, and everything
				   above and below this level, is FlushDirty's job now. */
				MarkDirty(uiBuffer, uiBrickID);
			}
		}
	}
}

/* --- The deferred pyramid build ------------------------------------------- */

void VoxelBrickGrid::ClearDirty()
{
	for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
	{
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (m_pDirty[uiLevel][i] == nullptr)
				continue;

			for (uint32_t uiWord = 0; uiWord < m_uiDirtyWords[uiLevel]; ++uiWord)
				m_pDirty[uiLevel][i][uiWord].store(0, std::memory_order_relaxed);
		}
	}

	m_bHasDirty[0].store(false, std::memory_order_relaxed);
	m_bHasDirty[1].store(false, std::memory_order_relaxed);
}

void VoxelBrickGrid::RebuildFineCells(uint32_t uiBuffer, uint32_t uiBrickID)
{
	std::atomic<uint16_t>* pFine = m_pLevels[0][uiBuffer].get();
	const std::atomic<uint64_t>* pWords = m_pOccupancy[uiBuffer].get();

	if (pFine == nullptr || pWords == nullptr)
		return;

	const UVector3& v3Bricks = m_v3LevelGrid[k_uiBrickLevel];
	const UVector3& v3Fine = m_v3LevelGrid[0];

	const uint32_t uiBrickX = uiBrickID % v3Bricks.x;
	const uint32_t uiBrickY = (uiBrickID / v3Bricks.x) % v3Bricks.y;
	const uint32_t uiBrickZ = uiBrickID / (v3Bricks.x * v3Bricks.y);

	/* k_uiBrickSize voxels a side, and a level-0 cell is LevelSize(0) of them,
	   so a brick holds this many fine cells per axis. */
	const uint32_t uiPerAxis = k_uiBrickSize / LevelSize(0);
	const uint32_t uiFineSize = LevelSize(0);

	uint16_t uiCounts[k_uiBrickVolume] = {};

	const uint32_t uiBaseX = uiBrickX * k_uiBrickSize;
	const uint32_t uiBaseY = uiBrickY * k_uiBrickSize;
	const uint32_t uiBaseZ = uiBrickZ * k_uiBrickSize;

	/* One read per (y, z) row of the brick rather than one per voxel: a row is
	   k_uiBrickSize adjacent bits, aligned, so it never straddles a word. */
	for (uint32_t uiZ = 0; uiZ < k_uiBrickSize && uiBaseZ + uiZ < m_v3WorldSize.z; ++uiZ)
	{
		for (uint32_t uiY = 0; uiY < k_uiBrickSize && uiBaseY + uiY < m_v3WorldSize.y; ++uiY)
		{
			const uint32_t uiFirstBit = VoxelID(uiBaseX, uiBaseY + uiY, uiBaseZ + uiZ);
			const uint32_t uiRun = std::min<uint32_t>(k_uiBrickSize, m_v3WorldSize.x - uiBaseX);

			const uint64_t uiWord = pWords[uiFirstBit >> k_uiWordShift].load(std::memory_order_relaxed);
			const uint64_t uiRow = (uiWord >> (uiFirstBit & k_uiWordMask)) & ((1ull << uiRun) - 1ull);

			if (uiRow == 0)
				continue;

			const uint32_t uiRowBase = (uiY / uiFineSize) * uiPerAxis + (uiZ / uiFineSize) * uiPerAxis * uiPerAxis;

			for (uint32_t uiX = 0; uiX < uiRun; ++uiX)
			{
				if (((uiRow >> uiX) & 1ull) != 0ull)
					++uiCounts[uiRowBase + uiX / uiFineSize];
			}
		}
	}

	for (uint32_t uiZ = 0; uiZ < uiPerAxis; ++uiZ)
	{
		for (uint32_t uiY = 0; uiY < uiPerAxis; ++uiY)
		{
			for (uint32_t uiX = 0; uiX < uiPerAxis; ++uiX)
			{
				const uint32_t uiCellX = uiBrickX * uiPerAxis + uiX;
				const uint32_t uiCellY = uiBrickY * uiPerAxis + uiY;
				const uint32_t uiCellZ = uiBrickZ * uiPerAxis + uiZ;

				if (uiCellX >= v3Fine.x || uiCellY >= v3Fine.y || uiCellZ >= v3Fine.z)
					continue;

				const uint32_t uiCellID = uiCellX + uiCellY * v3Fine.x + uiCellZ * v3Fine.x * v3Fine.y;
				const uint16_t uiCount = uiCounts[uiX + uiY * uiPerAxis + uiZ * uiPerAxis * uiPerAxis];

				pFine[uiCellID].store(uiCount, std::memory_order_relaxed);
				WriteMirror(uiBuffer, 0, uiCellID, uiCount);
			}
		}
	}
}

void VoxelBrickGrid::RebuildCoarseCell(uint32_t uiBuffer, uint32_t uiLevel, uint32_t uiCellID)
{
	std::atomic<uint16_t>* pCounts = m_pLevels[uiLevel][uiBuffer].get();
	const std::atomic<uint16_t>* pChildren = m_pLevels[uiLevel - 1][uiBuffer].get();

	if (pCounts == nullptr || pChildren == nullptr)
		return;

	const UVector3& v3Grid = m_v3LevelGrid[uiLevel];
	const UVector3& v3Child = m_v3LevelGrid[uiLevel - 1];

	const uint32_t uiX = uiCellID % v3Grid.x;
	const uint32_t uiY = (uiCellID / v3Grid.x) % v3Grid.y;
	const uint32_t uiZ = uiCellID / (v3Grid.x * v3Grid.y);

	uint32_t uiTotal = 0;

	for (uint32_t uiChildZ = 0; uiChildZ < 2; ++uiChildZ)
	{
		for (uint32_t uiChildY = 0; uiChildY < 2; ++uiChildY)
		{
			for (uint32_t uiChildX = 0; uiChildX < 2; ++uiChildX)
			{
				const uint32_t uiCX = uiX * 2 + uiChildX;
				const uint32_t uiCY = uiY * 2 + uiChildY;
				const uint32_t uiCZ = uiZ * 2 + uiChildZ;

				/* A window that is not a whole number of cells gives the edge
				   cell fewer than eight children, which is why this is a
				   bounds test rather than an unrolled sum. */
				if (uiCX >= v3Child.x || uiCY >= v3Child.y || uiCZ >= v3Child.z)
					continue;

				uiTotal += pChildren[uiCX + uiCY * v3Child.x + uiCZ * v3Child.x * v3Child.y]
					.load(std::memory_order_relaxed);
			}
		}
	}

	/* Saturating, not wrapping: an overcounted brick from an unaligned region
	   can push a coarse cell past what its volume can hold, and a wrapped
	   count would read as empty - which deletes geometry rather than
	   over-occluding it. */
	const uint16_t uiCount = static_cast<uint16_t>(std::min<uint32_t>(uiTotal, UINT16_MAX));

	pCounts[uiCellID].store(uiCount, std::memory_order_relaxed);
	WriteMirror(uiBuffer, uiLevel, uiCellID, uiCount);
}

void VoxelBrickGrid::FlushDirty()
{
	/* Reported whether or not anything was dirty: this is the cost the
	   deferral moved out of the write paths, and a frame where it is zero is
	   as much a part of the picture as the world load where it is not. */
	ScopedFrameTimer timer("CPU VoxelBrickGrid::FlushDirty");

	for (uint32_t uiBuffer = 0; uiBuffer < 2; ++uiBuffer)
	{
		if (!m_bHasDirty[uiBuffer].exchange(false, std::memory_order_relaxed))
			continue;

		const std::atomic<uint16_t>* pBricks = m_pLevels[k_uiBrickLevel][uiBuffer].get();

		/* Coarse to fine within a cell, fine to coarse across levels: each
		   level is rebuilt from the one below it, so the bricks have to be
		   settled before level 2 is summed, and level 2 before level 3. */
		for (uint32_t uiLevel = k_uiBrickLevel; uiLevel < k_uiPyramidLevels; ++uiLevel)
		{
			std::atomic<uint64_t>* pDirty = m_pDirty[uiLevel][uiBuffer].get();

			if (pDirty == nullptr)
				continue;

			for (uint32_t uiWord = 0; uiWord < m_uiDirtyWords[uiLevel]; ++uiWord)
			{
				uint64_t uiBits = pDirty[uiWord].exchange(0, std::memory_order_relaxed);

				while (uiBits != 0)
				{
					const uint32_t uiBit = static_cast<uint32_t>(__builtin_ctzll(uiBits));
					const uint32_t uiCellID = (uiWord << k_uiWordShift) + uiBit;

					uiBits &= uiBits - 1;

					if (uiCellID >= m_uiLevelCells[uiLevel])
						continue;

					if (uiLevel == k_uiBrickLevel)
					{
						/* The bricks are already current - they are the level
						   the write paths maintain - so this only mirrors them
						   and rebuilds the fine cells under them. */
						if (pBricks != nullptr)
							WriteMirror(uiBuffer, uiLevel, uiCellID, pBricks[uiCellID].load(std::memory_order_relaxed));

						RebuildFineCells(uiBuffer, uiCellID);

						/* After the rebuild, not before: overflowing the
						   region cap turns the frame into a full upload, and
						   a full upload of a mirror that is already current
						   is the cheapest way to be right. */
						RecordDensityRegion(uiBuffer, uiCellID);
					}
					else
					{
						RebuildCoarseCell(uiBuffer, uiLevel, uiCellID);
					}

					if (uiLevel + 1 < k_uiPyramidLevels)
						MarkDirtyAt(uiBuffer, uiLevel + 1, ParentCellID(uiLevel, uiCellID));
				}
			}
		}
	}
}

uint32_t VoxelBrickGrid::Validate(bool bBack, const uint32_t* pVoxelData)
{
	if (m_uiBrickCount == 0 || pVoxelData == nullptr)
		return 0;

	/* Everything above the bricks is only as current as the last flush, so
	   validating without one would report the backlog rather than a defect. */
	FlushDirty();

	const uint32_t uiIndex = Index(bBack);

	/* One expected-count array per level, all accumulated in the same pass
	   over the voxels: the window is 75 M of them and reading it is the whole
	   cost here, so walking it once per level would be five times as slow for
	   no more information. */
	std::vector<uint16_t> expected[k_uiPyramidLevels];

	for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
		expected[uiLevel].assign(m_uiLevelCells[uiLevel], 0);

	uint32_t uiBitMismatches = 0;
	uint32_t uiFirstBadBit = UINT32_MAX;

	for (uint32_t uiZ = 0; uiZ < m_v3WorldSize.z; ++uiZ)
	{
		for (uint32_t uiY = 0; uiY < m_v3WorldSize.y; ++uiY)
		{
			const uint32_t uiRowBase = uiY * m_v3WorldSize.x + uiZ * m_v3WorldSize.x * m_v3WorldSize.y;

			uint32_t uiLevelRowBase[k_uiPyramidLevels];

			for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
			{
				const uint32_t uiShift = k_uiFineShift + uiLevel;
				const UVector3& v3Grid = m_v3LevelGrid[uiLevel];

				uiLevelRowBase[uiLevel] =
					(uiY >> uiShift) * v3Grid.x +
					(uiZ >> uiShift) * v3Grid.x * v3Grid.y;
			}

			for (uint32_t uiX = 0; uiX < m_v3WorldSize.x; ++uiX)
			{
				const bool bOccupied = (pVoxelData[uiRowBase + uiX] >> 24) != 0;

				if (bOccupied)
				{
					for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
						++expected[uiLevel][uiLevelRowBase[uiLevel] + (uiX >> (k_uiFineShift + uiLevel))];
				}

				/* The bitmap is what every write path now believes about this
				   voxel, so it is worth as much as the counts are - a wrong
				   bit produces a wrong count on the *next* write to it, which
				   is much harder to trace back here from. */
				if (IsOccupied(bBack, uiRowBase + uiX) != bOccupied)
				{
					if (uiFirstBadBit == UINT32_MAX)
						uiFirstBadBit = uiRowBase + uiX;

					++uiBitMismatches;
				}
			}
		}
	}

	if (uiBitMismatches > 0)
	{
		fprintf(stderr, "[bricks] occupancy bitmap disagrees with the voxel buffer for %u voxels, first at %u\n",
		        uiBitMismatches, uiFirstBadBit);
	}

	uint32_t uiMismatches = 0;
	uint32_t uiLost = 0;
	uint32_t uiCells = 0;

	for (uint32_t uiLevel = 0; uiLevel < k_uiPyramidLevels; ++uiLevel)
	{
		const std::atomic<uint16_t>* pCounts = m_pLevels[uiLevel][uiIndex].get();

		if (pCounts == nullptr)
			continue;

		uint32_t uiLevelMismatches = 0;

		for (uint32_t uiCell = 0; uiCell < m_uiLevelCells[uiLevel]; ++uiCell)
		{
			const uint16_t uiActual = pCounts[uiCell].load(std::memory_order_relaxed);

			if (uiActual == expected[uiLevel][uiCell])
				continue;

			++uiLevelMismatches;

			/* The two failure modes are not equally bad: a cell counted higher
			   than it is only costs traversal, one counted at zero when it
			   holds something deletes geometry from the image - and above the
			   brick level it deletes it from the ambient term instead, which
			   is a good deal harder to notice. */
			if (uiActual == 0 && expected[uiLevel][uiCell] != 0)
				++uiLost;

			if (uiLevelMismatches <= 4)
			{
				fprintf(stderr, "[bricks] level %u (%u^3) cell %u: counted %u, actually %u\n",
				        uiLevel, LevelSize(uiLevel), uiCell, uiActual, expected[uiLevel][uiCell]);
			}
		}

		uiMismatches += uiLevelMismatches;
		uiCells += m_uiLevelCells[uiLevel];
	}

	fprintf(stderr, "[bricks] validated %u pyramid cells over %u levels and %u voxels: %u disagree, "
	                "%u of them would lose geometry; %u occupancy bits disagree\n",
	        uiCells, k_uiPyramidLevels, m_v3WorldSize.x * m_v3WorldSize.y * m_v3WorldSize.z,
	        uiMismatches, uiLost, uiBitMismatches);

	return uiMismatches + uiBitMismatches;
}

uint64_t VoxelBrickGrid::Hash(bool bBack) const
{
	const uint32_t uiIndex = Index(bBack);

	uint64_t uiHash = 1469598103934665603ull;

	auto fold = [&uiHash](uint64_t uiValue)
	{
		for (uint32_t uiByte = 0; uiByte < 8; ++uiByte)
		{
			uiHash ^= (uiValue >> (uiByte * 8)) & 0xFFull;
			uiHash *= 1099511628211ull;
		}
	};

	fold(m_uiBrickCount);
	fold(m_uiVoxelCount);

	/* The bricks and the bitmap only. Every other level of the pyramid is
	   *derived* from those two by FlushDirty - rebuilt wholesale rather than
	   accumulated - so it carries no state they do not, and folding it in
	   would only make a determinism hash depend on when the last flush
	   happened. */
	if (m_pLevels[k_uiBrickLevel][uiIndex] != nullptr)
	{
		for (uint32_t uiCell = 0; uiCell < m_uiBrickCount; ++uiCell)
			fold(m_pLevels[k_uiBrickLevel][uiIndex][uiCell].load(std::memory_order_relaxed));
	}

	if (m_pOccupancy[uiIndex] != nullptr)
	{
		for (uint32_t uiWord = 0; uiWord < m_uiWordCount; ++uiWord)
			fold(m_pOccupancy[uiIndex][uiWord].load(std::memory_order_relaxed));
	}

	return uiHash;
}

void VoxelBrickGrid::ReportUnderflow(uint32_t uiLevel, uint32_t uiCellID)
{
	static bool s_bWarned = false;

	if (s_bWarned)
		return;

	s_bWarned = true;

	fprintf(stderr, "[bricks] occupancy count for level %u cell %u went below zero - a voxel was cleared that "
	                "the grid never counted as occupied\n", uiLevel, uiCellID);
}

void VoxelBrickGrid::ReportUnalignedRegion(uint32_t uiLevel, const UVector3& v3Min, const UVector3& v3Size)
{
	static bool s_bWarned = false;

	if (s_bWarned)
		return;

	s_bWarned = true;

	fprintf(stderr, "[bricks] region (%u %u %u)+(%u %u %u) is not a whole number of %u-voxel cells (level %u); "
	                "edge cells are marked fully occupied\n",
	        v3Min.x, v3Min.y, v3Min.z, v3Size.x, v3Size.y, v3Size.z, LevelSize(uiLevel), uiLevel);
}

/* --- Coverage pyramid invariants (RENDERING_PLAN.md 7.1b) ------------------ */

/* The bricks have to *be* a level, not a sixth structure alongside the five:
   everything phase 2 built - the marcher's traversal, the dirty set, the
   particle landing test - reads them through this class. */
static_assert(VoxelBrickGrid::k_uiBrickShift >= VoxelBrickGrid::k_uiFineShift,
              "the pyramid's finest level must be at least as fine as a brick");
static_assert(VoxelBrickGrid::k_uiBrickLevel < VoxelBrickGrid::k_uiPyramidLevels,
              "the brick level has to be one of the levels that exists");
static_assert(VoxelBrickGrid::LevelSize(VoxelBrickGrid::k_uiBrickLevel) == VoxelBrickGrid::k_uiBrickSize,
              "k_uiBrickLevel does not name the level whose cell is a brick");

/* Counts are uint16 at every level, so the coarsest cell may not hold more
   voxels than that. At k_uiFineShift 1 and five levels the coarsest is 32^3 =
   32768, which leaves exactly one doubling of headroom. */
static_assert(VoxelBrickGrid::LevelVolume(VoxelBrickGrid::k_uiPyramidLevels - 1) <= UINT16_MAX,
              "the coarsest pyramid level overflows a uint16 count");
