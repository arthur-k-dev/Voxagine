#include "Scenarios/Common.h"

/* Every other tower belongs to an entity that refuses destruction, and the
   bursts are aimed at all of them equally.
 *
 * This scenario exists because island conversion never checked destructibility
 * - it did not have to, for as long as the seed set could only reach geometry a
 * burst had just cut. Widening the seeds made it reachable and a
 * non-destructible building collapsed in play. The protection invariant is
 * absolute: not one of these voxels may be cleared, by a burst or by
 * conversion. */
class ProtectedScenario : public SelftestScenario
{
public:
	const char* Name() const override { return "protected"; }

	void Configure(SelftestConfig& config) const override
	{
		config.uiTicks = 320 + SelftestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const SelftestConfig& config) const override
	{
		SelftestShapes::BuildTowers(world, config, true);
	}

	void Script(const SelftestConfig& config, std::vector<Burst>& o_bursts) const override
	{
		SelftestShapes::SweepTowers(config, o_bursts);
	}
};

VOXAGINE_SELFTEST_SCENARIO(ProtectedScenario)
