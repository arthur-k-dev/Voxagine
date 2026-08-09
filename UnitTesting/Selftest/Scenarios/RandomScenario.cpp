#include <string>

#include "Core/Utils/DeterministicRandom.h"
#include "Scenarios/Common.h"

/* Worlds nobody chose the shape of.
 *
 * The scenarios beside this one encode the cases somebody thought of, and that
 * set had a hole in it four times over. This is the part that does not depend
 * on imagination: boxes at random positions, some of them indestructible, some
 * of them floating by accident, and bursts wherever they land. The invariants
 * are the same ones - which is the whole reason invariants are separate from
 * scenarios.
 *
 * Seeded from the instance, so a failure names a world that can be rebuilt
 * exactly. */
class RandomScenario : public SelftestScenario
{
public:
	explicit RandomScenario(uint32_t uiIndex) :
		m_uiIndex(uiIndex),
		m_Name("random/" + std::to_string(uiIndex))
	{
	}

	const char* Name() const override { return m_Name.c_str(); }

	void Configure(SelftestConfig& config) const override
	{
		config.v3Size = UVector3(64, 48, 64);
		config.v3ChunkSize = UVector3(32, 48, 32);
		config.uiTicks = 240 + SelftestTiming::k_uiSettleTicks;
		config.uiSeed = m_uiIndex;
	}

	void Build(VoxelWorldHarness& world, const SelftestConfig& config) const override
	{
		world.FillGround(SelftestColours::k_uiGround);

		DeterministicRandom random(m_uiIndex * 7919u + 13u);

		for (uint16_t uiSlot = 1; uiSlot <= 12; ++uiSlot)
		{
			const UVector3 v3Min(
				static_cast<uint32_t>(random.Range(2.f, static_cast<float>(config.v3Size.x) - 14.f)),
				static_cast<uint32_t>(random.Range(1.f, static_cast<float>(config.v3Size.y) - 18.f)),
				static_cast<uint32_t>(random.Range(2.f, static_cast<float>(config.v3Size.z) - 14.f)));

			const UVector3 v3Size(
				static_cast<uint32_t>(random.Range(3.f, 12.f)),
				static_cast<uint32_t>(random.Range(3.f, 14.f)),
				static_cast<uint32_t>(random.Range(3.f, 12.f)));

			const bool bProtect = random.Range(0.f, 1.f) < 0.25f;

			world.FillBox(v3Min, v3Size, SelftestColours::k_uiStone,
			              bProtect ? SelftestWorld::k_uiProtectedSlot : uiSlot);
		}
	}

	void Script(const SelftestConfig& config, std::vector<Burst>& o_bursts) const override
	{
		/* A second stream, so changing the level generator does not silently
		   move every burst as well. */
		DeterministicRandom random(m_uiIndex * 104729u + 7u);

		for (uint32_t i = 0; i < 20; ++i)
		{
			Burst burst;
			burst.uiTick = 8 + i * 8;
			burst.v3Center = Vector3(
				random.Range(0.f, static_cast<float>(config.v3Size.x)),
				random.Range(1.f, static_cast<float>(config.v3Size.y) * 0.8f),
				random.Range(0.f, static_cast<float>(config.v3Size.z)));
			burst.fRadius = random.Range(3.f, 11.f);

			o_bursts.push_back(burst);
		}
	}

private:
	uint32_t m_uiIndex;
	std::string m_Name;
};

/* Registered as a batch rather than one per file, because the whole point is
   that nobody picked them. The count is the only knob. */
namespace
{
	const bool s_bRegisteredRandom = []()
	{
		for (uint32_t i = 1; i <= 24; ++i)
			SelftestRegistry::Get().Add(std::unique_ptr<SelftestScenario>(new RandomScenario(i)));

		return true;
	}();
}
