#include "Harness/WorldShapes.h"

/* Every other tower belongs to an entity that refuses destruction, and the
   bursts are aimed at all of them equally.
 *
 * This scenario exists because island conversion never checked destructibility
 * - it did not have to, for as long as the seed set could only reach geometry a
 * burst had just cut. Widening the seeds made it reachable and a
 * non-destructible building collapsed in play. The protection invariant is
 * absolute: not one of these voxels may be cleared, by a burst or by
 * conversion. */
class IndestructibleTowers : public Scenario
{
public:
	const char* Name() const override { return "indestructible-towers"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		WorldShapes::BuildTowers(world, config, true);
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		WorldShapes::SweepTowers(config, o_bursts);
	}
};

VOXAGINE_SCENARIO(IndestructibleTowers)
