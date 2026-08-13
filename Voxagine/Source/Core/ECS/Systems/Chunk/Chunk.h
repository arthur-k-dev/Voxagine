#pragma once
#include <chrono>
#include <functional>
#include <vector>
#include "Core/Math.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include <External/rapidjson/document.h>
#include "Core/ECS/Systems/Chunk/ChunkUpdateGroup.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"

#define DEFAULT_CHUNK_SIZE UVector3(256, 128, 256)

using namespace rapidjson;

struct TextureReadData;
class JsonSerializer;
class JobManager;
class Chunk
{
public:
	friend class ChunkSystem;

	Chunk(Application* pApp, World* pWorld, UVector2 chunkIndex = Vector2(0, 0));
	Chunk(Application* pApp, World* pWorld, UVector2 chunkIndex, UVector3 chunkSize, Value& rootEntities);
	~Chunk();

	void Load(UVector2 gridTargetIndex);
	void LoadAsync(ChunkUpdateGroup::Item* pItem, std::function<void(ChunkUpdateGroup::Item*)> callback);

	/* The main-thread half of unloading, resumable: find this chunk's entities,
	   serialize a bounded number of *roots* back out to JSON and destroy them.
	   Returns true when there is nothing left to serialize.

	   **The E1 defense, and it is by construction rather than by discipline.**
	   The experiment snapshotted raw `Entity*` here and serialized them across
	   many ticks while gameplay ran and could destroy them; `IsDestroyed()` was
	   consulted only after a root had finished, so the serializer itself walked
	   freed memory. Two rules replace that, and together they mean **no
	   `Entity*` ever crosses a frame boundary** (rule R4):

	     - the chunk's entities are re-discovered at the top of every slice, so
	       an entity gameplay destroyed in between is simply not found;
	     - a root is serialized whole, inside one slice, so nothing is ever
	       half-written. Measured: the largest root hierarchy in any shipped
	       level is 68 nodes, a whole chunk's roots cost 1.10 ms, and
	       re-discovery is 0.04 ms - so this costs a fraction of what the
	       resumable-post-order-stack version would have cost to make safe.

	   Roots already written are remembered by id, so re-discovery cannot
	   serialize one twice - which matters because an entity whose deletion was
	   pushed to a neighbouring chunk is written here and *not* destroyed. */
	bool PrepareUnloadBatch(StreamingBudget::Scope& budget);

	/* Whether an encode is part-way through this chunk's volume. IsUnloading()
	   covers the whole unload; this is the half that owns a cursor. */
	bool IsEncoding() const { return m_bEncodePrepared; }

	void BeginVoxelEncoding();

	/* One bounded slice of the RLE encode, charged per run. True when the whole
	   volume is encoded, at which point the voxel storage is logically empty and
	   ready for ChunkSystem's storage pool. */
	bool EncodeVoxelBatch(StreamingBudget::Scope& budget);

	/* R5. Everything a partly-run unload left behind, back to its
	   post-construction value - so a cancelled group cannot leak a half-encoded
	   chunk or a half-serialized root list into the next one (ledger E2). A
	   cancelled encode is free rather than merely safe: EncodeVoxelBatch does not
	   touch m_VoxelData until it completes, so the chunk's voxels are still
	   exactly where they were and the chunk is simply still resident. */
	void ResetStreamingState();

	bool IsLoaded() const { return m_bIsLoaded; }
	bool IsLoading() const { return m_bIsLoading; }
	bool IsUnloading() const { return m_bIsUnloading; }
	bool IsFirstLoad() const { return m_bFirstLoad; }
	bool IsTargetLoaded() const { return m_bIsTargetLoaded; }
	void SetTargetLoaded(bool bLoaded) { m_bIsTargetLoaded = bLoaded; }

	void SetGridTarget(UVector2 gridTarget) { m_GridTargetIndex = gridTarget; }
	UVector2 GetGridTarget() const { return m_GridTargetIndex; }

	UVector2 GetChunkIndex() { return m_ChunkIndex; }
	std::vector<Voxel>& GetVoxelData() { return m_VoxelData; }
	VoxelOwnerVolume& GetOwnerVolume() { return m_OwnerVolume; }
	UVector3 GetChunkSize() const { return m_ChunkSize; }
	const std::vector<Value>& GetRootEntities() { return m_RootEntities; }

	void FindEntitiesInChunk(const std::vector<Entity*>& allEntities, std::vector<std::pair<Entity*, bool>>& foundEntities) const;
	void SetGroundPlane(const std::string& texturePath);
	void UpdateGroundPlane();

	void LoadEntities();
	void UpdateEntities();

	/* Encodes and decodes this chunk's voxels in place and reports how many
	   came back different - the acceptance test for the RLE format change in
	   RENDERING_PLAN.md phase 4d, which is otherwise only reachable by walking
	   far enough for a chunk to unload and come back. Returns the number of
	   diverging voxels, so zero is a pass. */
	uint64_t VerifyVoxelCodecRoundTrip();

private:
	// Compress voxel chunk data with RLE encoding
	void EncodeVoxels();

	// Decompress data with RLE decoding
	void DecodeVoxels();

	static inline bool SlotEqual(uint16_t uiSlot, uint16_t uiEncoded);

	void UpdateRenderer(Entity* pEntity, bool bFirstLoad);

	/* One root, whole: serialize it into m_RootEntities and destroy it if this
	   chunk owns its deletion. */
	void SaveAndDeleteEntity(Entity* pEntity, bool bDelete);
	void DeleteEntity(Entity* pEntity);

	//Start at grid target of 1024, 1024 which is an invalid grid location
	UVector2 m_GridTargetIndex = UVector2(0, 0);
	UVector3 m_ChunkSize = UVector3(0, 0, 0);
	UVector2 m_ChunkIndex = UVector2(0, 0);

	std::vector<Value> m_RootEntities;
	std::vector<unsigned char> m_pEncodedVoxelData;
	std::vector<Voxel> m_VoxelData;

	/* Sized and freed in lockstep with m_VoxelData - see RENDERING_PLAN.md
	   phase 4d. Two bytes a voxel beside the colour's four, where the two used
	   to be eight bytes and a bool inside it. */
	VoxelOwnerVolume m_OwnerVolume;

	bool m_bIsLoaded = false;
	bool m_bIsLoading = false;
	bool m_bIsUnloading = false;
	bool m_bFirstLoad = true;
	bool m_bIsTargetLoaded = false;
	bool m_bUpdateGroundPlane = false;

	/* --- resumable unload state (phase 2). Everything here is reset together by
	   ResetStreamingState, which asserts it left nothing behind. -------------- */

	bool m_bUnloadPrepared = false;
	bool m_bEncodePrepared = false;
	size_t m_uiEncodeCursor = 0;
	uint64_t m_uiEncodeOccupied = 0;
	std::chrono::steady_clock::time_point m_EncodeStart;

	/* Ids, not pointers - see PrepareUnloadBatch. */
	std::vector<uint64_t> m_SerializedUnloadIds;

	TextureReadData* m_pTextureReadData;
	Document m_CopyDoc;
	World* m_pWorld;
	JobManager* m_pJobManager;
	JsonSerializer* m_pJsonSerializer;
};