#pragma once
#include "Core/ECS/ComponentSystem.h"
#include "Core/ECS/Systems/Chunk/ChunkUpdateGroup.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"

#include <vector>

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

	/* Destroy every root every chunk staged and never admitted. Phase 14, and
	   World::Unload is the only caller: those roots are entities, their
	   components hold pointers to this world's systems, and until this existed
	   they were destroyed from ~Chunk - which is inside ~ChunkSystem, four
	   systems after the AudioSystem their AudioSources call into. It is called
	   with every system still alive, so the rule it restores is the general one:
	   no entity in a world is destroyed after any of that world's systems. */
	void ReleaseStagedEntities();

	UVector2 GetWorldSize() { return m_WorldSize; }
	UVector2 GetChunkSize() const { return m_ChunkSize; }
	const std::unordered_map<uint32_t, Chunk*>& GetChunks() { return m_Chunks; }

	/* Is any part of the resident window still moving? Today that is an
	   outstanding update group and nothing else. Later phases of
	   Docs/CHUNK_STREAMING_PLAN.md give it more to answer for - a far-field
	   build in progress, renderers whose stamps have not been baked yet - so
	   callers should ask this rather than the group list. */
	bool IsStreaming() const;

	/* R1: gameplay never ticks against a missing initial window. True once the
	   world's first 3x3 resident window is committed and its roots are admitted;
	   World::Tick and World::FixedTick hold every entity and every gameplay
	   system until it is.

	   It is inert today and that is deliberate. ChunkSystem::Start still builds
	   the initial window synchronously, so this is true the moment Start
	   returns - the gate exists so that phase 4 can make the initial window
	   stream through the same machine as every other one and put the loading
	   screen over the wait, instead of discovering at that point that gameplay
	   has been running against a world that is not there. That is precisely the
	   experiment's E12/E3/E7/E8: it made every step resumable and never made
	   anything wait for the result, then patched the consequences per manager.

	   True with no camera as well, because a world with no camera has no window
	   to wait for and holding it forever would be worse than any hitch. */
	bool IsInitialWindowReady() const { return m_bInitialWindowReady; }

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

	/* The second half of that transaction, and phase 12's fix.
	 *
	 * The worker builds the whole incoming window into the back buffer from the
	 * chunks' CPU voxels, and the main thread goes on writing voxels into the
	 * front buffer the entire time - destruction clearing them and debris
	 * baking itself in are the two paths that do it during play. The swap then
	 * publishes a buffer that predates every one of those writes: the CPU keeps
	 * them and the image loses them, which is a voxel that is solid and
	 * invisible (or, the other way round, destroyed terrain that comes back
	 * visually while collision stays correct).
	 *
	 * Every such write is journalled by VoxelBrickGrid (see BeginWriteJournal)
	 * and replayed here, out of the CPU voxel rather than out of the recorded
	 * colour: the CPU grid is authoritative for everything the mapping holds,
	 * so replaying it also heals a write the worker read half of.
	 *
	 * Takes the offset the journalled ids were addressed in - the window has
	 * already moved by the time this runs. */
	void RepublishJournalledWrites(const Vector3& v3PreviousOffset);

	/* Construct the incoming chunks' entity trees, detached from the world,
	   under StreamingBudgets::EntityStaging. Called opportunistically from
	   US_RENDERING while the worker builds the back buffer, and again from
	   US_ADMITTING_GAMEPLAY until it returns true. */
	bool StageIncomingEntities(ChunkUpdateGroup& group);

	/* bBackBuffer says which of the voxel mapper's two buffers viewPortData
	   points at. The occupancy bricks are per-buffer, so writing voxels into
	   one while updating the other's counts silently loses geometry a swap
	   later; a pointer alone does not carry that. */
	void RenderChunk(ChunkUpdateGroup::Item& updateItem, uint32_t* viewPortData, bool bBackBuffer);
	void ClearChunk(UVector2 gridTargetIndex);

	void OnChunkLoaded(ChunkUpdateGroup::Item* pUpdateItem);
	void OnChunkUnloaded(ChunkUpdateGroup::Item* pUpdateItem);

	/* The 48 MiB a resident chunk's voxels and owner slots occupy, moved from
	   the chunk that just left to the chunk that is arriving instead of going
	   back to the allocator and coming out of it again. Ledger E10, simplified:
	   one block size per world, a hard cap, no re-sorting. */
	void AcquireChunkStorage(Chunk& chunk);
	void RecycleChunkStorage(Chunk& chunk);

	static size_t ChunkVoxelCount(const Chunk& chunk);

	void OnWorldResumed(World* pWorld);

private:
	struct ChunkStorage
	{
		std::vector<Voxel> Voxels;
		VoxelOwnerVolume Owners;
	};

	/* Six: a straight slide turns over three chunks and a second group can be
	   queued behind the first, so six is the most that can be in the air at
	   once. Anything past that is a block the pool would hold indefinitely,
	   which is what E10 objects to - those go back to the allocator. */
	static constexpr size_t k_uiMaxPooledChunkStorage = 6;

	VoxelGrid* m_pVoxelGrid;
	IVoxelWindow* m_pVoxelWindow = nullptr;
	std::unordered_map<uint32_t, Chunk*> m_Chunks;

	std::vector<ChunkUpdateGroup> m_UpdateGroups;
	std::vector<ChunkStorage> m_ChunkStoragePool;

	UVector2 m_WorldSize;
	UVector2 m_ChunkSize;
	uint32_t m_uiNumChunkY;
	uint32_t m_uiNumChunkX;
	bool m_bInitialWindowReady = false;

	/* The initial group has admitted its roots. Not the same as the window
	   being ready: the roots are in the world but their geometry may still be
	   arriving, because phase 5 made the stamp budgeted too. Gameplay waits for
	   both - a player who starts walking before the river bed has been stamped
	   walks into a hole, which is exactly what the first run of phase 5 did. */
	bool m_bInitialRootsAdmitted = false;

	UVector2 m_ClampedCameraPosition;
	Vector3 m_CameraLoadOffset = Vector3(0);
};
