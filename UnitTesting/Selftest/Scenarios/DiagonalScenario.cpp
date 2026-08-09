#include "Scenarios/Common.h"

/* A slab whose only connection to the ground is one diagonal step.
 *
 * 26-connectivity says it is supported; 6-connectivity says it is not.
 * IntegrityChecker walks 26, so the seed set has to as well - the first attempt
 * at narrowing the seeds used the six faces and left slabs like this standing
 * after their link was cut. The oracle invariant catches it. */
class DiagonalScenario : public SelftestScenario
{
public:
	const char* Name() const override { return "diagonal"; }

	void Configure(SelftestConfig& config) const override
	{
		config.uiTicks = 320 + SelftestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const SelftestConfig& config) const override
	{
		world.FillGround(SelftestColours::k_uiGround);

		world.FillBox(UVector3(20, 1, 20), UVector3(2, 12, 2), SelftestColours::k_uiStone, 1);
		world.Set(22, 13, 22, SelftestColours::k_uiStone, 1);
		world.FillBox(UVector3(23, 14, 23), UVector3(10, 3, 10), SelftestColours::k_uiStone, 2);

		(void)config;
	}

	void Script(const SelftestConfig& config, std::vector<Burst>& o_bursts) const override
	{
		Burst burst;
		burst.uiTick = 8;
		burst.v3Center = Vector3(22.f, 13.f, 22.f);
		burst.fRadius = 2.f;

		o_bursts.push_back(burst);

		(void)config;
	}
};

VOXAGINE_SELFTEST_SCENARIO(DiagonalScenario)
