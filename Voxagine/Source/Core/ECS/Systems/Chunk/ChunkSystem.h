#pragma once
#include "Core/ECS/ComponentSystem.h"
#include "Core/ECS/Systems/Chunk/ChunkUpdateGroup.h"

#define GRID_SIZE 3
#define GRID_CENTER_OFFSET 1

class Chunk;
class ChunkViewer;
class IVoxelWindow;
class ChunkSystem : public ComponentSystem
{
public:
	ChunkSystem(World* pWorld, std::unordered_map<uint32_t, Chunk*> chunks = std::unordered_map<uint32_t, Chunk*>(), UVector2 chunkSize = UVector2(256, 256), UVector2 worldSize = UVector2(256, 256));
	~ChunkSystem();

	virtual void Start() override;
	virtual bool CanProcessComponent(Component* pComponent) override;
	virtual void Tick(float fDeltaTime) override;
	virtual void FixedTick(const GameTimer& fixedTimer) override;
	virtual void PostTick(float fDeltaTime) override;

	void SetGroundPlane(const std::string& texturePath);

	UVector2 GetWorldSize() { return m_WorldSize; }
	UVector2 GetChunkSize() const { return m_ChunkSize; }
	const std::unordered_map<uint32_t, Chunk*>& GetChunks() { return m_Chunks; }

	/* Is any part of the resident window still moving? Today that is an
	   outstanding update group and nothing else. Later phases of
	   Docs/CHUNK_STREAMING_PLAN.md give it more to answer for - a far-field
	   build in progress, renderers whose stamps have not been baked yet - so
	   callers should ask this rather than the group list. */
	bool IsStreaming() const { return !m_UpdateGroups.empty(); }

	void SetCameraLoadOffset(Vector3 offset) { m_CameraLoadOffset = offset; }
	Vector3 GetCameraLoadOffset() const { return m_CameraLoadOffset; }

	/* The resident window this system builds into. Defaults to the render
	   context; a test replaces it with two plain vectors, which is the whole
	   reason the seam exists (CHUNK_STREAMING_PLAN.md T1, Core/Voxels/
	   VoxelWindow.h). Null where the process has no render context at all -
	   every use here checks, because that is also the state a backend that
	   failed to start leaves behind. */
	IVoxelWindow* GetVoxelWindow() const { return m_pVoxelWindow; }
	void SetVoxelWindow(IVoxelWindow* pWindow) { m_pVoxelWindow = pWindow; }

protected:
	virtual void OnComponentAdded(Component* pComponent) override;
	virtual void OnComponentDestroyed(Component* pComponent) override;

	Vector3 CalculateWorldOffset(Vector3 viewPosition);
	void UpdateChunks(IVector2 gridOffset, ChunkUpdateGroup& group, bool bAsync = true);

	void UpdateGroup(ChunkUpdateGroup& group);
	std::vector<ChunkUpdateGroup>::iterator RemoveUpdateGroup(const std::vector<ChunkUpdateGroup>::iterator& iter);

	/* The one main-thread transaction that publishes a window (R2): physics
	   volumes, grid targets, world offset, buffer swap and camera, in that
	   order, with nothing between them that can yield. Split out of
	   UpdateGroup so that there is exactly one place to read, one place to
	   assert about, and one place a future phase can add to. */
	void CommitWindow(ChunkUpdateGroup& group);

	/* bBackBuffer says which of the voxel mapper's two buffers viewPortData
	   points at. The occupancy bricks are per-buffer, so writing voxels into
	   one while updating the other's counts silently loses geometry a swap
	   later; a pointer alone does not carry that. */
	void RenderChunk(ChunkUpdateGroup::Item& updateItem, uint32_t* viewPortData, bool bBackBuffer);
	void ClearChunk(UVector2 gridTargetIndex);

	void OnChunkLoaded(ChunkUpdateGroup::Item* pUpdateItem);
	void OnChunkUnloaded(ChunkUpdateGroup::Item* pUpdateItem);

	void OnWorldResumed(World* pWorld);

private:
	VoxelGrid* m_pVoxelGrid;
	IVoxelWindow* m_pVoxelWindow = nullptr;
	std::unordered_map<uint32_t, Chunk*> m_Chunks;

	std::vector<ChunkUpdateGroup> m_UpdateGroups;

	UVector2 m_WorldSize;
	UVector2 m_ChunkSize;
	uint32_t m_uiNumChunkY;
	uint32_t m_uiNumChunkX;
	UVector2 m_ClampedCameraPosition;
	Vector3 m_CameraLoadOffset = Vector3(0);
};
