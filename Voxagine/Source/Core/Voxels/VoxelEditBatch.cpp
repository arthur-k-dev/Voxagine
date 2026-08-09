#include "pch.h"

#include "Core/Voxels/VoxelEditBatch.h"

#include <algorithm>
#include <cmath>

#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/Platform/Rendering/VoxelBrickGrid.h"

bool VoxelEditTarget::IsValid() const
{
	return pGrid != nullptr && pBricks != nullptr && pWords != nullptr &&
		v3WindowSize.x > 0 && v3WindowSize.y > 0 && v3WindowSize.z > 0;
}

VoxelEditBatch::VoxelEditBatch(const VoxelEditTarget& target) :
	m_Target(target)
{
}

bool VoxelEditBatch::Resolve(const Vector3& v3GridPosition, uint32_t& o_uiX, uint32_t& o_uiY, uint32_t& o_uiZ)
{
	if (!std::isfinite(v3GridPosition.x) ||
		!std::isfinite(v3GridPosition.y) ||
		!std::isfinite(v3GridPosition.z))
	{
		++m_uiNonFinite;
		++m_uiRejected;
		return false;
	}

	/* Tested before the cast, not after. Casting a negative float to uint32_t
	   is what turns "one voxel left of the window" into "four billion voxels
	   right of it", and the bounds test above it then passes. */
	if (v3GridPosition.x < 0.f || v3GridPosition.y < 0.f || v3GridPosition.z < 0.f)
	{
		++m_uiRejected;
		return false;
	}

	o_uiX = static_cast<uint32_t>(v3GridPosition.x);
	o_uiY = static_cast<uint32_t>(v3GridPosition.y);
	o_uiZ = static_cast<uint32_t>(v3GridPosition.z);

	if (o_uiX >= m_Target.v3WindowSize.x ||
		o_uiY >= m_Target.v3WindowSize.y ||
		o_uiZ >= m_Target.v3WindowSize.z)
	{
		++m_uiRejected;
		return false;
	}

	return true;
}

bool VoxelEditBatch::Write(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiOwnerSlot)
{
	/* The CPU voxel first, because it is the one that can decline. A chunk that
	   is not resident has no cell, and the correct answer then is to write
	   nothing at all rather than to update the mapping for a voxel the grid
	   does not have - that is precisely the disagreement VOXAGINE_SYNC_AUDIT
	   looks for. */
	const VoxelCell cell = m_Target.pGrid->GetCell(uiX, uiY, uiZ);

	if (!cell)
	{
		++m_uiRejected;
		return false;
	}

	cell.SetColor(uiColor);
	cell.SetSlot(uiOwnerSlot);

	const uint32_t uiVoxelID =
		uiX +
		uiY * m_Target.v3WindowSize.x +
		uiZ * m_Target.v3WindowSize.x * m_Target.v3WindowSize.y;

	if (uiVoxelID < m_Target.uiWordCount)
		m_Target.pWords[uiVoxelID] = uiColor;

	/* SetVoxel reads the old occupancy from the bitmap itself, so the count and
	   the bit cannot drift apart, and nothing here reads the mapping. */
	const uint32_t uiBrick = m_Target.pBricks->VoxelToBrick(uiVoxelID);

	m_Target.pBricks->SetVoxel(uiVoxelID, (uiColor >> 24) != 0);

	if (uiBrick != UINT32_MAX)
	{
		m_DirtyBricks.push_back(uiBrick);
		m_bDirtyBricksSorted = false;
	}

	/* An occupied voxel that no renderer owns has no AABB proxy, and the voxel
	   pass rasterizes proxies and nothing else. Registering it here is what
	   makes the bake-on-impact call site stop having to remember to. */
	if (m_Target.pLooseVoxels != nullptr &&
		(uiColor >> 24) != 0 &&
		uiOwnerSlot == VoxelOwnerVolume::k_uiNoOwnerSlot)
	{
		m_Target.pLooseVoxels->AddLooseVoxel(Vector3(
			static_cast<float>(uiX),
			static_cast<float>(uiY),
			static_cast<float>(uiZ)));
	}

	/* Anything that classified this voxel's neighbourhood - the integrity
	   checker's memo - is now describing a world that no longer exists. It
	   polls this rather than being told, so a new write path cannot forget to
	   invalidate. */
	m_Target.pGrid->BumpWriteGeneration();

	++m_uiWrites;

	return true;
}

bool VoxelEditBatch::Set(const Vector3& v3GridPosition, uint32_t uiColor, uint16_t uiOwnerSlot)
{
	if (!m_Target.IsValid())
	{
		++m_uiRejected;
		return false;
	}

	uint32_t uiX = 0;
	uint32_t uiY = 0;
	uint32_t uiZ = 0;

	if (!Resolve(v3GridPosition, uiX, uiY, uiZ))
		return false;

	return Write(uiX, uiY, uiZ, uiColor, uiOwnerSlot);
}

bool VoxelEditBatch::Clear(const Vector3& v3GridPosition)
{
	if (!m_Target.IsValid())
	{
		++m_uiRejected;
		return false;
	}

	uint32_t uiX = 0;
	uint32_t uiY = 0;
	uint32_t uiZ = 0;

	if (!Resolve(v3GridPosition, uiX, uiY, uiZ))
		return false;

	return Write(uiX, uiY, uiZ, 0, VoxelOwnerVolume::k_uiNoOwnerSlot);
}

uint32_t VoxelEditBatch::ClearRegion(const UVector3& v3Min, const UVector3& v3Size)
{
	if (!m_Target.IsValid())
		return 0;

	const uint32_t uiLastX = std::min(v3Min.x + v3Size.x, m_Target.v3WindowSize.x);
	const uint32_t uiLastY = std::min(v3Min.y + v3Size.y, m_Target.v3WindowSize.y);
	const uint32_t uiLastZ = std::min(v3Min.z + v3Size.z, m_Target.v3WindowSize.z);

	uint32_t uiCleared = 0;

	for (uint32_t uiZ = v3Min.z; uiZ < uiLastZ; ++uiZ)
	{
		for (uint32_t uiY = v3Min.y; uiY < uiLastY; ++uiY)
		{
			for (uint32_t uiX = v3Min.x; uiX < uiLastX; ++uiX)
			{
				if (Write(uiX, uiY, uiZ, 0, VoxelOwnerVolume::k_uiNoOwnerSlot))
					++uiCleared;
			}
		}
	}

	return uiCleared;
}

const std::vector<uint32_t>& VoxelEditBatch::GetDirtyBricks()
{
	if (!m_bDirtyBricksSorted)
	{
		/* Deduplicated lazily rather than on every write. A destruction burst
		   touches a few dozen bricks over tens of thousands of voxels, so a
		   set probe per voxel would cost far more than one sort of the
		   resulting vector - and most callers ask once, at the end. */
		std::sort(m_DirtyBricks.begin(), m_DirtyBricks.end());
		m_DirtyBricks.erase(std::unique(m_DirtyBricks.begin(), m_DirtyBricks.end()), m_DirtyBricks.end());

		m_bDirtyBricksSorted = true;
	}

	return m_DirtyBricks;
}
