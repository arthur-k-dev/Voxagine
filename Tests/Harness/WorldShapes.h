#pragma once

#include "Framework/Scenario.h"

namespace TestColours
{
	const uint32_t k_uiGround = 0xFF404040u;
	const uint32_t k_uiStone = 0xFF906030u;
	const uint32_t k_uiFlesh = 0xFFC08060u;
}

/* Ticks a scenario must run *after* its last burst before the oracle is a fair
   question. Debris in flight is legitimately unsupported - it is a particle,
   not a voxel - but a particle that has just baked and whose support is about
   to be classified is not settled yet either. A run that stops mid-fall reports
   that as "the checker missed an island", which is a bug in the test rather
   than in the checker. */
namespace TestTiming
{
	const uint32_t k_uiSettleTicks = 240;
}

/* World shapes shared by more than one scenario or benchmark.
 *
 * A shape only belongs here once a second caller wants it - a scenario's whole
 * value is the situation it sets up, and pulling that into a shared helper too
 * eagerly is how a suite ends up with eight cases that are all the same case.
 */
namespace WorldShapes
{
	/* A lattice of towers standing on the ground, each its own owner slot so
	   the owner half of the state hash carries information rather than being
	   uniformly zero. Deliberately *not* touching the ground row: a tower
	   resting directly on y = 1 is grounded, and blowing its base out is what
	   produces an island. */
	void BuildTowers(VoxelWorldHarness& world, const DestructionConfig& config,
	                 bool bAlternateProtected);

	/* Three waves down the same towers, each aimed at a tower's centre so the
	   sphere severs the whole cross-section - an explosion offset to a corner
	   leaves the far corners standing and the tower never becomes an island,
	   which is realistic and measures nothing.
	 *
	 * Waves rather than one pass because the cost curve that matters (integrity
	 * getting slower the more has been destroyed) only appears once destruction
	 * overlaps destruction. */
	void SweepTowers(const DestructionConfig& config, std::vector<Burst>& o_bursts);
}
