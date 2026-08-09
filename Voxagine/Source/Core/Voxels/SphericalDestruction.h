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

	/* The neighbours of one destroyed voxel, as candidate seeds.
	 *
	 * D6's producer half, and the scope here is the whole point. The original
	 * code pushed nine seeds for *every* destroyed voxel - the 3x3 in x/z one
	 * layer up - which is tens of thousands of hashes per explosion,
	 * deduplicated only within one batch, and almost all of them naming voxels
	 * the same loop destroyed moments later.
	 *
	 * The first attempt at fixing that swept the sphere's whole bounding box
	 * afterwards and seeded every occupied voxel with an empty face neighbour.
	 * That is cheaper and more thorough, and it was **wrong**: it re-asks the
	 * integrity question about geometry this burst never touched. A bullet
	 * clipping one voxel of rubble beside a building seeded the building's
	 * entire surface - and a building that was never ground-connected in the
	 * first place (a pristine level has 14,532 such voxels) then converted to
	 * debris in one go. Joey saw exactly that: a non-destructible building
	 * collapsing while a bullet bounced off it.
	 *
	 * Support can only have changed for a voxel that lost a neighbour. So the
	 * candidates are the neighbours of what was actually removed, gathered
	 * during the clear, and nothing else.
	 *
	 * All 26 of them, not the 6 faces. IntegrityChecker walks 26-connectivity,
	 * so a voxel held up through a *diagonal* link loses its support when that
	 * link is destroyed - and a face-only seed set never asks about it. Six was
	 * the first attempt and the gauntlet's oracle caught it immediately: six
	 * voxels in four components left standing that the exhaustive walk called
	 * ungrounded. The seed set has to use the same connectivity as the walk it
	 * feeds.
	 */
	inline void PushNeighbourCandidates(int32_t iX, int32_t iY, int32_t iZ, std::vector<uint64_t>& o_seeds);

	/* Keeps the candidates that are still standing, and drops the rest. Run
	   after the whole sphere is cleared, because most candidates are voxels the
	   same burst went on to destroy. */
	inline void FilterSeeds(const VoxelGrid& grid, std::vector<uint64_t>& seeds);

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
	             TCanDestroy&& canDestroy, TOnDestroyed&& onDestroyed,
	             std::vector<uint64_t>* pSeeds = nullptr)
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

					/* Candidate seeds: the six face neighbours of a voxel this
					   burst actually removed. Filtered to the ones still
					   standing once the whole sphere is cleared - see
					   FilterSeeds. */
					if (pSeeds != nullptr)
						PushNeighbourCandidates(iX, iY, iZ, *pSeeds);

					onDestroyed(v3Position, uiColor);
				}
			}
		}

		return result;
	}

}

/* Defined out of line only because it needs IntegrityChecker's hash, and that
   header has no business being pulled into everything that destroys a voxel. */
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"

inline void SphericalDestruction::PushNeighbourCandidates(
	int32_t iX, int32_t iY, int32_t iZ, std::vector<uint64_t>& o_seeds)
{
	for (int32_t iDY = -1; iDY <= 1; ++iDY)
	{
		for (int32_t iDZ = -1; iDZ <= 1; ++iDZ)
		{
			for (int32_t iDX = -1; iDX <= 1; ++iDX)
			{
				if (iDX == 0 && iDY == 0 && iDZ == 0)
					continue;

				const int32_t iNX = iX + iDX;
				const int32_t iNY = iY + iDY;
				const int32_t iNZ = iZ + iDZ;

				/* The ground layer supports everything and never falls, so
				   seeding it would only cost a walk that terminates
				   immediately. */
				if (iNX < 0 || iNY < static_cast<int32_t>(k_uiMinDestructibleY) || iNZ < 0)
					continue;

				o_seeds.push_back(IntegrityChecker::PositionToHash(
					static_cast<uint32_t>(iNX), static_cast<uint32_t>(iNY), static_cast<uint32_t>(iNZ)));
			}
		}
	}
}

inline void SphericalDestruction::FilterSeeds(const VoxelGrid& grid, std::vector<uint64_t>& seeds)
{
	size_t uiKept = 0;

	for (size_t i = 0; i < seeds.size(); ++i)
	{
		const Vector3 v3Position = IntegrityChecker::HashToPosition(seeds[i]);

		const Voxel* pVoxel = grid.GetVoxel(
			static_cast<uint32_t>(v3Position.x),
			static_cast<uint32_t>(v3Position.y),
			static_cast<uint32_t>(v3Position.z));

		if (pVoxel == nullptr || !pVoxel->IsActive())
			continue;

		seeds[uiKept++] = seeds[i];
	}

	seeds.resize(uiKept);
}
