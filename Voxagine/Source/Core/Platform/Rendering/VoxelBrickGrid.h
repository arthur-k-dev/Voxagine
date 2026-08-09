#pragma once

#include "Core/Math.h"
#include "Core/Platform/Rendering/RenderDefines.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

/* Coarse occupancy over the resident voxel window: one count of occupied
 * voxels per 8^3 brick, so the marcher can skip empty space structurally
 * instead of stepping through it. RENDERING_PLAN.md phase 2.
 *
 * A *count*, not a bit, because destruction has to be able to un-set a brick.
 * A bit can be set from a single voxel write but clearing it correctly needs a
 * rescan of the other 511; a count just decrements. 8^3 = 512 fits a uint16.
 *
 * Linearization is the voxel convention (rule 4) scaled down:
 *   brick = x + y*B.x + z*B.x*B.y,  B = ceil(worldSize / k_uiBrickSize)
 * HLSL's PosToBrickID/GetBrickGridSize in SDFMarcher.hlsl must agree.
 *
 * Two representations are kept deliberately:
 *
 *   - m_pLevels, plain CPU memory, is authoritative and is what every
 *     read-modify-write goes through.
 *   - m_pGPU points into the brick Mapper's host-visible buffers and is
 *     written to, never read from.
 *
 * That split is the point. The mapped buffers are HOST_VISIBLE and uncached
 * (and DEVICE_LOCAL too where ReBAR allows it), so `++count[brick]` performed
 * directly on the mapping would be an uncached read per voxel write. Keeping
 * the counts in ordinary memory makes every update a cached RMW plus one
 * streaming store.
 *
 * Front and back mirror the voxel Mapper's two buffers and must be swapped in
 * lockstep with it - a brick grid built against the pre-swap window describes
 * the wrong voxels. Each buffer's counts describe only that buffer's voxels,
 * so the two never have to agree.
 *
 * There is a third representation, for the same reason as the second:
 * m_pOccupancy, one *bit* per voxel of the window, in ordinary CPU memory.
 * A count cannot be maintained without knowing whether a write changes
 * occupancy, and the only other place that answer lives is the voxel word
 * itself - in the mapping. Reading it back cost a PCIe read of VRAM per baked
 * voxel and 74 ms of every chunk load (RENDERING_PLAN.md phase 5). The bitmap
 * answers it out of cache and makes every voxel write path write-only with
 * respect to the mapping. 768x128x768 bits is 9.4 MiB a buffer.
 *
 * There is a fourth, and it is the only one the GPU filters: m_pDensity, the
 * finest pyramid level as one RGBA texel a cell, staged for the 3D texture the
 * ambient-occlusion and diffuse cones sample (RENDERING_PLAN.md 7.1b route B
 * and 7.3). Alpha is the occupied fraction; RGB is the cell's albedo, linear
 * and premultiplied by that fraction. See SetDensityBuffers.
 *
 * And a fifth, which exists only to feed the fourth: m_pColor, one packed
 * linear albedo per *brick*. A cone gathering bounce light needs to know what
 * colour the matter it hit is, and neither the counts nor the bitmap carry
 * that. Per brick rather than per level-0 cell because a diffuse bounce does
 * not resolve four voxels - it is 9.4 MiB against the 75 MiB the fine version
 * would have cost, for a difference no cone can see.
 *
 * It is exact where the counts are allowed to be conservative: EndRegion may
 * mark a straddling brick fully occupied, but a bit that claims a voxel is
 * occupied when it is not turns the next clear of that voxel into a spurious
 * decrement. Region clears therefore cover the exact voxel region, not the
 * outward-rounded brick one.
 */
class VoxelBrickGrid
{
public:
	/* constexpr, not const: anything that takes one of these by reference - a
	   test assertion, vector::assign, unordered_map::emplace - odr-uses it and
	   needs a definition. Release inlines every use and links; Debug does not.
	   This tree has shipped that difference twice (see CLAUDE.md on the owner
	   slot constants); constexpr is implicitly inline in C++17 and ends it. */
	static constexpr uint32_t k_uiBrickShift = 2;
	static constexpr uint32_t k_uiBrickSize = 1u << k_uiBrickShift;
	static constexpr uint32_t k_uiBrickVolume = k_uiBrickSize * k_uiBrickSize * k_uiBrickSize;

	/* --- Coverage pyramid (RENDERING_PLAN.md 7.1b) -------------------------
	   Level 0 is the brick grid above, at k_uiBrickSize voxels a cell. Each
	   level after it doubles the cell, so level L covers
	   `k_uiBrickSize << L` voxels and the whole set is a mip chain over
	   occupancy.

	   Why there is a level *finer* than the brick is the finding this exists
	   for. A cone traced against the 4-voxel bricks alone cannot serve ambient
	   occlusion: started nearer than ~8 voxels it samples the bricks holding
	   its own wall and the surface occludes itself; started at 8, nothing
	   measures occlusion between one voxel and eight, which is exactly the
	   scale of the gaps between stones in a wall. Both were seen on screen.
	   The finest level therefore has to be smaller than the brick, and
	   k_uiFineShift is where that lives.

	   Only the brick level reaches the GPU as counts, in the buffer phase 2
	   already binds and starting at element zero. Route A appended every level
	   into that same buffer; route B replaced the levels with a 3D texture the
	   hardware filters, so the buffer went back to what the marcher actually
	   reads - one exact count at a time - and the 76.8 MiB of mirror the other
	   levels occupied went with it. The counts here are still kept for every
	   level: they are what the density mirror and Validate are computed from,
	   and they cost ordinary cached memory rather than a mapping. */
	static constexpr uint32_t k_uiFineShift = 1;
	static constexpr uint32_t k_uiPyramidLevels = 5;

	/* The level whose cells are the bricks. Everything in this class that says
	   "brick" - GetCount, VoxelToBrick, the dirty set VoxelEditBatch reports,
	   the marcher's own traversal - means this one; the pyramid is the same
	   structure with levels either side of it. */
	static constexpr uint32_t k_uiBrickLevel = k_uiBrickShift - k_uiFineShift;

	/* Cell edge in voxels for a pyramid level, level 0 being the finest. */
	static constexpr uint32_t LevelSize(uint32_t uiLevel)
	{
		return 1u << (k_uiFineShift + uiLevel);
	}

	static constexpr uint32_t LevelVolume(uint32_t uiLevel)
	{
		return LevelSize(uiLevel) * LevelSize(uiLevel) * LevelSize(uiLevel);
	}

	/* A voxel word (R, G, B, rendererState + 1 from the low byte up) to the low
	   three bytes of a linear albedo, alpha dropped. The table is the exact sRGB
	   transfer function, the same curve Color.hlsl applies to everything else -
	   see SetDensityBuffers for why the decode happens here rather than in the
	   cone. Public so that a test can state the expected texel in terms of it
	   rather than restating the curve. */
	static inline uint32_t PackLinearAlbedo(uint32_t uiColor)
	{
		return uint32_t(k_LinearFromSrgb[uiColor & 0xFFu])
			| (uint32_t(k_LinearFromSrgb[(uiColor >> 8) & 0xFFu]) << 8)
			| (uint32_t(k_LinearFromSrgb[(uiColor >> 16) & 0xFFu]) << 16);
	}

	/* Cells across each axis at a level, and how many in total. Ceil-div, so a
	   window that is not a whole number of cells gets a partial one rather than
	   losing its edge - HLSL's own GetBrickGridSize does the same. */
	const UVector3& GetLevelGridSize(uint32_t uiLevel) const { return m_v3LevelGrid[uiLevel]; }
	uint32_t GetLevelCellCount(uint32_t uiLevel) const { return m_uiLevelCells[uiLevel]; }

	/* Reallocates for a new window size and zeroes every count. Touches CPU
	   state only and drops the mirror pointers - the caller resizes the brick
	   Mapper to GetBrickCount() elements afterwards, hands the new mappings
	   back with SetBuffers, and calls Flush. */
	void Resize(const UVector3& v3WorldSize);

	/* The two host-visible mirrors, in the same front/back sense the voxel
	   Mapper is in right now. Re-supply these after any mapper resize. */
	void SetBuffers(uint32_t* pFront, uint32_t* pBack);

	/* --- The pyramid's GPU form (RENDERING_PLAN.md 7.1b route B) -----------
	   Route A kept every level as counts in the buffer the bricks bind and
	   filtered a sample with eight fetches by hand; that cost 4.6x the rest of
	   the cone. The levels are a mip chain over the window, so they belong in
	   a 3D texture where one SampleLevel is the whole filter.

	   What crosses to the GPU is therefore a *density* - the occupied fraction
	   of a cell, 0..1 as a unorm byte - rather than a count, and only level 0
	   of it. Everything coarser is a halving of level 0 and is blitted from it
	   on the GPU. Two mirrors for the same reason the counts have two: the
	   window's front and back buffers describe different voxels.

	   **RGBA rather than R as of 7.3**, packed as the texture's own byte order
	   (R, G, B, A from the low byte up): alpha is the density above, and RGB is
	   the cell's albedo in *linear* light, premultiplied by that density.

	   Both halves of that are load-bearing. Premultiplied, because the coarser
	   levels are a box average of their eight children and an average of
	   premultiplied radiance is exactly the right answer where an average of
	   raw colours would let one lit voxel in an empty cell speak for all eight.
	   Linear, because the alternative is a pow per cone sample and there are
	   thirty-five of those a pixel; the cost is that 8-bit linear quantizes the
	   darks coarsely, which a bounce term does not resolve.

	   Null for a grid with no texture behind it - the far field, and every
	   headless test - and every path here checks. */
	void SetDensityBuffers(uint32_t* pFront, uint32_t* pBack);

	/* Cells the level-0 density mirror has to hold, i.e. the texel count of
	   the texture's mip 0. */
	uint32_t GetFineCellCount() const { return m_uiLevelCells[0]; }
	const UVector3& GetFineGridSize() const { return m_v3LevelGrid[0]; }

	/* What of the front mirror the texture does not yet hold, as boxes of
	   level-0 cells, and clears the record. Returns true when the answer is
	   "all of it" - a resize, a clear, or a buffer swap, since the texture
	   only ever held the other buffer's contents - in which case the vector is
	   emptied and the caller uploads the whole level.

	   Also returns true when the boxes outnumber k_uiMaxDensityRegions: a
	   world load dirties every brick in the window, and a million copy regions
	   costs more to issue than the 9.4 MiB they would have saved. */
	static constexpr uint32_t k_uiMaxDensityRegions = 4096;
	bool TakeDensityRegions(std::vector<ImageRegion>& regions);

	/* Whether the mirror is ahead of anything the texture can hold yet. Only
	   an audit needs this: comparing the two while an upload is outstanding
	   reports the backlog rather than a defect. */
	bool HasPendingDensityRegions() const
	{
		return m_bDensityFull[m_uiFront] || !m_DensityRegions.empty();
	}

	/* Mirrors Mapper::SwapBuffer. */
	void Swap();

	/* Writes every count to its mirror. Only needed after a resize; the update
	   paths keep the mirrors current themselves. */
	void Flush();

	/* Rebuilds the pyramid over everything written since the last call, and
	   pushes it to the mirror. Main thread, once a frame, before the GPU reads
	   the buffer - RenderContext::Present.

	   Deferred rather than maintained per voxel because the per-voxel version
	   was measured and is the wrong shape: a world load stamps ~3.1 M voxels,
	   and five counter updates plus five scattered stores into uncached
	   host-visible memory took CPU VoxelBaker::Occupy (added) from 1.86 ms to
	   7.20 ms. Most of that work is redundant - a 32^3 cell is written by up to
	   32768 voxels of the same burst - and deferring is what collapses it to
	   one rebuild per cell that actually changed.

	   Bricks are exempt and stay incremental: gameplay reads them through
	   GetCount within the frame that wrote them. */
	void FlushDirty();

	/* Zeroes both buffers, CPU side and mirrors. */
	void ClearAll();

	const UVector3& GetWorldSize() const { return m_v3WorldSize; }
	const UVector3& GetGridSize() const { return m_v3GridSize; }
	uint32_t GetBrickCount() const { return m_uiBrickCount; }

	uint32_t GetCount(bool bBack, uint32_t uiBrickID) const
	{
		return GetLevelCount(bBack, k_uiBrickLevel, uiBrickID);
	}

	uint32_t GetLevelCount(bool bBack, uint32_t uiLevel, uint32_t uiCellID) const
	{
		const std::atomic<uint16_t>* pCounts = m_pLevels[uiLevel][Index(bBack)].get();

		return (pCounts != nullptr && uiCellID < m_uiLevelCells[uiLevel])
			? pCounts[uiCellID].load(std::memory_order_relaxed)
			: 0;
	}

	/* Linear index of the cell a voxel coordinate falls in, at a level. Same
	   convention as BrickID, which is this at k_uiBrickLevel. */
	inline uint32_t LevelCellID(uint32_t uiLevel, uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
	{
		const UVector3& v3Grid = m_v3LevelGrid[uiLevel];
		const uint32_t uiShift = k_uiFineShift + uiLevel;

		return (uiX >> uiShift)
			+ (uiY >> uiShift) * v3Grid.x
			+ (uiZ >> uiShift) * v3Grid.x * v3Grid.y;
	}

	inline uint32_t VoxelToBrick(uint32_t uiVoxelID) const
	{
		if (m_uiBrickCount == 0)
			return UINT32_MAX;

		const uint32_t uiX = uiVoxelID % m_v3WorldSize.x;
		const uint32_t uiRest = uiVoxelID / m_v3WorldSize.x;
		const uint32_t uiY = uiRest % m_v3WorldSize.y;
		const uint32_t uiZ = uiRest / m_v3WorldSize.y;

		return BrickID(uiX >> k_uiBrickShift, uiY >> k_uiBrickShift, uiZ >> k_uiBrickShift);
	}

	/* Was this voxel occupied? The question every write path has to answer to
	   maintain a count, answered from cache instead of from the mapping. */
	inline bool IsOccupied(uint32_t uiVoxelID) const
	{
		if (uiVoxelID >= m_uiVoxelCount || m_pOccupancy[m_uiFront] == nullptr)
			return false;

		const uint64_t uiWord = m_pOccupancy[m_uiFront][uiVoxelID >> k_uiWordShift]
			.load(std::memory_order_relaxed);

		return ((uiWord >> (uiVoxelID & k_uiWordMask)) & 1ull) != 0ull;
	}

	/* Point update for the front buffer, from RenderContext::ModifyVoxel and
	   friends. Only occupancy transitions move the count - overwriting an
	   occupied voxel with a different colour must not increment (rule 3:
	   occupancy is alpha > 0, and alpha carries rendererState + 1, so it is
	   never simply 1).

	   The old occupancy is read here rather than passed in, so the bitmap and
	   the counts are updated from the same value and cannot drift apart.

	   **Takes the colour rather than a bool as of 7.3.** Occupancy is still
	   alpha > 0 and is derived here, for the same reason the old occupancy is;
	   what the colour buys is the brick albedo the bounce cones read. Every
	   caller already had the word in hand. */
	/* Deleted rather than left to the implicit conversion. This took a bool
	   until 7.3, every caller spelled it `(uiColor >> 24) != 0`, and a call site
	   that kept doing so would compile - passing 1 as the colour, whose alpha
	   byte is zero, so *every voxel would read as empty* and nothing would say
	   so. Two call sites did exactly that. */
	void SetVoxel(uint32_t uiVoxelID, bool bIsOccupied) = delete;

	inline void SetVoxel(uint32_t uiVoxelID, uint32_t uiColor)
	{
		const bool bIsOccupied = (uiColor >> 24) != 0;

		if (uiVoxelID >= m_uiVoxelCount || IsOccupied(uiVoxelID) == bIsOccupied)
			return;

		SetOccupancyBit(m_uiFront, uiVoxelID, bIsOccupied);

		const uint32_t uiBrickID = VoxelToBrick(uiVoxelID);

		if (uiBrickID >= m_uiBrickCount)
			return;

		/* On the transition into occupied only, so that a re-stamp of a voxel
		   that was already there costs nothing. The brick then carries the
		   colour of the last voxel to *appear* in it, which is what a 4-voxel
		   cell of a bounce field can represent anyway. */
		if (bIsOccupied)
			SetBrickColor(m_uiFront, uiBrickID, uiColor);

		std::atomic<uint16_t>& count = m_pLevels[k_uiBrickLevel][m_uiFront][uiBrickID];

		if (bIsOccupied)
		{
			count.fetch_add(1, std::memory_order_relaxed);
		}
		else if (count.fetch_sub(1, std::memory_order_relaxed) == 0)
		{
			/* Underflow means a decrement arrived for a voxel this grid never
			   counted as occupied. Wrapping to 65535 would only cost traversal,
			   but it would also hide the accounting bug that produced it, so
			   put it back and say so. */
			count.fetch_add(1, std::memory_order_relaxed);
			ReportUnderflow(k_uiBrickLevel, uiBrickID);
			return;
		}

		MarkDirty(m_uiFront, uiBrickID);
	}

	/* Bulk path, for callers that overwrite a whole rectangular region of the
	   window in one pass (ChunkSystem::RenderChunk). Old occupancy is never
	   consulted - the region is zeroed, re-accumulated from the source data
	   the caller is writing, and flushed:

	       BeginRegion(...);
	       for each voxel written:  if occupied -> AddVoxel(...)
	       EndRegion(...);

	   Regions that are not brick-aligned leave bricks straddling the edge only
	   partly accounted for; EndRegion marks those fully occupied rather than
	   under-counting them, since an over-counted brick costs traversal while an
	   under-counted one loses geometry. */
	void BeginRegion(bool bBack, const UVector3& v3Min, const UVector3& v3Size);
	void EndRegion(bool bBack, const UVector3& v3Min, const UVector3& v3Size);

	inline void AddVoxel(bool bBack, uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
	{
		const uint32_t uiBuffer = Index(bBack);

		SetOccupancyBit(uiBuffer, VoxelID(uiX, uiY, uiZ), true);

		const uint32_t uiBrickID = BrickID(uiX >> k_uiBrickShift, uiY >> k_uiBrickShift, uiZ >> k_uiBrickShift);

		if (uiBrickID >= m_uiBrickCount)
			return;

		SetBrickColor(uiBuffer, uiBrickID, uiColor);

		/* Non-atomic on purpose: a region is owned by exactly one thread for
		   the length of a Begin/End pair, and the two buffers are disjoint.
		   EndRegion marks the whole region dirty, so this does not have to. */
		std::atomic<uint16_t>& count = m_pLevels[k_uiBrickLevel][uiBuffer][uiBrickID];
		count.store(static_cast<uint16_t>(count.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
	}

	/* Recomputes every count from the voxel buffer and reports disagreements.
	   Reads the whole window out of uncached host-visible memory, so it is a
	   debugging tool, not something to run per frame. Returns the number of
	   cells that disagreed.

	   Flushes first, since the pyramid above the bricks is only as current as
	   the last FlushDirty and validating a knowingly stale copy would report
	   noise. */
	uint32_t Validate(bool bBack, const uint32_t* pVoxelData);

	/* Order-independent-free FNV-1a over every count and every occupancy word,
	   for DESTRUCTION_PLAN.md phase 0's gauntlet state hash. Two runs of the
	   same script must produce the same value; a refactor that changes it has
	   changed what the grid holds, which is the whole point of having it.
	   Reads only CPU state, so it costs no PCIe traffic. */
	uint64_t Hash(bool bBack) const;

private:
	static constexpr uint32_t k_uiWordShift = 6;
	static constexpr uint32_t k_uiWordMask = (1u << k_uiWordShift) - 1u;

	uint32_t Index(bool bBack) const { return bBack ? (m_uiFront ^ 1u) : m_uiFront; }

	inline uint32_t BrickID(uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
	{
		return uiX + uiY * m_v3GridSize.x + uiZ * m_v3GridSize.x * m_v3GridSize.y;
	}

	/* Marks a brick as needing its share of the pyramid rebuilt. One cached
	   atomic OR over a bitmap of 1.2 M bits, in place of the four extra
	   counter updates and four extra streaming stores a per-voxel pyramid
	   would cost - which is what made the per-voxel version 3.9x the write
	   cost of the bricks alone. See FlushDirty. */
	inline void MarkDirty(uint32_t uiBuffer, uint32_t uiBrickID)
	{
		std::atomic<uint64_t>* pDirty = m_pDirty[k_uiBrickLevel][uiBuffer].get();

		if (pDirty == nullptr)
			return;

		pDirty[uiBrickID >> k_uiWordShift].fetch_or(1ull << (uiBrickID & k_uiWordMask), std::memory_order_relaxed);
		m_bHasDirty[uiBuffer].store(true, std::memory_order_relaxed);
	}

	/* Rebuilds one brick's eight level-0 cells from the occupancy bitmap, and
	   pushes them plus the brick itself to the mirror. The bitmap is the only
	   representation fine enough to answer this and it is ordinary cached
	   memory, so a 4^3 brick is sixteen nibble reads - one per (y, z) row -
	   rather than 64 separate lookups. */
	void RebuildFineCells(uint32_t uiBuffer, uint32_t uiBrickID);

	/* Rebuilds one coarse cell as the sum of its eight children at the level
	   below, and pushes it. Exact rather than approximate: a cell's count is
	   the number of occupied voxels under it however it is arrived at. */
	void RebuildCoarseCell(uint32_t uiBuffer, uint32_t uiLevel, uint32_t uiCellID);

	inline void MarkDirtyAt(uint32_t uiBuffer, uint32_t uiLevel, uint32_t uiCellID)
	{
		std::atomic<uint64_t>* pDirty = m_pDirty[uiLevel][uiBuffer].get();

		if (pDirty != nullptr)
			pDirty[uiCellID >> k_uiWordShift].fetch_or(1ull << (uiCellID & k_uiWordMask), std::memory_order_relaxed);
	}

	/* The cell one level coarser that contains this one. Not simply
	   uiCellID >> 3: the grids are independent ceil-divs, so a level's row
	   pitch is not twice its parent's wherever the window is not a whole
	   number of cells. */
	inline uint32_t ParentCellID(uint32_t uiLevel, uint32_t uiCellID) const
	{
		const UVector3& v3Grid = m_v3LevelGrid[uiLevel];
		const UVector3& v3Parent = m_v3LevelGrid[uiLevel + 1];

		const uint32_t uiX = uiCellID % v3Grid.x;
		const uint32_t uiY = (uiCellID / v3Grid.x) % v3Grid.y;
		const uint32_t uiZ = uiCellID / (v3Grid.x * v3Grid.y);

		return (uiX >> 1) + (uiY >> 1) * v3Parent.x + (uiZ >> 1) * v3Parent.x * v3Parent.y;
	}

	inline void SetBrickColor(uint32_t uiBuffer, uint32_t uiBrickID, uint32_t uiColor)
	{
		std::atomic<uint32_t>* pColor = m_pColor[uiBuffer].get();

		if (pColor == nullptr)
			return;

		/* Relaxed and last-writer-wins. Two threads stamping into the same brick
		   disagree about its colour and either answer is one of the colours
		   actually there, which is the whole accuracy this level claims. */
		pColor[uiBrickID].store(PackLinearAlbedo(uiColor), std::memory_order_relaxed);
	}

	inline void WriteMirror(uint32_t uiBuffer, uint32_t uiLevel, uint32_t uiCellID, uint16_t uiValue)
	{
		/* Bricks only. Everything the marcher reads is this level and it reads
		   one cell at a time; everything the cones read is the texture, and a
		   count nobody reads still costs a streaming store into uncached
		   host-visible memory on every write - which is the cost route A
		   measured when it maintained five levels in this mirror. */
		if (uiLevel == k_uiBrickLevel && m_pGPU[uiBuffer] != nullptr)
			m_pGPU[uiBuffer][uiCellID] = uiValue;
	}

	/* Level 0's texel, which is what the texture holds and the only level that
	   is uploaded. Alpha is the density: a level-0 cell is LevelVolume(0)
	   voxels, so the count and the byte are exactly inter-convertible and the
	   quantization is free. RGB is the containing brick's linear albedo scaled
	   by that density - premultiplied, so the blit chain that averages eight
	   children stays exact. See SetDensityBuffers.

	   Split out of WriteMirror because only RebuildFineCells reaches level 0,
	   and it is the only caller that knows which brick a cell belongs to
	   without recovering it from the cell id. */
	inline void WriteDensityMirror(uint32_t uiBuffer, uint32_t uiCellID, uint16_t uiValue, uint32_t uiAlbedo)
	{
		if (m_pDensity[uiBuffer] == nullptr)
			return;

		const uint32_t uiDensity =
			(static_cast<uint32_t>(uiValue) * 255u + k_uiFineVolume / 2u) / k_uiFineVolume;

		const uint32_t uiRed = ((uiAlbedo & 0xFFu) * uiDensity + 127u) / 255u;
		const uint32_t uiGreen = (((uiAlbedo >> 8) & 0xFFu) * uiDensity + 127u) / 255u;
		const uint32_t uiBlue = (((uiAlbedo >> 16) & 0xFFu) * uiDensity + 127u) / 255u;

		m_pDensity[uiBuffer][uiCellID] = uiRed | (uiGreen << 8) | (uiBlue << 16) | (uiDensity << 24);
	}

	/* Records that a brick's eight level-0 cells have to reach the texture.
	   Only the front buffer is tracked: the back one is wholesale-rewritten by
	   chunk streaming and becomes a full upload the moment it is swapped in,
	   so listing its boxes would be work thrown away.

	   Bricks arrive here in ascending id order - FlushDirty walks the dirty
	   bitmap word by word and bit by bit - so a run along x is recognised by
	   comparing against the last one, and a burst of a thousand bricks becomes
	   a hundred boxes rather than a thousand. */
	void RecordDensityRegion(uint32_t uiBuffer, uint32_t uiBrickID);

	inline uint32_t VoxelID(uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
	{
		return uiX + uiY * m_v3WorldSize.x + uiZ * m_v3WorldSize.x * m_v3WorldSize.y;
	}

	inline void SetOccupancyBit(uint32_t uiBuffer, uint32_t uiVoxelID, bool bOccupied)
	{
		if (uiVoxelID >= m_uiVoxelCount || m_pOccupancy[uiBuffer] == nullptr)
			return;

		std::atomic<uint64_t>& word = m_pOccupancy[uiBuffer][uiVoxelID >> k_uiWordShift];
		const uint64_t uiBit = 1ull << (uiVoxelID & k_uiWordMask);

		/* Atomic for the same reason the counts are: voxel writes reach here
		   from job threads as well as the main one, and a word covers 64
		   neighbouring voxels, so a plain read-modify-write would lose a
		   neighbour's bit rather than merely race on its own. */
		if (bOccupied)
			word.fetch_or(uiBit, std::memory_order_relaxed);
		else
			word.fetch_and(~uiBit, std::memory_order_relaxed);
	}

	bool IsOccupied(bool bBack, uint32_t uiVoxelID) const;

	/* Clears the bits of an exact voxel region - see the class comment on why
	   this is not rounded out to bricks the way the counts are. */
	void ClearOccupancyRegion(uint32_t uiBuffer, const UVector3& v3Min, const UVector3& v3Size);

	/* First and last cell of a level touched by a voxel region, inclusive and
	   outward-rounded. Clamped to the grid, since a region may run to the edge
	   of a window that is not a whole number of cells. */
	inline UVector3 LevelCellRangeFirst(uint32_t uiLevel, const UVector3& v3Min) const
	{
		const uint32_t uiShift = k_uiFineShift + uiLevel;

		return UVector3(v3Min.x >> uiShift, v3Min.y >> uiShift, v3Min.z >> uiShift);
	}

	inline UVector3 LevelCellRangeLast(uint32_t uiLevel, const UVector3& v3Min, const UVector3& v3Size) const
	{
		const uint32_t uiShift = k_uiFineShift + uiLevel;
		const UVector3& v3Grid = m_v3LevelGrid[uiLevel];

		return UVector3(
			std::min((v3Min.x + v3Size.x - 1) >> uiShift, v3Grid.x - 1),
			std::min((v3Min.y + v3Size.y - 1) >> uiShift, v3Grid.y - 1),
			std::min((v3Min.z + v3Size.z - 1) >> uiShift, v3Grid.z - 1)
		);
	}

	void ZeroCounts();
	void ZeroColors();
	void ZeroOccupancy();
	void ClearDirty();

	static void ReportUnderflow(uint32_t uiLevel, uint32_t uiCellID);
	static void ReportUnalignedRegion(uint32_t uiLevel, const UVector3& v3Min, const UVector3& v3Size);

	UVector3 m_v3WorldSize = UVector3(0, 0, 0);
	UVector3 m_v3GridSize = UVector3(0, 0, 0);
	uint32_t m_uiBrickCount = 0;
	uint32_t m_uiVoxelCount = 0;
	uint32_t m_uiWordCount = 0;

	uint32_t m_uiFront = 0;

	/* Cached rather than recomputed: LevelCellID runs per voxel per level on
	   the stamp path. The values are still derived by the ceil-div the shader
	   does, in Resize, so the two sides cannot disagree. */
	UVector3 m_v3LevelGrid[k_uiPyramidLevels];
	uint32_t m_uiLevelCells[k_uiPyramidLevels] = {};

	/* One count array per level per buffer. m_pLevels[k_uiBrickLevel] is the
	   brick grid phase 2 built; the rest are the pyramid around it. uint16 for
	   every level because the coarsest cell is 32^3 = 32768 voxels, which
	   still fits - a per-level type would save 9.4 MiB a buffer at level 0 and
	   cost every path here a branch on the level. */
	std::unique_ptr<std::atomic<uint16_t>[]> m_pLevels[k_uiPyramidLevels][2];

	/* One bit per cell, for the brick level and every level above it: what
	   FlushDirty has to rebuild. Level 0 needs none - it is rebuilt from the
	   brick that contains it. 170 KiB a buffer against the 43 MiB of counts,
	   which is why marking is cheap enough to do per voxel write. */
	std::unique_ptr<std::atomic<uint64_t>[]> m_pDirty[k_uiPyramidLevels][2];
	uint32_t m_uiDirtyWords[k_uiPyramidLevels] = {};
	std::atomic<bool> m_bHasDirty[2] = {};

	std::unique_ptr<std::atomic<uint64_t>[]> m_pOccupancy[2];
	uint32_t* m_pGPU[2] = { nullptr, nullptr };

	/* Level 0 as RGBA texels - linear premultiplied albedo and a density - one
	   mirror a buffer: the staging side of the coverage texture. See
	   SetDensityBuffers. */
	uint32_t* m_pDensity[2] = { nullptr, nullptr };

	/* One packed linear albedo per brick per buffer, the source of the RGB
	   above. Brick resolution rather than level 0's, which is 9.4 MiB instead
	   of 75 - see the class comment. */
	std::unique_ptr<std::atomic<uint32_t>[]> m_pColor[2];

	/* sRGB byte to linear byte, the exact piecewise curve. A class member and
	   not a function-local static so that the per-voxel path has no
	   thread-safe-initialization guard to test. */
	static const std::array<uint8_t, 256> k_LinearFromSrgb;

	/* Voxels in a level-0 cell, i.e. what a density byte is a fraction of.
	   Spelled out rather than LevelVolume(0) because a constexpr member
	   function is not complete inside the class that defines it; the
	   static_assert in the .cpp holds the two together. */
	static constexpr uint32_t k_uiFineVolume = 1u << (3u * k_uiFineShift);

	std::vector<ImageRegion> m_DensityRegions;
	uint32_t m_uiLastDensityBrick = UINT32_MAX;
	bool m_bDensityFull[2] = { true, true };
};
