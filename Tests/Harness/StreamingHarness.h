#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Core/Application.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"
#include "Core/Math.h"
#include "Core/Platform/Rendering/VoxelBrickGrid.h"
#include "Core/Voxels/VoxelWindow.h"

class Camera;
class ChunkSystem;
class Entity;
class VoxelGrid;
class World;

/* A whole chunk-streaming world with no GPU, no window and no render context.
 * CHUNK_STREAMING_PLAN.md T1.
 *
 * `VoxelWorldHarness` gets away with a plain `std::vector<uint32_t>` for the
 * voxel mapping because the destruction write path only ever *writes* it
 * (rule 1). Streaming is the same, but it reached the mapping through
 * World -> Application -> Platform -> RenderContext, and a RenderContext needs
 * a device - so the state machine could not be driven at all. `IVoxelWindow`
 * is the seam that fixes that and `HarnessVoxelWindow` below is its other
 * implementation; everything else here is real:
 *
 *   - a real `World`, built with `PreLoad(false)` so it has no `RenderSystem`
 *   - a real `PhysicsSystem` and `VoxelGrid`, from the fixture's own dimensions
 *   - real `Chunk`s carrying real `RootEntities`, deserialized by the real
 *     `JsonSerializer` through the real RTTR registrations
 *   - the real `JobManager`, with its real worker threads
 *
 * **On determinism, and where this deviates from T1 as written.** T1 asked for
 * a synchronous inline job queue. `JobQueue` is a concrete class whose enqueue
 * is a template, so inlining it means an interface and a virtual on the engine's
 * hottest dispatch path - a bigger production change than the seam it would be
 * testing. What the tests actually need is that *the state machine* advances
 * only when the test says so, and that is what `Frame()` gives: the chunk
 * system's states are only ever entered from `FixedTick`/`Tick`, both of which
 * are called from here, and completion callbacks only ever run from
 * `ProcessFinishedJobs`, which is also called from here. A worker taking a
 * different number of microseconds changes how many `Frame()` calls a
 * transition needs; it changes nothing about what any of them do. Where a test
 * needs to be *at* a particular state it says so by watching a
 * `StreamingCounters` value rather than by counting frames.
 *
 * The fixture worlds are small, synthetic and checked in under `Tests/Fixtures`
 * rather than taken from the shipped levels: CI needs no content pack, and a
 * failure names a two-entity chunk instead of a thousand-entity one.
 */

/* The resident window, as two vectors. Mirrors what RenderContext does with its
   voxel Mapper and brick grid, including the part that is easy to get wrong -
   the brick grid, its mirrors and the words flip *together* or the occupancy
   describes the wrong buffer. */
class HarnessVoxelWindow : public IVoxelWindow
{
public:
	void Create(const UVector3& v3Size);

	uint32_t* GetFrontData() override { return m_Words[m_uiFront].data(); }
	uint32_t* GetBackData() override { return m_Words[m_uiFront ^ 1u].data(); }
	uint32_t GetWordCount() const override { return static_cast<uint32_t>(m_Words[0].size()); }
	VoxelBrickGrid& GetBrickGrid() override { return m_Bricks; }
	void Swap() override;

	/* How many times the window was published, from the window's own side.
	   A commit that forgot to swap and a swap outside a commit are different
	   defects and this is what tells them apart. */
	uint32_t SwapCount() const { return m_uiSwaps; }

	const std::vector<uint32_t>& FrontWords() const { return m_Words[m_uiFront]; }
	const std::vector<uint32_t>& BackWords() const { return m_Words[m_uiFront ^ 1u]; }

	/* Occupied words of the front buffer, counted from the words themselves
	   rather than from the brick counts, so a test can catch the two
	   disagreeing. */
	uint64_t CountOccupiedFront() const;

private:
	std::vector<uint32_t> m_Words[2];
	std::vector<uint32_t> m_BrickMirror[2];

	VoxelBrickGrid m_Bricks;
	uint32_t m_uiFront = 0;
	uint32_t m_uiSwaps = 0;
};

/* Replaces the whole budget set for as long as it is in scope. T2: wall clock is
   the right runtime behaviour and the wrong test behaviour, so every scenario
   states its budgets in *units* and gets the same number of slices on any
   machine - and `Units(1)` sweeps every resumption point rather than the ones a
   fast machine happens to land on. Scoped rather than set-and-forget because the
   budgets are process-global and the next check must not inherit them. */
class StreamingBudgetOverride
{
public:
	explicit StreamingBudgetOverride(const StreamingBudgets& budgets)
	{
		StreamingBudgets::Set(budgets);
	}

	~StreamingBudgetOverride() { StreamingBudgets::Reset(); }

	StreamingBudgetOverride(const StreamingBudgetOverride&) = delete;
	StreamingBudgetOverride& operator=(const StreamingBudgetOverride&) = delete;
};

class StreamingHarness
{
public:
	/* Names a file under Tests/Fixtures, without the extension. */
	explicit StreamingHarness(const std::string& sFixture);
	~StreamingHarness();

	World& GetWorld() { return *m_pWorld; }
	ChunkSystem& Chunks();
	VoxelGrid& Grid();
	Camera& MainCamera();

	HarnessVoxelWindow& Window() { return m_Window; }
	VoxelBrickGrid& Bricks() { return m_Window.GetBrickGrid(); }

	UVector2 ChunkSize() const { return m_ChunkSize; }
	UVector3 WindowSize() const { return m_v3WindowSize; }

	/* Where the camera is, which is the only input the chunk system has. */
	void PlaceCamera(const Vector3& v3Position);

	/* One simulated frame, in the order Application::Run runs them: completion
	   callbacks, the world's add/remove queues, then the chunk system's fixed
	   tick and display tick. */
	void Frame();

	/* Frames until nothing is streaming. False if it never settles, which is a
	   wedged state machine and always a failure. */
	bool Settle(uint32_t uiMaxFrames = 20000);

	/* Chunks currently holding voxel storage, i.e. resident. */
	uint32_t ResidentChunkCount() const;

	/* Named entities the world holds, for asserting that an incoming chunk's
	   roots were admitted and an outgoing chunk's were taken away. */
	uint32_t CountEntitiesNamed(const std::string& sPrefix) const;

	/* One entity by exact name, or null. Streaming deletes and rebuilds the
	   entity, so a test must re-ask after every transition rather than holding
	   the pointer across one (R4). */
	Entity* FindEntityNamed(const std::string& sName) const;

private:
	/* One Application per process, not per harness: PlayerPrefs asserts on a
	   second instance, and the job manager's worker pool is worth starting once.
	   Worlds are what a streaming test creates and throws away. */
	Application& m_Application;

	/* Outlives the chunk system that writes it - the world below is destroyed
	   first, and its render job may still be in flight when it is. */
	HarnessVoxelWindow m_Window;
	std::unique_ptr<World> m_pWorld;

	UVector2 m_ChunkSize = UVector2(0, 0);
	UVector3 m_v3WindowSize = UVector3(0, 0, 0);
};
