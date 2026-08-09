#include "pch.h"
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"

#include "Core/ECS/Systems/Physics/VoxelGrid.h"

#include <algorithm>
#include <cmath>

namespace
{
	/* The ground test, in one place. A voxel resting directly on the ground
	   layer supports everything above it; y = 0 is the ground plane itself,
	   which post processing composites analytically and destruction never
	   touches. */
	inline bool IsGrounded(const Vector3& v3Position)
	{
		return v3Position.y - 1.f == 0.f;
	}
}

uint64_t IntegrityChecker::PositionToHash(const Vector3& v3Position)
{
	if (!std::isfinite(v3Position.x) || !std::isfinite(v3Position.y) || !std::isfinite(v3Position.z))
		return k_uiInvalidHash;

	if (v3Position.x < 0.f || v3Position.y < 0.f || v3Position.z < 0.f)
		return k_uiInvalidHash;

	if (v3Position.x > 65535.f || v3Position.y > 65535.f || v3Position.z > 65535.f)
		return k_uiInvalidHash;

	return PositionToHash(
		static_cast<uint32_t>(v3Position.x),
		static_cast<uint32_t>(v3Position.y),
		static_cast<uint32_t>(v3Position.z));
}

Vector3 IntegrityChecker::HashToPosition(uint64_t uiHash)
{
	Vector3 v3Position;
	v3Position.x = (uint16_t)(uiHash >> 32);
	v3Position.y = (uint16_t)(uiHash >> 16);
	v3Position.z = (uint16_t)(uiHash);
	return v3Position;
}

void IntegrityChecker::EnqueueBulk(std::vector<uint64_t>& checks)
{
	if (checks.empty())
		return;

	m_Stats.uiSeedsOffered += checks.size();

	for (uint64_t uiHash : checks)
	{
		/* A seed only reaches the invalid value by naming a position outside
		   the representable range, which the callers no longer produce - but
		   silently walking whatever 0xFFFF,0xFFFF,0xFFFF happens to be is
		   exactly the D11 defect. */
		if (uiHash == k_uiInvalidHash)
		{
			++m_Stats.uiSeedsDeduplicated;
			continue;
		}

		/* Against everything queued, not just against this batch. */
		if (!m_PendingSet.insert(uiHash).second)
		{
			++m_Stats.uiSeedsDeduplicated;
			continue;
		}

		m_Pending.push_back(uiHash);
	}
}

void IntegrityChecker::Reset()
{
	m_Pending.clear();
	m_PendingSet.clear();
	m_CheckStack.clear();
	m_CheckedVoxels.clear();
	m_bCheckActive = false;

	Invalidate();
}

void IntegrityChecker::Invalidate()
{
	m_GroundedMemo.clear();
	m_IslandMemo.clear();

	if (m_pVoxelGrid != nullptr)
		m_uiMemoGeneration = m_pVoxelGrid->GetWriteGeneration();
}

void IntegrityChecker::RefreshMemo()
{
	if (m_pVoxelGrid == nullptr)
		return;

	const uint64_t uiGeneration = m_pVoxelGrid->GetWriteGeneration();

	if (uiGeneration == m_uiMemoGeneration)
		return;

	m_GroundedMemo.clear();
	m_IslandMemo.clear();

	m_uiMemoGeneration = uiGeneration;
}

void IntegrityChecker::EndCheck(bool bReport, std::vector<std::vector<uint64_t>>& o_results)
{
	if (!m_CheckedVoxels.empty())
	{
		/* Both answers are worth remembering, and remembering the *grounded*
		   one is the phase 4 change: the walk used to throw it away, so the
		   next seed on the same structure flooded all of it again. */
		std::unordered_set<uint64_t>& memo = bReport ? m_IslandMemo : m_GroundedMemo;

		memo.insert(m_CheckedVoxels.begin(), m_CheckedVoxels.end());
	}

	if (bReport && !m_CheckedVoxels.empty())
	{
		std::vector<uint64_t> checkedPositions;
		checkedPositions.reserve(m_CheckedVoxels.size());

		for (uint64_t uiHash : m_CheckedVoxels)
			checkedPositions.push_back(uiHash);

		std::sort(checkedPositions.begin(), checkedPositions.end());
		o_results.push_back(std::move(checkedPositions));

		++m_Stats.uiIslandsEmitted;
	}

	m_CheckStack.clear();
	m_CheckedVoxels.clear();
	m_bCheckActive = false;
}

void IntegrityChecker::Process(uint32_t uiVisitBudget, std::vector<std::vector<uint64_t>>& o_results)
{
	if (m_pVoxelGrid == nullptr)
		return;

	RefreshMemo();

	while (uiVisitBudget > 0)
	{
		if (!m_bCheckActive)
		{
			if (m_Pending.empty())
				return;

			const uint64_t uiSeed = m_Pending.front();
			m_Pending.pop_front();
			m_PendingSet.erase(uiSeed);

			/* Already classified this epoch. This is where the seeds an
			   explosion produces stop costing anything: the first one walks the
			   structure and the rest land on it and are answered from the memo.
			   An island seed is skipped too - that island has been emitted and
			   is about to be converted. */
			if (m_GroundedMemo.count(uiSeed) != 0 || m_IslandMemo.count(uiSeed) != 0)
			{
				++m_Stats.uiSeedsSkippedByMemo;
				continue;
			}

			m_CheckStack.clear();
			m_CheckedVoxels.clear();
			m_CheckStack.push_back(HashToPosition(uiSeed));
			m_bCheckActive = true;
		}

		while (uiVisitBudget > 0 && !m_CheckStack.empty())
		{
			--uiVisitBudget;
			++m_Stats.uiVisits;

			const Vector3 v3Position = m_CheckStack.back();
			m_CheckStack.pop_back();

			const uint64_t uiHash = PositionToHash(v3Position);

			if (uiHash == k_uiInvalidHash)
				continue;

			if (m_CheckedVoxels.find(uiHash) != m_CheckedVoxels.end())
				continue;

			/* Meeting something already known grounded makes this walk
			   grounded, without visiting whatever holds it up. That is the
			   largest saving here: a seed on a standing building only ever
			   walks as far as the first classified voxel. */
			if (m_GroundedMemo.count(uiHash) != 0)
			{
				++m_Stats.uiMemoHits;
				EndCheck(false, o_results);
				break;
			}

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
			   none of it falls. What was collected is recorded as grounded
			   rather than discarded. */
			if (IsGrounded(v3Position))
			{
				m_CheckedVoxels.insert(uiHash);
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

bool IntegrityChecker::ClassifyExhaustive(const Vector3& v3Seed, std::vector<uint64_t>& o_island) const
{
	o_island.clear();

	if (m_pVoxelGrid == nullptr)
		return false;

	/* Deliberately a local copy of the pre-phase-4 walk: no memo, no budget, no
	   shared state. An oracle that reuses the machinery it is checking proves
	   nothing. */
	std::vector<Vector3> stack;
	std::unordered_set<uint64_t> visited;

	stack.push_back(v3Seed);

	while (!stack.empty())
	{
		const Vector3 v3Position = stack.back();
		stack.pop_back();

		const uint64_t uiHash = PositionToHash(v3Position);

		if (uiHash == k_uiInvalidHash || visited.find(uiHash) != visited.end())
			continue;

		const Voxel* pVoxel = m_pVoxelGrid->GetVoxel(
			(uint32_t)v3Position.x,
			(uint32_t)v3Position.y,
			(uint32_t)v3Position.z);

		if (pVoxel == nullptr || !pVoxel->IsActive())
			continue;

		if (IsGrounded(v3Position))
			return false;

		visited.insert(uiHash);

		for (int y = 1; y >= -1; --y)
		{
			for (int x = -1; x <= 1; ++x)
			{
				for (int z = -1; z <= 1; ++z)
				{
					if (x == 0 && y == 0 && z == 0) continue;

					stack.push_back(v3Position + Vector3(
						static_cast<float>(x),
						static_cast<float>(y),
						static_cast<float>(z)));
				}
			}
		}
	}

	if (visited.empty())
		return false;

	o_island.assign(visited.begin(), visited.end());
	std::sort(o_island.begin(), o_island.end());

	return true;
}
