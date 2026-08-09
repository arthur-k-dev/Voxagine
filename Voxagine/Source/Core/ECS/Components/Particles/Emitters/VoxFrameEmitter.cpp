#include "pch.h"
#include "VoxFrameEmitter.h"
#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Rendering/RenderSystem.h"
#include "Core/ECS/Components/Particles/ParticleSystem.h"

void VoxFrameEmitter::Emit(float fDeltatime, ParticlePool& particleData, uint32_t uiStartId, uint32_t uiEndId)
{
	if (m_pRenderer == nullptr)
		return;

	if (m_pRenderer->m_BakeData.Positions)
	{
		const VoxelGrid* pGrid = m_pSystem->GetWorld()->GetVoxelGrid();
		uint32_t* bakePositionData = m_pRenderer->m_BakeData.Positions;
		uint32_t arrSize = m_pRenderer->m_BakeData.Size;

		for (uint32_t i = uiStartId; i < uiEndId; ++i)
		{
			particleData.Position[i] = Vector3(-1000000, -1000000, -1000000);
			particleData.Color[i] = VColor(0.f, 0.f, 0.f, 0.f);
			particleData.GridPosition[i] = particleData.Position[i];
		}

		for (uint32_t i = 0; i < arrSize; ++i)
		{
			if (bakePositionData[i] == UINT_MAX)
				continue;

			if (uiStartId + i < uiEndId)
			{
				const UVector3 v3Grid = pGrid->GetVoxelPosition(bakePositionData[i]);

				particleData.Position[uiStartId + i] = (Vector3)v3Grid + pGrid->GetWorldOffset();

				/* From the CPU voxel, not from RenderSystem::GetVoxel. That
				   reads the mapping, which is ReBAR host-visible memory, so it
				   is an uncached PCIe read of VRAM per particle (rule 1) - and
				   an emitter can spawn a model's worth of them in one frame.
				   The chunk holds the same colour in ordinary cached memory.
				   Ledger D4; the destruction path had the identical defect. */
				const Voxel* pVoxel = pGrid->GetVoxel(v3Grid.x, v3Grid.y, v3Grid.z);

				particleData.Color[uiStartId + i] = pVoxel ? pVoxel->Color : 0u;
				particleData.GridPosition[uiStartId + i] = particleData.Position[uiStartId + i];
			}
		}

		for (uint32_t i = uiStartId; i < uiEndId; ++i)
			particleData.Timer[i] = m_pSystem->GetParticleLifeTime();

		float minForceLen = glm::length(m_MinForce);
		float maxForceLen = glm::length(m_MaxForce);
		float angle = atan2(m_SplashDirection.x, m_SplashDirection.z);

		for (uint32_t i = uiStartId; i < uiEndId; ++i)
		{
			Vector3 sphereRand = Utils::SphericalRand(1.f, 0.f, PI, 1.f, std::cos(m_fArcAngle));
			Vector3 rotatedRand = glm::normalize(glm::rotate(glm::angleAxis(angle, Vector3(0, 1, 0)), sphereRand));
			Vector3 minForce = (glm::normalize(m_MinForce) + rotatedRand) * minForceLen;
			Vector3 maxForce = (glm::normalize(m_MaxForce) + rotatedRand) * maxForceLen;

			particleData.Velocity[i] = glm::linearRand(minForce, maxForce) * m_pSystem->GetParticleStartSpeed();
		}

		for (uint32_t i = uiStartId; i < uiEndId; ++i)
			particleData.SpawnParticle(i);
	}
}