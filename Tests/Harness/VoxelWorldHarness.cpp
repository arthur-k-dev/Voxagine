#include "Harness/VoxelWorldHarness.h"

#include <algorithm>

VoxelWorldHarness::VoxelWorldHarness(const UVector3& v3Size, const UVector3& v3ChunkSize) :
	m_v3Size(v3Size),
	m_v3ChunkSize(v3ChunkSize)
{
	m_Grid.Create(v3Size.x, v3Size.y, v3Size.z, 1, v3ChunkSize);

	const uint32_t uiChunksX = v3Size.x / v3ChunkSize.x;
	const uint32_t uiChunksZ = v3Size.z / v3ChunkSize.z;
	const size_t uiChunkVoxels = static_cast<size_t>(v3ChunkSize.x) * v3ChunkSize.y * v3ChunkSize.z;

	/* Sized up front and never resized, because the grid holds bare pointers
	   into these - which is the same reason ChunkSystem allocates a chunk's
	   volume once and the reason P7 is a live race in the real thing. */
	m_ChunkVoxels.resize(static_cast<size_t>(uiChunksX) * uiChunksZ);
	m_ChunkOwners.resize(static_cast<size_t>(uiChunksX) * uiChunksZ);

	for (uint32_t uiZ = 0; uiZ < uiChunksZ; ++uiZ)
	{
		for (uint32_t uiX = 0; uiX < uiChunksX; ++uiX)
		{
			const size_t uiIndex = uiX + static_cast<size_t>(uiZ) * uiChunksX;

			m_ChunkVoxels[uiIndex].assign(uiChunkVoxels, Voxel());
			m_ChunkOwners[uiIndex].Resize(uiChunkVoxels);

			m_Grid.SetChunkStorage(UVector2(uiX, uiZ), &m_ChunkVoxels[uiIndex], &m_ChunkOwners[uiIndex]);
		}
	}

	m_Words.assign(static_cast<size_t>(v3Size.x) * v3Size.y * v3Size.z, 0u);

	m_Bricks.Resize(v3Size);

	m_BrickMirrorFront.assign(m_Bricks.GetBrickCount(), 0u);
	m_BrickMirrorBack.assign(m_Bricks.GetBrickCount(), 0u);

	m_Bricks.SetBuffers(m_BrickMirrorFront.data(), m_BrickMirrorBack.data());
	m_Bricks.Flush();
}

void VoxelWorldHarness::Set(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiOwnerSlot)
{
	if (uiX >= m_v3Size.x || uiY >= m_v3Size.y || uiZ >= m_v3Size.z)
		return;

	const VoxelCell cell = m_Grid.GetCell(uiX, uiY, uiZ);

	if (!cell)
		return;

	cell.SetColor(uiColor);
	cell.SetSlot(uiOwnerSlot);

	const uint32_t uiID = VoxelID(uiX, uiY, uiZ);

	m_Words[uiID] = uiColor;
	m_Bricks.SetVoxel(uiID, (uiColor >> 24) != 0);
}

void VoxelWorldHarness::SetDynamic(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
{
	if (uiX >= m_v3Size.x || uiY >= m_v3Size.y || uiZ >= m_v3Size.z)
		return;

	/* Deliberately not touching the grid: this is VoxelBaker::Occupy's
	   non-static path, where the voxel exists in the mapping and nowhere else.
	   See the header. */
	const uint32_t uiID = VoxelID(uiX, uiY, uiZ);

	m_Words[uiID] = uiColor;
	m_Bricks.SetVoxel(uiID, (uiColor >> 24) != 0);
}

void VoxelWorldHarness::Clear(uint32_t uiX, uint32_t uiY, uint32_t uiZ)
{
	Set(uiX, uiY, uiZ, 0, VoxelOwnerVolume::k_uiNoOwnerSlot);
}

void VoxelWorldHarness::FillGround(uint32_t uiColor)
{
	for (uint32_t uiZ = 0; uiZ < m_v3Size.z; ++uiZ)
	{
		for (uint32_t uiX = 0; uiX < m_v3Size.x; ++uiX)
			Set(uiX, 0, uiZ, uiColor, VoxelOwnerVolume::k_uiNoOwnerSlot);
	}
}

void VoxelWorldHarness::FillBox(const UVector3& v3Min, const UVector3& v3Size, uint32_t uiColor, uint16_t uiOwnerSlot)
{
	for (uint32_t uiZ = v3Min.z; uiZ < v3Min.z + v3Size.z; ++uiZ)
	{
		for (uint32_t uiY = v3Min.y; uiY < v3Min.y + v3Size.y; ++uiY)
		{
			for (uint32_t uiX = v3Min.x; uiX < v3Min.x + v3Size.x; ++uiX)
				Set(uiX, uiY, uiZ, uiColor, uiOwnerSlot);
		}
	}
}

VoxelEditTarget VoxelWorldHarness::MakeEditTarget(ILooseVoxelSink* pSink)
{
	VoxelEditTarget target;

	target.pGrid = &m_Grid;
	target.pBricks = &m_Bricks;
	target.pWords = m_Words.data();
	target.uiWordCount = static_cast<uint32_t>(m_Words.size());
	target.v3WindowSize = m_v3Size;
	target.pLooseVoxels = pSink;

	return target;
}

uint64_t VoxelWorldHarness::Hash() const
{
	uint64_t uiHash = m_Grid.Hash();

	auto fold = [&uiHash](uint64_t uiValue)
	{
		for (uint32_t uiByte = 0; uiByte < 8; ++uiByte)
		{
			uiHash ^= (uiValue >> (uiByte * 8)) & 0xFFull;
			uiHash *= 1099511628211ull;
		}
	};

	for (uint32_t uiWord : m_Words)
		fold(uiWord);

	fold(m_Bricks.Hash(false));

	return uiHash;
}

uint64_t VoxelWorldHarness::CountOccupied() const
{
	uint64_t uiCount = 0;

	for (uint32_t uiWord : m_Words)
	{
		if ((uiWord >> 24) != 0)
			++uiCount;
	}

	return uiCount;
}

uint32_t VoxelWorldHarness::Validate() const
{
	return m_Bricks.Validate(false, m_Words.data());
}
