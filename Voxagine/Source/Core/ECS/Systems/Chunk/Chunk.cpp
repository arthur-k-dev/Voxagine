#include "pch.h"
#include "Chunk.h"
#include "Core/Application.h"

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Physics/Box.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/Platform/Rendering/FrameProfiler.h"

#include "External/optick/optick.h"

#include <algorithm>
#include <cassert>

/* Encode and decode used to print a line to stderr unconditionally, from a job
   thread, once per chunk. That is noise in every headless capture and a lock on
   a shared FILE* in the middle of a window slide (rule R9 of the streaming
   plan). Read the environment once - both callers run off the main thread. */
static bool ChunkIoTimingsEnabled()
{
	static const bool s_bEnabled = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;
	return s_bEnabled;
}

Chunk::Chunk(Application* pApp, World* pWorld, UVector2 chunkIndex, UVector3 chunkSize, Value& rootEntities)
{
	m_pWorld = pWorld;
	m_pJobManager = &pApp->GetJobManager();
	m_pJsonSerializer = &pApp->GetSerializer();
	m_ChunkIndex = chunkIndex;
	m_ChunkSize = chunkSize;
	m_pTextureReadData = nullptr;

	m_RootEntities.resize(rootEntities.Size());
	for (SizeType i = 0; i < rootEntities.Size(); i++)
	{
		m_RootEntities[i].CopyFrom(rootEntities[i], m_CopyDoc.GetAllocator());
	}
}

Chunk::Chunk(Application* pApp, World* pWorld, UVector2 chunkIndex /*= Vector2(0, 0)*/)
{
	m_pWorld = pWorld;
	m_pJobManager = &pApp->GetJobManager();
	m_pJsonSerializer = &pApp->GetSerializer();
	m_ChunkIndex = chunkIndex;
	m_ChunkSize = DEFAULT_CHUNK_SIZE;
	m_pTextureReadData = nullptr;
}

Chunk::~Chunk()
{
	delete m_pTextureReadData;
}

void Chunk::Load(UVector2 gridTargetIndex)
{
	//Load new chunk data into voxel data vector
	SetGridTarget(gridTargetIndex);

	if (m_bFirstLoad)
	{
		m_VoxelData.resize(m_ChunkSize.x * m_ChunkSize.y * m_ChunkSize.z);
		m_OwnerVolume.Resize(m_VoxelData.size());
		UpdateGroundPlane();
	}

	if (m_bUpdateGroundPlane)
		UpdateGroundPlane();

	LoadEntities();
	m_bIsLoaded = true;
	m_bFirstLoad = false;
}

void Chunk::LoadAsync(ChunkUpdateGroup::Item* pItem, std::function<void(ChunkUpdateGroup::Item*)> callback)
{
	if (m_bIsLoading) return;
	m_bIsLoading = true;

	SetGridTarget(pItem->GridTargetIndex);
	JobQueue* pJobQueue = m_pWorld->GetJobQueue();
	if (pJobQueue)
	{
		pJobQueue->EnqueueWithType<bool>([this]()
		{
			if (m_bFirstLoad)
			{
				m_VoxelData.resize(m_ChunkSize.x * m_ChunkSize.y * m_ChunkSize.z);
				m_OwnerVolume.Resize(m_VoxelData.size());
				UpdateGroundPlane();
			}

			DecodeVoxels();

			if (m_bUpdateGroundPlane)
				UpdateGroundPlane();

			return true;
		}, [this, callback, pItem](bool ret)
		{
			callback(pItem);
			m_bIsLoading = false;
		}, JT_IO);
	}
}

bool Chunk::PrepareUnloadBatch(StreamingBudget::Scope& budget)
{
	const bool bProfiling = FrameProfiler::Get().IsEnabled();

	auto since = [](const std::chrono::steady_clock::time_point& start)
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
	};

	if (!m_bUnloadPrepared)
	{
		m_bIsUnloading = true;
		m_bUnloadPrepared = true;
		m_RootEntities.clear();
		m_SerializedUnloadIds.clear();

		/* And the arena the cleared Values came out of. rapidjson's pool
		   allocator never reuses anything until the Document owning it is
		   destroyed, so every unload of this chunk was adding a fresh copy of
		   its whole root hierarchy to a pool that only grew - for the life of
		   the level, once per transition. Safe here and nowhere else: the only
		   Values it owns are the ones just cleared, and the next lines are what
		   refill it. */
		m_CopyDoc = Document();
	}

	std::chrono::steady_clock::time_point phase =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	/* Re-discovered every slice rather than snapshotted once - the whole of the
	   E1 defense, and cheap enough to be the obvious choice (0.04 ms against a
	   2 ms slice). GetAddedEntities is included because an entity spawned this
	   frame is in this chunk too and would otherwise be left behind in a world
	   whose chunk no longer exists. */
	const std::vector<Entity*>& entities = m_pWorld->GetEntities();
	const std::vector<Entity*>& addedEntities = m_pWorld->GetAddedEntities();
	std::vector<Entity*> combinedEntities(entities);
	combinedEntities.insert(combinedEntities.end(), addedEntities.begin(), addedEntities.end());

	std::vector<std::pair<Entity*, bool>> foundEntities;
	FindEntitiesInChunk(combinedEntities, foundEntities);

	if (bProfiling)
	{
		FrameProfiler::Get().Report("CPU Chunk FindEntitiesInChunk", since(phase));
		phase = std::chrono::steady_clock::now();
	}

	uint32_t uiRoots = 0;

	for (std::pair<Entity*, bool>& found : foundEntities)
	{
		const uint64_t uiId = found.first->GetId();

		if (std::find(m_SerializedUnloadIds.begin(), m_SerializedUnloadIds.end(), uiId) !=
			m_SerializedUnloadIds.end())
			continue;

		/* Destroyed by gameplay since the last slice, or by this pass. Nothing
		   to write: an entity that is going away does not belong in the chunk
		   the player will find when they come back. */
		if (found.first->IsDestroyed())
		{
			m_SerializedUnloadIds.push_back(uiId);
			continue;
		}

		SaveAndDeleteEntity(found.first, found.second);
		m_SerializedUnloadIds.push_back(uiId);

		++uiRoots;
		budget.Consume();

		if (budget.Exhausted())
			break;
	}

	if (bProfiling && uiRoots > 0)
		FrameProfiler::Get().Report("CPU Chunk SaveAndDeleteEntities", since(phase));

	StreamingCounters::RaiseMax(
		StreamingCounters::Get().MaxRootsPerUnloadSlice, uiRoots);
	StreamingCounters::Get().UnloadRootsSerialized.fetch_add(uiRoots, std::memory_order_relaxed);

	/* Done when a whole re-discovery pass found nothing new. That is one extra
	   pass over the world's entities per chunk, and it is what makes "nothing
	   was left behind" an observation rather than an assumption. */
	if (uiRoots != 0)
		return false;

	/* Cleared here rather than only in ResetStreamingState, and the difference
	   is not academic: the ids are what stop re-discovery serializing a root
	   twice *within* one unload, and entity ids are restored from the stored
	   JSON, so the same chunk unloading a second time meets exactly the same
	   ids. Left behind, the second unload skips every root it already knows -
	   writing an empty chunk and leaving its entities in a world that no longer
	   has anywhere to put them. Found by the seeded walk in
	   Tests/Streaming/ChunkUnloadChecks.cpp, which is the argument for having
	   one. */
	m_bUnloadPrepared = false;
	m_SerializedUnloadIds.clear();

	return true;
}

void Chunk::BeginVoxelEncoding()
{
	m_pEncodedVoxelData.clear();

	/* One byte per source voxel, rather than a flat 10 MB that is both too much
	   for a small chunk and not obviously enough for a large one. A run costs
	   seven bytes and covers at least one voxel, so this over-reserves for every
	   world that compresses at all and never reallocates for one that doesn't.

	   Reserved here rather than inside the batch: it is the one allocation the
	   encode makes, and a budgeted loop should not be able to yield in the
	   middle of deciding whether to make it. */
	m_pEncodedVoxelData.reserve(m_VoxelData.size());

	m_uiEncodeCursor = 0;
	m_uiEncodeOccupied = 0;
	m_bEncodePrepared = true;
	m_EncodeStart = std::chrono::steady_clock::now();
}

bool Chunk::EncodeVoxelBatch(StreamingBudget::Scope& budget)
{
	if (!m_bEncodePrepared)
		BeginVoxelEncoding();

	const bool bReportTiming = ChunkIoTimingsEnabled();

	uint32_t uiRuns = 0;

	/* A run is a colour and an owner slot, seven bytes rather than the
	   eighteen the old (bool, colour, uintptr_t, ';', count) record cost - and
	   the runs are longer too, because the slot only changes at a model's
	   boundary where the old raw pointer changed with every particle.

	   Particle claims are deliberately *not* encoded. They name a Particle in
	   a pool that will have recycled it long before this chunk comes back, so
	   the old format was restoring dangling pointers; a chunk far enough away
	   to unload has no debris worth preserving. */
	const uint32_t compressedVoxelSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t);

	while (m_uiEncodeCursor < m_VoxelData.size())
	{
		size_t i = m_uiEncodeCursor;
		uint8_t repeatCount = 0;
		const uint32_t uiColor = m_VoxelData[i].Color;

		uint16_t uiSlot = m_OwnerVolume.GetSlot(static_cast<uint32_t>(i));

		/* The reserved slot is never written since phase 3 deleted particle
		   claims, but it can still be *read* - from chunk data encoded before
		   that, where it meant "a particle holds this". Normalising it to "no
		   owner" on encode is what the codec always did with a claim, and it is
		   what keeps old data from naming a live entity when slots are handed
		   out from a fresh table (rule 4). */
		if (uiSlot == VoxelOwnerVolume::k_uiReservedSlot)
			uiSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;

		while (i + 1 < m_VoxelData.size() && repeatCount < 255 &&
			m_VoxelData[i + 1].Color == uiColor && SlotEqual(m_OwnerVolume.GetSlot(i + 1), uiSlot))
		{
			++repeatCount;
			++i;
		}

		m_uiEncodeCursor = i + 1;

		if (bReportTiming && (uiColor >> 24) != 0)
			m_uiEncodeOccupied += static_cast<uint64_t>(repeatCount) + 1;

		/* Append. The old insert-at-byteOffset always addressed end() anyway, so
		   this says what it does and skips the generic insert's move machinery. */
		size_t writeOffset = m_pEncodedVoxelData.size();
		m_pEncodedVoxelData.resize(writeOffset + compressedVoxelSize);

		memcpy(&m_pEncodedVoxelData[writeOffset], &uiColor, sizeof(uint32_t));
		writeOffset += sizeof(uint32_t);

		memcpy(&m_pEncodedVoxelData[writeOffset], &uiSlot, sizeof(uint16_t));
		writeOffset += sizeof(uint16_t);

		memcpy(&m_pEncodedVoxelData[writeOffset], &repeatCount, sizeof(uint8_t));

		++uiRuns;
		budget.Consume();

		/* Checked after the run, never before: a budget of one unit must still
		   make progress or the state machine wedges rather than slows. */
		if (budget.Exhausted())
			break;
	}

	StreamingCounters::RaiseMax(StreamingCounters::Get().MaxEncodeRunsPerSlice, uiRuns);

	if (m_uiEncodeCursor < m_VoxelData.size())
		return false;

	m_pEncodedVoxelData.shrink_to_fit();

	/* Logical size only. The storage itself moves into ChunkSystem's small pool
	   (E10) and is handed to the next chunk that loads, so a 48 MiB free and a
	   48 MiB allocation are both removed from a window slide rather than being
	   moved somewhere quieter. */
	m_VoxelData.clear();
	m_OwnerVolume.Clear();

	m_bEncodePrepared = false;

	if (bReportTiming)
	{
		const auto execTime = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - m_EncodeStart);

		/* The occupied count either side of the codec is the instrument for M7
		   (CHUNK_STREAMING_PLAN.md): a chunk that took damage must encode fewer
		   occupied voxels than it decoded, and decode back exactly what it
		   encoded. It settled the question - the codec is innocent and the
		   re-stamp on reload was the cause - and it stays because it is the only
		   view of what a chunk actually carried away with it. Wall clock is the
		   whole transition's, not one slice's. */
		fprintf(stderr, "[chunk] (%u,%u) encode %lld ms, %llu occupied\n",
			m_ChunkIndex.x, m_ChunkIndex.y, static_cast<long long>(execTime.count()),
			static_cast<unsigned long long>(m_uiEncodeOccupied));
	}

	return true;
}

void Chunk::EncodeVoxels()
{
	/* The unbounded form, for the callers that are not the state machine:
	   VerifyVoxelCodecRoundTrip's audit and the editor. */
	StreamingBudget unbounded = StreamingBudget::Unbounded();
	StreamingBudget::Scope scope(unbounded);

	BeginVoxelEncoding();

	while (!EncodeVoxelBatch(scope))
	{
	}
}

void Chunk::ResetStreamingState()
{
	/* Nothing in flight. Deliberately an early return rather than an
	   unconditional wipe: a chunk that finished unloading holds its whole volume
	   in m_pEncodedVoxelData, and clearing that here would delete the level. */
	if (!m_bUnloadPrepared && !m_bEncodePrepared)
		return;

	if (m_bEncodePrepared)
	{
		/* A cancelled encode costs nothing to undo, and that is a property of
		   the order rather than of any cleanup here: EncodeVoxelBatch never
		   touches m_VoxelData until its last run, so a chunk whose encode was
		   interrupted is simply still resident with all of its voxels. */
		StreamingCounters::Get().CancelledEncodes.fetch_add(1, std::memory_order_relaxed);

		m_pEncodedVoxelData.clear();
		m_pEncodedVoxelData.shrink_to_fit();
	}

	m_uiEncodeCursor = 0;
	m_uiEncodeOccupied = 0;
	m_bEncodePrepared = false;

	m_bUnloadPrepared = false;
	m_SerializedUnloadIds.clear();
	m_SerializedUnloadIds.shrink_to_fit();

	m_bIsUnloading = false;

	/* T5: the reset is complete or it is not a reset. Every field the unload
	   states write is named here, so the next phase's staging cursors have an
	   obvious place to be added - and a failure names the reset rather than the
	   group that inherited the leftovers. */
	assert(!m_bEncodePrepared && !m_bUnloadPrepared && !m_bIsUnloading &&
		m_uiEncodeCursor == 0 && m_uiEncodeOccupied == 0 &&
		m_SerializedUnloadIds.empty() &&
		"Chunk::ResetStreamingState left streaming state behind");
}

void Chunk::DecodeVoxels()
{
	const bool bReportTiming = ChunkIoTimingsEnabled();
	const std::chrono::steady_clock::time_point start = bReportTiming
		? std::chrono::steady_clock::now()
		: std::chrono::steady_clock::time_point();

	m_VoxelData.resize(m_ChunkSize.x * m_ChunkSize.y * m_ChunkSize.z);
	m_OwnerVolume.Resize(m_VoxelData.size());

	uint32_t byteOffset = 0;
	uint32_t voxelsWritten = 0;
	uint64_t uiOccupied = 0;
	while (byteOffset < m_pEncodedVoxelData.size())
	{
		uint32_t color;
		memcpy(&color, &m_pEncodedVoxelData[byteOffset], sizeof(uint32_t));
		byteOffset += sizeof(uint32_t);

		uint16_t slot;
		memcpy(&slot, &m_pEncodedVoxelData[byteOffset], sizeof(uint16_t));
		byteOffset += sizeof(uint16_t);

		uint8_t repeatCount = m_pEncodedVoxelData[byteOffset];
		byteOffset += sizeof(uint8_t);

		if (bReportTiming && (color >> 24) != 0)
			uiOccupied += static_cast<uint64_t>(repeatCount) + 1;

		for (uint32_t i = 0; i < static_cast<uint32_t>(repeatCount) + 1; ++i)
		{
			m_VoxelData[voxelsWritten].Color = color;

			/* Slots are never recycled, so one recorded at encode time still
			   names the same entity now - which is what makes this safe to do
			   from the decode job without touching the shared slot table. */
			if (slot != VoxelOwnerVolume::k_uiNoOwnerSlot)
				m_OwnerVolume.SetSlot(voxelsWritten, slot);

			++voxelsWritten;
		}
	}

	m_pEncodedVoxelData.clear();
	m_pEncodedVoxelData.shrink_to_fit();

	if (bReportTiming)
	{
		const auto execTime = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start);

		fprintf(stderr, "[chunk] (%u,%u) decode %lld ms, %llu occupied\n",
			m_ChunkIndex.x, m_ChunkIndex.y, static_cast<long long>(execTime.count()),
			static_cast<unsigned long long>(uiOccupied));
	}
}

uint64_t Chunk::VerifyVoxelCodecRoundTrip()
{
	if (m_VoxelData.empty())
		return 0;

	const size_t uiCount = m_VoxelData.size();

	std::vector<uint32_t> colors(uiCount);
	std::vector<uint16_t> slots(uiCount);

	for (size_t i = 0; i < uiCount; ++i)
	{
		colors[i] = m_VoxelData[i].Color;

		const uint16_t uiSlot = m_OwnerVolume.GetSlot(static_cast<uint32_t>(i));

		/* The reserved slot is normalised to "no owner" by the codec, so the
		   expectation for one is that rather than "unchanged". Nothing writes
		   it any more; this is for chunk data older than phase 3. */
		slots[i] = uiSlot == VoxelOwnerVolume::k_uiReservedSlot
			? VoxelOwnerVolume::k_uiNoOwnerSlot
			: uiSlot;
	}

	EncodeVoxels();
	DecodeVoxels();

	uint64_t uiDiverged = 0;

	if (m_VoxelData.size() != uiCount)
		return uiCount;

	for (size_t i = 0; i < uiCount; ++i)
	{
		if (m_VoxelData[i].Color != colors[i] ||
			m_OwnerVolume.GetSlot(static_cast<uint32_t>(i)) != slots[i])
			++uiDiverged;
	}

	return uiDiverged;
}

/* The reserved slot encodes as no owner, so it has to compare as one too or a
   run would break where old chunk data happens to carry it. */
inline bool Chunk::SlotEqual(uint16_t uiSlot, uint16_t uiEncoded)
{
	if (uiSlot == VoxelOwnerVolume::k_uiReservedSlot)
		uiSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;

	return uiSlot == uiEncoded;
}

void Chunk::UpdateRenderer(Entity* pEntity, bool bFirstLoad)
{
	VoxRenderer* pRenderer = pEntity->GetComponent<VoxRenderer>();
	if (pRenderer && bFirstLoad)
		pRenderer->RequestUpdate();

	for (Entity* pChild : pEntity->GetChildren())
		UpdateRenderer(pChild, bFirstLoad);
}

void Chunk::LoadEntities()
{
	OPTICK_EVENT();
	for (SizeType i = 0; i < m_RootEntities.size(); i++)
	{
		Entity* pEntity = m_pJsonSerializer->ValueToEntity(m_RootEntities[i], *m_pWorld);

		if (!pEntity) continue;

		//Don't spawn a persistent entity after the first load
		if (pEntity->IsPersistent() && !m_bFirstLoad)
		{
			if (!pEntity->IsStatic())
			{
				Entity* pFoundEntity = m_pWorld->FindEntity(pEntity->GetId());
				if (pFoundEntity != nullptr)
					UpdateRenderer(pFoundEntity, true);
			}
			continue;
		}

		if (pEntity->IsStatic())
		{
			Entity* pFoundEntity = m_pWorld->FindEntityAll(pEntity->GetId());
			if (pFoundEntity == nullptr)
			{
				m_pJsonSerializer->AddRootEntityToWorld(*m_pWorld, pEntity);
			}
			else
			{
				UpdateRenderer(pFoundEntity, m_bFirstLoad);
				DeleteEntity(pEntity);
				continue;
			}
		}
		else
		{
			m_pJsonSerializer->AddRootEntityToWorld(*m_pWorld, pEntity);
			UpdateRenderer(pEntity, true);
		}

		if (!m_bFirstLoad)
		{
			auto updateInstanceLoaded = [this](Entity* pEntity, const auto& updateInstanceLoadedRef) -> void
			{
				for (Component* pComp : pEntity->GetAddedComponents())
				{
					if (pComp->get_type() == rttr::type::get<VoxRenderer>())
					{
						VoxRenderer* pRenderer = static_cast<VoxRenderer*>(pComp);
						pRenderer->SetChunkInstanceLoaded(true);
						break;
					}
				}

				for (Entity* pChild : pEntity->GetChildren())
					updateInstanceLoadedRef(pChild, updateInstanceLoadedRef);
			};

			updateInstanceLoaded(pEntity, updateInstanceLoaded);
		}
	}
}

void Chunk::UpdateEntities()
{
	OPTICK_EVENT();
	std::vector<std::pair<Entity*, bool>> foundEntities;
	FindEntitiesInChunk(m_pWorld->GetEntities(), foundEntities);

	for (std::pair<Entity*, bool>& foundEntity : foundEntities)
	{
		if (!foundEntity.first->IsStatic())
		{
			UpdateRenderer(foundEntity.first, true);
		}
	}
}

void Chunk::FindEntitiesInChunk(const std::vector<Entity*>& allEntities, std::vector<std::pair<Entity*, bool>>& foundEntities) const
{
	OPTICK_EVENT();
	UVector3 voxelDims = m_pWorld->GetVoxelGrid()->GetDimensions();
	UVector2 dimensions = m_pWorld->GetWorldSize();
	uint32_t numX = dimensions.x / m_ChunkSize.x;
	uint32_t numY = dimensions.y / m_ChunkSize.z;
	const std::unordered_map<uint32_t, Chunk*>& chunks = m_pWorld->GetChunkSystem()->GetChunks();

	for (Entity* pEntity : allEntities)
	{
		if (pEntity->GetParent() == nullptr)
		{
			Vector3 pos = pEntity->GetTransform()->GetPosition();
			int chunkXPos = floor(pos.x / (float)m_ChunkSize.x);
			int chunkYPos = floor(pos.z / (float)m_ChunkSize.z);

			chunkXPos = glm::clamp(chunkXPos, 0, static_cast<int>(numX));
			chunkYPos = glm::clamp(chunkYPos, 0, static_cast<int>(numY));

			// Dynamic entities are found when their transform position is inside the chunk
			if (!pEntity->IsStatic()) 
			{
				if (UVector2(chunkXPos, chunkYPos) == m_ChunkIndex)
					foundEntities.push_back(std::make_pair(pEntity, true));
			}
			else // Static entities are found when its position or render / collider bounds are inside the chunk
			{
				VoxRenderer* pRenderer = pEntity->GetComponent<VoxRenderer>();
				BoxCollider* pBoxCollider = pEntity->GetComponent<BoxCollider>();
				Box bounds;

				//Get the biggest bounding box for the chunk comparison
				if (pRenderer)
					bounds = pRenderer->GetBounds();
				if (pBoxCollider)
				{
					Box colliderBounds(pBoxCollider);
					if (colliderBounds.Max.x > bounds.Max.x) bounds.Max.x = colliderBounds.Max.x;
					if (colliderBounds.Max.z > bounds.Max.z) bounds.Max.z = colliderBounds.Max.z;
					if (colliderBounds.Min.x < bounds.Min.x) bounds.Min.x = colliderBounds.Min.x;
					if (colliderBounds.Min.z < bounds.Min.z) bounds.Min.z = colliderBounds.Min.z;
				}

				int neighborCountX = static_cast<int>(floor(bounds.GetSize().x / (float)m_ChunkSize.x)) + 1;
				int neighborCountY = static_cast<int>(floor(bounds.GetSize().z / (float)m_ChunkSize.y)) + 1;

				//Check if neighboring chunks contain the entity bounds
				bool deleteEntity = true;
				bool isInside = UVector2(chunkXPos, chunkYPos) == m_ChunkIndex;
				for (int x = -neighborCountX; x <= neighborCountX; ++x)
				{
					for (int y = -neighborCountY; y <= neighborCountY; ++y)
					{
						int neighborChunkX = chunkXPos + x;
						int neighborChunkY = chunkYPos + y;

						if (neighborChunkX < 0 || neighborChunkX >= static_cast<int>(numX)) continue;
						if (neighborChunkY < 0 || neighborChunkY >= static_cast<int>(numY)) continue;

						Box chunkBox;
						chunkBox.Max = Vector3((neighborChunkX + 1) * m_ChunkSize.x, voxelDims.y, (neighborChunkY + 1) * m_ChunkSize.z);
						chunkBox.Min = Vector3(neighborChunkX * m_ChunkSize.x, 0.f, neighborChunkY * m_ChunkSize.z);

						Chunk* pNeighborChunk = chunks.at((uint32_t)(neighborChunkX + neighborChunkY * numX));
						if (bounds.Intersects(chunkBox))
						{
							if (static_cast<uint32_t>(neighborChunkX) == m_ChunkIndex.x && static_cast<uint32_t>(neighborChunkY) == m_ChunkIndex.y)
								isInside = true;
							//Push deletion responsibility to another chunk if its loaded and not unloading
							else if (pNeighborChunk->IsLoaded() && !pNeighborChunk->IsUnloading())
							{
								deleteEntity = false;
								if (isInside)
									break;
							}
						}
					}
					if (isInside && !deleteEntity)
						break;
				}

				if (isInside)
					foundEntities.push_back(std::make_pair(pEntity, deleteEntity));
			}
		}
	}
}

void Chunk::SaveAndDeleteEntity(Entity* pEntity, bool bDelete)
{
	OPTICK_EVENT();

	Value entityVal(kObjectType);
	m_pJsonSerializer->EntityToValue(pEntity, entityVal, m_CopyDoc.GetAllocator());

	m_RootEntities.emplace_back();
	m_RootEntities.back().CopyFrom(entityVal, m_CopyDoc.GetAllocator());

	//Delete entity only when it needs to be done by this chunk and is not destroyed already and isn't persistent
	if (!bDelete || pEntity->IsDestroyed() || pEntity->IsPersistent())
		return;

	/* The window this chunk was drawn in has already been replaced (US_COMMIT
	   runs two states earlier), so a departing renderer's recorded voxel
	   positions no longer name its own voxels - they name whatever slid in
	   underneath. VoxelBaker::Clear shifts them by the offset delta and declines
	   to erase a cell some *other* renderer owns, which covers models but not
	   the ground row or settled debris: neither has an owner, so both would be
	   erased out of the freshly published window. Tell each renderer in the
	   departing hierarchy that its destruction is an unload, so the stamp is
	   forgotten rather than replayed. Its colours are in this chunk's own voxels
	   and are about to be encoded with them. */
	auto markChunkUnloading = [](Entity* pCurrent, const auto& markRef) -> void
	{
		if (VoxRenderer* pRenderer = pCurrent->GetComponentAll<VoxRenderer>())
			pRenderer->MarkChunkUnloading();

		for (Entity* pChild : pCurrent->GetChildren())
			markRef(pChild, markRef);
	};

	markChunkUnloading(pEntity, markChunkUnloading);

	pEntity->Destroy();
}

void Chunk::DeleteEntity(Entity* pEntity)
{
	for (Entity* pChild : pEntity->GetChildren())
		DeleteEntity(pChild);

	delete pEntity;
}

void Chunk::SetGroundPlane(const std::string& texturePath)
{
	if (!texturePath.empty())
	{
		if (m_pTextureReadData != nullptr)
		{
			delete m_pTextureReadData;
		}
		m_pTextureReadData = m_pWorld->GetApplication()->GetPlatform().GetRenderContext()->ReadTexture(texturePath);
		m_bUpdateGroundPlane = true;
	}

	if (m_VoxelData.size() > 0)
	{
		UpdateGroundPlane();
	}
}

void Chunk::UpdateGroundPlane()
{
	uint32_t id = 0;
	uint32_t color = VColor(static_cast<unsigned char>(50), 50, 50, 255).inst.Color;

	bool bHasData = m_pTextureReadData && m_pTextureReadData->m_Data && m_pTextureReadData->m_Dimensions.x > 0 && m_pTextureReadData->m_Dimensions.y > 0;

	for (uint32_t z = 0; z < m_ChunkSize.z; ++z)
	{
		for (uint32_t x = 0; x < m_ChunkSize.x; ++x)
		{
			if (bHasData)
			{
				id = x % m_pTextureReadData->m_Dimensions.x + ((m_pTextureReadData->m_Dimensions.y - 1 - z) * m_pTextureReadData->m_Dimensions.x) % (m_pTextureReadData->m_Dimensions.x * (m_pTextureReadData->m_Dimensions.y));
				color = m_pTextureReadData->m_Data[id];
			}

			/* Occupancy comes from the colour's alpha now, and every ground
			   texture is opaque - the 75.5 M-voxel audit found no voxel that
			   was active without one.

			   The alpha is *replaced* by a tag rather than trusted, because the
			   top byte stopped being an opacity in RENDERING_PLAN.md 7.4: a
			   ground texel's 255 would otherwise set every reserved bit,
			   VOXEL_EMISSIVE_TAG included, and the whole floor of the level
			   would glow. RS_DEFAULT is what the ground is. */
			m_VoxelData[x + z * m_ChunkSize.y * m_ChunkSize.x].Color =
				(color & 0x00FFFFFFu) | VoxelStateTag(RS_DEFAULT, false);
		}
	}

	m_bUpdateGroundPlane = false;
}
