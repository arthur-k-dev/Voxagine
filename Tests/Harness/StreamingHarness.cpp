#include "Harness/StreamingHarness.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <stdexcept>

#include <External/rapidjson/document.h>

#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/ECS/World.h"
#include "Core/GameTimer.h"

#ifndef VOXAGINE_TEST_FIXTURE_DIR
#define VOXAGINE_TEST_FIXTURE_DIR "Tests/Fixtures"
#endif

namespace
{
	/* ChunkSystem::FixedTick takes a GameTimer and never reads it. GameTimer is
	   abstract, so it needs a body; giving it one here rather than a fake clock
	   keeps the fact that the parameter is unused visible. */
	class UnusedGameTimer final : public GameTimer
	{
	public:
		const Time& GetCurrentSystemTime() const override { return m_Time; }
		void Update(const std::function<void()>&) override {}
	};

	const GameTimer& FixedTimer()
	{
		static UnusedGameTimer s_Timer;
		return s_Timer;
	}

	/* One per process. PlayerPrefs asserts if a second Application exists, and
	   the job manager's workers are worth starting once rather than once per
	   check. The holder is what joins them again at exit; a bare static
	   Application would leave the pool running through static destruction. */
	struct SharedApplication
	{
		Application App;

		SharedApplication() { App.GetJobManager().Initialize(); }
		~SharedApplication() { App.GetJobManager().Deinitialize(); }
	};

	Application& TestApplication()
	{
		static SharedApplication s_Shared;
		return s_Shared.App;
	}
}

void HarnessVoxelWindow::Create(const UVector3& v3Size)
{
	const size_t uiWords = static_cast<size_t>(v3Size.x) * v3Size.y * v3Size.z;

	m_Words[0].assign(uiWords, 0u);
	m_Words[1].assign(uiWords, 0u);

	m_Bricks.Resize(v3Size);

	/* Bricks only. The rest of the coverage pyramid reaches the GPU as a 3D
	   texture rather than as counts in this buffer (RENDERING_PLAN.md 7.1b
	   route B), and there is no texture to stage for here - SetDensityBuffers
	   is left null and every path in the grid checks for that. */
	m_BrickMirror[0].assign(m_Bricks.GetBrickCount(), 0u);
	m_BrickMirror[1].assign(m_Bricks.GetBrickCount(), 0u);

	m_Bricks.SetBuffers(m_BrickMirror[0].data(), m_BrickMirror[1].data());
	m_Bricks.Flush();
}

void HarnessVoxelWindow::Swap()
{
	m_uiFront ^= 1u;
	++m_uiSwaps;

	/* The brick grid flips with the words, and its mirrors follow the flip -
	   exactly what RenderContext's BufferSwapped subscriber does, and for the
	   same reason: a grid describing the pre-swap window describes the wrong
	   voxels. */
	m_Bricks.Swap();
	m_Bricks.SetBuffers(m_BrickMirror[m_uiFront].data(), m_BrickMirror[m_uiFront ^ 1u].data());
}

uint64_t HarnessVoxelWindow::CountOccupiedFront() const
{
	uint64_t uiOccupied = 0;

	for (uint32_t uiWord : m_Words[m_uiFront])
	{
		if ((uiWord >> 24) != 0)
			++uiOccupied;
	}

	return uiOccupied;
}

StreamingHarness::StreamingHarness(const std::string& sFixture, bool bInitialize) :
	m_Application(TestApplication())
{
	const std::string sPath = std::string(VOXAGINE_TEST_FIXTURE_DIR) + "/" + sFixture + ".wld";

	std::ifstream file(sPath, std::ios::binary);

	if (!file)
		throw std::runtime_error("streaming fixture not found: " + sPath);

	std::stringstream contents;
	contents << file.rdbuf();
	const std::string sJson = contents.str();

	rapidjson::Document doc;

	if (doc.Parse(sJson.c_str()).HasParseError())
		throw std::runtime_error("streaming fixture is not valid JSON: " + sPath);

	/* Real worker threads, started once for the process. See the class comment
	   on why this is not the inline queue T1 described, and why the difference
	   does not reach the assertions. */
	m_pWorld = std::make_unique<World>(&m_Application);

	/* The real deserializer, straight from the parsed document: this builds the
	   PhysicsSystem at the fixture's own grid size, the Chunks with their real
	   RootEntities, and the ChunkSystem over them. Deliberately not
	   DeserializeWorldFromFile - that route needs a FileSystem, and the
	   fixture is already in hand. */
	if (!m_Application.GetSerializer().DeserializeWorld(*m_pWorld, doc))
		throw std::runtime_error("streaming fixture failed to deserialize: " + sPath);

	/* No RenderSystem: there is no render context for it to resize a buffer
	   on. Everything the chunk system needs of the world survives that. */
	m_pWorld->PreLoad(false);

	VoxelGrid* pGrid = m_pWorld->GetPhysics()->GetVoxelGrid();
	m_v3WindowSize = pGrid->GetDimensions();

	m_Window.Create(m_v3WindowSize);

	ChunkSystem* pChunks = m_pWorld->GetChunkSystem();
	pChunks->SetVoxelWindow(&m_Window);

	/* The chunk grid, in voxels per chunk. GetWorldSize is the level; the
	   window is three chunks across wherever the level is bigger than one. */
	m_ChunkSize = UVector2(m_v3WindowSize.x / 3, m_v3WindowSize.z / 3);

	if (bInitialize)
		Initialize();
}

void StreamingHarness::Initialize(bool bSettleInitialWindow)
{
	if (m_bInitialized)
		return;

	m_bInitialized = true;

	/* World::Initialize creates the main camera and starts every system, which
	   is where ChunkSystem::Start loads the initial 3x3 window synchronously.
	   The window must already be attached by the constructor, or that first load
	   writes nothing. */
	m_pWorld->Initialize();
	m_pWorld->PreTick();

	/* The shipped levels' camera is a persistent `CameraMultiplayer`; the
	   default one World::Initialize creates is not, and a chunk unload
	   serializes and destroys every non-persistent root standing inside it - so
	   the window sliding over the camera deletes the thing that decides where
	   the window goes. That is a real defect and it is guarded now
	   (World::DeleteEntityFromLists nulls the world's pointer, ChunkSystem
	   checks it), but leaving it unpinned here would mean the streaming
	   scenarios were mostly measuring how long it takes to lose the camera.
	   Same call, and the same reason, as the GPU stress fixture's. */
	if (Camera* pCamera = m_pWorld->GetMainCamera())
		pCamera->SetPersistent(true);

	/* **The initial window arrives here rather than inside World::Initialize**,
	   as of chunk streaming phase 4: ChunkSystem::Start pushes an update group
	   like any other slide instead of loading nine chunks synchronously, and
	   gameplay is held (R1) until that group has admitted its roots. So the
	   harness drives it to completion, which restores the precondition every
	   check is written against - "you are handed a world whose first window is
	   resident" - without any of them having to know how it got there.

	   A check that wants to *observe* the hold constructs the harness with
	   bInitialize = false and calls Initialize(false) itself. */
	if (!bSettleInitialWindow)
		return;

	Settle();

	/* And the counters start at zero for the check rather than at whatever the
	   fixture's own startup cost. Before phase 4 the initial window was built
	   outside the state machine and contributed nothing to count; it is a
	   commit and nine chunk regions now, and every check that gates on
	   "commits == 1" means its own slide. */
	StreamingCounters::Reset();
	m_Window.ResetSwapCount();
}

StreamingHarness::~StreamingHarness()
{
	/* Unload discards this world's job queue, which cancels anything still
	   pending on it - an outstanding chunk job holds a pointer into a
	   ChunkUpdateGroup the world's ChunkSystem owns. The worker pool itself
	   belongs to the process and stays up. */
	m_pWorld->Unload();
	m_Application.GetJobManager().ProcessFinishedJobs();
}

ChunkSystem& StreamingHarness::Chunks()
{
	return *m_pWorld->GetChunkSystem();
}

VoxelGrid& StreamingHarness::Grid()
{
	return *m_pWorld->GetPhysics()->GetVoxelGrid();
}

Camera& StreamingHarness::MainCamera()
{
	return *m_pWorld->GetMainCamera();
}

void StreamingHarness::PlaceCamera(const Vector3& v3Position)
{
	MainCamera().GetTransform()->SetPosition(v3Position);
	MainCamera().GetTransform()->UpdateMatrix();
}

void StreamingHarness::Frame()
{
	m_Application.GetJobManager().ProcessFinishedJobs();

	m_pWorld->PreTick();

	if (m_bTickWorld)
	{
		/* The real thing, including R1's hold: World::Tick decides whether the
		   entities and the gameplay systems advance at all. */
		m_pWorld->FixedTick(FixedTimer());
		m_pWorld->Tick(1.f / 60.f);
	}
	else
	{
		Chunks().FixedTick(FixedTimer());
		Chunks().Tick(1.f / 60.f);
	}

	/* A real frame takes milliseconds and a JobThread that finds no work sleeps
	   for ten of them, so a test spinning this as fast as it can outruns the
	   worker pool by three orders of magnitude and reads as a wedged state
	   machine. Only while something is actually outstanding: an idle harness
	   should cost nothing. */
	if (Chunks().IsStreaming())
		std::this_thread::sleep_for(std::chrono::microseconds(200));
}

bool StreamingHarness::Settle(uint32_t uiMaxFrames)
{
	for (uint32_t uiFrame = 0; uiFrame < uiMaxFrames; ++uiFrame)
	{
		Frame();

		if (!Chunks().IsStreaming())
		{
			/* One more, so the frame that erased the last group also gets its
			   completion callbacks run. */
			Frame();
			return !Chunks().IsStreaming();
		}
	}

	return false;
}

uint32_t StreamingHarness::ResidentChunkCount() const
{
	uint32_t uiResident = 0;

	for (const auto& iter : m_pWorld->GetChunkSystem()->GetChunks())
	{
		if (iter.second->IsLoaded())
			++uiResident;
	}

	return uiResident;
}

Entity* StreamingHarness::FindEntityNamed(const std::string& sName) const
{
	for (Entity* pEntity : m_pWorld->GetEntities())
	{
		if (pEntity->GetName() == sName)
			return pEntity;
	}

	return nullptr;
}

uint32_t StreamingHarness::CountEntitiesNamed(const std::string& sPrefix) const
{
	uint32_t uiFound = 0;

	for (Entity* pEntity : m_pWorld->GetEntities())
	{
		if (pEntity->GetName().compare(0, sPrefix.size(), sPrefix) == 0)
			++uiFound;
	}

	return uiFound;
}
