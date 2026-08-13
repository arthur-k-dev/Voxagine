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

/* Voxel ownership, which is what the old `UserPointer` held.
 *
 * A *static renderer* owns every voxel it stamped: 970 K voxels over ~117
 * distinct entities, written once per stamped voxel - 3.1 M writes at a world
 * load. That has to be a flat array indexed by voxel, so a slot is `uint16_t`
 * per voxel and the entity id it stands for lives in a per-world table
 * (VoxelGrid::AcquireOwnerSlot). Two bytes rather than eight, and the write is
 * still a single store.
 *
 * There used to be a second thing here: a *particle claim*, one voxel reserved
 * by a debris particle while it was in flight, stored as the reserved slot
 * 0xFFFF plus a sparse map from voxel index to `Particle*`. DESTRUCTION_PLAN.md
 * phase 3 deleted it. It existed to answer "is this cell still mine" during
 * flight and to reserve a landing cell, and it failed at both: claims were
 * dropped wholesale on chunk unload, silently lost to takeover with the bake
 * skipped, corrupted by window slides, never persisted by design, and the one
 * write meant to transfer ownership to baked debris had never worked - so baked
 * debris was already unowned and the loose-voxel registry already carried it.
 * Every consumer either had a cheaper answer (the occupancy bitmap for "is it
 * empty", impact-time resolution for landing conflicts) or was satisfied by
 * "particles own nothing".
 *
 * **0xFFFF stays reserved and unused** (rule 4). Encoded chunk data can be
 * older than the code, and slots are never recycled, so handing it out would
 * make old data name a live entity. */
class VoxelOwnerVolume
{
public:
	/* constexpr, not const: `assign` and `emplace` take these by reference, so
	   a plain in-class const would be odr-used with no definition. Release
	   inlined them away and linked; Debug did not. */
	static constexpr uint16_t k_uiNoOwnerSlot = 0;

	/* Never assigned. See the class comment. */
	static constexpr uint16_t k_uiReservedSlot = 0xFFFF;

	void Resize(size_t uiNumVoxels) { m_Slots.assign(uiNumVoxels, k_uiNoOwnerSlot); }

	void Release()
	{
		m_Slots.clear();
		m_Slots.shrink_to_fit();
	}

	/* Logical size only, keeping the allocation. What a chunk does when its
	   storage is going into ChunkSystem's reuse pool rather than back to the
	   allocator - see ChunkSystem::RecycleChunkStorage. */
	void Clear() { m_Slots.clear(); }

	/* Whether this volume still owns an allocation, which is how a pooled block
	   is told apart from a fresh one. */
	size_t Capacity() const { return m_Slots.capacity(); }

	size_t Size() const { return m_Slots.size(); }

	uint16_t GetSlot(uint32_t uiIndex) const { return m_Slots[uiIndex]; }
	void SetSlot(uint32_t uiIndex, uint16_t uiSlot) { m_Slots[uiIndex] = uiSlot; }

private:
	std::vector<uint16_t> m_Slots;
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

	/* Null-safe, and that is not defensive tidiness. A cell is null whenever
	   its chunk is out of bounds or not resident, and an island discovered
	   before a window slide is converted after it - so `cell.IsActive()`
	   without a preceding `if (cell)` is a null dereference that needs the
	   camera to move at the wrong moment to fire. It was written that way in
	   PhysicsSystem::ProcessIntegrityChecks (ledger D7). Answering "no" for a
	   cell that does not exist is right for every caller: a voxel that is not
	   resident is not active. */
	bool IsActive() const { return pVoxel != nullptr && pVoxel->IsActive(); }
	uint32_t GetColor() const { return pVoxel != nullptr ? pVoxel->Color : 0; }
	void SetColor(uint32_t uiColor) const { if (pVoxel != nullptr) pVoxel->Color = uiColor; }

	uint16_t GetSlot() const { return pOwners ? pOwners->GetSlot(uiIndex) : VoxelOwnerVolume::k_uiNoOwnerSlot; }
	bool HasOwner() const { return GetSlot() != VoxelOwnerVolume::k_uiNoOwnerSlot; }

	void SetSlot(uint16_t uiSlot) const { if (pOwners) pOwners->SetSlot(uiIndex, uiSlot); }
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
	owner, call SetOwnerSlot with the voxel's coordinates. */
	bool GetChunk(Voxel** chunk, Vector3 chunkOrigin, Vector3 dimensions, bool bAllowOutBounds = false, uint16_t* pOwnerSlots = nullptr);

	/* Colour only. There is deliberately no ModifyVoxel beside these any more:
	   it wrote a colour and nothing else - no mapped word, no occupancy bit, no
	   brick count, no owner - with no bounds check and no residency check, and
	   it was called with unclamped coordinates from three places. Every write
	   goes through VoxelEditBatch now (DESTRUCTION_PLAN.md phase 1). */
	inline const Voxel* GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ) const;
	inline Voxel* GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ);

	/* The voxel and its owner in one index computation. Use this wherever an
	   owner is read or written; GetVoxel stays for the colour-only callers. */
	inline VoxelCell GetCell(uint32_t iX, uint32_t iY, uint32_t iZ);

	inline void SetOwnerSlot(uint32_t iX, uint32_t iY, uint32_t iZ, uint16_t uiSlot);

	/* Slots are handed out per entity id and never recycled, so a slot is a
	   stable identity for the life of the world - which is what lets the combo
	   streak and the bake's "did I stamp this" test compare slots directly
	   instead of resolving them. ~117 are in use in the largest level; the
	   65534 available are a cap, not a budget. */
	uint16_t AcquireOwnerSlot(uint64_t uiEntityID);

	/* The slot an entity already holds, without handing out a new one. For the
	   readers - "is this voxel still mine?" - which have no business consuming
	   a slot for an entity that has never stamped anything. */
	uint16_t FindOwnerSlot(uint64_t uiEntityID) const
	{
		std::unordered_map<uint64_t, uint16_t>::const_iterator it = m_EntityToSlot.find(uiEntityID);
		return it == m_EntityToSlot.end() ? VoxelOwnerVolume::k_uiNoOwnerSlot : it->second;
	}

	/* The entity id behind a slot, or 0 for "no owner" and for the reserved
	   slot, which is never assigned but can still arrive from chunk data
	   encoded before phase 3. */
	uint64_t ResolveOwnerSlot(uint16_t uiSlot) const
	{
		return uiSlot == VoxelOwnerVolume::k_uiNoOwnerSlot ||
		       uiSlot == VoxelOwnerVolume::k_uiReservedSlot ||
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

	/* Point a chunk slot at storage, or at nothing when passed nulls.
	   ChunkSystem reaches the same state through the private SetChunkVolumeAt;
	   this is public because two other callers need it and neither is a friend:
	   the phase 0 harness, which builds a grid with no ChunkSystem at all, and
	   phase 1's unload ordering, which has to detach a chunk on the main thread
	   *before* the job that frees the storage runs. */
	void SetChunkStorage(UVector2 loc, std::vector<Voxel>* pVoxels, VoxelOwnerVolume* pOwners);

	/* Nulls every slot pointing at this storage, and returns how many it found.
	   The unload ordering fix (ledger P7): `Chunk::EncodeVoxels` runs on a job
	   thread and does `m_VoxelData.resize(0); shrink_to_fit();
	   m_OwnerVolume.Release()`, while the grid still points at both - so a
	   main-thread `GetCell` could read freed storage. Calling this on the main
	   thread *before* the unload job is enqueued means readers see "not
	   resident", which every accessor already handles, instead of a freed
	   vector.

	   By storage rather than by location on purpose: an unloading chunk's slot
	   may already have been re-pointed at a chunk that moved into it, and
	   nulling that slot would evict a live chunk. */
	uint32_t DetachChunkStorage(const std::vector<Voxel>* pVoxels);

	/* FNV-1a over every CPU voxel colour and owner slot in the window, in
	   canonical x/y/z order. DESTRUCTION_PLAN.md phase 0's refactor net: the
	   same script over the same build must produce the same value. Detached
	   chunks fold as a fixed sentinel rather than being skipped, so a residency
	   change is visible in the hash instead of silently shortening it. */
	uint64_t Hash() const;

	/* Bumped by every voxel write that goes through VoxelEditBatch.
	 *
	 * The integrity checker memoises what it has classified, and everything it
	 * believes stops being true the moment a voxel changes. Having each write
	 * site remember to invalidate is exactly the per-call-site obligation phase
	 * 1 spent its whole existence removing - and it failed immediately when
	 * tried: the gauntlet, which writes through the batch but is not
	 * PhysicsSystem, silently kept a stale memo and found 20 islands where it
	 * should have found 100. A counter the checker polls cannot be forgotten. */
	uint64_t GetWriteGeneration() const { return m_uiWriteGeneration; }
	void BumpWriteGeneration() { ++m_uiWriteGeneration; }

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

	uint64_t m_uiWriteGeneration = 1;

	Vector3 m_worldOffset;
	UVector3 m_chunkSize;
	uint32_t m_uiNumChunkY;
	uint32_t m_uiNumChunkX;

	inline void SetChunkVolumeAt(UVector2 loc, std::vector<Voxel>& voxelVolume, VoxelOwnerVolume& ownerVolume);

	/* The chunk-local index every accessor below computes, shared so the two
	   halves of a cell cannot be looked up at different places.

	   Returns false for out of bounds *and* for a chunk that is not resident,
	   which since phase 1 is an ordinary state rather than an impossible one:
	   ChunkSystem detaches a chunk's storage before the job that frees it runs,
	   so a slot legitimately holds null and every reader has to cope. */
	inline bool ResolveIndex(uint32_t iX, uint32_t iY, uint32_t iZ, uint32_t& uiChunk, uint32_t& uiIndex) const;
};

inline bool VoxelGrid::ResolveIndex(uint32_t iX, uint32_t iY, uint32_t iZ, uint32_t& uiChunk, uint32_t& uiIndex) const
{
	if (iX >= m_uiDimensionX || iY >= m_uiDimensionY || iZ >= m_uiDimensionZ)
		return false;

	uiChunk = ftoi_sse1((float)(iX) * m_InvChunkSize.x) + ftoi_sse1((float)(iZ) * m_InvChunkSize.z) * m_uiNumChunkX;

	if (uiChunk >= m_ChunkVolumes.size() || m_ChunkVolumes[uiChunk] == nullptr)
		return false;

	uint32_t chunkXOffset = iX - (uiChunk % m_uiNumChunkX * m_chunkSize.x);
	uint32_t chunkZOffset = iZ - (ftoi_sse1((float)uiChunk / m_uiNumChunkX) * m_chunkSize.z);

	uiIndex = chunkXOffset + iY * m_chunkSize.x + m_chunkSize.x * m_chunkSize.y * chunkZOffset;

	return uiIndex < m_ChunkVolumes[uiChunk]->size();
}

inline VoxelCell VoxelGrid::GetCell(uint32_t iX, uint32_t iY, uint32_t iZ)
{
	VoxelCell cell;

	uint32_t uiChunk = 0;
	uint32_t uiIndex = 0;

	if (!ResolveIndex(iX, iY, iZ, uiChunk, uiIndex))
		return cell;

	cell.pVoxel = &(*m_ChunkVolumes[uiChunk])[uiIndex];
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

inline const Voxel* VoxelGrid::GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ) const
{
	uint32_t uiChunk = 0;
	uint32_t uiIndex = 0;

	if (!ResolveIndex(iX, iY, iZ, uiChunk, uiIndex))
		return nullptr;

	return &(*m_ChunkVolumes[uiChunk])[uiIndex];
}

inline Voxel* VoxelGrid::GetVoxel(uint32_t iX, uint32_t iY, uint32_t iZ)
{
	uint32_t uiChunk = 0;
	uint32_t uiIndex = 0;

	if (!ResolveIndex(iX, iY, iZ, uiChunk, uiIndex))
		return nullptr;

	return &(*m_ChunkVolumes[uiChunk])[uiIndex];
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
