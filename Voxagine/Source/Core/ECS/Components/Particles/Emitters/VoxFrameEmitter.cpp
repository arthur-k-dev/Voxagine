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

		/* Where this emitter's colours have to come from, and it is not the
		   same answer for every renderer.
		 *
		 * VoxelBaker::Occupy writes the CPU voxel only for a *static*
		 * renderer; a dynamic one's voxels exist in the mapped voxel buffer
		 * and nowhere else (CLAUDE.md, "Dynamic renderers are invisible to the
		 * physics grid"). So reading the CPU voxel here - which is what
		 * DESTRUCTION_PLAN.md's D4 asked for, to keep an uncached PCIe read of
		 * VRAM off a per-particle path - produced a colour of zero for every
		 * dynamic renderer. This emitter is what a dying humanoid uses
		 * (ParticleCorpse), and every humanoid is dynamic, so *every* corpse
		 * came apart into black particles.
		 *
		 * Decided once, outside the loop, rather than per voxel: the static
		 * case is the bulk and stays off the mapping, and a dying character is
		 * a few thousand reads once. D4's point was never "never read the
		 * mapping" - it was "do not read it per voxel on a path that runs
		 * millions of times". */
		RenderSystem* pRenderSystem = m_pSystem->GetWorld()->GetRenderSystem();
		const bool bColourFromMapping = !m_pRenderer->GetOwner()->IsStatic();
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

				if (bColourFromMapping)
				{
					particleData.Color[uiStartId + i] = pRenderSystem->GetVoxel(bakePositionData[i]);
				}
				else
				{
					/* Static: the chunk holds the same colour in ordinary
					   cached memory, so there is no reason to pay for the
					   mapping (rule 1, ledger D4). */
					const Voxel* pVoxel = pGrid->GetVoxel(v3Grid.x, v3Grid.y, v3Grid.z);

					particleData.Color[uiStartId + i] = pVoxel ? pVoxel->Color : 0u;
				}
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