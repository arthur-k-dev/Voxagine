#pragma once
#include <vector>
#include <unordered_map>
#include "Core/Math.h"

/* RENDERING_PLAN.md phase 4d. The CPU voxel is the GPU word and nothing else:
 * one colour, occupancy defined as a non-zero alpha byte exactly the way every
 * shader and VoxelBrickGrid already define it (rule 3 - the byte is a
 * rendererState tag, not an opacity).
 *
 * It used to carry a `bool Active` and a `uintptr_t UserPointer` beside the
 * colour, which cost 16 bytes a voxel - 128 MiB for one resident chunk and
 * 1.15 GiB across the 3x3 window. `Active` was verified redundant over
 * 75,497,472 voxels (0 active-without-alpha, 0 alpha-without-active) and is
 * simply derived now. The owner moved into VoxelOwnerVolume below.
 *
 * Dropping `Active` alone would have saved nothing: `uintptr_t` forces
 * alignof 8, so 8 + 4 pads straight back to 16. The owner field was the whole
 * change. */
struct Voxel
{
	uint32_t Color = 0;

	bool IsActive() const { return (Color >> 24) != 0; }
};

static_assert(sizeof(Voxel) == 4, "The CPU voxel is the GPU word - see RENDERING_PLAN.md phase 4d");

/* Voxel ownership, which is what the old `UserPointer` held. Two unrelated
 * things shared that field and they are stored differently here because they
 * behave nothing alike:
 *
 * - A *static renderer* owns every voxel it stamped. That is 970 K voxels over
 *   ~117 distinct entities, and it is written once per stamped voxel - 3.1 M
 *   writes at a world load. It has to be a flat array indexed by voxel, so the
 *   slot array below is `uint16_t` per voxel and the id it stands for lives in
 *   a per-world table (VoxelGrid::AcquireOwnerSlot). Two bytes rather than
 *   eight, and the write is still a single store.
 * - A *particle* claims exactly one voxel while it is in flight. There are at
 *   most a pool's worth of those and they are nowhere near a hot bulk path, so
 *   they live in a sparse map under the reserved slot k_uiParticleSlot. The
 *   slot value is what keeps them off the stamp's fast path: it answers
 *   "is this owned, and is it mine" without ever touching the map. */
class VoxelOwnerVolume
{
public:
	/* constexpr, not const: `assign` and `emplace` take these by reference, so
	   a plain in-class const would be odr-used with no definition. Release
	   inlined them away and linked; Debug did not. */
	static constexpr uint16_t k_uiNoOwnerSlot = 0;
	static constexpr uint16_t k_uiParticleSlot = 0xFFFF;

	void Resize(size_t uiNumVoxels)
	{
		m_Slots.assign(uiNumVoxels, k_uiNoOwnerSlot);
		m_Particles.clear();
	}

	void Release()
	{
		m_Slots.clear();
		m_Slots.shrink_to_fit();
		m_Particles.clear();
	}

	size_t Size() const { return m_Slots.size(); }

	uint16_t GetSlot(uint32_t uiIndex) const { return m_Slots[uiIndex]; }

	/* Zero unless a particle holds this voxel, so a caller that only wants to
	   know "is this claim mine" never pays for the map. */
	uint64_t GetParticle(uint32_t uiIndex) const
	{
		if (m_Slots[uiIndex] != k_uiParticleSlot)
			return 0;

		std::unordered_map<uint32_t, uint64_t>::const_iterator it = m_Particles.find(uiIndex);
		return it == m_Particles.end() ? 0 : it->second;
	}

	void SetSlot(uint32_t uiIndex, uint16_t uiSlot)
	{
		uint16_t& uiCurrent = m_Slots[uiIndex];

		if (uiCurrent == uiSlot)
			return;

		if (uiCurrent == k_uiParticleSlot)
			m_Particles.erase(uiIndex);

		uiCurrent = uiSlot;
	}

	void SetParticle(uint32_t uiIndex, uint64_t uiParticle)
	{
		if (!uiParticle)
		{
			SetSlot(uiIndex, k_uiNoOwnerSlot);
			return;
		}

		m_Slots[uiIndex] = k_uiParticleSlot;
		m_Particles[uiIndex] = uiParticle;
	}

private:
	std::vector<uint16_t> m_Slots;
	std::unordered_map<uint32_t, uint64_t> m_Particles;
};

/* One voxel plus the ownership beside it, resolved once. Everything that has
 * to read or write an owner goes through this rather than through a bare
 * `Voxel*`, because the owner no longer lives in the voxel and the index maths
 * that finds it is the same maths GetVoxel already does - doing it twice is
 * the only real cost this split could have introduced. */
struct VoxelCell
{
	Voxel* pVoxel = nullptr;
	VoxelOwnerVolume* pOwners = nullptr;
	uint32_t uiIndex = 0;

	explicit operator bool() const { return pVoxel != nullptr; }

	bool IsActive() const { return pVoxel->IsActive(); }
	uint32_t GetColor() const { return pVoxel->Color; }
	void SetColor(uint32_t uiColor) const { pVoxel->Color = uiColor; }

	uint16_t GetSlot() const { return pOwners ? pOwners->GetSlot(uiIndex) : VoxelOwnerVolume::k_uiNoOwnerSlot; }
	bool HasOwner() const { return GetSlot() != VoxelOwnerVolume::k_uiNoOwnerSlot; }
	uint64_t GetParticleOwner() const { return pOwners ? pOwners->GetParticle(uiIndex) : 0; }

	void SetSlot(uint16_t uiSlot) const { if (pOwners) pOwners->SetSlot(uiIndex, uiSlot); }
	void SetParticleOwner(uint64_t uiParticle) const { if (pOwners) pOwners->SetParticle(uiIndex, uiParticle); }
	void ClearOwner() const { SetSlot(VoxelOwnerVolume::k_uiNoOwnerSlot); }
};

class VoxelGrid
{
	friend class ChunkSystem;

public:
	VoxelGrid();
	VoxelGrid(uint32_t uiVoxelSize);
	~VoxelGrid();

	void Create(uint32_t uiDimX, uint32_t uiDimY, uint32_t uiDimZ, uint32_t uiVoxelSize, UVector3 chunkSize);
	void Create(uint32_t uiDimX, uint32_t uiDimY, uint32_t uiDimZ);
	void Clear();

	void SetWorldOffset(Vector3 worldOffset) { m_worldOffset = worldOffset; }
	Vector3 GetWorldOffset() const { return m_worldOffset; };

	/*Gets chunk with given origin and dimension inside voxel volume
	Set AllowOutBounds to true if you want the chunk to be padded with empty voxels if they are not in the voxel volume, otherwise an empty chunk will be returned
	pOwnerSlots, when given, is filled in parallel with each voxel's owner slot - k_uiNoOwnerSlot where the voxel is null. It is a read-only identity: to *write* an
	owner, call SetOwnerSlot/SetParticleOwner with the voxel's coordinates. */
	bool GetChunk(Voxel** chunk, Vector3 chunkOrigin, Vector3 dimensions, bool bAllowOutBounds = false, uint16_t* pOwnerSlots = nullptr);

	inline void ModifyVoxel(int iX, int iY, int iZ, uint32_t uiColor);

	inline const Voxel* GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ) const;
	inline Voxel* GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ);

	/* The voxel and its owner in one index computation. Use this wherever an
	   owner is read or written; GetVoxel stays for the colour-only callers. */
	inline VoxelCell GetCell(uint32_t iX, uint32_t iY, uint32_t iZ);

	inline void SetOwnerSlot(uint32_t iX, uint32_t iY, uint32_t iZ, uint16_t uiSlot);
	inline void SetParticleOwner(uint32_t iX, uint32_t iY, uint32_t iZ, uint64_t uiParticle);

	/* Slots are handed out per entity id and never recycled, so a slot is a
	   stable identity for the life of the world - which is what lets the combo
	   streak and the bake's "did I stamp this" test compare slots directly
	   instead of resolving them. ~117 are in use in the largest level; the
	   65534 available are a cap, not a budget. */
	uint16_t AcquireOwnerSlot(uint64_t uiEntityID);

	/* The entity id behind a slot, or 0 for "no owner" and for a particle
	   claim - which is what FindEntity would have made of a particle pointer
	   anyway. */
	uint64_t ResolveOwnerSlot(uint16_t uiSlot) const
	{
		return uiSlot == VoxelOwnerVolume::k_uiNoOwnerSlot ||
		       uiSlot == VoxelOwnerVolume::k_uiParticleSlot ||
		       uiSlot >= m_SlotToEntity.size() ? 0 : m_SlotToEntity[uiSlot];
	}

	size_t GetOwnerSlotCount() const { return m_SlotToEntity.size(); }

	inline uint32_t GetVoxelID(Vector3 gridPos) const;
	inline uint32_t GetVoxelID(UVector3 gridPos) const;
	inline UVector3 GetVoxelPosition(uint32_t voxelID) const;
	inline bool IsOutOfBounds(Vector3 position) const;

	static inline UVector3 IndexToVector(uint32_t index, UVector3 worldSize);

	Vector3 WorldToGridRound(Vector3 worldPos, bool bAllowOutBounds = false) const;
	Vector3 WorldToGrid(Vector3 worldPos, bool bAllowOutBounds = false) const;
	Vector3 GridToWorld(Vector3 gridPos) const;
	Vector3 GridToWorld(uint32_t volumeId) const;

	void GetDimensions(uint32_t& uiX, uint32_t& uiY, uint32_t& uiZ) const;
	UVector3 GetDimensions() const;
	uint64_t GetNumVoxels() const { return m_uiNumVoxels; }
	uint32_t GetVoxelSize() const { return m_uiVoxelSize; }

private:
	std::vector<std::vector<Voxel>*> m_ChunkVolumes;

	/* Parallel to m_ChunkVolumes and set by the same ChunkSystem call, so a
	   chunk's ownership moves with its voxels on load, move and unload. */
	std::vector<VoxelOwnerVolume*> m_ChunkOwners;

	std::unordered_map<uint64_t, uint16_t> m_EntityToSlot;
	std::vector<uint64_t> m_SlotToEntity;

	uint32_t m_uiDimensionX, m_uiDimensionY, m_uiDimensionZ;
	uint32_t m_uiVoxelSize;
	uint64_t m_uiNumVoxels;
	float m_fInvVoxelSize;
	Vector3 m_InvChunkSize;

	Vector3 m_worldOffset;
	UVector3 m_chunkSize;
	uint32_t m_uiNumChunkY;
	uint32_t m_uiNumChunkX;

	inline void SetChunkVolumeAt(UVector2 loc, std::vector<Voxel>& voxelVolume, VoxelOwnerVolume& ownerVolume);

	/* The chunk-local index every accessor below computes, shared so the two
	   halves of a cell cannot be looked up at different places. */
	inline bool ResolveIndex(uint32_t iX, uint32_t iY, uint32_t iZ, uint32_t& uiChunk, uint32_t& uiIndex) const;
};

inline bool VoxelGrid::ResolveIndex(uint32_t iX, uint32_t iY, uint32_t iZ, uint32_t& uiChunk, uint32_t& uiIndex) const
{
	if (iX >= m_uiDimensionX || iY >= m_uiDimensionY || iZ >= m_uiDimensionZ)
		return false;

	uiChunk = ftoi_sse1((float)(iX) * m_InvChunkSize.x) + ftoi_sse1((float)(iZ) * m_InvChunkSize.z) * m_uiNumChunkX;

	uint32_t chunkXOffset = iX - (uiChunk % m_uiNumChunkX * m_chunkSize.x);
	uint32_t chunkZOffset = iZ - (ftoi_sse1((float)uiChunk / m_uiNumChunkX) * m_chunkSize.z);

	uiIndex = chunkXOffset + iY * m_chunkSize.x + m_chunkSize.x * m_chunkSize.y * chunkZOffset;

	return true;
}

inline VoxelCell VoxelGrid::GetCell(uint32_t iX, uint32_t iY, uint32_t iZ)
{
	VoxelCell cell;

	uint32_t uiChunk = 0;
	uint32_t uiIndex = 0;

	if (!ResolveIndex(iX, iY, iZ, uiChunk, uiIndex))
		return cell;

	cell.pVoxel = &m_ChunkVolumes[uiChunk]->at(uiIndex);
	cell.pOwners = uiChunk < m_ChunkOwners.size() ? m_ChunkOwners[uiChunk] : nullptr;
	cell.uiIndex = uiIndex;

	/* A volume that has not been sized yet - the ground-plane pass can run
	   before the owner array exists - has no owner to report. */
	if (cell.pOwners && cell.pOwners->Size() <= uiIndex)
		cell.pOwners = nullptr;

	return cell;
}

inline void VoxelGrid::SetOwnerSlot(uint32_t iX, uint32_t iY, uint32_t iZ, uint16_t uiSlot)
{
	GetCell(iX, iY, iZ).SetSlot(uiSlot);
}

inline void VoxelGrid::SetParticleOwner(uint32_t iX, uint32_t iY, uint32_t iZ, uint64_t uiParticle)
{
	GetCell(iX, iY, iZ).SetParticleOwner(uiParticle);
}

inline void VoxelGrid::ModifyVoxel(int iX, int iY, int iZ, uint32_t uiColor)
{
	uint32_t chunkArrPos = ftoi_sse1((float)(iX)* m_InvChunkSize.x) + ftoi_sse1((float)(iZ)* m_InvChunkSize.z) * m_uiNumChunkX;
	uint32_t chunkXOffset = iX - (chunkArrPos % m_uiNumChunkX * m_chunkSize.x);
	uint32_t chunkZOffset = iZ - (ftoi_sse1((float)chunkArrPos / m_uiNumChunkX) * m_chunkSize.z);
	uint32_t chunkGridPos = chunkXOffset + iY * m_chunkSize.x + m_chunkSize.x * m_chunkSize.y * chunkZOffset;

	m_ChunkVolumes[chunkArrPos]->at(chunkGridPos).Color = uiColor;
}

inline const Voxel* VoxelGrid::GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ) const
{
	if (iX >= m_uiDimensionX || iY >= m_uiDimensionY || iZ >= m_uiDimensionZ)
		return nullptr;

	uint32_t chunkArrPos = ftoi_sse1((float)(iX)* m_InvChunkSize.x) + ftoi_sse1((float)(iZ)* m_InvChunkSize.z) * m_uiNumChunkX;
	uint32_t chunkXOffset = iX - (chunkArrPos % m_uiNumChunkX * m_chunkSize.x);
	uint32_t chunkZOffset = iZ - (ftoi_sse1((float)chunkArrPos / m_uiNumChunkX) * m_chunkSize.z);
	uint32_t chunkGridPos = chunkXOffset + iY * m_chunkSize.x + m_chunkSize.x * m_chunkSize.y * chunkZOffset;
	return &m_ChunkVolumes[chunkArrPos]->at(chunkGridPos);
}

inline Voxel* VoxelGrid::GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ)
{
	if (iX >= m_uiDimensionX || iY >= m_uiDimensionY || iZ >= m_uiDimensionZ)
		return nullptr;

	uint32_t chunkArrPos = ftoi_sse1((float)(iX) * m_InvChunkSize.x) + ftoi_sse1((float)(iZ) * m_InvChunkSize.z) * m_uiNumChunkX;
	uint32_t chunkXOffset = iX - (chunkArrPos % m_uiNumChunkX * m_chunkSize.x);
	uint32_t chunkZOffset = iZ - (ftoi_sse1((float)chunkArrPos / m_uiNumChunkX) * m_chunkSize.z);
	uint32_t chunkGridPos = chunkXOffset + iY * m_chunkSize.x + m_chunkSize.x * m_chunkSize.y * chunkZOffset;
	return &m_ChunkVolumes[chunkArrPos]->at(chunkGridPos);
}

inline uint32_t VoxelGrid::GetVoxelID(Vector3 gridPos) const
{
	return static_cast<uint32_t>(gridPos.x + gridPos.y * m_uiDimensionX + m_uiDimensionX * m_uiDimensionY * gridPos.z);
}

inline uint32_t VoxelGrid::GetVoxelID(UVector3 gridPos) const 
{
	return static_cast<uint32_t>(gridPos.x + gridPos.y * m_uiDimensionX + m_uiDimensionX * m_uiDimensionY * gridPos.z);
}

inline UVector3 VoxelGrid::GetVoxelPosition(uint32_t voxelID) const
{
	return UVector3(
		voxelID % m_uiDimensionX,
		(voxelID / m_uiDimensionX) % m_uiDimensionY,
		voxelID / (m_uiDimensionX * m_uiDimensionY)
	);
}

inline void VoxelGrid::SetChunkVolumeAt(UVector2 loc, std::vector<Voxel>& voxelVolume, VoxelOwnerVolume& ownerVolume)
{
	uint32_t index = loc.x + loc.y * m_uiNumChunkX;
	if (index >= m_ChunkVolumes.size()) return;
	m_ChunkVolumes[index] = &voxelVolume;
	m_ChunkOwners[index] = &ownerVolume;
}

inline bool VoxelGrid::IsOutOfBounds(Vector3 position) const
{
	return position.x < 0 || position.x >= m_uiDimensionX ||
		position.y < 0 || position.y >= m_uiDimensionY ||
		position.z < 0 || position.z >= m_uiDimensionZ;
}

inline UVector3 VoxelGrid::IndexToVector(uint32_t index, UVector3 worldSize)
{
	return UVector3(
		index % worldSize.x,
		(index / worldSize.x) % worldSize.y,
		index / (worldSize.x * worldSize.y)
	);
}
