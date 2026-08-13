#pragma once

#include <unordered_set>
#include <vector>
#include "Core/Event.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/Objects/TSubclass.h"
#include "Core/Math.h"
#include "Core/Threading/JobManager.h"

class ChunkSystem;
class Entity;
class ComponentSystem;
class Application;
class Component;
class RenderSystem;
class RenderContext;
class AudioSystem;
class VoxelGrid;
class Camera;
class PhysicsSystem;
struct HitResult;
class DebugRenderer;
class GameTimer;
class World
{
public:
	friend class JsonSerializer;

	World(Application* pApp);
	virtual ~World();

	Event<Entity*> EntityAdded;
	Event<Entity*> EntityRemoved;
	Event<World*> Paused;
	Event<World*> Resumed;

	/* Setup functions */
	virtual void Initialize();
	bool PreLoad(const std::string& filePath);

	/* Build the systems for a world whose content is already deserialized.

	   bCreateRenderSystem is false only for a world with no render context -
	   the streaming harness (CHUNK_STREAMING_PLAN.md T1). Such a world can be
	   Initialize()d, its systems Start()ed, its ChunkSystem driven, and - as of
	   phase 3 - run through Tick/FixedTick, which is what makes R1's gameplay
	   hold expressible as a check rather than only as a comment. */
	void PreLoad(bool bCreateRenderSystem = true);

	/* bReleaseSharedRenderState is false for exactly one caller: the loading
	   screen at the moment the world streamed in behind it is activated. That
	   world already owns the voxel window and the far-field build, so the
	   overlay leaving must not cancel or clear either - which is what an
	   ordinary unload does, and doing it here would wipe the level that was
	   just built. Docs/CHUNK_STREAMING_PLAN.md phase 8. */
	virtual void Unload(bool bReleaseSharedRenderState = true);

	/* Empty functions used in the future */
	void Pause();
	void Resume();

	/* Processes the add and remove queues for entities and components */
	virtual void PreTick();

	/* R1 of Docs/CHUNK_STREAMING_PLAN.md: gameplay never ticks against a
	   missing initial window. True while this world's ChunkSystem has not yet
	   published its first resident window and admitted its roots - during which
	   Tick and FixedTick advance the chunk system (and the renderer, so the
	   frame still presents) and nothing else.

	   This is the single rule that replaces the experiment's Bootstrap entity
	   metadata, its Player::SetPersistent-in-constructor and its PhysicsBody
	   non-resident-ground freeze (ledger E7, E8): all three were gameplay
	   defending itself against a world that had not arrived. PreTick is
	   deliberately *not* held - entity admission and link resolution are how the
	   world arrives. */
	bool IsGameplayHeld() const;

	/* How many PreTicks a serialized entity reference may wait for both of its
	   ends to be in the world before it is given up on and counted
	   (StreamingCounters::WorldLinksAbandoned). Four seconds at 60 Hz, against a
	   worst case of roughly sixty frames: three incoming chunks' static art at
	   sixteen roots a frame, plus the unload states behind it. Generous on
	   purpose - the cost of a too-small number is a link that silently never
	   forms, and the cost of a too-large one is a few pointer comparisons a
	   frame on a level that references something it no longer contains. */
	static constexpr uint32_t k_uiMaxWorldLinkRetries = 240;

	/* Bumped every time an entity enters or leaves the world.
	 *
	 * The link budget above is spent only when this has *not* moved since the
	 * last attempt - see JsonSerializer::ResolveWorldLinks. A streamed level
	 * admits the entity a link is waiting for whenever the player walks to its
	 * chunk, which may be minutes; a budget counted in frames is a deadline
	 * that loses that race, and it did. Counting "tries that learned nothing"
	 * instead keeps the bound doing its job - a link to an entity the level no
	 * longer contains still expires, because a settled world stops moving this
	 * number - without ever expiring one whose end is simply not resident yet. */
	uint64_t GetEntityPopulationGeneration() const { return m_uiEntityPopulationGeneration; }

	/* Called when chunk streaming admits a root, which is the *only* event that
	   can give a pending link a new chance. Bullets and monsters spawning move
	   the population too and are noise here: counting those, a link waiting for
	   a chunk three hundred metres away burns its whole budget on a firefight
	   somewhere else. */
	void NoteStreamedRootAdmitted() { ++m_uiEntityPopulationGeneration; }

	/* Every entity id the level file contains, across all chunks, collected
	   while loading. It answers the one question the link retry budget could
	   never ask: *can this end ever arrive?*
	 *
	 * An id that is in here is in a chunk, and a chunk arrives when the player
	 * walks to it - which may be minutes, and no frame count can wait that long
	 * without also refusing to give up on a link that is genuinely broken. An id
	 * that is not in here has no entity coming, so the bound still applies to
	 * it. Empty for a world with no chunk data, which leaves the old behaviour
	 * exactly as it was. */
	void SetKnownEntityIds(std::unordered_set<uint64_t>&& ids) { m_KnownEntityIds = std::move(ids); }
	bool LevelContainsEntityId(uint64_t uiId) const { return m_KnownEntityIds.find(uiId) != m_KnownEntityIds.end(); }
	bool HasKnownEntityIds() const { return !m_KnownEntityIds.empty(); }

	/* Processes Start and Tick functions on entities, components and systems */
	virtual void Tick(float fDeltaTime);

	/* Processes the FixedTick function for all entities and systems */
	virtual void FixedTick(const GameTimer& fixedTimer);

	/* Processes the PostFixedTick function for all entities and systems */
	virtual void PostFixedTick(const GameTimer& fixedTimer);

	/* Processes PostTick functions on entities and systems */
	virtual void PostTick(float fDeltaTime);

	/* Enables user to draw debug data anytime */
	void OnDrawGizmos(float fDeltaTime);

	/* Processes the Render function on the systems */
	void Render(const GameTimer& fixedTimer);

	/* Functions for registering entities, components and systems to the world */
	void RegisterComponent(Component* pComponent);
	void RegisterComponents(const std::vector<Component*>& components);

	/* Adds a single entity to the world, this does not include his children */
	void AddEntity(Entity* pEntity);

	/* Adds a entity to the world which also adds the entire child tree of this entity */
	void AddRootEntity(Entity* pRootEntity);

	void RemoveEntity(Entity* pEntity);
	void SetSystems(std::vector<ComponentSystem*> systems);

	template<typename T>
	T* SpawnEntity(Vector3 position, Vector3 rotation, Vector3 scale);

	template<typename T>
	T* SpawnEntity(Vector3 position, Quaternion rotation, Vector3 scale);

	template<typename T>
	TSubclass<T>& SpawnEntity(TSubclass<T>& subclass, Vector3 position, Quaternion rotation, Vector3 scale);

	Entity* SpawnEntity(rttr::type entityType, Vector3 position, Quaternion rotation, Vector3 scale);

	template<typename T>
	T* GetSystem();

	ChunkSystem* GetChunkSystem() const { return m_pChunkSystem; }
	RenderSystem* GetRenderSystem() const { return m_pRenderSystem; }
	PhysicsSystem* GetPhysics() const { return m_pPhysicsSystem; }
	DebugRenderer* GetDebugRenderer() const;

	/* The render context, or null. Null is an ordinary state in two situations
	   that both matter: a backend that failed to start (the world is already
	   built by the time Application reports it), and a world constructed with
	   no render context at all - which is how the streaming harness drives a
	   real ChunkSystem with no GPU (CHUNK_STREAMING_PLAN.md T1). Ask through
	   here rather than chaining Application -> Platform -> GetRenderContext, so
	   the check is impossible to forget. */
	RenderContext* GetRenderContext() const;

	template <typename T>
	std::vector<T*> FindEntitiesOfType();

	template<typename T>
	std::vector<Entity*> FindEntitiesWithComponent();

	/* Functions for searching entities in the world */
	Entity* FindEntity(std::string name);
	Entity* FindEntity(uint64_t uiId);
	//Searches all lists including pending add and pending kill entities
	Entity* FindEntityAll(uint64_t uiId);
	std::vector<Entity*> FindEntities(std::string name);
	std::vector<Entity*> FindEntitiesWithTag(std::string tag);
	bool FindEntityWithTag(std::string tag);
	const std::vector<Entity*>& GetEntities();
	const std::vector<Entity*>& GetAddedEntities();

	/* Forwarding RayCast function to PhysicsSystem */
	bool RayCast(Vector3 start, Vector3 dir, HitResult& hitResult, float fLength = FLT_MAX, uint32_t uiLayer = -1);
	const VoxelGrid* GetVoxelGrid();
	void ApplySphericalDestruction(const Vector3& position, float fRadius, float fForceMin, float fForceMax, bool bBakeParticles = true);

	/* Forwarding time functions */
	float GetDeltaSeconds();
	float GetTotalSeconds();

	bool IsPreLoaded() const;
	Application* GetApplication() const;
	void SetMainCamera(Camera* pCamera);
	Camera* GetMainCamera() const { return m_pCameraEntity; }
	std::string GetName() const { return m_WorldName; }
	UVector2 GetWorldSize() const;

	const std::string& GetGroundTexturePath() const { return m_GroundTexturePath; };
	void SetGroundTexturePath(const std::string& texturePath);

	void SetWorldName(std::string name) { m_WorldName = name; }

	//Loads a new world
	//Set replace to true if the new worlds needs to replace the current world
	void OpenWorld(const std::string& worldName, bool bReplace = true);

	//Loads a new world asynchronously
	//Set replace to true if the new worlds needs to replace the current world
	/* bWaitForInitialStreaming replaces the visible world only once the new
	   one's first window is resident, keeping the current one on screen and
	   ticking meanwhile - which is only correct when the current one is a
	   loading screen. Docs/CHUNK_STREAMING_PLAN.md phase 8. Ignored when
	   bReplace is false: a pushed world is an overlay, not a replacement. */
	void OpenWorldAsync(const std::string& worldName, bool bReplace = true,
	                    bool bWaitForInitialStreaming = false);

	JobQueue* GetJobQueue();

protected:
	void SetPhysicsSystem(PhysicsSystem* pNewPhysicsSystem);
	void SetAudioSystem(AudioSystem * pNewAudioSystem);
	void SetRenderSystem(RenderSystem* pNewRenderSystem);
	void SetChunkSystem(ChunkSystem* pNewChunkSystem);

	std::vector<ComponentSystem*> m_Systems;
	RenderSystem* m_pRenderSystem;
	PhysicsSystem* m_pPhysicsSystem;
	AudioSystem* m_pAudioSystem;
	ChunkSystem* m_pChunkSystem;
	QueueHandle m_JobQueueHandle;

	void DeleteEntityFromLists(Entity* pEntity);
	std::vector<Entity*>& GetRemovedEntities();

	void DeleteComponentFromEntityLists(Entity* pEntity, Component* pComponent);
	std::vector<Component*>& GetRemovedComponentsFromEntity(Entity* pEntity);

	void AddRemovedEntityToWorld(Entity* pEntity);
	Entity* RemoveEntityFromWorld(Entity* pEntity);
	bool RemoveEntityChildsFromWorld(Entity* pEntityChild);

private:
	/**
	 * @brief EntityConnection - a struct that defines information about the connected
	 * that needs to be made.
	 */
	struct WorldConnectionInformation
	{
		/* **The source is an identity, not a pointer, and that is M3.** This
		   used to hold a raw `rttr::instance` of the entity or component being
		   deserialized, and ResolveWorldLinks dereferenced it a frame or more
		   later to ask two questions: which entity owns you, and what type are
		   you. Both are answerable without holding the object - and holding it
		   was a use-after-free waiting for its case, which chunk streaming
		   supplies three of: a staged root deleted because the entity is already
		   in the world, a persistent duplicate, and a chunk unloaded between
		   deserialization and the next PreTick. Rule R4.

		   Resolving by identity also fixes a defect nobody had named: where a
		   static root is deserialized again and discarded because the world
		   already has it, the link now lands on the *live* entity rather than on
		   the copy that is about to be deleted. */
		uint64_t uiSourceEntityId = 0;

		/* Invalid/nullptr_t means the source is the entity itself; otherwise the
		   component of this type on it. */
		rttr::type rSourceComponentType = rttr::type::get<nullptr_t>();

		/* rttr::property has no default constructor; an invalid one is what
		   get_property returns for a name a type does not have, and it is the
		   only way to spell "not set yet" here. */
		rttr::property rProperty = rttr::type::get<nullptr_t>().get_property("");
		rttr::type rType = rttr::type::get<nullptr_t>();
		int64_t iEntityId = -1;
		int iIndex = -1;

		/* K6: a link whose target has not been admitted yet is retried, not
		   dropped. Streaming means the two ends of a cross-chunk reference
		   arrive in different frames, and dropping the first one is exactly the
		   transient null the experiment patched per manager (ledger E3). */
		uint32_t uiAttempts = 0;

		/* The entity population generation the last attempt was made against.
		   An attempt only costs a retry when this has not moved - see
		   GetEntityPopulationGeneration. */
		uint64_t uiLastPopulationGeneration = 0;
	};

	/* A link that *has* been made, remembered so that destroying the target can
	   null the pointer to it. M8.

	   Making a link writes a raw `Entity*` into a reflected property and nothing
	   afterwards knows it is there - so when chunk streaming destroys the target
	   (which is the ordinary case: the two ends of a cross-chunk reference are
	   in different chunks and one of them leaves first), every holder is left
	   with a dangling pointer. It is a use-after-free the moment anything reads
	   it, and the *serializer* reads it: `VariantToValue` dereferences an
	   `Entity*` property to write its id, so unloading the holder's chunk
	   crashes. Found by ASan through
	   Tests/Streaming/EntityStreamingChecks.cpp once StreamingProbeEntity gave
	   the suite a reflected `Entity*` at all - no engine type has one, which is
	   why nothing had ever caught it.

	   Same shape as the main-camera fix above and as
	   World::GetRenderContext: turn a dangling pointer into a state every
	   reader can check for. */
	struct EntityLinkRecord
	{
		uint64_t uiTargetId = 0;
		uint64_t uiSourceEntityId = 0;
		rttr::type rSourceComponentType = rttr::type::get<nullptr_t>();
		rttr::property rProperty = rttr::type::get<nullptr_t>().get_property("");
		int iIndex = -1;
	};

	Application* m_pApplication;
	std::vector<Entity*> m_Entities;
	std::vector<Entity*> m_AddedEntities;
	std::vector<Entity*> m_RemovedEntities;
	Camera* m_pCameraEntity;

	/* Latched once per frame in PreTick - see IsGameplayHeld. Starts true so a
	   world cannot tick gameplay before its first PreTick has decided. */
	bool m_bGameplayHeldThisFrame = true;

	std::string m_WorldName;
	bool m_bPreLoaded;

	std::string m_GroundTexturePath = "";

	std::vector<WorldConnectionInformation> m_vWorldConnections = {};

	/* Grows by one per link actually made and shrinks when either end dies, so
	   it is bounded by the number of live cross-entity references - tens, in
	   every shipped level. */
	std::vector<EntityLinkRecord> m_vEntityLinks = {};

	/* See GetEntityPopulationGeneration. */
	uint64_t m_uiEntityPopulationGeneration = 0;

	/* See SetKnownEntityIds. */
	std::unordered_set<uint64_t> m_KnownEntityIds = {};

	void DeleteEntity(Entity* pEntity);
};

template<typename T>
T* World::SpawnEntity(Vector3 position, Vector3 rotation, Vector3 scale)
{
	Quaternion q = Quaternion(Vector3(rotation.x, rotation.y, rotation.z));
	return SpawnEntity<T>(position, q, scale);
}

template<typename T>
T* World::SpawnEntity(Vector3 position, Quaternion rotation, Vector3 scale)
{
	static_assert(std::is_base_of<Entity, T>::value, "Type must derive from Entity");
	T* entity = new T(this);
	Transform* transform = entity->GetTransform();
	transform->SetPosition(position);
	transform->SetRotation(rotation);
	transform->SetScale(scale);
	AddEntity(entity);

	return entity;
}

template<typename T>
TSubclass<T>& World::SpawnEntity(TSubclass<T>& subclass, Vector3 position, Quaternion rotation, Vector3 scale)
{
	static_assert(std::is_base_of<Entity, T>::value, "Type must derive from Entity");

	if(rttr::type::get<Entity*>().is_base_of(subclass.get_derived_type()) && subclass.get_derived_type().is_valid())
	{
		rttr::variant entityVar = subclass.get_derived_type().create({ this });
		subclass.m_pClassReference = entityVar.get_value<T*>();
		Transform* transform = subclass.m_pClassReference->GetTransform();
		transform->SetPosition(position);
		transform->SetRotation(rotation);
		transform->SetScale(scale);
		AddEntity(subclass.m_pClassReference);
	}

	return subclass;
}

template<typename T>
T* World::GetSystem()
{
	for (ComponentSystem* pSystem : m_Systems)
	{
		if (T* pConvertedSystem = dynamic_cast<T*>(pSystem))
			return pConvertedSystem;
	}
	return nullptr;
}

template <typename T>
std::vector<Entity*> World::FindEntitiesWithComponent()
{
	static_assert(std::is_base_of<Component, T>::value, "Type must derive from Component");

	// result
	std::vector<Entity*> result = {};
 
	for (Entity* pEntity : m_Entities)
	{
		if (pEntity->GetComponent<T>())
			result.push_back(pEntity);
	}
	return result;
}

template <typename T>
std::vector<T*> World::FindEntitiesOfType()
{
	static_assert(std::is_base_of<Entity, T>::value, "Type must derive from Entity");
	
	std::vector<T*> entities;
	rttr::type entityType = rttr::type::get<T>();
	for (Entity* pEntity : m_Entities)
	{
		if (pEntity->get_type() == entityType)
			entities.push_back(static_cast<T*>(pEntity));
	}
	return entities;
}
