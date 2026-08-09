#pragma once

#include <cstdint>

#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/Math.h"
#include "Core/Platform/Rendering/VoxelBrickGrid.h"

/* Where a particle's voxel goes when it lands, resolved once.
 *
 * DESTRUCTION_PLAN.md phase 3. The old bake computed two positions and used
 * them inconsistently: `bakeVoxelPos` for the colour write and the loose-voxel
 * registration, `bakeCellPos` for the owner - and once `FindEmtpyNeighbor` had
 * answered, those were different cells (ledger P5, known and deliberately
 * unfixed at the time). Resolving once and returning one position makes that
 * impossible rather than careful.
 *
 * Occupancy comes from the brick grid's bitmap, which is one bit test in
 * ordinary cached memory. The mapped voxel buffer is never read (rule 1) and
 * the CPU voxel is not needed either - the question is "is this cell free",
 * not "what colour is it".
 */
namespace ParticleLanding
{
	/* How far above the impact cell to look before giving up and searching
	   sideways. One: debris settles on top of what it hit. */
	static constexpr int32_t k_iLiftHeight = 1;

	inline bool IsInside(const UVector3& v3WindowSize, int32_t iX, int32_t iY, int32_t iZ)
	{
		return iX >= 0 && iY >= 0 && iZ >= 0 &&
			static_cast<uint32_t>(iX) < v3WindowSize.x &&
			static_cast<uint32_t>(iY) < v3WindowSize.y &&
			static_cast<uint32_t>(iZ) < v3WindowSize.z;
	}

	inline uint32_t VoxelID(const UVector3& v3WindowSize, int32_t iX, int32_t iY, int32_t iZ)
	{
		return static_cast<uint32_t>(iX) +
			static_cast<uint32_t>(iY) * v3WindowSize.x +
			static_cast<uint32_t>(iZ) * v3WindowSize.x * v3WindowSize.y;
	}

	/* One bit test. This is the collision predicate the simulation runs per
	   particle that changed cell, replacing one to three full chunk-index
	   resolutions through VoxelGrid::GetCell. */
	inline bool IsOccupied(const VoxelBrickGrid& bricks, const UVector3& v3WindowSize,
	                       int32_t iX, int32_t iY, int32_t iZ)
	{
		if (!IsInside(v3WindowSize, iX, iY, iZ))
			return false;

		return bricks.IsOccupied(VoxelID(v3WindowSize, iX, iY, iZ));
	}

	/* The cell a particle impacting at `iX, iY, iZ` should bake into, or false
	   if there is nowhere for it.
	 *
	 * Directly above the impact first, then the ring around it at the same
	 * height and one above - the same neighbourhood the old FindEmtpyNeighbor
	 * searched, minus the two-position bookkeeping. Deterministic order, so a
	 * replay lands debris in the same cells.
	 */
	inline bool Resolve(const VoxelBrickGrid& bricks, const UVector3& v3WindowSize,
	                    int32_t iX, int32_t iY, int32_t iZ,
	                    int32_t& o_iX, int32_t& o_iY, int32_t& o_iZ)
	{
		if (v3WindowSize.x == 0 || v3WindowSize.y == 0 || v3WindowSize.z == 0)
			return false;

		const int32_t iAboveY = iY + k_iLiftHeight;

		if (IsInside(v3WindowSize, iX, iAboveY, iZ) &&
			!IsOccupied(bricks, v3WindowSize, iX, iAboveY, iZ))
		{
			o_iX = iX;
			o_iY = iAboveY;
			o_iZ = iZ;

			return true;
		}

		for (int32_t iDY = 0; iDY <= k_iLiftHeight; ++iDY)
		{
			for (int32_t iDZ = -1; iDZ <= 1; ++iDZ)
			{
				for (int32_t iDX = -1; iDX <= 1; ++iDX)
				{
					if (iDX == 0 && iDZ == 0)
						continue;

					const int32_t iNX = iX + iDX;
					const int32_t iNY = iY + iDY;
					const int32_t iNZ = iZ + iDZ;

					if (!IsInside(v3WindowSize, iNX, iNY, iNZ))
						continue;

					if (IsOccupied(bricks, v3WindowSize, iNX, iNY, iNZ))
						continue;

					o_iX = iNX;
					o_iY = iNY;
					o_iZ = iNZ;

					return true;
				}
			}
		}

		return false;
	}
}
