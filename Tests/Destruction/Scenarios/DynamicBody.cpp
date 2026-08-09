#include "Harness/WorldShapes.h"

/* A dynamic renderer's voxels: in the mapped buffer and the brick grid, and in
   neither the physics grid nor the owner volume.
 *
 * VoxelBaker::Occupy takes the physics-grid branch only when the owner
 * IsStatic(), and that asymmetry shipped a bug nothing could have caught -
 * taking a voxel's colour from the CPU voxel is right for static geometry and
 * returns zero for every dynamic renderer, so every humanoid corpse came apart
 * into black particles. The harness had no way to express a dynamic voxel, so
 * no test could fail.
 *
 * What this asserts is not the colour, which lives in the emitter: it is that
 * destruction, integrity and the brick grid all stay coherent when a body of
 * mapping-only voxels sits in the middle of the world. */
class DynamicBody : public Scenario
{
public:
	const char* Name() const override { return "dynamic-body"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		world.FillGround(TestColours::k_uiGround);
		world.FillBox(UVector3(20, 1, 20), UVector3(20, 20, 20), TestColours::k_uiStone, 1);

		for (uint32_t uiZ = 44; uiZ < 52; ++uiZ)
		for (uint32_t uiY = 1; uiY < 13; ++uiY)
		for (uint32_t uiX = 44; uiX < 52; ++uiX)
			world.SetDynamic(uiX, uiY, uiZ, TestColours::k_uiFlesh);

		(void)config;
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		uint32_t uiTick = 8;

		/* Straight through the dynamic body as well as the static block. */
		for (uint32_t i = 0; i < 10; ++i)
		{
			Burst burst;
			burst.uiTick = uiTick;
			burst.v3Center = Vector3(24.f + i * 3.f, 6.f, 24.f + i * 3.f);
			burst.fRadius = 8.f;

			o_bursts.push_back(burst);

			uiTick += 8;
		}

		(void)config;
	}
};

VOXAGINE_SCENARIO(DynamicBody)
