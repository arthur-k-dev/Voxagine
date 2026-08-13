#include "pch.h"
#include "ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Components/ChunkViewer.h"
#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Rendering/DebugRenderer.h"
#include "Core/ECS/Systems/Rendering/RenderSystem.h"

#include "Core/Platform/Platform.h"
#include "Core/Platform/Rendering/FrameProfiler.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/Platform/Rendering/RenderDefines.h"
#include "Core/Voxels/VoxelWindow.h"
#include "Core/ECS/World.h"
#include "Core/Application.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "External/optick/optick.h"

/* Shares VOXAGINE_CHUNK_IO_TIMINGS with Chunk's encode/decode reporting: one
   switch for "tell me what streaming costs in wall clock", read once because
   both sides of it are on paths that run per chunk. */
static bool ChunkIoTimingsEnabled()
{
	static const bool s_bEnabled = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;
	return s_bEnabled;
}

/* Milliseconds since a mark. Every budgeted state reports one, and each of them
   having its own lambda was how the two that existed already drifted apart on
   whether the clock was read when profiling was off. */
static double MillisecondsSince(const std::chrono::steady_clock::time_point& start)
{
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - start).count();
}

ChunkSystem::ChunkSystem(World* pWorld, std::unordered_map<uint32_t, Chunk*> chunks, UVector2 chunkSize, UVector2 worldSize) :
	ComponentSystem(pWorld)
{
	m_pVoxelGrid = m_pWorld->GetPhysics()->GetVoxelGrid();
	m_pVoxelWindow = pWorld->GetRenderContext();
	m_Chunks = chunks;
	m_WorldSize = worldSize;
	m_ChunkSize = chunkSize;
	m_uiNumChunkX = worldSize.x / chunkSize.x;
	m_uiNumChunkY = worldSize.y / chunkSize.y;
	m_CameraLoadOffset = Vector3(0, 0, 0);
	Entity entity(pWorld);
	ChunkViewer viewer = ChunkViewer(&entity);

	//Create default chunk
	if (m_Chunks.empty())
	{
		m_Chunks[0] = new Chunk(pWorld->GetApplication(), pWorld);
	}

	m_UpdateGroups.reserve(m_uiNumChunkX * m_uiNumChunkY);
}

ChunkSystem::~ChunkSystem()
{
	for (auto& iter : m_Chunks)
	{
		delete iter.second;
	}
	m_Chunks.clear();

	m_pWorld->Resumed -= this;
}

void ChunkSystem::Start()
{
	m_pWorld->Resumed -= this;
	m_pWorld->Resumed += Event<World*>::Subscriber(std::bind(&ChunkSystem::OnWorldResumed, this, std::placeholders::_1), this);

	Camera* pCamera = m_pWorld->GetMainCamera();

	/* The camera is this system's only input, and it is not guaranteed to
	   exist: a chunk unload destroys every non-persistent root inside it, and
	   the default Camera that World::Initialize creates is not persistent - so
	   a window that slides over the camera takes it with it. Every reader here
	   checks rather than assuming, which is the state World::DeleteEntityFromLists
	   now publishes instead of a dangling pointer. */
	if (pCamera == nullptr)
	{
		/* Nothing decides where a window goes, so there is no window to wait
		   for. Holding gameplay on a world that can never become ready would
		   turn a missing camera into a hang (R1). */
		m_bInitialRootsAdmitted = true;
		m_bInitialWindowReady = true;
		return;
	}

	Vector3 cameraPos = pCamera->GetTransform()->GetPosition() + m_CameraLoadOffset;
	Vector3 worldOffset = CalculateWorldOffset(cameraPos);

	m_pWorld->GetPhysics()->GetVoxelGrid()->SetWorldOffset(worldOffset);
	pCamera->SetCameraOffset(worldOffset);

	int chunkXPos = static_cast<int>(floor((cameraPos.x) / (float)m_ChunkSize.x));
	int chunkYPos = static_cast<int>(floor((cameraPos.z) / (float)m_ChunkSize.y));
	chunkXPos = std::min(std::max(chunkXPos, 0), (int)m_uiNumChunkX);
	chunkYPos = std::min(std::max(chunkYPos, 0), (int)m_uiNumChunkY);

	m_ClampedCameraPosition = UVector2(chunkXPos, chunkYPos);

	IVector2 gridOffset(worldOffset.x / (float)m_ChunkSize.x, worldOffset.z / (float)m_ChunkSize.y);

	/* The ground texture has to be on every chunk *before* anything decodes or
	   renders one - a streamed first window builds its voxels on a worker, and
	   UpdateGroundPlane is what puts the y = 0 row in them. */
	const auto groundStart = std::chrono::steady_clock::now();
	SetGroundPlane(m_pWorld->GetGroundTexturePath());
	if (ChunkIoTimingsEnabled())
		fprintf(stderr, "[world-switch]     SetGroundPlane %.1f ms\n", MillisecondsSince(groundStart));

	/* **The initial window is not special any more, and that is phase 4's
	   keystone.** It used to be built here, synchronously, inside
	   World::Initialize: nine chunks decoded, their roots deserialized and every
	   static renderer stamped, with the whole thing off the frame loop where the
	   frame profiler cannot see it. Measured as `[world-switch] initialize`,
	   that was **876 ms** for Beat2 - the single largest stall the game has, and
	   the whole of ledger M4.

	   It goes through the same update group as every other window now, which
	   costs nothing new to maintain and buys three things: the work is budgeted
	   by StreamingBudgets like any slide, gameplay is *held* until it lands
	   (R1 - and the hold stops being inert, which is what phase 3 promised this
	   phase would do), and a loading screen can be drawn over the wait because
	   frames keep being presented throughout.

	   The chunk the camera starts in is already resident regardless:
	   JsonSerializer::DeserializeWorld loads `CameraChunkIndex` while it is
	   still building the world, so the group sees it as a move rather than a
	   load and the player never stands on nothing. */
	JobQueue* pJobQueue = m_pWorld->GetJobQueue();

	ChunkUpdateGroup group(gridOffset.x + gridOffset.y * m_uiNumChunkX, worldOffset);
	UpdateChunks(gridOffset, group, pJobQueue != nullptr);
	group.MarkInitial();

	if (pJobQueue != nullptr)
	{
		m_UpdateGroups.push_back(group);
	}
	else
	{
		/* No job queue means nothing will ever advance the group and gameplay
		   would be held forever, so the old synchronous path stays as the
		   fallback rather than as the default. A world with no queue is a
		   world nobody is streaming. */
		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			switch (item.ItemTarget)
			{
			case ChunkUpdateGroup::Item::Target::T_LOAD:
			{
				item.pChunk->Load(item.GridTargetIndex);
				m_pVoxelGrid->SetChunkVolumeAt(item.GridTargetIndex, item.pChunk->GetVoxelData(), item.pChunk->GetOwnerVolume());
				break;
			}
			case ChunkUpdateGroup::Item::Target::T_MOVE:
			{
				m_pVoxelGrid->SetChunkVolumeAt(item.GridTargetIndex, item.pChunk->GetVoxelData(), item.pChunk->GetOwnerVolume());
				if (!item.pChunk->IsFirstLoad() && m_pVoxelWindow != nullptr)
				{
					RenderChunk(item, m_pVoxelWindow->GetFrontData(), false);
				}
				item.pChunk->SetGridTarget(item.GridTargetIndex);
				break;
			}
			default:
				break;
			}
		}

		m_bInitialWindowReady = true;
	}

	/* Far-field LOD volume (RENDERING_PLAN.md phase 4). Here rather than in
	   JsonSerializer::DeserializeWorld because it needs every chunk registered
	   with this system and the voxel grid sized, both of which are only true
	   once the system exists - and it is the chunk window's own limit that the
	   far field exists to work around, so this is where it belongs. */
	/* Begun here and continued from Tick, so `World::Initialize` no longer
	   carries the 447 ms it used to (CHUNK_STREAMING_PLAN.md phase 4). The
	   volume reports itself unbuilt until it finishes, and every shader reads
	   that as "no far field" - so the horizon arrives a fraction of a second
	   after the level does rather than the level arriving half a second after
	   the loading screen stops animating. */
	if (RenderContext* pRenderContext = m_pWorld->GetRenderContext())
		pRenderContext->BeginFarFieldBuild(m_pWorld);
}

bool ChunkSystem::IsStreaming() const
{
	if (!m_UpdateGroups.empty())
		return true;

	/* A far-field build in progress and renderers admitted but not yet stamped
	   are both "the window is still arriving" as far as any caller is
	   concerned - and both became possible in phases 4 and 5, which is what the
	   header comment anticipated. Without them a test settles on a world whose
	   geometry is still being written. */
	if (RenderContext* pRenderContext = m_pWorld->GetRenderContext())
	{
		if (pRenderContext->IsFarFieldBuilding())
			return true;
	}

	if (RenderSystem* pRenderSystem = m_pWorld->GetRenderSystem())
	{
		if (pRenderSystem->HasPendingVoxelBakes())
			return true;
	}

	return false;
}

bool ChunkSystem::CanProcessComponent(Component* pComponent)
{
	return false;
}

void ChunkSystem::Tick(float fDeltaTime)
{
	/* Chunk I/O and reflected entity construction are streaming work, not
	   simulation, so an outstanding group advances per *display* frame rather
	   than per fixed tick. FixedTick still creates groups - that is a question
	   about where the camera is - but advancing an existing one only at 60 Hz
	   leaves every display frame above that idle while the player crosses into
	   a window that has not arrived yet.

	   **One state per display frame, and that is the point.** The advance used
	   to be at the end of FixedTick; leaving it there as well would let the
	   commit and the entity pass land in the same frame, which is the pile-up
	   this phase exists to break up. Every state later phases add carries its
	   own budget on top (StreamingBudgets.h).

	   **A host that calls FixedTick must call this too.** Splitting the two
	   made that a requirement where it never used to be one, and EditorWorld
	   promptly failed it: in edit mode it hand-picks the systems it ticks, and
	   it named the chunk system in FixedTick and PostTick but not here - so the
	   editor created update groups and never advanced one, and moving the
	   camera stopped loading chunks entirely. Fixed there; stated here because
	   the next such host will not read EditorWorld.cpp. */
	if (!m_UpdateGroups.empty())
		UpdateGroup(m_UpdateGroups.front());

	/* R1's second half, and phase 5 is what made it necessary. The initial
	   group admitting its roots puts the level's *entities* in the world; the
	   VoxelBaker writing their models into the voxel window is budgeted now, so
	   the geometry arrives over the frames after that. Gameplay must wait for
	   both, and the first run of phase 5 is the argument: the player started
	   walking while the river bed was still being stamped, fell through the
	   hole where it was going to be, and the level ended on the game-over
	   screen. */
	if (!m_bInitialWindowReady && m_bInitialRootsAdmitted)
	{
		RenderSystem* pRenderSystem = m_pWorld->GetRenderSystem();

		if (pRenderSystem == nullptr || !pRenderSystem->HasPendingVoxelBakes())
		{
			m_bInitialWindowReady = true;

			/* The number CLAUDE.md's link-retry note asks for, and the one phase 9
			   trades against: bounding a stamp below one renderer moves work out
			   of the worst frame and into more frames, so this is where the cost
			   of that trade shows up. If it ever exceeds
			   World::k_uiMaxWorldLinkRetries, every cross-chunk link in the level
			   has already been abandoned before gameplay begins. */
			fprintf(stderr, "[world] gameplay held %llu ticks\n",
				static_cast<unsigned long long>(
					StreamingCounters::Get().GameplayTicksHeld.load(std::memory_order_relaxed)));
		}
	}

	/* One slice of the level's far field, if one is outstanding. Deliberately
	   after the group: a window that has not arrived is worth more than a
	   horizon that has not, and the two never share a frame's budget because
	   each carries its own. */
	if (RenderContext* pRenderContext = m_pWorld->GetRenderContext())
	{
		if (pRenderContext->IsFarFieldBuilding())
		{
			const bool bProfiling = FrameProfiler::Get().IsEnabled();
			const std::chrono::steady_clock::time_point start =
				bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

			StreamingBudget::Scope budget(StreamingBudgets::Get().FarFieldBuild);
			pRenderContext->ContinueFarFieldBuild(budget);

			if (bProfiling)
				FrameProfiler::Get().Report("CPU FarField Build", MillisecondsSince(start));
		}
	}
}

void ChunkSystem::FixedTick(const GameTimer& fixedTimer)
{
	Camera* pCamera = m_pWorld->GetMainCamera();

	if (pCamera == nullptr)
		return;

	Vector3 cameraPosition = pCamera->GetTransform()->GetPosition() + m_CameraLoadOffset;
	int chunkXPos = static_cast<int>(floor((cameraPosition.x) / (float)m_ChunkSize.x));
	int chunkYPos = static_cast<int>(floor((cameraPosition.z) / (float)m_ChunkSize.y));
	chunkXPos = std::min(std::max(chunkXPos, 0), (int)m_uiNumChunkX);
	chunkYPos = std::min(std::max(chunkYPos, 0), (int)m_uiNumChunkY);

	/* Where the streaming decision is made, and the first thing to check when a
	   headless run produces no transitions at all: this says whether the camera
	   is moving, separately from whether the window followed it. */
	if (ChunkIoTimingsEnabled() &&
		(m_ClampedCameraPosition.x != (uint32_t)chunkXPos || m_ClampedCameraPosition.y != (uint32_t)chunkYPos))
	{
		fprintf(stderr, "[chunk] camera entered chunk (%d,%d) at (%.0f, %.0f)\n",
			chunkXPos, chunkYPos, cameraPosition.x, cameraPosition.z);
	}

	//Make sure we don't go diagonally as the chunk system does not support this
	if (m_ClampedCameraPosition.x != (uint32_t)chunkXPos && m_ClampedCameraPosition.y != (uint32_t)chunkYPos)
		m_ClampedCameraPosition.x = chunkXPos;
	else
	{
		m_ClampedCameraPosition.x = chunkXPos;
		m_ClampedCameraPosition.y = chunkYPos;
	}

	Vector3 worldOffset = CalculateWorldOffset(cameraPosition);
	Vector3 currWorldOffset = m_pWorld->GetPhysics()->GetVoxelGrid()->GetWorldOffset();
	if (currWorldOffset != worldOffset &&
		(m_ClampedCameraPosition.x < m_uiNumChunkX && m_ClampedCameraPosition.y < m_uiNumChunkY))
	{
		IVector2 gridOffset(worldOffset.x / (float)m_ChunkSize.x, worldOffset.z / (float)m_ChunkSize.y);
		ChunkUpdateGroup group(gridOffset.x + gridOffset.y * m_uiNumChunkX, worldOffset);

		//Add the UpdateGroup if its not already in the list
		auto groupIter = std::find_if(m_UpdateGroups.begin(), m_UpdateGroups.end(), group);
		if (groupIter == m_UpdateGroups.end())
		{
			UpdateChunks(gridOffset, group);
			m_UpdateGroups.push_back(group);
		}
		else
		{
			//Check if the requested new group is not the previously requested group
			if (m_UpdateGroups[m_UpdateGroups.size() - 1].GetId() != group.GetId())
			{
				//We have returned to a chunk which we already requested for update, remove all elements above this one 
				//since they came after this chunk request
				for (auto iter = m_UpdateGroups.begin(); iter != m_UpdateGroups.end(); ++iter)
				{
					if (groupIter->GetId() == iter->GetId())
					{
						++iter;
						while (iter != m_UpdateGroups.end())
						{
							iter = RemoveUpdateGroup(iter);
						}
						break;
					}
				}
			}
		}
	}

	/* Creating the group is this method's whole job now; advancing it belongs
	   to Tick - see there. Advancing from both would let a state and its
	   successor land in the same frame, which is exactly the pile-up the commit
	   was split out of. */
}

void ChunkSystem::PostTick(float fDeltaTime)
{
#if defined(EDITOR) || defined(_DEBUG)
	if (m_pWorld->GetDebugRenderer() == nullptr)
		return;

	uint32_t numHorizontalLines = m_uiNumChunkY - 1;
	uint32_t numVerticalLines = m_uiNumChunkX - 1;

	for (uint32_t x = 0; x < numHorizontalLines; ++x)
	{
		m_pWorld->GetDebugRenderer()->AddLine(Vector3(0.f, 5.f, (x + 1) * m_ChunkSize.y), Vector3(m_uiNumChunkX * m_ChunkSize.x, 5.f, (x + 1) * m_ChunkSize.y), VColors::Green);
	}

	for (uint32_t y = 0; y < numVerticalLines; ++y)
	{
		m_pWorld->GetDebugRenderer()->AddLine(Vector3((y + 1) * m_ChunkSize.x, 5.f, 0.f), Vector3((y + 1) * m_ChunkSize.x, 5.f, m_uiNumChunkY * m_ChunkSize.y), VColors::Green);
	}
#endif
}

void ChunkSystem::SetGroundPlane(const std::string& texturePath)
{
	for (auto& chunkIter : m_Chunks)
	{
		chunkIter.second->SetGroundPlane(texturePath);
	}
}

void ChunkSystem::OnComponentAdded(Component* pComponent)
{

}

void ChunkSystem::OnComponentDestroyed(Component* pComponent)
{

}

Vector3 ChunkSystem::CalculateWorldOffset(Vector3 viewPosition)
{
	int chunkXPos = static_cast<int>(floor((viewPosition.x) / (float)m_ChunkSize.x));
	int chunkYPos = static_cast<int>(floor((viewPosition.z) / (float)m_ChunkSize.y));

	Vector3 worldOffset((chunkXPos - 1) * (float)m_ChunkSize.x, 0.f, (chunkYPos - 1) * (float)m_ChunkSize.y);
	worldOffset.x = glm::clamp(worldOffset.x, 0.f, (m_uiNumChunkX - 3) * (float)m_ChunkSize.x);
	worldOffset.z = glm::clamp(worldOffset.z, 0.f, (m_uiNumChunkY - 3) * (float)m_ChunkSize.y);

	return worldOffset;
}

void ChunkSystem::UpdateChunks(IVector2 gridOffset, ChunkUpdateGroup& group, bool bAsync)
{
	OPTICK_CATEGORY("ChunkSystem", Optick::Category::GameLogic);
	OPTICK_EVENT();

	/* RENDERING_PLAN.md Phase 0: the other known main-thread cost, alongside
	   VoxelBaker::Bake. Guarded so a disabled profiler pays nothing but this
	   one branch. */
	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	const std::chrono::steady_clock::time_point start =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	for (uint32_t x = 0; x < m_uiNumChunkX; ++x)
	{
		for (uint32_t y = 0; y < m_uiNumChunkY; ++y)
		{
			if (x >= m_uiNumChunkX || y >= m_uiNumChunkY) continue;

			Chunk* pChunk = m_Chunks[x + y * m_uiNumChunkX];
			if (!pChunk) continue;

			if (x >= static_cast<uint32_t>(gridOffset.x) && x < static_cast<uint32_t>(gridOffset.x + 3) && y >= static_cast<uint32_t>(gridOffset.y) && y < static_cast<uint32_t>(gridOffset.y + 3))
			{
				UVector2 gridTarget(x - gridOffset.x, y - gridOffset.y);
				if (!pChunk->IsTargetLoaded())
				{
					if (bAsync)
					{
						pChunk->SetTargetLoaded(true);
						group.AddItem(ChunkUpdateGroup::Item(ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD, pChunk, gridTarget, false));
					}
					else
					{
						pChunk->SetTargetLoaded(true);
						group.AddItem(ChunkUpdateGroup::Item(ChunkUpdateGroup::Item::Target::T_LOAD, pChunk, gridTarget));
					}
				}
				else
				{
					group.AddItem(ChunkUpdateGroup::Item(ChunkUpdateGroup::Item::Target::T_MOVE, pChunk, gridTarget));
				}
			}
			else if (pChunk->IsTargetLoaded())
			{
				pChunk->SetTargetLoaded(false);
				group.AddItem(ChunkUpdateGroup::Item(ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD, pChunk, pChunk->GetGridTarget()));
			}
		}
	}

	/* Item order is an accident of the world grid's x-major loop otherwise, and
	   that decides which chunk's entities are constructed first. It is inert
	   today - everything after the commit still runs to completion in one pass
	   - but it is where the order belongs, it is cheap, and it is far easier to
	   verify now than woven through three budgeted states in phase 3.

	   Two keys. Loads before moves before unloads, because a chunk arriving in
	   front of the player is worth more than one leaving behind them; then
	   nearest first, measured from where the camera is *looking* on the ground
	   plane rather than from the camera itself. A top-down camera's own
	   position ranks the chunk under the player first even when the player is
	   walking towards a boundary and the interesting chunk is two ahead.
	   Stable, so equal distances keep grid order and the sequence is
	   reproducible. */
	Camera* pMainCamera = m_pWorld->GetMainCamera();

	const Vector3 cameraPosition = pMainCamera != nullptr
		? pMainCamera->GetTransform()->GetPosition() : group.GetWorldOffset();
	const Vector3 cameraForward = pMainCamera != nullptr
		? pMainCamera->GetTransform()->GetForward() : Vector3(0.f, -1.f, 0.f);

	Vector3 groundFocus = cameraPosition;

	if (std::abs(cameraForward.y) > 0.0001f)
	{
		const float fGroundDistance = (R_GROUND_PLANE_HEIGHT - cameraPosition.y) / cameraForward.y;

		if (fGroundDistance >= 0.f && std::isfinite(fGroundDistance))
			groundFocus += cameraForward * fGroundDistance;
	}

	/* Window-local, because GridTargetIndex is a slot in the incoming window
	   and the offset that window will have is the group's, not the live one. */
	const Vector3 localFocus = groundFocus - group.GetWorldOffset();

	auto targetPriority = [](ChunkUpdateGroup::Item::Target target)
	{
		switch (target)
		{
		case ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD:
		case ChunkUpdateGroup::Item::Target::T_LOAD:
			return 0;
		case ChunkUpdateGroup::Item::Target::T_MOVE:
			return 1;
		case ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD:
			return 2;
		}

		return 3;
	};

	auto distanceSquared = [&](const ChunkUpdateGroup::Item& item)
	{
		const float fCenterX = (static_cast<float>(item.GridTargetIndex.x) + 0.5f) * m_ChunkSize.x;
		const float fCenterZ = (static_cast<float>(item.GridTargetIndex.y) + 0.5f) * m_ChunkSize.y;
		const float fDeltaX = fCenterX - localFocus.x;
		const float fDeltaZ = fCenterZ - localFocus.z;

		return fDeltaX * fDeltaX + fDeltaZ * fDeltaZ;
	};

	std::vector<ChunkUpdateGroup::Item>& items = group.GetItems();

	std::stable_sort(items.begin(), items.end(),
		[&](const ChunkUpdateGroup::Item& left, const ChunkUpdateGroup::Item& right)
		{
			const int iLeftPriority = targetPriority(left.ItemTarget);
			const int iRightPriority = targetPriority(right.ItemTarget);

			if (iLeftPriority != iRightPriority)
				return iLeftPriority < iRightPriority;

			return distanceSquared(left) < distanceSquared(right);
		});

	if (bProfiling)
	{
		const double fMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

		FrameProfiler::Get().Report("CPU ChunkSystem::UpdateChunks", fMilliseconds);
	}
}

void ChunkSystem::UpdateGroup(ChunkUpdateGroup& group)
{
	group.CountAdvance();

	switch (group.GetState())
	{
	case ChunkUpdateGroup::UpdateState::US_INIT:
	{
		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)
			{
				/* Here, on the main thread, before the job that will resize into
				   it exists: the pool is this system's state and the load job is
				   not allowed to touch it. */
				AcquireChunkStorage(*item.pChunk);

				item.pChunk->LoadAsync(&item, std::bind(&ChunkSystem::OnChunkLoaded, this, std::placeholders::_1));
			}
		}
		group.SetState(ChunkUpdateGroup::UpdateState::US_WAIT);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_WAIT:
	{
		bool isReady = true;
		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (!item.bIsDone)
			{
				isReady = false;
				break;
			}
		}
		if (isReady)
			group.SetState(ChunkUpdateGroup::UpdateState::US_RENDERING);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_RENDERING:
	{
		/* The worker owns the back buffer for this whole state and staging
		   touches no voxels, so the main thread spends the wait constructing the
		   incoming chunks' entity trees instead of idling. On a settled machine
		   that is most of a slide's deserialization paid for before the window
		   even publishes. Detached, so nothing observes it (see
		   Chunk::StageEntityBatch). */
		if (group.IsRendering())
		{
			StageIncomingEntities(group);
			break;
		}

		group.SetRendering(true);

		/* Null where there is no render context at all - a backend that failed
		   to start, or a headless test. The group still has to reach the commit
		   or the state machine wedges; the commit's render half simply does
		   nothing. */
		uint32_t* viewPortData = m_pVoxelWindow != nullptr ? m_pVoxelWindow->GetBackData() : nullptr;

		JobQueue* pJobQueue = m_pWorld->GetJobQueue();
		if (pJobQueue)
		{
			pJobQueue->Enqueue<bool>([this, &group, viewPortData]()
			{
				/* The whole incoming window is built here, on a worker, into a
				   buffer nothing else reads. That is what lets the publish be a
				   swap rather than a copy. */
				VoxelBrickGrid* pBrickGrid =
					m_pVoxelWindow != nullptr ? &m_pVoxelWindow->GetBrickGrid() : nullptr;

				if (pBrickGrid != nullptr)
					pBrickGrid->BeginBackBufferBuild();

				for (ChunkUpdateGroup::Item& item : group.GetItems())
				{
					if (item.ItemTarget != ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD)
						RenderChunk(item, viewPortData, true);
				}

				/* And the occupancy hierarchy over it, here rather than on the
				   main thread. This job owns the back buffer for its whole
				   length, so this is the only place the pyramid over it can be
				   rebuilt from settled data; the per-frame FlushDirty walks the
				   front buffer only and so cannot race it (M1). */
				if (pBrickGrid != nullptr)
				{
					pBrickGrid->FlushDirtyBackBuffer();
					pBrickGrid->EndBackBufferBuild();
				}

				return true;
			}, [&group](bool bFinished)
			{
				/* Completion callbacks run on the *main thread*. Master did the
				   entire rest of the transition in here - physics pointers,
				   every incoming chunk's entities, the offset, the swap, and
				   the outgoing chunks' serialization - which is why a window
				   slide read as one visible stop. It now only advances the
				   state; the commit is its own transaction on its own tick. */
				group.SetState(ChunkUpdateGroup::UpdateState::US_COMMIT);
				group.SetRendering(false);
			});
		}
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_COMMIT:
	{
		CommitWindow(group);
		group.SetState(ChunkUpdateGroup::UpdateState::US_ADMITTING_GAMEPLAY);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_ADMITTING_GAMEPLAY:
	{
		/* The gameplay contract, and the whole of ledger E3's replacement.
		   Staging finishes first - it usually already has, from US_RENDERING -
		   and then *every* non-static root of *every* incoming chunk enters the
		   world in this one frame.

		   Deliberately unbudgeted, and the measurement says why it can be: the
		   worst incoming slide in any shipped level is 430 non-static roots
		   (Walking_Through_The_Maru_Beat3), and admitting a root is a pointer
		   push per node - the work it sets off is the per-renderer stamp on the
		   next PreTick, which is phase 5's budget to bound. Splitting this would
		   buy nothing here and would re-open the transient-null link class the
		   experiment patched per manager. */
		if (!StageIncomingEntities(group))
			break;

		const bool bProfiling = FrameProfiler::Get().IsEnabled();
		const std::chrono::steady_clock::time_point phase =
			bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		uint32_t uiAdmitted = 0;

		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)
				uiAdmitted += item.pChunk->AdmitStagedGameplay();
		}

		StreamingCounters::RaiseMax(
			StreamingCounters::Get().MaxGameplayRootsPerAdmission, uiAdmitted);

		if (bProfiling && uiAdmitted > 0)
			FrameProfiler::Get().Report("CPU Chunk AdmitGameplay", MillisecondsSince(phase));

		group.ResetItemCursor();
		group.SetState(ChunkUpdateGroup::UpdateState::US_LOADING_ENTITIES);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_LOADING_ENTITIES:
	{
		/* What is left is static art, and it admits in bounded slices - this is
		   where phase 0's 41.6 ms and phase 1's 44.2 ms per incoming chunk went.

		   The budget is a *count* of roots rather than a clock, because the cost
		   of admitting one is not paid here: see StreamingBudgets::
		   EntityAdmission.

		   Running after the commit is not cosmetic. A first-load chunk's static
		   renderers stamp against the world offset and the chunk slots they will
		   actually be drawn at; master's callback repointed the grid's slots,
		   stamped against the *outgoing* offset, then swapped the buffer those
		   stamps went into. */
		const bool bProfiling = FrameProfiler::Get().IsEnabled();

		const std::chrono::steady_clock::time_point phase =
			bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		StreamingBudget::Scope budget(StreamingBudgets::Get().EntityAdmission);
		StreamingBudget::Scope refresh(StreamingBudgets::Get().EntityRefresh);

		uint32_t uiLoaded = 0;

		std::vector<ChunkUpdateGroup::Item>& items = group.GetItems();

		while (group.GetItemCursor() < items.size())
		{
			ChunkUpdateGroup::Item& item = items[group.GetItemCursor()];

			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)
			{
				/* Not done: leave the cursor here and come back next frame. */
				if (!item.pChunk->AdmitStagedStatic(budget))
					break;

				item.pChunk->m_bIsLoaded = true;
				item.pChunk->m_bFirstLoad = false;

				++uiLoaded;
			}
			else if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_MOVE)
			{
				/* One chunk's renderer refresh per frame. Cheap (0.06 ms) but
				   there is no reason for three of them to share a frame with an
				   admission slice. */
				if (refresh.Exhausted())
					break;

				item.pChunk->UpdateEntities();
				refresh.Consume();
			}

			group.AdvanceItemCursor();

			if (budget.Exhausted())
				break;
		}

		if (bProfiling)
		{
			FrameProfiler::Get().Report(
				uiLoaded > 0 ? "CPU Chunk LoadEntities" : "CPU Chunk UpdateEntities",
				MillisecondsSince(phase));
		}

		if (group.GetItemCursor() < items.size())
			break;

		/* R1 ends here, not at the commit: the window is published two states
		   earlier but its gameplay roots are only *in the world* now, and
		   holding until they are is the whole point of the rule. Static art may
		   still be arriving behind this - it is art. */
		if (group.IsInitial())
			m_bInitialRootsAdmitted = true;

		group.ResetItemCursor();
		group.SetState(ChunkUpdateGroup::UpdateState::US_START_UNLOADING);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_START_UNLOADING:
	{
		/* The main-thread half of unloading, one chunk at a time, a bounded
		   number of roots per display frame. Master did every outgoing chunk's
		   FindEntitiesInChunk and full RTTR serialization in one go inside the
		   render job's completion callback.

		   The budget is per *state entry*, not per chunk: three outgoing chunks
		   share one slice rather than getting one each, or "bounded per frame"
		   would mean three times the bound whenever a corner slide leaves
		   three. */
		const bool bProfiling = FrameProfiler::Get().IsEnabled();
		const std::chrono::steady_clock::time_point start =
			bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		StreamingBudget::Scope budget(StreamingBudgets::Get().UnloadSerialization);

		std::vector<ChunkUpdateGroup::Item>& items = group.GetItems();

		while (group.GetItemCursor() < items.size())
		{
			ChunkUpdateGroup::Item& item = items[group.GetItemCursor()];

			if (item.ItemTarget != ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD)
			{
				group.AdvanceItemCursor();
				continue;
			}

			/* **Unloading a chunk that was never loaded destroys it**, and that
			   is not a hypothetical: a chunk can reach this state with no voxels
			   and no entities in the world, because group creation reads
			   `IsTargetLoaded()` - a promise a *queued* group made - and emits
			   T_MOVE for a chunk that a later cancellation then never loads.
			   Serializing it writes an empty root list over the level's copy of
			   its entities, and encoding it writes an empty RLE stream over its
			   voxels. Both are permanent: nothing else holds either.

			   Pre-existing, and master loses exactly the same data by the same
			   route - `SaveAndDeleteEntities` cleared and resized the root list
			   to however many entities it found, which is zero. Found by the
			   seeded walk in Tests/Streaming/ChunkUnloadChecks.cpp, which is
			   what makes this the phase that can fix it: nothing before could
			   express "a chunk came back empty". */
			if (!item.pChunk->IsLoaded())
			{
				StreamingCounters::Get().UnloadsOfUnloadedChunks.fetch_add(1, std::memory_order_relaxed);

				item.pChunk->SetTargetLoaded(false);
				group.AdvanceItemCursor();
				continue;
			}

			/* Not done: leave the cursor where it is and come back to this same
			   chunk next frame. */
			if (!item.pChunk->PrepareUnloadBatch(budget))
				break;

			/* This chunk's entities are gone and its voxels are about to be.
			   Detach here, on this thread, before anything can read the storage
			   as if it were still in the window (ledger P7): every grid accessor
			   treats a detached slot as "not resident", which is what an
			   unloading chunk is. */
			m_pVoxelGrid->DetachChunkStorage(&item.pChunk->GetVoxelData());

			item.pChunk->m_bIsLoaded = false;
			item.pChunk->BeginVoxelEncoding();
			item.bIsDone = false;

			group.AdvanceItemCursor();

			if (budget.Exhausted())
				break;
		}

		if (bProfiling)
			FrameProfiler::Get().Report("CPU Chunk Unload", MillisecondsSince(start));

		if (group.GetItemCursor() < items.size())
			break;

		group.ResetItemCursor();
		group.SetState(ChunkUpdateGroup::UpdateState::US_ENCODING);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_ENCODING:
	{
		/* RLE compression of each outgoing chunk's voxels, in bounded slices on
		   this thread rather than in one uninterrupted pass on a worker.

		   Moving it here is not obviously the right direction and is worth the
		   sentence: a worker costs the main thread nothing, and encode is 9-12 ms
		   per chunk. What it costs instead is a 32 MiB read and a 48 MiB free
		   competing with the main thread for memory bandwidth during exactly the
		   frames a transition is already expensive, plus the storage being freed
		   somewhere the grid used to still point at. Slicing it here makes the
		   cost a bounded 2 ms a frame, and the pool below removes the free
		   altogether. What it buys back is paid in transition latency - see
		   StreamingBudgets.h. */
		const bool bProfiling = FrameProfiler::Get().IsEnabled();
		const std::chrono::steady_clock::time_point start =
			bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		StreamingBudget::Scope budget(StreamingBudgets::Get().VoxelEncoding);

		std::vector<ChunkUpdateGroup::Item>& items = group.GetItems();

		while (group.GetItemCursor() < items.size())
		{
			ChunkUpdateGroup::Item& item = items[group.GetItemCursor()];

			if (item.ItemTarget != ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD || item.bIsDone)
			{
				group.AdvanceItemCursor();
				continue;
			}

			if (!item.pChunk->EncodeVoxelBatch(budget))
				break;

			item.pChunk->m_bIsUnloading = false;
			OnChunkUnloaded(&item);

			group.AdvanceItemCursor();

			if (budget.Exhausted())
				break;
		}

		if (bProfiling)
			FrameProfiler::Get().Report("CPU Chunk Encode", MillisecondsSince(start));

		if (group.GetItemCursor() < items.size())
			break;

		group.ResetItemCursor();
		group.SetState(ChunkUpdateGroup::UpdateState::US_UNLOADING);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_UNLOADING:
	{
		bool isReady = true;
		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (!item.bIsDone)
			{
				isReady = false;
				break;
			}
		}
		if (isReady)
		{
			RemoveUpdateGroup(m_UpdateGroups.begin());
		}
		break;
	}
	default:
		break;
	}
}

std::vector<ChunkUpdateGroup>::iterator ChunkSystem::RemoveUpdateGroup(const std::vector<ChunkUpdateGroup>::iterator& iter)
{
	/* R5. A group erased before it published is the walk-back: the player
	   crossed a boundary and came straight back, so the window it was building
	   is no longer the one wanted. Counted rather than merely allowed, so a
	   cancellation scenario can assert it actually cancelled something instead
	   of quietly racing the transition to completion. */
	const bool bCancelled = !iter->HasCommitted();

	if (bCancelled)
		StreamingCounters::Get().CancelledGroups.fetch_add(1, std::memory_order_relaxed);

	/* Which chunk state this group is entitled to reset (R5), and it is *its
	   own* only. A chunk can be an item of several groups at once - the front
	   one unloading it, a queued one scheduled to load it back - so "reset every
	   chunk this cancelled group mentions" reaches into a transaction another
	   group is half-way through. It did: resetting m_bUnloadPrepared under the
	   front group made its next slice prepare a second time, clear the roots it
	   had already written, and re-serialize entities it had already destroyed -
	   so the chunk came back empty. Intermittent, because it needs a queued
	   group to be cancelled while the front group is inside US_START_UNLOADING.
	   A group that never advanced past the commit has created no per-chunk state
	   and must therefore touch none. */
	const bool bOwnsChunkState =
		iter->GetState() == ChunkUpdateGroup::UpdateState::US_RENDERING ||
		iter->GetState() == ChunkUpdateGroup::UpdateState::US_ADMITTING_GAMEPLAY ||
		iter->GetState() == ChunkUpdateGroup::UpdateState::US_LOADING_ENTITIES ||
		iter->GetState() == ChunkUpdateGroup::UpdateState::US_START_UNLOADING ||
		iter->GetState() == ChunkUpdateGroup::UpdateState::US_ENCODING;

	/* The three load states are here for completeness rather than because they
	   are reachable: only `m_UpdateGroups.front()` ever advances, and a
	   cancellation only ever erases groups *behind* the one the camera returned
	   to - so a cancelled group is in US_INIT and owns nothing. Naming them
	   anyway costs a comparison and means that if scheduling ever lets a
	   half-staged group be erased, its detached roots are deleted rather than
	   leaked. */

	/* What the player actually waits: boundary crossed -> new chunks there.
	   Off by default; see the comment on MillisecondsSinceCreated. */
	if (ChunkIoTimingsEnabled())
	{
		fprintf(stderr, "[chunk] transition %s after %.1f ms over %u advances\n",
			iter->HasCommitted() ? "completed" : "cancelled",
			iter->MillisecondsSinceCreated(), iter->GetAdvanceCount());
	}

	for (ChunkUpdateGroup::Item& item : iter->GetItems())
	{
		/* R5, ledger E2. Everything a partly-run unload left on a chunk goes
		   back to its post-construction value, or the next group inherits it:
		   a half-filled serialized-root list, an encode cursor part-way through
		   a volume, m_bIsUnloading stuck on. Only for a group that got far
		   enough to have created any - see bOwnsChunkState above. */
		if (bCancelled && bOwnsChunkState)
			item.pChunk->ResetStreamingState();

		if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD &&
			(!item.pChunk->IsLoaded() || item.pChunk->IsUnloading() ||
			m_UpdateGroups[0].IsChunkScheduledFor(item.pChunk, ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD)))
		{
			item.pChunk->SetTargetLoaded(false);
		}
		else if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD &&
			(item.pChunk->IsLoaded() || item.pChunk->IsLoading() ||
				m_UpdateGroups[0].IsChunkScheduledFor(item.pChunk, ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)))
		{
			item.pChunk->SetTargetLoaded(true);
		}
	}

	return m_UpdateGroups.erase(iter);
}

bool ChunkSystem::StageIncomingEntities(ChunkUpdateGroup& group)
{
	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	const std::chrono::steady_clock::time_point start =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	/* One budget for the whole group, not one per chunk: three incoming chunks
	   share a slice rather than getting one each, or "bounded per frame" would
	   mean three times the bound whenever a corner slide brings three. Same rule
	   as US_START_UNLOADING's. */
	StreamingBudget::Scope budget(StreamingBudgets::Get().EntityStaging);

	std::vector<ChunkUpdateGroup::Item>& items = group.GetItems();

	while (group.GetStagingCursor() < items.size())
	{
		ChunkUpdateGroup::Item& item = items[group.GetStagingCursor()];

		if (item.ItemTarget != ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)
		{
			group.AdvanceStagingCursor();
			continue;
		}

		/* The chunk's own decode has to have finished - m_RootEntities is what
		   is being read here and LoadAsync's job thread does not touch it, but
		   its voxel resize does run on that thread and the chunk is not this
		   system's to read from until the job reports done. */
		if (!item.bIsDone)
			break;

		if (!item.pChunk->StageEntityBatch(budget))
			break;

		group.AdvanceStagingCursor();

		if (budget.Exhausted())
			break;
	}

	if (bProfiling)
		FrameProfiler::Get().Report("CPU Chunk StageEntitiesPass", MillisecondsSince(start));

	return group.GetStagingCursor() >= items.size();
}

void ChunkSystem::CommitWindow(ChunkUpdateGroup& group)
{
	/* R2: the voxel window is published atomically or not at all. Physics
	   pointers, world offset, buffer swap and camera offset move together in
	   one main-thread transaction, and nothing between them yields - so nothing
	   can observe a half-window.

	   The back buffer this publishes already holds the complete static voxel
	   volume of every incoming chunk, built by the render job, so collision and
	   rendering are both correct the instant this returns even though the
	   incoming chunks' entities have not been admitted yet. */
	assert(!group.HasCommitted() && "an update group published its window twice");

	if (group.HasCommitted())
	{
		StreamingCounters::Get().PublishesOutsideCommit.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	const std::chrono::steady_clock::time_point start =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	for (ChunkUpdateGroup::Item& item : group.GetItems())
	{
		if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD)
			continue;

		m_pVoxelGrid->SetChunkVolumeAt(item.GridTargetIndex, item.pChunk->GetVoxelData(), item.pChunk->GetOwnerVolume());
		item.pChunk->SetGridTarget(item.GridTargetIndex);
	}

	m_pVoxelGrid->SetWorldOffset(group.GetWorldOffset());

	if (m_pVoxelWindow != nullptr)
	{
		/* The swap reverses the two buffers' roles, so whatever the GPU is
		   still fetching from becomes the CPU's next writable back buffer the
		   moment it returns. Retiring the readers here is the only thing
		   between that and streaming overwriting a buffer an in-flight
		   submission is reading. Once per commit, not once per frame - E4 is
		   the shape this is deliberately not. */
		m_pVoxelWindow->WaitForReaders();
		m_pVoxelWindow->Swap();
	}

	if (Camera* pMainCamera = m_pWorld->GetMainCamera())
	{
		pMainCamera->SetCameraOffset(group.GetWorldOffset());
		pMainCamera->GetTransform()->SetFromMatrix(pMainCamera->GetTransform()->GetMatrix());
		pMainCamera->Recalculate();
		pMainCamera->ForceUpdate();
	}

	/* **The initial window republishes a world that already had entities in
	   it, and they must be re-examined or their geometry is lost.**

	   JsonSerializer::DeserializeWorld loads the CameraChunkIndex chunk
	   synchronously, before this system exists, so its entities are in the
	   world for the very first PreTick - which registers their components and
	   bakes them. That bake runs *before* this commit has called
	   SetChunkVolumeAt, so VoxelGrid::GetCell finds no resident volume and
	   Occupy drops every voxel ("if (!cell) return;"). Worse, the bake clears
	   the renderer's Updated/UpdateRequested flags and m_bForcedUpdate along
	   with them, so nothing ever asks again and the models are simply absent
	   for the life of the level.

	   Master could not hit this: ChunkSystem::Start set every chunk volume
	   synchronously inside World::Initialize, before any PreTick existed to
	   stamp anything. Measured on Fishing_Village_Beat2, chunk (0,0): 367,115
	   occupied voxels on master against 201,038 without this.

	   Deliberately only for the *initial* group. A force is a re-examination
	   rather than a re-stamp (RENDERING_PLAN.md 4c), but on an ordinary slide
	   the offset change already makes bBakeCurrent false, so forcing there
	   would turn every transition into a full re-stamp of the window - which is
	   the 400 ms cost phase 4c removed. */
	if (group.IsInitial())
	{
		if (RenderSystem* pRenderSystem = m_pWorld->GetRenderSystem())
			pRenderSystem->ForceUpdate();
	}

	group.MarkCommitted();

	StreamingCounters::Get().Commits.fetch_add(1, std::memory_order_relaxed);
	StreamingCounters::Get().CommittedGroups.fetch_add(1, std::memory_order_relaxed);

	if (bProfiling)
	{
		FrameProfiler::Get().Report("CPU Chunk Commit",
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count());
	}
}

void ChunkSystem::RenderChunk(ChunkUpdateGroup::Item& updateItem, uint32_t* viewPortData, bool bBackBuffer)
{
	if (m_pVoxelWindow == nullptr || viewPortData == nullptr)
		return;

	UVector3 gridDimensions = m_pVoxelGrid->GetDimensions();
	UVector2 chunkOffset = updateItem.GridTargetIndex * m_ChunkSize;
	std::vector<Voxel>& voxelData = updateItem.pChunk->GetVoxelData();

	if (voxelData.size() > 0 && m_pVoxelWindow->GetWordCount() == m_pVoxelGrid->GetNumVoxels())
	{
		/* This overwrites the chunk's slice of the window in full, so its
		   occupancy bricks are rebuilt from the chunk's own CPU-side voxels
		   rather than by diffing against the mapping. Diffing would mean
		   reading eight million voxels back out of uncached host-visible
		   memory, which costs far more than the write itself. */
		VoxelBrickGrid& brickGrid = m_pVoxelWindow->GetBrickGrid();

		const UVector3 v3RegionMin(chunkOffset.x, 0, chunkOffset.y);
		const UVector3 v3RegionSize(m_ChunkSize.x, gridDimensions.y, m_ChunkSize.y);

		brickGrid.BeginRegion(bBackBuffer, v3RegionMin, v3RegionSize);

		/* A chunk that has never been admitted holds its ground row and nothing
		   else - UpdateGroundPlane wrote y = 0 and the models have not been
		   stamped yet, which happens after the commit. Scanning the other 127
		   rows for occupancy only rediscovers that they are empty. The rows are
		   still *written*, because the window slice they land in may hold the
		   previous occupant's geometry. */
		const bool bGroundOnly = updateItem.pChunk->IsFirstLoad();

		for (uint32_t z = chunkOffset.y; z < m_ChunkSize.y + chunkOffset.y; ++z)
		{
			for (uint32_t y = 0; y < gridDimensions.y; ++y)
			{
				uint32_t* ptr = viewPortData + chunkOffset.x + y * gridDimensions.x + z * gridDimensions.x * gridDimensions.y;
				Voxel* voxPtr = &voxelData[y * m_ChunkSize.x + (z - chunkOffset.y) * m_ChunkSize.x * gridDimensions.y];

				/* The mapping is write-combined memory. A word at a time gives
				   the CPU nothing to burst, and a Voxel is exactly one uint32_t
				   with a static_assert holding it there (RENDERING_PLAN.md 4d),
				   so a row is a contiguous transfer. */
				std::memcpy(ptr, voxPtr, sizeof(uint32_t) * m_ChunkSize.x);

				if (bGroundOnly && y != 0)
					continue;

				for (uint32_t x = 0; x < m_ChunkSize.x; ++x)
				{
					const uint32_t uiColor = voxPtr[x].Color;

					/* Occupancy is alpha > 0 (rule 3) - the byte is a
					   rendererState tag, not an opacity. */
					if ((uiColor >> 24) != 0)
						brickGrid.AddVoxel(bBackBuffer, chunkOffset.x + x, y, z, uiColor);
				}
			}
		}

		brickGrid.EndRegion(bBackBuffer, v3RegionMin, v3RegionSize);

		StreamingCounters::Get().ChunkRegionsWritten.fetch_add(1, std::memory_order_relaxed);
		StreamingCounters::Get().VoxelWordsWritten.fetch_add(
			uint64_t(m_ChunkSize.x) * m_ChunkSize.y * gridDimensions.y, std::memory_order_relaxed);
	}
}

void ChunkSystem::ClearChunk(UVector2 gridTargetIndex)
{
	OPTICK_EVENT();
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	UVector3 gridDimensions = m_pVoxelGrid->GetDimensions();
	UVector2 chunkOffset = gridTargetIndex * m_ChunkSize;
	uint32_t* viewportColorData = m_pVoxelWindow != nullptr ? m_pVoxelWindow->GetFrontData() : nullptr;

	if (viewportColorData != nullptr && m_pVoxelWindow->GetWordCount() == m_pVoxelGrid->GetNumVoxels())
	{
		for (uint32_t z = chunkOffset.y; z < m_ChunkSize.y + chunkOffset.y; ++z)
		{
			for (uint32_t y = 1; y < gridDimensions.y; ++y)
			{
				//Clear all voxels to 0 except the bottom layer which should be untouched
				memset(viewportColorData + chunkOffset.x + y * gridDimensions.x + z * gridDimensions.x * gridDimensions.y, 
					0, sizeof(uint32_t) * m_ChunkSize.x);
			}
		}
	}

	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	auto execTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	m_pWorld->GetApplication()->GetLoggingSystem().Log(LOGLEVEL_MESSAGE, "ChunkSystem", "Chunk clear (ms): " + std::to_string(execTime.count()));
}

void ChunkSystem::OnChunkLoaded(ChunkUpdateGroup::Item* pUpdateItem)
{
	pUpdateItem->bIsDone = true;

	UVector2 chunkIndex = pUpdateItem->pChunk->GetChunkIndex();
	std::string chunkLoc = "X: " + std::to_string(chunkIndex.x) + " Y: " + std::to_string(chunkIndex.y);
	m_pWorld->GetApplication()->GetLoggingSystem().Log(LOGLEVEL_MESSAGE, "ChunkSystem", "Chunk loaded at " + chunkLoc);
}

void ChunkSystem::AcquireChunkStorage(Chunk& chunk)
{
	/* A resident chunk is 48 MiB of voxels and owner slots (RENDERING_PLAN.md
	   4d). A slide turns over three of them, so without this every transition
	   frees three blocks of that size and allocates three more - which is the
	   allocator and TLB churn the experiment measured as main-thread
	   descheduling, arriving from a completely different direction than the code
	   that appears to be doing the work.
	 *
	 * Simplified against the experiment's version, per ledger E10: no free-list
	 * re-sort (every block in it is the same size by construction), a hard cap,
	 * and a per-world dimension assert instead of a size negotiation. The pool
	 * belongs to this system, so it dies with the world - a second world with a
	 * different chunk size cannot see it. */
	/* Already holds its own - a chunk whose encode was cancelled part-way is
	   still fully resident, so there is nothing to hand it and nothing to
	   count. */
	if (chunk.m_VoxelData.capacity() >= ChunkVoxelCount(chunk))
		return;

	if (m_ChunkStoragePool.empty())
	{
		StreamingCounters::Get().ChunkStorageAllocated.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	ChunkStorage& storage = m_ChunkStoragePool.back();

	assert(storage.Voxels.capacity() >= ChunkVoxelCount(chunk) &&
		"a pooled chunk block is too small for this world's chunks");

	chunk.m_VoxelData = std::move(storage.Voxels);
	chunk.m_OwnerVolume = std::move(storage.Owners);

	m_ChunkStoragePool.pop_back();

	StreamingCounters::Get().ChunkStorageReused.fetch_add(1, std::memory_order_relaxed);
}

void ChunkSystem::RecycleChunkStorage(Chunk& chunk)
{
	/* The encode cleared the logical sizes and left the allocations alone; this
	   is the move that makes that worth doing. Constant time, on the main
	   thread, with the grid slot already detached. */
	if (chunk.m_VoxelData.capacity() < ChunkVoxelCount(chunk) ||
		m_ChunkStoragePool.size() >= k_uiMaxPooledChunkStorage)
	{
		chunk.m_VoxelData.shrink_to_fit();
		chunk.m_OwnerVolume.Release();
		return;
	}

	m_ChunkStoragePool.push_back(
		ChunkStorage{ std::move(chunk.m_VoxelData), std::move(chunk.m_OwnerVolume) });
}

size_t ChunkSystem::ChunkVoxelCount(const Chunk& chunk)
{
	const UVector3 v3Size = chunk.GetChunkSize();

	return static_cast<size_t>(v3Size.x) * v3Size.y * v3Size.z;
}

void ChunkSystem::OnChunkUnloaded(ChunkUpdateGroup::Item* pUpdateItem)
{
	RecycleChunkStorage(*pUpdateItem->pChunk);

	pUpdateItem->bIsDone = true;

	UVector2 chunkIndex = pUpdateItem->pChunk->GetChunkIndex();
	std::string chunkLoc = "X: " + std::to_string(chunkIndex.x) + " Y: " + std::to_string(chunkIndex.y);
	m_pWorld->GetApplication()->GetLoggingSystem().Log(LOGLEVEL_MESSAGE, "ChunkSystem", "Chunk unloaded at " + chunkLoc);
}

void ChunkSystem::OnWorldResumed(World* pWorld)
{
	/* The far field belongs to the render context, not to a world, so whichever
	   world is on top owns it. A pushed menu world's ChunkSystem::Start builds
	   its own - and since every menu world is a single chunk, "its own" is
	   Resize(0,0,0), which throws this level's volume away. Nothing put it back,
	   so opening the pause menu once removed the horizon for the rest of the
	   session. It is a full rebuild (~215 ms); streaming it is phase 4 of
	   Docs/CHUNK_STREAMING_PLAN.md. */
	if (RenderContext* pRenderContext = m_pWorld->GetRenderContext())
		pRenderContext->BeginFarFieldBuild(m_pWorld);

	uint32_t* viewPortData = m_pVoxelWindow != nullptr ? m_pVoxelWindow->GetFrontData() : nullptr;
	for (auto& iter : m_Chunks)
	{
		if (iter.second->IsLoaded())
		{
			ChunkUpdateGroup::Item item(ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD, iter.second, iter.second->GetGridTarget(), false);
			RenderChunk(item, viewPortData, false);
			item.pChunk->UpdateEntities();
		}
	}
}
