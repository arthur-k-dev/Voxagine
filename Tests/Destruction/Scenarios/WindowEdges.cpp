#include "Harness/WorldShapes.h"

/* Bursts centred on, over and entirely outside the window's edges and corners.
   Everything here is clamping: a sphere that straddles the boundary, one whose
   centre is outside it, one above the ceiling. The batch rejects what it must
   and the rest still happens. */
class WindowEdges : public Scenario
{
public:
	const char* Name() const override { return "window-edges"; }

	void Configure(DestructionConfig& config) const override
	{
		config.uiTicks = 320 + TestTiming::k_uiSettleTicks;
	}

	void Build(VoxelWorldHarness& world, const DestructionConfig& config) const override
	{
		world.FillGround(TestColours::k_uiGround);
		world.FillBox(UVector3(0, 1, 0), UVector3(config.v3Size.x, 20, config.v3Size.z),
		              TestColours::k_uiStone, 1);
	}

	void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const override
	{
		const float fX = static_cast<float>(config.v3Size.x);
		const float fY = static_cast<float>(config.v3Size.y);
		const float fZ = static_cast<float>(config.v3Size.z);

		const Vector3 centres[] = {
			Vector3(0.f, 1.f, 0.f), Vector3(fX - 1.f, 1.f, fZ - 1.f),
			Vector3(-8.f, 10.f, fZ * 0.5f), Vector3(fX + 8.f, 10.f, fZ * 0.5f),
			Vector3(fX * 0.5f, fY + 4.f, fZ * 0.5f), Vector3(fX * 0.5f, 1.f, -6.f),
			Vector3(0.f, 10.f, fZ - 1.f), Vector3(fX - 1.f, 10.f, 0.f),
		};

		uint32_t uiTick = 8;

		for (const Vector3& v3Centre : centres)
		{
			Burst burst;
			burst.uiTick = uiTick;
			burst.v3Center = v3Centre;
			burst.fRadius = 10.f;

			o_bursts.push_back(burst);

			uiTick += 10;
		}
	}
};

VOXAGINE_SCENARIO(WindowEdges)
