#include "Scenarios/Common.h"

void SelftestShapes::BuildTowers(VoxelWorldHarness& world, const SelftestConfig& config,
                                 bool bAlternateProtected)
{
	world.FillGround(SelftestColours::k_uiGround);

	uint16_t uiSlot = 1;
	bool bProtect = false;

	for (uint32_t uiZ = 16; uiZ + 12 < config.v3Size.z - 8; uiZ += 32)
	{
		for (uint32_t uiX = 16; uiX + 12 < config.v3Size.x - 8; uiX += 24)
		{
			const uint16_t uiOwner = (bAlternateProtected && bProtect)
				? SelftestWorld::k_uiProtectedSlot
				: uiSlot;

			world.FillBox(UVector3(uiX, 1, uiZ), UVector3(12, 40, 12), SelftestColours::k_uiStone, uiOwner);

			bProtect = !bProtect;
			++uiSlot;
		}
	}
}

void SelftestShapes::SweepTowers(const SelftestConfig& config, std::vector<Burst>& o_bursts)
{
	uint32_t uiTick = 8;

	for (float fHeight : { 30.f, 16.f, 6.f })
	{
		for (uint32_t uiZ = 16; uiZ + 12 < config.v3Size.z - 8; uiZ += 32)
		{
			for (uint32_t uiX = 16; uiX + 12 < config.v3Size.x - 8; uiX += 24)
			{
				Burst burst;
				burst.uiTick = uiTick;
				burst.v3Center = Vector3(uiX + 6.f, fHeight, uiZ + 6.f);
				burst.fRadius = 9.f;

				o_bursts.push_back(burst);

				uiTick += 4;
			}
		}

		uiTick += 30;
	}
}
