#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/Voxels/VoxelEditBatch.h"

/* The explosion, as an algorithm rather than as a method on PhysicsSystem.
 *
 * DESTRUCTION_PLAN.md phase 2. Everything gameplay-shaped - "is this entity
 * destructible", "spawn a particle here with this colour and this velocity" -
 * is a callback, so the loop itself needs no World, no entities and no particle
 * pool. That is what lets the gauntlet and the unit tests drive the real thing
 * instead of a copy of it, which the phase 0 gauntlet had to.
 *
 * Callbacks rather than an interface: a burst clears tens of thousands of
 * voxels and an interface would be two virtual calls per voxel. Templated on
 * them, they inline.
 *
 * What this replaces, and why each part had to go:
 *
 * - Two `new[] uint8_t[diameter^3]` per call, allocated *before* the validity
 *   check that returns early, so a call against a non-resident chunk leaked
 *   both (D1).
 * - An unvalidated radius: `(uint32_t)fRadius * 2` on a negative or NaN radius
 *   is undefined, and `diameter^3` overflows uint32_t above 1625 (D2).
 * - A running `volumePos` incremented at the top of the loop and then read as
 *   `volumePos.x - 1`, with row-wrap corrections that did not agree with the
 *   flat index into the fetched box - so on a wrap the cleared voxel, the
 *   sphere test and `voxels[i]` were three different cells (D3).
 * - `sqrt` per candidate voxel, where squared distances answer the same
 *   question (part of D3's item).
 * - A colour read back out of the mapped voxel buffer per destroyed voxel,
 *   which is an uncached PCIe read of VRAM, when the CPU voxel with the same
 *   colour was already in hand (D4).
 */
namespace SphericalDestruction
{
	/* A radius past this is treated as a bug and clamped. The loop is over the
	   bounding box, so cost is (2r+1)^3: 64 is 2.1 M voxel tests, a few
	   milliseconds, and already far larger than anything the game fires. The
	   old code had no bound at all and `diameter^3` silently overflowed
	   uint32_t above 1625. */
	static constexpr float k_fMaxRadius = 64.f;

	/* The ground layer is never destructible - y = 0 is the ground plane the
	   post-processing pass composites analytically, and the integrity checker
	   defines "grounded" as y - 1 == 0. */
	static constexpr uint32_t k_uiMinDestructibleY = 1;

	struct Result
	{
		uint32_t uiDestroyed = 0;

		/* Voxels inside the sphere that were occupied but whose owner declined
		   destruction. Worth reporting separately from "not there": a shot that
		   destroys nothing because everything it hit is indestructible is a
		   different situation from one that hit air. */
		uint32_t uiProtected = 0;

		/* True when the requested radius was clamped to k_fMaxRadius, or the
		   request was rejected outright for being non-finite or non-positive. */
		bool bRadiusClamped = false;
		bool bRejected = false;
	};

	/* Clears every occupied voxel inside the sphere that `canDestroy` allows.
	 *
	 * `canDestroy(uiOwnerSlot)` is called once per occupied candidate voxel.
	 * `onDestroyed(v3GridPosition, uiColour)` is called after the clear, with
	 * the colour taken from the CPU voxel - it is where the caller spawns
	 * debris.
	 *
	 * Iteration is z/y/x with coordinates computed per cell rather than carried
	 * in a running vector, so there is no wrap correction to get wrong, and the
	 * distance test is squared.
	 */
	template <typename TCanDestroy, typename TOnDestroyed>
	Result Apply(VoxelEditBatch& batch, VoxelGrid& grid,
	             const Vector3& v3GridCenter, float fRadius,
	             TCanDestroy&& canDestroy, TOnDestroyed&& onDestroyed)
	{
		Result result;

		if (!std::isfinite(v3GridCenter.x) || !std::isfinite(v3GridCenter.y) || !std::isfinite(v3GridCenter.z) ||
			!std::isfinite(fRadius) || fRadius < 1.f)
		{
			result.bRejected = true;
			return result;
		}

		if (fRadius > k_fMaxRadius)
		{
			fRadius = k_fMaxRadius;
			result.bRadiusClamped = true;
		}

		const UVector3 v3Dimensions = grid.GetDimensions();

		if (v3Dimensions.x == 0 || v3Dimensions.y == 0 || v3Dimensions.z == 0)
		{
			result.bRejected = true;
			return result;
		}

		const int32_t iRadius = static_cast<int32_t>(fRadius);
		const float fRadiusSquared = fRadius * fRadius;

		const int32_t iCenterX = static_cast<int32_t>(std::floor(v3GridCenter.x));
		const int32_t iCenterY = static_cast<int32_t>(std::floor(v3GridCenter.y));
		const int32_t iCenterZ = static_cast<int32_t>(std::floor(v3GridCenter.z));

		/* Clamped once, here, so the loop body has no bounds test at all. */
		const int32_t iMinX = std::max(iCenterX - iRadius, 0);
		const int32_t iMinY = std::max(iCenterY - iRadius, static_cast<int32_t>(k_uiMinDestructibleY));
		const int32_t iMinZ = std::max(iCenterZ - iRadius, 0);

		const int32_t iMaxX = std::min(iCenterX + iRadius, static_cast<int32_t>(v3Dimensions.x) - 1);
		const int32_t iMaxY = std::min(iCenterY + iRadius, static_cast<int32_t>(v3Dimensions.y) - 1);
		const int32_t iMaxZ = std::min(iCenterZ + iRadius, static_cast<int32_t>(v3Dimensions.z) - 1);

		for (int32_t iZ = iMinZ; iZ <= iMaxZ; ++iZ)
		{
			const float fDZ = static_cast<float>(iZ) - v3GridCenter.z;
			const float fDZ2 = fDZ * fDZ;

			for (int32_t iY = iMinY; iY <= iMaxY; ++iY)
			{
				const float fDY = static_cast<float>(iY) - v3GridCenter.y;
				const float fDZY2 = fDZ2 + fDY * fDY;

				if (fDZY2 > fRadiusSquared)
					continue;

				for (int32_t iX = iMinX; iX <= iMaxX; ++iX)
				{
					const float fDX = static_cast<float>(iX) - v3GridCenter.x;

					if (fDZY2 + fDX * fDX > fRadiusSquared)
						continue;

					const VoxelCell cell = grid.GetCell(
						static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ));

					if (!cell.IsActive())
						continue;

					if (!canDestroy(cell.GetSlot()))
					{
						++result.uiProtected;
						continue;
					}

					/* The colour comes from the CPU voxel that is already
					   resolved, never from the mapping (D4, rule 1). */
					const uint32_t uiColor = cell.GetColor();

					const Vector3 v3Position(
						static_cast<float>(iX), static_cast<float>(iY), static_cast<float>(iZ));

					batch.Clear(v3Position);
					++result.uiDestroyed;

					onDestroyed(v3Position, uiColor);
				}
			}
		}

		return result;
	}

	/* Voxels that survived around the hole and might now be unsupported.
	 *
	 * This is D6's producer half. The old code pushed nine seeds for *every*
	 * destroyed voxel - the 3x3 in x/z one layer up - which is tens of
	 * thousands of hashes per explosion, deduplicated only within the one
	 * batch, and almost all of them naming voxels that were themselves
	 * destroyed a moment later in the same loop.
	 *
	 * Run after the clear instead, and the question has an exact answer: an
	 * occupied voxel near the hole whose support could have changed is one with
	 * an empty face neighbour. That is the surface of what is left, which is
	 * proportional to area rather than to volume, and it is a strict superset
	 * of the old seed set - the old seeds were occupied voxels sitting directly
	 * on a destroyed one, i.e. with an empty neighbour below.
	 *
	 * The window is the sphere's box grown by two, because a voxel one step
	 * outside the sphere can have a neighbour inside it.
	 */
	inline void CollectSeeds(const VoxelGrid& grid, const Vector3& v3GridCenter, float fRadius,
	                         std::vector<uint64_t>& o_seeds);
}

/* Defined out of line only because it needs IntegrityChecker's hash, and that
   header has no business being pulled into everything that destroys a voxel. */
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"

inline void SphericalDestruction::CollectSeeds(
	const VoxelGrid& grid, const Vector3& v3GridCenter, float fRadius, std::vector<uint64_t>& o_seeds)
{
	if (!std::isfinite(fRadius) || fRadius < 1.f)
		return;

	if (fRadius > k_fMaxRadius)
		fRadius = k_fMaxRadius;

	const UVector3 v3Dimensions = grid.GetDimensions();

	if (v3Dimensions.x == 0 || v3Dimensions.y == 0 || v3Dimensions.z == 0)
		return;

	const int32_t iRadius = static_cast<int32_t>(fRadius) + 2;

	const int32_t iCenterX = static_cast<int32_t>(std::floor(v3GridCenter.x));
	const int32_t iCenterY = static_cast<int32_t>(std::floor(v3GridCenter.y));
	const int32_t iCenterZ = static_cast<int32_t>(std::floor(v3GridCenter.z));

	const int32_t iMinX = std::max(iCenterX - iRadius, 0);
	const int32_t iMinY = std::max(iCenterY - iRadius, static_cast<int32_t>(k_uiMinDestructibleY) + 1);
	const int32_t iMinZ = std::max(iCenterZ - iRadius, 0);

	const int32_t iMaxX = std::min(iCenterX + iRadius, static_cast<int32_t>(v3Dimensions.x) - 1);
	const int32_t iMaxY = std::min(iCenterY + iRadius, static_cast<int32_t>(v3Dimensions.y) - 1);
	const int32_t iMaxZ = std::min(iCenterZ + iRadius, static_cast<int32_t>(v3Dimensions.z) - 1);

	static const int32_t k_iFaceNeighbours[6][3] =
	{
		{ -1, 0, 0 }, { 1, 0, 0 },
		{ 0, -1, 0 }, { 0, 1, 0 },
		{ 0, 0, -1 }, { 0, 0, 1 },
	};

	for (int32_t iZ = iMinZ; iZ <= iMaxZ; ++iZ)
	for (int32_t iY = iMinY; iY <= iMaxY; ++iY)
	for (int32_t iX = iMinX; iX <= iMaxX; ++iX)
	{
		const Voxel* pVoxel = grid.GetVoxel(
			static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ));

		if (pVoxel == nullptr || !pVoxel->IsActive())
			continue;

		for (const int32_t (&offset)[3] : k_iFaceNeighbours)
		{
			const int32_t iNX = iX + offset[0];
			const int32_t iNY = iY + offset[1];
			const int32_t iNZ = iZ + offset[2];

			if (iNX < 0 || iNY < 0 || iNZ < 0)
				continue;

			const Voxel* pNeighbour = grid.GetVoxel(
				static_cast<uint32_t>(iNX), static_cast<uint32_t>(iNY), static_cast<uint32_t>(iNZ));

			if (pNeighbour != nullptr && pNeighbour->IsActive())
				continue;

			o_seeds.push_back(IntegrityChecker::PositionToHash(
				static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ)));

			break;
		}
	}
}
