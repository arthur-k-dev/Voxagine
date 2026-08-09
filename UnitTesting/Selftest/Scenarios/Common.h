#pragma once

#include "Selftest/SelftestScenario.h"

namespace SelftestColours
{
	const uint32_t k_uiGround = 0xFF404040u;
	const uint32_t k_uiStone = 0xFF906030u;
	const uint32_t k_uiFlesh = 0xFFC08060u;
}

/* A lattice of towers and a sweep of bursts down them. Shared because four
   scenarios differ from it only in what the towers are made of. */
/* Ticks a scenario must run *after* its last burst before the oracle is a fair
   question. Debris in flight is legitimately unsupported - it is a particle,
   not a voxel - but a particle that has just baked and whose support is about
   to be classified is not settled yet either. A run that stops mid-fall reports
   that as "the checker missed an island", which is a bug in the test rather
   than in the checker. */
namespace SelftestTiming
{
	const uint32_t k_uiSettleTicks = 240;
}

namespace SelftestShapes
{
	void BuildTowers(VoxelWorldHarness& world, const SelftestConfig& config,
	                 bool bAlternateProtected);

	void SweepTowers(const SelftestConfig& config, std::vector<Burst>& o_bursts);
}
