#include "pch.h"
#include "ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Components/ChunkViewer.h"
#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Rendering/DebugRenderer.h"

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
	ChunkUpdateGroup group(gridOffset.x + gridOffset.y * m_uiNumChunkX, worldOffset);
	UpdateChunks(gridOffset, group, false);

	//Custom code for start only, it needs to load the chunks synchronously the first time
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

	SetGroundPlane(m_pWorld->GetGroundTexturePath());

	/* Far-field LOD volume (RENDERING_PLAN.md phase 4). Here rather than in
	   JsonSerializer::DeserializeWorld because it needs every chunk registered
	   with this system and the voxel grid sized, both of which are only true
	   once the system exists - and it is the chunk window's own limit that the
	   far field exists to work around, so this is where it belongs. */
	if (RenderContext* pRenderContext = m_pWorld->GetRenderContext())
		pRenderContext->BuildFarField(m_pWorld);
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
}

void ChunkSystem::FixedTick(const GameTimer& fixedTimer)
{
	Vector3 cameraPosition = m_pWorld->GetMainCamera()->GetTransform()->GetPosition() + m_CameraLoadOffset;
	int chunkXPos = static_cast<int>(floor((cameraPosition.x) / (float)m_ChunkSize.x));
	int chunkYPos = static_cast<int>(floor((cameraPosition.z) / (float)m_ChunkSize.y));
	chunkXPos = std::min(std::max(chunkXPos, 0), (int)m_uiNumChunkX);
	chunkYPos = std::min(std::max(chunkYPos, 0), (int)m_uiNumChunkY);

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
	const Vector3 cameraPosition = pMainCamera->GetTransform()->GetPosition();
	const Vector3 cameraForward = pMainCamera->GetTransform()->GetForward();

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
				item.pChunk->LoadAsync(&item, std::bind(&ChunkSystem::OnChunkLoaded, this, std::placeholders::_1));
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
		if (group.IsRendering()) break;
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
		group.SetState(ChunkUpdateGroup::UpdateState::US_LOADING_ENTITIES);
		break;
	}
	case ChunkUpdateGroup::UpdateState::US_LOADING_ENTITIES:
	{
		/* Main-thread and, as of phase 1, still unbounded - 41.6 ms per
		   incoming chunk (CHUNK_STREAMING_PLAN.md phase 0's baseline). Phases 2
		   and 3 make it resumable against StreamingBudgets::EntityWork; this
		   phase moved it rather than shrinking it.

		   Moving it is not cosmetic. It runs *after* the commit now, so a
		   first-load chunk's static renderers stamp against the world offset
		   and the chunk slots they will actually be drawn at. Before, the
		   callback repointed the grid's chunk slots, then stamped against the
		   *outgoing* offset, then swapped the buffer those stamps went into. */
		const bool bProfiling = FrameProfiler::Get().IsEnabled();

		auto now = []() { return std::chrono::steady_clock::now(); };
		auto since = [](const std::chrono::steady_clock::time_point& start)
		{
			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
		};

		std::chrono::steady_clock::time_point phase =
			bProfiling ? now() : std::chrono::steady_clock::time_point();

		uint32_t uiLoaded = 0;
		uint64_t uiRoots = 0;

		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_LOAD)
			{
				uiRoots += item.pChunk->GetRootEntities().size();

				item.pChunk->LoadEntities();
				item.pChunk->m_bIsLoaded = true;
				item.pChunk->m_bFirstLoad = false;

				++uiLoaded;
			}
			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_MOVE)
				item.pChunk->UpdateEntities();
		}

		/* T4. Unbounded on master and unbounded now; the number is what phases
		   2 and 3 ratchet down, and recording it here is what makes "bounded
		   work per tick" a value CI can compare rather than a comment. */
		StreamingCounters::RaiseMax(StreamingCounters::Get().MaxRootsPerEntityPass, uiRoots);

		if (bProfiling)
		{
			FrameProfiler::Get().Report(
				uiLoaded > 0 ? "CPU Chunk LoadEntities" : "CPU Chunk UpdateEntities", since(phase));
			phase = now();
		}

		// Unload chunk with entities
		for (ChunkUpdateGroup::Item& item : group.GetItems())
		{
			if (item.ItemTarget == ChunkUpdateGroup::Item::Target::T_ASYNC_UNLOAD)
			{
				/* Detach before enqueuing, not after it finishes. UnloadAsync's
				   job body calls EncodeVoxels, which frees this chunk's voxel
				   vector and owner volume on a worker thread - and the grid
				   still pointed at both, so a main-thread GetCell could read
				   freed storage (ledger P7). Nulling the slot here, on this
				   thread, before the job exists, means a reader sees "not
				   resident" instead. Every grid accessor already handles that;
				   since phase 1 they all check for it. */
				m_pVoxelGrid->DetachChunkStorage(&item.pChunk->GetVoxelData());

				item.bIsDone = false;
				item.pChunk->UnloadAsync(&item, std::bind(&ChunkSystem::OnChunkUnloaded, this, std::placeholders::_1));
			}
		}

		if (bProfiling)
			FrameProfiler::Get().Report("CPU Chunk Unload", since(phase));

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
	if (!iter->HasCommitted())
		StreamingCounters::Get().CancelledGroups.fetch_add(1, std::memory_order_relaxed);

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

	Camera* pMainCamera = m_pWorld->GetMainCamera();
	pMainCamera->SetCameraOffset(group.GetWorldOffset());
	pMainCamera->GetTransform()->SetFromMatrix(pMainCamera->GetTransform()->GetMatrix());
	pMainCamera->Recalculate();
	pMainCamera->ForceUpdate();

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

void ChunkSystem::OnChunkUnloaded(ChunkUpdateGroup::Item* pUpdateItem)
{
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
		pRenderContext->BuildFarField(m_pWorld);

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
