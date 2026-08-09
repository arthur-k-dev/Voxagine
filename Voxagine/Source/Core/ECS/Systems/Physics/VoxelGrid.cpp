#include "pch.h"
#include "VoxelGrid.h"
#include "External/optick/optick.h"
#include <algorithm>

VoxelGrid::VoxelGrid()
{
	m_uiDimensionX = 0;
	m_uiDimensionY = 0;
	m_uiDimensionZ = 0;
	m_uiNumVoxels = 0;
	m_uiNumChunkX = 0;
	m_uiNumChunkY = 0;

	m_uiVoxelSize = 1;
	m_fInvVoxelSize = 1.f / static_cast<float>(m_uiVoxelSize);
	m_InvChunkSize = Vector3(0.f);
	m_worldOffset = Vector3(0.f, 0.f, 0.f);
	m_chunkSize = UVector3(0, 0, 0);
}

VoxelGrid::VoxelGrid(uint32_t uiVoxelSize)
{
	m_uiDimensionX = 0;
	m_uiDimensionY = 0;
	m_uiDimensionZ = 0;
	m_uiNumVoxels = 0;
	m_uiNumChunkX = 0;
	m_uiNumChunkY = 0;

	m_uiVoxelSize = uiVoxelSize;
	m_fInvVoxelSize = 1.f / static_cast<float>(uiVoxelSize);
	m_InvChunkSize = Vector3(0.f);
	m_worldOffset = Vector3(0.f, 0.f, 0.f);
	m_chunkSize = UVector3(0, 0, 0);
}

VoxelGrid::~VoxelGrid()
{}

void VoxelGrid::Create(uint32_t uiDimX, uint32_t uiDimY, uint32_t uiDimZ, uint32_t uiVoxelSize, UVector3 chunkSize)
{
	m_uiVoxelSize = uiVoxelSize;
	m_chunkSize = chunkSize;
	m_fInvVoxelSize = 1.f / static_cast<float>(uiVoxelSize);
	m_InvChunkSize = 1.f / (Vector3)chunkSize;
	Create(uiDimX, uiDimY, uiDimZ);
}

void VoxelGrid::Create(uint32_t uiDimX, uint32_t uiDimY, uint32_t uiDimZ)
{
	m_uiDimensionX = uiDimX;
	m_uiDimensionY = uiDimY;
	m_uiDimensionZ = uiDimZ;

	m_uiNumVoxels = uiDimX * uiDimY * uiDimZ;
	m_uiNumChunkX = uiDimX / m_chunkSize.x;
	m_uiNumChunkY = uiDimZ / m_chunkSize.z;

	m_ChunkVolumes.resize(m_uiNumChunkX * m_uiNumChunkY);
	m_ChunkOwners.assign(m_uiNumChunkX * m_uiNumChunkY, nullptr);
}

void VoxelGrid::Clear()
{
	m_ChunkVolumes.clear();
	m_ChunkOwners.clear();

	m_EntityToSlot.clear();
	m_SlotToEntity.clear();

	m_uiDimensionX = 0;
	m_uiDimensionY = 0;
	m_uiDimensionZ = 0;
	m_uiNumVoxels = 0;
}

void VoxelGrid::SetChunkStorage(UVector2 loc, std::vector<Voxel>* pVoxels, VoxelOwnerVolume* pOwners)
{
	const uint32_t uiIndex = loc.x + loc.y * m_uiNumChunkX;

	if (uiIndex >= m_ChunkVolumes.size())
		return;

	m_ChunkVolumes[uiIndex] = pVoxels;
	m_ChunkOwners[uiIndex] = pOwners;
}

uint32_t VoxelGrid::DetachChunkStorage(const std::vector<Voxel>* pVoxels)
{
	if (pVoxels == nullptr)
		return 0;

	uint32_t uiDetached = 0;

	for (size_t i = 0; i < m_ChunkVolumes.size(); ++i)
	{
		if (m_ChunkVolumes[i] != pVoxels)
			continue;

		m_ChunkVolumes[i] = nullptr;

		if (i < m_ChunkOwners.size())
			m_ChunkOwners[i] = nullptr;

		++uiDetached;
	}

	return uiDetached;
}

uint64_t VoxelGrid::Hash() const
{
	uint64_t uiHash = 1469598103934665603ull;

	auto fold = [&uiHash](uint64_t uiValue)
	{
		for (uint32_t uiByte = 0; uiByte < 8; ++uiByte)
		{
			uiHash ^= (uiValue >> (uiByte * 8)) & 0xFFull;
			uiHash *= 1099511628211ull;
		}
	};

	fold(m_uiDimensionX);
	fold(m_uiDimensionY);
	fold(m_uiDimensionZ);

	for (uint32_t uiZ = 0; uiZ < m_uiDimensionZ; ++uiZ)
	{
		for (uint32_t uiY = 0; uiY < m_uiDimensionY; ++uiY)
		{
			for (uint32_t uiX = 0; uiX < m_uiDimensionX; ++uiX)
			{
				uint32_t uiChunk = 0;
				uint32_t uiIndex = 0;

				if (!ResolveIndex(uiX, uiY, uiZ, uiChunk, uiIndex) ||
					uiChunk >= m_ChunkVolumes.size() ||
					m_ChunkVolumes[uiChunk] == nullptr ||
					uiIndex >= m_ChunkVolumes[uiChunk]->size())
				{
					/* Not resident. Folded rather than skipped - see the header. */
					fold(0xD15EA5Eull);
					continue;
				}

				fold((*m_ChunkVolumes[uiChunk])[uiIndex].Color);

				const VoxelOwnerVolume* pOwners =
					uiChunk < m_ChunkOwners.size() ? m_ChunkOwners[uiChunk] : nullptr;

				fold(pOwners && uiIndex < pOwners->Size()
					? pOwners->GetSlot(uiIndex)
					: VoxelOwnerVolume::k_uiNoOwnerSlot);
			}
		}
	}

	return uiHash;
}

uint16_t VoxelGrid::AcquireOwnerSlot(uint64_t uiEntityID)
{
	if (!uiEntityID)
		return VoxelOwnerVolume::k_uiNoOwnerSlot;

	if (m_SlotToEntity.empty())
		m_SlotToEntity.push_back(0); /* slot 0 is "no owner" */

	std::unordered_map<uint64_t, uint16_t>::const_iterator it = m_EntityToSlot.find(uiEntityID);

	if (it != m_EntityToSlot.end())
		return it->second;

	/* k_uiReservedSlot is reserved, so the last usable slot is one below it.
	   Exhausting this means 65533 distinct static owners in one world; the
	   largest level here uses 117. Report it rather than wrap, and treat the
	   overflow as unowned - which is what the field held before phase 4d gave
	   it a slot at all. */
	if (m_SlotToEntity.size() >= VoxelOwnerVolume::k_uiReservedSlot)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[voxel-grid] out of owner slots (%zu); further static renderers will bake as unowned\n",
			        m_SlotToEntity.size());
		}

		return VoxelOwnerVolume::k_uiNoOwnerSlot;
	}

	const uint16_t uiSlot = static_cast<uint16_t>(m_SlotToEntity.size());

	m_SlotToEntity.push_back(uiEntityID);
	m_EntityToSlot.emplace(uiEntityID, uiSlot);

	return uiSlot;
}

bool VoxelGrid::GetChunk(Voxel** chunk, Vector3 chunkOrigin, Vector3 dimensions, bool bAllowOutBounds /*= false*/, uint16_t* pOwnerSlots /*= nullptr*/)
{
	OPTICK_EVENT();
	uint32_t dimX = static_cast<uint32_t>(dimensions.x);
	uint32_t dimY = static_cast<uint32_t>(dimensions.y);
	uint32_t dimZ = static_cast<uint32_t>(dimensions.z);

	int originX = static_cast<int>(chunkOrigin.x);
	int originY = static_cast<int>(chunkOrigin.y);
	int originZ = static_cast<int>(chunkOrigin.z);

	/* Set voxels to nullptr on default */
	for (uint32_t i = 0; i < dimX * dimY * dimZ; ++i)
		chunk[i] = nullptr;

	if (pOwnerSlots)
	{
		for (uint32_t i = 0; i < dimX * dimY * dimZ; ++i)
			pOwnerSlots[i] = VoxelOwnerVolume::k_uiNoOwnerSlot;
	}

	/* Return if some of the chunk is out of bounds */
	if (!bAllowOutBounds && (originX + dimX > m_uiDimensionX ||
		originY + dimY > m_uiDimensionY ||
		originZ + dimZ > m_uiDimensionZ ||
		originX < 0 || originY < 0 || originZ < 0))
		return false;

	/* Return if chunk is fully out of bounds */
	if ((originX + (int)dimX < 0 && originY + (int)dimY < 0 && originZ + (int)dimZ < 0) ||
		originX > (int)m_uiDimensionX || originY > (int)m_uiDimensionY || originZ > (int)m_uiDimensionZ)
		return false;

	/* One implementation, walked in runs.
	 *
	 * This used to be two near-duplicate three-deep loops selected by an
	 * alignment test - "does this box fit inside a single chunk in x and z" -
	 * and they did not agree. The fast one clamped the *read* start to x = 0
	 * for a negative origin but kept writing from index 0 of the output, so a
	 * box straddling the left edge filled the wrong output cells; the general
	 * one resolved the chunk per voxel, three integer divides deep, which is
	 * what the fast path existed to avoid (ledger D10).
	 *
	 * A run keeps both properties without the duplication. Chunk residency and
	 * the chunk-local base index only change when x crosses a chunk boundary,
	 * so resolve at the start of each run and then walk pointers - which is the
	 * fast path's behaviour, applied to boxes of any shape and position.
	 */
	for (uint32_t z = 0; z < dimZ; ++z)
	{
		const int worldZ = originZ + static_cast<int>(z);

		if (worldZ < 0 || worldZ >= static_cast<int>(m_uiDimensionZ))
			continue;

		for (uint32_t y = 0; y < dimY; ++y)
		{
			const int worldY = originY + static_cast<int>(y);

			if (worldY < 0 || worldY >= static_cast<int>(m_uiDimensionY))
				continue;

			uint32_t x = 0;

			while (x < dimX)
			{
				const int worldX = originX + static_cast<int>(x);

				if (worldX < 0)
				{
					/* Skip straight to the first in-bounds column rather than
					   testing every one of them. */
					x += static_cast<uint32_t>(-worldX);
					continue;
				}

				if (worldX >= static_cast<int>(m_uiDimensionX))
					break;

				uint32_t uiChunk = 0;
				uint32_t uiIndex = 0;

				if (!ResolveIndex(static_cast<uint32_t>(worldX), static_cast<uint32_t>(worldY),
				                  static_cast<uint32_t>(worldZ), uiChunk, uiIndex))
				{
					/* Out of bounds is impossible here, so this is a chunk that
					   is not resident. Its whole x span reads as absent. */
					++x;
					continue;
				}

				/* How far this run can go: to the end of the requested box, the
				   end of the world, or the end of this chunk in x. */
				const uint32_t uiChunkEndX =
					(static_cast<uint32_t>(worldX) / m_chunkSize.x + 1) * m_chunkSize.x;

				uint32_t uiRun = std::min(dimX - x, uiChunkEndX - static_cast<uint32_t>(worldX));
				uiRun = std::min(uiRun, m_uiDimensionX - static_cast<uint32_t>(worldX));

				std::vector<Voxel>& volume = *m_ChunkVolumes[uiChunk];
				const VoxelOwnerVolume* pOwners =
					pOwnerSlots && uiChunk < m_ChunkOwners.size() ? m_ChunkOwners[uiChunk] : nullptr;

				for (uint32_t i = 0; i < uiRun; ++i)
				{
					const uint32_t uiVoxelIndex = uiIndex + i;

					if (uiVoxelIndex >= volume.size())
						break;

					const uint32_t chunkPos = (x + i) + y * dimX + dimX * dimY * z;

					chunk[chunkPos] = &volume[uiVoxelIndex];

					if (pOwners && uiVoxelIndex < pOwners->Size())
						pOwnerSlots[chunkPos] = pOwners->GetSlot(uiVoxelIndex);
				}

				x += uiRun;
			}
		}
	}

	return true;
}

Vector3 VoxelGrid::WorldToGridRound(Vector3 worldPos, bool bAllowOutBounds /*= false*/) const
{
	Vector3 gridPos = (worldPos - m_worldOffset) * m_fInvVoxelSize;
	gridPos.x = round(gridPos.x);
	gridPos.y = round(gridPos.y);
	gridPos.z = round(gridPos.z);

	if (bAllowOutBounds ||
		(gridPos.x > 0 && gridPos.x < m_uiDimensionX &&
			gridPos.y > 0 && gridPos.y < m_uiDimensionY &&
			gridPos.z > 0 && gridPos.z < m_uiDimensionZ)
		)
		return gridPos;

	return Vector3(-1, -1, -1);
}

Vector3 VoxelGrid::WorldToGrid(Vector3 worldPos, bool bAllowOutBounds /*= false*/) const
{
	Vector3 gridPos = (worldPos - m_worldOffset) * m_fInvVoxelSize;
	gridPos.x = floor(gridPos.x);
	gridPos.y = floor(gridPos.y);
	gridPos.z = floor(gridPos.z);

	if (bAllowOutBounds || 
		(gridPos.x > 0 && gridPos.x < m_uiDimensionX &&
		gridPos.y > 0 && gridPos.y < m_uiDimensionY &&
		gridPos.z > 0 && gridPos.z < m_uiDimensionZ)
	)
		return gridPos;
		
	return Vector3(-1, -1, -1);
}

Vector3 VoxelGrid::GridToWorld(Vector3 gridPos) const
{
	return gridPos * static_cast<float>(m_uiVoxelSize) + m_worldOffset;
}

Vector3 VoxelGrid::GridToWorld(uint32_t volumeId) const
{
	float fInvXY = 1.0f / (float)(m_uiDimensionX * m_uiDimensionY);
	float fInvX = 1.0f / (float)m_uiDimensionX;

	Vector3 gridPos;
	
	gridPos.z = static_cast<float>((int)(volumeId * fInvXY));
	gridPos.y = static_cast<float>((int)((volumeId - (gridPos.z * m_uiDimensionX * m_uiDimensionY)) * fInvX));
	gridPos.x = static_cast<float>(volumeId - gridPos.z * m_uiDimensionX *m_uiDimensionY - gridPos.y * m_uiDimensionX);

	return gridPos * static_cast<float>(m_uiVoxelSize) + m_worldOffset;
}

void VoxelGrid::GetDimensions(uint32_t& uiX, uint32_t& uiY, uint32_t& uiZ) const
{
	uiX = m_uiDimensionX;
	uiY = m_uiDimensionY;
	uiZ = m_uiDimensionZ;
}

UVector3 VoxelGrid::GetDimensions() const
{
	return UVector3(m_uiDimensionX, m_uiDimensionY, m_uiDimensionZ);
}