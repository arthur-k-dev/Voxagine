#include "Harness/WorldShapes.h"

/* A slab whose only connection to the ground is one diagonal step.
 *
 * 26-connectivity says it is supported; 6-connectivity says it is not.
 * IntegrityChecker walks 26, so the seed set has to as well - the first attempt
 * at narrowing the seeds used the six faces and left slabs like this standing
 * after their link was cut. The oracle invariant catches it. */
class DiagonalSupport : public Scenario
{
public:
	const char* Name() const override { return "diagonal-support"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		world.FillGround(TestColours::k_uiGround);

		world.FillBox(UVector3(20, 1, 20), UVector3(2, 12, 2), TestColours::k_uiStone, 1);
		world.Set(22, 13, 22, TestColours::k_uiStone, 1);
		world.FillBox(UVector3(23, 14, 23), UVector3(10, 3, 10), TestColours::k_uiStone, 2);

		(void)config;
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		Burst burst;
		burst.uiTick = 8;
		burst.v3Center = Vector3(22.f, 13.f, 22.f);
		burst.fRadius = 2.f;

		o_bursts.push_back(burst);

		(void)config;
	}
};

VOXAGINE_SCENARIO(DiagonalSupport)
