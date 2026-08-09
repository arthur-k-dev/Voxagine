#include "pch.h"
#include "Chunk.h"
#include "Core/Application.h"

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Physics/Box.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/Platform/Rendering/FrameProfiler.h"

#include "External/optick/optick.h"

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
		pJobQueue->Enqueue<bool>([this]()
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
		});
	}
}

void Chunk::UnloadAsync(ChunkUpdateGroup::Item* pItem, std::function<void(ChunkUpdateGroup::Item*)> callback)
{
	if (m_bIsUnloading) return;
	m_bIsUnloading = true;

	/* Everything down to the Enqueue below is on the *main thread*, despite the
	   name - only EncodeVoxels is a job. Measured in two halves because they
	   are different costs: the search walks every entity in the world against
	   this chunk's neighbours, while the save is full RTTR serialization of
	   each entity it found into JSON. */
	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	std::chrono::steady_clock::time_point phase =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	auto since = [](const std::chrono::steady_clock::time_point& start)
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
	};

	const std::vector<Entity*>& entities = m_pWorld->GetEntities();
	const std::vector<Entity*>& addedEntities = m_pWorld->GetAddedEntities();
	std::vector<Entity*> combinedEntities = std::vector<Entity*>(entities);
	combinedEntities.insert(combinedEntities.end(), addedEntities.begin(), addedEntities.end());

	std::vector<std::pair<Entity*, bool>> foundEntities;
	FindEntitiesInChunk(combinedEntities, foundEntities);

	if (bProfiling)
	{
		FrameProfiler::Get().Report("CPU Chunk FindEntitiesInChunk", since(phase));
		phase = std::chrono::steady_clock::now();
	}

	SaveAndDeleteEntities(foundEntities);

	if (bProfiling)
		FrameProfiler::Get().Report("CPU Chunk SaveAndDeleteEntities", since(phase));

	JobQueue* pJobQueue = m_pWorld->GetJobQueue();
	if (pJobQueue)
	{
		pJobQueue->Enqueue<bool>([this, combinedEntities]()
		{
			EncodeVoxels();
			return true;
		}, [this, pItem, callback](bool ret)
		{
			callback(pItem);
			m_bIsLoaded = false;
			m_bIsUnloading = false;
		});
	}
}

void Chunk::EncodeVoxels()
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	m_pEncodedVoxelData.clear();

	//Reserve data vector to at least 10mb
	m_pEncodedVoxelData.reserve(10000000);

	/* A run is a colour and an owner slot, seven bytes rather than the
	   eighteen the old (bool, colour, uintptr_t, ';', count) record cost - and
	   the runs are longer too, because the slot only changes at a model's
	   boundary where the old raw pointer changed with every particle.

	   Particle claims are deliberately *not* encoded. They name a Particle in
	   a pool that will have recycled it long before this chunk comes back, so
	   the old format was restoring dangling pointers; a chunk far enough away
	   to unload has no debris worth preserving. */
	const uint32_t compressedVoxelSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t);
	uint8_t repeatCount = 0;
	uint32_t byteOffset = 0;
	for (size_t i = 0; i < m_VoxelData.size(); ++i)
	{
		repeatCount = 0;
		const uint32_t uiColor = m_VoxelData[i].Color;
		uint16_t uiSlot = m_OwnerVolume.GetSlot(static_cast<uint32_t>(i));

		if (uiSlot == VoxelOwnerVolume::k_uiParticleSlot)
			uiSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;

		while (i + 1 < m_VoxelData.size() && repeatCount < 255 &&
			m_VoxelData[i + 1].Color == uiColor && SlotEqual(m_OwnerVolume.GetSlot(i + 1), uiSlot))
		{
			++repeatCount;
			++i;
		}

		m_pEncodedVoxelData.insert(m_pEncodedVoxelData.begin() + byteOffset, compressedVoxelSize, 0);

		memcpy(&m_pEncodedVoxelData[byteOffset], &uiColor, sizeof(uint32_t));
		byteOffset += sizeof(uint32_t);

		memcpy(&m_pEncodedVoxelData[byteOffset], &uiSlot, sizeof(uint16_t));
		byteOffset += sizeof(uint16_t);

		memcpy(&m_pEncodedVoxelData[byteOffset], &repeatCount, sizeof(uint8_t));
		byteOffset += sizeof(uint8_t);
	}

	m_pEncodedVoxelData.shrink_to_fit();
	m_VoxelData.resize(0);
	m_VoxelData.shrink_to_fit();
	m_OwnerVolume.Release();

	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	auto execTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	std::string message = "Chunk encode (ms): " + std::to_string(execTime.count()) + "\n";
	fprintf(stderr, "%s", message.c_str());
}

void Chunk::DecodeVoxels()
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	m_VoxelData.resize(m_ChunkSize.x * m_ChunkSize.y * m_ChunkSize.z);
	m_OwnerVolume.Resize(m_VoxelData.size());

	uint32_t byteOffset = 0;
	uint32_t voxelsWritten = 0;
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

	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	auto execTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	std::string message = "Chunk decode (ms): " + std::to_string(execTime.count()) + "\n";
	fprintf(stderr, "%s", message.c_str());
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

		/* A particle claim is deliberately dropped by the codec, so the
		   expectation for one is "no owner" rather than "unchanged". */
		slots[i] = uiSlot == VoxelOwnerVolume::k_uiParticleSlot
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

/* A particle claim encodes as no owner, so it has to compare as one too or a
   run would break on debris that is about to be discarded anyway. */
inline bool Chunk::SlotEqual(uint16_t uiSlot, uint16_t uiEncoded)
{
	if (uiSlot == VoxelOwnerVolume::k_uiParticleSlot)
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

void Chunk::SaveAndDeleteEntities(std::vector<std::pair<Entity*, bool>>& entities)
{
	OPTICK_EVENT();
	m_RootEntities.clear();
	m_RootEntities.resize(entities.size());

	for (size_t i = 0; i < entities.size(); ++i)
	{
		std::pair<Entity*, bool>& entityPair = entities[i];

		Value entityVal(kObjectType);
		m_pJsonSerializer->EntityToValue(entityPair.first, entityVal, m_CopyDoc.GetAllocator());
		m_RootEntities[i].CopyFrom(entityVal, m_CopyDoc.GetAllocator());

		//Delete entity only when it needs to be done by this chunk and is not destroyed already and isn't persistent
		if (entityPair.second && !entityPair.first->IsDestroyed() && !entityPair.first->IsPersistent())
		{
			entityPair.first->Destroy();
		}
	}
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
			   was active without one. */
			m_VoxelData[x + z * m_ChunkSize.y * m_ChunkSize.x].Color = color;
		}
	}

	m_bUpdateGroundPlane = false;
}
