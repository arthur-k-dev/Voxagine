#include "Harness/WorldShapes.h"

/* Four boxes stacked on each other, cut at the bottom.
 *
 * The case that says nothing can "rest on" an island without being part of it:
 * an island is a maximal 26-connected component, so cutting the bottom box
 * makes the entire pile one island. It is here because the opposite belief cost
 * seven milliseconds of seeding that could never have found anything. */
class StackedPile : public Scenario
{
public:
	const char* Name() const override { return "stacked-pile"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		world.FillGround(TestColours::k_uiGround);

		uint16_t uiSlot = 1;

		for (uint32_t uiLevel = 0; uiLevel < 4; ++uiLevel)
		{
			const uint32_t uiY = 1 + uiLevel * 10;
			const uint32_t uiInset = uiLevel * 2;

			world.FillBox(UVector3(30 + uiInset, uiY, 30 + uiInset),
			              UVector3(20 - uiInset * 2, 10, 20 - uiInset * 2),
			              TestColours::k_uiStone, uiSlot);
			++uiSlot;
		}

		(void)config;
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		for (uint32_t i = 0; i < 6; ++i)
		{
			Burst burst;
			burst.uiTick = 8 + i * 10;
			burst.v3Center = Vector3(32.f + i * 3.f, 5.f, 32.f + i * 3.f);
			burst.fRadius = 8.f;

			o_bursts.push_back(burst);
		}

		(void)config;
	}
};

VOXAGINE_SCENARIO(StackedPile)
