#include "Harness/WorldShapes.h"

/* Geometry that never touched the ground and never should be asked about:
   decoration, props resting on props, models whose only path down runs through
   another model. A real level is full of it - the in-game audit reports 14,532
   such voxels in 211 components on a pristine Fishing_Village.
 *
 * All the bursts land on a structure in one corner. The shelves are well clear
 * of them and comfortably inside the box that a whole-sphere seed sweep would
 * have covered, which is what collapsed a building in play. MinStandingAtEnd
 * returns everything it started with: not one shelf voxel may be lost. */
class SuspendedDecoration : public Scenario
{
public:
	const char* Name() const override { return "suspended-decoration"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		world.FillGround(TestColours::k_uiGround);
		world.FillBox(UVector3(16, 1, 16), UVector3(16, 24, 16), TestColours::k_uiStone, 1);

		uint16_t uiSlot = 50;

		for (uint32_t uiZ = 20; uiZ + 6 < config.v3Size.z; uiZ += 20)
		{
			for (uint32_t uiX = 40; uiX + 6 < config.v3Size.x; uiX += 20)
			{
				world.FillBox(UVector3(uiX, 30, uiZ), UVector3(6, 4, 6), TestColours::k_uiStone, uiSlot);
				++uiSlot;
			}
		}
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		uint32_t uiTick = 8;

		for (uint32_t i = 0; i < 12; ++i)
		{
			Burst burst;
			burst.uiTick = uiTick;
			burst.v3Center = Vector3(24.f, 4.f + (i % 6) * 4.f, 24.f);
			burst.fRadius = 7.f;

			o_bursts.push_back(burst);

			uiTick += 8;
		}

		(void)config;
	}

	uint64_t MinStandingAtEnd(uint64_t uiStandingAtStart) const override
	{
		return uiStandingAtStart;
	}
};

VOXAGINE_SCENARIO(SuspendedDecoration)
