#pragma once

#include <cstdint>

#include "Core/Math.h"
#include "Core/Particles/ParticleCore.h"
#include "Core/Particles/ParticleLanding.h"

/* One particle, one tick, as an algorithm rather than as a method on
 * PhysicsSystem.
 *
 * Same reasoning as SphericalDestruction (DESTRUCTION_PLAN.md phase 2): the
 * only thing here that needs a World is the *write* at the end, so that is a
 * callback and everything else runs against a VoxelBrickGrid and a window size.
 * The gauntlet drives this rather than a copy of it, which is what lets the
 * particle sim be measured deterministically at a chosen particle count -
 * phase 6's gate is exactly that measurement.
 */
namespace ParticleSimulation
{
	struct Settings
	{
		Vector3 v3Gravity = Vector3(0.f, -39.81f, 0.f);

		/* Below this speed an impact settles rather than bounces. */
		float fDestroyThreshold = 1.f;
		float fBounceMultiplier = 1.5f;
	};

	struct Outcome
	{
		bool bRetire = false;

		/* Set when the particle settled somewhere and wants baking. */
		bool bBake = false;
		int32_t iBakeX = 0;
		int32_t iBakeY = 0;
		int32_t iBakeZ = 0;
	};

	/* Advances the particle at a dense index and says what should happen to it.
	 * Writes nothing: the caller performs the bake through its own
	 * VoxelEditBatch and retires through the core, because those are the two
	 * things that differ between the engine and a harness.
	 */
	inline Outcome Step(ParticleCore& core, uint32_t uiIndex, float fDeltaTime,
	                    const VoxelBrickGrid& bricks, const UVector3& v3WindowSize,
	                    const Vector3& v3WorldOffset, const Settings& settings)
	{
		Outcome outcome;

		/* The timer path is live. The old pool set it to the "no timer"
		   sentinel and nothing else, so all of it was dead code (ledger M3). */
		if (core.Timer[uiIndex] > 0.f)
		{
			core.Timer[uiIndex] -= fDeltaTime;

			if (core.Timer[uiIndex] <= 0.f)
			{
				outcome.bRetire = true;
				return outcome;
			}
		}

		/* Grid position is derived, never stored. A window slide changes what a
		   grid coordinate means, and the old pool cached one per particle - so
		   a slide shorter than the 100-unit teleport clamp left every particle
		   in flight touching the wrong cells (ledger P9). Position is level
		   space, so this is a subtract and a floor. */
		const Vector3 v3PrevGrid = glm::floor(core.Position[uiIndex] - v3WorldOffset);

		core.Velocity[uiIndex] += settings.v3Gravity * fDeltaTime;
		core.Position[uiIndex] += core.Velocity[uiIndex] * fDeltaTime;

		Vector3 v3NewGrid = glm::floor(core.Position[uiIndex] - v3WorldOffset);

		/* Clamped so a very fast particle does not skip the ground and fall out
		   of the world before it can bake. */
		if (v3NewGrid.y < 0.f)
			v3NewGrid.y = 0.f;

		if (v3PrevGrid == v3NewGrid)
			return outcome;

		const int32_t iX = static_cast<int32_t>(v3NewGrid.x);
		const int32_t iY = static_cast<int32_t>(v3NewGrid.y);
		const int32_t iZ = static_cast<int32_t>(v3NewGrid.z);

		if (!ParticleLanding::IsInside(v3WindowSize, iX, iY, iZ))
		{
			/* Out of the window entirely. Nothing to bake into. */
			outcome.bRetire = true;
			return outcome;
		}

		/* One bit test in cached memory, where the old code ran one to three
		   full chunk-index resolutions through GetCell - and one of those was
		   only ever asked so the particle could check its own claim, which no
		   longer exists. */
		if (!ParticleLanding::IsOccupied(bricks, v3WindowSize, iX, iY, iZ))
			return outcome;

		const float fSpeed = glm::length(core.Velocity[uiIndex]);
		const Vector3 v3Normal = glm::normalize(v3PrevGrid - v3NewGrid);

		if (fSpeed >= settings.fDestroyThreshold && v3Normal != Vector3(0.f, 1.f, 0.f))
		{
			const float fContactVel = glm::dot(core.Velocity[uiIndex], v3Normal);

			if (fContactVel < 0.f)
				core.Velocity[uiIndex] += -v3Normal * fContactVel * settings.fBounceMultiplier;

			return outcome;
		}

		outcome.bRetire = true;

		if (!core.BakeOnImpact[uiIndex])
			return outcome;

		/* One resolution, one position, used for the colour, the occupancy, the
		   brick count, the owner and the loose-voxel registration - because the
		   batch does all five from it. The old bake computed two positions and
		   used them inconsistently (P5), and skipped the bake entirely whenever
		   its claim had been taken over, which is debris that silently
		   vanished (P6). */
		outcome.bBake = ParticleLanding::Resolve(
			bricks, v3WindowSize, iX, iY, iZ,
			outcome.iBakeX, outcome.iBakeY, outcome.iBakeZ);

		return outcome;
	}
}
