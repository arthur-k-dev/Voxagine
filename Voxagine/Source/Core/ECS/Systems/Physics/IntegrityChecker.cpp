#include "pch.h"
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"

#include "Core/ECS/Systems/Physics/VoxelGrid.h"

#include <algorithm>

uint64_t IntegrityChecker::PositionToHash(const Vector3& v3Position)
{
	uint64_t uiHash = 0;
	((uint16_t*)&uiHash)[0] = (uint16_t)v3Position.z;
	((uint16_t*)&uiHash)[1] = (uint16_t)v3Position.y;
	((uint16_t*)&uiHash)[2] = (uint16_t)v3Position.x;
	return uiHash;
}

Vector3 IntegrityChecker::HashToPosition(uint64_t uiHash)
{
	Vector3 v3Position;
	v3Position.x = ((uint16_t*)&uiHash)[2];
	v3Position.y = ((uint16_t*)&uiHash)[1];
	v3Position.z = ((uint16_t*)&uiHash)[0];
	return v3Position;
}

void IntegrityChecker::EnqueueBulk(std::vector<uint64_t>& checks)
{
	if (checks.empty())
		return;

	/* The job deduplicated on the worker as it dequeued; there is no worker to
	   defer it to now, and doing it here keeps duplicates out of the queue
	   rather than out of the walk. */
	std::sort(checks.begin(), checks.end());
	checks.erase(std::unique(checks.begin(), checks.end()), checks.end());

	m_Pending.insert(m_Pending.end(), checks.begin(), checks.end());
}

void IntegrityChecker::Reset()
{
	m_Pending.clear();
	m_CheckStack.clear();
	m_CheckedVoxels.clear();
	m_bCheckActive = false;
}

void IntegrityChecker::EndCheck(bool bReport, std::vector<std::vector<uint64_t>>& o_results)
{
	if (bReport && !m_CheckedVoxels.empty())
	{
		std::vector<uint64_t> checkedPositions;
		checkedPositions.reserve(m_CheckedVoxels.size());

		for (uint64_t uiHash : m_CheckedVoxels)
			checkedPositions.push_back(uiHash);

		std::sort(checkedPositions.begin(), checkedPositions.end());
		o_results.push_back(std::move(checkedPositions));
	}

	m_CheckStack.clear();
	m_CheckedVoxels.clear();
	m_bCheckActive = false;
}

void IntegrityChecker::Process(uint32_t uiVisitBudget, std::vector<std::vector<uint64_t>>& o_results)
{
	if (m_pVoxelGrid == nullptr)
		return;

	while (uiVisitBudget > 0)
	{
		if (!m_bCheckActive)
		{
			if (m_Pending.empty())
				return;

			const uint64_t uiSeed = m_Pending.front();
			m_Pending.pop_front();

			m_CheckStack.clear();
			m_CheckedVoxels.clear();
			m_CheckStack.push_back(HashToPosition(uiSeed));
			m_bCheckActive = true;
		}

		while (uiVisitBudget > 0 && !m_CheckStack.empty())
		{
			--uiVisitBudget;

			const Vector3 v3Position = m_CheckStack.back();
			m_CheckStack.pop_back();

			const uint64_t uiHash = PositionToHash(v3Position);
			if (m_CheckedVoxels.find(uiHash) != m_CheckedVoxels.end())
				continue;

			/* Out of bounds reads as inactive - GetVoxel resolves the chunk and
			   returns null rather than indexing past it. */
			const Voxel* pVoxel = m_pVoxelGrid->GetVoxel(
				(uint32_t)v3Position.x,
				(uint32_t)v3Position.y,
				(uint32_t)v3Position.z);

			const bool bIsVoxelActive = (pVoxel && pVoxel->IsActive());
			if (!bIsVoxelActive)
				continue;

			/* Reached the ground layer, so the whole island is supported and
			   none of it falls. Discard what was collected. */
			if (v3Position.y - 1 == 0)
			{
				EndCheck(false, o_results);
				break;
			}

			m_CheckedVoxels.insert(uiHash);

			for (int y = 1; y >= -1; --y)
			{
				for (int x = -1; x <= 1; ++x)
				{
					for (int z = -1; z <= 1; ++z)
					{
						if (x == 0 && y == 0 && z == 0) continue;

						m_CheckStack.push_back(v3Position + Vector3(
							static_cast<float>(x),
							static_cast<float>(y),
							static_cast<float>(z)));
					}
				}
			}
		}

		/* Ran the island to exhaustion without ever touching the ground, so
		   everything collected is unsupported. If the budget ran out first,
		   m_bCheckActive stays set and the next call resumes this same walk. */
		if (m_bCheckActive && m_CheckStack.empty())
			EndCheck(true, o_results);
	}
}
