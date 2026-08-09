#include "Harness/WorldShapes.h"

/* The baseline: ordinary destructible towers, swept from the top down. Nothing
   exotic - it is here so that a regression in the common case is not hidden by
   the fact that every other scenario is unusual. */
class SeveredTowers : public Scenario
{
public:
	const char* Name() const override { return "severed-towers"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		WorldShapes::BuildTowers(world, config, false);
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		WorldShapes::SweepTowers(config, o_bursts);
	}
};

VOXAGINE_SCENARIO(SeveredTowers)
