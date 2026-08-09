#include "Scenarios/Common.h"

/* The baseline: ordinary destructible towers, swept from the top down. Nothing
   exotic - it is here so that a regression in the common case is not hidden by
   the fact that every other scenario is unusual. */
class TowersScenario : public SelftestScenario
{
public:
	const char* Name() const override { return "towers"; }

	void Configure(SelftestConfig& config) const override
	{
		config.uiTicks = 320 + SelftestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const SelftestConfig& config) const override
	{
		SelftestShapes::BuildTowers(world, config, false);
	}

	void Script(const SelftestConfig& config, std::vector<Burst>& o_bursts) const override
	{
		SelftestShapes::SweepTowers(config, o_bursts);
	}
};

VOXAGINE_SELFTEST_SCENARIO(TowersScenario)
